// QA only. Exhaustively observe intrinsic non-visual item flags in five contexts.
#define maicord_gsub_execution_reference_entry maicord_unused_gsub_reference_entry
#include "gsub_execution_reference.cpp"
#undef maicord_gsub_execution_reference_entry
namespace maicord {namespace {
void append_scalar(Analysis& a,u32 cp) noexcept {
    if(cp<0x10000)a.text[a.length++]=static_cast<wchar_t>(cp);
    else {cp-=0x10000;a.text[a.length++]=static_cast<wchar_t>(0xd800+(cp>>10));a.text[a.length++]=static_cast<wchar_t>(0xdc00+(cp&1023));}
}
bool visibility_observation(IDWriteTextAnalyzer* analyzer,u32 context) noexcept {
    const u32 surroundings[]{0,0x61,0x628,0x1f600,0x915};u32 cp=0,total=0,hidden=0,start=0,last=0;bool open=false,comma=false;
    field(utf8_literal("{\"context\":"),context);print(utf8_literal(",\"nonvisual_ranges\":["));
    const auto emit=[&]() noexcept {if(comma)print(utf8_literal(","));comma=true;field(utf8_literal("["),start);field(utf8_literal(","),last);print(utf8_literal("]"));};
    while(cp<0x110000){Analysis a;u32 points[256],offsets[256],count=0;
        while(cp<0x110000&&a.length+6<=text_capacity){if(cp==0xd800)cp=0xe000;
            if(context)append_scalar(a,surroundings[context]);offsets[count]=a.length;points[count++]=cp;append_scalar(a,cp);
            if(context)append_scalar(a,surroundings[context]);++cp;}
        const auto hr=analyzer->AnalyzeScript(&a,0,a.length,&a);if(FAILED(hr))return false;
        for(u32 i=0;i<a.length;++i)if(!a.seen[i])return false;
        for(u32 i=0;i<count;++i){const u32 value=points[i],at=offsets[i];const auto flags=a.scripts[at].shapes;
            if((flags!=DWRITE_SCRIPT_SHAPES_DEFAULT&&flags!=DWRITE_SCRIPT_SHAPES_NO_VISUAL)||(value>0xffff&&a.scripts[at+1].shapes!=flags))return false;
            ++total;if(flags==DWRITE_SCRIPT_SHAPES_NO_VISUAL){++hidden;if(open&&value!=last+1){emit();open=false;}if(!open){start=value;open=true;}last=value;}
            else if(open){emit();open=false;}}
    }
    if(open)emit();field(utf8_literal("],\"scalars\":"),total);field(utf8_literal(",\"nonvisual\":"),hidden);
    if(!module_path())return false;print(utf8_literal("}\n"));return total==0x110000-0x800;
}
int visibility_main() noexcept {
    IDWriteFactory* factory=nullptr;IDWriteTextAnalyzer* analyzer=nullptr;
    auto hr=DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&factory));
    if(SUCCEEDED(hr))hr=factory->CreateTextAnalyzer(&analyzer);
    for(u32 context=0;context<5&&SUCCEEDED(hr);++context)if(!visibility_observation(analyzer,context))hr=E_FAIL;
    com_release(analyzer);com_release(factory);flush();return SUCCEEDED(hr)&&output_ok?0:1;
}
}}
extern "C" void maicord_visibility_reference_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::visibility_main()));}
