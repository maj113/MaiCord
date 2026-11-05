# Wayland protocol XML

The protocol descriptions the Linux window backend (`src/platform/window/wayland`) speaks, unmodified, and under
the MIT licence stated in each file's `<copyright>` block. `tools/generate_wayland_protocols.py` turns them into the
`wl_interface` tables and the typed request and listener wrappers at build time (`cmake/linux_window.cmake`);
`wayland-scanner` is not used and not needed.

| File | Source | Used for |
|---|---|---|
| `wayland.xml` | wayland 1.22 | core: compositor, shm, seat, pointer, keyboard, data device |
| `xdg-shell.xml` | wayland-protocols 1.45 (stable) | toplevel windows |
| `viewporter.xml` | stable | fractional-scale buffer sizes |
| `tablet-v2.xml` | stable | only the types `cursor-shape-v1` names |
| `fractional-scale-v1.xml` | staging | fractional scale |
| `cursor-shape-v1.xml` | staging | cursor shapes |
| `xdg-activation-v1.xml` | staging | window attention |
| `ext-idle-notify-v1.xml` | staging | system idle |
| `xdg-toplevel-icon-v1.xml` | staging | window icon |
| `xdg-system-bell-v1.xml` | staging | alert beep |
| `xdg-foreign-unstable-v2.xml` | unstable | the window as the parent of portal dialogs |
| `text-input-unstable-v3.xml` | unstable | IME |
| `primary-selection-unstable-v1.xml` | unstable | primary selection |
| `xdg-decoration-unstable-v1.xml` | unstable | server-side decorations |
| `idle-inhibit-unstable-v1.xml` | unstable | idle inhibit during calls |
| `wlr-virtual-pointer-unstable-v1.xml` | wlr-protocols | tests only: a virtual pointer for sway |
| `virtual-keyboard-unstable-v1.xml` | wtype (atx/wtype) | tests only: a virtual keyboard for sway |
| `input-method-unstable-v2.xml` | wlroots 0.17 | tests only: the test acts as an IME |
| `wlr-data-control-unstable-v1.xml` | wlr-protocols | tests only: the test acts as a clipboard manager |

To update one, copy the new file over the old, rebuild and run `maicord_wayland_input`, which compares the generated core
tables with libwayland-client's own.
