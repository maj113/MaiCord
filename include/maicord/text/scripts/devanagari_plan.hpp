#pragma once
#include "maicord/text/scripts/arabic_plan.hpp"
namespace maicord {
// Modern Indic feature stages, in HarfBuzz's order for the Microsoft Indic specs: locl and ccmp, one
// stage per basic feature (nukt, akhn, rphf, rkrf, pref, blwf, abvf, half, pstf, vatu, cjct), the final
// reordering, then the presentation features with calt, clig, rlig and rclt in one stage. Planned GSUB
// steps come in this order; their stage field is 0 before the final reordering and 1 after it, the
// only two points where the shaper reorders.
enum class SfntDevanagariStage : u8 {
    localized,nukta,akhand,reph,rakar,pre_base,below_base,above_base,half,post_base,vatu,
    conjunct,final_reorder,presentation,count
};
// Per-glyph eligibility of the masked features (SfntGsubGlyph::reserved): init, rphf, blwf, half, pref,
// and abvf/pstf (post). vatu is only a font-form probe kind: the vatu feature applies to every glyph.
enum SfntDevanagariForm : u8 {
    sfnt_deva_init=1,sfnt_deva_reph=2,sfnt_deva_below=4,sfnt_deva_half=8,
    sfnt_deva_pre=16,sfnt_deva_post=32,sfnt_deva_vatu=64
};
}
