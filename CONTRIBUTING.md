# Contributing to MaiCord

MaiCord is a small, opinionated codebase. These notes keep changes easy to
review and the client small and fast.

## Before you start

- For anything larger than a bug fix, open an issue first so the approach can
  be agreed before you write the code. The issue forms cover bugs, missing
  Discord features and performance problems.
- Contributions must not include Discord-owned assets (images, sounds, fonts)
  or copied client code or bundles; see the [disclaimer](README.md#disclaimer).
- Research notes, captures and client bundles stay on your machine.
  `Analysis/` and `docs/` are git-ignored for this reason.

## Building and testing

Follow [Building from source](README.md#building-from-source). In short:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1         # client only
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1 -Test   # client + whole test suite
```

- A local build numbers itself from git as CI numbers master builds (see
  [tools/release/README.md](tools/release/README.md#client-configuration)), so
  the updater never offers it an older build, and nothing is installed until
  you choose **Restart to update**. Add `-NoUpdater` to test the local client
  against your normal profile without it checking for or downloading releases.
- There is one build directory, the sibling `..\MaiCord-build\Release`. Don't
  add other build trees, staging copies or archives. `build.ps1` removes test
  executables, profiles and logs after every run.
- The whole test suite must finish within **20 seconds** of execution. If a
  new test is slow, shrink its input but keep the coverage that matters.
  Assert outputs and counts, never wall-clock times, so tests stay stable on
  slower machines.
- If MaiCord is running from the build directory, add `-SkipClient` to a
  `-Test` run; otherwise the script refuses to rebuild over it.
- `.\tools\build.ps1 -Test -TestNames name1,name2`, run from a PowerShell
  prompt, runs only the named CTest cases while you iterate.
- MaiCord builds only with the LLVM fork at
  [maj113/llvm-pe](https://github.com/maj113/llvm-pe): clang-cl, lld-link and
  llvm-lib from `-LlvmBin`, else `MAICORD_LLVM_BIN`, else the release pinned
  in `tools/llvm_toolchain.json`, which `tools/resolve_llvm_toolchain.ps1`
  downloads once, checks against the pinned SHA-256 and keeps in
  `%LOCALAPPDATA%\MaiCord-toolchain\<tag>`. Another toolchain directory
  reconfigures the build directory from scratch. Visual Studio still supplies
  the Windows SDK, MASM (`ml64`), `rc`, `mt` and `dumpbin`.

### Linux

The Linux port builds the client (`MaiCord`), the libraries behind it and their tests. Run
`powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build_linux.ps1 -Test` (or `tools/build_linux.sh
--test` inside WSL): the script rsyncs the tree to ext4, builds with clang and ld.lld 23 into `~/mc-build`, runs
`tools/audit_elf.py` on the images and the Linux tests, and empties the build directory afterwards but for the client
and its runtime assets (`--keep` / `-KeepIntermediates` keeps it all). Its header documents the layout and decisions. Platform code lives in
`src/core/linux` and `include/maicord/sys/linux`; shared headers choose between the two with `#if defined(_WIN32)`
and the Windows branch stays byte-for-byte the code it was. Shared code uses the portable wrappers in
`include/maicord/core` (`intrinsics.hpp`, `virtual_memory.hpp`, `interlocked.hpp`) instead of MSVC intrinsics.

The Linux client is held to the Windows client's size (6,500,000 bytes stripped) without slowing hot paths: it follows
`cmake/size_policy.cmake` (a new `src/app` unit is size-compiled by default there, and any other new `src` unit compiles `-Os`
instead of `-O2` unless that file's list of hot code names it; a unit goes on that list only for a measurement, instructions
and best-of-five wall time at both levels, that shows `-Os` more than 2% slower, whose method and numbers the file records),
links with the machine outliner, `--icf=all` and `--gc-sections`, packs its relative relocations (`src/core/linux/start.cpp`
applies them) and ships
stripped. `tools/build_linux.sh --symbols --keep` keeps the symbol table for `llvm-nm -S --size-sort`;
`tools/linux_memory_report.py PID` breaks a running client's resident memory down by mapping (the program image and
its own pages against the GL driver stack, which is Mesa's, not ours). The GLES backend hands freed driver memory back to
the system after it builds a shader (`heap_trim()` in `include/maicord/core/heap.hpp`).

The Win32 value vocabulary stays in shared code (`Interlocked*`, SRW locks, `LONG`/`DWORD`/`HANDLE`, `WAIT_*`,
`ERROR_*`, `SecureZeroMemory`): the `win32_*.hpp` headers give Linux its twins from `include/maicord/os/linux`.
Operations that mean different things on the two systems have neutral names in `include/maicord/os`: events,
waitable timers and waits (`wait.hpp`: `Waitable`, `wait_any`, `wait_many`), threads, sleep and the processor count
(`thread.hpp`). They are inline forwarders to the Win32 call on Windows (`os/windows/*.inl`) and out-of-line bodies in
`src/os/linux` over futex, eventfd, timerfd, ppoll and pthread. Page-granular memory is `pages_*`
(`core/virtual_memory.hpp`) and heap blocks are `heap_allocate`/`heap_free` (`core/heap.hpp`), never `VirtualAlloc`
and `HeapAlloc` in shared code; a forwarder must compile to the same code (`python tools/compare_pe.py`), which is
why a few call sites keep a Windows `#if` where a wrapper would change a stack frame. The millisecond tick is
`CLOCK_BOOTTIME` and `clock_now` `CLOCK_MONOTONIC`. The Linux test registry (`cmake/linux_test_registry.cmake`)
lists the library tests the Linux build runs; shared tests call a few Win32 file and console functions that
`tests/support/win32_test_compat.hpp` supplies there.

The audio interfaces in `include/maicord/platform/audio` name no Windows type: borrowed events are `Waitable`, the
cross-thread wake of media and notification audio is a `Waker` posted with `window_post`, and the WASAPI test seam of
the notification worker is `include/maicord/platform/audio/wasapi`. The transport-free halves are shared files
(`media_audio_ring.cpp`, `notification_audio_mixer.cpp`, helpers in `audio_support.hpp`); `media_audio.cpp`,
`notification_audio.cpp`, `voice_audio_*.cpp` and `speech.cpp` stay the WASAPI and SAPI code (with
`wasapi_endpoint_watch.hpp`) and their Linux counterparts are in `src/platform/audio/pulse`, over libpulse opened with
`dlopen`. The change bits of endpoint watching are in `endpoint_watch.hpp`; each backend fills them its own way
(COM observers, a `pa_context_subscribe` subscription).

AAC decoding is behind `include/maicord/platform/decode/aac_decoder.hpp`: Media Foundation on Windows
(`src/platform/decode/aac_decoder.cpp`, unchanged), and on Linux the portable AAC-LC core in `src/media/audio`
(`aac_lc.cpp`, no libm, no allocation: the caller gives it memory) with the adapter in
`src/platform/decode/linux/aac_decoder.cpp`. A change to either is checked by `maicord_aac_lc` against the
libavcodec output stored in `tests/fixtures/aac`.

The GPU backend is chosen at compile time behind `include/maicord/gpu/api.hpp` (D3D11-shaped free functions over the
`Gpu*` handles of `gpu.hpp`). On Linux it is OpenGL ES 3.1 over EGL, both opened with dlopen
(`src/gpu/gles`, tests `tests/gpu`): a surfaceless Mesa context (or a pbuffer) per process, so the tests run on
llvmpipe anywhere and on a GPU with the driver's own selection (on WSL `GALLIUM_DRIVER=d3d12`). Shaders stay HLSL:
`tools/build_gles_shader_pack.py` turns the packed shaders of `cmake/generated_sources.cmake` into a deflate-packed
GLSL ES 3.10 pack with DXC and SPIRV-Cross, both pinned in `tools/linux_shader_toolchain.json` and built or fetched
into `~/.cache/maicord-toolchain` (`MAICORD_TOOLCHAIN_CACHE`) by `tools/fetch_linux_shader_toolchain.py`. A change to
a shader or to the ui quads, glyph or media scenes renews the PNGs under `tests/gpu/golden` from Direct3D 11 with
`tools/update_gpu_goldens.py`; the Linux renders must match them within the tolerances of the scene headers
(`tests/gpu/*_scene.hpp`, helpers in `png_golden.hpp`). A window surface on Linux is an EGL surface on the Wayland
window's `wl_egl_window` fed by an offscreen frame texture in Direct3D orientation, so shared renderer code keeps its
top-left origin, damage rectangles and previous-buffer logic: `tools/test_gpu_surface.sh` checks it on sway.

Background effects run through the shared pass executor (`maicord_effect_host`): an `.mcfx` of interface 4 holds the
effect's GLSL next to its DXBC (`tools/maicord-plugin/mcfx_tool.py effect build --glsl yes --dxbc-from <Windows build>`,
the translation in `tools/glsl_translate.py`), and Windows ignores the section. Changing an effect's HLSL, the SDK
header or a sample rebuilds `tests/gpu/effects/*.mcfx` (DXBC on Windows, then the GLSL on Linux) and renews the PNGs
with `tools/update_gpu_goldens.py --scene effects`; the Linux frames must stay within the PSNR of `tests/gpu/effects.cpp`.

The official catalog serves the Linux client an effect's interface 4 package in `index-4.json` and keeps the DXBC package in
`index-3.json` for Windows (`kCatalogIndexFile` picks the file; `tools/release/README.md`, "Plugin catalog"). MaiCord-plugins'
`scripts/build_universal.py` makes the package from the Windows build with `mcfx_tool.py effect build --glsl yes
--dxbc-from`, and `plugin_index.py` (`check_source`, `check_universal`) checks it. To check a catalog build on Linux, run
`maicord_catalog_effects` with `MAICORD_CATALOG_E2E_DIR=<site>` and `MAICORD_CATALOG_E2E_KEY=<public key hex>`.
To see the client's store against a local catalog, build the client with the test key
(`MAICORD_PLUGIN_CATALOG_PUBLIC_KEY=<hex> tools/build_linux.sh`, in a fresh build directory), build a site with the catalog
scripts and a throwaway key, and run `tools/test_catalog_wayland.sh --binary <client> --site <site> --expect index-4.json
--expect .mcfx wait:2 click:1139,16 ...` (the steps are `tools/client_flow.py`'s).
Signed-in screens need no account either: `tools/test_discord_wayland.sh --binary <client> [--site <site>] [--root] [--dbus]
STEP...` runs the client on headless sway against `tools/fake_discord_server.py`, a fake Discord (REST, and the user
gateway in ETF with one zstd frame per message, over TLS in a private network namespace) that serves one account, a guild
with two channels, messages with markdown, emoji, reactions, an embed, a picture, an MP4 and a file, and, with
`MAICORD_FAKE_MENTION_AFTER=N`, a message that mentions the account. Sign in with any e-mail and password (type and
`key:Return` steps). `--site` serves a catalog site on the catalog host too, so the Plugins and Themes stores, a plugin's
consent dialog and its install work (`--root` makes the namespace with sudo so the plugin loader's ownership check on the
directories above the plugins folder sees real root; `MAICORD_PLUGIN_DEV=<dir>` names a dev folder for Developer Mode);
`--dbus` starts a private session bus with the desktop mocks of `tests/platform/dbus/mock_services.py` and a swaybar that
hosts the tray, and prints the notifications and portal calls the client made (`desktop-calls.txt`). The fake server
answers only what the client reads (an unknown request is 404 and logged), and its READY must stay faithful to what the
client's decoders require: the user's settings proto and the guild's `user_guild_settings` entry, for instance, decide
whether a mention may raise a desktop notification. `tools/test_signin_wayland.sh --binary <client>` is the regression run
for the sign-in and the first paint after it (without and with `--bar`, a swaybar that reserves the top 23 pixels, so the
window is lower and shorter and clicks use output pixels); a screenshot step repaints headless sway's output first, since
it otherwise shows the frame from before the window's last commits when the window covers the output.
`tools/test_voice_wayland.sh --binary <client>` is the voice run: the fake server also runs the voice gateway and a UDP
socket, `--audio` gives the client a private PulseAudio server, and the run joins the stage (a voice channel needs DAVE) and
checks the handshake, a capture and a playback stream, decrypted Opus packets and the echo played back.

The renderer (`src/render`) builds for both systems (`cmake/linux_render.cmake` is the Linux side) and renders the same
scenes in `tests/render/scenes.cpp`: on an offscreen surface on Linux (`renderer_initialize` with a null window), over a
window on Windows. Shared renderer code takes the window only as a `GpuSurfaceDesc` (`renderer_window_surface(Window)`),
reads the clock through `core/time.hpp`, the executable and font files through `core/data_path.hpp` and `os/file.hpp`, and
local dates through `os/local_time.hpp`; it names no HWND, GetTickCount64 or GetClientRect. A new renderer test is written
against `gpu_*` and the renderer API (`gpu_surface_target_texture` and `gpu_readback` read a frame), not against
`ID3D11*`, so it runs on both; the `tests/integration` probes stay Windows-only.

On Windows the same functions are the Direct3D 11 backend: those marked `MAICORD_GPU_INLINE` are force-inlined one-call
wrappers in `include/maicord/gpu/d3d11/api.inl` (the `Gpu*` handles alias the `ID3D11*` interfaces, `GpuStatus` is
`HRESULT`), and device, surface, shader and video-frame code that is not a single call lives in `src/gpu/d3d11`
(library `maicord_gpu_d3d11`). Shared code (`src/render`, `src/app`, `src/ui`) calls `gpu_*` only, never `ID3D11*`
methods or `D3D11_*` descriptors; the Windows decoders (`src/platform/decode`), Windows-only tests and tools, and
the DXGI error codes `account_media.cpp` maps are the documented exceptions. A new GPU operation is declared in
`api.hpp`, defined inline in `api.inl` and out of line in `src/gpu/gles`; a wrapper must compile to the call it
replaces (compare per object with the previous commit, not whole executables, since LTO moves code). To keep that
true, the enumerators of `GpuFormat`, `GpuUsage`, `GpuBind`, `GpuBlendFactor`, `GpuFilter` and `GpuAddress` carry
Direct3D 11's values (the D3D11 backend casts, `api.inl` asserts it; other backends switch on the names), buffer
views carry their element count, and resizing a texture goes through `gpu_texture_info` and
`gpu_texture_create_from_info`, so no call site queries a description it did not before.

Fonts follow the same rule. The text catalog asks `SystemFontLocator` (GDI on Windows, byte-for-byte what it was;
`include/maicord/platform/font/system_font_locator.hpp` on Linux, fontconfig through `dlopen` with a directory-scan
fallback) and the platform defaults (`default_fonts.hpp`: interface, monospace and emoji families) for files; names
and ranges that differ per system sit under `#if defined(_WIN32)` in `src/ui/text/native_text_catalog*.inc` and
`native_system_fallback*.inc`. Colour bitmap glyphs (CBLC/CBDT) exist on Linux only and do not enter the Windows
build. Tests that observe Windows font files (`catalog_test`, `catalog_coverage_test`) are not registered on Linux;
the Linux font tests assert installed families only with `MAICORD_TEST_FONT_SET=ubuntu`. Regenerate the fallback
table with `tools/font_reference/linux_fallback.py` after a Unicode version change.

Lottie stickers lean on Direct2D as a CPU geometry engine (`src/render/effects/lottie_program.cpp`: path, ellipse and
rounded rectangle geometries, transforms, bounds, simplification, area, the four boolean combinations of mattes, masks
and merge paths, and stroke widening). Windows still calls Direct2D, unchanged. Elsewhere the compiler gets the same
method names from `include/maicord/os/linux/d2d1.hpp` (the Direct2D vocabulary over `include/maicord/render/effects/
vector_geometry.hpp`) and the engine behind them in `src/render/effects`: `vector_geometry.cpp` (figures of cubics and
lines in the arenas of one `Factory`, primitives, transforms, bounds, simplification), `vector_overlay.cpp` (flatten
to a tolerance, snap to an integer grid, split every crossing exactly, a sheared sweep for the winding numbers, rings
that keep the original curves between crossings: the four combinations and the area of a nonzero fill) and
`vector_stroke.cpp` (caps, miter, bevel and round joins with the miter clip, one outline per figure or separate pieces
where an inner join folds). Fills are nonzero everywhere; nothing is reference counted. A change to it is checked by
`maicord_lottie_geometry` (a fixed list of shapes, combinations and strokes, areas and coverage against
`tests/render/effects/golden/lottie_geometry.bin`, what Direct2D answered; Windows runs both and compares them) and
`maicord_lottie_program` (the corpus `tests/render/effects/corpus`, which `tools/generate_lottie_corpus.py` draws,
compiled and compared mask by mask with `golden/lottie_program.bin`). Renew a golden by running the test on Windows
with `MAICORD_LOTTIE_GEOMETRY_CAPTURE` or `MAICORD_LOTTIE_PROGRAM_CAPTURE` set to the file's path. The sticker files
the client shows are not ours to ship; to compare them anyway, list their paths in a file, capture their digests on
Windows with `MAICORD_LOTTIE_CORPUS_LIST` and `MAICORD_LOTTIE_CORPUS_CAPTURE`, and compare on Linux with
`MAICORD_LOTTIE_CORPUS_COMPARE`.

Time zones and dates: `src/platform/time/tzif.cpp` reads TZif files and POSIX rules as a pure function over bytes, and
`tests/platform/time/tzif_test.cpp` runs on both systems against zic's output and Python's zoneinfo
(`tools/tzif_fixtures.py` regenerates `tests/generated/tzif_*.bin` and `tests/fixtures/zoneinfo`; it needs zic and
Python 3.9). On Linux `os/local_time.hpp` finds the user's zone (`$TZ`, `$TZDIR`, `/etc/localtime`, `/etc/timezone`,
NixOS's `/etc/zoneinfo`) and also serves `host_iana_timezone` and the Linux log's time stamps; Windows keeps calling
the Win32 zone functions. `os/date_format.hpp` is the one place that formats a date or a time as text: forwarders to
`GetDateFormatEx`/`GetTimeFormatEx` on Windows, English names with the 12 or 24 hour cycle of `LC_ALL`/`LC_TIME`/`LANG`
on Linux (D19). The Linux spell checker is Hunspell (`libhunspell-1.7.so.0`) or Enchant 2 through `dlopen`; its
`spell_checker.hpp` interface is the Windows one, and `tests/fixtures/hunspell` is the dictionary the Linux cases use
through `$DICPATH`.

Linux CI runs in `.github/workflows/ci.yml` on `ubuntu-24.04`: the `linux` job installs the toolchain with
`tools/install_linux_toolchain.sh --integration`, builds and tests with `tools/build_linux.sh --test --keep --timeout-scale 3`
(which audits every image with `tools/audit_elf.py`), checks the `dlopen` tables against the installed headers
(`tools/check_openssl_abi.py`, `tools/check_pulse_abi.py`, `tools/check_speechd_abi.py`, `tools/check_fido2_abi.py`), runs the headless-sway, D-Bus,
PulseAudio and speech-dispatcher scripts
(reported, not failing, until the repository variable `MAICORD_LINUX_INTEGRATION_BLOCKING` is `true`) and packs the
portable cases; the `distros` job runs those cases in Ubuntu 22.04, Debian 12, Fedora, Arch Linux and NixOS containers
with `tools/test_linux_distros.sh`. To reproduce one distribution locally in WSL, build with `tools/build_linux.sh --test
--keep`, install podman, run `sudo mount --make-rshared /`, then `tools/test_linux_distros.sh --build ~/mc-build
debian-12`. A test that belongs in the portable set is added to `tools/linux_portable_tests.txt` (it must hold on any
host: no installed-package assumptions, no `MAICORD_TEST_FONT_SET`/`MAICORD_TEST_SPELL_SET`, skips return 77).

Linux packaging lives in `packaging/` (its README has the layout and the steps only the owner can take): the release
tarball (`packaging/build_tarball.py`, with a per-user `install.sh`/`uninstall.sh`), the AUR `maicord-bin`, `.deb` and
`.rpm` from nfpm, a Flatpak manifest and a Nix flake. `bash packaging/check.sh` checks the tarball, the desktop entry, the
AppStream metadata and an install/uninstall round trip without a client (CI runs it). The `linux` job of
`.github/workflows/release.yml` attaches the tarball to the draft release when the repository variable
`MAICORD_LINUX_RELEASES` is `true`.

The client identity follows the host platform's official desktop client (`include/maicord/protocol/http/client_user_agent.hpp`
holds each platform's constants; `src/app/account/linux/client_identity.cpp` reads the Linux host). It reads the
kernel release, `$LANGUAGE`/`$LC_ALL`/`$LC_MESSAGES`/`$LANG`, `$XDG_CURRENT_DESKTOP`, `$GDMSESSION`,
`$XDG_SESSION_TYPE`, the Flatpak/Snap/AppImage variables and `/etc/lsb-release` or `/etc/os-release`, into a
`LinuxClientEnvironment`, so a new property is a field there and a fixture host in `tests/app/account/client_identity.cpp`.

Files and sockets follow the same pattern. `include/maicord/os/file.hpp` is the file seam: `file_open` with a
`FileOpen` (access, creation, share, flags), reads and writes at the position or at an offset, `file_replace` for
atomic replacement, `file_stat` (kind, size, times as FILETIME, a volume and file identity), `dir_open`/`dir_next`,
`file_map` and `dir_watch_*`; failures leave a Win32 code in `thread_last_error()`. On Linux sharing is an advisory
`flock` (taken only when the opener denies write sharing), files are created `0600`, and a directory watch is inotify
with a short polling scan on filesystems that do not report changes (9p, NFS, FUSE, CIFS). Caches and logs live
under `cache_path` and `state_path` (`$XDG_CACHE_HOME`/`$XDG_STATE_HOME`, `data_path` on Windows). Consumers pass a
`NetworkAddress` (family, port, 16 address bytes, scope) to the net layer and read errors with
`network_last_error()`, which keeps the Winsock values (`WSAECONNREFUSED`, `WSAEWOULDBLOCK`, ...) on both systems;
`include/maicord/platform/net/socket.hpp` holds the few places that still need a socket of their own (loopback
listeners, the updater's blocking transport, test peers) and `SocketWatch` for waiting on one. On Linux
`NetworkLoop` is an epoll set with an eventfd wake and a ready list that gives completions the order IOCP would;
closing a connection or datagram yields one cancelled completion per pending operation, and `getaddrinfo` runs on a
two-thread pool. `maicord_network_loop` covers it on local sockets; `tests/platform/net/tls_stream_smoke.sh`
(`maicord_tls_stream_smoke`) drives an HTTPS GET, a download, a bad host name, a refused and a cancelled connection
through the real stack against `openssl s_server`.

The updater, the HTTP/REST and CDN transport and the Gateway client are compiled by `cmake/linux_app_libraries.cmake`
(`maicord_app_net`, `maicord_app_update`, `maicord_app_packages`). Their file work goes through the seam
(`file_close` reports a failed close; `file_duplicate_sealed` hands out a read-only second handle on an open file, the
updater's sealed stage; `file_set_last_write` sets a modification time for the fixtures that need a changed file),
their events, threads and clocks through `os/wait.hpp`, `os/thread.hpp` and `core/time.hpp`, and a worker's entry is
`u32 f(void*)`, never `DWORD WINAPI`. A path that policy checks (an update artifact or acceptance record, a download
target) is a drive or UNC path on Windows and a rooted one with no backslash on Linux; a caller asks
`include/maicord/os/path.hpp` (`path_root`, `path_leaf_start`, `path_plain`) instead of testing a drive letter. The suite
runs the updater pipeline fixtures (`maicord_update_pipeline`), the download owner (`maicord_https_file_download`) and the transport
fixtures (`maicord_transport`); a test written for both systems states what it asserts about sharing under
`#if defined(_WIN32)` because Linux sharing is advisory.

The application proper (`src/app` outside `shell/` and `windows/`, and `src/ui`) is `maicord_app_client` and `maicord_ui`
on Linux (`cmake/linux_app_client.cmake` globs `src/app`, so a new unit is built there by default; the UI Automation
server is replaced by the AT-SPI host in `src/app/accessibility/linux`, over `platform/atspi`, and the shared snapshot and
its actions, `accessibility_describe.cpp` and `accessibility_actions.cpp`, build on both systems, so a new describer or
action needs no AT-SPI work). Shared application code reaches the system only through the seams:
`os/process.hpp` (`process_launch_detached`), `os/text.hpp` (`utf16_to_utf8`, `utf8_to_utf16`), `os/thread.hpp`
(`thread_waitable` lets a wait set hold a thread; `thread_com_enter` brackets COM use), `os/local_time.hpp`,
`core/data_path.hpp` (`environment_variable`, `local_data_root`, `executable_path`), `core/module.hpp` (`module_image`),
`core/resource.hpp` (`program_resource`), `core/virtual_memory.hpp` (`system_memory_low`) and
`platform/desktop/shell_open.hpp` (`desktop_open_target`, `desktop_reveal_file`, `desktop_alert_beep`); where a window is
passed it is a `Window`, never an `HWND`. A part that only Windows has keeps its `#if defined(_WIN32)` in the file with
the Linux alternative next to it: the tray and single instance (`notifications/desktop.cpp`, over
`platform/dbus/tray.hpp` and `instance.hpp`), the taskbar badge (`taskbar_attention.cpp`, the launcher entry), toasts
(`platform/desktop/linux/notifications.cpp`), startup and protocol registration (`platform/desktop/linux/integration.cpp`:
an XDG autostart file, a scheme handler desktop file and the default in `mimeapps.list`), WebAuthn (unavailable), the
plugin loader (an ELF loader beside the PE one: `src/app/plugins/loader.cpp`) and game detection (`/proc/*/comm`). The application and UI fixtures that
need no Win32 window are registered in `cmake/linux_app_ui_tests.cmake` under their Windows names, over
`maicord_window_null`; the few Win32 calls they make are shimmed for tests only in `tests/support/linux_test_app.hpp`.
A fixture that calls other Windows API directly (BCrypt, Winsock, GDI, UI Automation, the registry, a real window) is
not registered on Linux, and the clipboard-bound ones wait for a clipboard in the headless backend. Plugins are shared
objects there (`maicord_plugin()` in `cmake/test_support.cmake` builds them into `plugins/` of the build directory; the loader
is the ELF half of `src/app/plugins/loader.cpp`): `cmake/linux_plugin_tests.cmake` runs `maicord_plugin_loader_elf` (the
loader's policy: a fixture per defect, links, modes, a renamed pin) and the plugin runtime tests of the Windows suite
(`maicord_plugin_abi`, `_binding`, `_network`, `_pipeline`, `_ui`, `_surfaces`, and `maicord_command_ui`) over the same
fixtures, plus the SDK's compile diagnostics (`maicord_plugin_compile_*`); `tools/maicord-plugin build --target linux`
makes the same file from a `plugin.cpp`.

The client itself is the `MaiCord` target of `cmake/linux_client.cmake` (the shell, `src/app/shell`, with the Linux entry
and window glue in `src/app/linux`, over `maicord_window_wayland`; the null backend is the tests'). `src/app/linux/entry.cpp`
turns `argv` into the one command line `desktop_launch_parse` reads (`platform/desktop/launch_line.hpp`, tested by
`maicord_linux_launch_line`), so `--startup`, `--url -- <link>`, a bare `discord://` argument and single-instance
forwarding are the Windows startup's. `tools/build_linux.sh` leaves `MaiCord`, `fonts/` and `licenses/` in `~/mc-build`
(the resources Windows takes from `.rc` are embedded: `core/linux/resource.cpp`, the icon from `tools/generate_app_icon.py`).
`tools/test_client_wayland.sh` runs it on headless sway (or nested, or the running compositor) with a private empty
profile, takes a `grim` screenshot and reports the idle CPU, wakeups and memory; `tools/client_flow.py` drives it through
clicks, typing (`wtype`), scroll, output scale and window commands with screenshots in between (`tools/wayland_pointer.py`
is the long-lived virtual pointer it uses). WSLg's Weston has no screencopy, so a screenshot there is the nested run.
`tools/test_atspi.sh` runs the client behind the real `at-spi-bus-launcher` and registry on a private session bus and reads
it as a screen reader does (`tests/platform/atspi/atspi_client.py`, python3-gi with gir1.2-atspi-2.0): accessibility is off
when the client starts and its tree is invisible, then on; roles, names, states, bounds and actions of the sign-in form, a
button's action, field text read and set, focus and text events. `--orca` runs Orca too (on a private Xvfb with
speech-dispatcher's dummy module) and checks what it says for the fields. The same checks of the mapping without a bus are
`maicord_atspi_map`.
The updater is on in a Linux build only for the tarball install: the build must be told that the release channels carry the
tarball (`MAICORD_LINUX_UPDATES_PUBLISHED`, from `MAICORD_LINUX_RELEASES=true` at configure time, as `release.yml`'s `linux`
job does), and `update_portable_gate` enables it for the install that has the `.maicord-portable` marker next to its real
executable (`update_portable_install`) and for no other. It reads the Linux manifests (`updates/p2/linux/`, signed by
`maicord_release.py linux`), stages the release tarball's program, `fonts/` and `licenses/` through
`src/app/update/linux/package.cpp`, swaps them in at exit keeping the old version in `MaiCord.old/` (`stage.cpp`) and relaunches
with `process_replace` (`execve`). `maicord_update_pipeline` replays all of that over a fake network with a throwaway key
(`tests/app/update/linux_package.cpp`); `MAICORD_UPDATE_TEST_TARBALL=<a tarball from packaging/build_tarball.py>` runs the same
over the producer's own file.
Text that names the operating system is chosen per system: `MAICORD_OS_TEXT("Windows didn't save this change.", "Couldn't
save this change.")` (`include/maicord/core/os_text.hpp`) compiles one wording, and the settings catalog keeps its Windows
data (`catalog_data.inc`, packed) with a Linux table applied once in `catalog.cpp`; a row Linux has nothing behind is listed
in `settings_binding_unavailable`. `maicord_linux_catalog_text` and `tools/check_linux_ui_text.py` (on the built image) fail
when shown text names Windows on Linux. The Linux entry blocks SIGTERM, SIGINT and SIGHUP before any thread exists and reads
them from a `signalfd` the main loop waits on (`src/app/linux/shutdown_signal.cpp`); the loop quits as the Quit command does
and the normal exit saves (`maicord_linux_quit_signal`; `tools/test_client_wayland.sh --terminate TERM|INT` on the client).
Because the compositor knows only the last `xdg_toplevel.set_min_size`, the loop sends the layout's minimum every turn
(`application_window_refresh_minimum_size`; `maicord_linux_window_minimum`). `tools/audit_linux_reachability.sh` is the dead
code audit: the client built without LTO and linked twice (`--no-gc-sections -Map`, `--print-gc-sections`), the code no path
from the entry reaches listed by source file, to find a feature that a stub or an `#if` cut off on Linux.
`tools/test_wayland_window.sh`'s drivers wait for the build before the tests (`MAICORD_DRIVER_WAIT`, default 1800 s) and find
their sway by its log and pid, not by the newest socket.
GPU handles are released with `gpu_release`, never `com_release` (a handle is a COM object on Windows only): the renderer
once released two buffers as COM objects, which crashed every Linux run that had opened Settings when the window closed
(`tests/render/scenes.cpp` creates both buffers and shuts down; `python3 tools/client_flow.py wait:4 click:1139,16 wait:1.5
close` is the same run on the client, and its exit status must be 0).

### The toolchain release

Local builds and CI (`.github/actions/setup-toolchain/action.yml`, used by
`ci.yml`, `release.yml` and `analysis.yml`) fetch the same pinned release of
the fork through `tools/resolve_llvm_toolchain.ps1`; CI puts its `bin` on
`PATH` and in `MAICORD_LLVM_BIN`, and caches the extracted toolchain with
`actions/cache` under the pinned tag and SHA-256, so a new pin fetches anew.

Each toolchain release of `maj113/llvm-pe` must provide:

- a tag `toolchain-YYYY.MM.DD`;
- the asset `llvm-pe-x86_64-windows.zip`, whose root is the toolchain
  directory: `bin\clang-cl.exe`, `bin\lld-link.exe` and `bin\llvm-lib.exe`
  (the build uses Visual Studio's `rc`, `ml64` and `mt`) and clang's core,
  x86 and Windows resource headers in `lib\clang\<major>\include`, which
  clang-cl finds relative to `bin`. The executables link the static MSVC
  runtime, so no redistributable is needed;
- the asset `llvm-pe-x86_64-windows.zip.sha256`, the archive's hex SHA-256.

To move to a new release, set `tag` and `sha256` in `tools/llvm_toolchain.json`
to the tag and the `.sha256` value.

### Build files

`CMakeLists.txt` includes the modules in `cmake/`:

| Module | Contents |
| --- | --- |
| `build_contract.cmake` | Compiler/linker contract, `maicord_library()`, `maicord_program()` |
| `generated_sources.cmake` | Packed UI data (including the emoji name pools and entry tables, `tools/pack_emoji_pools.py`, the settings catalog, `tools/pack_settings_catalog.py`, the permission grid and built-in games, `tools/pack_tables.py`, and cold text such as the built-in command index, `tools/pack_text_blobs.py`), shaders (`maicord_shader()`), icon, sounds |
| `libraries.cmake` | Shipping static libraries |
| `client.cmake` | `MaiCord.exe` with the account module, updater identity and runtime assets |
| `size_policy.cmake` | The cold-code size policy of both clients (`MaiCord.exe` and the Linux `MaiCord`): the sources compiled `-Oz` (the only code the machine outliner touches), the ones that also fold helpers (`/Ob2`, `-finline-functions`) and, on Linux only, the `-Os` tier for what is left at `-O2` that is not a byte or sample loop; one list names Windows and Linux files side by side |
| `tests.cmake`, `test_support.cmake` | The temporary test configuration and its helpers |
| `test_registry.cmake` | Every test executable and CTest case |
| `perf_probe.cmake`, `benchmarks.cmake`, `integration.cmake`, `reference_tools.cmake` | Opt-in tools |

A test is one `maicord_test()` call; more cases on the same executable use
`maicord_add_tests()`. Tests link non-LTO copies of the shipping libraries
automatically and compile their own `tests/` sources at `/Od` (code from `src/`
stays `/O2`); a test that needs the optimizer is listed in `cmake/tests.cmake`.
Small tests share an executable with `BUNDLE <name>` on their
`maicord_test()`, which needs no `LIBS`: the executable's libraries and include
directories are declared once by `maicord_test_bundle()` at the top of
`test_registry.cmake` (add what a new member needs there). Each case runs its own
entry symbol (`MAICORD_TEST_ENTRY`), so a bundled test must not define symbols
another member or library defines. `STANDALONE` also builds the test's own
executable (with its own `LIBS`), which the offline tools in `tools/` run
directly (no CTest case uses it).

In a `-Test` build the test targets' compiles wait for the client's (the stamp in
`maicord_order_tests_after_client()`): Ninja starts ready edges by the length of the
chain behind them and counts the client's one-thread LTO link as a single edge, so
with every test compile ready the link would start only after all of them. A
generated input listed as (or `OBJECT_DEPENDS` of) a source of `MaiCord` holds back
every object of the client until its generator has run; link such inputs through an
object library, like `maicord_client_resources`.

```cmake
maicord_test(MaiCordFooTests ENTRY maicord_foo_tests_entry
    SOURCES tests/foo.cpp
    LIBS maicord_core kernel32
    TIMEOUT 10
    TEST maicord_foo)
```

Source order in `libraries.cmake` is the archive member order, so append new
files instead of re-sorting the lists.

## Code guidelines

- For Discord API, authentication and Gateway work, use
  [Discord Userdoccers](https://docs.discord.food/) alongside Discord's official
  documentation when adding features and checking existing behavior. Cross-check
  uncertain details against local client research, and cover request metadata,
  session lifetimes, rate limits and challenge handling with regression tests.
  Its [MCP endpoint](https://docs.discord.food/mcp) provides `search_docs` and
  `fetch_page`; use the sitemap/navigation for complete page and section coverage.
- **Freestanding C++23.** Don't use the STL, the CRT, exceptions or RTTI. Use
  the types in `include/maicord/core`. New Win32 imports need a reason. Every
  build runs the PE audits in `tools/audit_pe.ps1`.
- **No SDK or compiler headers.** Targets (maiopus included) compile with
  `-nostdinc`, so `<Windows.h>`, the C library and the compiler's intrinsic
  headers are not found. Declare the SDK functions, types and constants you use
  in `include/maicord/sys` (or `core/win32_*.hpp`) exactly as the SDK does, and
  intrinsics in `include/maicord/sys/simd.hpp` as clang defines them; the
  conformance unit generated from the declarations fails the build on any
  difference. Only the reference oracles and opt-in benchmarks use the SDK
  headers (`MAICORD_SDK_INCLUDES`).
- **No unwind tables.** Every unit (maiopus included) compiles without
  `.pdata`/`.xdata`, so nothing may rely on unwinding MaiCord frames:
  no SEH, no unhandled-exception filters (frame-based dispatch stops at the
  first MaiCord frame; the tests report crashes from a vectored handler), and
  crash-dump stacks through MaiCord code are heuristic. Only the perf probe and
  `benchmark_gateway_pipeline.ps1 -Profile`, which walk stacks in-process,
  configure with `MAICORD_UNWIND_TABLES`.
- **Structs default to zero without initializers.** A struct with any default
  member initializer, even `= {}`, makes each `T x{}` store its fields one by
  one; without them it is a single fill. Leave zero-valued members bare and
  value-initialise (`T x{}`, `T{}`), and encode a nonzero default so that zero
  is the default (`hidden` rather than `visible = true`, a minus-one count)
  instead of initialising it; `T x;` leaves members unset.
- Format with the repository `.clang-format` and match the file you are editing.
- Mention any EXE/DLL size change in the PR description, and back performance
  changes with before/after numbers from [`tools/perf`](tools/perf/README.md).
- UI colors, radii and spacing come from theme tokens;
  `tools/check_theme_literals.py` (part of the suite) rejects new literals.
- HLSL constant buffers keep the layout their C++ struct has under HLSL packing, and the Linux build reads the same
  buffers as std140 uniform blocks (a `float3` or `uint2` that straddles a register, or a `packoffset` gap, differs).
  Wherever the two differ, name the bytes with scalars and the gaps with `_unused_` members inside
  `#if defined(__spirv__)`; `tools/check_cbuffer_layout.py` (part of the suite) rejects a divergent cbuffer.
- Public headers in `include/maicord/` document ownership and failure
  behavior; keep those comments accurate when behavior changes.
- **Source layout.** Each module (`app`, `ui`, `render`, `gpu`, `protocol`, `model`,
  `text`, `media`, `platform`) is split into topic folders, and
  `include/maicord/<module>/<folder>/` mirrors `src/<module>/<folder>/`. Put a
  new file in the folder of its feature; a header private to a module stays
  beside its sources. File names drop a prefix the folder already states
  (`src/app/chat/messages/send.cpp`, not `application_message_send.cpp`) and,
  in `src/app`, the `application_` prefix of the client-side glue, unless the
  shorter name would collide with another file in the folder; `account_` (the
  account module's side of a feature) is kept. Symbol names are unaffected.
- **Windows, input and tests.** Shared app and UI code never sees a window message: the backend turns every message
  into a `WindowEvent` (`platform/window/event.hpp`; `win32_translate` on Windows) and hands it to one sink, and the
  app acts on the window through the service in `platform/window/window.hpp` (`window_set_cursor`,
  `window_capture_pointer`, `window_key_down`, `window_focus`, ...), never through `HWND` calls. Pointer, wheel,
  cursor and context-menu events reach handlers in content pixels (the dispatcher takes the caption's rows off the
  client point once, `window_dispatch.cpp`); `app/shell/window_events.hpp` has the predicates handlers use
  (`input_press`, `input_release`, `input_double`, `input_pointer_step`, `input_side_button`, ...) and `input.hpp`
  the DIP conversions. What stays Windows-only (UI Automation, tray and single instance) is a hook of
  `src/app/windows/platform_window.cpp` (`Win32Hooks`; the non-client frame forwards to the portable caption controller
  through `Win32CaptionOps`), not a case in the dispatcher; the window state observer is the dispatcher's own. Handler
  tests build
  events with `tests/support/window_fixture.hpp` (`window_test::pointer_down(x, y)`, `key_down`, `wheel`, ...) and
  drive them against the headless backend (`window_test::open()`), which keeps capture, focus, key state and the
  cursor shape in memory and reports a lost capture synchronously, as Windows does
  (`tests/platform/window/win32_behaviour.cpp` captures that behaviour on a real window). A test that uses the
  headless backend lists `maicord_window_null` before `maicord_platform` in its `LIBS`, sets modifiers with
  `null_window_set_key` (`window_test::hold`, `null_window_release_keys`) and reads the cursor, focus, close requests
  and fatal messages with `null_window_probe()`; the handler runtimes (friends, forward, search, picker, video, ...)
  all do. Tests of the Windows backend itself (`win32_*`) create real windows; so do the UI probes that need a swap
  chain and the few handler tests that need real window state (attachments, pane resize, content navigation, the
  resize runtime, surface motion host); they read capture and focus through the service (`window_pointer_captured`,
  `window_focus`).
- Add or update tests in `tests/<module>/<folder>/`, mirroring the code under
  test, for every behavior change, and register them in
  `cmake/test_registry.cmake` (see [Build files](#build-files)). Helpers shared
  across folders live in `tests/support/`, data in `tests/fixtures/`.
  A test or probe too large for one file is split beside itself into
  `<stem>_<part>.cpp` files that share a private `<stem>_internal.hpp`; the
  core suite's own cases live in the `core_*.cpp` parts listed in
  `tests/core_test_cases.inc`, with the expectation sink in `tests/test_main.cpp`.

## Pull requests

- Keep each PR to one topic and use imperative commit subjects, for example
  `ui: fix member list scroll clamp`. Labels are added from the changed paths.
- CI must pass. It builds the Release client, runs the PE audits and runs the
  whole test suite on Windows.
- Describe what you checked by hand. Include screenshots for UI changes, with
  personal data and server content redacted.
- Don't edit `.github/workflows/release.yml` and the signing tools in the same
  PR as unrelated changes.

## Licensing

MaiCord is licensed under the [Mozilla Public License 2.0](LICENSE).
Contributions are accepted under the same license. Third-party code must come
with its license under `third_party/` or `assets/licenses/`.
