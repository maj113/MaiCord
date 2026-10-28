#include "maicord/platform/network.hpp"

#include "maicord/core/win32_kernel.hpp"

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
