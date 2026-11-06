#include "support/application_downloads_inactive.hpp"
#include "support/test_main_shared.hpp"

#include "support/test_output.hpp"
#include "maicord/core/win32_exception.hpp"

namespace maicord {
namespace {

u32 g_failures = 0;

const char* volatile g_last_check = "none";

} // namespace

void expect(bool condition, const char* name) noexcept {
    g_last_check = name;
    if (!condition) {
        ++g_failures;
        test_write_text("FAIL ");
        test_write_text(name);
        test_write_text("\r\n");
    }
}

namespace {

void write_hex(u64 value) noexcept {
    char digits[19] = {'0', 'x'};
    u32 count = 2;
    for (u32 shift = 64; shift;) {
        shift -= 4;
        const u32 nibble = static_cast<u32>(value >> shift) & 15u;
        if (nibble || count > 2 || !shift)
            digits[count++] = static_cast<char>(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
    }
    digits[count] = 0;
    test_write_text(digits);
}

// Names the crashing module offset and the last check reached; CTest shows it.
// A vectored handler: MaiCord code has no unwind tables, so frame-based
// dispatch never reaches an unhandled-exception filter. It also sees exceptions
// that system DLLs raise and handle themselves, so it names only error-status
// codes (access violation, stack overflow, illegal instruction and the like).
LONG WINAPI report_crash(EXCEPTION_POINTERS* info) noexcept {
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    if ((record.ExceptionCode & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    const auto address = reinterpret_cast<u64>(record.ExceptionAddress);
    HMODULE module = nullptr;
    wchar_t path[260]{};
    char name[64]{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(record.ExceptionAddress), &module)) {
        const DWORD length = GetModuleFileNameW(module, path, 260);
        u32 start = length < 260 ? length : 0;
        while (start && path[start - 1] != L'\\')
            --start;
        for (u32 i = 0; i < 63 && start + i < length; ++i)
            name[i] = path[start + i] < 128 ? static_cast<char>(path[start + i]) : '?';
    }
    test_write_text("CRASH ");
    write_hex(record.ExceptionCode);
    test_write_text(" at ");
    test_write_text(name[0] ? name : "?");
    test_write_text("+");
    write_hex(address - reinterpret_cast<u64>(module));
    if (record.ExceptionCode == 0xC0000005u && record.NumberParameters >= 2) {
        test_write_text(record.ExceptionInformation[0] ? " writing " : " reading ");
        write_hex(record.ExceptionInformation[1]);
    }
    test_write_text(" after check ");
    test_write_text(g_last_check);
    test_write_text("\r\n");
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_tests_entry() noexcept {
    AddVectoredExceptionHandler(1, maicord::report_crash);
#define CORE_LOCAL_TEST(name) maicord::name();
#define CORE_EXTERNAL_TEST(name) maicord::name(maicord::expect);
#include "core_test_cases.inc"
#undef CORE_EXTERNAL_TEST
#undef CORE_LOCAL_TEST

    if (maicord::g_failures == 0) {
        maicord::test_write_text("PASS maicord_core\r\n");
    }
    ExitProcess(maicord::g_failures);
}
