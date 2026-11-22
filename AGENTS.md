# Working on MaiCord

Rules for agents and contributors. [CONTRIBUTING.md](CONTRIBUTING.md) has the detail (build, toolchain, test fakes, environment
overrides, per-system seams); each area's design notes are in the `README.md` beside its code.

## Build and test

`tools\build.ps1` (Windows PowerShell 5.1: `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1`) and
`tools/build_linux.sh` (WSL: `wsl -d Ubuntu-Preview --cd <checkout> -- bash -l tools/build_linux.sh ...`) take the same options:

|Windows|Linux||
|---|---|---|
|`-Test`|`--test`|also build and run the suite|
|`-TestNames a,b`|`--test-names a,b`|only these tests; links the client only if one uses it|
|`-SkipClient`|`--skip-client`|tests use the client already built|
|`-KeepIntermediates`|`--keep-intermediates`|keep objects and `compile_commands.json`|
|`-SizeReport`|`--size-report`|the client's sections and largest code|
|`-LlvmBin DIR`|`--llvm-bin DIR`|another LLVM (default: the llvm-pe pin in `tools/llvm_toolchain.json`)|
|`-TestTimeoutScale N`|`--test-timeout-scale N`|multiply every test timeout|

- Windows builds only into the sibling `..\MaiCord-build\Release`: no other build trees, staging copies, QA directories or
  archives; delete unwanted output. If `MaiCord.exe` runs from it, add `-SkipClient` to `-Test`; never close someone's running
  client. `-TestHooks` replaces the shipping client there.
- Linux builds into `~/mc-build` on ext4, never under `/mnt` (`MAICORD_LINUX_SOURCE`/`MAICORD_LINUX_BUILD` select others; use
  your own on a shared machine). A checkout on a Windows mount is copied to `~/mc` first, only changed files when Windows
  PowerShell can list them (`tools/list_checkout_files.ps1`).
- Runs on one build directory wait for each other (Windows: mutex from `tools/build_lock_name.ps1`; Linux: `.lock` beside it);
  test runs wait machine-wide (`Local\MaiCordTestRun`, `~/.maicord-linux-test.lock`); `tools/build.ps1` starts its 20 s check
  after that wait.
- A run leaves only the client and its runtime assets (Windows: also the CMake cache in `cmake`). With `-KeepIntermediates`,
  `python tools/check_sources.py` parses changed files and every unit including a changed header without building (Windows);
  finish with a normal run.
- Every link audits the client (`tools/audit_image.py`). Linux `--test` and Windows `-TestHooks` builds define
  `MAICORD_TEST_HOOKS`: ship, package and measure size only from builds without them.
- CI builds with `MAICORD_REQUIRE_DEFLATE_CACHE=1`. After changing a shader, icon, flag or emoji table, build
  `maicord_update_deflate_cache` (Linux GLSL pack: `tools/build_gles_shader_pack.py` with `MAICORD_UPDATE_DEFLATE_CACHE=1`)
  and commit `tools/deflate_cache.txt`; check release candidates with the variable set.
- The suite finishes within 20 s. Tests assert outputs and counts (operations, allocations, bytes), never wall-clock times.
  Keep coverage when trimming.
- Linux integration runs are `tools/test_*.sh` (CI set: `tools/test_linux_integration.sh`). Validate workflow changes with
  `actionlint` and `zizmor`.
- Work on one system must not change the other's client (`python tools/compare_image.py OLD NEW`).

## Code rules

- Discord API, authentication, Gateway: consult [Userdoccers](https://docs.discord.food/) (MCP `https://docs.discord.food/mcp`:
  `search_docs`, `fetch_page`; pages via the sitemap), Discord's official docs and the local client research. Keep regression
  tests for request metadata, session lifetimes, rate limits and challenge handling.
- Freestanding C++23: no STL, CRT, exceptions or RTTI; use `include/maicord/core`. No unwind tables: no SEH, unhandled-exception
  filters or stack walks through MaiCord frames.
- `-nostdinc`: SDK, C library and compiler declarations (intrinsics too) live in `include/maicord/sys` and `core/win32_*.hpp`
  (Linux: `include/maicord/sys/linux`), declared exactly as the SDK and compiler declare them. Add what you use there; a
  generated conformance unit checks them each build.
- Compiler baseline: Haswell without AVX2 and FMA (`/arch:AVX2` or `-march=haswell`, `-mno-avx2 -mno-fma`); do not change it.
  AVX2 only in hand-written `MAICORD_HASWELL` functions (`include/maicord/sys/simd_haswell.hpp`), FMA only in
  `MAICORD_HASWELL_FMA` ones, with no other path: the minimum processor is Haswell. Each client's entry checks the processor
  first (`maicord/app/shell/processor_check.hpp`); code running before the check returns is `MAICORD_ANY_PROCESSOR`. FMA only
  where no output must keep exact rounding (never layout, hit tests or bit-pinned output), via the fmadd intrinsics, in a
  function starting with `#pragma clang fp contract(off)` so both systems fuse the same operations.
