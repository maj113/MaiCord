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

The Linux port builds the client (`MaiCord`), the libraries behind it and their tests: one freestanding PIE with its own `_start`,
raw syscalls and no CRT, needing only libc, libdl and libpthread (generated stubs pin the oldest symbol versions), everything else
opened with `dlopen`. The same bytes run on glibc and, with the interpreter changed by `tools/retarget_libc.py --libc musl`, on musl
(`tools/audit_elf.py [--libc musl]` audits either form). The target is Linux 6.1 or newer; the code uses the 6.1 kernel interface
unconditionally, with no version probes and no fallbacks for older kernels, and the network loop needs io_uring. Optional user-space
libraries (VA-API drivers, libfido2, speech-dispatcher, libpulse, ...) degrade to "unavailable".

**Build.** `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build_linux.ps1 -Test` (or `tools/build_linux.sh --test`
inside WSL) rsyncs the tree to ext4, builds with clang and ld.lld 23 into `~/mc-build`, runs `tools/audit_elf.py` on the images and
the Linux tests, and empties the build directory afterwards but for the client and its runtime assets (`--keep` keeps it all). The
script's header documents the layout, the variables (`MAICORD_LINUX_SOURCE`, `MAICORD_LINUX_BUILD`) and every option;
`tools/install_linux_toolchain.sh` installs the toolchain and test packages on Debian and Ubuntu.

**A build with tests is not a shipping build.** `--test` (`-DMAICORD_BUILD_TESTS=ON`) defines `MAICORD_TEST_HOOKS`, which turns on the
environment switches that only tests use (`include/maicord/core/test_hook.hpp`: `MAICORD_GLES_*` driver limits, the `MAICORD_*_LIBRARY` sonames,
`MAICORD_WAYLAND_NO_FRACTIONAL`, `MAICORD_INSTANCE_BACKEND`, ...). Such an image carries a marker string; `tools/audit_elf.py
--no-test-hooks` (run by `tools/build_linux.sh` without `--test` and by `packaging/build_tarball.py`) and `tools/audit_pe.ps1
-ForbidTestHooks` (run by `tools/build.ps1`) reject it. Anything shipped, packaged or measured for size comes from a build without
`--test`. A new test-only knob calls `test_hook()`, never `environment_get()`.

**Environment overrides for users and operators** (documented, shipped):

| Variable | Effect |
| --- | --- |
| `MAICORD_LIBRARY_PATH` | Directories searched before the loader's own for every `dlopen`ed library (unusual installs, NixOS). |
| `MAICORD_VAAPI=off` / `MAICORD_VAAPI_DEVICE=/dev/dri/renderD128` | Disable VA-API hardware decode / pick the render node. |
| `MAICORD_SECRET_SERVICE=off` | Keep the master key in the 0600 key file instead of the Secret Service. |
| `MAICORD_NOTIFICATION_BACKEND=portal\|direct` | Force the notification route. |
| `MAICORD_SPELLCHECK_BACKEND=hunspell\|enchant` | Force the spell-check library. |
| `MAICORD_WSL_GPU=0` | On WSL keep Mesa's default renderer instead of `GALLIUM_DRIVER=d3d12`. |
| `MAICORD_PLUGIN_DEV`, `MAICORD_EFFECT_DEV` | Developer Mode folders (both systems). |
| `MAICORD_FPS_TITLE=1` | Frame rate in the window title (both systems). |

**Size.** The Linux client is held to the Windows client's size (6,500,000 bytes stripped) without slowing hot paths. It follows
`cmake/size_policy.cmake` (the one list of size-compiled sources, Windows and Linux paths side by side: a new `src/app` unit is
size-compiled by default on Linux, and any other new `src` unit compiles `-Os` instead of `-O2` unless that file's list of hot code
names it; a unit goes on that list only for a measurement, instructions and best-of-five wall time at both levels, that shows `-Os`
more than 2% slower, whose method and numbers the file records), links with the machine outliner, `--icf=all` and `--gc-sections`,
packs its relative relocations as RELR under the Android tags (no glibc knows them, so `src/core/linux/start.cpp` applies them) and is
linked `--strip-all`. `tools/build_linux.sh --symbols --keep` keeps the symbol table for `llvm-nm -S --size-sort`;
`tools/linux_memory_report.py PID` splits a running client's resident memory by mapping (at the login screen the program is about 7 MB;
the rest is Mesa's). The GLES backend returns freed driver memory after it builds a shader (`heap_trim()`).
`tools/audit_linux_reachability.sh` links the client without LTO and lists the code no path from `_start` reaches, by source file, to
find a feature that a stub or an `#if` cut off on Linux. Report a size change in either client.

