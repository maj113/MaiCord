# Projection-only text benchmark

`text_projection.cpp` copies the bounded retained fixture initialization from
`tests/text_emitter.cpp`: 192 source rows, real shell/navigation/rail/Friends
producers, 1,500 stored command slots and a 4,096-byte numeric text bank. It
relayouts and updates the retained scenes outside timing for **1280×900** and
**1280×8192** DIP cases. It performs no shaping, glyph construction, rasterization,
presentation, account access or network work. “Cold” means a new text projection
and consumption iteration, not a flushed hardware cache or newly allocated scene.

Four modes are reported independently for each viewport:

0. `stored_preflight`: project/validate/hash into the stored bank and cross an
   opaque metadata-observer boundary. Command writes remain materialized.
1. `emitter_preflight`: project/validate/hash with a null synchronous consumer,
   then cross the same opaque observer boundary.
2. `stored_cold_observe`: one stored projection plus a cheap ordered consumer.
3. `emitter_cold_observe`: null-consumer preflight followed by a second production
   projection into the same cheap synchronous consumer.

Exact ordered payload comparison occurs before timing. Each timed iteration
checks the production signature/count, and cold modes additionally check the
shared observer's ordered edge digest. The edge digest deliberately samples
little payload: it is not a second complete content hash. Seven samples use
rotated, alternating order; each mode has 16 warmups and 128 iterations per
sample. Output includes every sample, QPC frequency/ticks, min/median/max in
**nanoseconds per iteration**, visible command count, compiler/build metadata,
input and static storage budgets, and final checksum. An emitter cold iteration
has two projections; its time must not be described as a single projection.

## Root-owned wiring

In `cmake/benchmarks.cmake` (`MAICORD_BUILD_BENCHMARKS`), with shipping code
generation (`/GL`, LTCG) from the compile contract:

```cmake
maicord_program(MaiCordTextProjectionBenchmark ENTRY maicord_text_projection_benchmark_entry
    SOURCES tests/bench/text_projection.cpp tests/bench/text_projection_observer.cpp
    LIBS maicord_core maicord_platform maicord_ui maicord_ui_input maicord_model maicord_protocol
        maicord_app_core maicord_text kernel32 user32 crypt32 dwrite)
set_source_files_properties(tests/bench/text_projection_observer.cpp PROPERTIES COMPILE_OPTIONS "/GL-")
```

The listed library set follows the existing native UI benchmark dependencies
without its rendering/media implementation; root can trim archives that prove
unneeded or resolve any transitive source dependency during the owned build.
The benchmark source and observer are separate translation units. Verify the
observer's actual compiler invocation ends with **/GL-**: a function marked
`noinline` inside an LTCG-visible TU alone is insufficient to guarantee stored
record materialization. Do not unity-build the two sources.

Build and run only this target in the existing build tree. Redirect output and
keep binary/hash/compiler invocation metadata outside the checkout; no CTest
registration or production target dependency is needed. Turn the existing
benchmark option off again for the ordinary build. No result or speed claim is
provided before root executes it. Results apply to these two fixture workloads,
not native frame latency or general client performance.

The normal build has the benchmark option OFF; the standalone measurement binary
and logs remain external.
