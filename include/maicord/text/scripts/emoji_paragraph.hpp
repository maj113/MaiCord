#pragma once
#include "maicord/text/scripts/emoji_line.hpp"
#include "maicord/text/scripts/latin_paragraph.hpp"
namespace maicord {
// One selected font/size and resolved horizontal Common profile. Visible spans
// shape independently between non-visual items, which keep one cluster per
// scalar and nondrawing source-map sentinels without glyph lookup. Tabs and
// hard breaks use the paragraph controls; soft hyphens and bidi formatting
// controls need layout policy and return unsupported_sequence. Script, bidi and
// font selection and mixed-font assembly belong to the caller. No tracking is
// added. Storage follows native_latin_paragraph_shape; errors clear output.
// paragraph_start: the input begins its paragraph (see native_emoji_run).
SfntStatus native_emoji_paragraph_shape(const NativeRunPlanStore&,const NativeEmojiPlan&,Utf8View,float em,
    const NativeAsciiParagraphStorage&,NativeLatinBoundaryStorage,NativeParagraphShape*,bool require_source_coverage=false,bool right_to_left=false,bool arabic_join_controls=false,bool paragraph_start=true) noexcept;
}
