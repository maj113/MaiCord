# Contributing to MaiCord

Thanks for your interest! MaiCord is a small, opinionated codebase. These
notes keep changes easy to review and the client small and fast.

## Before you start

- For anything larger than a bug fix, open an issue first so the approach can
  be agreed before you write the code. The issue forms cover bugs, missing
  Discord features and performance problems.
- Read the [disclaimer](README.md#disclaimer). Contributions must not include
  Discord-owned assets. That covers images, sounds, fonts, and copied client
  code or bundles.
- Research notes, captures and client bundles stay on your machine.
  `Analysis/` and `docs/` are git-ignored for this reason.

## Building and testing

Follow [Building from source](README.md#building-from-source). In short:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1         # client only
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1 -Test   # client + whole test suite
```

- There is one build directory, the sibling `..\MaiCord-build\Release`. Don't
  add other build trees, staging copies or archives. `build.ps1` removes test
  executables, profiles and logs after every run.
- The whole test suite must finish within **20 seconds** of execution. If a
  new test is slow, shrink its input but keep the coverage that matters.
  Assert outputs and counts, never wall-clock times, so tests stay stable on
  slower machines.
- If MaiCord is running from the build directory, add `-SkipClient`; the
  script won't overwrite a running client.
- `.\tools\build.ps1 -Test -TestNames name1,name2`, run from a PowerShell
  prompt, runs only the named CTest cases while you iterate.

### Build files

`CMakeLists.txt` includes the modules in `cmake/`:

| Module | Contents |
| --- | --- |
| `build_contract.cmake` | Compiler/linker contract, `maicord_library()`, `maicord_program()` |
| `generated_sources.cmake` | Packed UI data, shaders (`maicord_shader()`), icon, sounds |
| `libraries.cmake` | Shipping static libraries |
| `client.cmake` | `MaiCord.exe`, `MaiCordAccount.dll`, updater identity, cold-code `/Os` |
| `tests.cmake`, `test_support.cmake` | The temporary test configuration and its helpers |
| `test_registry.cmake` | Every test executable and CTest case |
| `perf_probe.cmake`, `benchmarks.cmake`, `integration.cmake`, `reference_tools.cmake` | Opt-in tools |

A test is one `maicord_test()` call; more cases on the same executable use
`maicord_add_tests()`. Tests link non-`/GL` copies of the shipping libraries
automatically.

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

- **Freestanding C++23.** Don't use the STL, the CRT, exceptions or RTTI. Use
  the types in `include/maicord/core`. New Win32 imports need a reason. Every
  build runs the PE audits in `tools/audit_pe.ps1`.
- Format with the repository `.clang-format`. Match the style of the file you
  are editing.
- Keep memory and binary size in mind. Mention any EXE/DLL size change in the
  PR description, and back performance changes with before/after numbers from
  [`tools/perf`](tools/perf/README.md).
- UI colors, radii and spacing come from theme tokens;
  `tools/check_theme_literals.py` (part of the suite) rejects new literals.
- Public headers in `include/maicord/` document ownership and failure
  behavior. Keep those comments accurate when you change behavior.
- Add or update tests in `tests/` for every behavior change, and register them
  in `cmake/test_registry.cmake` (see [Build files](#build-files)).

## Pull requests

- Keep each PR focused on one topic. Use imperative commit subjects, for
  example `ui: fix member list scroll clamp`. Labels are added from the
  changed paths automatically.
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
