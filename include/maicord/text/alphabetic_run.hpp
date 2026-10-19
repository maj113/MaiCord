#pragma once
#include "maicord/text/latin_run.hpp"
namespace maicord {
// GSUB and metrics are borrowed from the parent plan. Script lookup roots (the
// parent's for Latin) and any GPOS subset the parent lacks live in the caller's state
// arena; parent and state must stay live. Horizontal en-US Latin, Greek, Cyrillic,
// Hebrew, Han, kana, Hangul, Armenian, Georgian, Ethiopic, Cherokee, Runic,
// unformatted Egyptian and simple_script.hpp; kana composition is left to the font. The internal tag jamo
// selects old Hangul's ordered feature stages instead of hang; script tag 0 selects
// the Common frontend.
struct NativeAlphabeticPlan {
    NativeRunPlanHandle run;
    ConstSlice<u16> substitution_roots,positioning_roots;
    u32 script_tag;
    bool independent;
    bool hebrew_preparation;
    bool allow_missing = false;
    const SfntGposProgram* positioning_program=nullptr;
};
static_assert(sizeof(NativeAlphabeticPlan)==56);
// State/scratch/output follow native_emoji_plan_prepare's disjoint-storage
// contract. Scratch rewinds on return; failure also rewinds state and clears
// output. Latin roots borrow the parent's already prepared typography mode.
SfntStatus native_alphabetic_plan_prepare(const NativeRunPlanStore&,NativeRunPlanHandle,u32 script_tag,bool independent,
    Arena* state,Arena* scratch,NativeAlphabeticPlan*,u32 stage_work=1000000) noexcept;
// One already resolved supported alphabetic/Common item. Shares Latin's
// storage and source-coverage contracts; Hebrew has its own character grammar.
// Direction is supplied by itemization. This does not resolve paragraph bidi.
SfntStatus native_alphabetic_run(const NativeRunPlanStore&,const NativeAlphabeticPlan&,Utf8View,float em,
    const NativeAsciiStorage&,NativeAsciiResult*,SfntShapeLimits limits={},bool require_source_coverage=false,bool force_shaping=false,bool rtl=false) noexcept;
}
