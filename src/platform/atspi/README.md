# Accessibility on Linux: AT-SPI

The counterpart of the UI Automation server (`src/platform/uia`): the same `UiAccessTree` snapshots, served over the in-house
D-Bus client (`src/platform/dbus`). Headers are in `include/maicord/platform/atspi`.

Nothing is connected until it is wanted. `server.hpp` watches `org.a11y.Bus` on the session bus and reads `org.a11y.Status`
without starting the service. When `IsEnabled` or `ScreenReaderEnabled` is true it asks `GetAddress`, opens the accessibility bus
and registers the application root with the registry (`org.a11y.atspi.Socket.Embed`); both false, or the bus ending, closes it.

| File | Role |
| --- | --- |
| `accessible.cpp` | One method call in, one reply out: the application root (`Application`) and each snapshot node (`Accessible`, `Component`, `Action`, `Text`, `EditableText`, `Value`, `Selection`, `Properties`). Paths are `/org/a11y/atspi/accessible/<key>`. |
| `map.cpp` | Role, state, interface and action mapping. |
| `events.cpp` | Focus, state, text (insert and delete as a diff of the focused value), caret, property, children and `Announcement` signals once a client has called; before that a frame costs a flag test. |
| `server.cpp` | The connection lifecycle above. |

- The accessibility bus connection is a `SessionBusExtra`: the Wayland pump waits on it and processes it with the session bus.
- Bounds are logical pixels (the snapshot's DIPs) from the window's own origin; Wayland gives a client no screen position, so
  screen coordinates equal window coordinates.
- Password fields expose no text.
- The host is `src/app/accessibility/linux`: callbacks over the shared snapshot and `access_act` (deferred actions arrive as
  `wake_accessibility` events in `application_window_event`) and the frame hook. The snapshot (`accessibility_describe.cpp`),
  the actions (`accessibility_actions.cpp`) and the status announcements (`accessibility_status.cpp`) are shared with
  Windows, so a new describer, action or announcement needs no AT-SPI work.

## Tests

- `maicord_atspi_map`: the mapping and the answers on a synthetic window, no bus.
- `maicord_atspi_bus` (`tools/test_dbus.sh`, the bare private bus): plays `org.a11y.Bus`, the registry and a screen reader around the
  server: off means no connection; `IsEnabled` or `ScreenReaderEnabled` brings it and the `Embed`, a call, an action, the events
  of a snapshot difference, off again, and a registry that comes back.
- `tools/test_atspi.sh`: the client on headless sway behind the real `at-spi-bus-launcher` and registry;
  `tests/platform/atspi/atspi_client.py` (python3-gi, gir1.2-atspi-2.0) walks the sign-in form, invokes a button, reads and sets
  text and waits for events. `--orca` also runs Orca (private Xvfb, speech-dispatcher's dummy module) and reads what it spoke
  from its debug log.
