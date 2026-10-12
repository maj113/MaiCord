# Working on MaiCord

Guidance for coding agents and contributors. [CONTRIBUTING.md](CONTRIBUTING.md)
has the same rules in more detail.

## Build and test

- Build only with `tools/build.ps1`, from Windows PowerShell 5.1:
  `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1`
  builds the Release client; add `-Test` for the whole suite and
  `-TestNames a,b` for single tests.
- The only build directory is the sibling `..\MaiCord-build\Release`. Don't
  create other build trees, staging copies, QA directories or archives, and
  don't move unwanted output elsewhere: delete it.
- If `MaiCord.exe` is running from the build directory, use `-SkipClient`; the
  script won't overwrite a running client. Never close someone's running
  client for them.
- The suite must finish within 20 seconds. Keep coverage when trimming test
  work. Tests assert outputs and counts (operations, allocations, bytes),
  never wall-clock times.
- The script removes test executables, objects, temporary profiles and logs
  after each run; leave the build directory holding only the client binaries
  and their runtime assets.

## Code rules

- Freestanding C++23: no STL, CRT, exceptions or RTTI. Use
  `include/maicord/core`. Every build runs the PE audits in
  `tools/audit_pe.ps1` (imports, debug data, initialized-data budget).
- UI colors, radii and spacing come from theme tokens and scales
  (`include/maicord/ui/theme_tokens.inc`); `tools/check_theme_literals.py`
  fails on new literals.
- Never ship Discord-owned assets (icons, sounds, illustrations, fonts) or
  copied client code. Research material stays in the git-ignored `Analysis/`
  and `docs/` directories.
- Name things by what they do, not after Discord or reverse-engineering
  artifacts.
- Performance and size work needs before/after numbers: `tools/perf/run_perf.ps1`
  for CPU, wakeups, allocations and memory, and the EXE/DLL sizes from the build.
  Keep only changes that pay for themselves.
- Never log tokens, passwords, signed URLs or message content.

## Commits

- Imperative subject lines, one topic per commit.
- Update tests with every behavior change and register them in
  `cmake/test_registry.cmake`.
