#include "maicord/text/arabic_joining.hpp"

namespace maicord {
ArabicJoiningStatus arabic_joining_forms(ConstSlice<u32> scalars,Slice<u8> output,u32 stride,u64 work_limit,bool reverse) noexcept {
    using S=ArabicJoiningStatus;using J=UnicodeJoiningType;using F=ArabicJoiningForm;
    if(!stride || (scalars.count && !scalars.data) || (output.count && !output.data))return S::invalid_input;
    if(!scalars.count)return S::ok;
    const u64 span=static_cast<u64>(scalars.count-1)*stride+1;
    if(span>output.count)return S::output_too_small;
    if(static_cast<u64>(scalars.count)*2>work_limit)return S::work_limit;
    if(byte_ranges_overlap_or_wrap(scalars.data,static_cast<u64>(scalars.count)*sizeof(u32),output.data,span))return S::invalid_input;
    u32 previous=0;bool forward=false,previous_has_form=false;
    for(u32 at=0;at<scalars.count;++at) {const u32 i=reverse?scalars.count-1-at:at;
        const J type=unicode_properties(scalars[i])->joining_type();
        // Forced-LTR Windows gives ZWJ form eligibility; Tatweel remains nominal.
        const bool has_form=type==J::d || type==J::r || type==J::l || (reverse&&scalars[i]==0x200d);
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
    return S::ok;
}
} // namespace maicord
