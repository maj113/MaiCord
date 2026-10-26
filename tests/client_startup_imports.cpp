// MaiCord.exe's import tables decide which DLLs the loader maps before the
// entry point, ahead of the GPU device prestart. Libraries the first frame
// never needs (MAICORD_CLIENT_DELAY_LOADS in cmake/client.cmake) are
// delay-loaded: present in the delay-load directory and absent from the static
// imports, while the device path (kernel32, d3d11) stays static.
#include "test_output.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_image.hpp"

namespace maicord::client_startup_imports_test {
namespace {
u32 failures{};
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}

char lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
}
// b names a DLL with its ".dll" extension. The loader appends ".dll" to a
// module name without an extension, so a may omit it.
bool same_name(const char* a, const char* b, u32 b_length) noexcept {
    u32 i = 0;
    for (; i < b_length && a[i]; ++i)
        if (lower(a[i]) != lower(b[i]))
            return false;
    if (a[i])
        return false;
    if (i == b_length)
        return true;
    return b_length - i == 4 && b[i] == '.' && lower(b[i + 1]) == 'd' && lower(b[i + 2]) == 'l' &&
           lower(b[i + 3]) == 'l';
}

struct Image {
    const u8* base;
    const IMAGE_NT_HEADERS64* nt;
    const IMAGE_DATA_DIRECTORY& directory(u32 index) const noexcept {
        return nt->OptionalHeader.DataDirectory[index];
    }
};

// Import descriptors (static, then delay-load) whose DLL name equals name.
u32 count_static(const Image& image, const char* name, u32 length) noexcept {
    const auto& directory = image.directory(IMAGE_DIRECTORY_ENTRY_IMPORT);
    u32 found = 0;
    if (directory.VirtualAddress)
        for (auto* entry = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image.base + directory.VirtualAddress);
             entry->Name; ++entry)
            found += same_name(reinterpret_cast<const char*>(image.base + entry->Name), name, length);
    return found;
}
u32 count_delayed(const Image& image, const char* name, u32 length) noexcept {
    const auto& directory = image.directory(IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
    u32 found = 0;
    if (directory.VirtualAddress)
        for (auto* entry = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(image.base + directory.VirtualAddress);
             entry->DllNameRVA; ++entry)
            found += entry->Attributes.RvaBased &&
                     same_name(reinterpret_cast<const char*>(image.base + entry->DllNameRVA), name, length);
    return found;
}
u32 static_descriptors(const Image& image) noexcept {
    const auto& directory = image.directory(IMAGE_DIRECTORY_ENTRY_IMPORT);
    u32 count = 0;
    if (directory.VirtualAddress)
        for (auto* entry = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image.base + directory.VirtualAddress);
             entry->Name; ++entry)
            ++count;
    return count;
}
u32 delayed_descriptors(const Image& image) noexcept {
    const auto& directory = image.directory(IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
    u32 count = 0;
    if (directory.VirtualAddress)
        for (auto* entry = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(image.base + directory.VirtualAddress);
             entry->DllNameRVA; ++entry)
            ++count;
    return count;
}

void test_imports() noexcept {
    const HANDLE file = CreateFileW(MAICORD_CLIENT_IMPORTS_EXECUTABLE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(file != INVALID_HANDLE_VALUE, "MaiCord.exe opens");
    if (file == INVALID_HANDLE_VALUE)
        return;
    // Mapped as an image: every RVA is an offset from the view.
    const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr);
    const auto* base = mapping ? static_cast<const u8*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)) : nullptr;
    check(base != nullptr, "MaiCord.exe maps as an image");
    if (base) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const Image image{base, reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew)};
        check(image.nt->Signature == IMAGE_NT_SIGNATURE &&
                  image.nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC,
              "MaiCord.exe is a PE32+ image");
        constexpr char list[] = MAICORD_CLIENT_DELAY_LOADS;
        u32 expected = 0;
        for (u32 begin = 0, end = 0; list[begin]; begin = list[end] ? end + 1 : end) {
            end = begin;
            while (list[end] && list[end] != ',')
                ++end;
            const char* name = list + begin;
            const u32 length = end - begin;
            ++expected;
            check(count_delayed(image, name, length) == 1, "each listed DLL has one delay-load descriptor");
            check(count_static(image, name, length) == 0, "a delay-loaded DLL is not also a static import");
        }
        check(expected >= 1 && delayed_descriptors(image) == expected,
              "the delay-load directory holds exactly the listed DLLs");
        // The exact set the loader maps before the entry point; a new static import must be added here on purpose.
        check(count_static(image, "kernel32.dll", 12) == 1 && count_static(image, "user32.dll", 10) == 1 &&
                  count_static(image, "d3d11.dll", 9) == 1 && count_static(image, "gdi32.dll", 9) == 1 &&
                  count_static(image, "ntdll.dll", 9) == 1 && static_descriptors(image) == 5,
              "the static imports are exactly kernel32, user32, d3d11, gdi32 and ntdll");
        check(count_delayed(image, "d3d11.dll", 9) == 0 && count_delayed(image, "user32.dll", 10) == 0,
              "the device and window paths are not delay-loaded");
        UnmapViewOfFile(base);
    }
    if (mapping)
        CloseHandle(mapping);
    CloseHandle(file);
}
} // namespace
} // namespace maicord::client_startup_imports_test

extern "C" __declspec(noreturn) void WINAPI maicord_client_startup_imports_entry() noexcept {
    using namespace maicord::client_startup_imports_test;
    test_imports();
    const bool failed = failures != 0;
    constexpr char pass[] = "PASS client startup imports\n", fail[] = "FAIL client startup imports\n";
    DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}
