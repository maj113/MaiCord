# Offline performance benchmark

```
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\perf\run_perf.ps1 [-Runs 2] [-WarmOnly] [-Raw] [-Sampling] [-Debugger]
```

`run_perf.ps1` builds `MaiCordPerfProbe.exe` and `MaiCordPerfAccount.dll` through
`tools\build.ps1 -PerfProbe -SkipClient` in the single Release cache, runs the probe
against a temporary `LOCALAPPDATA`, prints a table, and removes the probe, its objects
and the profiles. It never needs or touches a running client, a real account or the
network.

| Switch | Effect |
| --- | --- |
| `-Runs N` | Repeats (1-10, default 2): a cold launch on a fresh profile, then a warm launch on the same profile. Tables show medians. |
| `-WarmOnly` | Skip the cold launches; all runs reuse one profile. |
| `-Raw` | Also print each probe's raw report. |
| `-Sampling` | 1 kHz in-process sampling of the main thread (see below). |
| `-Debugger` | Run each launch under `cdb` and print the faulting stack. Diagnosis only. |

- **MaiCordPerfProbe** is the shipping client source set built with
  `MAICORD_PERF_PROBE` and shipping code generation (`/GL`, LTCG). The define swaps
  the account module file name and skips desktop integration, notifications,
  updates and instance forwarding (`MAICORD_ISOLATED_MAIN`).
- **MaiCordPerfAccount.dll** implements the account module ABI offline: a
  deterministic READY (100 guilds, ~2,600 channels, 2,000 users, 300 friends,
  180 DMs) projected by the production Gateway routes, generated message history
  with mixed content, member list syncs for a 48,000-member guild, generated media
  frames, and typing/presence/message churn. Every REST submission is answered.

## Phases

Each launch runs one window through these phases, in order: `startup` (process
start to restored shell), `history`, `idle_static` (text-only channel), `scroll`,
`switch` (channel switches), `member_list`, `churn`, `settings`, `idle_anim`
(animated content visible), `idle_final` and `idle_hidden` (minimized; the end
memory snapshot is taken before minimizing).

An idle client may not wake on its own, so the probe arms one timer for its next
deadline (phase end or scripted input) and posts a single `WM_NULL`, which counts
as one message wake. The window is never activated; the client treats that like a
focused window (it pauses media animations only after a real deactivation).

## Report

- **Per phase:** wall time, presents, main-thread and process CPU (TSC cycles),
  main-thread CPU per presented frame (p50/p95/p99), main-loop wakes, context
  switches, client `VirtualAlloc`/`VirtualFree`/heap calls (import hooks on the
  probe image only), page faults, private working set, private bytes, threads and
  handles.
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
  files. Synthetic module and probe memory are reported separately.
- **Startup:** the client's `load_trace` stages (ms since `application_run`) per
  cold/warm run, then a timeline from `perf_startup.cpp`: named main-thread points
  (`MAICORD_PERF_MARK`, compiled only into the probe), time to the entry point,
  memory at the first present, and, for the first cold and warm run, every DLL
  mapped before the first present, timed system calls made through the probe image's
  import tables, D3D11/DXGI/DirectComposition calls reached through their vtables,
  the first presents, and main-thread waits and module loads over 0.2 ms with their
  callers.
- **Always printed:** a per-member residency audit of `AccountUiRuntime` and a
  thread table (owner module, CPU, context switches, idle context switches, stack
  reservation and committed stack).

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
