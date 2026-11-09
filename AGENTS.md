# Working on MaiCord

Guidance for coding agents and contributors. [CONTRIBUTING.md](CONTRIBUTING.md) has the same rules in more detail; subsystem design
notes are in the `README.md` beside each area's code (index in CONTRIBUTING.md, "Linux").

## Build and test

Both systems have one entry point with the same options (PowerShell `-TestNames` is `--test-names` in the shell script):

| Windows (`tools\build.ps1`) | Linux (`tools/build_linux.sh`) | |
| --- | --- | --- |
| `-Test` | `--test` | also build and run the test suite |
| `-TestNames a,b` | `--test-names a,b` | only these tests (with `-Test`) |
| `-SkipClient` | `--skip-client` | tests use the client already in the build directory instead of building it |
| `-KeepIntermediates` | `--keep-intermediates` | keep objects, ninja state and `compile_commands.json` (incremental builds) |
| `-SizeReport` | `--size-report` | print the client's sections and largest code (Linux: keeps its symbol table) |
| `-LlvmBin DIR` | `--llvm-bin DIR` | another LLVM bin directory (default `MAICORD_LLVM_BIN`, else the pinned release) |
| `-TestTimeoutScale N` | `--test-timeout-scale N` | multiply every test timeout (slow hosted runners) |

- The toolchain is the maj113/llvm-pe fork's release pinned in `tools/llvm_toolchain.json`, fetched once (Windows
  `%LOCALAPPDATA%\MaiCord-toolchain`, Linux `~/.cache/maicord-toolchain`). The contract adds the fork's code generation options
  (`maicord_llvm_options` and `/OPT:LLDBRANCHRELAX` in `cmake/build_contract.cmake`) only when the toolchain takes them, so another
  LLVM gets none of them instead of failing.
- Each run ends with the build directory holding only the client and its runtime assets; `-KeepIntermediates` keeps the rest, so later
  builds are incremental and `python tools/check_sources.py` can parse changed files and every unit including a changed header without
  building. Finish with a normal run to restore the clean directory.
- The client audits itself when it links (`tools/audit_image.py`: imports, debug data, initialized-data budget, ELF symbol versions).
  A build with tests is not a shipping build: it defines `MAICORD_TEST_HOOKS` (test-only environment switches through `test_hook()`,
  `include/maicord/core/test_hook.hpp`); ship, package and measure size only from a build without them (the audit's
  `--no-test-hooks` enforces it).
- The suite must finish within 20 seconds. Keep coverage when trimming test work. Tests assert outputs and counts (operations,
  allocations, bytes), never wall-clock times.

Windows:

- Build only with `tools/build.ps1`, from Windows PowerShell 5.1:
  `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1` builds the Release client.
