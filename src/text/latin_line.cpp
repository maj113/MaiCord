#include "maicord/text/latin_line.hpp"
#include "maicord/text/latin_run.hpp"
#include "maicord/text/alphabetic_run.hpp"
#include "unicode_line_geometry.hpp"
namespace maicord {
SfntStatus native_latin_line(const NativeRunPlanStore& store,NativeRunPlanHandle handle,Utf8View text,bool independent,float em,const NativeAsciiLineStorage& s,NativeAsciiLineResult* out,SfntShapeLimits limits,float trailing_spacing,const NativeAlphabeticPlan* profile,bool force_shaping,bool rtl,bool paragraph_only) noexcept {
    if(!out)return SfntStatus::invalid_input;*out={};if(s.clusters.count&&!s.clusters.data)return SfntStatus::invalid_input;
    if(profile&&(profile->run.slot!=handle.slot||profile->run.generation!=handle.generation||profile->independent!=independent))return SfntStatus::invalid_input;
    if(rtl&&!profile)return SfntStatus::unsupported_sequence;
    const bool common=profile&&!profile->script_tag;
    NativeAsciiLineResult line{};auto status=profile?native_alphabetic_run(store,*profile,text,em,s.run,&line.run,limits,!common&&!profile->allow_missing,force_shaping,rtl):native_latin_run(store,handle,text,independent,em,s.run,&line.run,limits,true,force_shaping);if(status!=SfntStatus::ok)return status;
    u32 spacing_bits=0;copy_bytes(&spacing_bits,&trailing_spacing,4);
    if(spacing_bits&0x7fffffffu){status=sfnt_trailing_spacing({s.run.placements.data,line.run.glyph_count},
        line.run.nominal_ranges?ConstSlice<u8>{}:ConstSlice<u8>{s.run.shaping.flags.data,line.run.glyph_count},trailing_spacing,limits.stage_work,rtl);
        if(status!=SfntStatus::ok)return status;}
    const auto* metrics=native_run_plan_metrics(store,handle);if(!metrics)return SfntStatus::invalid_input;
    // The shaper validates coverage before compacting glyph storage. Only an
    // unpaired variation selector may retain its source's missing-glyph box.
    status=native_unicode_line_geometry(*metrics,text,em,s,&line,paragraph_only);if(status==SfntStatus::ok)*out=line;return status;
}
}
