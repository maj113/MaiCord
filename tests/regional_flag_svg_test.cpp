#include "maicord/media/regional_flag_svg.hpp"
#include "maicord/ui/regional_flag.hpp"

namespace maicord { namespace {
void print(const char* value) noexcept {u32 n=0;while(value[n])++n;DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),value,n,&written,nullptr);}
void number(u64 value) noexcept {char text[32];u32 n=0;do{text[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n/2;++i){const char t=text[i];text[i]=text[n-i-1];text[n-i-1]=t;}DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,n,&written,nullptr);}
#define CHECK(x) do {if(!(x)){print("FAIL line=");number(__LINE__);print("\n");return false;}}while(0)
bool run(bool self_test) noexcept {
    RegionalFlagSvg image{}; GlyphPathCommand commands[kRegionalFlagSvgMaximumCommands]{};
    const auto parse=[&](Utf8View text) noexcept {return regional_flag_svg_parse({reinterpret_cast<const u8*>(text.data),text.bytes},&image);};
    constexpr const char* bad[]={
        "", "<!DOCTYPE svg><svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 36 36\"/>",
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 36 36\"><script/></svg>",
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 36 36\"><path d=\"M0 0L1 1\" opacity=\".5\"/></svg>",
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 36 36\"><path d=\"M0 0L1 1\" fill=\"url(#x)\"/></svg>",
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 36 36\"><ellipse rx=\"1\" ry=\"1\" transform=\"rotate(20)\"/></svg>",
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 36 36\"><path d=\"M0 0L1 1\" fill=\"red\" fill=\"navy\"/></svg>",
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 36 36\"><path d=\"&entity;\"/></svg>"};
    for(const auto text:bad){u32 n=0;while(text[n])++n;CHECK(!parse({text,n}));}
    CHECK(!regional_flag_svg_parse({nullptr,1},&image));CHECK(!regional_flag_svg_parse({},nullptr));
    image.count=7;CHECK(!regional_flag_svg_parse({reinterpret_cast<const u8*>(&image),sizeof(image)},&image));CHECK(image.count==7);
    CHECK(parse(utf8_literal("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 36 36\"><g fill=\"#123\" fill-rule=\"evenodd\"><path d=\"M1 1h4v4h-4z M2 2l2 0 0 2-2 0z\"/></g></svg>")));
    CHECK(image.count==1&&image.layers[0].fill_rule==GlyphFillRule::alternate&&image.layers[0].color.a==1);
    GlyphOutline outline{commands,kRegionalFlagSvgMaximumCommands};CHECK(regional_flag_svg_outline(image.layers[0],&outline));CHECK(outline.count==10);
    ColrPaintProgram program{};CHECK(arena_initialize(&program.storage,8*1024*1024,0,4096));
    CHECK(regional_flag_svg_compile(image,&program,64.f/36));
    CHECK(program.mask_count==1&&program.stack==1);
    const char* malformed[]={"M0 0T1 1","M0 0L1e99 2","M0 0A1 1 0 2 0 1 1","M0 0C1 1 2 2","L0 0","M0 0zL1 1"};
    for(const auto text:malformed){u32 n=0;while(text[n])++n;RegionalFlagSvgLayer layer{};layer.path={text,n};CHECK(!regional_flag_svg_outline(layer,&outline));}
    RegionalFlagSvgLayer invalid{};invalid.path={nullptr,1};CHECK(!regional_flag_svg_outline(invalid,&outline));
    invalid.path={};invalid.rx=invalid.ry=1;invalid.rotation=3.402823466e38f;CHECK(!regional_flag_svg_outline(invalid,&outline));
    union {u32 bits;float value;} nonfinite{0x7fc00000u};invalid.rotation=0;invalid.cx=nonfinite.value;
    CHECK(!regional_flag_svg_outline(invalid,&outline));nonfinite.bits=0x7f800000u;invalid.cx=nonfinite.value;
    CHECK(!regional_flag_svg_outline(invalid,&outline));
    print("parser_bytes=");number(sizeof(RegionalFlagSvg));print(" layer_bytes=");number(sizeof(RegionalFlagSvgLayer));
    print(" outline_workspace_bytes=");number(sizeof(commands));print("\n");
    if(self_test){arena_release(&program.storage);print("regional_flag_svg_self_test_passed\n");return true;}
    auto* encoded=static_cast<u8*>(VirtualAlloc(nullptr,kRegionalFlagSvgMaximumBytes+1,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(encoded);
    CHECK(!regional_flag_svg_parse({encoded,kRegionalFlagSvgMaximumBytes+1},&image));
    u32 admitted=0,max_layers=0,max_commands=0,max_nodes=0,max_mask_pixels=0,max_program=0;u64 total_bytes=0,max_committed=0;
    for(u16 id=1;id<=679;++id){char target[kRegionalFlagSvgTargetBytes]{};const u32 target_bytes=regional_flag_svg_target(id,{target,sizeof(target)});if(!target_bytes)continue;
        wchar_t filename[46]{};for(u32 i=kRegionalFlagSvgTargetPrefixBytes;i<target_bytes;++i)filename[i-kRegionalFlagSvgTargetPrefixBytes]=static_cast<wchar_t>(target[i]);
        const HANDLE file=CreateFileW(filename,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE){print("Missing research flag id=");number(id);print("\n");return false;}
        DWORD bytes=0;const bool read=ReadFile(file,encoded,kRegionalFlagSvgMaximumBytes+1,&bytes,nullptr)!=FALSE;CloseHandle(file);
        if(!read||!regional_flag_svg_parse({encoded,bytes},&image)){print("Parse flag id=");number(id);print("\n");return false;}
        for(u32 i=0;i<image.count;++i){if(!regional_flag_svg_outline(image.layers[i],&outline)){print("Outline flag id=");number(id);print(" layer=");number(i);print("\n");return false;}max_commands=maximum(max_commands,outline.count);}
        if(!regional_flag_svg_compile(image,&program,64.f/36)){print("Compile flag id=");number(id);print("\n");return false;}
        CHECK(program.stack==1&&program.peak_stack<=64&&program.storage.offset<=8*1024*1024);
        ++admitted;total_bytes+=bytes;max_layers=maximum(max_layers,image.count);max_nodes=maximum(max_nodes,program.nodes);
        max_mask_pixels=maximum(max_mask_pixels,program.mask_pixels);max_program=maximum(max_program,static_cast<u32>(program.storage.offset));
        max_committed=maximum(max_committed,static_cast<u64>(program.storage.committed));
    }
    CHECK(admitted==kRegionalFlagAssetCount&&total_bytes==728487);
    print("flags=");number(admitted);print(" bytes=");number(total_bytes);print(" max_layers=");number(max_layers);
    print(" max_commands=");number(max_commands);print(" max_nodes=");number(max_nodes);print(" max_mask_pixels=");number(max_mask_pixels);
    print(" max_program_bytes=");number(max_program);print(" max_program_committed=");number(max_committed);print("\n");
    VirtualFree(encoded,0,MEM_RELEASE);arena_release(&program.storage);return true;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_regional_flag_svg_test_entry() noexcept {
    using namespace maicord;
    const wchar_t* args=GetCommandLineW();if(*args=='"'){++args;while(*args&&*args!='"')++args;if(*args)++args;}
    else while(*args&&*args!=' '&&*args!='\t')++args;
    while(*args==' '||*args=='\t')++args;
    constexpr wchar_t self[]=L"--self-test";u32 i=0;while(self[i]&&args[i]==self[i])++i;
    const bool self_test=!self[i]&&(!args[i]||args[i]==' '||args[i]=='\t');
    ExitProcess(maicord::run(self_test)?0:1);
}
