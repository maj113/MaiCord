#include "maicord/text/font_fallback_range.hpp"
namespace maicord {namespace {u8 band(u32 cp) noexcept{return cp<0xe000?0u:cp<0xf900?1u:2u;}}
SfntStatus native_font_fallback_routed_range(Utf8View text,NativeFallbackIndexSelect route,NativeFallbackIndexSelect select,
    NativeFallbackCoverage coverage,void* context,NativeFallbackRangeResult* output) noexcept {
    NativeFallbackRangeResult result{{native_fallback_missing,native_fallback_missing},0,0};u8 previous_scalar=0,previous_route=0,initial_band=0;bool unpaired=false;
    while(result.bytes<text.bytes){
        const auto cp=utf8_decode_one(text.data+result.bytes,text.bytes-result.bytes);if(!cp.valid)return SfntStatus::invalid_input;
        const u8 flags=native_font_fallback_class(cp.codepoint);
        const bool attached=unpaired&&(flags&native_fallback_selector);
        if(!result.bytes){
            auto status=route(context,cp.codepoint,&result.choice.policy);if(status!=SfntStatus::ok)return status;
            status=select(context,cp.codepoint,&result.choice.font);if(status!=SfntStatus::ok)return status;
            initial_band=band(cp.codepoint);
        }else{bool probed=false,covered=false;
            if(!attached&&!((flags&native_fallback_selector)&&result.choice.font!=native_fallback_missing)){u32 policy=native_fallback_missing;
                const auto status=route(context,cp.codepoint,&policy);if(status!=SfntStatus::ok)return status;
                if(policy!=result.choice.policy||band(cp.codepoint)!=initial_band){
                    if(result.choice.font==native_fallback_missing||!((flags&native_fallback_extending)||(previous_route&native_fallback_joining)))break;
                    const auto probe=coverage(context,result.choice.font,cp.codepoint,&covered);if(probe!=SfntStatus::ok)return probe;probed=true;if(!covered)break;
                }
            }
            bool retain=(flags&native_fallback_selector)&&result.choice.font!=native_fallback_missing;
            if(!retain&&result.choice.font!=native_fallback_missing&&((flags&native_fallback_extending)||(previous_scalar&native_fallback_joining))){
                if(!probed){const auto status=coverage(context,result.choice.font,cp.codepoint,&covered);if(status!=SfntStatus::ok)return status;}
                retain=covered;
            }
            if(!retain){u32 font=native_fallback_missing;
                const auto status=select(context,cp.codepoint,&font);if(status!=SfntStatus::ok)return status;if(font!=result.choice.font)break;}
        }
        result.bytes+=cp.width;result.units+=cp.codepoint>0xffff?2u:1u;previous_scalar=flags;if(!attached)previous_route=flags;unpaired=!attached;
    }*output=result;return SfntStatus::ok;
}
}
