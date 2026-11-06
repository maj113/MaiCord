# Performance benchmarks

Opt-in micro-benchmarks (`benchmarks/`, `tests/bench`, targets in `cmake/benchmarks.cmake`) run with `tools\benchmark_<name>.ps1` where
a driver parses their output (text, glyph, compression, event pump, ...), and with `tools\benchmark.ps1 -Target <MaiCord...Benchmark>
[-Samples N]` for every other target, which prints the `name=value` lines the benchmark writes. The Linux size policy's measurements
(`cmake/size_policy.cmake`) used these sources built for Linux.

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
named startup points (`MAICORD_PERF_MARK` in `main.cpp`, `app/shell/window.cpp`,
`app/shell/frame_presentation.cpp` and `app/shell/shutdown_metrics.cpp`), in ms since process creation.
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
background effect), `theme` (two built-in theme switches 0.8 s apart, to Light and
back: the frames a theme transition draws), `call_open`, `call_30fps` and `call_5fps` (a synthetic call
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
  After each present the probe draws the same retained stream again, with the
  presented damage, into an offscreen target of the back buffer's format, so the
  main-thread cost of submitting one draw stream is measured apart from `Present` and
  its wait for the back buffer. The report lists the replayed damage area and that
  submission CPU; the replay's main-thread CPU is excluded from the CPU columns.
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
composing a covered window, so `Present`, the frame-latency waitable
and the `idle_*` phases behave normally. A sleeping display is the exception: DWM
then composes only about every 250 ms, so frame times, wakeups and idle figures are
not comparable, and the resize-pacing probe reports a skip. Input is synthetic
(posted wheel messages, direct calls), never `SendInput`. Hidden probe windows
(message links, glyph cache) are never shown. Run window-backed probes sparingly:
once for a baseline and once to confirm a change.

## Chat list benchmark

`tools\benchmark_chat_list.ps1 [-Sizes 100,10000,1000000,10000000] [-Reps 3] [-Seconds 5]` builds `MaiCordChatListBenchmark`
(`benchmarks/chat_list_benchmark.cpp`, `cmake/benchmarks.cmake`), runs it once per process, size and repetition, and writes
`results.json` (medians, with the min and max of each field) and `results.md` (BUI's columns) to `%TEMP%\maicord-chat-list-bench`.
It answers the questions of BUI's `bench_list_gpui` (blenderui-vlist-gpui `examples/bench_list_gpui.rs`, runner `bench/run.mjs`) the
way that harness asks them. Do not run it with `MaiCord.exe` open, and leave the machine alone while it runs.

**What BUI measures.** One process runs one library at one size and prints one JSON line; a watchdog thread ends it, still printing
a line that says what failed and how far it got, when its working set grows by more than `--cap-mb` (4096) over the baseline taken
before anything was built or runs longer than `--limit-seconds` (150). The runner repeats each library and size three times and
takes, per field, the median (sorted, index `floor((n-1)/2)`) of the runs that finished; a library that fails at one size is
skipped at the next.

- *Data*: `blenderui_vlist::synth::message(i)` is a pure function of `i`: author `h % 9` where `h = mix(i)`; time
  `1_700_000_000 + 37 i + mix(i ^ 7) % 30` s (so 8 to 66 s apart); a body of 3 to 12 words (10/16 of the messages), 14 to 43 (5/16) or
  60 to 199 (1/16) drawn from 24 lowercase words by `x = mix(x + k)`; an image of 120 to 219 px on 1 in 40. A message starts a run
  (author name, avatar, time) when the author changes or after 300 s, and a day (UTC) starts with a date divider; the first message
  has both. Up to 1,000,000 messages are an in-memory vector, 10,000,000 a file-backed log that every library reads per row.
- *Window*: a 740x640 gpui window holding a 700x600 list, 14 px text on a 19 px line, a 32 px avatar column, 10 px padding.
- *Stages* (continuous frames, which BUI requests itself, so they run at the display's refresh rate): opened at the newest message;
  300 ms later the list jumps to item n/2; scroll 57 px per frame from there (2 s warm-up, 5 s recorded); a repaint of a fixed
  position (1 s warm-up, 2.5 s recorded); 40 jumps to the items an xorshift64 generator (seed `0x9E3779B97F4A7C15`, shifts 13/7/17)
  picks, 30 ms apart.
- *Metrics*: `setup_ms` builds the library's state for n items (the height index; the `SumTree`; `virtual_list`: measuring every
  message). `first_frame_ms` is the time from process start to the first painted frame, minus the time the data took to create
  (it includes a 300 ms wait for BUI's TSC calibration thread, which starts with the process: with small data it is part of the
  figure). `frame_ms_*`: from the start of the window's `render` to the end of `paint` (a canvas element painted last) on the main
  thread, so layout, element building and paint into gpui's scene, with GPU submission and the wait for the display left out.
  `cpu_ms_per_frame`: process CPU cycles of all threads (`QueryProcessCycleTime`, converted with a calibrated TSC rate) over the
  recorded frames. `main_allocs_per_frame`: calls to the global allocator on the UI thread (alloc and realloc) per frame.
  `jump_ms`: the dispatch of the jump plus the render-to-paint of the frame that lands. `ram_above_data_mb`: working set after the
  scroll stage minus the baseline; `peak_above_data_mb` the peak at the end.

**What MaiCord runs.** The real timeline: `message_view_runtime` (message store, variable-height index, timeline), the message scene
and its layout, `message_text_coordinator_converge` (shaping and wrapping with MaiCord's own text system), the renderer's paint
stream and glyph pipeline, a swap chain, in a real window sized so the timeline is exactly 700x600 dips (the shell around it is the
server rail's 64 dips and the title bar's 48; DPI is the system's). The generator is a bit-for-bit port of `synth.rs`; messages
reach the model as Discord's JSON, with the same ids, authors ("User n"), timestamps and text.

A channel of n messages is not held by the client: the timeline keeps `kMessageViewItemCapacity` (256) messages and the rest is
history, fetched in pages of `kMessageHistoryRequestLimit` (25). The harness plays the server: `GET .../messages` pages (latest,
newer than a cursor, around a message) are written from the generator as the API's JSON, decoded by `decode_message_history_page`,
and applied by `message_timeline_apply_history_page` (which evicts from the far edge); a jump to a message that is not resident
resets the window with an around page (`message_history_jump_edges` decides what is left to page) and scrolls to it, and scrolling
near the resident bottom of a window that is not the live edge pages forward, as `scroll_message_view_by` does. So the sizes are
all reachable, with the same code path for 100 and 10,000,000, and the figures that depend on n are the ones that include a page.

What differs from BUI, deliberately or not:

- *Frame time* spans the scroll tick, layout and shaping (`message_text_coordinator_converge`), the paint stream, the draw submission
  and the `Present` call, after the frame slot is acquired (`gpu_frame_wait`, as the client's loop does, so the wait for the display
  is outside the figure); BUI's stops before gpui submits its scene to the GPU. `layout_ms_p50`, `text_ms_p50` and `paint_ms_p50`
  split it (`paint` includes the submission).
- *Retained rendering*: MaiCord keeps its paint stream and shaped glyphs between frames and moves them on a scroll; gpui rebuilds
  everything each frame. The repaint stage drops the retained presentation first, so it does the paint-stream build BUI does (layout
  and shaped text stay cached: MaiCord's text cache survives between frames by design).
- *Clamped scrolling*: at the end of the channel BUI keeps repainting the clamped position; there is nothing to draw here (the
  renderer skips an unchanged frame), so a scroll that reaches the live edge starts over from the middle and that frame is not
  recorded. At 100 messages BUI's recorded frames are all clamped repaints; MaiCord's are all scrolling.
- *Paging*: no network between the harness and the model (a real page takes a round trip and happens off the UI thread; its decode
  and apply happen on it, which this includes, in the frame that applies it).
- *Images* (1 in 40 messages): BUI draws a grey box; MaiCord has no attachment to show, so the generator's image height is not
  carried (the text, authors and timing are).
- *Allocations*: MaiCord has no malloc on Windows; the benchmark's imports of `HeapAlloc`, `HeapReAlloc` and `VirtualAlloc` (which
  the arenas and `heap_allocate` call directly) are counted on the UI thread. Drivers' allocations are not, as BUI's global
  allocator does not see its drivers'.
- *First frame*: includes window creation, the renderer's device and glyph pipeline, the timeline owners and the first page; it does
  not include BUI's calibration wait (the TSC rate is measured after the first frame here).
- *Setup* is the creation of the timeline's owners and the scene, independent of n (the data is behind the history pages).
  *RAM above data*: nothing to subtract (the baseline is the start of `run`).

Run-to-run spread is in `results.json` as `<field>__min` and `<field>__max`.

Fields BUI has no counterpart for: `scroll_full_*` (the scroll stage again with the scroll-only shortcut off, so the message text
is projected again every frame, as a resize or a new page does; the figure to set beside a library that rebuilds its frame each
time), `layout_ms_p50`/`text_ms_p50`/`paint_ms_p50` (and the `text` and `paint` tails) splitting the scroll frame, `page_decode_ms_mean`
and `page_apply_ms_mean` (per history page: `decode_message_history_page`, then the store and index update), `pages`,
`pages_evicted`, `jump_fetches`, `resident_messages`, `scene_rows`, `text_quads` and `paint_quads` (what was on screen at the end,
a check that frames are not empty), `still_draw_calls` (the draw calls of the last forced repaint; on OpenGL ES a draw also ends
where consecutive quads sample different atlas pages), `view_*_dips` and `dpi`. The time the harness spends writing a page's JSON (it plays the
server) is left out of every frame time, jump time and the CPU figure.

`tools\benchmark_chat_list.ps1 -Profile -Sizes 1000000 -Seconds 25` runs one process with a sampler: a thread reads the UI thread's
instruction pointer every 200 us (a spinning core; each sample suspends the thread, so the frame figures of a profiled run are not
results) and `profile_chat_list.py` resolves the samples against the linker map (`/MAP`, public symbols of the one LTO object; a
static or inlined helper is charged to the public symbol before it) into the share of busy time per module and per function. The
one ntdll address holding most samples is the wait for the display and counts as idle.

`cpu_frame_ms_*` and `gpu_submit_present_ms_*` split each scroll frame at the first call of the present into the immediate context
(a draw, map, copy, update, clear or dispatch): before it, layout, text and the paint stream; after it, submission and `Present`.
The mark comes from 11 slots of the context's vtable patched in the process (`--marks 0` leaves it alone and drops the fields). The
table is the runtime's and shared by every caller, so each hook takes exactly its method's parameters (derived from the method's
declaration) and forwards them unchanged. `atlas_pool_kib` is the tile-pool memory of the paged atlas surfaces at the end.

`tools\benchmark_chat_list.ps1 -Warp [-Reps 1 -Seconds 2]` is a validation run, not a measurement: every process runs on WARP with
the Direct3D 11 debug layer (`--warp 1`: `tests/support/warp_device.hpp` points the image's `D3D11CreateDevice` at WARP and
drains the debug layer's messages every frame), never on the hardware adapter, and prints one `D3D11` line per message kind. A
process fails (exit 40) on any message but `NULL_TILE_MAPPING_ACCESS_WARNING` (`src/render/README.md`) or when the debug layer is
missing (Windows' Graphics Tools optional feature). It may run beside an open client.

**Linux.** `tools/build_linux.sh --benchmarks --keep` also builds `MaiCordChatListBenchmark` (`cmake/linux_benchmarks.cmake`: the same
source, the Wayland window service and the GLES backend, the message runtime's units compiled into the program), and
`tools/benchmark_chat_list.py --binary <build>/MaiCordChatListBenchmark` runs it with the same medians and columns. It needs a
Wayland compositor and shows its window there: run it under headless sway (as `tools/test_gpu_surface.sh` starts it; the output
must be at least 764x648 and the window floating at that size) so no desktop is involved. On WSL the GPU is Mesa's d3d12 driver, as
the client chooses it; `GALLIUM_DRIVER=llvmpipe` forces software rendering. What differs from Windows: the loop is paced only by
what the compositor and the swap do (it ran at 800+ frames a second on WSLg's compositor and about 100 under llvmpipe), not by a
display's refresh, so `fps` is the loop's rate and `paint_ms` includes whatever the GL driver does in the swap; the CPU figure is
the process's CPU time (`CLOCK_PROCESS_CPUTIME_ID`);
allocations are the image's `malloc`/`calloc`/`realloc` calls on the UI thread (`--wrap`; the arenas' page mappings are raw
system calls and are not counted); memory is `VmRSS` and `VmHWM` of `/proc/self/status`; `--profile` is Windows only (use `perf`
on a `--symbols` build). The benchmark opens the client's log (`XDG_STATE_HOME`), where a renderer or window failure is reported.
