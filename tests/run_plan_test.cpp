// Native run plans (MaiCordRunPlanTests). No argument runs the built-in corpora; the
// registry also runs `<font> <weight> lazy-units` and `... profiles`. Manual modes, not run
// by the suite: plan-cost, emoji-plan-cost, nominal-cost, metric-cost and profile-cost
// (benchmarks), lazy-failure (needs a purpose-built font with exactly two GSUB lookups), and
// the audits line:<file>, ascii:<file> and metrics.
#include "maicord/text/run_plan.hpp"
#include "../src/text/nominal_placements.hpp"
#include "maicord/text/nominal_sources.hpp"
#include "maicord/text/ascii_run.hpp"
#include "maicord/text/ascii_line.hpp"
#include "maicord/text/latin_line.hpp"
#include "maicord/text/ascii_paragraph.hpp"
#include "maicord/text/latin_paragraph.hpp"
#include "maicord/text/alphabetic_run.hpp"
#include "maicord/text/emoji_run.hpp"
#include "maicord/text/detail/paragraph_layout.hpp"
#include "../src/text/script_paragraph.hpp"
#include "../src/text/unicode_line_geometry.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord {namespace {
#include "generated/lookup_plan_fixture.inc"
#include "generated/font_metrics_fixture.inc"
#include "generated/line_measurement_fixture.inc"
char print_buffer[65536];u32 print_count=0;bool print_ok=true;
void flush() noexcept {DWORD written=0;if(print_count&&(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),print_buffer,print_count,&written,nullptr)||written!=print_count))print_ok=false;print_count=0;}
void print_bytes(const char* data,u32 count) noexcept {while(count){if(print_count==sizeof(print_buffer))flush();const u32 n=minimum(count,static_cast<u32>(sizeof(print_buffer))-print_count);copy_bytes(print_buffer+print_count,data,n);print_count+=n;data+=n;count-=n;}}
void print(const char* s) noexcept {u32 n=0;while(s[n])++n;print_bytes(s,n);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){const char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}print_bytes(s,n);}
u64 checks=0,fonts=0,values=0,state_bytes=0;
#define CHECK(x) do {++checks;if(!(x)){print("FAIL line=");number(__LINE__);print(" font=");number(fonts);print("\n");return false;}} while(0)
NativeFontFileSlot files[2];NativeFontFaceSlot faces[2];NativeFontInstanceSlot instances[2];NativeRunPlanSlot plans[2];
NativeFontStore font_store{{files,2},{faces,2}};NativeFontInstanceStore instance_store{&font_store,{instances,2}};NativeRunPlanStore store{&instance_store,{plans,2}};
Arena scratch{};wchar_t temporary[MAX_PATH]{};
struct Reader {ConstSlice<u8> bytes;u32 at=0;bool word(u32* value) noexcept {if(bytes.count-at<4)return false;*value=byte_load_le32(bytes.data+at);at+=4;return true;}bool take(u32 count,ConstSlice<u8>* out) noexcept {if(count>bytes.count-at)return false;*out={bytes.data+at,count};at+=count;return true;}};
bool write_font(ConstSlice<u8> data) noexcept {HANDLE file=CreateFileW(temporary,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);if(file==INVALID_HANDLE_VALUE)return false;DWORD written=0;const bool ok=WriteFile(file,data.data,data.count,&written,nullptr)&&written==data.count;CloseHandle(file);return ok;}
bool acquire(const wchar_t* path,NativeRunPlanHandle* out,u32 weight=0) noexcept {
    NativeFontHandle face{};NativeFontInstanceHandle instance{};AxisSegmentMap maps[16];double coordinates[16];
    NativeFontPrepareScratch prepare{{maps,16},{coordinates,16},{},{}};FontAxisSetting setting{sfnt_tag('w','g','h','t'),static_cast<double>(weight)};
    CHECK(native_font_acquire(&font_store,path,0,&face).ok());
    CHECK(native_font_instance_acquire(&instance_store,face,weight?ConstSlice<FontAxisSetting>{&setting,1}:ConstSlice<FontAxisSetting>{},false,prepare,&instance).ok());
    const auto result=native_run_plan_acquire(&store,instance,&scratch,out);
    if(!result.ok()){print("plan_status=");number(static_cast<u32>(result.font_status));print("\n");}
    CHECK(result.ok());const auto& state=plans[out->slot-1].state;state_bytes+=state.committed;
    MEMORY_BASIC_INFORMATION info{};CHECK(VirtualQuery(state.base,&info,sizeof(info))==sizeof(info)&&info.Protect==PAGE_READONLY);
    NativeRunPlanHandle alias{};CHECK(native_run_plan_acquire(&store,instance,&scratch,&alias,1000000,0).ok()&&alias.slot==out->slot&&alias.generation==out->generation);
    CHECK(native_run_plan_release(&store,alias));
    if(!fonts){NativeRunPlanHandle emoji{},cached{},failed{};
        CHECK(!native_run_plan_acquire(&store,instance,&scratch,&failed,1000000,1,0,NativeRunPlanKind::emoji).ok()&&!failed.slot);
        CHECK(!native_run_plan_acquire(&store,instance,&scratch,&failed,1000000,0,0,static_cast<NativeRunPlanKind>(2)).ok()&&!failed.slot);
        CHECK(native_run_plan_acquire(&store,instance,&scratch,&emoji,1000000,16*1024*1024,0,NativeRunPlanKind::emoji).ok()&&emoji.slot!=out->slot);
        CHECK(!native_run_plan_nominal(store,emoji).glyph_limit);
        CHECK(native_run_plan_acquire(&store,instance,&scratch,&cached,1000000,0,0,NativeRunPlanKind::emoji).ok()&&cached.slot==emoji.slot&&cached.generation==emoji.generation);
        CHECK(VirtualQuery(plans[emoji.slot-1].state.base,&info,sizeof(info))==sizeof(info)&&info.Protect==PAGE_READONLY);
        CHECK(native_run_plan_release(&store,cached)&&native_run_plan_release(&store,emoji)&&!native_run_plan_get(store,emoji));
    }
    const auto* normal=native_run_plan_get(store,*out);const auto* independent=native_run_plan_get(store,*out,true);
    CHECK(normal&&independent&&normal->substitutions==independent->substitutions&&normal->positioning==independent->positioning&&normal->metric_context==independent->metric_context);
    CHECK(native_font_instance_release(&instance_store,instance)&&native_font_release(&font_store,face));
    CHECK(native_font_stats(font_store).files==1&&instances[instance.slot-1].references==1);
    const auto nominal=native_run_plan_nominal(store,*out);CHECK(nominal.ascii_count<=95);
    for(u32 i=0;i<nominal.ascii_count;++i){u16 glyph=0;const auto* selected=native_font_get(font_store,face);
        CHECK(selected&&sfnt_cmap_glyph(selected->cmap,32+i,0,&glyph)==SfntStatus::ok&&glyph==static_cast<u32>(nominal.ascii_first)+i&&glyph&&glyph<nominal.glyph_limit);}
    return true;
}
bool shape(NativeRunPlanHandle handle,bool independent,bool rtl,u32 expected_glyph,const u32* expected_placement) noexcept {
    const auto* plan=native_run_plan_get(store,handle,independent);CHECK(plan);
    const auto* instance=native_font_instance_get(instance_store,plans[handle.slot-1].instance);CHECK(instance);
    const auto* face=native_font_get(font_store,instance->face);CHECK(face);u16 glyph=0;CHECK(sfnt_cmap_glyph(face->cmap,'a',0,&glyph)==SfntStatus::ok);
    SfntClusterGlyph input{{glyph,sfnt_gdef_glyph_class(plan->gdef,glyph),0},0};
    SfntComponentGlyph records[16],replacements[16];SfntComponentNode nodes[1];SfntComponentUndo journal[128];
    u64 bits[2],cursive_bits[1];u32 characters[1],targets[16],positions[128],rebased[128],consumed[16],gp_positions[128];
    SfntMarkAssociation associations[16];double advances[16],prefix[16];u8 flags[16];SfntPosition placed[16];SfntPlacement output[16];
    SfntGsubExecutionFrame frames[16];SfntGposExecutionFrame gp_frames[16];SfntCursiveChainNode chains[16];
    SfntShapeStorage storage{{records,16},{nodes,1},{bits,2},{characters,1},{targets,16},{associations,16},{advances,16},{flags,16},{placed,16},{output,16},
        {{frames,16},{positions,128},{rebased,128},{consumed,16},{reinterpret_cast<u8*>(replacements),sizeof(replacements)},{journal,128}},
        {{gp_frames,16},{gp_positions,128},{cursive_bits,1},{prefix,16},{chains,16}}};
    SfntShapeRunResult result{};const auto status=sfnt_shape_run(*plan,{&input,1},1,{},rtl,1000,storage,&result);
    if(status!=SfntStatus::ok){print("shape_status=");number(static_cast<u32>(status));print("\n");}
    CHECK(status==SfntStatus::ok&&result.glyph_count);
    if(expected_placement){CHECK(result.glyph_count==1&&records[0].glyph.glyph==expected_glyph&&characters[0]==0&&bytes_equal(output,expected_placement,12));values+=5;}
    return true;
}
bool release(NativeRunPlanHandle h,bool kept=false) noexcept {
    const auto instance=plans[h.slot-1].instance;CHECK(native_run_plan_retain(&store,h)&&native_run_plan_release(&store,h));
    CHECK(native_run_plan_release(&store,h));CHECK(!store.substitutions);CHECK(!native_run_plan_get(store,h)&&!native_run_plan_retain(&store,h)&&!native_run_plan_release(&store,h));
    CHECK((native_font_instance_get(instance_store,instance)!=nullptr)==kept&&native_font_stats(font_store).files==(kept?1u:0u));return true;
}
struct AsciiBuffers {
    SfntClusterGlyph mapped[256];u16 glyphs[1024];SfntPlacement placements[1024];u32 map[256];
    SfntComponentGlyph records[1024],replacements[1024];SfntComponentNode nodes[256];SfntComponentUndo journal[4096];
    u64 bits[8],cursive_bits[16];u32 characters[256],targets[1024],positions[4096],rebased[4096],consumed[1024],gp_positions[4096];
    SfntMarkAssociation associations[1024];double advances[1024],prefix[1024];u8 flags[1024];SfntPosition placed[1024];SfntPlacement output[1024];
    SfntGsubExecutionFrame frames[64];SfntGposExecutionFrame gp_frames[64];SfntCursiveChainNode chains[1024];
    NativeAsciiStorage storage() noexcept {return {{mapped,256},
        {{records,1024},{nodes,256},{bits,8},{characters,256},{targets,1024},{associations,1024},{advances,1024},{flags,1024},{placed,1024},{output,1024},
         {{frames,64},{positions,4096},{rebased,4096},{consumed,1024},{reinterpret_cast<u8*>(replacements),sizeof(replacements)},{journal,4096}},
         {{gp_frames,64},{gp_positions,4096},{cursive_bits,16},{prefix,1024},{chains,1024}}},
        {glyphs,1024},{placements,1024},{map,256}};}
    NativeAsciiStorage shared() noexcept {auto result=storage();result.mapped={reinterpret_cast<SfntClusterGlyph*>(result.shaping.positions.data),256};result.glyphs={reinterpret_cast<u16*>(result.shaping.glyphs.data),1024};result.placements=result.shaping.placements;result.character_glyphs=result.shaping.character_glyphs;return result;}
} ascii_buffers;
NativeLineCluster line_clusters[256];
bool ascii_shared_case(NativeRunPlanHandle handle,Utf8View text,bool independent,float em,NativeAsciiStorage shared) noexcept {
    NativeAsciiResult separate_result{},shared_result{};
    const auto separate=ascii_buffers.storage();
    const auto expected=native_ascii_run(store,handle,text,independent,em,separate,&separate_result);
    const auto actual=native_ascii_run(store,handle,text,independent,em,shared,&shared_result);
    CHECK(actual==expected);
    CHECK(shared_result.glyph_count==separate_result.glyph_count && shared_result.source_units==separate_result.source_units &&
        shared_result.nominal_ranges==separate_result.nominal_ranges && shared_result.shaped_ranges==separate_result.shaped_ranges &&
        shared_result.lookup_operations==separate_result.lookup_operations);
    if(actual==SfntStatus::ok){
        CHECK(bytes_equal(shared.glyphs.data,separate.glyphs.data,shared_result.glyph_count*sizeof(u16)));
        CHECK(bytes_equal(shared.placements.data,separate.placements.data,shared_result.glyph_count*sizeof(SfntPlacement)));
        CHECK(bytes_equal(shared.character_glyphs.data,separate.character_glyphs.data,shared_result.source_units*sizeof(u32)));
    }
    return true;
}
bool ascii_shared_guards(NativeRunPlanHandle handle) noexcept {
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;
    const u32 record_bytes=sizeof(ascii_buffers.records),rounded=(record_bytes+page-1)/page*page;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,rounded+page*3,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(memory);
    struct Cleanup {u8* memory;~Cleanup() noexcept {VirtualFree(memory,0,MEM_RELEASE);}} cleanup{memory};
    DWORD old=0;CHECK(VirtualProtect(memory+rounded,page,PAGE_NOACCESS,&old));
    CHECK(VirtualProtect(memory+rounded+page*2,page,PAGE_NOACCESS,&old));
    auto* input=reinterpret_cast<char*>(memory+rounded+page*2-256);for(u32 i=0;i<256;++i)input[i]='a';
    CHECK(VirtualProtect(memory+rounded+page,page,PAGE_READONLY,&old));
    auto shared=ascii_buffers.shared();shared.shaping.glyphs={reinterpret_cast<SfntComponentGlyph*>(memory+rounded-record_bytes),1024};
    shared.glyphs={reinterpret_cast<u16*>(shared.shaping.glyphs.data),1024};
    const u32 lengths[]{7,9,65,256};
    for(u32 count:lengths)for(u32 mode=0;mode<2;++mode)
        CHECK(ascii_shared_case(handle,{input+256-count,count},mode!=0,17.5f,shared));
    return true;
}
bool ascii_fixture(NativeRunPlanHandle handle,bool independent,u32 glyph,const u32* placement) noexcept {
    NativeAsciiResult result{};auto storage=ascii_buffers.storage();
    CHECK(native_ascii_run(store,handle,{"a",1},independent,1000,storage,&result)==SfntStatus::ok&&result.glyph_count==1&&result.source_units==1);
    CHECK(ascii_buffers.glyphs[0]==glyph&&ascii_buffers.map[0]==0&&bytes_equal(ascii_buffers.placements,placement,12));values+=5;
    const auto shared=ascii_buffers.shared();NativeAsciiResult shared_result{};
    CHECK(native_ascii_run(store,handle,{"a",1},independent,1000,shared,&shared_result)==SfntStatus::ok && shared_result.glyph_count==1 && shared_result.source_units==1);
    CHECK(shared.glyphs[0]==glyph && shared.character_glyphs[0]==0 && bytes_equal(shared.placements.data,placement,12));
    if(result.nominal_ranges){auto no_shaping=storage;no_shaping.shaping={};
        CHECK(native_ascii_run(store,handle,{"a",1},independent,1000,no_shaping,&result)==SfntStatus::ok&&result.glyph_count==1&&bytes_equal(ascii_buffers.placements,placement,12));}
    if(result.lookup_operations){CHECK(native_ascii_run(store,handle,{"a",1},independent,1000,storage,&result,{0,100000,1000000})==SfntStatus::resource_limit&&!result.glyph_count&&!result.source_units);}
    CHECK(native_ascii_run(store,handle,{},independent,1000,storage,&result)==SfntStatus::ok&&!result.glyph_count&&!result.source_units);
    CHECK(native_ascii_run(store,{}, {"a",1},independent,1000,storage,&result)==SfntStatus::invalid_input&&!result.glyph_count);
    CHECK(native_ascii_run(store,handle,{"a",1},independent,-1,storage,&result)==SfntStatus::invalid_input&&!result.glyph_count);
    const char* rejected_inputs[]{"\n","\x7f","\xc3\xa9"};
    for(const char* rejected : rejected_inputs) {
        CHECK(native_ascii_run(store,handle,{rejected,1},independent,1000,storage,&result)==SfntStatus::unsupported_sequence&&!result.glyph_count&&!result.source_units);
    }
    auto short_output=storage;short_output.glyphs.count=0;
    CHECK(native_ascii_run(store,handle,{"a",1},independent,1000,short_output,&result)==SfntStatus::output_too_small&&!result.glyph_count&&!result.source_units);
    CHECK(native_ascii_run(store,handle,{"a",1},independent,1000,storage,&result,{1000000,100000,0})==SfntStatus::resource_limit&&!result.glyph_count&&!result.source_units);
    return true;
}
bool ascii_audit(NativeRunPlanHandle handle,const wchar_t* command,bool measure_line=false) noexcept {
    u32 em_bits=0,mode=0;
    while(*command>='0'&&*command<='9'){CHECK(em_bits<=(0xffffffffu-static_cast<u32>(*command-'0'))/10);em_bits=em_bits*10+*command++-'0';}
    CHECK(*command++==':'&&(*command=='0'||*command=='1'));mode=*command++-'0';CHECK(*command++==':');
    char text[256];u32 count=0;
    while(*command){u32 cp=0,digits=0;while(*command&&*command!=','){const wchar_t c=*command++;const u32 d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:16;CHECK(d<16&&cp<=15);cp=cp*16+d;++digits;}
        CHECK(digits&&count<256);text[count++]=static_cast<char>(cp);if(*command)++command;}
    float em=0;copy_bytes(&em,&em_bits,4);NativeAsciiResult result{};
    NativeAsciiLineResult line{};
    const auto buffers=ascii_buffers.shared();
    CHECK(ascii_shared_case(handle,{text,count},mode!=0,em,buffers));
    const auto status=measure_line?native_ascii_line(store,handle,{text,count},mode!=0,em,{buffers,{line_clusters,256}},&line):
        native_ascii_run(store,handle,{text,count},mode!=0,em,buffers,&result);
    if(measure_line)result=line.run;
    print("{\"status\":");number(static_cast<u32>(status));print(",\"glyphs\":[");
    for(u32 i=0;i<result.glyph_count;++i){if(i)print(",");number(buffers.glyphs[i]);}print("],\"clusters\":[");
    for(u32 i=0;i<result.source_units;++i){if(i)print(",");number(buffers.character_glyphs[i]);}print("],\"placement_bits\":[");
    for(u32 i=0;i<result.glyph_count;++i){u32 bits[3];copy_bytes(bits,buffers.placements.data+i,12);if(i)print(",");print("[");for(u32 j=0;j<3;++j){if(j)print(",");number(bits[j]);}print("]");}
    print("],\"nominal_ranges\":");number(result.nominal_ranges);print(",\"shaped_ranges\":");number(result.shaped_ranges);
    if(measure_line&&status==SfntStatus::ok){u32 fields[5];const float dims[]{line.width,line.width_including_trailing,line.vertical.height,line.vertical.baseline};copy_bytes(fields,dims,16);fields[4]=line.trailing_source_units;
        print(",\"line_metrics\":[");for(u32 i=0;i<5;++i){if(i)print(",");number(fields[i]);}print("],\"cluster_extents\":[");
        for(u32 i=0;i<line.cluster_count;++i){if(i)print(",");u32 bits[3];copy_bytes(bits,line_clusters+i,12);print("[");for(u32 j=0;j<3;++j){if(j)print(",");number(bits[j]);}print("]");}
        print("],\"position_hits\":[");for(u32 at=0;at<=result.source_units;++at)for(u32 side=0;side<2;++side){NativeLinePosition hit{};CHECK(native_ascii_line_position({line_clusters,line.cluster_count},line,at,side!=0,&hit)==SfntStatus::ok);u32 bits[5];copy_bytes(bits,&hit,20);if(at||side)print(",");print("[");for(u32 j=0;j<5;++j){if(j)print(",");number(bits[j]);}print("]");}print("]");}
    print("}\n");
    CHECK(status==SfntStatus::ok||status==SfntStatus::unsupported_sequence);return true;
}
bool corpus() noexcept {
    Reader r{{plan_fixture,sizeof(plan_fixture)},8};u32 count=0;CHECK(r.word(&count));
    for(u32 f=0;f<count;++f){u32 size=0,kind=0,script=0,language=0,found=0;ConstSlice<u8> font{};
        CHECK(r.word(&size)&&r.word(&kind)&&r.word(&script)&&r.word(&language)&&r.word(&found)&&r.take(size,&font));
        CHECK(write_font(font));NativeRunPlanHandle h{};CHECK(acquire(temporary,&h));
        for(u32 mode=0;mode<2;++mode){u32 independent=0,rtl=0,n=0,glyph=0,placement[3]{},nominal=0,layout_glyph=0,layout_placement[3]{};ConstSlice<u8> indices{};
            CHECK(r.word(&independent)&&r.word(&rtl)&&r.word(&n)&&r.word(&glyph)&&r.word(&placement[0])&&r.word(&placement[1])&&r.word(&placement[2])&&r.word(&nominal)&&r.word(&layout_glyph)&&r.word(&layout_placement[0])&&r.word(&layout_placement[1])&&r.word(&layout_placement[2])&&r.take(n*2,&indices));
            const auto* plan=native_run_plan_get(store,h,independent!=0);const auto roots=kind?plan->positioning_roots:plan->substitution_roots;
            CHECK(roots.count==n);for(u32 i=0;i<n;++i){CHECK(roots[i]==byte_load_le16(indices.data+i*2));++values;}
            CHECK(shape(h,independent!=0,rtl!=0,glyph,placement));
            const auto nominal_plan=native_run_plan_nominal(store,h);const auto* instance=native_font_instance_get(instance_store,plans[h.slot-1].instance);
            const auto* mapped_face=native_font_get(font_store,instance->face);u16 mapped=0;CHECK(sfnt_cmap_glyph(mapped_face->cmap,'a',0,&mapped)==SfntStatus::ok);
            const bool simple=mapped&&mapped<nominal_plan.glyph_limit&&nominal_scalar_is_simple('a')&&
                (!nominal_plan.participation.count||!(nominal_plan.participation[mapped/64]&(1ull<<(mapped%64))));
            CHECK(simple==(nominal!=0));
            CHECK(ascii_fixture(h,independent!=0,layout_glyph,layout_placement));
        }
        CHECK(ascii_shared_guards(h));
        const auto instance=plans[h.slot-1].instance;CHECK(native_font_instance_retain(&instance_store,instance));
        const usize committed=plans[h.slot-1].state.committed;CHECK(release(h,true));
        NativeRunPlanHandle failed{};CHECK(native_run_plan_acquire(&store,instance,&scratch,&failed,1000000,committed-1).font_status==SfntStatus::resource_limit&&!failed.slot);
        CHECK(!native_run_plan_acquire(&store,instance,&scratch,&failed,0).ok()&&!failed.slot);
        CHECK(native_font_instance_release(&instance_store,instance)&&!native_font_stats(font_store).files);
        ++fonts;
    }
    return r.at==r.bytes.count;
}
void signed_number(i32 value) noexcept {if(value<0){print("-");number(static_cast<u64>(-static_cast<i64>(value)));}else number(value);}
void metric_fields(const SfntFontLayoutMetrics& m,i32* output) noexcept {
    const i32 fields[]{m.units_per_em,m.ascent,m.descent,m.line_gap,m.underline_position,m.underline_thickness,m.strike_position,m.strike_thickness};copy_bytes(output,fields,sizeof(fields));
}
bool audit_metrics(NativeRunPlanHandle handle) noexcept {
    const auto* metrics=native_run_plan_metrics(store,handle);CHECK(metrics);i32 fields[8];metric_fields(*metrics,fields);
    print("{\"metrics\":[");for(u32 i=0;i<8;++i){if(i)print(",");signed_number(fields[i]);}print("],\"lines\":[");
    const float sizes[]{14,17.5f,1000};for(u32 i=0;i<3;++i){SfntSingleFontLine line{};CHECK(sfnt_single_font_line(*metrics,sizes[i],&line)==SfntStatus::ok);u32 bits[2];copy_bytes(bits,&line,8);if(i)print(",");print("[");number(bits[0]);print(",");number(bits[1]);print("]");}print("]}\n");return true;
}
bool prepared_static_advances(NativeRunPlanHandle handle) noexcept {
    const auto* instance=native_font_instance_get(instance_store,plans[handle.slot-1].instance);CHECK(instance);
    if(instance->geometry.variation)return true;
    const auto* font=native_font_get(font_store,instance->face);CHECK(font);SfntHorizontalMetrics metrics{};
    CHECK(sfnt_horizontal_metrics_open(font->geometry.face,&metrics)==SfntStatus::ok);
    for(u32 mode=0;mode<2;++mode){const auto* selected=native_run_plan_get(store,handle,mode!=0);CHECK(selected);
        for(u32 glyph=0;glyph<metrics.glyph_count;++glyph){SfntGlyphMetrics expected{};double actual=-1;
            CHECK(sfnt_horizontal_metrics(metrics,static_cast<u16>(glyph),&expected)==SfntStatus::ok);
            CHECK(selected->advance(selected->metric_context,static_cast<u16>(glyph),&actual)==SfntStatus::ok&&actual==expected.advance);}
        const u16 invalid[]{metrics.glyph_count,65535};
        for(u16 glyph:invalid){double value=1;CHECK(selected->advance(selected->metric_context,glyph,&value)==SfntStatus::glyph_out_of_range&&value==0);}
    }
    return true;
}
bool metric_corpus() noexcept {
    Reader r{{font_metrics_fixture,sizeof(font_metrics_fixture)},8};u32 count=0;CHECK(r.word(&count));
    for(u32 i=0;i<count;++i){u32 size=0,weight=0,expected[8];ConstSlice<u8> data{};CHECK(r.word(&size)&&r.word(&weight));for(u32 j=0;j<8;++j)CHECK(r.word(expected+j));CHECK(r.take(size,&data)&&write_font(data));
        NativeRunPlanHandle handle{};CHECK(acquire(temporary,&handle,weight)&&prepared_static_advances(handle));const auto* metrics=native_run_plan_metrics(store,handle);CHECK(metrics);i32 fields[8];metric_fields(*metrics,fields);
        for(u32 j=0;j<8;++j){CHECK(static_cast<u32>(fields[j])==expected[j]);++values;}
        for(u32 j=0;j<3;++j){u32 em_bits=0,bits[2];CHECK(r.word(&em_bits)&&r.word(bits)&&r.word(bits+1));float em=0;copy_bytes(&em,&em_bits,4);SfntSingleFontLine line{};
            CHECK(sfnt_single_font_line(*metrics,em,&line)==SfntStatus::ok&&bytes_equal(&line,bits,8));values+=2;
            CHECK(sfnt_single_font_line(*metrics,0,&line)==SfntStatus::invalid_input&&!line.height&&!line.baseline);}
        CHECK(release(handle)&&!native_run_plan_metrics(store,handle));++fonts;
    }
    CHECK(r.at==r.bytes.count);return true;
}
bool metric_guards() noexcept {
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(memory);
    struct Cleanup {u8* memory;~Cleanup() noexcept {VirtualFree(memory,0,MEM_RELEASE);}} cleanup{memory};
    DWORD old=0;CHECK(VirtualProtect(memory+page,page,PAGE_NOACCESS,&old));
    for(u32 table=0;table<2;++table){const u32 needed=table?12:78;
        for(u32 length=0;length<=needed;++length){u8 directory[32]{};
            byte_store_be32(directory,sfnt_tag('O','S','/','2'));byte_store_be32(directory+16,sfnt_tag('p','o','s','t'));
            byte_store_be32(directory+8,table?0:page-length);byte_store_be32(directory+12,table?78:length);
            byte_store_be32(directory+24,table?page-length:0);byte_store_be32(directory+28,table?length:12);
            SfntFace face{};face.file={memory,page};face.directory={directory,32};SfntHorizontalMetrics h{};h.units_per_em=1000;h.ascender=800;h.descender=-200;
            SfntFontLayoutMetrics metrics{};CHECK(sfnt_font_layout_metrics(face,h,nullptr,{},&metrics)==(length<needed?SfntStatus::truncated:SfntStatus::ok));
            if(length<needed)CHECK(!metrics.units_per_em&&!metrics.ascent&&!metrics.underline_thickness);
        }
    }
    return true;
}
bool line_corpus() noexcept {
    Reader r{{line_measurement_fixture,sizeof(line_measurement_fixture)},8};u32 count=0;CHECK(r.word(&count));
    for(u32 i=0;i<count;++i){u32 font_size=0,text_size=0,glyph_count=0,cluster_count=0,em_bits=0;ConstSlice<u8> font{},text{};
        CHECK(r.word(&font_size)&&r.word(&text_size)&&r.word(&glyph_count)&&r.word(&cluster_count)&&r.word(&em_bits)&&r.take(font_size,&font)&&r.take(text_size,&text)&&write_font(font));
        NativeRunPlanHandle h{};CHECK(acquire(temporary,&h));float em=0;copy_bytes(&em,&em_bits,4);NativeAsciiLineResult line{};const Utf8View input{reinterpret_cast<const char*>(text.data),text_size};
        NativeAsciiLineStorage storage{ascii_buffers.storage(),{line_clusters,256}};
        CHECK(native_ascii_line(store,h,input,false,em,storage,&line)==SfntStatus::ok&&line.run.glyph_count==glyph_count&&line.run.source_units==text_size&&line.cluster_count==cluster_count);
        for(u32 j=0;j<glyph_count;++j){u32 glyph=0,bits[3];CHECK(r.word(&glyph)&&r.word(bits)&&r.word(bits+1)&&r.word(bits+2));CHECK(ascii_buffers.glyphs[j]==glyph&&bytes_equal(ascii_buffers.placements+j,bits,12));values+=4;}
        for(u32 j=0;j<text_size;++j){u32 expected=0;CHECK(r.word(&expected)&&ascii_buffers.map[j]==expected);++values;}
        u32 expected[5],actual[5];for(u32 j=0;j<5;++j)CHECK(r.word(expected+j));const float dims[]{line.width,line.width_including_trailing,line.vertical.height,line.vertical.baseline};copy_bytes(actual,dims,16);actual[4]=line.trailing_source_units;CHECK(bytes_equal(actual,expected,20));values+=5;
        for(u32 j=0;j<cluster_count;++j){u32 bits[3];CHECK(r.word(bits)&&r.word(bits+1)&&r.word(bits+2)&&bytes_equal(line_clusters+j,bits,12));values+=3;}
        for(u32 at=0;at<=text_size;++at)for(u32 side=0;side<2;++side){NativeLinePosition hit{};u32 bits[5];for(u32 j=0;j<5;++j)CHECK(r.word(bits+j));CHECK(native_ascii_line_position({line_clusters,cluster_count},line,at,side!=0,&hit)==SfntStatus::ok&&bytes_equal(&hit,bits,20));values+=5;}
        NativeLinePosition hit{};CHECK(native_ascii_line_position({line_clusters,cluster_count},line,text_size+1,false,&hit)==SfntStatus::invalid_input&&!hit.source_count&&!hit.width);
        auto small=storage;small.clusters.count=cluster_count-1;CHECK(native_ascii_line(store,h,input,false,em,small,&line)==SfntStatus::output_too_small&&!line.run.glyph_count&&!line.cluster_count&&!line.width);
        CHECK(native_ascii_line(store,h,{},false,em,storage,&line)==SfntStatus::ok&&!line.run.source_units&&!line.width&&line.vertical.height>0);
        CHECK(native_ascii_line_position({},line,0,false,&hit)==SfntStatus::ok&&!hit.x&&!hit.source_count);
        CHECK(release(h));++fonts;
    }
    CHECK(r.at==r.bytes.count);return true;
}
bool lookup_bound_guards() noexcept {
    SfntGsubGlyph glyph{0,SfntGlyphClass::unassigned,0};const double nominal=1;const u8 flag=sfnt_position_cluster_start;SfntPosition position{1,0,0};
    SfntGposExecutableLookup lookup{};lookup.type=1;lookup.bounds.ready=1;
    SfntGposProgram program{{&lookup,1},{},1};SfntGposExecutionResult result{};
    auto execute=[&](u64 work) noexcept {return sfnt_gpos_execute_program_lookup(program,0,sfnt_glyph_view(ConstSlice<SfntGsubGlyph>{&glyph,1}),{&nominal,1},{&flag,1},{},{},{&position,1},0,1,false,{},SfntNullAnchorPolicy::parent_coordinates,SfntComponentAnchorPolicy::skip_missing,{},{work,100},&result);};
    CHECK(execute(1)==SfntStatus::ok&&result.operations==1&&!result.matches&&position.advance==1);
    CHECK(execute(0)==SfntStatus::resource_limit&&result.operations==0);
    lookup.bounds.ready=0;CHECK(execute(1)==SfntStatus::resource_limit);CHECK(execute(100)==SfntStatus::ok&&!result.matches&&position.advance==1);
    lookup.bounds.ready=1;glyph.glyph=1;CHECK(execute(100)==SfntStatus::invalid_input);glyph.glyph=0;
    glyph.glyph_class=static_cast<SfntGlyphClass>(5);CHECK(execute(100)==SfntStatus::invalid_input);glyph.glyph_class=SfntGlyphClass::unassigned;
    const u64 nan_bits=0x7ff8000000000000ull;copy_bytes(&position.advance,&nan_bits,8);CHECK(execute(100)==SfntStatus::invalid_input);
    return true;
}
u32 float_bits(float value) noexcept {u32 bits=0;copy_bytes(&bits,&value,4);return bits;}
#include "ascii_paragraph_cases.hpp"
#include "positioning_profile_units.inc"
#include "lazy_substitution_units.inc"
#include "nominal_metric_cost.inc"
#include "../tools/font_reference/metric_cost.inc"
bool metric_throughput(NativeRunPlanHandle handle) noexcept {
    const auto* plan=native_run_plan_get(store,handle);CHECK(plan);double advances[256];
    CHECK(metric_cost(native_run_plan_nominal(store,handle).glyph_limit,[&](const u16* ids,u32 count) noexcept {
        for(u32 i=0;i<count;++i)if(plan->advance(plan->metric_context,ids[i],advances+i)!=SfntStatus::ok)return false;return true;
    },[&](u32 i) noexcept {return static_cast<i64>(advances[i]);},print,number));
    return true;
}
bool run() noexcept {
    CHECK(nominal_metric_guards());
    CHECK(arena_initialize(&scratch,1024*1024,0,4096));
    const wchar_t* command=GetCommandLineW();bool quoted=false;while(*command){if(*command=='"')quoted=!quoted;else if(!quoted&&(*command==' '||*command=='\t'))break;++command;}while(*command==' '||*command=='\t')++command;
    if(*command){wchar_t path[32768];u32 n=0;quoted=*command=='"';if(quoted)++command;while(*command&&!(quoted?*command=='"':*command==' '||*command=='\t')){CHECK(n+1<32768);path[n++]=*command++;}path[n]=0;if(quoted&&*command=='"')++command;while(*command==' '||*command=='\t')++command;u32 weight=0;while(*command>='0'&&*command<='9'){CHECK(weight<10000);weight=weight*10+*command++-'0';}while(*command==' '||*command=='\t')++command;
        NativeRunPlanHandle h{};CHECK(acquire(path,&h,weight));if(lstrcmpW(command,L"lazy-failure")==0){CHECK(lazy_substitution_failure(h));++fonts;return true;}if(lstrcmpW(command,L"lazy-units")==0){CHECK(lazy_substitution_units(h));++fonts;return true;}if(lstrcmpW(command,L"emoji-plan-cost")==0){CHECK(emoji_plan_cost(h));++fonts;return true;}if(lstrcmpW(command,L"plan-cost")==0){CHECK(run_plan_cost(h));++fonts;return true;}if(*command){if(lstrcmpW(command,L"nominal-cost")==0){CHECK(nominal_metric_cost(h)&&release(h));++fonts;return true;}if(lstrcmpW(command,L"metric-cost")==0){CHECK(metric_throughput(h)&&release(h));++fonts;return true;}if(lstrcmpW(command,L"profile-cost")==0)CHECK(positioning_profile_cost(h));else if(lstrcmpW(command,L"profiles")==0)CHECK(positioning_profiles(h));else if(command[0]=='l'&&command[1]=='i'&&command[2]=='n'&&command[3]=='e'&&command[4]==':')CHECK(ascii_audit(h,command+5,true));else if(command[0]=='a'&&command[1]=='s'&&command[2]=='c'&&command[3]=='i'&&command[4]=='i'&&command[5]==':')CHECK(ascii_audit(h,command+6));else CHECK(lstrcmpW(command,L"metrics")==0&&audit_metrics(h));}CHECK(shape(h,false,false,0,nullptr)&&shape(h,true,false,0,nullptr));CHECK(release(h));++fonts;return true;}
    wchar_t folder[MAX_PATH];CHECK(GetTempPathW(MAX_PATH,folder)&&GetTempFileNameW(folder,L"mcp",0,temporary));return corpus()&&metric_corpus()&&metric_guards()&&line_corpus()&&lookup_bound_guards()&&paragraph_cases()&&ordered_paragraph_cases()&&latin_boundary_cases()&&styled_paragraph_cases();
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_run_plan_entry() noexcept {
    using namespace maicord;bool ok=run();
    for(u32 i=0;i<2;++i)while(plans[i].references)native_run_plan_release(&store,{i+1,plans[i].generation});
    for(u32 i=0;i<2;++i)while(instances[i].references)native_font_instance_release(&instance_store,{i+1,instances[i].generation});
    for(u32 i=0;i<2;++i)while(faces[i].references)native_font_release(&font_store,{i+1,faces[i].generation});
    arena_release(&scratch);if(temporary[0]&&!DeleteFileW(temporary))ok=false;
    print("fonts=");number(fonts);print("\nchecks=");number(checks);print("\nvalues=");number(values);print("\nstate_bytes_sum=");number(state_bytes);print("\nslot_bytes=");number(sizeof(NativeRunPlanSlot));
    print(ok?"\nPASS native run plans\n":"\nFAIL native run plans\n");flush();ExitProcess(ok&&print_ok?0:1);
}
