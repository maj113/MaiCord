# Contributing to MaiCord

MaiCord is a small, opinionated codebase; these notes keep changes easy to review and the client small and fast.
[AGENTS.md](AGENTS.md) is the one-page rule sheet: read it first. This guide has the procedures behind it, and each area's
design notes are in the `README.md` beside its code ([Per-system seams](#per-system-seams) lists them).

## Before you start

- For anything larger than a bug fix, open an issue first so the approach can be agreed before you write the code. The issue
  forms cover bugs, missing Discord features and performance problems.
- Contributions must not include Discord-owned assets (images, sounds, fonts) or copied client code or bundles; see the
  [disclaimer](README.md#disclaimer). Research notes, captures and client bundles stay on your machine: `Analysis/` and
  `docs/` are git-ignored for this reason.

## Building and testing

One script per system builds the client, audits it and, with `-Test` / `--test`, builds and runs the test suite:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1 -Test
```

```sh
tools/build_linux.sh --test
```

Both take the same options, `-PascalCase` on Windows and `--kebab-case` on Linux: [AGENTS.md](AGENTS.md#build-and-test) has
the table and the rules of a run. `build.ps1`'s parameter block and `build_linux.sh`'s header document every option
(`-NoUpdater`, `-TestHooks`, the [`tools/perf`](tools/perf/README.md) probes, CI's shards and deadlines), and
[tools/README.md](tools/README.md#images-size-and-memory) lists the audit, comparison, size and memory tools.

The suite must finish within **20 seconds**: shrink a slow test's input but keep the coverage that matters, and assert
outputs and counts, never wall-clock times.

### Windows

- [Building from source](README.md#building-from-source) lists what to install: two Build Tools components (import
  `.vsconfig`; no IDE and no developer shell), CMake and Ninja (`winget install Kitware.CMake Ninja-build.Ninja`), Python.
- Close MaiCord before rebuilding. If it is running from the build directory (`..\MaiCord-build\Release`), add `-SkipClient`
  to a `-Test` run; otherwise the script refuses to rebuild over it.
- A local build numbers itself from git as CI numbers master builds
  ([client configuration](tools/release/README.md#client-configuration)), so the updater never offers it an older build, and
  nothing is installed until you choose **Restart to update**. `-NoUpdater` builds a client that never checks for or downloads
  releases, to test against your normal profile.

### Linux

- [Building from source](README.md#building-from-source) lists what to install; `tools/install_linux_toolchain.sh` does it
  on Debian and Ubuntu (`--tests` adds what the suite uses, `--integration` what the integration scripts use).
- Older hosts (Ubuntu 22.04, Debian 12) build with a stock LLVM, as distribution packages do:
  `MAICORD_LLVM_MAJOR=23 tools/build_linux.sh --llvm-bin /usr/lib/llvm-23/bin` (apt.llvm.org's clang 23).
- The build directory is `~/mc-build` (`MAICORD_LINUX_BUILD`); a checkout on a Windows or network mount is rsynced to `~/mc`
  (`MAICORD_LINUX_SOURCE`) first. From Windows, run the script in WSL:
  `wsl -d Ubuntu-Preview --cd <checkout> -- bash -l tools/build_linux.sh --test`.

### Toolchain

Both systems build with LLVM 24 from the [maj113/llvm-pe](https://github.com/maj113/llvm-pe/releases) release pinned in
`tools/llvm_toolchain.json`. The build downloads it on first use, checks its SHA-256 and keeps it in
`%LOCALAPPDATA%\MaiCord-toolchain\<tag>` or `~/.cache/maicord-toolchain/<tag>`; CI fetches the same pin.

- Another LLVM: `-LlvmBin DIR` / `--llvm-bin DIR` or `MAICORD_LLVM_BIN`, and on Linux `MAICORD_LLVM_MAJOR` for a major other
  than 24. The fork's code generation options are added only where the configure step finds the toolchain takes them, so a
  stock LLVM builds a bigger client instead of failing.
- MASM is the Build Tools' `ml64`, never `llvm-ml`, which would change `MaiCord.exe`.
- A new pin sets `tag`, `sha256` and `linux.sha256` in that file. [tools/README.md](tools/README.md#the-llvm-toolchain) says
  what a toolchain release must contain and how the build finds the Build Tools.

### Test hooks and the fake Discord

- A test-only environment switch calls `test_hook()` (`include/maicord/core/test_hook.hpp`), never `environment_get()`: a
  shipped client compiles it away. `--test` on Linux and `-TestHooks` on Windows define `MAICORD_TEST_HOOKS`; such an image
  carries a marker string that `tools/audit_image.py --no-test-hooks` rejects (run when a client without hooks links, and by
  `packaging/build_tarball.py`). Ship, package and measure size only from a build without them.
- `.\tools\build.ps1 -TestHooks` builds the Windows client alone with the hooks, in place of the shipping client.
  `tools\run_fake_server_client.ps1 [-Executable PATH] [-World DIR]` runs it against `tools/fake_discord_server.py` with a
  private profile and no account (sign in with any e-mail and password; `-Stop` ends both; redirect its output to a file, not
  a pipe). On Linux `tools/test_discord_wayland.sh --binary <client> STEP...` does the same on headless sway.
  [tools/README.md](tools/README.md#the-fake-discord-server) has the server's variables and the two Windows switches.

### Environment overrides

Overrides a user or an operator may want are not hooks: every build reads them, and they are documented here.

| Variable | System | Effect |
| --- | --- | --- |
| `MAICORD_LIBRARY_PATH` | Linux | Directories searched before the loader's own for every `dlopen`ed library (unusual installs, NixOS) |
| `MAICORD_VAAPI=off`, `MAICORD_VAAPI_DEVICE=/dev/dri/renderD128` | Linux | Disable VA-API hardware decode; pick the render node |
| `MAICORD_SECRET_SERVICE=off` | Linux | Keep the master key in the 0600 key file instead of the Secret Service |
| `MAICORD_NOTIFICATION_BACKEND=portal\|direct` | Linux | Force the notification route |
| `MAICORD_SPELLCHECK_BACKEND=hunspell\|enchant` | Linux | Force the spell-check library |
| `MAICORD_PLUGIN_DEV`, `MAICORD_EFFECT_DEV` | both | Developer Mode folders |
| `MAICORD_FPS_TITLE=1` | both | Frame rate in the window title |

### Linux integration runs

Tests that need an environment the suite cannot assume (a compositor, a sound server, namespaces) are registered disabled and
run through their script, which sets the opt-in variable at configure time (`MAICORD_WAYLAND_INTEGRATION`,
`MAICORD_PULSE_INTEGRATION`, `MAICORD_SPEECHD_INTEGRATION`, `MAICORD_FAKE_DISCORD_INTEGRATION`); status 77 is a skip.
`tools/test_linux_integration.sh` runs the CI set; [tools/README.md](tools/README.md#linux-qa-scripts) lists every script.

### CI

`.github/workflows/ci.yml` builds, audits and tests the client on Windows (`client`, `tests`) and Linux (`linux-client`,
`linux-tests`). The Linux jobs also run the ABI checks against the installed headers (`tools/check_*_abi.py`),
`packaging/check.sh` and `tools/test_linux_integration.sh` (reported, not failing, until the repository variable
`MAICORD_LINUX_INTEGRATION_BLOCKING` is `true`); `distros` runs the portable tests in glibc and musl distribution containers
(`tools/test_linux_distros.sh --list`).

- To reproduce a distribution in WSL: build with `tools/build_linux.sh --test --keep-intermediates`, install podman, run
  `sudo mount --make-rshared /`, then `tools/test_linux_distros.sh --build ~/mc-build debian-12`.
- A test that belongs in the portable set is added to `tools/linux_portable_tests.txt`. It must hold on any host: no
  installed-package assumptions, no `MAICORD_TEST_FONT_SET` or `MAICORD_TEST_SPELL_SET`, skips return 77.
- Validate workflow changes with `actionlint` and `zizmor`; keep actions pinned by SHA, permissions minimal and concurrency
  as it is.

## Build files

`CMakeLists.txt` includes the modules in `cmake/`:

| Module | Contents |
| --- | --- |
| `build_contract.cmake` | The compiler and linker contract, `maicord_library()`, `maicord_program()` |
| `packed_data.cmake`, `generated_sources.cmake` | Generated inputs: the packed UI data (`tools/pack_*.py`), the Windows shaders (`maicord_shader()`), icon and sounds, the Linux GLSL shader pack, Wayland bindings and desktop entry |
| `libraries.cmake`, `windows_libraries.cmake`, `linux_libraries.cmake` | The static libraries both clients link (a source prefixed `windows:` or `linux:` belongs to that system only) and those one system builds |
| `client.cmake`, `update_config.cmake`, `runtime_assets.cmake` | The client, its updater identity, the image audit, the source rules (`tools/check_source_rules.py`), and the licenses and fonts beside the executable |
| `size_policy.cmake` | The one list of size-compiled sources, Windows and Linux paths side by side |
| `tests.cmake`, `test_support.cmake`, `test_registry.cmake` | The test configuration, its helpers, and every test executable and CTest case |
| `perf_probe.cmake`, `integration.cmake`, `reference_tools.cmake`, `build_outputs.cmake` | Windows only: the perf probe, the live-network probes and the Opus conformance run, the DirectWrite oracles, the cleanup's list of build products |

- Source order in the library files is the archive member order: append new files instead of re-sorting the lists.
- **Size.** `size_policy.cmake` sets the optimisation level per source: on Linux a new `src/app` unit is size-compiled by
  default, and any other new `src` unit compiles `-Os` instead of `-O2` unless that file's list of hot code names it. A unit
  goes on that list only for a measurement (instructions and best-of-five wall time at both levels) that shows `-Os` more than
  2% slower; record the method and the numbers in the file.
- A generated input listed as a source (or `OBJECT_DEPENDS`) of `MaiCord` holds back every object of the client until its
  generator has run: link such inputs through an object library, like `maicord_client_resources`.

### Registering tests

```cmake
maicord_test(MaiCordFooTests ENTRY maicord_foo_tests_entry
    SOURCES tests/foo.cpp
    LIBS maicord_core kernel32
    TIMEOUT 10
    TEST maicord_foo)
```

- A test is one `maicord_test()` call in `cmake/test_registry.cmake`; more cases on the same executable use
  `maicord_add_tests()`. A test keeps one name, labels and sources everywhere it runs; a platform difference inside one is an
  `#if` in the test.
- Both systems register it unless `ONLY WINDOWS` or `ONLY LINUX` names one: a test of code one system builds, or a fixture
  that calls Windows API directly (BCrypt, Winsock, GDI, UI Automation, the registry, a real window). After `WINDOWS` or
  `LINUX` the arguments belong to that system until the next `WINDOWS`, `LINUX` or `BOTH` (or the test properties); the
  Windows import libraries (`kernel32`, `user32`, ...) in `LIBS` are dropped on Linux.
- Tests link non-LTO copies of the shipping libraries: list a library in `LIBS`, never in `DEPENDS`, which would build the
  shipping one too. On Windows `tests/` sources compile at `/Od` (code from `src/` stays `/O2`); a test that needs the
  optimizer is listed in `cmake/tests.cmake`.
- Small tests share an executable with `BUNDLE <name>`, which needs no `LIBS`: `maicord_test_bundle()` at the top of
  `test_registry.cmake` declares the executable's libraries and include directories (add what a new member needs there). Each
  case runs its own entry symbol (`MAICORD_TEST_ENTRY`), so a bundled test must not define symbols another member or library
  defines. `STANDALONE` also builds the test's own executable on Windows, for the offline tools that run it directly.
- Application and UI fixtures that need no Win32 window run on Linux over the headless window
  (`maicord_linux_app_test_libs`); `tests/support/win32_test_compat.hpp` supplies the few Win32 file, console and process
  calls shared tests make, for tests only.
- Tests live in `tests/<module>/<folder>/`, mirroring the code under test, with shared helpers in `tests/support/` and data in
  `tests/fixtures/`. A test or probe too large for one file is split beside itself into `<stem>_<part>.cpp` files that share a
  private `<stem>_internal.hpp`; the core suite's cases live in the `core_*.cpp` parts listed in
  `tests/core_test_cases.inc`, with the expectation sink in `tests/test_main.cpp`.

## Project layout

```
include/maicord/   public headers; each module's folders mirror its src/ folders (sdk/: the plugin API, sys/: the Win32
                   and Linux system declarations, os/ and gpu/: the per-system seams)
src/app/           the client by feature: shell (window, input, startup), chat, voice, account, guild, media, settings,
                   gateway, net, plugins, store, packages (theme, effect and link install), update, ..., and each
                   system's entry (windows, linux)
src/core/          freestanding runtime: memory, strings, JSON, containers, the log
src/model/         chat state: chat, guild, user, voice, notifications
src/protocol/      Discord wire formats: gateway, http, auth, encoding, messages, voice, ...
src/ui/            framework (core, layout, kit, widgets, controls, theme, text) and scenes by feature
src/render/        renderer: d3d11, text, media atlases, effects, shaders
src/gpu/           GPU backends: d3d11 (Windows), gles (Linux)
src/os/            Linux system layer behind include/maicord/os
src/text/          text engine: unicode, opentype, fonts, shaping, scripts, layout
src/media/         decoders: image, video, audio, containers, compression
src/platform/      per-system services (table below)
tests/             unit tests mirroring src/, shared support/, fixtures/, generated/ data and integration/ probes
samples/           example plugins and background effects
cmake/, config/    build modules; application manifest and resource scripts
tools/             build scripts, PE and ELF audits, QA scripts, perf harness, release scripts, the maicord-plugin tool
packaging/         Linux packages, AppImage, installer and repositories
third_party/       Opus, Lucide icons, Wayland protocol XML
assets/            fonts, icons, flags, sounds, the example theme and license notices
```

### Per-system seams

Shared code reaches the system only through these. Interfaces are under `include/maicord`; a linked directory has a README
with the area's design and test notes.

| Seam | Interface | Windows | Linux |
| --- | --- | --- | --- |
| Files, threads, waits, processes, text, paths, local time | `os/*.hpp` | inline forwarders, `os/windows/*.inl` | [`src/os/linux`](src/os/linux/README.md) |
| Memory, app paths, environment, images, resources, clock, libraries | `core/{virtual_memory,heap,data_path,module,resource,time,shared_library}.hpp` | | [`src/core/linux`](src/core/linux/README.md): runtime, libc and ELF contract |
| Sockets, I/O loop | `platform/net` | completion ports | io_uring, [`src/platform/net/linux`](src/platform/net/linux/README.md) |
| Window service | `platform/window/{window,event,key,pump}.hpp` | `src/platform/window/win32`; glue `src/app/windows` | [`src/platform/window/wayland`](src/platform/window/wayland/README.md) ([`third_party/wayland`](third_party/wayland/README.md)); glue `src/app/linux` |
| GPU | `gpu/api.hpp` | Direct3D 11, `src/gpu/d3d11` | OpenGL ES 3.1 over EGL, [`src/gpu/gles`](src/gpu/gles/README.md) |
| Client shaders | | HLSL, `src/render/shaders` | GLSL ES 3.10 twins packed with Python, [`src/render/shaders/gles`](src/render/shaders/gles/README.md); only plugin effects use DXC and SPIRV-Cross |
| Renderer, effects, Lottie geometry | [`src/render`](src/render/README.md), shared | | |
| Text input | `ui/text/secure_text_store.hpp` | TSF | text-input-v3, `src/ui/text/linux` |
| Audio, speech | `platform/audio` | WASAPI, SAPI | [`src/platform/audio`](src/platform/audio/README.md): `pulse`, `speechd`, in-house AAC |
| Video decode | `platform/decode/gpu_stream_decoder.hpp` | DXVA, Media Foundation | [`src/platform/decode`](src/platform/decode/README.md): `vaapi`, `software`, `linux` |
| Go Live encode | `platform/encode/gpu_stream_encoder.hpp` | Media Foundation hardware transforms | VA-API, [`src/platform/encode`](src/platform/encode/README.md) |
| Go Live capture | `platform/capture/screen_capture.hpp`, `platform/audio/stream_audio.hpp` | Windows.Graphics.Capture, DXGI Desktop Duplication, WASAPI process loopback | ScreenCast portal and PipeWire, [`src/platform/capture`](src/platform/capture/README.md) |
| Crypto, TLS | `platform/crypto`, `platform/net` | CNG, SChannel | system OpenSSL 3 or 4, `src/platform/crypto/openssl` |
| Desktop: shell open, notifications, spell checker, security keys | `platform/desktop` | | [`src/platform/desktop`](src/platform/desktop/README.md) over the in-house D-Bus client, [`src/platform/dbus`](src/platform/dbus/README.md) |
| Accessibility | | UI Automation, `src/platform/uia` | AT-SPI, [`src/platform/atspi`](src/platform/atspi/README.md) |
| System fonts | `platform/font` | GDI | fontconfig, [`src/platform/font`](src/platform/font/README.md) |
| Time zones, dates | `os/local_time.hpp` | | [`src/platform/time`](src/platform/time/README.md) |
| Plugin loader, catalog | [`src/app/plugins`](src/app/plugins/README.md) | PE | ELF |
| Client entry, identity, updater, OS text | `src/app/shell`, `src/app/update` | `src/app/windows` | [`src/app/linux`](src/app/linux/README.md), `src/app/update/linux` |
| Packaging, release | | [`tools/release`](tools/release/README.md) | [`packaging`](packaging/README.md) |

- A Windows forwarder must compile to the same code as the Win32 call it replaces.
- Shared code never calls `VirtualAlloc` or `HeapAlloc`: page-granular memory is `pages_*`, heap blocks are
  `heap_allocate`/`heap_free`. A worker's entry is `u32 f(void*)`, never `DWORD WINAPI`.
- A failed `os/` call leaves a Win32 code in `thread_last_error()`; `network_last_error()` keeps the Winsock values. File
  sharing is an advisory `flock` on Linux: a test for both systems asserts sharing under `#if defined(_WIN32)`.

## Themes, effects and plugins

[README.md](README.md#themes-effects-and-plugins) says what each kind is. Formats and tools:

- **Themes**: [`assets/themes/README.md`](assets/themes/README.md).
- **Background effects**: [`samples/effects/README.md`](samples/effects/README.md); the runtime is in
  [`src/render/README.md`](src/render/README.md#background-effects).
- **Plugins**: the SDK in [`include/maicord/sdk/README.md`](include/maicord/sdk/README.md), examples in
  [`samples/plugins`](samples/plugins), the loader in [`src/app/plugins/README.md`](src/app/plugins/README.md).
- [`tools/maicord-plugin`](tools/maicord-plugin) creates, builds, checks and packs plugins and builds effects (Python 3.9+;
  no Visual Studio on Windows, clang and ld.lld on Linux). The catalog is the
  [MaiCord-plugins](https://github.com/maj113/MaiCord-plugins) repository; its index is described in
  [`tools/release/README.md`](tools/release/README.md#plugin-catalog).

## Code guidelines

[AGENTS.md](AGENTS.md#code-rules) states every rule in a line; these need more.

- **System declarations.** Everything (maiopus included) compiles with `-nostdinc` and without unwind tables. Intrinsics go
  in `include/maicord/sys/simd.hpp` as clang defines them; only the reference oracles and the Opus conformance run use the SDK
  headers (`MAICORD_SDK_INCLUDES`). New Win32 imports need a reason.
- **No unwind tables.** Frame-based exception dispatch stops at the first MaiCord frame, so the tests report crashes from a
  vectored handler and crash-dump stacks through MaiCord code are heuristic. Only the perf probe, which walks stacks
  in-process, configures with `MAICORD_UNWIND_TABLES`.
- **Structs default to zero without initializers.** A struct with any default member initializer, even `= {}`, makes each
  `T x{}` store its fields one by one; without them it is a single fill. Leave zero-valued members bare and value-initialise
  (`T x{}`, `T{}`), and encode a nonzero default so that zero is the default (`hidden` rather than `visible = true`, a
  minus-one count); `T x;` leaves members unset.
- **One implementation means no function table.** A table of function pointers, a hook or a callback struct with one shipped
  implementation costs an indirect call, a load and a null check per use and blocks inlining: call the implementation by
  name. Seams with several implementations (text input targets, upload sources, settings pages, video sinks, the Linux
  decoder tiers) or one per system stay tables, as do the `dlopen`/`GetProcAddress` tables, COM vtables and the plugin ABI.
- **Tests fake at link time.** A test defines the function it fakes and the link takes that definition over the library's. A
  fake replaces a whole source file, so a replaceable function sits in a file of its own and the test lists its fake in
  `SOURCES` (`tests/support/voice_log_sink.cpp` for `src/app/voice/log_record.cpp`). A test that links application code lists
  `tests/support/account_module_fake.cpp` for the account module (`account_module_*`, `app/account/account_module_api.hpp`):
  it answers as the module does without an instance unless the test sets the entry's hook
  (`g_account_fake.message_ack = fake`; restore it when the case ends). A test of the module itself links the module.
- **Window events.** Shared app and UI code never sees a window message or an `HWND`: the backend hands `WindowEvent`s
  (`platform/window/event.hpp`) to one sink, and the app acts on the window through the `window_*` calls of
  `platform/window/window.hpp`. Pointer, wheel, cursor and context-menu events reach handlers in content pixels; handlers use
  the predicates of `src/app/shell/window_events.hpp` (`input_press`, `input_release`, `input_double`, ...) and the DIP
  conversions of `input.hpp`. What stays Windows-only (UI Automation, tray, single instance) is a function of
  `src/app/windows/platform_window.cpp`, not a case in the dispatcher. Handler tests drive the headless backend through
  `tests/support/window_fixture.hpp` (`maicord_window_null` before `maicord_platform` in `LIBS`); only tests of the Windows
  backend itself and the few that need real window state or a swap chain create windows.
- **GPU.** Shared code never touches `ID3D11*` or `D3D11_*`; the Windows decoders, Windows-only tests and tools, and the DXGI
  error codes `account_media.cpp` maps are the exceptions. A new GPU operation is declared in
  `include/maicord/gpu/api.hpp`, defined inline in `gpu/d3d11/api.inl` and out of line in `src/gpu/gles`; the Windows wrapper
  must compile to the call it replaces (compare per object with the previous commit, not whole executables: LTO moves code).
- **Shaders.** [The twins' README](src/render/shaders/gles/README.md#keeping-a-twin-in-step) has the steps for changing an
  HLSL shader with its GLSL twin. A change to a shader or to the ui quads, glyph or media scenes renews the PNGs under
  `tests/gpu/golden` from Direct3D 11 with `tools/update_gpu_goldens.py`; the Linux renders must match them within the
  tolerances of the scene headers (`tests/gpu/*_scene.hpp`, `png_golden.hpp`).
- **Constant buffers.** An HLSL cbuffer keeps the layout its C++ struct has under HLSL packing, and Linux reads the same
  buffer as a std140 uniform block (a `float3` or `uint2` that straddles a register, or a `packoffset` gap, differs). Where
  the two differ, name the bytes with scalars and the gaps with `_unused_` members inside `#if defined(__spirv__)` (the
  spelling the GLSL twin's block copies and plugin effects compile with); `tools/check_cbuffer_layout.py` (every build)
  rejects a divergent cbuffer or a twin block whose members are elsewhere.
- **Logging.** One event is one line: build it with `LogEvent` (`core/log_event.hpp`) and `write` it once, never one
  `log_u64` per value, and keep user names, paths and account ids out of it. The log is `maicord.log` in
  `%LOCALAPPDATA%\MaiCord\logs` or `$XDG_STATE_HOME/maicord/logs` (at 16 MiB it moves to `maicord.previous.log`);
  `core/log.hpp` documents the line format, `src/app/shell/startup_record.hpp` each session's `[start]` record and
  `memory_summary.hpp` the `[memory] summary` line.
- **Source layout.** Each module (`app`, `ui`, `render`, `gpu`, `protocol`, `model`, `text`, `media`, `platform`) is split
  into topic folders, and `include/maicord/<module>/<folder>/` mirrors `src/<module>/<folder>/`. Put a new file in the folder
  of its feature; a header private to a module stays beside its sources. File names drop a prefix the folder already states
  (`src/app/chat/messages/send.cpp`, not `application_message_send.cpp`) and, in `src/app`, the `application_` prefix of the
  client-side glue, unless the shorter name would collide in the folder; `account_` (the account module's side of a feature)
  is kept. Symbol names are unaffected.
- Public headers in `include/maicord/` document ownership and failure behavior: keep those comments accurate. Format with the
  repository `.clang-format` and match the file you are editing.

## Pull requests

- Keep each PR to one topic and use imperative commit subjects, for example `ui: fix member list scroll clamp`. Labels are
  added from the changed paths.
- CI must pass ([CI](#ci)).
- Describe what you checked by hand. Mention any EXE, DLL or Linux image size change, and back performance changes with
  before/after numbers from [`tools/perf`](tools/perf/README.md). Include screenshots for UI changes, with personal data and
  server content redacted.
- Don't edit `.github/workflows/release.yml` and the signing tools in the same PR as unrelated changes.

## Licensing

MaiCord is licensed under the [Mozilla Public License 2.0](LICENSE). Contributions are accepted under the same license.
Third-party code must come with its license under `third_party/` or `assets/licenses/`.
