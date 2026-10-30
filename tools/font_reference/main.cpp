// QA-only native cmap/metrics differential oracle. DirectWrite is linked by
// this executable, never by maicord_font or MaiCordFontTests.
#include "maicord/text/sfnt.hpp"
#include "maicord/text/truetype_outline.hpp"
#include "maicord/text/cff_outline.hpp"
#include "maicord/text/color_font.hpp"
#include "maicord/text/colr_variations.hpp"
#include "maicord/text/font_axes.hpp"
#include "maicord/text/font_geometry.hpp"
#include "maicord/text/font_store.hpp"
#include "maicord/text/font_instance.hpp"
#include "maicord/text/tt_outline.hpp"
#include "maicord/text/tt_orientation.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/ui/text.hpp"
#include "maicord/render/native_glyph_outlines.hpp"
#include "dwrite_text_system.hpp"
#include "maicord/text/font_run.hpp"
#include <dwrite_3.h>
#include <d2d1.h>
#include "program_oracle.hpp"

namespace maicord {
namespace {

wchar_t path[32768];
bool owned_fonts=false;
u32 scalars[4096];
u16 native_glyphs[4096];
DWRITE_GLYPH_METRICS native_metrics[4096];
SfntOutlinePoint outline_points[65536];
u32 contour_ends[32768];
u16 active_glyphs[64];
GlyphPathCommand candidate_commands[131072], reference_commands[131072];
u64 outlines_compared = 0, outline_failures = 0, outline_differences = 0;
u64 outline_faces_unsupported = 0;
u64 coordinate_differences = 0, structure_differences = 0;
u64 bounds_differences = 0;
u64 cff_metrics_compared = 0, cff_metric_differences = 0;
u64 palette_entries_compared=0,palette_differences=0;
u64 color_roots_compared=0,color_root_differences=0;
u64 color_value_hash=14695981039346656037ull;
DWRITE_COLOR_F palette_entries[4096];
double variation_scalars[32767];
ColrResolvedColorStop variation_stops[65535];
const ColrVariationInstance* color_resolution=nullptr;
u64 color_variation_samples=0;
u64 windows_variation_clips=0,windows_variation_clip_differences=0;
u64 windows_variation_alphas=0,windows_variation_alpha_differences=0;
u64 windows_variation_stops=0,windows_variation_stop_differences=0;
D2D1_GRADIENT_STOP windows_stops[65535];
constexpr double qa_variation_coordinates[]{-1,-.75,-1.0/3.0,-.25,0,.125,1.0/3.0,.5,.875,1};
constexpr u32 qa_variation_steps=10;
float maximum_coordinate_difference = 0;
PaintProgramComparison paint_programs{};
PaintProgramComparison native_paint_programs{};
PaintProgramComparison varied_paint_programs{};
bool native_source_only=false;
bool native_source_grid=false;
u32 gvar_weight=0;
bool gvar_dump=false;
bool geometry_only=false;
u32 simulation_flags=0;
bool renderer_outlines=false;
bool points_dump=false;
bool execute_instructions=false;
u32 instruction_state_glyph=0xffffffffu;
u32 first_outline_glyph=0,outline_glyph_count=65536;
i32 tt_stack[4096],tt_storage[4096],tt_cvt[8192],tt_prepared_storage[4096],tt_prepared_cvt[8192];
TtFunction tt_functions[4096];TtCallFrame tt_frames[64];TtPoint tt_points[65540],tt_twilight[4096],tt_prepared_twilight[4096];u32 tt_ends[32768];
u32 gvar_shared_points[65540],gvar_private_points[65540];
i32 gvar_x[65540],gvar_y[65540];
SfntGvarDelta gvar_deltas[131080];
double gvar_shared_scalars[4096];

void write(const char* text) noexcept {
    u32 count = 0;
    while (text[count]) ++count;
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, count, &written, nullptr);
}
void number(u64 value) noexcept {
    char digits[32]{};
    u32 at = 30;
    do { digits[at--] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    write(digits + at + 1);
}
void metric(const char* name, u64 value) noexcept { write(name); write("="); number(value); write("\r\n"); }
void signed_metric(const char* name, i32 value) noexcept {
    write(name); write("=");
    if (value < 0) { write("-"); number(static_cast<u64>(-static_cast<i64>(value))); }
    else number(static_cast<u32>(value));
    write("\r\n");
}

bool argument() noexcept {
    const wchar_t* command = GetCommandLineW();
    const bool quoted = *command == L'"';
    if (quoted) ++command;
    while (*command && (quoted ? *command != L'"' : *command != L' ' && *command != L'\t')) ++command;
    if (quoted && *command) ++command;
    while (*command == L' ' || *command == L'\t') ++command;
    const bool path_quoted = *command == L'"';
    if (path_quoted) ++command;
    u32 count = 0;
    while (*command && (path_quoted ? *command != L'"' : *command != L' ' && *command != L'\t')) {
        if (count == 32767) return false;
        path[count++] = *command++;
    }
    path[count] = 0;
    if (path_quoted) { if (*command != L'"') return false; ++command; }
    while (*command == L' ' || *command == L'\t') ++command;
    {const wchar_t* option=L"--simulate=";const wchar_t* probe=command;while(*option&&*option==*probe){++option;++probe;}
        if(!*option){if(*probe<L'0'||*probe>L'3'||(probe[1]&&probe[1]!=L' '&&probe[1]!=L'\t'))return false;simulation_flags=static_cast<u32>(*probe++-L'0');command=probe;while(*command==L' '||*command==L'\t')++command;}}
    // Bound outline probes without changing the default whole-font checks.
    {const wchar_t* option=L"--glyphs=";const wchar_t* probe=command;while(*option&&*option==*probe){++option;++probe;}
        if(!*option){u32 values[2]{};for(u32 i=0;i<2;++i){if(*probe<L'0'||*probe>L'9')return false;
            do{values[i]=values[i]*10+static_cast<u32>(*probe++-L'0');if(values[i]>65536)return false;}while(*probe>=L'0'&&*probe<=L'9');
            if(!i&&*probe++!=L',')return false;}
            if(!values[1]||values[0]>=65536||values[1]>65536-values[0]||(*probe&&*probe!=L' '&&*probe!=L'\t'))return false;
            first_outline_glyph=values[0];outline_glyph_count=values[1];command=probe;while(*command==L' '||*command==L'\t')++command;}}
    {const wchar_t* option=L"--owned";const wchar_t* probe=command;while(*option && *option==*probe){++option;++probe;}
        if(!*option && (!*probe || *probe==L' ' || *probe==L'\t')){owned_fonts=true;command=probe;while(*command==L' ' || *command==L'\t')++command;}}
    {const wchar_t* option=L"--renderer";const wchar_t* probe=command;while(*option&&*option==*probe){++option;++probe;}
        if(!*option&&(!*probe||*probe==L' '||*probe==L'\t')){
            renderer_outlines=true;command=probe;while(*command==L' '||*command==L'\t')++command;
        }}
    {const wchar_t* option=L"--points";const wchar_t* probe=command;while(*option&&*option==*probe){++option;++probe;}
        if(!*option&&(!*probe||*probe==L' '||*probe==L'\t')){if(renderer_outlines||owned_fonts)return false;points_dump=gvar_dump=true;command=probe;while(*command==L' '||*command==L'\t')++command;}}
    {const wchar_t* option=L"--instructed";const wchar_t* probe=command;while(*option && *option==*probe){++option;++probe;}
        if(!*option && (!*probe || *probe==L' ' || *probe==L'\t')){execute_instructions=true;command=probe;while(*command==L' ' || *command==L'\t')++command;}}
    if(*command){const wchar_t* option=L"--instructions";const wchar_t* probe=command;while(*option && *option==*probe){++option;++probe;}if(!*option){if(*probe){const wchar_t* state=L"-state=";const wchar_t* cursor=probe;while(*state && *state==*cursor){++state;++cursor;}if(!*state){u32 glyph=0;if(!*cursor)return false;while(*cursor){if(*cursor<L'0' || *cursor>L'9' || glyph>6553)return false;glyph=glyph*10+static_cast<u32>(*cursor++-L'0');}if(glyph>65535)return false;instruction_state_glyph=glyph;gvar_dump=true;geometry_only=execute_instructions=true;return count!=0;}const wchar_t* dump=L"-dump";while(*dump && *dump==*probe){++dump;++probe;}if(*dump || *probe)return false;gvar_dump=true;}geometry_only=true;execute_instructions=true;return count!=0;}}
    if(*command){const wchar_t* option=L"--geometry";const wchar_t* probe=command;while(*option && *option==*probe){++option;++probe;}if(!*option){if(*probe){const wchar_t* dump=L"-dump";while(*dump && *dump==*probe){++dump;++probe;}if(*dump || *probe)return false;gvar_dump=true;}geometry_only=true;return count!=0;}}
    if(*command){const wchar_t* option=L"--gvar";const wchar_t* probe=command;while(*option && *option==*probe){++option;++probe;}if(!*option){if(probe[0]!='4' && probe[0]!='6')return false;gvar_weight=probe[0]=='4'?400:600;if(probe[1]!='0' || probe[2]!='0')return false;probe+=3;if(*probe){const wchar_t* dump=L"-dump";while(*dump && *dump==*probe){++dump;++probe;}if(*dump || *probe)return false;gvar_dump=true;}return count!=0;}}
    if(*command){const wchar_t* option=L"--native-source";while(*option && *option==*command){++option;++command;}if(*option)return false;native_source_only=true;
        if(*command){const wchar_t* grid=L"-grid";while(*grid && *grid==*command){++grid;++command;}if(*grid || *command)return false;native_source_grid=true;}}
    return count!=0;
}

void color_hash_integer(u64 value,u32 bytes) noexcept {
    for(u32 i=0;i<bytes;++i){color_value_hash^=static_cast<u8>(value);color_value_hash*=1099511628211ull;value>>=8;}
}
void color_hash_float(double value) noexcept {u64 bits=0;copy_bytes(&bits,&value,8);color_hash_integer(bits,8);}
bool color_hash_clip(const ColrFont& font,u16 glyph) noexcept {
    ColrClip clip{};if(colr_clip(font,glyph,&clip)!=SfntStatus::ok)return false;
    if(color_resolution){ColrClip resolved{};if(colr_resolve_clip(*color_resolution,clip,&resolved)!=SfntStatus::ok)return false;clip=resolved;}
    color_hash_integer(clip.present?1:0,4);color_hash_integer(clip.variable?1:0,4);
    color_hash_float(clip.minimum_x);color_hash_float(clip.minimum_y);color_hash_float(clip.maximum_x);color_hash_float(clip.maximum_y);
    color_hash_integer(clip.variation_index,4);return true;
}
bool color_hash_tree(const ColrFont& font,u32 offset,u32 depth,u32* work) noexcept {
    if(!*work || depth>=64)return false;--*work;
    ColrPaint p{};if(colr_paint_read(font,offset,&p)!=SfntStatus::ok)return false;
    if(color_resolution){ColrPaintValues resolved{};if(colr_resolve_paint_values(*color_resolution,p,&resolved)!=SfntStatus::ok)return false;for(u32 i=0;i<6;++i)p.values[i]=resolved.values[i];}
    color_hash_integer(p.format,4);color_hash_integer(p.glyph,4);color_hash_integer(p.palette,4);
    color_hash_integer(p.mode,4);color_hash_integer(p.layer_count,4);color_hash_integer(p.variation_index,4);
    color_hash_integer(p.value_count,4);for(double value:p.values)color_hash_float(value);
    if(p.format==1) {
        for(u32 i=0;i<p.layer_count;++i){u32 child=0;if(colr_layer_paint(font,p.first_layer+i,&child)!=SfntStatus::ok || !color_hash_tree(font,child,depth+1,work))return false;}
    } else if(p.format>=4 && p.format<=9) {
        ColrColorLine line{};if(colr_color_line(font,p,&line)!=SfntStatus::ok)return false;
        color_hash_integer(line.extend,4);color_hash_integer(line.count,4);
        if(color_resolution && colr_resolve_color_stops(font,*color_resolution,line,{variation_stops,65535})!=SfntStatus::ok)return false;
        for(u32 i=0;i<line.count;++i) {
            if(!*work)return false;--*work;
            if(color_resolution){const auto& stop=variation_stops[i];color_hash_float(stop.position);color_hash_float(stop.alpha);color_hash_integer(stop.palette,4);color_hash_integer(stop.original_index,4);continue;}
            ColrColorStop stop{};
            if(colr_color_stop(font,line,i,&stop)!=SfntStatus::ok)return false;
            color_hash_float(stop.position);color_hash_float(stop.alpha);
            color_hash_integer(stop.palette,4);color_hash_integer(stop.variation_index,4);
        }
    } else if(p.format>=10) {
        if(p.format==11 && !color_hash_clip(font,p.glyph))return false;
        if(!color_hash_tree(font,p.child[0],depth+1,work))return false;
        if(p.format==32 && !color_hash_tree(font,p.child[1],depth+1,work))return false;
    }
    return true;
}

bool compare_variation_values(const ColrFont& font) noexcept {
    if(!font.variation_store)return true;
    ColrVariationData data{};const auto status=colr_variation_data_open(font,&data);
    metric("variation_store_status",static_cast<u32>(status));
    if(status!=SfntStatus::ok)return false;
    metric("variation_axis_count",data.store.axis_count);metric("variation_region_count",data.store.region_count);
    if(data.store.axis_count>2){metric("variation_grid_unsupported_axes",data.store.axis_count);return false;}
    const auto& samples=qa_variation_coordinates;
    const u32 count=data.store.axis_count==2?qa_variation_steps*qa_variation_steps:data.store.axis_count==1?qa_variation_steps:1u;
    const u64 saved_hash=color_value_hash;bool ok=true;
    for(u32 sample=0;ok && sample<count;++sample) {
        const double coordinates[]{samples[sample%qa_variation_steps],samples[sample/qa_variation_steps]};
        ColrVariationInstance instance{};
        if(colr_variation_instance_prepare(data,{coordinates,data.store.axis_count},{variation_scalars,32767},&instance)!=SfntStatus::ok){ok=false;break;}
        color_resolution=&instance;color_value_hash=14695981039346656037ull;
        for(u32 glyph=0;ok && glyph<font.glyph_count;++glyph) {
            u32 offset=0;const auto found=colr_base_paint(font,static_cast<u16>(glyph),&offset);
            if(found==SfntStatus::unsupported_sequence)continue;
            if(found!=SfntStatus::ok){ok=false;break;}
            u32 work=1000000;color_hash_integer(glyph,4);
            ok=color_hash_clip(font,static_cast<u16>(glyph)) && color_hash_tree(font,offset,0,&work);
        }
        color_resolution=nullptr;
        if(ok){metric("variation_sample",sample);metric("variation_value_hash",color_value_hash);++color_variation_samples;}
    }
    color_resolution=nullptr;color_value_hash=saved_hash;return ok;
}

#pragma float_control(precise,on,push)
bool compare_windows_variations(const SfntFace& face,const ColrFont& font,IDWriteFontFace7* reference) noexcept {
    if(!font.variation_store)return true;
    ColrVariationData data{};if(colr_variation_data_open(font,&data)!=SfntStatus::ok)return false;
    if(data.store.axis_count>2)return false;
    FontAxes axes{};FontAxisMappings mappings{};AxisSegmentMap map_storage[2]{};
    if(font_axes_open(face,&axes)!=SfntStatus::ok || axes.axis_count!=data.store.axis_count ||
       font_axis_mappings_open(face,axes.axis_count,{map_storage,2},&mappings)!=SfntStatus::ok)return false;
    const u32 count=axes.axis_count;
    IDWriteFontResource* resource=nullptr;
    if(FAILED(reference->GetFontResource(&resource)))return false;
    DWRITE_FONT_METRICS metrics{};reference->GetMetrics(&metrics);
    const auto& samples=qa_variation_coordinates;
    const u32 sample_count=count==2?qa_variation_steps*qa_variation_steps:count==1?qa_variation_steps:1u;bool ok=true;
    for(u32 sample=0;ok && sample<sample_count;++sample) {
        DWRITE_FONT_AXIS_VALUE values[2]{};double coordinates[2]{};FontAxisSetting settings[2]{};
        for(u32 axis=0;axis<count;++axis) {
            FontAxis metadata{};if(font_axis(axes,axis,&metadata)!=SfntStatus::ok){ok=false;break;}
            const u32 tag=metadata.tag;
            values[axis].axisTag=static_cast<DWRITE_FONT_AXIS_TAG>((tag>>24)|((tag>>8)&0xff00)|((tag<<8)&0xff0000)|(tag<<24));
            const double low=metadata.minimum/65536.0,normal=metadata.normal/65536.0,high=metadata.maximum/65536.0;
            const double requested=samples[axis?sample/qa_variation_steps:sample%qa_variation_steps];
            values[axis].value=static_cast<float>(normal+requested*(requested<0?normal-low:high-normal));
            settings[axis]={tag,values[axis].value};
        }
        double fixed_coordinates[2]{};i16 condition_coordinates[2]{};
        if(!ok || font_axis_coordinates(axes,mappings,{settings,count},{fixed_coordinates,count},{condition_coordinates,count})!=SfntStatus::ok){ok=false;break;}
        for(u32 axis=0;axis<count;++axis) {
            u32 bits=0;copy_bytes(&bits,&values[axis].value,4);
            metric("axes_sample",sample);metric("axes_index",axis);metric("axes_user_float_bits",bits);
            metric("axes_fixed16_biased",static_cast<u32>(fixed_coordinates[axis]*65536+65536));
            metric("axes_condition_biased",static_cast<u32>(condition_coordinates[axis]+16384));
        }
        if(!ok || font_axis_coordinates(axes,mappings,{settings,count},{coordinates,count},{},FontAxisPrecision::full)!=SfntStatus::ok){ok=false;break;}
        ColrVariationInstance instance{};
        if(colr_variation_instance_prepare(data,{coordinates,count},{variation_scalars,32767},&instance,1000000,ColrVariationMode::quantized)!=SfntStatus::ok){ok=false;break;}
        IDWriteFontFace5* selected=nullptr;IDWriteFontFace7* selected7=nullptr;IDWritePaintReader* reader=nullptr;
        HRESULT hr=resource->CreateFontFace(DWRITE_FONT_SIMULATIONS_NONE,values,count,&selected);
        if(SUCCEEDED(hr))hr=selected->QueryInterface(__uuidof(IDWriteFontFace7),reinterpret_cast<void**>(&selected7));
        if(SUCCEEDED(hr))hr=selected7->CreatePaintReader(DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE,DWRITE_PAINT_FEATURE_LEVEL_COLR_V1,&reader);
        if(SUCCEEDED(hr))hr=reader->SetTextColor({1,1,1,1});
        if(FAILED(hr))ok=false;
        for(u32 glyph=0;ok && glyph<font.glyph_count;++glyph) {
            u32 offset=0;const auto found=colr_base_paint(font,static_cast<u16>(glyph),&offset);
            if(found==SfntStatus::unsupported_sequence)continue;
            if(found!=SfntStatus::ok){ok=false;break;}
            const u64 prior_differences=varied_paint_programs.differences;
            compare_paint_program(face,reader,selected7,static_cast<u16>(glyph),32,&varied_paint_programs,true,&instance);
            if(!prior_differences && varied_paint_programs.differences)metric("varied_first_sample",sample);
            ColrClip raw{},clip{};DWRITE_PAINT_ELEMENT root{};D2D_RECT_F box{};
            if(colr_clip(font,static_cast<u16>(glyph),&raw)!=SfntStatus::ok || colr_resolve_clip(instance,raw,&clip)!=SfntStatus::ok ||
               FAILED(reader->SetCurrentGlyph(glyph,&root,&box))){ok=false;break;}
            const float scale=1.0f/metrics.designUnitsPerEm;
            const float actual[]{static_cast<float>(clip.minimum_x)*scale,-static_cast<float>(clip.maximum_y)*scale,
                                 static_cast<float>(clip.maximum_x)*scale,-static_cast<float>(clip.minimum_y)*scale};
            const float expected[]{box.left,box.top,box.right,box.bottom};
            bool different=false;
            for(u32 i=0;i<4;++i)if(actual[i]!=expected[i]){
                different=true;
                if(windows_variation_clip_differences<2){u32 a=0,b=0;copy_bytes(&a,&actual[i],4);copy_bytes(&b,&expected[i],4);metric("windows_clip_field",i);metric("windows_clip_candidate_bits",a);metric("windows_clip_reference_bits",b);}
            }
            ++windows_variation_clips;
            if(different){if(windows_variation_clip_differences<8){metric("windows_clip_sample",sample);metric("windows_clip_glyph",glyph);}++windows_variation_clip_differences;}
            ColrPaint paint{};ColrPaintValues resolved{};
            if(colr_paint_read(font,offset,&paint)!=SfntStatus::ok){ok=false;break;}
            if(paint.format==2 || paint.format==3) {
                if(colr_resolve_paint_values(instance,paint,&resolved)!=SfntStatus::ok){ok=false;break;}
                CpalColor color{255,255,255,255};
                if(paint.palette!=0xffff && cpal_color(font.palette,0,paint.palette,&color)!=SfntStatus::ok){ok=false;break;}
                const float alpha=static_cast<float>(resolved.values[0])*(color.alpha/255.0f);
                ++windows_variation_alphas;
                if(root.paintType!=DWRITE_PAINT_TYPE_SOLID || alpha!=root.paint.solid.value.a) {
                    if(windows_variation_alpha_differences<8){metric("windows_alpha_sample",sample);metric("windows_alpha_glyph",glyph);metric("windows_alpha_root_type",static_cast<u32>(root.paintType));u32 a=0,b=0;copy_bytes(&a,&alpha,4);copy_bytes(&b,&root.paint.solid.value.a,4);metric("windows_alpha_candidate_bits",a);metric("windows_alpha_reference_bits",b);}
                    ++windows_variation_alpha_differences;
                }
            }
            if(paint.format>=4 && paint.format<=9) {
                ColrColorLine line{};
                if(colr_color_line(font,paint,&line)!=SfntStatus::ok ||
                   colr_resolve_color_stops(font,instance,line,{variation_stops,65535})!=SfntStatus::ok){ok=false;break;}
                const u32 expected_count=root.paintType==DWRITE_PAINT_TYPE_LINEAR_GRADIENT?root.paint.linearGradient.gradientStopCount:
                    root.paintType==DWRITE_PAINT_TYPE_RADIAL_GRADIENT?root.paint.radialGradient.gradientStopCount:
                    root.paintType==DWRITE_PAINT_TYPE_SWEEP_GRADIENT?root.paint.sweepGradient.gradientStopCount:0;
                if(expected_count!=line.count){++windows_variation_stop_differences;continue;}
                if(FAILED(reader->GetGradientStops(0,line.count,windows_stops))){ok=false;break;}
                for(u32 i=0;i<line.count;++i) {
                    CpalColor color{255,255,255,255};const auto& stop=variation_stops[i];
                    if(stop.palette!=0xffff && cpal_color(font.palette,0,stop.palette,&color)!=SfntStatus::ok){ok=false;break;}
                    const float position=static_cast<float>(stop.position),alpha=static_cast<float>(stop.alpha)*(color.alpha/255.0f);
                    ++windows_variation_stops;
                    if(position!=windows_stops[i].position || alpha!=windows_stops[i].color.a) {
                        if(windows_variation_stop_differences<8){metric("windows_stop_sample",sample);metric("windows_stop_glyph",glyph);metric("windows_stop_index",i);}
                        ++windows_variation_stop_differences;
                    }
                }
            }
        }
        com_release(reader);com_release(selected7);com_release(selected);
    }
    com_release(resource);return ok;
}
#pragma float_control(pop)

bool compare_color(const SfntFace& face,IDWriteFontFace* reference) noexcept {
    ConstSlice<u8> table{};
    const auto found=sfnt_table(face,sfnt_tag('C','O','L','R'),&table);
    if(found==SfntStatus::missing_table)return true;
    if(found!=SfntStatus::ok)return false;
    ColrFont font{};const auto opened=colr_open(face,&font);
    metric("color_open_status",static_cast<u32>(opened));
    if(opened!=SfntStatus::ok)return false;
    IDWriteFontFace7* oracle=nullptr;
    if(FAILED(reference->QueryInterface(__uuidof(IDWriteFontFace7),reinterpret_cast<void**>(&oracle))))return false;
    bool ok=oracle->GetColorPaletteCount()==font.palette.palette_count && oracle->GetPaletteEntryCount()==font.palette.entry_count;
    for(u32 palette=0;ok && palette<font.palette.palette_count;++palette) {
        for(u32 first=0;ok && first<font.palette.entry_count;first+=4096) {
            const u32 count=minimum<u32>(4096,font.palette.entry_count-first);
            if(FAILED(oracle->GetPaletteEntries(palette,first,count,palette_entries))){ok=false;break;}
            for(u32 i=0;i<count;++i) {
                CpalColor color{};
                if(cpal_color(font.palette,static_cast<u16>(palette),static_cast<u16>(first+i),&color)!=SfntStatus::ok){ok=false;break;}
                const auto e=palette_entries[i];
                // Compare source sRGB bytes only; float normalization and
                // compositing are not checked here.
                if(color.red!=static_cast<u32>(e.r*255+.5f) || color.green!=static_cast<u32>(e.g*255+.5f) ||
                   color.blue!=static_cast<u32>(e.b*255+.5f) || color.alpha!=static_cast<u32>(e.a*255+.5f))++palette_differences;
                ++palette_entries_compared;
            }
        }
    }
    IDWritePaintReader* reader=nullptr;
    if(ok && font.version && FAILED(oracle->CreatePaintReader(DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE,
        DWRITE_PAINT_FEATURE_LEVEL_COLR_V1,&reader)))ok=false;
    for(u32 glyph=0;ok && glyph<font.glyph_count;++glyph) {
        u32 offset=0;const auto status=colr_base_paint(font,static_cast<u16>(glyph),&offset);
        if(status==SfntStatus::unsupported_sequence)continue;
        if(status!=SfntStatus::ok){ok=false;break;}
        u32 hash_work=1000000;
        color_hash_integer(glyph,4);
        if(!color_hash_clip(font,static_cast<u16>(glyph)) || !color_hash_tree(font,offset,0,&hash_work)){ok=false;break;}
        if(reader) {
            DWRITE_PAINT_ELEMENT root{};D2D_RECT_F clip{};
            if(FAILED(reader->SetCurrentGlyph(static_cast<u16>(glyph),&root,&clip)) || root.paintType==DWRITE_PAINT_TYPE_NONE)++color_root_differences;
            ++color_roots_compared;
            constexpr float sizes[]{12,16,20,24,32,48,64,128,15.5f,31.25f};
            if(!native_source_only)for(float size:sizes)compare_paint_program(face,reader,reference,static_cast<u16>(glyph),size,&paint_programs);
            if(native_source_grid)for(float size:sizes)compare_paint_program(face,reader,reference,static_cast<u16>(glyph),size,&native_paint_programs,true);
            else compare_paint_program(face,reader,reference,static_cast<u16>(glyph),32,&native_paint_programs,true);
        }
    }
    if(ok)ok=compare_variation_values(font);
    if(ok)ok=compare_windows_variations(face,font,oracle);
    com_release(reader);com_release(oracle);return ok;
}

bool compare_face(const SfntFace& face, IDWriteFontFace* reference,
                    u64* compared, u64* differences, u64* metrics_compared,
                    u64* metric_differences,const NativeFontFace* owned=nullptr,const NativeFontInstance* owned_instance=nullptr,IDWriteFontFace* metric_reference=nullptr) noexcept {
    SfntCmap cmap{};
    SfntHorizontalMetrics metrics{};
    const auto cmap_status = owned ? (cmap=owned->cmap,SfntStatus::ok) : sfnt_cmap_open(face, &cmap);
    const auto metrics_status = sfnt_horizontal_metrics_open(face, &metrics);
    if (cmap_status != SfntStatus::ok || metrics_status != SfntStatus::ok) {
        metric("cmap_status", static_cast<u32>(cmap_status));
        metric("metrics_status", static_cast<u32>(metrics_status)); return false;
    }
    metric("cmap_format", cmap.format);
    if(first_outline_glyph>=cmap.glyph_count)return false;
    if(!gvar_weight && !geometry_only && !compare_color(face,reference))return false;
    if (reference->GetGlyphCount() != cmap.glyph_count) return false;
    for (u32 start = gvar_weight || geometry_only?0x110000:0; start <= 0x10ffffu;) {
        u32 count = 0;
        while (count < 4096 && start <= 0x10ffffu) {
            if (start < 0xd800u || start > 0xdfffu) scalars[count++] = start;
            ++start;
        }
        if (FAILED(reference->GetGlyphIndices(scalars, count, native_glyphs))) return false;
        for (u32 i = 0; i < count; ++i) {
            u16 glyph = 0;
            const auto status = sfnt_cmap_glyph(cmap, scalars[i], 0, &glyph);
            if (status != SfntStatus::ok || glyph != native_glyphs[i]) {
                if (*differences < 8) {
                    metric("different_scalar", scalars[i]); metric("candidate_glyph", glyph);
                    metric("reference_glyph", native_glyphs[i]); metric("status", static_cast<u32>(status));
                }
                ++*differences;
            }
            ++*compared;
        }
    }
    for (u32 start = gvar_weight || geometry_only?cmap.glyph_count:0; start < cmap.glyph_count; start += 4096) {
        const u32 count = minimum<u32>(4096, cmap.glyph_count - start);
        for (u32 i = 0; i < count; ++i) native_glyphs[i] = static_cast<u16>(start + i);
        if (FAILED(reference->GetDesignGlyphMetrics(native_glyphs, count, native_metrics, FALSE))) return false;
        for (u32 i = 0; i < count; ++i) {
            SfntGlyphMetrics m{};
            if (sfnt_horizontal_metrics(metrics, native_glyphs[i], &m) != SfntStatus::ok) return false;
            if (m.advance != native_metrics[i].advanceWidth || m.bearing != native_metrics[i].leftSideBearing) {
                if (*metric_differences < 64) {
                    metric("different_metric_glyph", native_glyphs[i]);
                    metric("candidate_advance", m.advance); metric("reference_advance", native_metrics[i].advanceWidth);
                    signed_metric("candidate_hmtx_bearing", m.bearing);
                    signed_metric("reference_ink_bearing", native_metrics[i].leftSideBearing);
                }
                ++*metric_differences;
            }
            ++*metrics_compared;
        }
    }
    FontGeometry geometry_storage{};const FontGeometry& geometry=owned?owned->geometry:geometry_storage;
    const bool cff_face = face.version == sfnt_tag('O','T','T','O');
    auto outline_status=owned?SfntStatus::ok:font_geometry_open(face,&geometry_storage);
    TtOutlineInstance tt_instance{};TtOutlineContext tt_context{};
    AxisSegmentMap axis_maps[16];double gvar_coordinates[16];SfntGvarInstance variation{};FontGeometryInstance instance{};
    if(owned_instance)instance=owned_instance->geometry;
    else if(outline_status==SfntStatus::ok){const FontAxisSetting setting{sfnt_tag('w','g','h','t'),static_cast<double>(gvar_weight)};
        outline_status=font_geometry_instance(geometry,gvar_weight?ConstSlice<FontAxisSetting>{&setting,1}:ConstSlice<FontAxisSetting>{},{{axis_maps,16},{gvar_coordinates,16},{gvar_shared_scalars,4096},&variation},&instance);}
    if(outline_status==SfntStatus::ok && execute_instructions && !cff_face){
        TtInterpreter prepared{};prepared.stack={tt_stack,4096};prepared.storage={tt_prepared_storage,4096};prepared.cvt={tt_prepared_cvt,8192};prepared.functions={tt_functions,4096};prepared.frames={tt_frames,64};prepared.twilight={tt_prepared_twilight,4096};
        if(owned_instance){if(!owned_instance->instructions)outline_status=SfntStatus::invalid_input;}
        else outline_status=tt_outline_prepare(geometry,&prepared,&tt_instance,&tt_context.execution);
        metric("instruction_prepare_status",static_cast<u32>(outline_status));metric("instruction_prepare_opcode",tt_context.execution.opcode);
        if(outline_status!=SfntStatus::ok){metric("instruction_prepare_offset",tt_context.execution.byte_offset);metric("instruction_prepare_depth",tt_context.execution.call_depth);metric("instruction_prepare_stack",tt_context.execution.stack_count);}
        if(outline_status==SfntStatus::ok)metric("instruction_control",owned_instance?owned_instance->instructions->graphics.instruction_control:tt_instance.graphics.instruction_control);
        if(points_dump)metric("instruction_prepare_missing_function",tt_context.execution.missing_function?1:0);
        if(points_dump&&outline_status==SfntStatus::ok){metric("instruction_instance_prep_recovered",(owned_instance?owned_instance->instructions:&tt_instance)->prep_recovered_missing_function?1:0);metric("instruction_instance_bytes",sizeof(TtOutlineInstance));}
        tt_context.instance=owned_instance?owned_instance->instructions:&tt_instance;tt_context.scratch.stack={tt_stack,4096};tt_context.scratch.storage={tt_storage,4096};tt_context.scratch.cvt={tt_cvt,8192};tt_context.scratch.twilight={tt_twilight,4096};tt_context.scratch.frames={tt_frames,64};tt_context.scratch.glyph={tt_points,65540};tt_context.contour_ends={tt_ends,32768};
    }
    metric("outline_open_status", static_cast<u32>(outline_status));
    if (outline_status == SfntStatus::ok) {
        NativeGlyphOutlines* renderer_owner=nullptr;NativeFontSession* renderer_fonts=nullptr;
        NativeFontRunHandle renderer_run{};NativeFontInstanceHandle renderer_instance{};Arena renderer_scratch{};
        struct ReleaseRenderer {NativeGlyphOutlines*& owner;NativeFontSession*& fonts;NativeFontRunHandle& run;NativeFontInstanceHandle& instance;Arena& scratch;
            ~ReleaseRenderer() noexcept {native_glyph_outlines_release(&owner);if(fonts){
                if(run.slot)native_font_run_release(native_font_session_runs(fonts),run);
                if(instance.slot)native_font_instance_release(native_font_session_instances(fonts),instance);
                native_font_session_release(&fonts);}arena_release(&scratch);}
        } release_renderer{renderer_owner,renderer_fonts,renderer_run,renderer_instance,renderer_scratch};
        if(renderer_outlines){NativeFontSelection source{};bool oblique=false,bold=false;
            if(!native_font_session_create(&renderer_fonts)||!arena_initialize(&renderer_scratch,4*1024*1024,0,4096)||
               !native_font_bind_selected(renderer_fonts,reference,false,&renderer_scratch,&renderer_instance,&oblique,&bold,&source).ok()||
               !source.face.slot||!native_font_session_run(renderer_fonts,renderer_instance,source,(bold?1u:0u)|(oblique?2u:0u),&renderer_run).ok())return false;}
        FontGeometryScratch scratch{{{outline_points,65536},{contour_ends,32768},{active_glyphs,64},0,0,1000000},
            {{gvar_shared_points,65540},{gvar_private_points,65540},{gvar_x,65540},{gvar_y,65540}},{gvar_deltas,131080}};
        if(execute_instructions && !cff_face)scratch.instructions=&tt_context;
        if(instruction_state_glyph!=0xffffffffu && (cff_face || instruction_state_glyph>=cmap.glyph_count))return false;
        GlyphOutline candidate{}, expected{};
        glyph_outline_initialize(&candidate, {candidate_commands, 131072});
        glyph_outline_initialize(&expected, {reference_commands, 131072});
        for (u32 i = first_outline_glyph; i < minimum<u32>(cmap.glyph_count,first_outline_glyph+outline_glyph_count); ++i) {
            if(instruction_state_glyph!=0xffffffffu && i!=instruction_state_glyph)continue;
            if(instruction_state_glyph!=0xffffffffu){const auto& tt=geometry.outline.truetype;const u32 begin=tt.loca_stride==4?byte_load_be32(tt.loca.data+i*4):byte_load_be16(tt.loca.data+i*2)*2u;
                const u32 end=tt.loca_stride==4?byte_load_be32(tt.loca.data+(i+1)*4):byte_load_be16(tt.loca.data+(i+1)*2)*2u;
                if(end-begin<10 || static_cast<i16>(byte_load_be16(tt.glyf.data+begin))<0)return false;
            }
            CffGlyphMetrics resolved{};
            SfntStatus native{};
            if(renderer_outlines)native=native_glyph_outline(&renderer_owner,1,static_cast<u16>(i),&candidate,renderer_fonts,renderer_run)?SfntStatus::ok:SfntStatus::unsupported;
            else native=font_geometry_outline(instance,static_cast<u16>(i),&scratch,&candidate,1000000,&resolved,(simulation_flags&2)!=0,(simulation_flags&1)!=0);
            const auto oracle = text_outline_glyph(reference, static_cast<u16>(i), &expected);
            if(gvar_dump&&oracle.ok()){write("gvar_outline=");number(i);write(",");number(expected.count);write(",");number(static_cast<u32>(expected.fill_rule));write(",");number(expected.has_bounds?1:0);
                const float bb[]{expected.minimum_x,expected.minimum_y,expected.maximum_x,expected.maximum_y};for(float v:bb){u32 bits=0;copy_bytes(&bits,&v,4);write(",");number(bits);}
                for(u32 j=0;j<expected.count;++j){const auto& c=expected.commands[j];const float values[]{c.x1,c.y1,c.x2,c.y2,c.x3,c.y3};for(float v:values){u32 bits=0;copy_bytes(&bits,&v,4);write(",");number(bits);}write(",");number(static_cast<u32>(c.kind));}write("\r\n");}
            if (native != SfntStatus::ok || !oracle.ok()) {
                if (outline_failures < 8) {
                    metric("outline_failed_glyph", i); metric("outline_status", static_cast<u32>(native));
                    metric("outline_reference_status", static_cast<u32>(oracle.error));
                    if(execute_instructions){metric("instruction_opcode",tt_context.execution.opcode);metric("instruction_offset",tt_context.execution.byte_offset);metric("instruction_depth",tt_context.execution.call_depth);}
                }
                ++outline_failures; continue;
            }
            ++outlines_compared;
            if(points_dump&&!cff_face){
                u32 orientation_work=0;const auto orientation_status=tt_mark_contour_directions({scratch.truetype.points.data,scratch.truetype.point_count},{scratch.truetype.contour_ends.data,scratch.truetype.contour_count},0,1000000,&orientation_work);
                write("native_contour_directions=");number(i);write(",");number(static_cast<u32>(orientation_status));write(",");number(orientation_work);write(",");number(scratch.truetype.contour_count);
                if(orientation_status==SfntStatus::ok)for(u32 j=0;j<scratch.truetype.contour_count;++j){write(",");number(scratch.truetype.points[j?scratch.truetype.contour_ends[j-1]+1:0].reserved[0]);}write("\r\n");
                write("native_points=");number(i);write(",");number(metrics.units_per_em);write(",");number(scratch.truetype.point_count);write(",");number(scratch.truetype.contour_count);
                for(u32 j=0;j<scratch.truetype.contour_count;++j){write(",");number(scratch.truetype.contour_ends[j]);}
                for(u32 j=0;j<scratch.truetype.point_count;++j){const auto& p=scratch.truetype.points[j];const float coordinates[]{p.x,p.y};
                    for(float v:coordinates){u32 bits=0;copy_bytes(&bits,&v,4);write(",");number(bits);}write(",");number(p.flags);}
                write("\r\n");
                write("native_outline=");number(i);write(",");number(candidate.count);write(",");number(static_cast<u32>(candidate.fill_rule));write(",");number(candidate.has_bounds?1:0);
                const float box[]{candidate.minimum_x,candidate.minimum_y,candidate.maximum_x,candidate.maximum_y};
                for(float v:box){u32 bits=0;copy_bytes(&bits,&v,4);write(",");number(bits);}
                for(u32 j=0;j<candidate.count;++j){const auto& c=candidate.commands[j];const float values[]{c.x1,c.y1,c.x2,c.y2,c.x3,c.y3};
                    for(float v:values){u32 bits=0;copy_bytes(&bits,&v,4);write(",");number(bits);}write(",");number(static_cast<u32>(c.kind));}write("\r\n");
            }
            if(instruction_state_glyph!=0xffffffffu && tt_context.execution.operations){
                write("tt_stack=");for(u32 j=0;j<tt_context.execution.stack_count;++j){if(j)write(",");const i32 v=tt_stack[j];if(v<0){write("-");number(static_cast<u64>(-static_cast<i64>(v)));}else number(static_cast<u32>(v));}write("\r\n");
                const auto functions=tt_context.instance->functions;for(u32 j=0;j<functions.count;++j)if(functions[j].defined){write("tt_function=");number(j);for(u32 k=0;k<functions[j].code.count;++k){write(",");number(functions[j].code[k]);}write("\r\n");}
                for(u32 j=0;j<scratch.truetype.point_count+4;++j){const auto& p=tt_points[j];write("tt_point=");number(j);const i32 values[]{p.x,p.y,p.original_x,p.original_y,p.touched};for(i32 v:values){write(",");if(v<0){write("-");number(static_cast<u64>(-static_cast<i64>(v)));}else number(static_cast<u32>(v));}write("\r\n");}
            }
            if (cff_face&&!renderer_outlines) {
                const u16 glyph=static_cast<u16>(i);
                DWRITE_GLYPH_METRICS expected_metrics{};
                // CffGlyphMetrics remains the unsimulated charstring design
                // box. The emitted GlyphOutline carries simulated ink bounds.
                if (FAILED((metric_reference?metric_reference:reference)->GetDesignGlyphMetrics(&glyph,1,&expected_metrics,FALSE))) return false;
                const auto floor_value=[](double value) noexcept -> i64 {
                    const i64 whole=static_cast<i64>(value); return whole-(static_cast<double>(whole)>value?1:0);
                };
                const auto ceil_value=[](double value) noexcept -> i64 {
                    const i64 whole=static_cast<i64>(value); return whole+(static_cast<double>(whole)<value?1:0);
                };
                const i64 actual[]{static_cast<i64>(resolved.advance_x+(resolved.advance_x<0?-.5:.5)),
                    floor_value(resolved.minimum_x),floor_value(resolved.minimum_y),
                    ceil_value(resolved.maximum_x),ceil_value(resolved.maximum_y)};
                const i64 expected_ink[]{expected_metrics.advanceWidth,expected_metrics.leftSideBearing,
                    static_cast<i64>(expected_metrics.verticalOriginY)-expected_metrics.advanceHeight+expected_metrics.bottomSideBearing,
                    static_cast<i64>(expected_metrics.advanceWidth)-expected_metrics.rightSideBearing,
                    static_cast<i64>(expected_metrics.verticalOriginY)-expected_metrics.topSideBearing};
                bool mismatch=resolved.advance_y!=0;
                for(u32 k=0;k<5;++k) if(actual[k]!=expected_ink[k]) {
                    mismatch=true;
                    if(cff_metric_differences<8) {
                        metric("cff_metric_glyph",i);metric("cff_metric_field",k);
                        signed_metric("cff_metric_candidate",static_cast<i32>(actual[k]));
                        signed_metric("cff_metric_reference",static_cast<i32>(expected_ink[k]));
                    }
                }
                ++cff_metrics_compared;
                if(mismatch) ++cff_metric_differences;
            }
            bool different = false, structure = candidate.count != expected.count || candidate.fill_rule != expected.fill_rule;
            const float candidate_bounds[]{candidate.minimum_x,candidate.minimum_y,candidate.maximum_x,candidate.maximum_y};
            const float reference_bounds[]{expected.minimum_x,expected.minimum_y,expected.maximum_x,expected.maximum_y};
            const bool bounds = candidate.has_bounds != expected.has_bounds ||
                                !bytes_equal(candidate_bounds, reference_bounds, sizeof(candidate_bounds));
            if (bounds) ++bounds_differences;
            u64 coordinates = 0;
            for (u32 j = 0; !structure && j < candidate.count; ++j) {
                const auto& a = candidate.commands[j]; const auto& b = expected.commands[j];
                if (a.kind != b.kind) { structure = true; break; }
                const float aa[]{a.x1,a.y1,a.x2,a.y2,a.x3,a.y3}, bb[]{b.x1,b.y1,b.x2,b.y2,b.x3,b.y3};
                for (u32 k = 0; k < 6; ++k) {
                    // Bit comparisons include signed zero for strict diagnostics.
                    if (!bytes_equal(&aa[k], &bb[k], sizeof(float))) {
                        ++coordinates;
                        if (outline_differences < 6 && coordinates <= 16) {
                            u32 a_bits = 0, b_bits = 0;
                            copy_bytes(&a_bits, &aa[k], sizeof(float)); copy_bytes(&b_bits, &bb[k], sizeof(float));
                            metric("detail_glyph", i); metric("detail_command", j); metric("detail_coordinate", k);
                            metric("candidate_float_bits", a_bits); metric("reference_float_bits", b_bits);
                        }
                        const float delta = aa[k] > bb[k] ? aa[k] - bb[k] : bb[k] - aa[k];
                        maximum_coordinate_difference = maximum(maximum_coordinate_difference, delta);
                    }
                }
            }
            coordinate_differences += coordinates;
            if (structure) {
                if (structure_differences < 8) {
                    metric("structure_glyph", i); metric("structure_candidate_count", candidate.count);
                    metric("structure_reference_count", expected.count);
                }
                ++structure_differences;
            }
            different = structure || coordinates || bounds;
            if (different) {
                if (outline_differences < 8) {
                    metric("outline_different_glyph", i); metric("candidate_commands", candidate.count);
                    metric("reference_commands", expected.count); metric("coordinate_differences", coordinates);
                }
                ++outline_differences;
            }
        }
    } else ++outline_faces_unsupported;
    return true;
}

int run() noexcept {
    if (!argument()) { write("Usage: MaiCordFontReference.exe <font-path>\r\n"); return 1; }
    NativeFontFileSlot file_slots[1]{};NativeFontFaceSlot face_slots[2]{};NativeFontStore owner{{file_slots,1},{face_slots,2}};
    struct ReleaseFonts {NativeFontStore& s;~ReleaseFonts() noexcept {for(u32 i=0;i<s.faces.count;++i)while(s.faces[i].references)native_font_release(&s,{i+1,s.faces[i].generation});}} release_fonts{owner};
    NativeFontInstanceSlot instance_slots[1]{};NativeFontInstanceStore instances{&owner,{instance_slots,1}};
    struct ReleaseInstances {NativeFontInstanceStore& s;~ReleaseInstances() noexcept {for(u32 i=0;i<s.slots.count;++i)while(s.slots[i].references)native_font_instance_release(&s,{i+1,s.slots[i].generation});}} release_instances{instances};
    const u8* data=nullptr;LARGE_INTEGER length{};NativeFontHandle root{};
    if(owned_fonts){const auto acquired=native_font_acquire(&owner,path,0,&root);
        metric("owner_acquire_error",static_cast<u32>(acquired.error));metric("owner_font_status",static_cast<u32>(acquired.font_status));metric("owner_native_error",acquired.native_error);
        if(!acquired.ok())return 2;const auto* face=native_font_get(owner,root);data=face->geometry.face.file.data;length.QuadPart=face->geometry.face.file.count;
    }else{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 2;
    if (!GetFileSizeEx(file, &length) || length.QuadPart <= 0 || length.QuadPart > 0xffffffffll) {
        CloseHandle(file); return 3;
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file);
    if (!mapping) return 4;
    data = static_cast<const u8*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    CloseHandle(mapping);
    if (!data) return 5;
    }
    IDWriteFactory* factory = nullptr;
    IDWriteFontFile* font_file = nullptr;
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown**>(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateFontFileReference(path, nullptr, &font_file);
    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE file_type{};
    DWRITE_FONT_FACE_TYPE face_type{};
    UINT32 count = 0;
    if (SUCCEEDED(hr)) hr = font_file->Analyze(&supported, &file_type, &face_type, &count);
    bool ok = SUCCEEDED(hr) && supported && count;
    u64 compared = 0, differences = 0, metrics_compared = 0, metric_differences = 0;
    for (u32 i = 0; ok && i < count; ++i) {
        IDWriteFontFace* reference = nullptr;IDWriteFontFace* metric_reference=nullptr;
        hr = factory->CreateFontFace(face_type, 1, &font_file, i, static_cast<DWRITE_FONT_SIMULATIONS>(simulation_flags), &reference);
        if(simulation_flags&&SUCCEEDED(hr))hr=factory->CreateFontFace(face_type,1,&font_file,i,DWRITE_FONT_SIMULATIONS_NONE,&metric_reference);
        if(gvar_weight && SUCCEEDED(hr)){IDWriteFontFace5* original=nullptr;IDWriteFontResource* resource=nullptr;IDWriteFontFace5* selected=nullptr;
            hr=reference->QueryInterface(__uuidof(IDWriteFontFace5),reinterpret_cast<void**>(&original));if(SUCCEEDED(hr))hr=original->GetFontResource(&resource);
            const DWRITE_FONT_AXIS_VALUE axis{DWRITE_FONT_AXIS_TAG_WEIGHT,static_cast<float>(gvar_weight)};if(SUCCEEDED(hr))hr=resource->CreateFontFace(static_cast<DWRITE_FONT_SIMULATIONS>(simulation_flags),&axis,1,&selected);
            if(SUCCEEDED(hr)){DWRITE_FONT_AXIS_VALUE values[16];const u32 axes=selected->GetFontAxisValueCount();bool found=false;if(axes>16)hr=E_FAIL;else{hr=selected->GetFontAxisValues(values,axes);if(SUCCEEDED(hr))for(u32 a=0;a<axes;++a)if(values[a].axisTag==DWRITE_FONT_AXIS_TAG_WEIGHT){found=values[a].value==static_cast<float>(gvar_weight);u32 bits=0;copy_bytes(&bits,&values[a].value,4);metric("gvar_reference_weight_bits",bits);}}if(!found)hr=E_FAIL;}
            if(SUCCEEDED(hr)){com_release(reference);reference=selected;selected=nullptr;}com_release(selected);com_release(resource);com_release(original);}
        SfntFace face{};
        NativeFontHandle handle{};const NativeFontFace* owned=nullptr;SfntStatus status=SfntStatus::ok;
        if(owned_fonts){const auto acquired=native_font_acquire(&owner,path,i,&handle);if(acquired.ok()){owned=native_font_get(owner,handle);face=owned->geometry.face;}else status=acquired.font_status==SfntStatus::ok?SfntStatus::resource_limit:acquired.font_status;}
        else status=sfnt_open({data, static_cast<u32>(length.QuadPart)}, i, &face);
        NativeFontInstanceHandle instance_handle{};const NativeFontInstance* owned_instance=nullptr;
        if(owned){AxisSegmentMap maps[16];double coordinates[16];const FontAxisSetting setting{sfnt_tag('w','g','h','t'),static_cast<double>(gvar_weight)};
            const auto acquired=native_font_instance_acquire(&instances,handle,gvar_weight?ConstSlice<FontAxisSetting>{&setting,1}:ConstSlice<FontAxisSetting>{},execute_instructions&&owned->geometry.kind==FontGeometryKind::truetype,
                {{maps,16},{coordinates,16},{tt_stack,4096},{tt_frames,64}},&instance_handle);
            metric("owner_instance_error",static_cast<u32>(acquired.error));metric("owner_instance_font_status",static_cast<u32>(acquired.font_status));
            if(acquired.ok()){owned_instance=native_font_instance_get(instances,instance_handle);metric("owner_instance_state_bytes",instance_slots[instance_handle.slot-1].state.committed);metric("owner_instance_reserved_bytes",instance_slots[instance_handle.slot-1].state.reserved);}
            else status=acquired.font_status==SfntStatus::ok?SfntStatus::resource_limit:acquired.font_status;
        }
        metric("face", i); metric("sfnt_status", static_cast<u32>(status));
        metric("reference_face_status", static_cast<u32>(hr));
        ok = SUCCEEDED(hr) && status == SfntStatus::ok && face.face_count == count &&
             compare_face(face, reference, &compared, &differences, &metrics_compared, &metric_differences,owned,owned_instance,metric_reference);
        if(instance_handle.slot && !native_font_instance_release(&instances,instance_handle))ok=false;
        if(handle.slot && !native_font_release(&owner,handle))ok=false;
        com_release(reference);
        com_release(metric_reference);
    }
    com_release(font_file); com_release(factory);
    if(owned_fonts){const auto retained=native_font_stats(owner);metric("owner_files",retained.files);metric("owner_faces",retained.faces);metric("owner_mapped_bytes",retained.mapped_bytes);
        if(!native_font_release(&owner,root))ok=false;const auto released=native_font_stats(owner);metric("owner_files_after_release",released.files);metric("owner_faces_after_release",released.faces);if(released.files||released.faces||released.references||released.mapped_bytes)ok=false;
    }else UnmapViewOfFile(data);
    metric("scalars_compared", compared); metric("mapping_differences", differences);
    metric("glyph_metrics_compared", metrics_compared); metric("metric_differences", metric_differences);
    metric("outlines_compared", outlines_compared); metric("outline_failures", outline_failures);
    metric("outline_faces_unsupported", outline_faces_unsupported);
    metric("outline_differences", outline_differences); metric("structure_differences", structure_differences);
    metric("coordinate_differences_total", coordinate_differences);
    metric("bounds_differences", bounds_differences);
    metric("cff_resolved_metrics_compared", cff_metrics_compared);
    metric("cff_resolved_metric_differences", cff_metric_differences);
    metric("palette_entries_compared",palette_entries_compared);metric("palette_differences",palette_differences);
    metric("color_roots_compared",color_roots_compared);metric("color_root_differences",color_root_differences);
    metric("color_value_hash",color_value_hash);
    metric("color_variation_samples",color_variation_samples);
    metric("windows_variation_clips",windows_variation_clips);metric("windows_variation_clip_differences",windows_variation_clip_differences);
    metric("windows_variation_alphas",windows_variation_alphas);metric("windows_variation_alpha_differences",windows_variation_alpha_differences);
    metric("windows_variation_stops",windows_variation_stops);metric("windows_variation_stop_differences",windows_variation_stop_differences);
    metric("maximum_coordinate_difference_nanounits", static_cast<u64>(maximum_coordinate_difference * 1.e9f));
    metric("paint_programs_compared",paint_programs.compared);metric("paint_program_failures",paint_programs.failures);
    metric("paint_program_adapter_enabled",native_source_only?0:1);
    metric("paint_program_differences",paint_programs.differences);metric("paint_program_bytes",paint_programs.bytes);
    metric("paint_program_first_different_glyph",paint_programs.first_glyph);metric("paint_program_first_different_offset",paint_programs.first_offset);
    metric("paint_program_first_original_word",paint_programs.first_original);metric("paint_program_first_candidate_word",paint_programs.first_candidate);
    metric("native_paint_programs_compared",native_paint_programs.compared);metric("native_paint_program_failures",native_paint_programs.failures);
    metric("native_paint_size_count",native_source_grid?10:1);
    metric("native_paint_program_differences",native_paint_programs.differences);metric("native_paint_program_bytes",native_paint_programs.bytes);
    metric("native_paint_first_glyph",native_paint_programs.first_glyph);metric("native_paint_first_offset",native_paint_programs.first_offset);
    metric("native_paint_first_original",native_paint_programs.first_original);metric("native_paint_first_candidate",native_paint_programs.first_candidate);
    metric("native_paint_node_kind",native_paint_programs.node_kind);metric("native_paint_node_field",native_paint_programs.node_field);
    metric("native_element_mismatches",native_paint_programs.element_mismatches);metric("native_element_kind",native_paint_programs.element_kind);
    metric("native_element_field",native_paint_programs.element_field);metric("native_element_original",native_paint_programs.element_original);
    metric("native_element_candidate",native_paint_programs.element_candidate);metric("native_element_paint",native_paint_programs.element_paint);
    metric("native_element_comparisons",native_paint_programs.element_comparisons);metric("native_element_failures",native_paint_programs.element_failures);
    metric("varied_paint_programs_compared",varied_paint_programs.compared);metric("varied_paint_program_failures",varied_paint_programs.failures);
    metric("varied_paint_program_differences",varied_paint_programs.differences);metric("varied_element_mismatches",varied_paint_programs.element_mismatches);
    metric("varied_paint_program_bytes",varied_paint_programs.bytes);
    metric("varied_element_failures",varied_paint_programs.element_failures);metric("varied_element_comparisons",varied_paint_programs.element_comparisons);
    metric("varied_first_glyph",varied_paint_programs.first_glyph);metric("varied_element_kind",varied_paint_programs.element_kind);metric("varied_element_field",varied_paint_programs.element_field);
    metric("varied_element_original",varied_paint_programs.element_original);metric("varied_element_candidate",varied_paint_programs.element_candidate);metric("varied_element_paint",varied_paint_programs.element_paint);
    for(u32 i=0;i<6;++i){metric("varied_root_field",i);metric("varied_root_original",varied_paint_programs.root_original[i]);metric("varied_root_candidate",varied_paint_programs.root_candidate[i]);}
    for(u32 i=0;i<6;++i){metric("native_paint_root_field",i);metric("native_paint_root_original",native_paint_programs.root_original[i]);metric("native_paint_root_candidate",native_paint_programs.root_candidate[i]);}
    return ok && !varied_paint_programs.element_failures && !varied_paint_programs.element_mismatches && !varied_paint_programs.failures && !varied_paint_programs.differences &&
           !native_paint_programs.element_failures && !native_paint_programs.element_mismatches && !native_paint_programs.failures && !native_paint_programs.differences && !paint_programs.failures && !paint_programs.differences && !differences && !metric_differences && !outline_failures && !outline_differences &&
           !outline_faces_unsupported && !cff_metric_differences && !palette_differences &&
           !color_root_differences && !windows_variation_clip_differences &&
           !windows_variation_alpha_differences && !windows_variation_stop_differences ? 0 : 6;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_font_reference_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
