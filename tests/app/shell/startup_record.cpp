// The startup record (src/app/shell/startup_record.cpp) written to a fresh log: its lines start the file, each names
// its fields, and nothing in it is a user name, a profile path or a token. The updater's build inputs are stand-ins
// here (the real ones are compiled into src/app/update/entry.cpp from CMake).
#include "../../support/test_output.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/log.hpp"
#include "shell/startup_record.hpp"
#include "update/updates.hpp"
#if defined(_WIN32)
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_process.hpp"
#else
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/path.hpp"
#include "maicord/sys/linux/process.hpp"
#endif

namespace maicord::app_detail {
u32 application_update_build_version_text(Slice<char> output) noexcept {
    constexpr char version[] = "0.7.0-dev.3+abc1234";
    if (output.count < sizeof(version) - 1)
        return 0;
    copy_bytes(output.data, version, sizeof(version) - 1);
    return sizeof(version) - 1;
}
Utf8View application_update_build_commit() noexcept {
    return utf8_literal("abc1234");
}
const char* application_update_install_kind() noexcept {
    return "portable";
}
ApplicationUpdatesView application_updates_view() noexcept {
    ApplicationUpdatesView view{};
    view.state = ApplicationUpdateState::idle;
    view.channel = UpdateChannel::preview;
    return view;
}
} // namespace maicord::app_detail

namespace maicord {
namespace {
u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

// Where `needle` starts in `text`, or ~0u.
u32 find(const char* text, const char* needle) noexcept {
    const u32 length = c_string_length(needle);
    for (u32 start = 0; length && text[start]; ++start)
        if (bytes_equal(text + start, needle, length))
            return start;
    return ~0u;
}

// The text of the line `index` (0-based) of `text`, without its prefix up to "] " of the component.
bool line_has(const char* text, u32 index, const char* needle) noexcept {
    u32 start = 0;
    for (; index && text[start]; ++start)
        if (text[start] == '\n')
            --index;
    u32 end = start;
    while (text[end] && text[end] != '\n')
        ++end;
    char line[1024];
    const u32 length = minimum(end - start, static_cast<u32>(sizeof(line) - 1));
    copy_bytes(line, text + start, length);
    line[length] = 0;
    return find(line, needle) != ~0u;
}
} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_startup_record_entry) {
    using namespace maicord;
    char user[256]{};
#if defined(_WIN32)
    // A private data directory, as the core log test makes one.
    wchar_t root[512]{};
    DWORD length = GetTempPathW(400, root);
    const wchar_t suffix[] = L"MaiCord-start-test";
    bool ok = length != 0 && length < 400;
    for (u32 index = 0; ok && suffix[index]; ++index)
        root[length++] = suffix[index];
    ok = ok && (CreateDirectoryW(root, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) &&
         SetEnvironmentVariableW(L"LOCALAPPDATA", root);
    wchar_t wide_user[128]{};
    const DWORD user_length = GetEnvironmentVariableW(L"USERNAME", wide_user, 128);
    for (u32 index = 0; index < user_length && index < 127; ++index)
        user[index] = wide_user[index] < 0x80 ? static_cast<char>(wide_user[index]) : '?';
#else
    // The harness's private XDG_STATE_HOME holds the log.
    bool ok = true;
    const char* user_name = environment_get("USER");
    for (u32 index = 0; user_name && user_name[index] && index < 255; ++index)
        user[index] = user_name[index];
#endif
    wchar_t file[768]{};
    ok = ok && state_path(file, 768, L"logs/maicord.log", true).ok();
#if defined(_WIN32)
    DeleteFileW(file);
#else
    char path[2304]{};
    ok = ok && path_to_utf8(file, path, sizeof(path)) != 0;
    sys_unlink(path);
#endif
    ok = ok && log_open();
    app_detail::application_log_start_record();
    app_detail::application_log_start_display(Window{});
    app_detail::application_log_start_install();
    log_close();

    char text[8192]{};
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD read = 0;
    if (handle != INVALID_HANDLE_VALUE) {
        ReadFile(handle, text, sizeof(text) - 1, &read, nullptr);
        CloseHandle(handle);
    }
    DeleteFileW(file);
    char root_buffer[512]{};
    for (u32 index = 0; root[index] && index < 511; ++index)
        root_buffer[index] = root[index] < 0x80 ? static_cast<char>(root[index]) : '?';
    const char* root_text = root_buffer;
#else
    const long descriptor = sys_open(path, kOpenReadOnly);
    long read = 0;
    if (!syscall_failed(descriptor)) {
        read = sys_read(static_cast<int>(descriptor), text, sizeof(text) - 1);
        sys_close(static_cast<int>(descriptor));
    }
    sys_unlink(path);
    const char* root_text = environment_get("XDG_STATE_HOME");
#endif
    ok = ok && read > 0;
    char pid[16]{};
    const char pid_key[] = "pid=";
    copy_bytes(pid, pid_key, 4);
    {
        u32 value = process_id(), digits = 1;
        for (u32 rest = value; rest >= 10; rest /= 10)
            ++digits;
        for (u32 index = digits; index; --index, value /= 10)
            pid[3 + index] = static_cast<char>('0' + value % 10);
    }

    check(ok, "startup_record_is_written");
    check(line_has(text, 0, "[I] [start] ==== MaiCord 0.7.0-dev.3+abc1234 start ====") && text[4] == '-',
          "startup_record_starts_the_session_with_its_banner");
    check(line_has(text, 1, "[start] build version=0.7.0-dev.3+abc1234 commit=abc1234 flavour=development"),
          "startup_record_names_the_build");
#if defined(_WIN32)
    check(line_has(text, 2, "[start] system os=Windows version=10.0."), "startup_record_names_the_windows_build");
#else
    check(line_has(text, 2, "[start] system os=Linux kernel=") && line_has(text, 2, " distro=\"") &&
              line_has(text, 2, " libc=") && line_has(text, 2, " runtime="),
          "startup_record_names_the_kernel_distribution_and_libc");
#endif
    check(line_has(text, 3, "[start] machine cpu=\"") && line_has(text, 3, " cores=") &&
              line_has(text, 3, " ram_mib=") && line_has(text, 3, " locale=") && line_has(text, 3, pid) &&
              !line_has(text, 3, "cores=0 ") && !line_has(text, 3, "ram_mib=0 "),
          "startup_record_names_the_machine");
    check(line_has(text, 5, "[start] install kind=portable channel=preview updates=on"),
          "startup_record_names_the_install");
    const bool user_absent = c_string_length(user) < 3 || find(text, user) == ~0u;
    const bool path_absent = !root_text || !root_text[0] || find(text, root_text) == ~0u;
    check(user_absent && path_absent && find(text, "token") == ~0u && find(text, "Token") == ~0u,
          "startup_record_holds_no_user_name_path_or_token");
#if defined(_WIN32)
    // The log's directories, innermost first: <root>\MaiCord\logs, <root>\MaiCord, then the root.
    for (u32 level = 0; level < 2; ++level) {
        u32 end = 0;
        while (file[end])
            ++end;
        while (end && file[end - 1] != L'\\')
            --end;
        file[end ? end - 1 : 0] = 0;
        RemoveDirectoryW(file);
    }
    RemoveDirectoryW(root);
#endif
    test_exit(g_failures, "startup_record");
}
