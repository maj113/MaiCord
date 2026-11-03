# Projection-only text benchmark

`text_projection.cpp` copies the bounded retained fixture initialization from
`tests/ui/text/emitter.cpp`: 192 source rows, real shell/navigation/rail/Friends
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

## Build

`cmake/benchmarks.cmake` (`MAICORD_BUILD_BENCHMARKS`) builds `MaiCordTextProjectionBenchmark` with shipping code
generation (`/GL`, LTCG); no CTest registration. The benchmark and its observer are separate translation units and
the observer is excluded from LTCG: a `noinline` function inside an LTCG-visible TU alone does not guarantee stored
record materialization. Do not unity-build the two sources. Results apply to these two fixture workloads, not
native frame latency or general client performance.
