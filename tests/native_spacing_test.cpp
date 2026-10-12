// QA only: compare the production spacing primitive with installed Windows.
#include "maicord/text/gpos_execute.hpp"
#include <dwrite_1.h>
namespace maicord {namespace {
bool output_ok=true;
void print(Utf8View text) noexcept {DWORD written=0;output_ok=output_ok&&WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text.data,text.bytes,&written,nullptr)&&written==text.bytes;}
void number(u32 value) noexcept {char text[16];u32 n=0;do{text[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n/2;++i){const char a=text[i];text[i]=text[n-i-1];text[n-i-1]=a;}print({text,n});}
void field(Utf8View name,u32 value) noexcept {print(name);number(value);}
int run() noexcept {
    IDWriteFactory* factory=nullptr;IDWriteTextAnalyzer* analyzer=nullptr;IDWriteTextAnalyzer1* a1=nullptr;
    HRESULT hr=DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&factory));
    if(SUCCEEDED(hr))hr=factory->CreateTextAnalyzer(&analyzer);if(SUCCEEDED(hr))hr=analyzer->QueryInterface(__uuidof(IDWriteTextAnalyzer1),reinterpret_cast<void**>(&a1));
    if(FAILED(hr))return 1;
    SfntPlacement guard[1]{{1,0,0}};u8 guard_flags[1]{sfnt_position_cluster_start};u32 nan_bits=0x7fc00000u,inf_bits=0x7f800000u,max_bits=0x7f7fffffu;float nan=0,infinity=0,largest=0;copy_bytes(&nan,&nan_bits,4);copy_bytes(&infinity,&inf_bits,4);copy_bytes(&largest,&max_bits,4);
    if(sfnt_trailing_spacing({nullptr,1},{},.5f)!=SfntStatus::invalid_input||
       sfnt_trailing_spacing({guard,1},{guard_flags,2},.5f)!=SfntStatus::invalid_input||
       sfnt_trailing_spacing({guard,1},{},-.5f)!=SfntStatus::invalid_input||
       sfnt_trailing_spacing({guard,1},{},nan)!=SfntStatus::invalid_input||
       sfnt_trailing_spacing({guard,1},{},infinity)!=SfntStatus::invalid_input||
       sfnt_trailing_spacing({guard,1},{},.5f,0)!=SfntStatus::resource_limit||
       sfnt_trailing_spacing({guard,1},{},-0.0f)!=SfntStatus::ok)return 1;
    guard[0].advance=largest;if(sfnt_trailing_spacing({guard,1},{},largest)!=SfntStatus::resource_limit)return 1;
    u32 cases=0,failed=0;const float spacings[]{0,.125f,.3f,16};const float values[]{0,1.25f,4.875f,10000000};
    print(utf8_literal("{\"failures\":["));
    for(u32 direction=0;direction<2;++direction)for(u32 count=1;count<=6;++count)for(u32 boundaries=0;boundaries<(1u<<(count-1));++boundaries)for(u32 zero=0;zero<(1u<<count);++zero)for(u32 property_mode=0;property_mode<2;++property_mode)for(u32 pattern=0;pattern<4;++pattern)for(float spacing:spacings){
        UINT16 clusters[6]{};DWRITE_SHAPING_GLYPH_PROPERTIES props[6]{};float advances[6]{},actual[6]{},expected[6]{};
        DWRITE_GLYPH_OFFSET offsets[6]{},actual_offsets[6]{},expected_offsets[6]{};u32 start=0;
        for(u32 i=0;i<count;++i){const bool boundary=!i||(boundaries&(1u<<(i-1)));if(boundary)start=i;clusters[i]=static_cast<u16>(start);
            props[i].isClusterStart=boundary;props[i].isZeroWidthSpace=(zero&(1u<<i))!=0;props[i].isDiacritic=!property_mode&&props[i].isZeroWidthSpace;
            advances[i]=expected[i]=props[i].isZeroWidthSpace?0:values[(i+pattern)%4];offsets[i]=expected_offsets[i]={static_cast<float>(i)*.7f,static_cast<float>(i)*-.2f};}
        SfntPlacement native[6]{};u8 native_flags[6]{};
        for(u32 i=0;i<count;++i){native[i]={advances[i],offsets[i].advanceOffset,offsets[i].ascenderOffset};native_flags[i]=static_cast<u8>((props[i].isClusterStart?sfnt_position_cluster_start:0)|(props[i].isZeroWidthSpace?sfnt_position_zero_advance:0));}
        if(sfnt_trailing_spacing({native,count},{native_flags,count},spacing,1000000,direction!=0)!=SfntStatus::ok)return 1;
        for(u32 i=0;i<count;++i){expected[i]=native[i].advance;expected_offsets[i]={native[i].advance_offset,native[i].ascender_offset};}
        hr=a1->ApplyCharacterSpacing(direction?spacing:0,direction?0:spacing,0,count,count,clusters,advances,offsets,props,actual,actual_offsets);++cases;
        bool match=SUCCEEDED(hr)&&bytes_equal(actual,expected,count*4)&&bytes_equal(actual_offsets,expected_offsets,count*sizeof(DWRITE_GLYPH_OFFSET));
        if(!match){if(failed<16){if(failed)print(utf8_literal(","));field(utf8_literal("{\"count\":"),count);field(utf8_literal(",\"boundaries\":"),boundaries);field(utf8_literal(",\"zero\":"),zero);field(utf8_literal(",\"pattern\":"),pattern);u32 sb=0;copy_bytes(&sb,&spacing,4);field(utf8_literal(",\"spacing\":"),sb);
            print(utf8_literal(",\"rows\":["));for(u32 i=0;i<count;++i){if(i)print(utf8_literal(","));print(utf8_literal("["));const float observed[]{advances[i],expected[i],actual[i],offsets[i].advanceOffset,expected_offsets[i].advanceOffset,actual_offsets[i].advanceOffset};for(u32 j=0;j<6;++j){u32 b=0;copy_bytes(&b,observed+j,4);if(j)print(utf8_literal(","));number(b);}print(utf8_literal("]"));}print(utf8_literal("]}"));}++failed;}
    }
    field(utf8_literal("],\"cases\":"),cases);field(utf8_literal(",\"failed\":"),failed);field(utf8_literal(",\"validation_cases\":"),8);print(utf8_literal("}\n"));com_release(a1);com_release(analyzer);com_release(factory);return output_ok&&!failed?0:1;
}
}}
extern "C" void maicord_native_spacing_entry(){ExitProcess(static_cast<UINT>(maicord::run()));}
