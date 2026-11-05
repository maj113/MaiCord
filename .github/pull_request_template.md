## What and why

<!-- What does this change, and what problem does it solve? Link issues with "Fixes #123". -->

## How it was checked

<!-- Tests added or updated, and anything checked by hand. For performance changes, include before/after numbers from tools/perf/run_perf.ps1 or a benchmark. -->

- [ ] `tools\build.ps1 -Test` passes (under 20 s)
- [ ] `tools/build_linux.sh --test` passes, if Linux or shared code changed
- [ ] No Discord-owned assets and no hard-coded colors outside theme tokens
- [ ] Binary size change in either client, if any, is noted above
