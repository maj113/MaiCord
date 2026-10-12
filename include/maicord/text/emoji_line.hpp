#pragma once
#include "maicord/text/emoji_run.hpp"
#include "maicord/text/ascii_line.hpp"
namespace maicord {
// A selected horizontal Common/emoji run with source clusters and line
// geometry, using the Latin geometry stage and the position, hit and selection
// queries in ascii_line.hpp. The caller selects presentation/font/script items
// and handles non-visual spans such as tags. Missing glyphs map to glyph zero
// as in native_emoji_run; require_source_coverage rejects them so font fallback
// can pick another face. Empty input is one empty typographic line. A cluster
// counts as trailing whitespace by its first scalar. Emoji overrides carry no
// tracking or independent-letter typography. Errors clear the result; scratch
// and output bytes may change. paragraph_only: see native_latin_line.
SfntStatus native_emoji_line(const NativeRunPlanStore&,const NativeEmojiPlan&,Utf8View,
    float em_size,const NativeAsciiLineStorage&,NativeAsciiLineResult*,SfntShapeLimits limits={},bool require_source_coverage=false,bool right_to_left=false,bool arabic_join_controls=false,bool paragraph_only=false) noexcept;
}
