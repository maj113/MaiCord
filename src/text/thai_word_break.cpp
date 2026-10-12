#include "thai_word_break.hpp"
#include "thai_forward.hpp"
#include "maicord/text/unicode.hpp"

namespace maicord {namespace {
bool thai_context(u32 cp) noexcept {
    return cp-0xe01u<91&&unicode_properties(cp)->line_break()==UnicodeLineBreak::sa;
}
}
bool native_thai_word_breaks(ConstSlice<u32> text,Slice<u8> breaks) noexcept {
    if((text.count&&!text.data)||!breaks.data||breaks.count<=text.count)return false;
    for(u32 at=0;at<text.count;){
        if(!thai_context(text[at])){++at;continue;}
        const u32 first=at++;
        while(at<text.count&&(thai_context(text[at])||text[at]==0x20||text[at]==0x200b))++at;
        struct Projection {ConstSlice<u32> text;u8* breaks;u32 position;};
        Projection output{{text.data+first,at-first},breaks.data+first,0};
        const auto emit=[](void* opaque,u8 length) noexcept {
            auto& p=*static_cast<Projection*>(opaque);p.position+=length;
            if(p.position<p.text.count&&p.text[p.position]!=0x20&&p.text[p.position]!=0x200b)p.breaks[p.position]=1;
        };
        if(!native_thai_forward_segments(output.text,&output,emit))return false;
    }
    return true;
}
}
