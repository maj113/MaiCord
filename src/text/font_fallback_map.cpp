#include "maicord/text/font_fallback_range.hpp"
namespace maicord {namespace {
struct Mapping {
    NativeFallbackIndexSelect base,route,fallback;NativeFallbackCoverage coverage;void* context;
    u32 first;bool pending=true;
};
SfntStatus base_font(Mapping& m,u32 cp,u32* output) noexcept {
    if(m.pending){m.pending=false;*output=m.first;return SfntStatus::ok;}
    return m.base(m.context,cp,output);
}
SfntStatus base_choice(void* context,u32 cp,NativeFallbackChoice* output) noexcept {
    auto& m=*static_cast<Mapping*>(context);output->policy=native_fallback_missing;return base_font(m,cp,&output->font);
}
SfntStatus preferred(void* context,u32 cp,u32* output) noexcept {
    auto& m=*static_cast<Mapping*>(context);const auto status=base_font(m,cp,output);
    return status!=SfntStatus::ok||*output!=native_fallback_missing?status:m.fallback(m.context,cp,output);
}
SfntStatus route(void* context,u32 cp,u32* output) noexcept {auto& m=*static_cast<Mapping*>(context);return m.route(m.context,cp,output);}
SfntStatus cover(void* context,u32 font,u32 cp,bool* output) noexcept {auto& m=*static_cast<Mapping*>(context);return m.coverage(m.context,font,cp,output);}
}
SfntStatus native_font_fallback_map(Utf8View text,NativeFallbackIndexSelect base,NativeFallbackIndexSelect routing,
    NativeFallbackIndexSelect fallback,NativeFallbackCoverage coverage,void* context,NativeFallbackRangeResult* output,u32 work,u32* used) noexcept {
    if(used)*used=0;constexpr NativeFallbackRangeResult empty{{native_fallback_missing,native_fallback_missing},0,0};if(output)*output=empty;
    if(!output||(text.bytes&&!text.data)||!routing||!fallback||!coverage)return SfntStatus::invalid_input;
    if(!base||!text.bytes)return native_font_fallback_routed_range(text,routing,fallback,coverage,context,output,work,used);
    if(!work)return SfntStatus::resource_limit;
    const auto cp=utf8_decode_one(text.data,text.bytes);if(used)*used=1;if(!cp.valid)return SfntStatus::invalid_input;
    if(work<2)return SfntStatus::resource_limit;Mapping m{base,routing,fallback,coverage,context,native_fallback_missing};
    if(used)*used=2;const auto status=base(context,cp.codepoint,&m.first);if(status!=SfntStatus::ok)return status;
    u32 scanned=0;const auto result=m.first!=native_fallback_missing?
        native_font_fallback_range(text,base_choice,cover,&m,output,work-2,&scanned):
        native_font_fallback_routed_range(text,route,preferred,cover,&m,output,work-2,&scanned);
    if(used)*used=scanned+2;return result;
}
}
