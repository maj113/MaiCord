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
    NativeFallbackIndexSelect fallback,NativeFallbackCoverage coverage,void* context,NativeFallbackRangeResult* output) noexcept {
    const auto cp=utf8_decode_one(text.data,text.bytes);if(!cp.valid)return SfntStatus::invalid_input;
    Mapping m{base,routing,fallback,coverage,context,native_fallback_missing};
    const auto status=base(context,cp.codepoint,&m.first);if(status!=SfntStatus::ok)return status;
    return m.first!=native_fallback_missing?native_font_fallback_range(text,base_choice,cover,&m,output):
        native_font_fallback_routed_range(text,route,preferred,cover,&m,output);
}
}
