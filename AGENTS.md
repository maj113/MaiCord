# Working on MaiCord

Rules for coding agents and contributors. [CONTRIBUTING.md](CONTRIBUTING.md) has the detail (build, toolchain, test fakes,
environment overrides, and the map of per-system seams under "Project layout"); subsystem design notes are in the `README.md`
beside each area's code.

## Build and test

One entry point per system with the same options: `tools\build.ps1` (from Windows PowerShell 5.1:
`powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1`) and `tools/build_linux.sh` (in WSL, or
`wsl -d Ubuntu-Preview --cd <checkout> -- bash -l tools/build_linux.sh ...`).

|Windows|Linux||
|---|---|---|
|`-Test`|`--test`|also build and run the test suite|
|`-TestNames a,b`|`--test-names a,b`|only these tests|
|`-SkipClient`|`--skip-client`|tests use the client already in the build directory|
|`-KeepIntermediates`|`--keep-intermediates`|keep objects and `compile_commands.json` (incremental builds)|
|`-SizeReport`|`--size-report`|print the client's sections and largest code|
|`-LlvmBin DIR`|`--llvm-bin DIR`|another LLVM (default: the llvm-pe release pinned in `tools/llvm_toolchain.json`)|
|`-TestTimeoutScale N`|`--test-timeout-scale N`|multiply every test timeout|

- Windows: the only build directory is the sibling `..\MaiCord-build\Release`. No other build trees, staging copies, QA
  directories or archives; delete unwanted output. If `MaiCord.exe` is running from it, add `-SkipClient` to a `-Test` run and
  never close someone's running client. `-TestHooks` replaces the shipping client there.
- Linux: build directory `~/mc-build` on ext4, never under `/mnt`; `MAICORD_LINUX_SOURCE` and `MAICORD_LINUX_BUILD` select
  others (use your own on a shared machine).
- A run leaves only the client and its runtime assets. On Windows with `-KeepIntermediates`, `python tools/check_sources.py`
  parses changed files and every unit including a changed header without building; finish with a normal run.
- Every link audits the client (`tools/audit_image.py`). A Linux `--test` build and a Windows `-TestHooks` client define
  `MAICORD_TEST_HOOKS` and are not shipping builds: ship, package and measure size only from a build without them.
- The suite must finish within 20 seconds. Tests assert outputs and counts (operations, allocations, bytes), never wall-clock
  times. Keep coverage when trimming.
- Linux integration runs are `tools/test_*.sh` (`tools/test_linux_integration.sh` is the CI set). Validate workflow changes
  with `actionlint` and `zizmor`.
- Work on one system must not change the other's client: `python tools/compare_image.py OLD NEW` compares two builds.

## Code rules

- Discord API, authentication and Gateway: consult [Discord Userdoccers](https://docs.discord.food/) (MCP endpoint
  `https://docs.discord.food/mcp`: `search_docs`, `fetch_page`; enumerate pages through the sitemap) alongside Discord's
  official documentation and the local client research. Keep regression tests for request metadata, session lifetimes, rate
  limits and challenge handling.
- Freestanding C++23: no STL, CRT, exceptions or RTTI; use `include/maicord/core`.
- No unwind tables: no SEH, unhandled-exception filters or stack walks through MaiCord frames.
- `-nostdinc`: SDK, C library and compiler declarations (intrinsics included) live in `include/maicord/sys` and
  `core/win32_*.hpp` (Linux: `include/maicord/sys/linux`), declared exactly as the SDK and compiler declare them. Add what you
  use there; a generated conformance unit checks them on every build.
- UI colors, radii and spacing come from theme tokens (`include/maicord/ui/theme/tokens.inc`);
  `tools/check_theme_literals.py` fails on new literals.
- Shaders: the HLSL in `src/render/shaders` and its hand-written GLSL ES 3.10 twin in `src/render/shaders/gles` change in one
  commit; `tools/check_shader_twins.py --stamp <twin>` records the reviewed pair.
- Size: each client is at most 6,500,000 bytes (Linux stripped); report a change in either. `cmake/size_policy.cmake` is the
  one list of size-compiled sources.
