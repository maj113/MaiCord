// The delay-load helper (src/core/delay_load.cpp) on this image's own delay
// imports, as the linker's thunks call it: ws2_32 imports by ordinal and by
// name, ole32 exports that forward through an API set to combase, and first
// calls racing on one import from several threads.
#include "test_output.hpp"

#include "maicord/core/module.hpp"
#include "maicord/core/win32_image.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/com.hpp"
#include "maicord/sys/winsock.hpp"

namespace maicord::delay_load_helper_test {
namespace {
u32 failures{};
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}

const u8* image() noexcept {
    return reinterpret_cast<const u8*>(&__ImageBase);
}
const IMAGE_NT_HEADERS64* headers() noexcept {
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(image() + __ImageBase.e_lfanew);
}
bool in_image(u64 address) noexcept {
    return address - reinterpret_cast<u64>(image()) < headers()->OptionalHeader.SizeOfImage;
}

char lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
}
bool same_name(const char* a, const char* b) noexcept {
    for (; *a && lower(*a) == lower(*b); ++a, ++b) {
    }
    return lower(*a) == lower(*b);
}

const IMAGE_DELAYLOAD_DESCRIPTOR* descriptor(const char* dll) noexcept {
    const auto& directory = headers()->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!directory.VirtualAddress)
        return nullptr;
    for (auto* entry = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(image() + directory.VirtualAddress);
         entry->DllNameRVA; ++entry)
        if (same_name(reinterpret_cast<const char*>(image() + entry->DllNameRVA), dll))
            return entry;
    return nullptr;
}
HMODULE recorded_module(const IMAGE_DELAYLOAD_DESCRIPTOR* entry) noexcept {
    return *reinterpret_cast<const HMODULE*>(image() + entry->ModuleHandleRVA);
}
// The import address slot of procedure: a name, or an ordinal below 0x10000.
const volatile u64* slot(const IMAGE_DELAYLOAD_DESCRIPTOR* entry, const char* procedure) noexcept {
    const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(image() + entry->ImportNameTableRVA);
    const auto* addresses = reinterpret_cast<const volatile u64*>(image() + entry->ImportAddressTableRVA);
    const u64 ordinal = reinterpret_cast<u64>(procedure);
    for (u32 i = 0; names[i].u1.AddressOfData; ++i) {
        const u64 name = names[i].u1.AddressOfData;
        if (IMAGE_SNAP_BY_ORDINAL64(name) ? ordinal == IMAGE_ORDINAL64(name)
                                          : ordinal >= 0x10000 &&
                                                same_name(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(image() + name)->Name,
                                                          procedure))
            return addresses + i;
    }
    return nullptr;
}
const char* ordinal(u16 value) noexcept {
    return reinterpret_cast<const char*>(static_cast<u64>(value));
}
u64 export_address(const wchar_t* dll, const char* procedure) noexcept {
    const HMODULE module = GetModuleHandleW(dll);
    return module ? reinterpret_cast<u64>(GetProcAddress(module, procedure)) : 0;
}

constexpr u32 kRacers = 8;
HANDLE g_start{};
u16 g_swapped[kRacers]{};
DWORD WINAPI race(LPVOID parameter) {
    const u32 index = static_cast<u32>(reinterpret_cast<u64>(parameter));
    WaitForSingleObject(g_start, INFINITE);
    g_swapped[index] = ntohs(0x3412);
    return 0;
}

void test_helper() noexcept {
    const IMAGE_DELAYLOAD_DESCRIPTOR* ws2 = descriptor("ws2_32.dll");
    const IMAGE_DELAYLOAD_DESCRIPTOR* ole = descriptor("ole32.dll");
    check(ws2 && ole, "ws2_32 and ole32 are delay-loaded");
    if (!ws2 || !ole)
        return;
    const volatile u64* by_ordinal = slot(ws2, ordinal(9));      // htons
    const volatile u64* by_name = slot(ws2, "InetPtonW");
    const volatile u64* raced = slot(ws2, ordinal(15));          // ntohs
    const volatile u64* forwarded = slot(ole, "CoTaskMemAlloc"); // api-ms-win-core-com-l1-1-0
    check(by_ordinal && by_name && raced && forwarded, "the imports have delay-load slots");
    if (!by_ordinal || !by_name || !raced || !forwarded)
        return;
    check(!recorded_module(ws2) && !recorded_module(ole), "no delay-loaded DLL is recorded before its first call");
    check(in_image(*by_ordinal) && in_image(*by_name) && in_image(*raced) && in_image(*forwarded),
          "unresolved slots point at the image's thunks");

    check(htons(0x1234) == 0x3412, "an import by ordinal returns its result");
    const HMODULE ws2_module = GetModuleHandleW(L"ws2_32.dll");
    check(ws2_module && recorded_module(ws2) == ws2_module, "the first call records the loaded DLL");
    check(*by_ordinal == export_address(L"ws2_32.dll", ordinal(9)), "the ordinal slot holds the export");
    check(in_image(*by_name) && in_image(*raced), "imports not yet called stay unresolved");

    u32 address = 0;
    check(InetPtonW(AF_INET, L"1.2.3.4", &address) == 1 && address == 0x04030201,
          "an import by name returns its result");
    check(*by_name == export_address(L"ws2_32.dll", "InetPtonW"), "the name slot holds the export");

    g_start = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE threads[kRacers]{};
    u32 started = 0;
    for (u32 i = 0; i < kRacers; ++i) {
        threads[i] = CreateThread(nullptr, 64 * 1024, race, reinterpret_cast<LPVOID>(static_cast<u64>(i)), 0, nullptr);
        started += threads[i] != nullptr;
    }
    check(g_start && started == kRacers, "the racing threads start");
    if (g_start)
        SetEvent(g_start);
    if (started)
        WaitForMultipleObjects(started, threads, TRUE, INFINITE);
    u32 correct = 0;
    for (u32 i = 0; i < kRacers; ++i) {
        correct += g_swapped[i] == 0x1234;
        if (threads[i])
            CloseHandle(threads[i]);
    }
    if (g_start)
        CloseHandle(g_start);
    check(correct == kRacers, "every racing first call returns its result");
    check(*raced == export_address(L"ws2_32.dll", ordinal(15)), "the raced slot holds the export");

    void* block = CoTaskMemAlloc(16);
    check(block != nullptr, "a forwarded import returns its result");
    CoTaskMemFree(block);
    check(recorded_module(ole) == GetModuleHandleW(L"ole32.dll"), "the forwarding DLL is recorded");
    check(*forwarded && *forwarded == export_address(L"combase.dll", "CoTaskMemAlloc"),
          "the forwarded slot holds the API set target's export");
}
} // namespace
} // namespace maicord::delay_load_helper_test

extern "C" __declspec(noreturn) void WINAPI maicord_delay_load_helper_entry() noexcept {
    using namespace maicord::delay_load_helper_test;
    test_helper();
    maicord::test_exit(failures, "delay load helper");
}
