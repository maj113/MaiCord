# Wayland window backend

`src/platform/window/wayland` is the Linux implementation of the window service (`include/maicord/platform/window/window.hpp`,
`event.hpp`, `key.hpp`): one connection, one top-level window, no X11. Everything runs on the UI thread except `window_wake`,
`window_post` and the global key bits (atomic). Listeners only record state and push `WindowEvent`s into a ring; `window_pump`
drains it and calls the sink, so no `wl_*_listener` ever runs app code. `wayland.hpp` carries what has no Windows counterpart.

## Files

| File | Holds |
|---|---|
| `client.cpp`, `xkb.cpp` | `dlopen` of libwayland-client, libwayland-cursor, libxkbcommon (no `DT_NEEDED`) |
| `window.cpp` | registry, xdg-shell window, configure sequence, scale (fractional-scale + viewporter, or integer), opaque region, frame callback, `platform_settings_read` |
| `extras.cpp` | xdg-activation, idle inhibit, xdg-foreign export, ext-idle-notify watch, xdg-system-bell, xdg-toplevel-icon |
| `pointer.cpp`, `keyboard.cpp`, `keymap.cpp`, `global_keys.cpp`, `frame.cpp` | input, cursor (cursor-shape, `wl_cursor` fallback), keys, CSD hit tests |
| `data.cpp`, `uri.cpp`, `text_input.cpp` | clipboard, primary selection, drag and drop, IME (text-input-v3) |
| `pump.cpp` | `window_wait` / `window_pump`: display fd, loop wake, key-repeat timer, session bus |

