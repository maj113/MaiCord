# Performance benchmarks

## Offline workload

```
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\perf\run_perf.ps1 [-Runs 2] [-WarmOnly] [-Raw] [-Sampling] [-Debugger]
```

`run_perf.ps1` builds `MaiCordPerfProbe.exe` and `MaiCordPerfAccount.dll` through
`tools\build.ps1 -PerfProbe -SkipClient` in the single Release cache, runs the probe
against a temporary `LOCALAPPDATA`, prints a table, and removes the probe, its objects
and the profiles. It never needs or touches a running client, a real account or the
network.

Each offline launch uses a short scenario and a 15-second timeout.
The scenario keeps 90 timeline scrolls, 24 channel switches, 60 member-list scrolls and six settings visits.
Scenario deadlines and input intervals are ten times shorter than the original workload.
The synthetic account sends churn events at ten times the original rate.
CPU, GPU, memory and elapsed-time measurements use the real clocks.

The shorter idle phases do not measure long-term timer behavior or cache expiry.
Compare results from the same scenario version.
Use `-Runs 1 -WarmOnly` for one launch during local comparisons.
The build time is separate from the timeout for each launch.

| Switch | Effect |
| --- | --- |
| `-Runs N` | Repeats (1-10, default 2): a cold launch on a fresh profile, then a warm launch on the same profile. Tables show medians. |
| `-WarmOnly` | Skip the cold launches; all runs reuse one profile. |
| `-Raw` | Also print each probe's raw report. |
| `-Sampling` | 1 kHz in-process sampling of the main thread (see below). |
| `-Owners` | Attribute other modules' heap blocks and private regions (see below). Slows startup. |
| `-Debugger` | Run each launch under `cdb` and print the faulting stack. Diagnosis only. |

- **MaiCordPerfProbe** is the shipping client source set built with
  `MAICORD_PERF_PROBE` and shipping code generation (`/GL`, LTCG). The define swaps
  the account module file name and skips desktop integration, notifications,
  updates and instance forwarding (`MAICORD_ISOLATED_MAIN`). Unlike the client,
  the probe configuration keeps unwind tables (`MAICORD_UNWIND_TABLES`): the
  sampler and the allocation call stacks walk them.
- **MaiCordPerfAccount.dll** implements the account module ABI offline: a
  deterministic READY (100 guilds, ~2,600 channels, 2,000 users, 300 friends,
  180 DMs) projected by the production Gateway routes, generated message history
  with mixed content, member list syncs for a 48,000-member guild, generated media
  frames, typing/presence/message churn and a call whose cameras present
  synthetic NV12 pictures on the client's device. Every REST submission is answered.

