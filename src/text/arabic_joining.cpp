#include "maicord/text/arabic_joining.hpp"

namespace maicord {
ArabicJoiningStatus arabic_joining_forms(ConstSlice<u32> scalars,Slice<u8> output,u32 stride,u64 work_limit,bool reverse) noexcept {
    using S=ArabicJoiningStatus;using J=UnicodeJoiningType;using F=ArabicJoiningForm;
    if(!stride || (scalars.count && !scalars.data) || (output.count && !output.data))return S::invalid_input;
    if(!scalars.count)return S::ok;
    const u64 span=static_cast<u64>(scalars.count-1)*stride+1;
    if(span>output.count)return S::output_too_small;
    if(static_cast<u64>(scalars.count)*2>work_limit)return S::work_limit;
    const u64 input_begin=reinterpret_cast<usize>(scalars.data),output_begin=reinterpret_cast<usize>(output.data);
    const u64 input_bytes=static_cast<u64>(scalars.count)*sizeof(u32);
    if(input_begin>~u64{0}-input_bytes || output_begin>~u64{0}-span ||
       (input_begin<output_begin+span && output_begin<input_begin+input_bytes))return S::invalid_input;
    for(u32 i=0;i<scalars.count;++i)if(!unicode_is_scalar(scalars[i]))return S::invalid_input;
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
