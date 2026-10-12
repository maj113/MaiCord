// QA only. Batch arbitrary script-analysis strings, retaining scalar positions.
#define maicord_script_analysis_reference_entry maicord_unused_script_analysis_entry
#include "script_analysis_reference.cpp"
#undef maicord_script_analysis_reference_entry
namespace maicord {namespace {
bool sequence_observe(IDWriteTextAnalyzer1* analyzer) noexcept {
    const auto read=[](void* p,u32 count) noexcept {DWORD got=0;return ReadFile(GetStdHandle(STD_INPUT_HANDLE),p,count,&got,nullptr)&&got==count;};
    u32 header[2]{};if(!read(header,8)||header[0]!=0x3151534d||header[1]>1000000||!save(header,8))return false;
    Analysis<8192> input;u32 scalars[4096];u16 starts[4096],result[4096];
    for(u32 row=0;row<header[1];++row){u32 count=0;if(!read(&count,4)||!count||count>4096||!read(scalars,count*4))return false;input.length=0;
        for(u32 i=0;i<count;++i){const u32 cp=scalars[i];if(cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return false;starts[i]=static_cast<u16>(input.length);
            if(cp>0xffff){input.text[input.length++]=static_cast<wchar_t>(0xd800+((cp-0x10000)>>10));input.text[input.length++]=static_cast<wchar_t>(0xdc00+((cp-0x10000)&1023));}
            else input.text[input.length++]=static_cast<wchar_t>(cp);}
        for(u32 i=0;i<input.length;++i)input.scripts[i]={0x7fff,DWRITE_SCRIPT_SHAPES_DEFAULT};
        if(FAILED(analyzer->AnalyzeScript(&input,0,input.length,&input)))return false;
        for(u32 i=0;i<count;++i){const auto s=input.scripts[starts[i]];if(!packed(analyzer,s,result+i))return false;
            if(scalars[i]>0xffff&&(input.scripts[starts[i]+1].script!=s.script||input.scripts[starts[i]+1].shapes!=s.shapes))return false;}
        if(!save(&count,4)||!save(result,count*2))return false;
    }
    u8 tail=0;DWORD got=0;if(!ReadFile(GetStdHandle(STD_INPUT_HANDLE),&tail,1,&got,nullptr)||got)return false;
    return save(&script_end,4)&&save(properties,script_end*sizeof(DWRITE_SCRIPT_PROPERTIES))&&flush();
}
int sequence_run() noexcept {
    IDWriteFactory* factory=nullptr;IDWriteTextAnalyzer* base=nullptr;IDWriteTextAnalyzer1* analyzer=nullptr;
    auto hr=DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&factory));
    if(SUCCEEDED(hr))hr=factory->CreateTextAnalyzer(&base);if(SUCCEEDED(hr))hr=base->QueryInterface(__uuidof(IDWriteTextAnalyzer1),reinterpret_cast<void**>(&analyzer));
    const bool okay=SUCCEEDED(hr)&&sequence_observe(analyzer);if(analyzer)analyzer->Release();if(base)base->Release();if(factory)factory->Release();return okay?0:1;
}
}}
extern "C" int maicord_script_sequence_reference_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::sequence_run()));}
