#include "maicord/ui/emoji_presentation.hpp"
#include "maicord/ui/emoji_font_ranges.hpp"
#include "maicord/core/utf.hpp"
namespace maicord {namespace {
inline bool keycap_base(u32 cp) noexcept {return cp=='#'||cp=='*'||(cp>='0'&&cp<='9');}
inline bool atom(Utf8View text,TextEmojiCursor& cursor,bool* selected) noexcept {
    const u8 lead=static_cast<u8>(text.data[cursor.byte]);
    if(lead<0x80&&!keycap_base(lead)){
        do{++cursor.byte;++cursor.unit;}while(cursor.byte<text.bytes&&static_cast<u8>(text.data[cursor.byte])<0x80&&!keycap_base(static_cast<u8>(text.data[cursor.byte])));
        *selected=false;return true;
    }
    const auto first=lead<0x80?Utf8DecodeResult{lead,1,true}:utf8_decode_one(text.data+cursor.byte,text.bytes-cursor.byte);
    if(!first.valid)return false;
    cursor.byte+=first.width;cursor.unit+=first.codepoint>0xffff?2u:1u;
    bool emoji=lead<0x80||text_emoji_scalar(first.codepoint),text_selector=false,keycap=false;
    while(emoji&&cursor.byte<text.bytes){
        const auto next=utf8_decode_one(text.data+cursor.byte,text.bytes-cursor.byte);
        if(!next.valid)return false;
        const u32 cp=next.codepoint;
        if(cp==0xfe0e||cp==0xfe0f||cp==0x20e3||(cp>=0x1f3fb&&cp<=0x1f3ff)||(cp>=0xe0020&&cp<=0xe007f)){
            text_selector|=cp==0xfe0e;keycap|=cp==0x20e3;
            cursor.byte+=next.width;cursor.unit+=cp>0xffff?2u:1u;continue;
        }
        if(cp==0x200d&&next.width<text.bytes-cursor.byte){
            const auto joined=utf8_decode_one(text.data+cursor.byte+next.width,text.bytes-cursor.byte-next.width);
            if(joined.valid&&text_emoji_scalar(joined.codepoint)){
                cursor.byte+=next.width+joined.width;cursor.unit+=1+(joined.codepoint>0xffff?2u:1u);continue;
            }
        }
        break;
    }
    *selected=emoji&&!text_selector&&(first.codepoint>0x7f||keycap);return true;
}
}
TextEmojiNext text_emoji_next(Utf8View text,TextEmojiCursor* cursor,TextEmojiRange* output) noexcept {
    if(output)*output={};
    if(!cursor||!output||(text.bytes&&!text.data)||cursor->byte>text.bytes||cursor->unit>cursor->byte)return TextEmojiNext::invalid_input;
    auto position=*cursor;TextEmojiRange range{};bool have_range=false;
    while(position.byte<text.bytes){
        const auto first=position;bool selected=false;
        if(!atom(text,position,&selected))return TextEmojiNext::invalid_input;
        if(selected){
            if(!have_range){range.first_byte=first.byte;range.first_unit=first.unit;have_range=true;}
            range.end_byte=position.byte;range.end_unit=position.unit;
        }else if(have_range){*cursor=first;*output=range;return TextEmojiNext::range;}
    }
    *cursor=position;*output=range;return have_range?TextEmojiNext::range:TextEmojiNext::end;
}
}
