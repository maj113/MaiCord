#include "../src/app/rest_client.cpp"

namespace maicord {
namespace {
void emit(const char* phase, u32 capacity, bool shared, u32 iterations, u64 us, u64 committed) noexcept {
    TraceRecord line("rest_worker_storage");
    line.field("phase", phase); line.field("capacity", capacity); line.field("shared", shared ? 1u : 0u);
    line.field("iterations", iterations); line.field("us", us); line.field("committed", committed);
    DWORD written; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.text, line.used, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\r\n", 2, &written, nullptr);
}
bool run() noexcept {
    const Clock clock = clock_initialize();
    const u32 capacities[]{4096u, 262144u, 524288u};
    const bool sharing[]{false, true};
    for (u32 capacity : capacities) for (bool shared : sharing) {
        RestSharedState state{};
        rest_rate_scheduler_initialize(&state.scheduler, {state.buckets, 32}, {state.bindings, 64});
        constexpr u32 iterations = 2000;
        u64 committed = 0;
        const i64 begin = clock_now();
        for (u32 n = 0; n < iterations; ++n) {
            RestClient client{};
            const u32 bytes = allocation_bytes(capacity, false, true, shared);
            auto* runtime = static_cast<RestClientRuntime*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!runtime) return false;
            client.runtime = runtime; runtime->owner = &client;
            runtime->allocation_bytes = bytes; runtime->response_capacity = capacity;
            runtime->authorization = RestAuthorization::normal_user;
            assign_storage(runtime, shared ? &state : nullptr);
            if (shared) client.shared = &state;
            if (n == 0) {
                MEMORY_BASIC_INFORMATION info{};
                if (!VirtualQuery(runtime, &info, sizeof(info)) || info.State != MEM_COMMIT) return false;
                committed = info.RegionSize;
            }
            const RestRouteKey route{1, 0, RestMethod::get, RestMajorParameter::none};
            auto acquired = rest_rate_acquire(rate_scheduler(runtime), route, n + 1);
            if (!acquired.acquired_request()) return false;
            rest_rate_cancel(rate_scheduler(runtime), &acquired.lease);
            destroy_client_storage(&client, nullptr);
        }
        emit("allocate_initialize_release", capacity, shared, iterations,
             clock_microseconds(clock, begin, clock_now()), committed);
        const i64 start = clock_now();
        for (u32 n = 0; n < 64; ++n) {
            RestClient client{};
            if (!rest_client_start_public(&client, capacity, shared ? &state : nullptr).ok()) return false;
            if (!rest_client_stop(&client, 3000).ok()) return false;
        }
        emit("worker_start_stop", capacity, shared, 64, clock_microseconds(clock, start, clock_now()), 0);
    }
    return true;
}
} // namespace
} // namespace maicord
extern "C" void maicord_rest_worker_storage_entry() noexcept { ExitProcess(maicord::run() ? 0 : 1); }
