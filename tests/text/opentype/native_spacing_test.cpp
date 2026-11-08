// The production letter-spacing primitive against CSS Text 3 §8.2 (letter-spacing): every typographic character
// unit (a shaped cluster) grows by the spacing at its inline end, and the unit's own glyphs keep their places within
// it. The placements are in the glyph run's visual order with offsets along +x, the frame HarfBuzz positions in;
// `rtl` says the run reads right to left, so its inline end is each cluster's visual left (LTR: its right). A cluster
// without an advancing glyph (a zero-width control or a lone mark) has no extent to space and is left alone.
#include "maicord/text/opentype/gpos_execute.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord {namespace {
bool output_ok=true;
void print(Utf8View text) noexcept {DWORD written=0;output_ok=output_ok&&WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text.data,text.bytes,&written,nullptr)&&written==text.bytes;}
void number(u32 value) noexcept {char text[16];u32 n=0;do{text[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n/2;++i){const char a=text[i];text[i]=text[n-i-1];text[n-i-1]=a;}print({text,n});}
void field(Utf8View name,u32 value) noexcept {print(name);number(value);}
double magnitude(double value) noexcept {return value<0?-value:value;}
// Each glyph's x from the run's left edge: the pen plus its offset.
void visual(const SfntPlacement* glyphs,u32 count,double* x,double* width) noexcept {
    double pen=0;for(u32 i=0;i<count;++i){x[i]=pen+glyphs[i].advance_offset;pen+=glyphs[i].advance;}*width=pen;
}
int run() noexcept {
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
    for(u32 direction=0;direction<2;++direction)for(u32 count=1;count<=6;++count)for(u32 boundaries=0;boundaries<(1u<<(count-1));++boundaries)for(u32 zero=0;zero<(1u<<count);++zero)for(u32 pattern=0;pattern<4;++pattern)for(float spacing:spacings){
        const bool rtl=direction!=0;SfntPlacement before[6]{},after[6]{};u8 flags[6]{};u32 cluster[6]{};u32 clusters=0;
        for(u32 i=0;i<count;++i){const bool boundary=!i||(boundaries&(1u<<(i-1)));if(boundary)++clusters;cluster[i]=clusters-1;
            const bool zero_advance=(zero&(1u<<i))!=0;
            before[i]=after[i]={zero_advance?0:values[(i+pattern)%4],static_cast<float>(i)*.7f,static_cast<float>(i)*-.2f};
            flags[i]=static_cast<u8>((boundary?sfnt_position_cluster_start:0)|(zero_advance?sfnt_position_zero_advance:0));}
        if(sfnt_trailing_spacing({after,count},{flags,count},spacing,1000000,rtl)!=SfntStatus::ok)return 1;
        // A cluster is spaced when it has an advancing glyph. A cluster moves right by the spacing of the spaced
        // clusters on its left, and in RTL also by its own, which lies on its left.
        bool spaced[6]{};for(u32 i=0;i<count;++i)spaced[cluster[i]]|=!(flags[i]&sfnt_position_zero_advance);
        u32 spaced_count=0;for(u32 c=0;c<clusters;++c)spaced_count+=spaced[c]?1u:0u;
        double x0[6]{},x1[6]{},width0=0,width1=0;visual(before,count,x0,&width0);visual(after,count,x1,&width1);
        const double tolerance=1e-6*(width0+spacing*clusters)+1e-4;
        bool match=magnitude(width1-(width0+static_cast<double>(spacing)*spaced_count))<=tolerance;
        for(u32 i=0;i<count;++i){u32 left=0;for(u32 c=0;c<clusters;++c)if(spaced[c]&&(rtl?c<=cluster[i]:c<cluster[i]))++left;
            match=match&&magnitude(x1[i]-(x0[i]+static_cast<double>(spacing)*left))<=tolerance&&after[i].ascender_offset==before[i].ascender_offset;}
        ++cases;
        if(!match){if(failed<16){if(failed)print(utf8_literal(","));field(utf8_literal("{\"rtl\":"),direction);field(utf8_literal(",\"count\":"),count);field(utf8_literal(",\"boundaries\":"),boundaries);field(utf8_literal(",\"zero\":"),zero);field(utf8_literal(",\"pattern\":"),pattern);u32 sb=0;copy_bytes(&sb,&spacing,4);field(utf8_literal(",\"spacing\":"),sb);
            print(utf8_literal(",\"rows\":["));for(u32 i=0;i<count;++i){if(i)print(utf8_literal(","));print(utf8_literal("["));const float observed[]{before[i].advance,after[i].advance,before[i].advance_offset,after[i].advance_offset};for(u32 j=0;j<4;++j){u32 b=0;copy_bytes(&b,observed+j,4);if(j)print(utf8_literal(","));number(b);}print(utf8_literal("]"));}print(utf8_literal("]}"));}++failed;}
    }
    field(utf8_literal("],\"cases\":"),cases);field(utf8_literal(",\"failed\":"),failed);field(utf8_literal(",\"validation_cases\":"),8);print(utf8_literal("}\n"));return output_ok&&!failed?0:1;
}
}}
extern "C" void maicord_native_spacing_entry(){ExitProcess(static_cast<UINT>(maicord::run()));}
