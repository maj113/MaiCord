#pragma once
#include "maicord/text/emoji_line.hpp"
#include "maicord/text/latin_paragraph.hpp"
namespace maicord {
// One selected font/size and resolved horizontal Common profile. Visible
// spans shape independently across non-visual items; those retain one cluster
// per scalar and nondrawing source-map sentinels without glyph/font lookup.
// Tabs and hard breaks use the existing paragraph controls. Soft hyphens and
// bidi formatting controls require their respective layout policies and decline. General script/bidi/font
// selection and mixed-font assembly belong to the caller. No tracking is added.
// Caller-owned storage follows native_latin_paragraph_shape; errors clear output.
SfntStatus native_emoji_paragraph_shape(const NativeRunPlanStore&,const NativeEmojiPlan&,Utf8View,float em,
    const NativeAsciiParagraphStorage&,NativeLatinBoundaryStorage,NativeParagraphShape*,bool require_source_coverage=false,bool right_to_left=false,bool arabic_join_controls=false) noexcept;
}
