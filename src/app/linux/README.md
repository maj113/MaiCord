# The Linux client

The client is the `MaiCord` target of `cmake/linux_client.cmake`: the shell (`src/app/shell`) with this directory's entry and window
glue, over the Wayland backend only (`maicord_window_null` is the tests'). Resources Windows takes from `.rc` are embedded
(`core/linux/resource.cpp`: sounds and the icon, which `window_app_icon_png` reads as an ICO).

Not in `maicord_app_client` on Linux: `src/app/shell` (the client's own sources), `src/app/windows` (Win32 entry and window
procedure), `src/app/linux` (this), and the UI Automation server (`accessibility.cpp`; `src/app/accessibility/linux` stands in with the
AT-SPI host).

## Entry and window

- `entry.cpp`: `MaiCord --version` prints `MaiCord <version>` (the build's number as the updater knows it, `-dev.N+commit` for a
  master build) and exits 0 before anything else runs (`maicord_linux_client_version`); otherwise `argv` becomes the Windows command
  line (`platform/desktop/launch_line.hpp`).
- `platform_window.cpp`: the window through the Wayland backend; `application_platform_window_prepare` connects the compositor
  before the renderer's device start, which EGL needs. `application_platform_window_create` makes `window_set_frame_listener` feed
  `gpu_surface_frame_callback` (the surface's eventfd), and `gpu_surface_set_frame_request` has every windowed `gpu_surface_present`
  ask the window for the next callback before the swap commits. On Wayland `gpu_frame_wait` never blocks.
- The renderer is whatever Mesa's environment chooses; the client sets no driver itself. On WSL, `GALLIUM_DRIVER=d3d12` selects the
  GPU instead of llvmpipe. Measured on WSLg (Mesa 25.2.8, RX 9070 XT): scroll 61 fps on both renderers, 136% CPU on llvmpipe against
  28% on d3d12; first frame with cold caches 1.0 s of CPU on llvmpipe (shader JIT) against 0.4 s on d3d12 (the AMD d3d12 driver keeps
  its own cache in `$XDG_CACHE_HOME/AMD/DxcCache`).
- `shutdown_signal.cpp`: SIGTERM, SIGINT and SIGHUP are blocked at the entry before any thread exists and read from a `signalfd`; the
  main loop answers them like Quit, so the normal exit saves what a close saves (`maicord_linux_quit_signal`;
  `tools/test_client_wayland.sh --terminate TERM|INT|HUP` on the client).
- Wayland keeps only the last `xdg_toplevel.set_min_size`, so the loop calls `application_window_refresh_minimum_size()` every turn
  (`maicord_linux_window_minimum`).
- Push to talk beyond the focused window is the GlobalShortcuts portal (`app/voice/push_to_talk_binding.cpp`, bound while a call is open
  in push-to-talk mode; `maicord_push_to_talk_binding`).

## Shared code on Linux

`cmake/linux_app_client.cmake` globs `src/app` into `maicord_app_client`, so a new unit builds on Linux by default and must use the
seams (see AGENTS.md, "Where things live"): `os/process.hpp`, `os/text.hpp`, `os/thread.hpp`, `os/local_time.hpp`, `core/data_path.hpp`,
`core/module.hpp`, `core/resource.hpp`, `core/virtual_memory.hpp`, `platform/desktop/shell_open.hpp`, and a `Window` wherever a window is
passed, never an `HWND`. A Windows-only part keeps `#if defined(_WIN32)` in its file with the Linux alternative beside it:
`notifications/desktop.cpp`, `platform/desktop/taskbar_attention.cpp`, `platform/desktop/linux/notifications.cpp`,
`platform/desktop/linux/integration.cpp`, `fido2.cpp`/`webauthn_ceremony.cpp`, the plugin loader (`../plugins/README.md`) and game
detection. Application and UI fixtures that need no Win32 window run on Linux from `cmake/linux_app_ui_tests.cmake` over
`maicord_window_null`, with the Win32 calls they make shimmed for tests only in `tests/support/linux_test_app.hpp`; a fixture that calls
other Windows API directly stays Windows-only and is listed there.

Text that names the system goes through `MAICORD_OS_TEXT("Windows ...", "your desktop ...")` (`core/os_text.hpp`) and, for the settings
catalog, the Linux table in `catalog.cpp`; a row with nothing behind it on Linux is `settings_binding_unavailable` (the Windows
notification page, the voice processing switch). `maicord_linux_catalog_text` and `tools/check_linux_ui_text.py` (run on the built
image) fail when shown text names Windows on Linux. Shared code asks `os/path.hpp` (`path_root`, `path_leaf_start`, `path_plain`)
about paths, never a drive letter, and joins with `kPathSeparator`.

## Client identity (`account/linux/client_identity.cpp`)

The identity is the official desktop client of the host platform, because the account is the same account.
`client_user_agent.hpp` selects the profile (user agent, `client_version`, Electron and Chrome versions, `native_build_number`, `os`,
`sec-ch-ua-platform`) at compile time; `user_client_configuration.cpp` fills the host-derived properties through `RtlGetVersion` and the
registry on Windows, through this file on Linux: `os_version` is the kernel release (`uname`); `system_locale` and Accept-Language
come from `$LANGUAGE`, `$LC_ALL`, `$LC_MESSAGES`, `$LANG` through the shipped-locale resolution; `window_manager`
(`$XDG_CURRENT_DESKTOP`, `$GDMSESSION`), `distro` (lsb-release's description, else os-release's `PRETTY_NAME`), `runtime_environment`
(Flatpak/Snap/AppImage variables) and `display_server` (`$XDG_SESSION_TYPE`) follow discord_desktop_core.
`GatewayUserProperties::linux_desktop` selects the Linux object layout (`os_version_is_release` after `os_version`, no `os_sdk_version`,
host fields after `browser_version`); the Gateway client keeps the identify's properties in `GatewayUserRuntime` (`client_storage.cpp`),
or the identify is the Windows object and no session ever starts. A new property is a field of `LinuxClientEnvironment` and a fixture host
in `tests/app/account/client_identity.cpp` (`maicord_client_identity` pins the exact JSON); when Discord ships a new desktop build, bump the
Linux constants and those expected strings together (the Windows profile is separate).

## Updater (`src/app/update/linux`)

`package.cpp` reads the release tarball (or, for an AppImage, takes the file whole after a check of the runtime's identification),
`stage.cpp` swaps the install's `MaiCord`, `fonts/` and `licenses/` (keeping the old version in `MaiCord.old/`), `portable.cpp` is
`update_portable_gate`. The updater is on only for the install with the `.maicord-portable` marker
beside its real executable (`update_portable_install`, checked next to `/proc/self/exe` in `application_update_build_config()`) and for an
AppImage (`$APPIMAGE` a regular file in a writable directory, the program running from its mount: `update_appimage_install`), and only in
a build compiled with `MAICORD_LINUX_UPDATES_PUBLISHED` (from `MAICORD_LINUX_RELEASES=true` at configure time, as `release.yml`'s linux job
does). It reads the Linux manifests (`updates/p2/linux/`, signed by `maicord_release.py linux`; the AppImage's own, `arch`
`linux-x64-appimage`, in `linux/appimage/`: `g_update_flavor`); the AppImage is staged as `<APPIMAGE>.update`, swapped by the same
`update_swap` (one item, files instead of directories) and relaunched by `execve` of `$APPIMAGE`; the tarball install relaunches through `process_replace` (`execve`) too. Every package-manager
install leaves the marker out. `maicord_update_pipeline` replays the whole thing over a fake network
with a throwaway key (`tests/app/update/linux_package.cpp`); `MAICORD_UPDATE_TEST_TARBALL=<tarball from packaging/build_tarball.py>`
runs the same over the producer's own file.

## What is unreachable on purpose

`tools/audit_linux_reachability.sh` finds code no path from `_start` reaches. What remains is the PE plugin loader, the Windows
notification page and the voice processing switch (`settings_binding_unavailable`), and test-only entry points.
