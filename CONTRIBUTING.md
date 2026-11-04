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

### Linux (in progress)

The Linux port builds the core library and its tests, nothing more yet. Run
`powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build_linux.ps1 -Test` (or `tools/build_linux.sh
--test` inside WSL): the script rsyncs the tree to ext4, builds with clang and ld.lld 23 into `~/mc-build`, runs
`tools/audit_elf.py` on the images and the Linux tests, and empties the build directory afterwards (`--keep` /
`-KeepIntermediates` keeps it). Its header documents the layout and decisions. Platform code lives in
`src/core/linux` and `include/maicord/sys/linux`; shared headers choose between the two with `#if defined(_WIN32)`
and the Windows branch stays byte-for-byte the code it was. Shared code uses the portable wrappers in
`include/maicord/core` (`intrinsics.hpp`, `virtual_memory.hpp`, `interlocked.hpp`) instead of MSVC intrinsics.

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
| `client.cmake` | `MaiCord.exe` with the account module, updater identity, cold-code `-Oz` (the only code the machine outliner touches) |
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
- Public headers in `include/maicord/` document ownership and failure
  behavior; keep those comments accurate when behavior changes.
- **Source layout.** Each module (`app`, `ui`, `render`, `protocol`, `model`,
  `text`, `media`, `platform`) is split into topic folders, and
  `include/maicord/<module>/<folder>/` mirrors `src/<module>/<folder>/`. Put a
  new file in the folder of its feature; a header private to a module stays
  beside its sources. File names drop a prefix the folder already states
  (`src/app/chat/messages/send.cpp`, not `application_message_send.cpp`) and,
  in `src/app`, the `application_` prefix of the client-side glue, unless the
  shorter name would collide with another file in the folder; `account_` (the
  account module's side of a feature) is kept. Symbol names are unaffected.
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
