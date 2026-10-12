#pragma once
#include "maicord/text/indic_plan.hpp"
namespace maicord {
// Raw probes use only reph/half/pre/below/post bits. In the resolved form map,
// this otherwise unused bit records a default positional class. It is not a
// GSUB eligibility mask and never changes the standalone raw form queries.
constexpr u8 indic_default_position=sfnt_deva_vatu;
// Private expanded-cluster header bit, never a raw/font-wide form property.
// The first local form byte carries the model even if this cluster contains
// none of the consonants assigned default positions. It is not eligibility.
constexpr u8 indic_default_model=128;

// Raw form probes remain font observations. Malayalam's prepared positional
// classes use defaults only when no consonant has a post-base form. Resolve
// this once per font plan, not in the scalar scanner or each decode window.
inline void indic_form_defaults(SfntIndicProfile profile,u8 (&forms)[128]) noexcept {
    if(profile!=SfntIndicProfile::malayalam)return;
    for(u8 form:forms)if(form&sfnt_deva_post)return;
    forms[0x2f]|=sfnt_deva_post|indic_default_position;forms[0x30]|=sfnt_deva_post|indic_default_position;forms[0x35]|=sfnt_deva_post|indic_default_position;
    forms[0x32]|=sfnt_deva_below|indic_default_position;
}
}
