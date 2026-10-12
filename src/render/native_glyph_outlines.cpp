#include "maicord/render/native_glyph_outlines.hpp"
#include "maicord/render/glyph_font_limits.hpp"
#include "maicord/text/font_instance.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/log.hpp"
#include "maicord/text/font_run.hpp"
#include "maicord/render/colr_source.hpp"

#include <Windows.h>
namespace maicord {
namespace {constexpr u32 capacity=glyph_font_maximum_capacity;}
struct NativeColorOutline {
    NativeFontInstanceHandle instance;u32 key;
    GlyphOutline outline;GlyphPathCommand commands[32];
};
static_assert(64*sizeof(NativeColorOutline)<=65536);
struct NativeColorState {
    ColrFont font;
    ColrVariationInstance variation;
    ColrSourceFrame frames[65];
    Arena scalars,stops;
    NativeFontInstanceHandle binding;
    NativeColorOutline* outlines;
    u64 builds,outline_hits;
    bool outline_cache_attempted;
};
static_assert(sizeof(NativeColorState)<=4096); // Traversal links and font state fit one committed page.
struct NativeGlyphOutlines {
    NativeFontInstanceHandle bindings[capacity];NativeFontSession* fonts;Arena scratch;
    u64 decodes,retries,failures,native_bindings,instruction_requests,instruction_preparations;
    u32 oblique_faces[(capacity+31)/32],bold_faces[(capacity+31)/32],prepared_faces[(capacity+31)/32];
    NativeColorState* color;
};
static_assert(sizeof(NativeGlyphOutlines)<=4096); // Expanded IDs still fit the existing committed page.
namespace {
template<class T> Slice<T> take(Arena& a,u32 n) noexcept {return {n?arena_allocate<T>(&a,n):nullptr,n};}
bool failed(NativeGlyphOutlines& o,u32 stage,u32 error) noexcept {++o.failures;log_u64(LogLevel::error,"render","native_outline_stage",stage);log_u64(LogLevel::error,"render","native_outline_error",error);return false;}
bool bind(NativeGlyphOutlines& o,u32 index,NativeFontRunHandle run,bool instructions=false) noexcept {
    bool oblique=false,bold=false;NativeFontResult result{};NativeFontInstanceHandle prepared{};
    result=native_font_run_prepare(native_font_session_runs(o.fonts),run,&o.scratch,&prepared,&oblique,&bold,instructions);
    if(!result.ok())return failed(o,12,result.native_error?result.native_error:static_cast<u32>(result.font_status));
    if(!o.bindings[index].slot)++o.native_bindings;
    if(o.bindings[index].slot)native_font_instance_release(native_font_session_instances(o.fonts),o.bindings[index]);
    o.bindings[index]=prepared;
    if(instructions){o.prepared_faces[index/32]|=1u<<(index%32);++o.instruction_preparations;}
    if(oblique)o.oblique_faces[index/32]|=1u<<(index%32);
    if(bold)o.bold_faces[index/32]|=1u<<(index%32);
    return true;
}
SfntStatus decode(NativeGlyphOutlines& o,const NativeFontInstance& instance,u16 glyph,GlyphOutline* output,u32 points,u32 contours,SfntOutlineMode mode,bool oblique,bool bold) noexcept {
    arena_reset(&o.scratch);FontGeometryScratch scratch{};TtOutlineContext context{};
    scratch.truetype.points=take<SfntOutlinePoint>(o.scratch,points);scratch.truetype.contour_ends=take<u32>(o.scratch,contours);scratch.truetype.active_glyphs=take<u16>(o.scratch,64);
    scratch.truetype.require_instructions=true;
    if(instance.geometry.font->gvar.table.data){const u32 count=points+4;scratch.variation={take<u32>(o.scratch,count),take<u32>(o.scratch,count),take<i32>(o.scratch,count),take<i32>(o.scratch,count)};
        scratch.deltas=take<SfntGvarDelta>(o.scratch,points*2+256);}
    if(instance.instructions){const auto& prepared=*instance.instructions;context.instance=&prepared;
        if(!(prepared.graphics.instruction_control&1)){context.scratch.stack=take<i32>(o.scratch,prepared.stack_capacity);context.scratch.frames=take<TtCallFrame>(o.scratch,64);
            context.scratch.cvt=take<i32>(o.scratch,prepared.cvt.count);context.scratch.storage=take<i32>(o.scratch,prepared.storage.count);context.scratch.twilight=take<TtPoint>(o.scratch,prepared.twilight.count);
            context.scratch.glyph=take<TtPoint>(o.scratch,points+4);context.contour_ends=take<u32>(o.scratch,contours);}
        scratch.instructions=&context;
    }
    if(mode==SfntOutlineMode::colr && instance.geometry.font->kind==FontGeometryKind::truetype && !instance.geometry.font->gvar.table.data) {
        scratch.truetype.work_limit=1000000;
        return sfnt_truetype_outline(instance.geometry.font->outline.truetype,glyph,&scratch.truetype,output,SfntOutlineMode::colr_commands,nullptr,scratch.instructions);
    }
    return font_geometry_outline(instance.geometry,glyph,&scratch,output,1000000,nullptr,oblique,bold);
}
bool prepare_owner(NativeGlyphOutlines** pointer,u16 face_id,NativeFontSession* fonts,NativeFontRunHandle run) noexcept {
    if(!pointer||!face_id||face_id>capacity||!fonts||!run.slot)return false;
    if(!*pointer){auto* o=static_cast<NativeGlyphOutlines*>(VirtualAlloc(nullptr,sizeof(NativeGlyphOutlines),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!o)return false;
        *o={};if(!arena_initialize(&o->scratch,4*1024*1024,0,4096)){VirtualFree(o,0,MEM_RELEASE);return false;}
        const bool shared=native_font_session_retain(fonts);if(shared)o->fonts=fonts;
        if(!shared){arena_release(&o->scratch);VirtualFree(o,0,MEM_RELEASE);return false;}*pointer=o;}
    auto& o=**pointer;if(fonts!=o.fonts)return false;const u32 index=face_id-1;if(!o.bindings[index].slot&&!bind(o,index,run))return false;
    return true;
}
bool outline_impl(NativeGlyphOutlines** pointer,u16 face_id,u16 glyph,GlyphOutline* output,SfntOutlineMode mode,NativeFontSession* fonts,NativeFontRunHandle run) noexcept {
    if(!output||!output->commands||!output->capacity||!prepare_owner(pointer,face_id,fonts,run))return false;
    auto& o=**pointer;const u32 index=face_id-1;
    const auto* instance=native_font_instance_get(*native_font_session_instances(o.fonts),o.bindings[index]);if(!instance)return failed(o,13,0);
    const bool oblique=(o.oblique_faces[index/32]&(1u<<(index%32)))!=0,bold=(o.bold_faces[index/32]&(1u<<(index%32)))!=0;
    if(bold&&!(o.prepared_faces[index/32]&(1u<<(index%32)))){
        if(!bind(o,index,run,true))return false;
        instance=native_font_instance_get(*native_font_session_instances(o.fonts),o.bindings[index]);if(!instance)return failed(o,13,0);
    }
    ++o.decodes;u32 points=1,contours=1;const u32 maximum_points=minimum<u32>(output->capacity,65535)*2+4,maximum_contours=minimum<u32>(output->capacity,65535);
    if(instance->geometry.font->kind==FontGeometryKind::truetype){ConstSlice<u8> maxp{};const auto s=sfnt_table(instance->geometry.font->face,sfnt_tag('m','a','x','p'),&maxp);
        if(s!=SfntStatus::ok||maxp.count<32)return failed(o,14,static_cast<u32>(s));
        points=maximum<u32>(1,minimum(maximum_points,maximum<u32>(byte_load_be16(maxp.data+6),byte_load_be16(maxp.data+10))));
        contours=maximum<u32>(1,minimum(maximum_contours,maximum<u32>(byte_load_be16(maxp.data+8),byte_load_be16(maxp.data+12))));}
    // The app's reference color renderer disables its simple-outline shortcut
    // for simulated faces and uses the text outline provider for each mask.
    if(mode==SfntOutlineMode::colr&&(oblique||bold))mode=SfntOutlineMode::text;
    SfntStatus status=SfntStatus::invalid_input;
    for(u32 attempt=0;attempt<3;++attempt){
        status=decode(o,*instance,glyph,output,points,contours,mode,oblique,bold);
        if(status==SfntStatus::instructions_required&&!(o.prepared_faces[index/32]&(1u<<(index%32)))){
            ++o.instruction_requests;if(!bind(o,index,run,true))return false;
            instance=native_font_instance_get(*native_font_session_instances(o.fonts),o.bindings[index]);if(!instance)return failed(o,13,0);
        }else if(status==SfntStatus::output_too_small&&(points<maximum_points||contours<maximum_contours)){
            ++o.retries;points=maximum_points;contours=maximum_contours;
        }else break;
    }
    return status==SfntStatus::ok?true:failed(o,15,static_cast<u32>(status));
}
}
bool native_glyph_outline(NativeGlyphOutlines** pointer,u16 face_id,u16 glyph,GlyphOutline* output,NativeFontSession* fonts,NativeFontRunHandle run) noexcept {
    return outline_impl(pointer,face_id,glyph,output,SfntOutlineMode::text,fonts,run);
}
bool native_color_glyph_build(NativeGlyphOutlines** pointer,u16 face_id,u16 glyph,
    const ColrColor& foreground,float pixels_per_em,ColrPaintProgram* program,bool* uses_foreground,NativeFontSession* fonts,NativeFontRunHandle run) noexcept {
    if(!program||!uses_foreground||!prepare_owner(pointer,face_id,fonts,run))return false;
    auto& o=**pointer;const auto binding=o.bindings[face_id-1];
    // Explicit emoji overrides reset style, but text-presentation fallback can
    // select a simulated color face. Preserve the app's existing mask-provider
    // behavior; do not add a transform to its paint graph or clip rectangles.
    const auto* instance=native_font_instance_get(*native_font_session_instances(o.fonts),binding);if(!instance)return failed(o,20,0);
    auto* instances=native_font_session_instances(o.fonts);if(!native_font_instance_retain(instances,binding))return failed(o,20,0);
    struct Retain {NativeFontInstanceStore* store;NativeFontInstanceHandle handle;~Retain() noexcept {native_font_instance_release(store,handle);}} retained{instances,binding};
    if(!o.color){
        auto* color=static_cast<NativeColorState*>(VirtualAlloc(nullptr,sizeof(NativeColorState),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        if(!color)return failed(o,21,GetLastError());
        if(!arena_initialize(&color->scalars,1024*1024,0,4096)||!arena_initialize(&color->stops,2*1024*1024,0,4096)){
            arena_release(&color->scalars);arena_release(&color->stops);VirtualFree(color,0,MEM_RELEASE);return failed(o,22,0);}
        o.color=color;
    }
    auto& c=*o.color;const auto& geometry=*instance->geometry.font;
    if(c.binding.slot!=binding.slot||c.binding.generation!=binding.generation){
        c.binding={};arena_reset(&c.scalars);
        auto status=colr_open(geometry.face,&c.font);if(status!=SfntStatus::ok)return failed(o,23,static_cast<u32>(status));
        ColrVariationData data{};status=colr_variation_data_open(c.font,&data);if(status!=SfntStatus::ok)return failed(o,24,static_cast<u32>(status));
        const ConstSlice<double> coordinates=instance->geometry.variation?instance->geometry.variation->coordinates:ConstSlice<double>{};
        auto scalars=take<double>(c.scalars,data.present?data.store.region_count:0);
        status=colr_variation_instance_prepare(data,coordinates,scalars,&c.variation,1000000,ColrVariationMode::paint_reader);
        if(status!=SfntStatus::ok)return failed(o,25,static_cast<u32>(status));c.binding=binding;
    }
    struct OutlineContext {NativeGlyphOutlines** owner;u16 face_id;NativeFontSession* fonts;NativeFontRunHandle run;} outline{pointer,face_id,fonts,run};
    const auto provider=+[](void* context,u16 glyph,GlyphOutline* output) noexcept {
        const auto& c=*static_cast<OutlineContext*>(context);
        if(!output||!output->commands||!output->capacity)return false;
        auto& owner=**c.owner;auto& color=*owner.color;
        if(color.builds<64)return outline_impl(c.owner,c.face_id,glyph,output,SfntOutlineMode::colr,c.fonts,c.run);
        const auto instance=owner.bindings[c.face_id-1];
        const u32 key=glyph|(static_cast<u32>(c.face_id)<<16),slot=(glyph*2654435761u^c.face_id*40503u)&63;
        auto* cached=color.outlines?color.outlines+slot:nullptr;
        if(cached&&cached->key==key&&cached->instance.slot==instance.slot&&cached->instance.generation==instance.generation&&cached->outline.count<=output->capacity){
            auto* commands=output->commands;const u32 output_capacity=output->capacity;*output=cached->outline;output->commands=commands;output->capacity=output_capacity;
            copy_bytes(commands,cached->commands,output->count*sizeof(GlyphPathCommand));++color.outline_hits;return true;
        }
        if(!outline_impl(c.owner,c.face_id,glyph,output,SfntOutlineMode::colr,c.fonts,c.run))return false;
        if(output->count>32)return true;
        if(!color.outline_cache_attempted){color.outline_cache_attempted=true;
            color.outlines=static_cast<NativeColorOutline*>(VirtualAlloc(nullptr,64*sizeof(NativeColorOutline),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));}
        if(color.outlines){auto& saved=color.outlines[slot];saved.key=key;saved.instance=owner.bindings[c.face_id-1];saved.outline=*output;
            saved.outline.commands=nullptr;saved.outline.capacity=0;copy_bytes(saved.commands,output->commands,output->count*sizeof(GlyphPathCommand));}
        return true;
    };
    ColrNativeSource source{};source.font=&c.font;source.variation=&c.variation;source.frames={c.frames,65};
    source.outline_context=&outline;source.outline_provider=provider;source.foreground=foreground;
    source.outline_capacity_font=geometry.kind==FontGeometryKind::truetype?&geometry.outline.truetype:nullptr;
    source.inverse_em=1.f/(geometry.kind==FontGeometryKind::truetype?geometry.outline.truetype.metrics.units_per_em:geometry.outline.cff.units_per_em);
    source.stop_arena=&c.stops;
    ColrPaintElement root{};ColrRect clip{};ColrPaintSource callbacks{};
    const auto status=colr_source_begin(&source,glyph,1000000,&root,&clip,&callbacks);
    if(status==SfntStatus::unsupported_sequence){
        // Ordinary glyphs in the selected color font retain their own outline.
        root={};root.kind=ColrPaintKind::solid_glyph;root.paint.solidGlyph={glyph,{foreground}};
        source.status=SfntStatus::ok;source.uses_foreground=true;
        callbacks={&outline,+[](void*,ColrPaintElement*) noexcept{return false;},
            +[](void*,ColrPaintElement*) noexcept{return false;},+[](void*) noexcept{return false;},
            +[](void*,Slice<ColrGradientStop>) noexcept{return false;},provider};
    }else if(status!=SfntStatus::ok)return failed(o,26,static_cast<u32>(status));
    if(!colr_program_build(program,callbacks,root,clip,pixels_per_em))return failed(o,27,static_cast<u32>(source.status));
    ++c.builds;*uses_foreground=source.uses_foreground;return true;
}
void native_glyph_outlines_release(NativeGlyphOutlines** pointer) noexcept {
    if(!pointer||!*pointer)return;auto* o=*pointer;
    log_u64(LogLevel::info,"render","native_outline_decodes",o->decodes);log_u64(LogLevel::info,"render","native_outline_retries",o->retries);log_u64(LogLevel::info,"render","native_outline_failures",o->failures);
    log_u64(LogLevel::info,"render","native_outline_scratch_commit",o->scratch.committed);
    log_u64(LogLevel::info,"render","native_outline_run_bindings",o->native_bindings);
    log_u64(LogLevel::info,"render","native_instruction_requests",o->instruction_requests);
    log_u64(LogLevel::info,"render","native_instruction_preparations",o->instruction_preparations);
    if(o->color){auto* c=o->color;log_u64(LogLevel::info,"render","native_colr_builds",c->builds);
        log_u64(LogLevel::info,"render","native_colr_outline_hits",c->outline_hits);
        log_u64(LogLevel::info,"render","native_colr_stop_commit",c->stops.committed);
        if(c->outlines)VirtualFree(c->outlines,0,MEM_RELEASE);
        arena_release(&c->scalars);arena_release(&c->stops);VirtualFree(c,0,MEM_RELEASE);}
    for(auto h:o->bindings)if(h.slot)native_font_instance_release(native_font_session_instances(o->fonts),h);
    if(!native_font_session_release(&o->fonts))log_write(LogLevel::error,"render","native_outline_session_has_live_handles");
    arena_release(&o->scratch);VirtualFree(o,0,MEM_RELEASE);*pointer=nullptr;
}
} // namespace maicord