- Lossy decoders (Opus, JPEG, WebP/VP8, video) are held to high PSNR against their reference and the same look, not bit for
  bit; Opus float paths to libopus (RFC 8251 `opus_compare` plus a PSNR floor), not to their own PCM. A change that moves their
  output replaces the bit-exact pin with a PSNR floor against a reference decoder and says so. SILK and lossless formats (PNG,
  GIF, lossless WebP) stay bit-exact.
- UI colours, radii and spacing come from theme tokens (`include/maicord/ui/theme/tokens.inc`).
- An HLSL shader in `src/render/shaders` and its hand-written GLSL ES 3.10 twin in `src/render/shaders/gles` change in one
  commit; `tools/check_shader_twins.py --stamp <twin>` records the reviewed pair.
- Each client is at most 6,500,000 bytes (Linux stripped); report a change in either. `cmake/size_policy.cmake` is the one list
  of size-compiled sources.
- Performance and size work needs before/after numbers (`tools/perf/run_perf.ps1`: CPU, wakeups, allocations, memory; the
  client sizes). A change making a measured hot path more than 2% slower does not go in; keep only changes that pay.
- No test-only code in `src`: tests link fakes; a test-only switch goes through `test_hook()` (`core/test_hook.hpp`). User and
  operator environment overrides are documented in CONTRIBUTING.md.
- Handlers take `WindowEvent`s, never window messages; shared code calls `gpu_*` only and releases with `gpu_release`.
- Never ship Discord-owned assets (icons, sounds, illustrations, fonts) or copied client code; research stays in the
  git-ignored `Analysis/` and `docs/`. Name things by what they do, not after Discord or reverse-engineering artifacts.
- Never log tokens, passwords, signed URLs or message content.

## Linux rules

- One freestanding PIE with its own `_start`, raw syscalls, no CRT. `DT_NEEDED` is `libc.so.6` alone, symbol versions at most
  GLIBC_2.34, and the same bytes run on musl (`tools/retarget_libc.py`); `tools/audit_image.py [--libc musl]` enforces it. A new
  libc import must be defined by musl (else weak) and appear in the stubs, `ALLOWED_IMPORTS` and `tools/check_libc_abi.py`'s
  pins.
- Every other library is `dlopen`ed through `core/shared_library.hpp`: absent means unavailable, never a crash. A new `dlopen`ed
  function needs its table entry (no unused ones) and passes `tools/check_<library>_abi.py`.
- Kernel floor 6.1: use its interface unconditionally, no version probes or older-kernel alternatives; newer interfaces only
  for a real win, naming the kernel. File-system gaps (RENAME_NOREPLACE) are not kernel versions. The network loop is io_uring
  only: without it the client exits at startup naming the cause (containers need `--security-opt seccomp=unconfined`).
- Shared code reaches the system only through the seams (`include/maicord/os`, `platform`, `gpu/api.hpp`,
  `core/{virtual_memory,heap,data_path,module,resource,time,shared_library}.hpp`). Where systems differ, the Windows branch stays
  under `#if defined(_WIN32)` with the Linux one beside it. The Win32 value vocabulary (`Interlocked*`, SRW locks,
  `LONG`/`DWORD`/`HANDLE`, `WAIT_*`, `ERROR_*`) stays in shared code; behaviour has neutral names.
- Shown text naming the system goes through `MAICORD_OS_TEXT("Windows ...", "your desktop ...")`;
  `tools/check_linux_ui_text.py` fails when shown text names Windows on Linux.
- Each Linux test gets private `XDG_DATA_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME` and `MAICORD_SECRET_SERVICE=off`; opt-in
  integration tests need their script.
- The client identity is the host platform's official desktop client (`client_user_agent.hpp`); bump the Linux constants and
  `tests/app/account/client_identity.cpp` together.
- Packaging: application id `io.github.maj113.MaiCord`. `packaging/nfpm/nfpm.yaml` is the source of the dependency lists the
  other recipes repeat (`packaging/check_recipes.py`); `packaging/check.sh` and `check_artifacts.sh` are the gates. Only the
  tarball install (it carries `.maicord-portable`) and the glibc AppImage self-update; musl installs never do. Never write or
  submit a Flathub manifest (its policy refuses AI-generated manifests and AI-opened submissions).

## Build files and commits

- `CMakeLists.txt` includes `cmake/*.cmake`: libraries both systems build are in `libraries.cmake` (`windows:`/`linux:`
  prefixes mark one-system entries), the others in `windows_libraries.cmake` and `linux_libraries.cmake`; `src/app` and
  `src/ui` build for both.
- Update tests with every behaviour change and register them in `cmake/test_registry.cmake` (`ONLY WINDOWS`/`ONLY LINUX` for
  code one system builds). A test on the headless window lists `maicord_window_null` before `maicord_platform`.
- Imperative subject lines, one topic per commit.
