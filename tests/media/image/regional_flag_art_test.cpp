#include "maicord/media/image/regional_flag_art.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord { namespace {
void print(const char* value) noexcept {u32 n=0;while(value[n])++n;DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),value,n,&written,nullptr);}
void number(u64 value) noexcept {char text[32];u32 n=0;do{text[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n/2;++i){const char t=text[i];text[i]=text[n-i-1];text[n-i-1]=t;}DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,n,&written,nullptr);}
#define CHECK(x) do {if(!(x)){print("FAIL line=");number(__LINE__);print("\n");return false;}}while(0)
bool run() noexcept {
    RegionalFlagArt art{}; GlyphPathCommand commands[kRegionalFlagMaximumCommands]{};
    const auto parse=[&](ConstSlice<u8> record) noexcept {return regional_flag_record_parse(record,&art);};
    // M1 1h4v4h-4z M2 2l2 0 0 2-2 0z in tenths (zigzag varints), filled #112233 even-odd: the commands, their zero
    // end, then the numbers at even places (the x stream) and at odd places (the y stream).
    constexpr u8 squares[]{4,0x11,0x22,0x33,'M',2,'h',1,'v',1,'h',1,'z',0,'M',2,'l',6,'z',0,0,
        20,80,80,79,40,40,0,39, 20,40,0,40,0, 0xFF};
    CHECK(parse({squares,sizeof(squares)}));
    const auto& layer=art.layers[0];
    CHECK(art.count==1&&layer.fill_rule==GlyphFillRule::alternate&&layer.color.r==0x11/255.f&&
          layer.color.g==0x22/255.f&&layer.color.b==0x33/255.f&&layer.color.a==1);
    GlyphOutline outline{commands,kRegionalFlagMaximumCommands};CHECK(regional_flag_layer_outline(layer,&outline));
    CHECK(outline.count==10&&outline.fill_rule==GlyphFillRule::alternate);
    CHECK(commands[0].kind==GlyphPathCommandKind::begin_filled&&commands[0].x1==1&&commands[0].y1==1);
    CHECK(commands[3].kind==GlyphPathCommandKind::line&&commands[3].x1==1&&commands[3].y1==5);
    CHECK(commands[4].kind==GlyphPathCommandKind::end_closed&&commands[8].x1==2&&commands[8].y1==4);
    CHECK(outline.minimum_x==1&&outline.minimum_y==1&&outline.maximum_x==5&&outline.maximum_y==5);
    ColrPaintProgram program{};CHECK(arena_initialize(&program.storage,8*1024*1024,0,4096));
    CHECK(regional_flag_compile(art,&program));
    // A black circle at (1, 1), radius .5, and the rotated ellipse, in hundredths.
    constexpr u8 shapes[]{1,0,0,0,0xC8,0x01,0xC8,0x01,0x64,10,0,0,0x80,0xC8,0x01,0xC8,0x01,0x64,0xC8,0x01,0xFF};
    CHECK(parse({shapes,sizeof(shapes)}));
    CHECK(art.count==2&&art.layers[0].cx==1&&art.layers[0].ry==.5f&&art.layers[0].fill_rule==GlyphFillRule::winding&&
          !art.layers[0].rotation&&art.layers[1].rotation==-76.714f&&art.layers[1].ry==1&&art.layers[1].color.b==128/255.f);
    for(u32 i=0;i<2;++i){CHECK(regional_flag_layer_outline(art.layers[i],&outline));CHECK(outline.count==6);}
    // Truncation, no layers, a fourth kind, a rotated circle, unknown header bits,
    // a count past the record, an empty path.
    constexpr u8 bad[][8]{{0,0,0,0,'M',2,0,0},{0xFF},{3,0,0,0,0xFF},{9,0,0,0,2,2,2,0xFF},{16,0,0,0,2,2,2,0xFF},
        {0,0,0,0,'M',9,0,0},{0,0,0,0,0,0xFF}};
    constexpr u32 bad_bytes[]{8,1,5,8,8,8,6};
    for(u32 i=0;i<7;++i)CHECK(!parse({bad[i],bad_bytes[i]}));
    // A byte past the end, a y stream that runs past the record, a flat circle.
    constexpr u8 trailing[]{1,0,0,0,2,2,2,0xFF,0};CHECK(!parse({trailing,sizeof(trailing)}));
    constexpr u8 long_y[]{0,0,0,0,'M',2,0,2,0x80};CHECK(!parse({long_y,sizeof(long_y)}));
    constexpr u8 flat[]{1,0,0,0,2,2,0,0xFF};CHECK(!parse({flat,sizeof(flat)}));
    static u8 crowded[(kRegionalFlagMaximumLayers+1)*7+1];
    for(u32 i=0;i<=kRegionalFlagMaximumLayers;++i){u8* at=crowded+i*7;at[0]=1;at[4]=at[5]=at[6]=2;}
    crowded[sizeof(crowded)-1]=0xFF;CHECK(!parse({crowded,sizeof(crowded)}));
    CHECK(parse({crowded+7,sizeof(crowded)-7})&&art.count==kRegionalFlagMaximumLayers);
    // An unsupported letter, a number past the view box, arc flag 2, a short cubic, a line before any move, and a
    // line after a close: each path's commands, their zero end, its x numbers and its y numbers.
    constexpr u8 malformed[][14]{{'M',2,'T',2,0, 0,2, 0,2},{'M',2,'L',2,0, 0,0xFE,0xFF,0x7F, 0,0},
        {'M',2,'A',7,0, 0,20,0,0,2, 0,20,40,2},{'M',2,'C',4,0, 0,2,4, 0,2,4},{'L',2,0, 0, 0},{'M',2,'z',0,'L',2,0, 0,2, 0,2}};
    constexpr u32 malformed_bytes[][3]{{4,2,2},{4,4,2},{4,5,4},{4,3,3},{2,1,1},{6,2,2}};
    const auto path_layer=[&](const u8* at,const u32* bytes) noexcept {
        RegionalFlagLayer path{};path.fill_rule=GlyphFillRule::winding;
        path.path={at,bytes[0]+1+bytes[1]+bytes[2]};path.command_bytes=bytes[0];path.x_bytes=bytes[1];return path;
    };
    for(u32 i=0;i<6;++i)CHECK(!regional_flag_layer_outline(path_layer(malformed[i],malformed_bytes[i]),&outline));
    constexpr u8 move[]{'M',2,0,0,0};constexpr u32 move_bytes[]{2,1,1};
    CHECK(regional_flag_layer_outline(path_layer(move,move_bytes),&outline)&&outline.count==2);
    arena_release(&program.storage);print("regional_flag_art_test_passed\n");return true;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_regional_flag_art_test_entry() noexcept {
    ExitProcess(maicord::run()?0:1);
}
