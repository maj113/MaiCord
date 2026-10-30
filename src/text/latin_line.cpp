#include "maicord/core/memory.hpp"
#include "maicord/text/latin_run.hpp"
#include "maicord/text/alphabetic_run.hpp"
#include "unicode_line_geometry.hpp"
namespace maicord {
SfntStatus native_latin_line(const NativeRunPlanStore& store,NativeRunPlanHandle handle,Utf8View text,bool independent,float em,const NativeAsciiLineStorage& s,NativeAsciiLineResult* out,SfntShapeLimits limits,float trailing_spacing,const NativeAlphabeticPlan* profile,bool force_shaping,bool rtl,bool paragraph_only) noexcept {
    if(!out)return SfntStatus::invalid_input;reset_value(out);
    if(profile&&(profile->run.slot!=handle.slot||profile->run.generation!=handle.generation||profile->independent!=independent))return SfntStatus::invalid_input;
    if(rtl&&!profile)return SfntStatus::unsupported_sequence;
    const bool common=profile&&!profile->script_tag;
    NativeAsciiLineResult line{};const auto status=profile?native_alphabetic_run(store,*profile,text,em,s.run,&line.run,limits,!common&&!profile->allow_missing,force_shaping,rtl):native_latin_run(store,handle,text,independent,em,s.run,&line.run,limits,true,force_shaping);if(status!=SfntStatus::ok)return status;
    // The shaper validates coverage before compacting glyph storage. Only an
    // unpaired variation selector may retain its source's missing-glyph box.
    return native_finish_line(store,handle,text,em,s,line,out,limits,trailing_spacing,rtl,paragraph_only?NativeLineText::paragraph:NativeLineText::unicode);
}
}