- Needs the Windows SDK and the MSVC x64 toolset of the Visual Studio Build Tools (`.vsconfig`; the SDK's headers, import libraries,
  `rc`, `mt` and `fxc`, MSVC's C and C++ library headers for the units compiled against the SDK), with CMake and Ninja on `PATH`;
  `tools/enter_build_environment.ps1` finds them without the developer shell (MASM is the toolset's `ml64`).
- The only build directory is the sibling `..\MaiCord-build\Release`. Don't create other build trees, staging copies, QA directories or
  archives, and don't move unwanted output elsewhere: delete it.
- If `MaiCord.exe` is running from the build directory, add `-SkipClient` to a `-Test` run; the script won't overwrite a running client.
  Never close someone's running client for them.
- `-TestHooks` builds the client alone with `MAICORD_TEST_HOOKS` (a Windows client that `MAICORD_TEST_SERVER` and
  `MAICORD_TEST_CA_FILE` point at `tools/fake_discord_server.py`; CONTRIBUTING.md has the recipe). It replaces the shipping client
  in the build directory: ship, measure and compare only from a run without it.

Linux:

- Build only with `tools/build_linux.sh` (in WSL, or from Windows `wsl -d Ubuntu-Preview --cd <checkout> -- bash -l
  tools/build_linux.sh ...`): clang and ld.lld 24 of the pinned release (`MAICORD_LLVM_MAJOR` names the major of another LLVM, which
  builds without the fork's options), `-nostdinc` with the kernel and glibc interface in `include/maicord/sys/linux`. Build directory
  `~/mc-build` on ext4 (never under `/mnt`); `MAICORD_LINUX_SOURCE` and `MAICORD_LINUX_BUILD` select other directories (use your
  own when others share the machine). `tools/install_linux_toolchain.sh` installs the toolchain and the test packages.
- Code the Windows build compiles must not change for the Linux port; `python tools/compare_image.py OLD NEW` compares two builds
  (PE or ELF) section by section.
- Linux integration runs (headless sway, D-Bus, PulseAudio, AT-SPI, fake Discord) are `tools/test_*.sh`;
  `tools/test_linux_integration.sh` runs the CI set. CI is `.github/workflows/ci.yml` (Windows client and test shards, Linux job,
  distribution containers); validate workflow changes with `actionlint` and `zizmor`.

## Code rules

- For Discord API, authentication and Gateway work, consult [Discord Userdoccers](https://docs.discord.food/) for new features and to
  verify existing behavior, alongside Discord's official documentation. Cross-check uncertain details against the supplied client
  research; keep regression tests for request metadata, session lifetimes, rate limits and challenge handling. Its
  [MCP endpoint](https://docs.discord.food/mcp) provides `search_docs` and `fetch_page`; use the sitemap/navigation to enumerate all
  pages and sections.
- Freestanding C++23: no STL, CRT, exceptions or RTTI. Use `include/maicord/core`. Every link of either client runs
  `tools/audit_image.py` (PE: imports, debug data, mitigations, initialized-data budget; ELF: see Linux rules).
- Code compiles without unwind tables: no SEH, unhandled-exception filters or stack walks through MaiCord frames (see CONTRIBUTING.md).
- Targets compile with `-nostdinc`: Windows SDK, C library and compiler declarations (intrinsics included) come from
  `include/maicord/sys` and `include/maicord/core/win32_*.hpp`, declared exactly as the SDK and compiler declare them. Add what you use
  there; the generated conformance unit (`tools/generate_sys_conformance.py`) checks it against the SDK on every build. On Linux the
  kernel and libc interface is `include/maicord/sys/linux`.
- UI colors, radii and spacing come from theme tokens and scales (`include/maicord/ui/theme/tokens.inc`);
  `tools/check_theme_literals.py` fails on new literals.
- Never ship Discord-owned assets (icons, sounds, illustrations, fonts) or copied client code. Research material stays in the
  git-ignored `Analysis/` and `docs/` directories.
- Name things by what they do, not after Discord or reverse-engineering artifacts.
- Performance and size work needs before/after numbers: `tools/perf/run_perf.ps1` for CPU, wakeups, allocations and memory, and the
  EXE/DLL sizes from the build. Keep only changes that pay for themselves.
- Never log tokens, passwords, signed URLs or message content.

## Linux rules

- One freestanding PIE with its own `_start`, raw syscalls and no CRT, in two forms of the same bytes: glibc (`DT_NEEDED`
  `libc.so.6` alone, a generated stub pins symbol versions at most GLIBC_2.34: dlopen and pthread live in libc since glibc 2.34, the floor) and musl (`tools/retarget_libc.py`
  changes the interpreter); `tools/audit_image.py [--libc musl]` enforces it. A new libc import must also be defined by musl (else weak)
  and appear in the stubs, `ALLOWED_IMPORTS` and `tools/check_libc_abi.py`'s pins. Every other library is opened with `dlopen` through
  `core/shared_library.hpp`: absent means "unavailable", never a crash. A new `dlopen`ed function needs its table entry (and no unused
  ones) and passes `tools/check_<library>_abi.py` against the installed `-dev` headers.
- Kernel floor is Linux 6.1: use its syscalls and features unconditionally, no version probes, no alternate implementation for older
  kernels. Newer interfaces only where the win is real, naming the kernel they need. The network loop is io_uring only: a system
  without it (`kernel.io_uring_disabled`, a seccomp filter: Docker's and Podman's defaults) makes the client exit at startup naming the
  cause; container runs need `--security-opt seccomp=unconfined`. File-system gaps (RENAME_NOREPLACE) are not kernel versions.
- Environment switches: user/operator overrides are documented in CONTRIBUTING.md ("Environment overrides"); anything that exists for
  tests goes through `test_hook()`. Test-only code does not live in `src`.
- The Linux client is held to the Windows client's size: both at most 6,500,000 bytes (stripped); report a change in either. The one
  list of size-compiled sources is `cmake/size_policy.cmake`; a change that makes a measured hot path more than 2% slower does not go
  in.
- Shared code reaches the system only through the seams below; where the two systems differ the Windows branch stays under
  `#if defined(_WIN32)` with the Linux alternative beside it. The Win32 value vocabulary (`Interlocked*`, SRW locks, `LONG`/`DWORD`/
  `HANDLE`, `WAIT_*`, `ERROR_*`) stays in shared code; behavior has neutral names.
- Text that names the system goes through `MAICORD_OS_TEXT("Windows ...", "your desktop ...")`; `tools/check_linux_ui_text.py` and
  `maicord_linux_catalog_text` fail when shown text names Windows on Linux.
- Linux tests: each gets private `XDG_DATA_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME` and `MAICORD_SECRET_SERVICE=off`. Opt-in
  integration tests need their script (`MAICORD_WAYLAND_INTEGRATION`, `MAICORD_PULSE_INTEGRATION`, `MAICORD_SPEECHD_INTEGRATION`).
- The client identity is the official desktop client of the host platform (`client_user_agent.hpp`); bump the Linux constants and
  `tests/app/account/client_identity.cpp` together.
- Packaging: the application id is `io.github.maj113.MaiCord`; the tarball install (it alone carries `.maicord-portable`) and the glibc
  AppImage (`$APPIMAGE`, own manifests: `arch` `linux-x64-appimage`, `g_update_flavor`) may self-update; musl installs never do, and the
  musl AppImage keeps AppImageUpdate. `packaging/nfpm/nfpm.yaml` is the source of the dependency lists; `debian/control`,
  `rpm/maicord.spec` and the AUR PKGBUILDs repeat them and `packaging/check_recipes.py` keeps them in step;
  `packaging/check.sh` (which runs it) and `check_artifacts.sh` are the gates. Flathub needs a hand-written
  source-build manifest (its policy refuses AI-generated manifests and AI-opened submissions): do not write or submit one.

## Where things live (the per-OS seams)

- Files, threads, waits, processes, text, paths, time: `include/maicord/os` (Windows inline forwarders in `os/windows`, Linux in
  `src/os/linux`); memory `core/virtual_memory.hpp`, `core/heap.hpp`; app paths and environment `core/data_path.hpp`; images
  `core/module.hpp`; resources `core/resource.hpp`; clock `core/time.hpp`; libraries `core/shared_library.hpp`.
- Sockets and the I/O loop: `include/maicord/platform/net` (`NetworkAddress`, `network_last_error()`); Linux `src/platform/net/linux`
  (the IOCP model on io_uring, one ring per loop, DNS on a getaddrinfo pool; see its README).
- Window service: `platform/window/window.hpp`, `event.hpp`, `key.hpp`, `pump.hpp` (`WindowEvent`s into one sink, `window_*` calls
  out). Windows `src/platform/window/win32`, Linux `src/platform/window/wayland` (README: Wayland only, `dlopen`ed libwayland,
  bindings generated from `third_party/wayland`, the GPU backend owns the buffers, every integration protocol optional), tests
  `src/platform/window/null` (`maicord_window_null`, listed before `maicord_platform`; `tests/support/window_fixture.hpp`). Handlers
  take events, never messages; the shell (`src/app/shell`) holds no window Win32; Windows glue is `src/app/windows`, Linux
  `src/app/linux`.
- GPU: `include/maicord/gpu/api.hpp`; Windows Direct3D 11 (`src/gpu/d3d11`), Linux GLES 3.1 over EGL (`src/gpu/gles`). Shared code
  calls `gpu_*` only and releases handles with `gpu_release`.
- Renderer, effects, Lottie geometry: `src/render` (shared). Text input: `ui/text/secure_text_store.hpp` (Windows TSF, Linux
  text-input-v3 in `src/ui/text/linux`).
- Client shaders: HLSL in `src/render/shaders` is the Windows source; its hand-written GLSL ES 3.10 twin in `src/render/shaders/gles`
  (README there) is what the Linux client packs, with no shader compiler in the build. Change both in one commit: the twin's first
  line records the HLSL's hash, `tools/check_shader_twins.py` (every build) fails naming the twin when the HLSL changed
  alone, and `--stamp <twin>` records the hash after you reviewed the pair. DXC and SPIRV-Cross are for plugin effects only.
- Audio: Windows WASAPI/SAPI, Linux `src/platform/audio/pulse` and `speechd`. Video: Windows DXVA/Media Foundation, Linux
  `src/platform/decode/{vaapi,software,linux}` behind `gpu_stream_decoder.hpp` (VA-API dma-buf to EGLImage, dav1d/openh264 software
  tier, MP4; planes in `src/gpu/gles/video_planes.cpp`; see its README); AAC in-house on Linux. Go Live encoding:
  `src/platform/encode` behind `gpu_stream_encoder.hpp` (Windows hardware Media Foundation transforms on D3D11 textures, Linux
  VA-API on the decoders' display; see its README), quality presets in `media/video/stream_quality.hpp`. Go Live capture:
  `src/platform/capture` behind `platform/capture/screen_capture.hpp` and `platform/audio/stream_audio.hpp` (Windows
  Windows.Graphics.Capture, DXGI Desktop Duplication and WASAPI process loopback; Linux the ScreenCast portal and
  `dlopen`ed PipeWire: dma-bufs, and the other applications' playback for stream audio; see its README).
- Crypto and TLS: Windows CNG/SChannel, Linux system OpenSSL 3 or 4 through `dlopen` (`src/platform/crypto/openssl`,
  `src/platform/net/linux/tls_client.cpp`).
- Desktop: `platform/desktop` (`shell_open.hpp`, notifications, spell checker, security keys); Linux over the in-house D-Bus client
  (`src/platform/dbus`: portals, tray, single instance, secrets) and AT-SPI (`src/platform/atspi`). System fonts: `platform/font`
  (GDI / fontconfig). Time zones: `os/local_time.hpp`.
- Application: `src/app`, `src/ui` build for both systems (`cmake/linux_libraries.cmake` globs `src/app` into `maicord_app_client`); Windows-only parts keep
  `#if defined(_WIN32)` beside the Linux alternative.
- Plugins: PE and ELF loaders in `src/app/plugins/loader.cpp`. Updater: `src/app/update` (Linux `update/linux`).
- Runtime: `src/core/linux` (`_start`, raw-syscall runtime, memory functions, libc stub contract, the glibc/musl image; see its
  README). Packaging: `packaging/` (tarball, AppImage, one-line installer, nfpm `.deb`/`.rpm`/Arch package, signed apt/dnf/pacman repositories, PPA, COPR/OBS,
  AUR, Alpine/Void/Gentoo, Nix; its README is the reference).
- Build files: `CMakeLists.txt` includes `cmake/*.cmake`; the libraries both systems build are in `cmake/libraries.cmake` (entries
  prefixed `windows:` or `linux:` belong to one system), the ones only one system builds in `cmake/windows_libraries.cmake` and
  `cmake/linux_libraries.cmake`; tests of both systems are registered in `cmake/test_registry.cmake`.

## Commits

- Imperative subject lines, one topic per commit.
- Update tests with every behavior change and register them in `cmake/test_registry.cmake` (`ONLY WINDOWS` or `ONLY LINUX` for
  a test of code one system builds).
