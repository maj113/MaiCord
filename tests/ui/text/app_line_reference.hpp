// Committed references of the native app line driver (MaiCordNativeAppLineTests), written by
// tools/font_reference/text_reference.py into tests/generated/app_line_reference.inc.
//
// Two references, both independent of the platform's own text stack:
//  - Layout (engine-defined: positions, lines, hits, decorations, editor geometry): every observation feeds a digest per
//    group of 64 cases, with fonts named by their table-directory digest. Each group also records the fonts it used:
//    those of its observed runs, or, when it observed no runs, every font the registered sessions have shaped with.
//  - Shaping (HarfBuzz): each native run is also shaped by HarfBuzz with the run's font, text, direction, script,
//    features and, for a joining run, context, and compared in logical order at the engine's cluster granularity (one
//    cluster per syllable; glyph IDs and positions exact). The reference keeps a digest per run of the shape found
//    equal to HarfBuzz's. A run that equalled HarfBuzz when recorded must keep its shape; a run that differed is
//    marked divergent and only counted (text_reference.py lists them by script), so no unverified output is pinned.
// A system font (under a Windows font folder) whose file the profile never recorded is foreign: the groups and runs
// that use it are skipped and counted, the rest are still checked, and a process with nothing left to check exits 77
// (CTest skip). The repository's own fonts must always match. MAICORD_TEXT_REFERENCE_RECORD=<directory> records
// instead of comparing; MAICORD_TEXT_REFERENCE_FOREIGN=1 treats every system font as foreign (the coverage another
// machine keeps); MAICORD_TEXT_REFERENCE_SUMMARY=<file> appends "<checked> <skipped>" group counts per process.
#pragma once
#include "maicord/ui/text/text.hpp"
namespace maicord::native_app_line {
// Before the first observation: selects the profile and the mode.
bool reference_begin() noexcept;
// After the last one: writes the record, or checks that the recorded cases and groups were all seen. Returns the
// process status (0, 1, or 77 when every group used a foreign font or the profile was never recorded).
u32 reference_end(bool ok) noexcept;
// Layout digest of the current case group (`cases` / 64).
void observe_bytes(const void* data, u32 bytes) noexcept;
template<class T> void observe_value(const T& value) noexcept {observe_bytes(&value, sizeof(value));}
void observe_measure(const TextMeasureResult& value) noexcept;
// The shaped output (runs by font identity, not handle) plus the HarfBuzz check of each native run. `source` is the
// text that was shaped; `independent` disables ligatures and kerning as the system's independent_letters does,
// `tracking` (em fraction) and simulated runs leave only the glyph and cluster structure comparable.
void observe_shape(const ShapedText& text, Utf8View source, bool independent, float tracking) noexcept;
// Leaves the HarfBuzz check out of the following shapes (generated corpora whose runs the other profiles cover).
void reference_shaping(bool enabled) noexcept;
// A long-lived font session whose shaping fonts groups without observed runs depend on.
void reference_session(NativeFontSession* session) noexcept;
// Closes the open group and forgets the sessions: call before releasing them.
void reference_release_sessions() noexcept;
} // namespace maicord::native_app_line
