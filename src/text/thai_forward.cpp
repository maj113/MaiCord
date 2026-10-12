#include "thai_forward.hpp"
#include "thai_lexical.hpp"
#include "thai_syllable.hpp"

namespace maicord {
bool native_thai_forward_segments(ConstSlice<u32> text,void* context,NativeThaiSegmentSink sink) noexcept {
    if((text.count&&!text.data)||!sink)return false;
    if(text.count<=1)return true;
    NativeThaiWordCandidates retained{},selected{};
    u32 cursor=0,slots=text.count-1;u8 current=0;bool unknown=false;
    const auto scalar=[&](u32 offset) noexcept {
        return cursor<text.count&&offset<text.count-cursor?text[cursor+offset]:0u;
    };
    const auto thai=[](u32 cp) noexcept {return cp-0xe00u<92;};
    const auto append=[&](u32 length) noexcept {current=static_cast<u8>(current+length);};
    const auto emit=[&]() noexcept {
        if(!current)return false;
        sink(context,current);current=0;return --slots!=0;
    };
    const auto lookup=[](void* opaque,u32 position) noexcept {
        return native_thai_lexical_candidates(*static_cast<ConstSlice<u32>*>(opaque),position);
    };
    while(cursor<text.count){
        const u32 cp=text[cursor];
        if(!thai(cp)){
            if(current==255){if(!emit())break;current=1;}
            else {append(1);retained={};}
            unknown=false;++cursor;continue;
        }
        if(cp==0xe2f){
            const u32 length=scalar(1)==0xe25&&scalar(2)==0xe2f?3u:
                scalar(1)==0xe1e&&scalar(2)==0xe13&&scalar(3)==0xe2f?4u:1u;
            append(length);if(!emit())break;cursor+=length;continue;
        }
        if(!thai(scalar(1))||scalar(1)==0xe00){
            append(1);if(!emit())break;++cursor;continue;
        }
        const u32 count=native_thai_word_select(cursor,text.count,&text,lookup,retained,selected);
        u32 length=static_cast<u32>(selected.lengths[0]);
        if(count){
            if(unknown){unknown=false;if(current>=3&&!emit())break;}
            // Later alternatives cannot change this branch: only the first
            // adjacent length gap determines whether the controller joins.
            if(count>1&&selected.lengths[0]-selected.lengths[1]<3){
                cursor+=length;append(length);continue;
            }
        }else if(!length){
            const u32 recovery=native_thai_unknown_recovery(text,cursor);
            if(recovery<=2){
                unknown=true;length=cp>0xe2e?2u:1u;cursor+=length;append(length);
                while(cursor<=text.count&&!native_thai_boundary_class(scalar(0))){++cursor;append(1);}
                continue;
            }
            length=recovery-1;
        }
        cursor+=length;append(length);
        while(scalar(0)==0xe46||scalar(0)==0xe2f){++cursor;append(1);retained={};}
        if(cursor>text.count)break;
        if(thai(scalar(0))&&!emit())break;
    }
    if(current)sink(context,current);
    return true;
}
}