**Platform layers.** Platform code lives in `src/core/linux` and `include/maicord/sys/linux`; shared headers choose between the
systems with `#if defined(_WIN32)` and the Windows branch stays the code it was (`python tools/compare_pe.py OLD.exe NEW.exe` compares
two builds section by section). Shared code uses the portable wrappers in `include/maicord/core` (`intrinsics.hpp`,
`virtual_memory.hpp`, `interlocked.hpp`). The Win32 value vocabulary stays in shared code (`Interlocked*`, SRW locks, `LONG`/`DWORD`/
`HANDLE`, `WAIT_*`, `ERROR_*`, `SecureZeroMemory`); the `win32_*.hpp` headers give Linux its twins from `include/maicord/os/linux`.
Operations that mean different things on the two systems have neutral names in `include/maicord/os`: events, waitable timers and waits
(`wait.hpp`: `Waitable`, `wait_any`, `wait_many`), threads, sleep and the processor count (`thread.hpp`), files (`file.hpp`), paths
(`path.hpp`: `path_root`, `path_leaf_start`, `path_plain`), processes, text, local time and date formats. They are inline forwarders to
the Win32 call on Windows (`os/windows/*.inl`) and out-of-line bodies in `src/os/linux`; a forwarder must compile to the same code.
Page-granular memory is `pages_*` (`core/virtual_memory.hpp`) and heap blocks `heap_allocate`/`heap_free` (`core/heap.hpp`), never
`VirtualAlloc` and `HeapAlloc` in shared code. The millisecond tick is `CLOCK_BOOTTIME` and `clock_now` is `CLOCK_MONOTONIC`.

**Files and sockets.** `include/maicord/os/file.hpp` is the file seam: `file_open` with a `FileOpen` (access, creation, share, flags),
reads and writes at the position or at an offset, `file_replace` for atomic replacement, `file_stat`, `dir_open`/`dir_next`,
`file_map`, `dir_watch_*`, `file_duplicate_sealed`; failures leave a Win32 code in `thread_last_error()`. On Linux sharing is an advisory
`flock` (taken only when the opener denies write sharing; a test written for both systems states what it asserts about sharing under
`#if defined(_WIN32)`), files are created `0600`, and a directory watch is inotify with a short polling scan on filesystems that do not
report changes (9p, NFS, FUSE, CIFS). Caches and logs live under `cache_path` and `state_path` (`$XDG_CACHE_HOME`/`$XDG_STATE_HOME`,
`data_path` on Windows); each Linux test gets private ones. Consumers pass a `NetworkAddress` to the net layer and read errors with
`network_last_error()`, which keeps the Winsock values on both systems; `include/maicord/platform/net/socket.hpp` holds the few places
that still need a socket of their own. The Linux net layer (`src/platform/net/linux`, see its README) is the IOCP model on io_uring
(Linux 6.1; one ring per loop; no other backend), with name resolution on a small getaddrinfo pool; without io_uring (`kernel.io_uring_disabled`,
a seccomp filter such as Docker's or Podman's default profile) the client exits at startup naming the cause. `maicord_network_loop` tests it on
local sockets and `maicord_tls_stream_smoke` runs `tls_stream` over it against a local `openssl s_server`. A worker's entry is
`u32 f(void*)`, never `DWORD WINAPI`.

**GPU.** The backend is chosen at compile time behind `include/maicord/gpu/api.hpp` (D3D11-shaped free functions over the `Gpu*` handles of
`gpu.hpp`). On Windows the same functions are the Direct3D 11 backend: those marked `MAICORD_GPU_INLINE` are force-inlined one-call
wrappers in `include/maicord/gpu/d3d11/api.inl` (the `Gpu*` handles alias the `ID3D11*` interfaces, `GpuStatus` is `HRESULT`), and
device, surface, shader and video-frame code that is not a single call lives in `src/gpu/d3d11`. Shared code (`src/render`, `src/app`,
`src/ui`) calls `gpu_*` only, never `ID3D11*` methods or `D3D11_*` descriptors; the Windows decoders, Windows-only tests and tools, and
the DXGI error codes `account_media.cpp` maps are the documented exceptions. A new GPU operation is declared in `api.hpp`, defined inline
in `api.inl` and out of line in `src/gpu/gles`; a wrapper must compile to the call it replaces (compare per object with the previous
commit, not whole executables, since LTO moves code). The enumerators of `GpuFormat`, `GpuUsage`, `GpuBind`, `GpuBlendFactor`,
`GpuFilter` and `GpuAddress` carry Direct3D 11's values, buffer views carry their element count, and resizing a texture goes through
`gpu_texture_info` and `gpu_texture_create_from_info`. On Linux the backend is OpenGL ES 3.1 over EGL, both opened with `dlopen`
(`src/gpu/gles`, tests `tests/gpu`; see its README). Shaders stay HLSL: `tools/build_gles_shader_pack.py` turns the packed shaders of
`cmake/generated_sources.cmake` into a deflate-packed GLSL ES 3.10 pack with DXC and SPIRV-Cross, pinned in
`tools/linux_shader_toolchain.json` and built or fetched into `~/.cache/maicord-toolchain` (`MAICORD_TOOLCHAIN_CACHE`) by
`tools/fetch_linux_shader_toolchain.py`. A change to a shader or to the ui quads, glyph or media scenes renews the PNGs under
`tests/gpu/golden` from Direct3D 11 with `tools/update_gpu_goldens.py`, and a change to a shader needs `tools/deflate_cache.txt` to carry the
new pack's stream (`MAICORD_UPDATE_DEFLATE_CACHE=1 python3 tools/build_gles_shader_pack.py`; CI is strict); the Linux renders must match
the goldens within the tolerances of the
scene headers (`tests/gpu/*_scene.hpp`, `png_golden.hpp`). HLSL constant buffers keep their HLSL layout and Linux reads them as std140
(see Code guidelines).

