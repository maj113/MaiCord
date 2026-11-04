// Name resolution for the Linux network layer: getaddrinfo(3) blocks, so lookups run on a small process-wide pool of
// threads and complete on the loop that asked, as the Windows asynchronous lookup does.
//
// A lookup lives in the table of jobs from network_resolver_start until both sides are done with it: the pool
// thread (it fills in the answers) and the consumer (it takes them when the completion is polled, or abandons the
// lookup by cancelling or by releasing its loop). The table, not the caller's NetworkResolver, holds the host name
// and the answers, so a cancelled resolver can be released while a lookup that glibc cannot interrupt still runs.
// Cancelling completes the resolver at once with WSA_E_CANCELLED; the late answer is dropped.
#include "internal.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/sys/linux/libc.hpp"

namespace maicord {
namespace {

using namespace net_detail;

constexpr u32 kJobCapacity = 32;
constexpr u32 kPoolThreads = 2;
constexpr u32 kHostCapacity = 256;
constexpr u32 kServiceCapacity = 32;
// getaddrinfo can load name service modules on the calling thread.
constexpr u32 kPoolStackBytes = 512 * 1024;

struct Job {
    bool used;
    bool worker_done;
    bool consumer_done;
    bool cancelled;
    // The pool queued the completion: it is too late to cancel.
    bool posted;
    u32 generation;
    NetworkLoop* loop;
    NetworkResolver* resolver;
    char host[kHostCapacity];
    char service[kServiceCapacity];
    NetworkAddress addresses[kNetworkResolverMaximumAddresses];
    u32 count;
};

struct Pool {
    SRWLOCK lock;
    Job jobs[kJobCapacity];
    u32 queue[kJobCapacity];
    u32 queue_head;
    u32 queue_count;
    u32 serial;
    bool started;
    Waitable wake;
};

Pool g_pool;

struct Locked {
    Locked() noexcept {
        AcquireSRWLockExclusive(&g_pool.lock);
    }
    ~Locked() noexcept {
        ReleaseSRWLockExclusive(&g_pool.lock);
    }
};

// glibc's EAI_* codes (negative) as the Winsock errors a lookup fails with.
i32 map_lookup_error(int status) noexcept {
    switch (status) {
    case -2: // EAI_NONAME
    case -5: // EAI_NODATA
        return kWsaHostNotFound;
    case -3: // EAI_AGAIN
    case -11: // EAI_SYSTEM
        return kWsaTryAgain;
    case -4: // EAI_FAIL
        return kWsaNoRecovery;
    case -10: // EAI_MEMORY
        return ERROR_NOT_ENOUGH_MEMORY;
    default:
        return WSAEINVAL;
    }
}

Job* job_of(const NetworkResolver* resolver) noexcept {
    if (resolver->job >= kJobCapacity) {
        return nullptr;
    }
    Job* job = &g_pool.jobs[resolver->job];
    return job->used && job->generation == resolver->job_generation ? job : nullptr;
}

// A job both sides are done with goes back to the table. The lock is held.
void release_if_finished(Job* job) noexcept {
    if (job->worker_done && job->consumer_done) {
        job->used = false;
    }
}

// Pops the next queued job, or null. The lock is held.
Job* dequeue() noexcept {
    if (g_pool.queue_count == 0) {
        return nullptr;
    }
    Job* job = &g_pool.jobs[g_pool.queue[g_pool.queue_head]];
    g_pool.queue_head = (g_pool.queue_head + 1) % kJobCapacity;
    --g_pool.queue_count;
    return job;
}

u32 pool_main(void*) noexcept {
    for (;;) {
        wait_one(g_pool.wake, 0xFFFFFFFFu);
        for (;;) {
            char host[kHostCapacity];
            char service[kServiceCapacity];
            u32 index = 0;
            {
                Locked locked;
                Job* job = dequeue();
                if (!job) {
                    break;
                }
                index = static_cast<u32>(job - g_pool.jobs);
                if (job->cancelled || !job->loop) {
                    // Abandoned before it started: nothing to look up.
                    job->worker_done = true;
                    release_if_finished(job);
                    continue;
                }
                copy_bytes(host, job->host, sizeof(host));
                copy_bytes(service, job->service, sizeof(service));
                // Another thread of the pool takes the next job while this one blocks in the resolver.
                if (g_pool.queue_count != 0) {
                    event_set(g_pool.wake);
                }
            }
            NetworkAddress addresses[kNetworkResolverMaximumAddresses];
            u32 count = 0;
            i32 error = 0;
            lookup(host, service, addresses, &count, &error);
            Locked locked;
            Job* job = &g_pool.jobs[index];
            job->worker_done = true;
            if (job->cancelled || !job->loop) {
                release_if_finished(job);
                continue;
            }
            copy_bytes(job->addresses, addresses, sizeof(addresses));
            job->count = count;
            job->posted = true;
            // The resolver is alive until this completion is polled; the lock keeps the loop alive meanwhile.
            post(job->loop, &job->resolver->operation, error == 0, 0, error);
        }
    }
}

// Starts the pool's threads on first use. The lock is held. False when they cannot be made.
bool start_pool() noexcept {
    if (g_pool.started) {
        return true;
    }
    g_pool.wake = event_create(false, false);
    if (!g_pool.wake) {
        return false;
    }
    for (u32 index = 0; index < kPoolThreads; ++index) {
        Thread* thread = thread_start(pool_main, nullptr, kPoolStackBytes, nullptr);
        if (!thread) {
            // A pool of the threads that did start still works.
            if (index == 0) {
                waitable_close(g_pool.wake);
                g_pool.wake = nullptr;
                return false;
            }
            break;
        }
        thread_close(thread);
    }
    g_pool.started = true;
    return true;
}

} // namespace

namespace net_detail {

bool narrow_text(const wchar_t* text, char* output, u32 capacity) noexcept {
    u32 length = 0;
    if (text) {
        for (; text[length]; ++length) {
            if (length + 1 >= capacity || static_cast<u16>(text[length]) >= 0x80) {
                return false;
            }
            output[length] = static_cast<char>(text[length]);
        }
    }
    output[length] = 0;
    return true;
}

void lookup(const char* host, const char* service, NetworkAddress* addresses, u32* count, i32* error) noexcept {
    AddrInfo hints{};
    hints.family = kAfUnspec;
    hints.type = kSockStream;
    hints.protocol = kProtocolTcp;
    AddrInfo* list = nullptr;
    const int status = getaddrinfo(host, service && service[0] ? service : nullptr, &hints, &list);
    *count = 0;
    *error = 0;
    if (status != 0) {
        *error = map_lookup_error(status);
        return;
    }
    for (const AddrInfo* entry = list; entry && *count < kNetworkResolverMaximumAddresses; entry = entry->next) {
        if (from_socket_address(entry->address, entry->address_length, &addresses[*count])) {
            ++*count;
        }
    }
    freeaddrinfo(list);
    if (*count == 0) {
        *error = kWsaHostNotFound;
    }
}

void translate_resolver(NetworkOperation* operation, NetworkPollResult* result) noexcept {
    auto* resolver = static_cast<NetworkResolver*>(operation->owner);
    NetworkCompletion& completion = result->completion;
    completion.resolver = resolver;
    completion.kind = NetworkOperationKind::resolve;
    completion.stale = !resolver || operation->generation != resolver->generation;
    if (completion.stale) {
        completion.native_error = resolver ? operation->native_error : WSAEINVAL;
        return;
    }
    resolver->native_error = operation->native_error;
    {
        Locked locked;
        if (Job* job = job_of(resolver)) {
            if (operation->succeeded && !job->cancelled) {
                copy_bytes(resolver->addresses, job->addresses, sizeof(resolver->addresses));
                resolver->address_count = job->count;
            }
            job->consumer_done = true;
            release_if_finished(job);
        }
    }
    resolver->pending = false;
    if (resolver->native_error == 0) {
        resolver->state = NetworkResolverState::resolved;
    } else if (resolver->native_error == WSA_E_CANCELLED) {
        resolver->state = NetworkResolverState::cancelled;
    } else {
        resolver->state = NetworkResolverState::failed;
    }
    completion.native_error = resolver->native_error;
    completion.succeeded = resolver->native_error == 0;
}

void resolver_forget_loop(NetworkLoop* loop) noexcept {
    Locked locked;
    for (Job& job : g_pool.jobs) {
        if (job.used && job.loop == loop) {
            job.loop = nullptr;
            job.consumer_done = true;
            release_if_finished(&job);
        }
    }
}

} // namespace net_detail

NetworkResult network_resolver_start(NetworkLoop* loop, NetworkResolver* resolver, const wchar_t* hostname,
                                     const wchar_t* service) noexcept {
    if (!loop || !loop->ready || !resolver || resolver->pending || resolver->address_count || !hostname ||
        hostname[0] == 0) {
        return failure(NetworkError::invalid_argument, WSAEINVAL);
    }
    char host[kHostCapacity];
    char service_name[kServiceCapacity];
    if (!narrow_text(hostname, host, kHostCapacity) || !narrow_text(service, service_name, kServiceCapacity)) {
        return failure(NetworkError::invalid_argument, WSAEINVAL);
    }

    reset_value<NetworkResolver>(resolver);
    resolver->generation = 1;
    resolver->operation.owner = resolver;
    resolver->operation.generation = resolver->generation;
    resolver->operation.kind = NetworkOperationKind::resolve;
    resolver->loop = loop;
    resolver->state = NetworkResolverState::resolving;
    resolver->pending = true;

    bool queued = false;
    {
        Locked locked;
        if (start_pool()) {
            for (u32 index = 0; index < kJobCapacity && !queued; ++index) {
                Job& job = g_pool.jobs[index];
                if (job.used) {
                    continue;
                }
                const u32 generation = ++g_pool.serial;
                reset_value<Job>(&job);
                job.used = true;
                job.generation = generation;
                job.loop = loop;
                job.resolver = resolver;
                copy_bytes(job.host, host, sizeof(host));
                copy_bytes(job.service, service_name, sizeof(service_name));
                resolver->job = index;
                resolver->job_generation = generation;
                g_pool.queue[(g_pool.queue_head + g_pool.queue_count) % kJobCapacity] = index;
                ++g_pool.queue_count;
                queued = true;
            }
        }
    }
    if (!queued) {
        resolver->pending = false;
        resolver->state = NetworkResolverState::failed;
        return failure(NetworkError::resolve_start, ERROR_NOT_ENOUGH_MEMORY);
    }
    event_set(g_pool.wake);
    return success();
}

NetworkResult network_resolver_cancel(NetworkResolver* resolver) noexcept {
    if (!resolver || !resolver->pending || resolver->state != NetworkResolverState::resolving) {
        return failure(NetworkError::invalid_argument, WSAEINVAL);
    }
    {
        Locked locked;
        Job* job = job_of(resolver);
        if (!job || job->posted || job->cancelled) {
            // The answer is already on its way to the loop: there is nothing left to cancel.
            return failure(NetworkError::resolve_cancel, ERROR_INVALID_HANDLE);
        }
        job->cancelled = true;
    }
    complete(resolver->loop, &resolver->operation, false, 0, WSA_E_CANCELLED);
    return success();
}

bool network_resolver_release(NetworkResolver* resolver) noexcept {
    if (!resolver || resolver->pending) {
        return false;
    }
    reset_value<NetworkResolver>(resolver);
    return true;
}

} // namespace maicord
