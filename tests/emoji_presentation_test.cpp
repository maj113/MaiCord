#include "maicord/ui/emoji_presentation.hpp"
#include "maicord/core/utf.hpp"
namespace maicord {namespace {
u32 checks=0,cases=0;
void print(Utf8View text) noexcept {DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text.data,text.bytes,&done,nullptr);}
void number(u32 v) noexcept {char s[16];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){const char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}print({s,n});}
#define CHECK(x) do{++checks;if(!(x)){print(utf8_literal("FAIL line="));number(__LINE__);print(utf8_literal(" case="));number(cases);print(utf8_literal("\n"));return false;}}while(0)
struct Expected {u32 first,last;};
bool expect(Utf8View text,Expected a={},Expected b={},Expected c={}) noexcept {
    ++cases;const Expected expected[]{a,b,c};TextEmojiCursor cursor{};TextEmojiRange range{};u32 count=0;
    for(;;){const auto before=cursor;const auto status=text_emoji_next(text,&cursor,&range);CHECK(status!=TextEmojiNext::invalid_input);
        if(status==TextEmojiNext::end){CHECK(!range.first_byte&&!range.end_byte&&!range.first_unit&&!range.end_unit);break;}
        CHECK(count<3&&expected[count].last&&range.first_unit==expected[count].first&&range.end_unit==expected[count].last);
        u32 first=0,last=0;CHECK(utf8_byte_offset_for_utf16_position(text,expected[count].first,&first)&&utf8_byte_offset_for_utf16_position(text,expected[count].last,&last));
        CHECK(range.first_byte==first&&range.end_byte==last&&cursor.byte>before.byte&&cursor.byte<=text.bytes&&cursor.unit<=cursor.byte);++count;
    }
    CHECK(count==static_cast<u32>(a.last!=0)+static_cast<u32>(b.last!=0)+static_cast<u32>(c.last!=0));
    u32 units=0;CHECK(utf8_utf16_position_for_byte_offset(text,text.bytes,&units)&&cursor.byte==text.bytes&&cursor.unit==units);
    CHECK(text_emoji_next(text,&cursor,&range)==TextEmojiNext::end);return true;
}
bool policy() noexcept {
    CHECK(expect({}));CHECK(expect(utf8_literal("plain 123 #* \t\r\n")));
    CHECK(expect(utf8_literal("A\0B")));
    CHECK(expect(utf8_literal("\xc2\xa9"),{0,1}));
    CHECK(expect(utf8_literal("\xf0\x9f\x98\x80"),{0,2}));
    CHECK(expect(utf8_literal("A\xf0\x9f\x98\x80" "B"),{1,3}));
    CHECK(expect(utf8_literal("\xf0\x9f\x98\x80\xf0\x9f\x98\x80"),{0,4}));
    CHECK(expect(utf8_literal("\xf0\x9f\x98\x80 \xf0\x9f\x98\x80"),{0,2},{3,5}));
    CHECK(expect(utf8_literal("1\xef\xb8\x8f")));
    CHECK(expect(utf8_literal("1\xe2\x83\xa3"),{0,2}));
    CHECK(expect(utf8_literal("#\xef\xb8\x8f\xe2\x83\xa3*\xe2\x83\xa3"),{0,5}));
    CHECK(expect(utf8_literal("1\xef\xb8\x8e\xe2\x83\xa3")));
    CHECK(expect(utf8_literal("\xe2\x9d\xa4\xef\xb8\x8e")));
    CHECK(expect(utf8_literal("\xe2\x9d\xa4\xef\xb8\x8f"),{0,2}));
    CHECK(expect(utf8_literal("\xf0\x9f\x98\x80\xef\xb8\x8f\xef\xb8\x8e\xef\xb8\x8f")));
    CHECK(expect(utf8_literal("\xf0\x9f\x98\x80\xe2\x80\x8d\xf0\x9f\x91\xa9"),{0,5}));
    CHECK(expect(utf8_literal("\xf0\x9f\x98\x80\xe2\x80\x8d\xf0\x9f\x91\xa9\xef\xb8\x8e")));
    CHECK(expect(utf8_literal("\xf0\x9f\x98\x80\xe2\x80\x8d"),{0,2}));
    CHECK(expect(utf8_literal("\xe2\x80\x8d\xf0\x9f\x98\x80"),{1,3}));
    CHECK(expect(utf8_literal("\xf0\x9f\x98\x80\xe2\x80\x8d" "A\xf0\x9f\x98\x80"),{0,2},{4,6}));
    CHECK(expect(utf8_literal("\xf0\x9f\x98\x80\xe2\x80\x8c\xf0\x9f\x98\x80"),{0,2},{3,5}));
    CHECK(expect(utf8_literal("\xf0\x9f\x98\x80\xcc\x81\xf0\x9f\x98\x80"),{0,2},{3,5}));
    CHECK(expect(utf8_literal("\xf0\x9f\x98\x80\xf0\x9f\x8f\xbd"),{0,4}));
    CHECK(expect(utf8_literal("\xf0\x9f\x8f\xbd"),{0,2}));
    CHECK(expect(utf8_literal("\xf0\x9f\x87\xba\xf0\x9f\x87\xb8\xf0\x9f\x87\xba"),{0,6}));
    CHECK(expect(utf8_literal("\xf0\x9f\x8f\xb4\xf3\xa0\x81\xa7\xf3\xa0\x81\xbf"),{0,6}));
    CHECK(expect(utf8_literal("\xf3\xa0\x81\xa7\xf3\xa0\x81\xbf")));
    CHECK(expect(utf8_literal("\xef\xb8\x8f\xf0\x9f\x98\x80"),{1,3}));
    CHECK(expect(utf8_literal("\xd8\xa8\xf0\x9f\x98\x80\xd8\xaa"),{1,3}));
    CHECK(expect(utf8_literal("\xe4\xb8\xad\xf0\x9f\x98\x80\xe6\x96\x87"),{1,3}));
    return true;
}
bool errors() noexcept {
    const Utf8View malformed[]{utf8_literal("\x80"),utf8_literal("\xc0\x80"),utf8_literal("\xed\xa0\x80"),utf8_literal("\xf4\x90\x80\x80"),utf8_literal("a\xe2\x80"),utf8_literal("\xf0\x9f\x98\x80\xff")};
    for(const auto text:malformed){TextEmojiCursor cursor{};TextEmojiRange range{1,2,3,4};CHECK(text_emoji_next(text,&cursor,&range)==TextEmojiNext::invalid_input);CHECK(!cursor.byte&&!cursor.unit&&!range.first_byte&&!range.end_byte&&!range.first_unit&&!range.end_unit);}
    TextEmojiCursor cursor{};TextEmojiRange range{};
    CHECK(text_emoji_next({nullptr,1},&cursor,&range)==TextEmojiNext::invalid_input);
    CHECK(text_emoji_next({},nullptr,&range)==TextEmojiNext::invalid_input);
    CHECK(text_emoji_next({},&cursor,nullptr)==TextEmojiNext::invalid_input);
    cursor={1,0};CHECK(text_emoji_next({},&cursor,&range)==TextEmojiNext::invalid_input&&cursor.byte==1&&!cursor.unit);
    cursor={0,1};CHECK(text_emoji_next(utf8_literal("a"),&cursor,&range)==TextEmojiNext::invalid_input&&!cursor.byte&&cursor.unit==1);
    cursor={};const auto text=utf8_literal("\xf0\x9f\x98\x80 a\xff");
    CHECK(text_emoji_next(text,&cursor,&range)==TextEmojiNext::range&&cursor.byte==4&&cursor.unit==2);
    CHECK(text_emoji_next(text,&cursor,&range)==TextEmojiNext::invalid_input&&cursor.byte==4&&cursor.unit==2&&!range.end_byte);
    return true;
}
bool long_runs() noexcept {
    static char buffer[65536];for(auto& c:buffer)c='a';CHECK(expect({buffer,sizeof(buffer)}));
    const auto emoji=utf8_literal("\xf0\x9f\x98\x80");for(u32 i=0;i<sizeof(buffer);i+=4)copy_bytes(buffer+i,emoji.data,4);
    CHECK(expect({buffer,sizeof(buffer)},{0,sizeof(buffer)/2}));
    for(u32 i=0;i<sizeof(buffer);i+=4){buffer[i]='1';buffer[i+1]='\xef';buffer[i+2]='\xb8';buffer[i+3]='\x8f';}
    CHECK(expect({buffer,sizeof(buffer)}));return true;
}
}}
extern "C" int maicord_emoji_presentation_entry() noexcept {
    using namespace maicord;const bool ok=policy()&&errors()&&long_runs()&&!GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll");
    print(utf8_literal("cases="));number(cases);print(utf8_literal(" checks="));number(checks);print(ok?utf8_literal("\nPASS native emoji presentation\n"):utf8_literal("\nFAIL\n"));ExitProcess(ok?0u:1u);
}