**Where the details are.** Each area keeps its current design and test notes in a README beside its code:

| Area | Notes |
| --- | --- |
| Wayland window, input, clipboard, IME | `src/platform/window/wayland/README.md`, `third_party/wayland/README.md` |
| GLES backend, EGL surface, shader pack | `src/gpu/gles/README.md` |
| Renderer, effects (`.mcfx` GLSL), Lottie geometry | `src/render/README.md` |
| Video (VA-API, software tier, MP4) | `src/platform/decode/README.md` |
| Audio (PulseAudio, speech-dispatcher, AAC) | `src/platform/audio/README.md` |
| D-Bus, portals, tray, single instance, secrets | `src/platform/dbus/README.md` |
| AT-SPI accessibility | `src/platform/atspi/README.md` |
| Spell checker, security keys, desktop integration | `src/platform/desktop/README.md` |
| System fonts, line boxes | `src/platform/font/README.md` |
| Time zones, dates | `src/platform/time/README.md` |
| Plugin loader, catalog | `src/app/plugins/README.md` |
| The client: entry, identity, updater, OS text | `src/app/linux/README.md` |
| Kernel I/O, runtime, libc stubs, musl | `src/os/linux/README.md`, `src/platform/net/linux/README.md`, `src/core/linux/README.md` |
| Packaging and release | `packaging/README.md` |

**Tests.** The Linux registry is `cmake/linux_test_registry.cmake` (and the `cmake/linux_*_tests.cmake` it includes); tests keep their
Windows names and sources, and a platform difference inside one is an `#if` in the test. Shared tests call a few Win32 file, console
and process functions that `tests/support/win32_test_compat.hpp` (with `linux_test_app.hpp` and `linux_test_process.hpp`) supplies on
Linux: they are test code only, used by about a hundred test sources that also build on Windows, so they stay until those tests use the
neutral `os/` seams. A fixture that calls other Windows API directly (BCrypt, Winsock, GDI, UI Automation, the registry, a real window) is
not registered on Linux. Application and UI fixtures that need no Win32 window run over `maicord_window_null`
(`cmake/linux_app_ui_tests.cmake`). Tests that need an environment the suite cannot assume are opt-in through an environment variable at
configure time (`MAICORD_WAYLAND_INTEGRATION`, `MAICORD_PULSE_INTEGRATION`, `MAICORD_SPEECHD_INTEGRATION`) and run through their script.

