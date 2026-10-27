// The delay-load helper (__delayLoadHelper2) of images linked with /DELAYLOAD
// (MAICORD_CLIENT_DELAY_LOADS in cmake/client.cmake). The linker's per-DLL
// thunk calls it on the first call through an import slot and jumps to the
// address it returns.
//
// ntdll resolves the slot as it binds static imports: it loads the DLL through
// this image's dependency path (KnownDLLs, API sets), looks the import up by
// name or ordinal following forwarders, stores the address in the slot and
// records the module in the descriptor. Threads racing on one slot resolve the
// same address without a lock of ours. A system DLL or export missing on a
// supported Windows ends the process here: kernel32's default failure hook
// would instead hand back kernelbase's error-returning stubs or raise an
// exception, neither of which MaiCord's callers expect.
#include "maicord/core/module.hpp"
#include "maicord/core/win32_image.hpp"
#include "maicord/core/win32_kernel.hpp"

// Exported by kernel32 (ntdll's LdrResolveDelayLoadedAPI); no SDK header declares it.
extern "C" __declspec(dllimport) PVOID WINAPI ResolveDelayLoadedAPI(PVOID ParentModuleBase,
    const IMAGE_DELAYLOAD_DESCRIPTOR* DelayloadDescriptor, PVOID FailureDllHook,
    PVOID(WINAPI* FailureSystemHook)(LPCSTR DllName, LPCSTR ProcName), IMAGE_THUNK_DATA64* ThunkAddress,
    ULONG Flags);

namespace {
PVOID WINAPI delay_load_failed(LPCSTR, LPCSTR) noexcept {
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}
} // namespace

extern "C" FARPROC WINAPI __delayLoadHelper2(const IMAGE_DELAYLOAD_DESCRIPTOR* descriptor, FARPROC* slot) noexcept {
    return reinterpret_cast<FARPROC>(ResolveDelayLoadedAPI(&__ImageBase, descriptor, nullptr, delay_load_failed,
                                                           reinterpret_cast<IMAGE_THUNK_DATA64*>(slot), 0));
}
