#include "maicord/text/scripts/arabic_joining.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
ArabicJoiningStatus arabic_joining_forms(ConstSlice<u32> scalars,Slice<u8> output,u32 stride,u64 work_limit,ArabicJoiningContext context) noexcept {
    using S=ArabicJoiningStatus;using J=UnicodeJoiningType;using F=ArabicJoiningForm;
    if(!stride || (scalars.count && !scalars.data) || (output.count && !output.data))return S::invalid_input;
    if(!scalars.count)return S::ok;
    const u64 span=static_cast<u64>(scalars.count-1)*stride+1;
    if(span>output.count)return S::output_too_small;
    if(static_cast<u64>(scalars.count)*2>work_limit)return S::work_limit;
    if(byte_ranges_overlap_or_wrap(scalars.data,static_cast<u64>(scalars.count)*sizeof(u32),output.data,span))return S::invalid_input;
    u32 previous=0;bool forward=context.before,previous_has_form=false;
    for(u32 i=0;i<scalars.count;++i) {
        const J type=unicode_properties(scalars[i])->joining_type();
        // HarfBuzz joins a join-causing character (ZWJ, tatweel) as a dual-joining one, forms included.
        const bool has_form=type==J::d || type==J::r || type==J::l || type==J::c;
        auto& form=output.data[static_cast<usize>(i)*stride];
        form=static_cast<u8>(has_form?F::isolated:F::none);
        if(type==J::t)continue;
        if(forward && (type==J::d || type==J::r || type==J::c)) {
            if(previous_has_form) {
                auto& prior=output.data[static_cast<usize>(previous)*stride];
                prior=static_cast<u8>(prior==static_cast<u8>(F::final)?F::medial:F::initial);
            }
            if(has_form)form=static_cast<u8>(F::final);
        }
        previous=i;previous_has_form=has_form;
        forward=type==J::d || type==J::l || type==J::c;
    }
    if(context.after&&forward&&previous_has_form){auto& prior=output.data[static_cast<usize>(previous)*stride];
        prior=static_cast<u8>(prior==static_cast<u8>(F::final)?F::medial:F::initial);}
    return S::ok;
}
ArabicJoiningContext arabic_joining_context(Utf8View text,u32 begin,u32 end) noexcept {
    using J=UnicodeJoiningType;ArabicJoiningContext context{};
    const auto separator=[](u32 cp) noexcept {return cp-10u<4||cp==0x85||cp==0x2028||cp==0x2029;};
    for(u32 at=begin,seen=0;at&&seen<5;++seen){u32 start=at-1;while(start&&(static_cast<u8>(text.data[start])&0xc0)==0x80)--start;
        const auto cp=utf8_decode_one(text.data+start,at-start);if(!cp.valid||separator(cp.codepoint))break;at=start;
        const J type=unicode_properties(cp.codepoint)->joining_type();if(type==J::t)continue;
        context.before=type==J::d||type==J::l||type==J::c;break;}
    for(u32 at=end,seen=0;at<text.bytes&&seen<5;++seen){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);
        if(!cp.valid||separator(cp.codepoint))break;at+=cp.width;
        const J type=unicode_properties(cp.codepoint)->joining_type();if(type==J::t)continue;
        context.after=type==J::d||type==J::r||type==J::c;break;}
    return context;
}
} // namespace maicord
