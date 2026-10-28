#include "maicord/text/font_fallback_range.hpp"
#include "sfnt_read.hpp"
#include "maicord/text/unicode.hpp"
namespace maicord {namespace {
struct FallbackOverride {u32 first,last_flags;};
#include "generated/fallback_compat_data.inc"
}
u8 native_font_fallback_class(u32 cp) noexcept {
    if(!unicode_is_scalar(cp))return native_fallback_invalid;
    if(cp<0x300)return 0;
    const auto* properties=unicode_properties(cp);
    if(properties->has(UnicodeFlag::variation_selector))return native_fallback_extending|native_fallback_selector;
    if(cp>=fallback_overrides[0].first){u32 lo=0,hi=sizeof(fallback_overrides)/sizeof(fallback_overrides[0]);
        while(lo<hi){const u32 mid=lo+(hi-lo)/2;if(fallback_overrides[mid].first<=cp)lo=mid+1;else hi=mid;}
        if(lo){const u32 value=fallback_overrides[lo-1].last_flags;if(cp<=(value&0x1fffffu))return static_cast<u8>(value>>21);}}
    const auto category=properties->general_category();
    return category==UnicodeGeneralCategory::mn||category==UnicodeGeneralCategory::mc||category==UnicodeGeneralCategory::me?native_fallback_extending:0;
}
SfntStatus native_font_fallback_range(Utf8View text,NativeFallbackSelect select,NativeFallbackCoverage coverage,void* context,
    NativeFallbackRangeResult* output,u32 work,u32* used) noexcept {
    const sfnt_read::WorkReport report{work,work,used};
    constexpr NativeFallbackRangeResult empty{{native_fallback_missing,native_fallback_missing},0,0};if(output)*output=empty;
    if(!output||(text.bytes&&!text.data)||!select||!coverage)return SfntStatus::invalid_input;
    NativeFallbackRangeResult result=empty;u8 previous=0;
    while(result.bytes<text.bytes){if(!work)return SfntStatus::resource_limit;--work;
        const auto cp=utf8_decode_one(text.data+result.bytes,text.bytes-result.bytes);if(!cp.valid)return SfntStatus::invalid_input;
        const u8 flags=native_font_fallback_class(cp.codepoint);bool retain=false;
        if(result.bytes){
            if((flags&native_fallback_selector)&&result.choice.font!=native_fallback_missing)retain=true;
            else if(result.choice.font!=native_fallback_missing&&((flags&native_fallback_extending)||(previous&native_fallback_joining))){
                if(!work)return SfntStatus::resource_limit;--work;
                const auto status=coverage(context,result.choice.font,cp.codepoint,&retain);if(status!=SfntStatus::ok)return status;}
        }
        if(!retain){if(!work)return SfntStatus::resource_limit;--work;NativeFallbackChoice choice{native_fallback_missing,native_fallback_missing};
            const auto status=select(context,cp.codepoint,&choice);if(status!=SfntStatus::ok)return status;
            const bool orphan_selector=result.bytes&&(flags&native_fallback_selector)&&result.choice.font==native_fallback_missing&&choice.policy!=result.choice.policy;
            if(!orphan_selector){if(result.bytes&&(choice.font!=result.choice.font||choice.policy!=result.choice.policy))break;result.choice=choice;}}
        result.bytes+=cp.width;result.units+=cp.codepoint>0xffff?2u:1u;previous=flags;
    }*output=result;return SfntStatus::ok;
}
}
