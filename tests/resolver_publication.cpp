// Also the manual MaiCordResolverPublicationProbe benchmark (cmake/benchmarks.cmake,
// MAICORD_RESOLVER_PUBLICATION_PROBE=1 and MAICORD_RESOLVER_PUBLICATION_BENCHMARK=1).
#include "maicord/platform/network.hpp"

#include <Windows.h>

namespace {
maicord::NetworkResolver* released_resolver;
bool release_succeeded;
BOOL WINAPI resolver_test_post(HANDLE port, DWORD bytes, ULONG_PTR key, OVERLAPPED* overlapped) noexcept {
    if (released_resolver && overlapped == reinterpret_cast<OVERLAPPED*>(&released_resolver->operation.overlapped)) {
        // Model a consumer releasing the resolver immediately after dequeue,
        // before PostQueuedCompletionStatus has returned on the publisher.
        DWORD previous = 0;
        release_succeeded = VirtualProtect(released_resolver, 4096, PAGE_NOACCESS, &previous) != FALSE;
        return TRUE;
    }
    return PostQueuedCompletionStatus(port, bytes, key, overlapped);
}
}

#define PostQueuedCompletionStatus resolver_test_post
#include "../src/platform/network.cpp"
#undef PostQueuedCompletionStatus

namespace maicord {
void test_resolver_publication(void (*check)(bool, const char*) noexcept) noexcept {
    auto* resolver = static_cast<NetworkResolver*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(resolver != nullptr, "resolver publication guard allocation");
    if (!resolver) return;
    resolver->operation.owner = resolver;
    resolver->operation.kind = NetworkOperationKind::resolve;
    released_resolver = resolver;
    release_succeeded = false;
    resolve_complete(0, 0, native(&resolver->operation.overlapped));
    released_resolver = nullptr;
    check(release_succeeded, "resolver callback does not touch storage after publishing completion");
    VirtualFree(resolver, 0, MEM_RELEASE);
}
}

#if defined(MAICORD_RESOLVER_PUBLICATION_PROBE)
#include "maicord/core/time.hpp"
#include "maicord/core/trace_record.hpp"
extern "C" void maicord_resolver_publication_entry() noexcept {
    wchar_t benchmark[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_RESOLVER_PUBLICATION_BENCHMARK", benchmark, 2) == 1 && benchmark[0] == L'1') {
        using namespace maicord;
        NetworkLoop loop{};
        if (!network_loop_initialize(&loop).ok()) ExitProcess(1);
        NetworkResolver resolver{};
        resolver.operation.owner = &resolver;
        resolver.operation.kind = NetworkOperationKind::resolve;
        resolver.operation.generation = resolver.generation = 1;
        resolver.completion_port = loop.completion_port;
        using Complete = void (CALLBACK *)(DWORD, DWORD, OVERLAPPED*) noexcept;
        Complete volatile complete = resolve_complete;
        const Clock clock = clock_initialize();
        constexpr u32 calls = 200000;
        for (u32 round = 0; round < 5; ++round) {
            const i64 start = clock_now();
            for (u32 i = 0; i < calls; ++i) {
                complete(i & 1 ? WSAHOST_NOT_FOUND : 0, 0, &resolver.operation.overlapped);
                const auto polled = network_loop_poll(&loop, INFINITE);
                if (polled.state != NetworkPollState::completion || polled.completion.resolver != &resolver ||
                    polled.completion.stale || polled.completion.native_error != (i & 1 ? WSAHOST_NOT_FOUND : 0)) ExitProcess(1);
            }
            TraceRecord line("resolver_publication");
            line.field("round", round); line.field("calls", calls);
            line.field("us", clock_microseconds(clock, start, clock_now()));
            line.field("resolver_bytes", sizeof(NetworkResolver));
            DWORD written;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.text, line.used, &written, nullptr);
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\r\n", 2, &written, nullptr);
        }
        network_loop_release(&loop);
        ExitProcess(0);
    }
    maicord::test_resolver_publication(+[](bool ok, const char*) noexcept { if (!ok) ExitProcess(1); });
    ExitProcess(0);
}
#endif
