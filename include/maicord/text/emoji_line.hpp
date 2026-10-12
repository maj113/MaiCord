#pragma once
#include "maicord/text/emoji_run.hpp"
#include "maicord/text/ascii_line.hpp"
namespace maicord {
// A prepared, already selected horizontal Common/emoji run, with source
// clusters and line geometry. Reuses the Latin geometry stage and the
// source-position, point-hit and selection queries in ascii_line.hpp. No extra
// storage, allocation, OS service or external library. The caller still selects
// presentation/font/script items and handles non-visual spans such as tags.
// Missing glyphs use the font's glyph-zero metrics, as in native_emoji_run.
// Callers can require original-source coverage while resolving font fallback.
// Empty input is one empty typographic line. Trailing whitespace follows each
// cluster's first scalar.
// Emoji font overrides carry no tracking or independent-letter typography.
// Errors clear the result; private scratch/output bytes can change on failure.
// paragraph_only omits standalone vertical/caret/trailing geometry; only the
// paragraph collector uses that mode after shaping validates source clusters.
SfntStatus native_emoji_line(const NativeRunPlanStore&,const NativeEmojiPlan&,Utf8View,
    float em_size,const NativeAsciiLineStorage&,NativeAsciiLineResult*,SfntShapeLimits limits={},bool require_source_coverage=false,bool right_to_left=false,bool arabic_join_controls=false,bool paragraph_only=false) noexcept;
}