**Linux QA scripts.** All run on headless sway (or nested sway; WSLg's Weston has no screencopy) with private buses, profiles and
servers, and report instead of touching the user's session. `tools/test_linux_integration.sh` runs the CI set: the Wayland window tests
(`tools/test_wayland_window.sh`, `--scale`, `--integer`), the GPU surface (`test_gpu_surface.sh`), the client on its signed-out screen
(`test_client_wayland.sh`: screenshot, idle CPU, wakeups, memory), AT-SPI (`test_atspi.sh`), the D-Bus mocks (`test_dbus.sh`),
PulseAudio (`test_pulse_audio.sh`) and speech-dispatcher (`test_speechd.sh`). `tools/client_flow.py` drives the client through clicks,
typing (`wtype`), scroll, output scale and window commands with screenshots (`tools/wayland_pointer.py` is its virtual pointer); a
signed-in window that headless sway scans out directly stays unrepainted in grim until some input repaints the output, so `shot:` first
re-applies the output scale; never judge a screenshot from a bare `grim`. Signed-in screens need no account:
`tools/test_discord_wayland.sh --binary <client> [--site <site>] [--root] [--dbus] STEP...` runs the client against
`tools/fake_discord_server.py`, a fake Discord (REST and the user gateway in ETF with one zstd frame per message, over TLS in a private
user, network and mount namespace) serving one account, a guild with two channels, messages with markdown, emoji, reactions, an embed, a
picture, an MP4 and a file (`MAICORD_FAKE_MENTION_AFTER=N` adds a mention of the account, `MAICORD_FAKE_EXTRA_MESSAGES=FILE` more
messages). Sign in with any e-mail and password. `--site` serves a catalog site on the catalog host too (Plugins and Themes stores,
consent dialog, install; `--root` makes the namespace with sudo so the plugin loader's ownership check sees real root); `--dbus` starts the
desktop mocks and a swaybar hosting the tray and prints the notifications and portal calls (`desktop-calls.txt`). The fake answers only what
the client reads (an unknown request is 404 and logged), and its READY must stay faithful to what the client's decoders require (the
user's settings proto and the guild's `user_guild_settings` entry decide whether a mention raises a notification).
`tools/test_signin_wayland.sh --binary <client>` is the sign-in regression run (without and with `--bar`, a swaybar that reserves the top
23 pixels); `tools/test_voice_wayland.sh --binary <client>` joins the fake guild's stage (a voice channel needs DAVE, which the fake does
not serve) with `--audio` giving the client a private PulseAudio server; `tools/test_catalog_wayland.sh --binary <client> --site <site>
--expect index-4.json ...` runs the client against a catalog site built with the catalog scripts and a throwaway key
(`MAICORD_PLUGIN_CATALOG_PUBLIC_KEY=<hex> tools/build_linux.sh` in a fresh build directory makes the matching client).
`tools/test_dbus_real.sh` (real xdg-desktop-portal and gnome-keyring) and `tools/test_dbus_tray.sh` (swaybar's tray) need a graphical
session and stay manual. A client run that opens Settings and closes (`python3 tools/client_flow.py wait:4 click:1139,16 wait:1.5 close`)
must exit 0.

**CI.** Linux CI is the `linux` and `distros` jobs of `.github/workflows/ci.yml` on `ubuntu-24.04`: install the toolchain with
`tools/install_linux_toolchain.sh --integration`, build and test with `tools/build_linux.sh --test --keep --timeout-scale 3`, check the
`dlopen` tables and the libc pins against the installed headers (`tools/check_{libc,openssl,pulse,speechd,fido2,video}_abi.py`), run
`packaging/check.sh`, run `tools/test_linux_integration.sh` (reported, not failing, until the repository variable
`MAICORD_LINUX_INTEGRATION_BLOCKING` is `true`) and pack the portable cases for the `distros` job, which runs them in Ubuntu 22.04,
Debian 12, Fedora, Arch Linux, NixOS, Alpine, Void (musl) and Chimera containers (`tools/test_linux_distros.sh`; its header lists each
distribution's packages; containers run with `--security-opt seccomp=unconfined` because the runtime needs io_uring). The shader toolchain
cache is keyed by the pins, the tool's source and the compiler. The tarball for the preview feed is built in a second build without tests.
To reproduce a distribution locally in WSL: build with `tools/build_linux.sh --test --keep`, install podman, run
`sudo mount --make-rshared /`, then `tools/test_linux_distros.sh --build ~/mc-build debian-12`. A test that belongs in the portable set
is added to `tools/linux_portable_tests.txt` (it must hold on any host: no installed-package assumptions, no `MAICORD_TEST_FONT_SET`/
`MAICORD_TEST_SPELL_SET`, skips return 77). The `dlopen` ABI checkers compare each library's function, constant and struct tables with the
installed `-dev` headers; a new `dlopen`ed function needs its table entry and no more (an unused entry is dead weight). Validate workflow
changes with `actionlint` and `zizmor`; keep actions pinned by SHA, permissions minimal and concurrency as it is.

**Packaging** lives in `packaging/` (its README is the reference, with the layout and the steps only the owner can take): the release
tarball (`build_tarball.py`), the AppImage, the one-line installer, nfpm `.deb`/`.rpm`, signed apt/dnf repositories, a PPA, COPR/OBS,
AUR, Alpine, Void, Gentoo, Flatpak and a Nix flake. `bash packaging/check.sh` (needs squashfs-tools, zsync, openssl, desktop-file-utils,
appstream; nfpm optional through `packaging/nfpm/install_nfpm.sh`) is the gate without a client. Shipped binaries are built without
`--test`. The `linux` job of `release.yml` attaches the tarball to the draft release when the repository variable `MAICORD_LINUX_RELEASES`
is `true`. Only the tarball install carries `.maicord-portable` and so may self-update.

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
