#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/dbus/file_chooser.hpp"
#include "maicord/platform/dbus/instance.hpp"
#include "maicord/platform/dbus/tray.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/platform/dbus/desktop_settings.hpp"
#include "maicord/platform/dbus/portal.hpp"
#include "maicord/platform/dbus/secret_service.hpp"
#include "maicord/platform/dbus/session.hpp"
#include "maicord/sys/linux/process.hpp"
#include "maicord/platform/crypto/secure_buffer.hpp"
#include "../../support/test_output.hpp"

// The clients against the real desktop services (tools/test_dbus_real.sh starts xdg-desktop-portal with its GTK backend and
// gnome-keyring on a private session bus; MAICORD_DBUS_REAL=1 says so): what the settings portal and the keyring answer is
// printed, and the checks are the ones any conforming service passes. The values themselves (the desktop's own) are not
// asserted. Without MAICORD_DBUS_REAL=1 the test skips (77).

namespace maicord {
namespace {

using namespace dbus;

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

void report(const char* name, u64 value) noexcept {
    test_write_metric(name, value);
}

void report_text(const char* name, const char* text) noexcept {
    test_write_text(name);
    test_write_line(text);
}

bool text_equal(const char* a, const char* b) noexcept {
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

void test_settings() noexcept {
    check(desktop_settings_start(), "the session bus opens");
    const bool loaded = desktop_settings_wait_loaded(20000);  // the portal starts on first use
    check(loaded, "the settings portal answers ReadAll");
    DesktopSettings s;
    desktop_settings_get(&s);
    report("portal: color scheme (0 none, 1 dark, 2 light) ", static_cast<u64>(s.color_scheme));
    report("portal: reduced motion ", s.reduced_motion ? 1 : 0);
    report("portal: animations ", s.animations ? 1 : 0);
    report("portal: cursor size ", s.cursor_size);
    report("portal: double click ms (0 unknown) ", s.double_click_ms);
    report_text("portal: ui font ", s.ui_font);
    report_text("portal: monospace font ", s.monospace_font);
    report_text("portal: cursor theme ", s.cursor_theme);
    // Whatever the desktop publishes, a loaded read has a font (GTK's settings carry one) and sane ranges.
    check(!loaded || s.cursor_blink_ms >= 100, "the blink time is in range");
}

void test_probes() noexcept {
    report("portal: Settings version ", portal_interface_version("org.freedesktop.portal.Settings", 5000));
    report("portal: OpenURI version ", portal_interface_version("org.freedesktop.portal.OpenURI", 5000));
    report("portal: Notification version ", portal_interface_version("org.freedesktop.portal.Notification", 5000));
    report("portal: Inhibit version ", portal_interface_version("org.freedesktop.portal.Inhibit", 5000));
    check(portal_interface_version("org.freedesktop.portal.Settings", 5000) >= 1, "the Settings portal has a version");
    check(portal_interface_version("org.freedesktop.portal.NoSuchThing", 5000) == 0, "an unknown interface is version 0");
}

void test_secrets() noexcept {
    const SecretAttribute attributes[2] = {{"application", "maicord-test"}, {"purpose", "real-service-check"}};
    u8 secret[64];
    u32 length = 0;
    const SecretStatus first = secret_lookup(attributes, 2, secret, sizeof(secret), &length);
    report_text("keyring: first lookup ", secret_status_name(first));
    if (first == SecretStatus::unavailable || first == SecretStatus::locked) {
        test_write_line("SKIP keyring checks: no unlocked Secret Service on the bus");
        return;
    }
    check(first == SecretStatus::not_found, "a fresh keyring has no such item");
    u8 key[32];
    for (u32 i = 0; i < 32; ++i)
        key[i] = static_cast<u8>(i * 5 + 3);
    const SecretStatus stored = secret_store("MaiCord test secret", attributes, 2, key, 32);
    report_text("keyring: store ", secret_status_name(stored));
    check(stored == SecretStatus::ok, "the keyring stores a secret");
    check(secret_lookup(attributes, 2, secret, sizeof(secret), &length) == SecretStatus::ok && length == 32 &&
              bytes_equal(secret, key, 32),
          "it reads back");
    u8 key2[32];
    for (u32 i = 0; i < 32; ++i)
        key2[i] = static_cast<u8>(200 - i);
    check(secret_store("MaiCord test secret", attributes, 2, key2, 32) == SecretStatus::ok &&
              secret_lookup(attributes, 2, secret, sizeof(secret), &length) == SecretStatus::ok && length == 32 &&
              bytes_equal(secret, key2, 32),
          "storing again replaces it");
    check(secret_erase(attributes, 2) == SecretStatus::ok, "erase");
    check(secret_lookup(attributes, 2, secret, sizeof(secret), &length) == SecretStatus::not_found, "and it is gone");
}

// The user-bound secure buffer over the real keyring: the master key lands in the Secret Service, not in a file.
void test_master_key() noexcept {
    const SecretAttribute attributes[2] = {{"application", "maicord"}, {"purpose", "master-key"}};
    secret_erase(attributes, 2);
    SecureBuffer plain{};
    check(secure_buffer_initialize(&plain, 32).ok(), "a secure buffer");
    const u8 text[12] = {'r', 'e', 'a', 'l', ' ', 'k', 'e', 'y', 'r', 'i', 'n', 'g'};
    secure_buffer_assign(&plain, ConstSlice<u8>{text, 12});
    u8 blob[96];
    const auto sealed = secure_buffer_protect_user_bound(plain, ConstSlice<u8>{}, Slice<u8>{blob, sizeof(blob)});
    check(sealed.ok(), "protect with the real keyring");
    SecureBuffer opened{};
    secure_buffer_initialize(&opened, 32);
    const auto unsealed = secure_buffer_unprotect_user_bound(ConstSlice<u8>{blob, sealed.bytes}, ConstSlice<u8>{}, &opened);
    check(unsealed.ok() && opened.bytes == 12 && bytes_equal(opened.data, text, 12), "and it opens");
    u8 key[32];
    u32 length = 0;
    check(secret_lookup(attributes, 2, key, sizeof(key), &length) == SecretStatus::ok && length == 32,
          "the keyring holds a 32-byte master key (application=maicord, purpose=master-key)");
    secure_buffer_release(&plain);
    secure_buffer_release(&opened);
    secret_erase(attributes, 2);
}

// The file chooser portal: the real one (the GTK backend, on the user's display) accepts our options, keeps the request
// open for the user, and closes it on cancel. The dialog itself needs a person, so it is not answered.
void test_file_chooser() noexcept {
    const char* const images[2] = {"*.png", "*.jpg"};
    const FileFilter filters[1] = {{"Images", images, 2}};
    FileChooserOptions options{};
    options.title = "MaiCord test (closes itself)";
    options.filters = filters;
    options.filter_count = 1;
    options.multiple = true;
    options.current_folder = "/tmp";
    FileChooser* chooser = file_chooser_open(options);
    check(chooser != nullptr, "the open dialog request is made");
    if (!chooser)
        return;
    const u64 until = clock_monotonic_milliseconds() + 2500;
    while (clock_monotonic_milliseconds() < until && file_chooser_state(chooser) == FileChooserState::pending) {
        Waitable waitable = session_bus_waitable();
        if (waitable)
            wait_many(&waitable, 1, 50, 0);
        session_bus_process(clock_monotonic_milliseconds());
    }
    const FileChooserState state = file_chooser_state(chooser);
    if (state == FileChooserState::failed) {
        test_write_line("SKIP file chooser: the portal refused (no display for its backend?)");
    } else {
        check(state == FileChooserState::pending, "the real portal accepts the options and waits for the user");
    }
    file_chooser_cancel(chooser);
    check(file_chooser_state(chooser) != FileChooserState::pending, "cancel ends it");
    file_chooser_release(chooser);
    report("portal: FileChooser version ", portal_interface_version("org.freedesktop.portal.FileChooser", 5000));
    report("portal: GlobalShortcuts version (0: not offered by this desktop) ",
           portal_interface_version("org.freedesktop.portal.GlobalShortcuts", 5000));
}

// Files shared with the script that drives the --tray and --instance runs.
bool sync_exists(const char* directory, const char* name) noexcept {
    char path[512];
    u32 length = 0;
    for (; directory[length] && length < 400; ++length)
        path[length] = directory[length];
    path[length++] = '/';
    for (u32 i = 0; name[i]; ++i)
        path[length++] = name[i];
    path[length] = 0;
    const long opened = sys_openat(kAtCurrentDirectory, path, kOpenReadOnly | kOpenCloseOnExec);
    if (syscall_failed(opened))
        return false;
    sys_close(static_cast<int>(opened));
    return true;
}

void sync_write(const char* directory, const char* name, const char* text, bool append) noexcept {
    char path[512];
    u32 length = 0;
    for (; directory[length] && length < 400; ++length)
        path[length] = directory[length];
    path[length++] = '/';
    for (u32 i = 0; name[i]; ++i)
        path[length++] = name[i];
    path[length] = 0;
    const long opened = sys_openat(kAtCurrentDirectory, path,
                                   kOpenWriteOnly | kOpenCreate | (append ? 0x400 /* O_APPEND */ : 0x200 /* O_TRUNC */) |
                                       kOpenCloseOnExec,
                                   0600);
    if (syscall_failed(opened))
        return;
    u32 n = 0;
    while (text[n])
        ++n;
    sys_write(static_cast<int>(opened), text, n);
    sys_close(static_cast<int>(opened));
}

void pump_bus(u32 milliseconds) noexcept {
    const u64 until = clock_monotonic_milliseconds() + milliseconds;
    while (clock_monotonic_milliseconds() < until) {
        Waitable waitable = session_bus_waitable();
        u32 wait = 50;
        if (waitable)
            wait_many(&waitable, 1, wait, 0);
        session_bus_process(clock_monotonic_milliseconds());
    }
}

// A real StatusNotifierHost (swaybar's tray, in tools/test_dbus_tray.sh) takes the item: a magenta mark in two sizes, the
// menu of the Windows tray. `ready` says it registered; the script screenshots the bar and asks the item and its menu
// with busctl, then writes `done`.
void run_tray(const char* directory) noexcept {
    static u32 big[32 * 32];
    static u32 small[22 * 22];
    for (u32& pixel : big)
        pixel = 0xFFFF00FFu;
    for (u32& pixel : small)
        pixel = 0xFFFF00FFu;
    const TrayPixmap icons[2] = {{32, 32, big}, {22, 22, small}};
    const TrayMenuEntry menu[4] = {{"Open MaiCord", 1, false, false, false},
                                   {"Mute", 2, true, false, false},
                                   {"Deafen", 3, true, true, false},
                                   {"Quit MaiCord", 4, false, false, true}};
    check(tray_start("maicord", [](void*, const TrayEvent&) noexcept {}, nullptr), "the tray starts");
    tray_update(TrayState{"MaiCord", "tray check", icons, 2, false, menu, 4});
    const u64 until = clock_monotonic_milliseconds() + 15000;
    while (clock_monotonic_milliseconds() < until && !tray_available())
        pump_bus(50);
    check(tray_available(), "the bar's watcher accepts the item");
    if (tray_available())
        sync_write(directory, "ready", "ready\n", false);
    const u64 wait_until = clock_monotonic_milliseconds() + 40000;
    while (clock_monotonic_milliseconds() < wait_until && !sync_exists(directory, "done"))
        pump_bus(50);
    tray_stop();
}

// The first instance for `gdbus call`, a real GLib client of org.freedesktop.Application: requests it receives are
// appended to `requests`; `ready` says it is up; `done` ends it.
void run_instance(const char* key, const char* directory) noexcept {
    const InstanceConfig config{"io.github.maj113.MaiCord", key};
    LaunchRequest none{};
    check(instance_acquire(config, none, 2000) == InstanceResult::primary, "the first instance");
    instance_set_handler(
        [](void* context, const LaunchRequest& request) noexcept {
            const char* directory = static_cast<const char*>(context);
            sync_write(directory, "requests", request.url_present ? request.url : "(activate)", true);
            sync_write(directory, "requests", request.activation_token[0] ? " token=" : "", true);
            sync_write(directory, "requests", request.activation_token, true);
            sync_write(directory, "requests", "\n", true);
        },
        const_cast<char*>(directory));
    sync_write(directory, "ready", "ready\n", false);
    const u64 wait_until = clock_monotonic_milliseconds() + 40000;
    while (clock_monotonic_milliseconds() < wait_until && !sync_exists(directory, "done"))
        pump_bus(50);
    instance_release();
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_dbus_real_entry) {
    using namespace maicord;
    const char* real = environment_get("MAICORD_DBUS_REAL");
    if (!real || !text_equal(real, "1")) {
        test_write_line("SKIP dbus_real (tools/test_dbus_real.sh starts the real portal and keyring on a private bus)");
        test_process_exit(77);
    }
    const ProcessStartup& startup = process_startup();
    if (startup.argc >= 3 && text_equal(startup.argv[1], "--tray")) {
        run_tray(startup.argv[2]);
        test_exit(g_failures, "dbus_real_tray");
    }
    if (startup.argc >= 4 && text_equal(startup.argv[1], "--instance")) {
        run_instance(startup.argv[2], startup.argv[3]);
        test_exit(g_failures, "dbus_real_instance");
    }
    test_settings();
    test_probes();
    test_secrets();
    test_master_key();
    test_file_chooser();
    test_exit(g_failures, "dbus_real");
}
