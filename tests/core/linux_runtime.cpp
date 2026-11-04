// The Linux runtime: process entry state, the pinned libc imports (dlopen, pthread_create, clock_gettime), the
// shared-library search and the XDG data path. argv[1] selects the case; cmake/linux_test_registry.cmake runs each
// with the environment it needs.
#include "../support/test_output.hpp"
#include "../support/test_thread.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/shared_library.hpp"
#include "maicord/core/intrinsics.hpp"
#include "maicord/core/time.hpp"
#include "maicord/sys/linux/libc.hpp"
#include "maicord/sys/linux/path.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord {
namespace {

u32 g_failures = 0;

void expect(bool condition, const char* name) noexcept {
    if (!condition) {
        ++g_failures;
        test_write_text("FAIL ");
        test_write_line(name);
    }
}

bool text_equal(const char* left, const char* right) noexcept {
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

u32 thread_body(void* argument) {
    *static_cast<volatile u32*>(argument) = 42;
    return 7;
}

void test_startup() noexcept {
    const ProcessStartup& startup = process_startup();
    expect(startup.argc >= 1 && startup.argv && startup.argv[0] && !startup.argv[startup.argc],
           "startup_arguments");
    expect(startup.environment != nullptr, "startup_environment");
    expect(startup.page_size == 4096 && auxiliary_value(kAuxPageSize) == startup.page_size, "startup_page_size");
    expect(auxiliary_value(kAuxSystemInfoHeader) != 0 && auxiliary_value(kAuxRandom) != 0,
           "startup_auxiliary_vector");
    expect(process_id() != 0 && sys_gettid() > 0 && current_thread_id() == static_cast<u32>(sys_gettid()),
           "startup_process_and_thread_ids");
    expect(environment_get("MAICORD_NO_SUCH_VARIABLE") == nullptr, "startup_unset_variable");
}

void test_clock() noexcept {
    const i64 first = clock_now();
    const i64 second = clock_now();
    const Clock clock = clock_initialize();
    expect(clock.frequency == 1'000'000'000 && first > 0 && second >= first &&
               clock_microseconds(clock, first, second) < 1'000'000,
           "clock_monotonic_counter");
    // After 2020-01-01 and before 2100-01-01.
    const u64 unix_milliseconds = clock_unix_milliseconds();
    expect(unix_milliseconds > 1'577'836'800'000ull && unix_milliseconds < 4'102'444'800'000ull,
           "clock_unix_milliseconds");
    expect(clock_monotonic_milliseconds() > 0, "clock_boottime_milliseconds");
}

void test_threads() noexcept {
    TestThread thread{};
    volatile u32 value = 0;
    u32 result = 0;
    expect(test_thread_start(&thread, thread_body, const_cast<u32*>(&value)) &&
               test_thread_finish(&thread, 5000, &result) && result == 7 && value == 42,
           "pthread_create_and_join");
}

void test_libraries() noexcept {
    SharedLibrary library{};
    expect(shared_library_open(&library, "libc.so.6"), "dlopen_libc");
    using GetProcessId = int (*)();
    const auto get_process_id = reinterpret_cast<GetProcessId>(shared_library_symbol(library, "getpid"));
    expect(get_process_id && static_cast<u32>(get_process_id()) == process_id(), "dlsym_getpid");
    expect(shared_library_symbol(library, "maicord_no_such_symbol") == nullptr, "dlsym_missing_symbol");
    shared_library_close(&library);
    expect(library.handle == nullptr, "dlclose");

    SharedLibrary missing{};
    expect(!shared_library_open(&missing, "libmaicord-no-such-library.so.9") && missing.handle == nullptr &&
               shared_library_error() != nullptr,
           "dlopen_missing_library_reports_error");
    expect(!shared_library_open(nullptr, "libc.so.6") && !shared_library_open(&missing, "") &&
               !shared_library_open(&missing, nullptr),
           "dlopen_rejects_empty_names");
}

// MAICORD_LIBRARY_PATH names a directory without the library first: the search continues to the loader.
void test_library_path() noexcept {
    expect(text_equal(environment_get("MAICORD_LIBRARY_PATH") ? environment_get("MAICORD_LIBRARY_PATH") : "",
                      "/maicord-no-such-directory:/maicord-another"),
           "library_path_environment");
    SharedLibrary library{};
    expect(shared_library_open(&library, "libc.so.6"), "library_path_falls_through_to_the_loader");
    shared_library_close(&library);
}

bool wide_equal_text(const wchar_t* wide, const char* text) noexcept {
    char converted[512];
    return path_to_utf8(wide, converted, sizeof(converted)) != 0 && text_equal(converted, text);
}

// XDG_DATA_HOME is set (absolute): <it>/MaiCord, with the relative path's backslashes as slashes.
void test_data_path_xdg() noexcept {
    wchar_t path[256]{};
    const DataPathResult plain = data_path(path, 256, nullptr);
    expect(plain.ok() && wide_equal_text(path, "/maicord-xdg-test/data/MaiCord"), "data_path_xdg_data_home");
    const DataPathResult nested = data_path(path, 256, L"plugin-data\\effect.bin");
    expect(nested.ok() && wide_equal_text(path, "/maicord-xdg-test/data/MaiCord/plugin-data/effect.bin"),
           "data_path_xdg_relative_with_backslash");
    const DataPathResult tiny = data_path(path, 8, nullptr);
    expect(!tiny.ok() && tiny.error == DataPathError::path, "data_path_xdg_capacity");
    expect(profile_data_path(0x1fu, path, 256, L"accounts\\links-", L".trust") &&
               wide_equal_text(path, "/maicord-xdg-test/data/MaiCord/accounts/links-000000000000001f.trust"),
           "data_path_xdg_profile_file");
    expect(!profile_data_path(0, path, 256, L"accounts\\links-", L".trust"), "data_path_xdg_profile_zero");
}

// XDG_DATA_HOME is unset: $HOME/.local/share/MaiCord.
void test_data_path_home() noexcept {
    wchar_t path[256]{};
    const DataPathResult built = data_path(path, 256, L"logs/maicord.log");
    expect(environment_get("XDG_DATA_HOME") == nullptr && built.ok() &&
               wide_equal_text(path, "/maicord-home-test/.local/share/MaiCord/logs/maicord.log"),
           "data_path_home_fallback");
}

// A relative XDG_DATA_HOME is ignored, as the XDG specification requires.
void test_data_path_relative_xdg() noexcept {
    wchar_t path[256]{};
    const DataPathResult built = data_path(path, 256, nullptr);
    expect(built.ok() && wide_equal_text(path, "/maicord-home-test/.local/share/MaiCord"),
           "data_path_relative_xdg_ignored");
}

// Directories are created from a private XDG_DATA_HOME ("test-profile" in its path): every ancestor of the file,
// the data root included, and an existing directory is accepted again.
void test_data_path_create() noexcept {
    const char* data_home = environment_get("XDG_DATA_HOME");
    bool private_home = false;
    for (u32 start = 0; data_home && data_home[start] && !private_home; ++start) {
        const char* name = "test-profile";
        u32 index = 0;
        while (name[index] && data_home[start + index] == name[index]) ++index;
        private_home = name[index] == 0;
    }
    expect(private_home, "data_path_create_requires_private_data_home");
    if (!private_home) return;
    wchar_t path[512]{};
    char text[1536]{};
    const DataPathResult built = data_path(path, 512, L"a\\b\\c.bin", true);
    expect(built.ok() && path_to_utf8(path, text, sizeof(text)) != 0, "data_path_create_builds");
    // The parent directories exist now, the file itself does not.
    u32 end = 0;
    while (text[end]) ++end;
    while (end && text[end - 1] != '/') --end;
    text[end - 1] = 0;
    const long directory = sys_open(text, kOpenReadOnly | kOpenDirectory);
    expect(!syscall_failed(directory), "data_path_create_makes_parents");
    if (!syscall_failed(directory)) sys_close(static_cast<int>(directory));
    expect(data_path(path, 512, L"a\\b\\c.bin", true).ok(), "data_path_create_accepts_existing");
    // Remove what was made: b, a and MaiCord (the profile directory is the harness's), then make MaiCord again.
    for (u32 directories = 0; directories < 3; ++directories) {
        sys_remove_directory(text);
        end = 0;
        while (text[end]) ++end;
        while (end && text[end - 1] != '/') --end;
        text[end ? end - 1 : 0] = 0;
    }
    expect(data_path(path, 512, L"d", true).ok() && data_path(path, 512, nullptr).ok() &&
               path_to_utf8(path, text, sizeof(text)) != 0 && !syscall_failed(sys_remove_directory(text)),
           "data_path_create_recreates_after_removal");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_linux_runtime_entry) {
    using namespace maicord;
    const ProcessStartup& startup = process_startup();
    const char* mode = startup.argc > 1 ? startup.argv[1] : "";
    if (text_equal(mode, "--library-path")) {
        test_library_path();
    } else if (text_equal(mode, "--data-path-xdg")) {
        test_data_path_xdg();
    } else if (text_equal(mode, "--data-path-home")) {
        test_data_path_home();
    } else if (text_equal(mode, "--data-path-relative-xdg")) {
        test_data_path_relative_xdg();
    } else if (text_equal(mode, "--data-path-create")) {
        test_data_path_create();
    } else {
        test_startup();
        test_clock();
        test_threads();
        test_libraries();
    }
    test_exit(g_failures, "maicord_linux_runtime");
}