Bindings are generated at build time by `tools/generate_wayland_protocols.py` from `third_party/wayland` (see its README): every
request is one `wl_proxy_marshal_array_flags` call (libwayland-client 1.20, Ubuntu 22.04's), which also makes a constructor's
object and destroys a destructor's proxy. `wl_proxy_marshal_array` and `_constructor_versioned` forward to it through one
to three calls, and a destructor then took the display lock a second time in `wl_proxy_destroy`. Measured with callgrind on
`tools/test_wayland_window.sh` (libwayland 1.22, the same requests in both runs, Ir in libwayland per request): lifecycle
2068 to 2036 (246 requests), input 2490 to 2470 (599); a destructor (`wl_surface_destroy`, `wl_data_offer_destroy`, an
activation token) 89 fewer; the resize step (`surface_sync`: viewport, opaque region) 1838 to 1813; the cursor 1871 to 1859.
The test program's own code grew by 308 Ir over the lifecycle run (1.3 per request), and the client by 64 bytes. A new
global is bound in `on_global` (`window.cpp`), kept in `Backend` (`backend.hpp`), destroyed in `disconnect`.

## Rules

- The window has no buffers of its own. The GPU backend (`src/gpu/gles`) creates its `wl_egl_window` on `window_surface_native`
  and commits with `eglSwapBuffers`; the window keeps the viewport destination, the opaque region and the buffer scale in step
  (`surface_sync`, pending state applied by the swap's commit), each sent only when its value changes: the surface keeps it.
  Never attach or commit a buffer from here.
- Frame clock: swap interval 0, and one `wl_surface.frame` per present (`window_frame_callback_request` queues it, the swap
  commits it); `frame_done` feeds `gpu_surface_frame_callback`. Do not create objects on the surface that the EGL driver owns:
  NVIDIA's egl-wayland makes the surface's `wp_linux_drm_syncobj_surface_v1` (a second one is the protocol error
  `surface_exists`), Mesa's EGL binds `wp_presentation` and its own frame and `wl_display_sync` throttles (Mesa 25.2
  `platform_wayland.c`), and Mesa's Vulkan WSI makes the fifo and commit-timer objects (a second fifo object is `already_exists`).
- Features are optional: every protocol except wl_compositor 4, wl_shm and xdg_wm_base is bound when present and silently absent
  otherwise; the call then returns false or does nothing. Never fail the window for a missing integration protocol.
- Values the compositor sends are applied at the xdg `configure` ack, never in the middle of a sequence.
- `window_pump` after a `window_wait` reads only what that wait saw ready: the wait read the display itself, and the session
  bus, instance socket and bus extras it saw idle are not read again (`connection_process(..., readable)`). Descriptors are
  level-triggered, so whatever arrives later makes the next wait return at once; a pump without a wait before it reads all.
  After a wait the pump does not flush either: what the listeners asked goes out with the swap's commit or the next wait's
  flush. Calls whose contract is "at once" (text input, selections, activation, the bell, the icon) flush themselves.
- New env knobs are test hooks (`test_hook`), not user options.

## Protocols

| Protocol | Use |
|---|---|
| xdg-shell 7, xdg-decoration | window, client-side frame (our caption bar), `suspended` state |
| fractional-scale-v1 + viewporter | scale in 1/120; falls back to wl_surface buffer scale |
| cursor-shape-v1 | cursors; `wl_cursor` theme fallback |
| text-input-v3, wl_data_device, primary-selection | IME, clipboard, drag and drop, primary selection |
| xdg-activation-v1 | `window_request_attention` (own token), `window_wayland_activate` (the token of a notification click or a second launch) |
| zwp_idle_inhibit | `window_wayland_set_idle_inhibit`: screen stays on while the call view is open |
| ext-idle-notify-v1 | `window_wayland_watch_idle`: `PlatformSettings::system_idle_ms` (presence idle, read acks); v2's input-only notification when offered; GNOME has none |
| xdg-foreign-v2 (exporter) | the window's handle goes to `dbus::portal_set_parent_window`; FileChooser, OpenURI and GlobalShortcuts send it as `parent_window` |
| xdg-system-bell-v1 | `desktop_alert_beep` |
| xdg-toplevel-icon-v1 | `window_set_icon` (the theme-coloured mark), KWin and labwc |

Evaluated and not used (support from wayland.app, Mutter 51, KWin 6.7, Sway 1.11, Hyprland 0.52, COSMIC 1.0 beta, Weston 15):

- fifo-v1 (Mutter, KWin, Hyprland, Weston), commit-timing-v1 (Mutter, Hyprland, Weston): a barrier constrains commits that
  `eglSwapBuffers` makes, a queue of constrained commits cannot be replaced by a newer frame (latency grows), and the protocol
  still asks for frame callbacks where the surface is not updated. The callback is the right clock for an event-driven UI.
- linux-drm-syncobj-v1 (all but niri, Wayfire, Weston): NVIDIA egl-wayland creates the surface's syncobj itself, Mesa EGL relies
  on implicit sync; ours would be `surface_exists`.
- linux-dmabuf-v1 feedback: Mesa's EGL follows it for the swapchain; VA-API takes the EGL device's render node
  (`EGL_DRM_RENDER_NODE_FILE_EXT`), so the main device is already the same.
- presentation-time: nothing reads a refresh rate or a presentation time (animations sample the clock; Mesa binds its own).
- tearing-control-v1: a chat client has nothing to gain from tearing.
- xdg-dialog-v1, xdg-toplevel-drag-v1, xdg-toplevel-tag-v1: one window, in-window dialogs, no tabs to tear off.
- content-type-v1, single-pixel-buffer, alpha-modifier: no surface they would simplify (opaque EGL window).
- color-management-v1 / color-representation-v1 (HDR): Mutter, KWin, Hyprland have color-management; the renderer is sRGB 8-bit
  throughout; revisit with an HDR video path.
- ext-data-control: clipboard managers are the other side of the protocol; the window uses wl_data_device.

## Tests

- `maicord_wayland_input` (normal suite): key table, characters, double click, repeat, generated tables against libwayland-client's.
- `tools/test_wayland_window.sh [--compositor headless|nested|current] [--scale S] [--integer]`: headless sway runs
  `maicord_wayland_window_lifecycle` (window, frame callbacks, xdg-foreign handle, bell, icon, opaque region) and
  `maicord_wayland_window_input` (virtual pointer and keyboard, two helper clients, idle inhibit, system idle time).
  Without a GPU the tests attach a `wl_shm` buffer themselves (`Painter`). xdg-toplevel-icon and xdg-system-bell are not in
  sway 1.9 (wlroots 0.17): their requests are exercised for protocol errors only where the compositor has them.
