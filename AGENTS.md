# Working on MaiCord

Guidance for coding agents and contributors. [CONTRIBUTING.md](CONTRIBUTING.md)
has the same rules in more detail.

## Build and test

- Build only with `tools/build.ps1`, from Windows PowerShell 5.1:
  `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1`
  builds the Release client; add `-Test` for the whole suite and
  `-TestNames a,b` for single tests.
- The only toolchain is clang-cl with lld-link and llvm-lib from the
  maj113/llvm-pe fork: `-LlvmBin`, `MAICORD_LLVM_BIN`, else the release pinned
  in `tools/llvm_toolchain.json` (fetched once into
  `%LOCALAPPDATA%\MaiCord-toolchain`); Visual Studio supplies the SDK, MASM,
  rc and mt.
- The only build directory is the sibling `..\MaiCord-build\Release`. Don't
  create other build trees, staging copies, QA directories or archives, and
  don't move unwanted output elsewhere: delete it.
- If `MaiCord.exe` is running from the build directory, add `-SkipClient` to a
  `-Test` run; the script won't overwrite a running client. Never close
  someone's running client for them.
- The suite must finish within 20 seconds. Keep coverage when trimming test
  work. Tests assert outputs and counts (operations, allocations, bytes),
  never wall-clock times.
- The script removes test executables, objects, temporary profiles and logs
  after each run; leave the build directory holding only the client binaries
  and their runtime assets. For iterative work, `-KeepIntermediates` keeps
  objects, ninja state and `compile_commands.json`, so later builds are
  incremental and `python tools/check_sources.py` can parse changed files and
  every unit including a changed header without building. Finish with a
  normal run to restore the clean directory.
- The Linux port (`src/core` first) builds only with `tools/build_linux.sh`, run in WSL by
  `tools\build_linux.ps1`: stock clang and ld.lld 23, `-nostdinc` with the kernel and glibc interface in
  `include/maicord/sys/linux`, ELF images audited by `tools/audit_elf.py`. Its build directory is `~/mc-build`
  on ext4 (never under `/mnt`), emptied after each run unless `--keep`. The Windows build, its toolchain and every
  rule above stay as they are: code the Windows build compiles must not change for the port, and
  `python tools/compare_pe.py OLD.exe NEW.exe` proves it by comparing two builds section by section.
  The Linux crypto and TLS backend (`src/platform/crypto/openssl`, `src/platform/net/linux/tls_client.cpp`) uses the
  system OpenSSL 3 or 4 through `dlopen` only, so images gain no `DT_NEEDED`: `python tools/check_openssl_abi.py`
  checks its function and constant tables against the installed libssl-dev headers, and the TLS tests run
  `openssl s_server` on the loopback (`tests/platform/net/tls_smoke.sh`).
  The Linux window backend (`src/platform/window/wayland`) loads libwayland-client, libwayland-egl, libwayland-cursor
  and libxkbcommon through `dlopen` too; its protocol bindings are generated at build time by
  `tools/generate_wayland_protocols.py` from the XML in `third_party/wayland` (no `wayland-scanner`). The pure part
  of it (`maicord_wayland_input`: key table, characters, double click, key repeat, the generated tables against
  libwayland-client's) runs in the normal Linux suite; `tools/test_wayland_window.sh` starts headless or nested sway
  and runs the opt-in integration tests (`maicord_wayland_window_lifecycle`, `maicord_wayland_window_input`, which
  drive a virtual pointer and keyboard) with `grim` screenshots.

## Code rules

- For Discord API, authentication and Gateway work, consult
  [Discord Userdoccers](https://docs.discord.food/) for new features and to verify
  existing behavior, alongside Discord's official documentation. Cross-check
  uncertain details against the supplied client research; keep regression tests
  for request metadata, session lifetimes, rate limits and challenge handling.
  Its [MCP endpoint](https://docs.discord.food/mcp) provides `search_docs` and
  `fetch_page`; use the sitemap/navigation to enumerate all pages and sections.
- Freestanding C++23: no STL, CRT, exceptions or RTTI. Use
  `include/maicord/core`. Every build runs the PE audits in
  `tools/audit_pe.ps1` (imports, debug data, initialized-data budget).
- Code compiles without unwind tables: no SEH, unhandled-exception filters or
  stack walks through MaiCord frames (see CONTRIBUTING.md).
- Targets compile with `-nostdinc`: Windows SDK, C library and compiler
  declarations (intrinsics included) come from `include/maicord/sys` and
  `include/maicord/core/win32_*.hpp`, declared exactly as the SDK and compiler
  declare them. Add what you use there; the generated conformance unit
  (`tools/generate_sys_conformance.py`) checks it against the SDK on every
  build.
- UI colors, radii and spacing come from theme tokens and scales
  (`include/maicord/ui/theme/tokens.inc`); `tools/check_theme_literals.py`
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
