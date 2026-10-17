#pragma once
#include "maicord/text/arabic_plan.hpp"
namespace maicord {
// Modern Indic (dev2-model) feature stages and per-glyph form eligibility bits.
// Scheduling features does not classify syllables/forms; the script frontend sets
// glyph eligibility and checks font support.
enum class SfntDevanagariStage : u8 {
    localized,nukta,akhand,reph,rakar,pre_base,below_base,half,post_base,vatu,
    conjunct,final_reorder,presentation,count
};
enum SfntDevanagariForm : u8 {
    sfnt_deva_nukta=1,sfnt_deva_reph=2,sfnt_deva_below=4,sfnt_deva_half=8,
    sfnt_deva_pre=16,sfnt_deva_post=32,sfnt_deva_vatu=64,sfnt_deva_conjunct=128
};
}