## Production startup

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\perf\run_perf.ps1 -ProductionStartup -Runs 5
```

It builds the client through `tools\build.ps1 -StartupProbe`.
It builds the actual `MaiCord.exe` with `MAICORD_STARTUP_PROBE` and the real account backend.
The build script refuses to overwrite a client that runs from the Release directory.
The default is three fresh/warm pairs. `-Runs 5` requests five pairs, or ten launches.
`-ProductionStartup` does not accept `-WarmOnly`, `-Sampling`, `-Owners` or `-Debugger`.

Each pair starts with a fresh temporary profile in the single Release directory.
The warm launch reuses that profile. A cold launch means a fresh application profile, not empty Windows filesystem caches.
Only each child process receives the temporary `LOCALAPPDATA`, `TEMP` and `TMP` paths.
The child also requires `MAICORD_STARTUP_BENCHMARK=1` and a valid profile marker.
The build script removes the temporary profiles and restores the normal client before it returns.

The client starts without saved credentials through the production login path.
It can contact the unauthenticated remote-auth service. This mode does not use the offline account module.
It skips desktop registration, notifications, updates and instance forwarding.
Its window uses the quiet probe styles and does not take focus.
After the first successful `Present`, the client requests normal window closure.

The runner prints each `MAICORD_STARTUP` record and cold/warm medians, then the medians of the
named startup points (`MAICORD_PERF_MARK` in `main.cpp`, `application_window.cpp`,
`application_frame_presentation.cpp` and `application_shutdown_metrics.cpp`), in ms since process creation.
It accepts a record only from its child process, after exit code zero and valid cycle, CPU-time and memory queries.
It also requires `backend_loaded=1` and `login_active=1` for every accepted launch.
These flags confirm that the real backend and login instance exist. They do not mean that authentication finished.

| Measurement | Definition |
| --- | --- |
| Process TTFF | Process creation to the first successful `Present` return, including the loader work before entry. |
| App TTFF | `application_run` to the same `Present` return. |
| Main/process cycles | Raw lifetime counts from `QueryThreadCycleTime` and `QueryProcessCycleTime`, through the first present. |
| Main/process CPU time | User plus kernel time from `GetThreadTimes` and `GetProcessTimes`, through the first present. |
| WS/private/PWS | Whole-process working set, private committed bytes and private working set at the first present. |

TTFF does not measure display scanout. CPU cycle counts do not measure retired instructions.
The raw record uses microseconds for times and bytes for memory. The table uses milliseconds and KiB.

If the private working-set query is unavailable, the record has `pws_ok=0` and the table shows `unavailable`.
The other memory values remain valid. The runner does not print arbitrary child stderr or account logs after a failure.

Use the same machine, display state, DPI and compiler options for each comparison.
Record the requested and actual compiler-worker counts with the build duration.
Measure the client EXE bytes for each layout.
Keep production-startup results separate from the offline workload results.
The offline workload always uses its synthetic account module and does not measure the production account implementation.

## Phases

Each launch runs one window through these phases, in order: `startup` (process
start to restored shell), `history`, `idle_static` (text-only channel), `scroll`,
`switch` (channel switches), `member_list`, `churn`, `settings`, `anim_scroll`
(back to a mixed channel's GIFs), `idle_anim` (one second with two animations
playing), `idle_fx` (text channel under the built-in Stars
background effect), `call_open`, `call_30fps` and `call_5fps` (a synthetic call
with two cameras presenting 30 and then 5 pictures a second), `idle_final` and `idle_hidden` (minimized; the end
memory snapshot is taken before minimizing).

An idle client may not wake on its own, so the probe arms one timer for its next
deadline (phase end or scripted input) and posts a single `WM_NULL`, which counts
as one message wake. The window is never activated; the client treats that like a
focused window (it pauses media animations only after a real deactivation).

## Report

- **Per phase:** wall time, presents, main-thread and process CPU,
  main-thread CPU per presented frame (p50/p95/p99), main-loop wakes, context
  switches, client `VirtualAlloc`/`VirtualFree`/heap calls (import hooks on the
  probe image only), page faults, private working set, private bytes, threads and
  handles.
  Raw reports include exact `main_cycles` and `process_cycles` deltas from the Windows cycle counters.
  The existing CPU-millisecond columns divide those counts by the calibrated TSC rate.
- **Retained work:** Markdown parser runs against cache hits, content text streams
  built (and how many kept the shell's glyphs), paint streams built.
- **Uploads:** paint, glyph and media instance bytes written to the GPU per
  presented frame; member list rows resolved for media; shell segments replayed
  instead of built.
- **GPU:** immediate-context calls per painted frame, counted through the context's
  vtable (state binds, scissor rectangles and repeats of the bound one, buffer maps,
  copies, compute dispatches and binds; draws come from the renderer's own count).
  GPU time is measured on a replay: timestamps around `Present` would include the
  swap chain's wait for its back buffer, so after each present the probe draws the
  same retained stream again, with the presented damage, into an offscreen target of
  the back buffer's format. The replay's main-thread CPU is excluded from the CPU
  columns.
- **Memory snapshots** (shell, after scroll, end): committed, resident and
  private-resident bytes attributed to client `VirtualAlloc` regions (by allocation
  call stack, resolved through `MaiCordPerfProbe.map`), heaps, stacks,
  write-combined and other private memory (driver), images by module, and mapped
  files. Heap blocks above the heap's virtual-memory threshold count as heap memory.
  Synthetic module and probe memory are reported separately. Each heap and the
  largest write-combined and other private regions are listed.
- **GPU resources:** live and created bytes of the buffers, 2D textures and swap chain
  buffers the client created, by usage (sizes from the descriptions; driver padding and
  internal allocations excluded), at the first present, the shell and the end, and the
  largest live resources with their creating call. The probe's own replay target is
  excluded.
- **Startup:** the client's `load_trace` stages (ms since `application_run`) per
  cold/warm run, then a timeline from `perf_startup.cpp`: named main-thread points
  (`MAICORD_PERF_MARK`, compiled only into the probe), time to the entry point,
  memory at the first present, and, for the first cold and warm run, every DLL
  mapped before the first present, timed system calls made through the probe image's
  import tables, D3D11/DXGI/DirectComposition calls reached through their vtables
  (totals, then each call in order with its thread, interleaved with the named
  points), the first presents, and main-thread waits and module loads over 0.2 ms
  with their callers.
- **Always printed:** a per-member residency audit of `AccountUiRuntime` and a
  thread table (owner module, CPU, context switches, idle context switches, stack
  reservation and committed stack).

`-Owners` (`perf_owners.cpp`) patches the import tables of every other loaded module
(the GPU driver, system DLLs, the synthetic account) for the heap, `LocalAlloc`,
msvcrt/ucrtbase allocators, `HeapCreate` and `VirtualAlloc`. The end snapshot then
names each heap's creator and lists live heap bytes per allocating module, and for modules above 256 KiB those bytes by the client graphics call in
progress when they were allocated (device creation, shader creation, `Map`, `Present`,
other) and by block size. Allocations from ntdll itself, from before a module's imports
were bound, or through `GetProcAddress` stay unattributed.

`-Sampling` unwinds through `.pdata` (no symbols needed) and reports CPU-weighted
hot functions per phase: self, inclusive, and time in system DLLs charged to the
nearest client caller. The startup phase is sampled by wall clock so waits show.
Sampling perturbs the timing columns; use separate runs for timings.

A cold launch has no font catalog, caches or preferences. Phase tables use warm
launches. Timings are wall-clock measurements for comparison on one machine; the
test suite never asserts them.

## Probe windows

Benchmarks and probes run while someone uses the same desktop, so every window they
open stays out of the way (`probe_window.hpp`). MaiCordPerfProbe's client window,
MaiCordUiProbe (including the GPU, resize-pacing and render probes it hosts) and the
composer, logout and scoped-read UI probes are created with
`WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE` (no taskbar button, never activated by a
click) and shown with `SetWindowPos(HWND_BOTTOM, SWP_NOACTIVATE | SWP_SHOWWINDOW)`
instead of `ShowWindow`, so they never take focus and open behind every other
window. Frames use the same extended styles, so client sizes are unchanged.

The window stays shown (not minimized, cloaked or moved off the desktop): DWM keeps
composing a covered window, so `Present`, the frame-latency waitable, GPU timestamps
and the `idle_*` phases behave normally. A sleeping display is the exception: DWM
then composes only about every 250 ms, so frame times, wakeups and idle figures are
not comparable, and the resize-pacing probe reports a skip. Input is synthetic
(posted wheel messages, direct calls), never `SendInput`. Hidden probe windows
(message links, glyph cache) are never shown. Run window-backed probes sparingly:
once for a baseline and once to confirm a change.