- Performance and size work needs before/after numbers (`tools/perf/run_perf.ps1`: CPU, wakeups, allocations, memory; the
  client sizes). A change that makes a measured hot path more than 2% slower does not go in; keep only changes that pay.
- Test-only code does not live in `src`: tests link fakes, and a test-only switch goes through `test_hook()`
  (`core/test_hook.hpp`). User and operator environment overrides are documented in CONTRIBUTING.md.
- Handlers take `WindowEvent`s, never window messages; shared code calls `gpu_*` only and releases handles with `gpu_release`.
- Never ship Discord-owned assets (icons, sounds, illustrations, fonts) or copied client code. Research material stays in the
  git-ignored `Analysis/` and `docs/`.
- Name things by what they do, not after Discord or reverse-engineering artifacts.
- Never log tokens, passwords, signed URLs or message content.

## Linux rules

- One freestanding PIE with its own `_start`, raw syscalls and no CRT. `DT_NEEDED` is `libc.so.6` alone, with symbol versions
  at most GLIBC_2.34, and the same bytes run on musl (`tools/retarget_libc.py`); `tools/audit_image.py [--libc musl]` enforces
  it. A new libc import must also be defined by musl (else weak) and appear in the stubs, `ALLOWED_IMPORTS` and
  `tools/check_libc_abi.py`'s pins.
- Every other library is opened with `dlopen` through `core/shared_library.hpp`: absent means "unavailable", never a crash. A
  new `dlopen`ed function needs its table entry (and no unused ones) and passes `tools/check_<library>_abi.py`.
- Kernel floor is Linux 6.1: use its interface unconditionally, with no version probes and no alternatives for older kernels;
  newer interfaces only where the win is real, naming the kernel they need. File-system gaps (RENAME_NOREPLACE) are not kernel
  versions. The network loop is io_uring only: without it the client exits at startup naming the cause (container runs need
  `--security-opt seccomp=unconfined`).
- Shared code reaches the system only through the seams (`include/maicord/os`, `platform`, `gpu/api.hpp` and
  `core/{virtual_memory,heap,data_path,module,resource,time,shared_library}.hpp`). Where the systems differ, the Windows branch
  stays under `#if defined(_WIN32)` with the Linux alternative beside it. The Win32 value vocabulary (`Interlocked*`, SRW
  locks, `LONG`/`DWORD`/`HANDLE`, `WAIT_*`, `ERROR_*`) stays in shared code; behavior has neutral names.
- Shown text that names the system goes through `MAICORD_OS_TEXT("Windows ...", "your desktop ...")`;
  `tools/check_linux_ui_text.py` fails when shown text names Windows on Linux.
- Each Linux test gets private `XDG_DATA_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME` and `MAICORD_SECRET_SERVICE=off`; opt-in
  integration tests need their script.
- The client identity is the official desktop client of the host platform (`client_user_agent.hpp`); bump the Linux constants
  and `tests/app/account/client_identity.cpp` together.
- Packaging: the application id is `io.github.maj113.MaiCord`. `packaging/nfpm/nfpm.yaml` is the source of the dependency
  lists, which the other recipes repeat (`packaging/check_recipes.py`); `packaging/check.sh` and `check_artifacts.sh` are the
  gates. Only the tarball install (it carries `.maicord-portable`) and the glibc AppImage self-update; musl installs never do.
  Do not write or submit a Flathub manifest: its policy refuses AI-generated manifests and AI-opened submissions.

## Build files and commits

- `CMakeLists.txt` includes `cmake/*.cmake`: libraries both systems build are in `libraries.cmake` (`windows:` or `linux:`
  prefixes mark one-system entries), the others in `windows_libraries.cmake` and `linux_libraries.cmake`; `src/app` and
  `src/ui` build for both systems.
- Update tests with every behavior change and register them in `cmake/test_registry.cmake` (`ONLY WINDOWS` or `ONLY LINUX`
  for a test of code one system builds). A test on the headless window lists `maicord_window_null` before `maicord_platform`.
- Imperative subject lines, one topic per commit.
