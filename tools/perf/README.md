# Offline performance benchmark

```
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\perf\run_perf.ps1 [-Runs 2] [-WarmOnly] [-Raw] [-Sampling]
```

`run_perf.ps1` builds `MaiCordPerfProbe.exe` and `MaiCordPerfAccount.dll` through
`tools\build.ps1 -PerfProbe -SkipClient` in the single Release cache, runs the probe
against a temporary `LOCALAPPDATA`, prints a table and removes the probe, its objects
and the profiles. It never needs or touches a running client, a real account or the
network.

- **MaiCordPerfProbe** is the shipping client source set built with
  `MAICORD_PERF_PROBE` and shipping code generation (`/GL`, LTCG). The define only
  swaps the account module file name and skips desktop integration,
  notifications, updates and instance forwarding (`MAICORD_ISOLATED_MAIN`).
- **MaiCordPerfAccount.dll** implements the account module ABI offline: a
  deterministic READY (100 guilds, ~2,600 channels, 2,000 users, 300 friends,
  180 DMs) projected by the production Gateway routes, generated message
  history with mixed content (markdown, code, embeds, replies, mentions,
  reactions, stickers, PNG/GIF/WebP attachments, animated emoji), member list
  syncs for a 48,000-member guild, member chunks, generated media frames and
  typing/presence/message churn. Every REST submission is answered.

Each launch runs these phases in order (one window, shown without focus and
behind every other window, see [Probe windows](#probe-windows)):
`startup` (process start to restored shell), `history`, `idle_static`
(text-only channel), `scroll` (wheel up through history, then down),
`switch` (24 channel switches), `member_list` (wheel through the member
list), `churn` (8 typing, 40 presence, 2 messages per second), `settings`
(open, six pages, close), `idle_anim` (animated content visible),
`idle_final` and `idle_hidden` (minimized; the end memory snapshot is taken
before minimizing). An idle client may not wake on its own, so the probe arms
one timer for its next deadline (phase end or scripted input) and posts a
single `WM_NULL` then; it counts as one message wake. The window is never
activated, which the client treats like a focused window (it pauses media
animations only after a real deactivation).

Per phase it reports wall time, presents, main-thread and process CPU (TSC
cycles), main-thread CPU per presented frame (p50/p95/p99), main-loop wakes,
context switches, client `VirtualAlloc`/`VirtualFree`/heap calls (import hooks
on the probe image only), page faults, private working set, private bytes,
threads and handles, plus a retained-work table: Markdown parser runs against
cache hits, content text streams built (and how many kept the shell's
glyphs) and paint streams built, and an upload table: paint, glyph and media
instance bytes written to the GPU per presented frame, member list rows
resolved for media and shell segments replayed instead of built.

A GPU table counts immediate-context work per painted frame through the
context's vtable (state binds, scissor rectangles and those repeating the
bound one, buffer maps, copies, compute dispatches and binds; draws come from
the renderer's own count). Timestamps around a presented frame would include
the swap chain's wait for its back buffer, so after each present the probe
draws the same retained stream again, with the presented damage, into an
offscreen target of the back buffer's format and times that replay: GPU
microseconds for full and damage-clipped frames, and the replay's main-thread
CPU (draw submission alone, excluded from the CPU columns). Memory
snapshots (shell, after scroll, end) attribute committed, resident and
private-resident bytes to client `VirtualAlloc` regions (aggregated by
allocation call stack and resolved through `MaiCordPerfProbe.map`), heaps,
stacks, write-combined and other private memory (driver), images by module,
mapped files and sections; synthetic module and probe memory are reported
separately.

The startup table adds the client's `load_trace` stages (ms since
`application_run`) per cold/warm run, then a startup timeline from
`perf_startup.cpp`: named main-thread points (`MAICORD_PERF_MARK`, compiled
only into the probe) as ms since process creation with main-thread CPU, the
time to the entry point, memory at the first present, and for the first cold
and warm run every DLL mapped before the first present (with its thread),
timed system calls made through the probe image's import tables (window,
`D3D11CreateDevice`, DirectComposition, COM, module loads, waits), D3D11
device, DXGI factory, swap chain and DirectComposition calls reached through
their vtables (main thread and worker separately), the first presents, and
main-thread waits and module loads over 0.2 ms with their callers. `-Sampling` adds a 1 kHz in-process
sampling profiler of the main thread (unwound through `.pdata`, no symbols
needed): CPU-weighted hot functions per phase (self, inclusive, and time in
system DLLs charged to the nearest client caller); the startup phase is
sampled by wall clock so waits show. It perturbs the timing columns, so use
separate runs for timings. A per-member residency audit of `AccountUiRuntime`
and a thread table (owner module, CPU, context switches, idle context
switches, stack reservation and committed stack) are always printed.

Cold runs use a fresh profile (no font catalog or caches); warm runs repeat
the same profile. Phase tables use warm runs. Timings are wall-clock
measurements for comparison on one machine; the regular test suite never
asserts them.

## Probe windows

Benchmarks and probes run while someone uses the same desktop, so every window
they open stays out of the way (`probe_window.hpp`): MaiCordPerfProbe's client
window, MaiCordUiProbe (including the GPU, resize-pacing and render probes it
hosts) and the composer, logout and scoped-read UI probes are created with
`WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE` (no taskbar button, never activated by a
click) and shown with `SetWindowPos(HWND_BOTTOM, SWP_NOACTIVATE |
SWP_SHOWWINDOW)` instead of `ShowWindow`: they never take focus or come to the
foreground, and open at the bottom of the z-order behind every other window.
Frames are sized with the same extended styles, so client sizes are unchanged.

The window stays shown (not minimized, cloaked or moved off the desktop): DWM
keeps composing a covered window, so `Present`, the frame-latency waitable,
GPU timestamps and the `idle_*` phases behave as before. The exception is a
sleeping display: DWM then composes only every ~250 ms, so vsync-paced
frame times, wakeups and idle figures are not comparable, and the
resize-pacing probe reports a skip. Input is synthetic
(posted wheel messages, direct calls), never `SendInput`. Hidden probe windows
(message links, glyph cache) are never shown at all. Run window-backed probes
sparingly: once for a baseline and once to confirm a change.
