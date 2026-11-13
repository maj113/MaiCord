# Desktop services over D-Bus (Linux)

An in-house D-Bus client: no libdbus, no sd-bus. Headers are in `include/maicord/platform/dbus`.

| File | Role |
| --- | --- |
| `wire.hpp` | The wire format: little endian; the Reader walks the whole type system, the Builder writes what the client sends; received `UNIX_FD`s over SCM_RIGHTS. |
| `connection.hpp` | One bus connection: EXTERNAL authentication, calls with timeouts, `AddMatch` signals, exported methods. One thread per connection; a `Waitable` for the loop. |
| `session.hpp` | The session bus the window loop waits on, plus `SessionBusExtra` (further connections the Wayland pump waits on; their due times bound the sleep). |
| `desktop_settings.hpp` | The settings portal: colour scheme, motion, interface fonts, cursor. The window backend maps it to `PlatformSettings` and `settings_changed`. |
| `notifications.hpp` | Toasts: the daemon directly, the portal when sandboxed (`MAICORD_NOTIFICATION_BACKEND=portal\|direct` overrides). |
| `open_uri.hpp` | OpenURI portal, then `xdg-open` through `posix_spawnp` looked up with `dlsym`. |
| `secret_service.hpp` | Secret Service, private connections, any thread. |
| `tray.hpp` | StatusNotifierItem with `com.canonical.dbusmenu`, registered when a watcher exists, silent without (stock GNOME has none). |
| `global_shortcuts.hpp` | GlobalShortcuts portal; the window backend's `window_wayland_global_shortcut` makes a bound key read as down in `global_key_down`; focused-window only where the portal is missing. |
| `instance.hpp` | Single instance and `discord://` links over `org.freedesktop.Application` on the application id, or an abstract Unix socket when there is no bus. |
| `file_chooser.hpp` | FileChooser portal, same shape as `native_file_picker`. |
| `launcher_entry.hpp` | The Unity launcher badge signal. |
| portal probe | `portal.cpp`: whether and at which version a portal interface exists. |

## Rules

- The application id is `io.github.maj113.MaiCord`; it is also the window's `app_id` and the `.desktop` file's name.
  `tools/generate_desktop_entry.py` writes the `.desktop` file (`MimeType=x-scheme-handler/discord`) and the hicolor icons
  into `<build>/packaging` (target `maicord_desktop_entry`).
- Portal dialogs (file chooser, OpenURI, global shortcuts, the colour picker's eyedropper: Screenshot.PickColor in
  `src/platform/desktop/screen_color.cpp`) send the window's `xdg-foreign` handle as `parent_window`
  (`portal_set_parent_window`, set by the Wayland window; empty without it).
- `secure_buffer` (`src/platform/crypto/linux`) keeps its master key in the Secret Service (`application=maicord`,
  `purpose=master-key`; an old key file moves in and is deleted) and falls back to the 0600 key file, logged, when there is no
  usable keyring. `MAICORD_SECRET_SERVICE=off` forces the file; every test runs with it off unless it asks for `auto`.
- Notifications carry what the Windows toast carries: title, body, the author avatar as the icon, the `default` action with the
  server's activation token, the sound request, removal by message, channel or profile; no reply box. The avatar PNG is
  `application_notification_avatar_uri`'s, in `$XDG_CACHE_HOME/maicord/notification-avatars`, handed to the daemon as a plain
  path and to the portal as the file's bytes (a sandbox's host cannot see the file). The daemon is asked once per session for
  `body-markup`; when it reads markup the body goes out with `&`, `<`, `>` as entities, so a message with a link or an image tag
  stays text.
- Startup entry and scheme handler (`platform/desktop/linux/integration.cpp`): an XDG autostart file, and a desktop file plus the
  default in `mimeapps.list`.

## Tests

- `maicord_dbus_wire` needs no bus.
- `tools/test_dbus.sh` starts private `dbus-daemon`s (`tools/dbus_test_bus.sh`: no service activation) with
  `tests/platform/dbus/mock_services.py` (python3-dbus, python3-gi; libdbus decodes every request and the tests spell out what it
  saw) and a bare one, and runs the connection, service, secure-buffer and no-service tests (`maicord_dbus_*`,
  `maicord_secure_buffer_service`, `maicord_linux_notifications_mock`); they skip (77) without the bus.
  `MAICORD_NOTIFICATION_BACKEND` and the test-only `MAICORD_INSTANCE_BACKEND=socket|bus` are set by the script.
- `tools/test_dbus_real.sh`: the clients against xdg-desktop-portal with its GTK backend and gnome-keyring on a private bus
  (needs a graphical session; not in CI).
- `tools/test_dbus_tray.sh`: the tray against swaybar's real watcher and host (a screenshot of its bar, `busctl` on the item and
  its menu) and single instance with `gdbus call`.
- `tools/test_wayland_window.sh` also starts the mock bus for the lifecycle test's settings stage.
