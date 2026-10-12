#include "maicord/render/message_hit_test.hpp"

#include "maicord/core/utf.hpp"
#include "maicord/ui/message_text_presentation.hpp"
#include "maicord/ui/text.hpp"
#include "maicord/ui/native_text_candidate.hpp"
#include "maicord/text/paragraph_hits.hpp"

#include <limits.h>

#include <Windows.h>

namespace maicord {
namespace {

bool initialize_storage(MessageHitTestCacheOwner& owner) noexcept {
    return owner.storage.base||arena_initialize(&owner.storage,512*1024,0,4096);
}
struct NativeMessageGeometry {NativeParagraphHits hits;TextMeasureResult measure;float* widths;u16* bytes;};
template<class T> Slice<T> message_take(Arena& arena,u32 count) noexcept {return {count?arena_allocate<T>(&arena,count):nullptr,count};}
template<class T> ConstSlice<T> pack_message_slice(Arena& arena,ConstSlice<T> source) noexcept {
    auto* target=source.count?arena_allocate<T>(&arena,source.count):nullptr;
    if(source.count)copy_bytes(target,source.data,source.count*sizeof(T));return {target,source.count};
}
void compact_geometry(Arena& arena,NativeMessageGeometry& geometry) noexcept {
    if(arena.offset>16*1024){
        // All destinations move down within the same arena, in allocation order.
        // Publish only after packing; no caller can retain these private views.
        arena.offset=sizeof(NativeMessageGeometry);auto& h=geometry.hits;
        h.clusters=pack_message_slice(arena,h.clusters);h.lines=pack_message_slice(arena,h.lines);
        h.advances=pack_message_slice(arena,h.advances);
        if(h.style_count)h.styles=pack_message_slice(arena,ConstSlice<SfntFontLineParts>{h.styles,h.style_count}).data;
        h.visual_runs=pack_message_slice(arena,h.visual_runs);
        if(geometry.widths)geometry.widths=const_cast<float*>(pack_message_slice(arena,ConstSlice<float>{geometry.widths,h.lines.count}).data);
        geometry.bytes=const_cast<u16*>(pack_message_slice(arena,ConstSlice<u16>{geometry.bytes,h.source_units+1}).data);
    }
    const usize keep=(arena.offset+arena.commit_step-1)&~(arena.commit_step-1);
    // Release large temporary/high-water tails on cold rebuilds, never on hits.
    if(arena.committed>=keep+16*1024&&VirtualFree(arena.base+keep,arena.committed-keep,MEM_DECOMMIT))arena.committed=keep;
}
bool acquire_native(MessageHitTestCacheOwner& owner,const MessageTextCommand& command,const TextSystem& source,float width,float height,u32 units) noexcept {
    if(!source.native_text||!source.native_format||command.styles.count>kTextStyleSpanMaximum||(command.styles.count&&!command.styles.data))return false;
    auto system=source;
    system.tracking=0;system.independent_letters=false;system.fixed_weight=false;system.message_hit_policy=true;
    TextStyleSpan spans[kTextStyleSpanMaximum];u32 count=0;
    constexpr u32 position_flags=text_style_bold|text_style_italic|text_style_monospace|text_style_jumbo_emoji|text_style_heading_1|text_style_heading_2|text_style_heading_3|text_style_subtext|text_style_custom_emoji;
    for(u32 i=0;i<command.styles.count;++i){const auto& span=command.styles[i];u32 first=0,last=0;
        if(!span.byte_count||span.byte_offset>command.text.bytes||span.byte_count>command.text.bytes-span.byte_offset||
           !utf8_utf16_position_for_byte_offset(command.text,span.byte_offset,&first)||
           !utf8_utf16_position_for_byte_offset(command.text,span.byte_offset+span.byte_count,&last)||first>=last)return false;
        if(span.size>=text_size_count)return false;const u32 flags=text_style_fold_size(span.flags,span.size)&position_flags;
        if(flags)spans[count++]={span.byte_offset,span.byte_count,flags};}
    const ConstSlice<TextStyleSpan> styles{spans,count};
    // Shape exactly what drawing shaped (hidden default-ignorables included).
    const TextShapingView shaping(command.text);
    // Drawing's same-length substitute when the native items reject the text.
    TextNativeFallback fallback;Utf8View shaped=shaping.text;u32 fallback_stage=0;
    u32 style_capacity=native_text_candidate_style_capacity(shaping.text,styles),run_capacity=0;
    for(u32 attempt=0;attempt<3+TextNativeFallback::stages;++attempt){arena_reset(&owner.storage);
        auto* geometry=arena_allocate_zero<NativeMessageGeometry>(&owner.storage);if(!geometry)return false;
        auto clusters=message_take<NativeParagraphHitCluster>(owner.storage,units);
        auto lines=message_take<NativeParagraphHitLine>(owner.storage,units+1);
        auto advances=message_take<float>(owner.storage,units*4+16);
        auto parts=message_take<SfntFontLineParts>(owner.storage,style_capacity);
        auto runs=message_take<NativeParagraphHitRun>(owner.storage,run_capacity);
        const bool aligned=command.horizontal_alignment!=UiTextAlignment::start;
        auto widths=message_take<float>(owner.storage,aligned?units+1:0);
        if(!clusters.data||!lines.data||!advances.data||(style_capacity&&!parts.data)||(run_capacity&&!runs.data)||(aligned&&!widths.data))return false;
        u32 required_runs=0;NativeParagraphHitStorage storage{clusters,lines,advances,&geometry->hits,parts,runs,&required_runs,widths};
        const auto wrap=command.wrapping==UiTextWrapping::no_wrap?TextWrapping::no_wrap:TextWrapping::wrap;
        const bool built=count?native_text_candidate_styled(system,shaped,styles,width,height,nullptr,&geometry->measure,wrap,TextOverflow::clip,{},&storage):
            native_text_candidate_hits(system,shaped,width,height,wrap,storage,&geometry->measure);
        if(!built){const u32 required=native_text_candidate_hit_style_capacity(system);
            if(required>units||required_runs>units||(required<=style_capacity&&required_runs<=run_capacity)){
                while(fallback_stage<TextNativeFallback::stages&&!fallback.build(system,shaping.text,fallback_stage))++fallback_stage;
                if(fallback_stage++>=TextNativeFallback::stages)return false;shaped=fallback.text;continue;}
            style_capacity=maximum(style_capacity,required);run_capacity=maximum(run_capacity,required_runs);continue;}
        geometry->widths=widths.data;geometry->bytes=arena_allocate<u16>(&owner.storage,units+1);if(!geometry->bytes||geometry->hits.source_units!=units)return false;
        u32 byte=0,unit=0;geometry->bytes[0]=0;
        while(byte<command.text.bytes){const auto cp=utf8_decode_one(command.text.data+byte,command.text.bytes-byte);if(!cp.valid)return false;
            if(cp.codepoint>0xffff)geometry->bytes[++unit]=0xffff;byte+=cp.width;geometry->bytes[++unit]=static_cast<u16>(byte);}
        if(unit!=units)return false;compact_geometry(owner.storage,*geometry);owner.geometry=true;return true;
    }return false;
}
bool native_position(const MessageHitTestCacheOwner& owner,float x,float y,u32* position) noexcept {
    const auto& geometry=*reinterpret_cast<const NativeMessageGeometry*>(owner.storage.base);
    const float extra=owner.height-geometry.measure.height;
    if(owner.vertical_alignment==static_cast<u8>(UiTextAlignment::center))y-=extra*.5f;
    else if(owner.vertical_alignment==static_cast<u8>(UiTextAlignment::end))y-=extra;
    if(geometry.widths){u32 first=0,last=geometry.hits.lines.count;
        while(first<last){const u32 middle=first+(last-first)/2;if(geometry.hits.lines[middle].top<=y)first=middle+1;else last=middle;}
        const float remaining=owner.width-geometry.widths[first?first-1:0];
        x-=owner.horizontal_alignment==static_cast<u8>(UiTextAlignment::center)?remaining*.5f:remaining;}
    NativeParagraphHitPoint point{};
    if(native_paragraph_hit_point(geometry.hits,x,y,&point)!=SfntStatus::ok||point.source_position>owner.utf16_units)return false;
    const u32 byte=geometry.bytes[point.source_position];if(byte==0xffff)return false;*position=byte;return true;
}

bool cache_matches(const MessageHitTestCacheOwner& owner, const MessageTextCommand& command,
                   const TextSystem& text_system, u32 command_index, float width, float height) noexcept {
    return owner.geometry && text_system.native_text && text_system.native_format &&
           owner.native_format == native_text_format_identity(text_system) &&
           owner.message_id == command.message_id &&
           owner.measurement_identity == command.measurement_identity &&
           owner.message_revision == command.message_revision && owner.command_index == command_index &&
           owner.width == width && owner.height == height && owner.em_size == text_system.em_size &&
           owner.horizontal_alignment == static_cast<u8>(command.horizontal_alignment) &&
           owner.vertical_alignment == static_cast<u8>(command.vertical_alignment) &&
           owner.wrapping == static_cast<u8>(command.wrapping);
}

bool acquire_layout(MessageHitTestCacheOwner* owner, const MessageTextCommand& command,
                    const TextSystem& text_system, u32 command_index) noexcept {
    const float width = maximum(1.0f, ui_rect_width(command.rectangle));
    const float height = maximum(1.0f, ui_rect_height(command.rectangle));
    if (cache_matches(*owner, command, text_system, command_index, width, height)) {
        saturating_increment(owner->hits);
        return true;
    }

    message_hit_test_cache_invalidate(owner);
    u32 units=0;
    if(!utf8_utf16_position_for_byte_offset(command.text,command.text.bytes,&units)||!units||units>kMessageHitTestTextCapacity||!initialize_storage(*owner)||
       !acquire_native(*owner,command,text_system,width,height,units)){
        owner->geometry=false;saturating_increment(owner->rejected);return false;}
    owner->native_format=native_text_format_identity(text_system);
    owner->message_id = command.message_id;
    owner->measurement_identity = command.measurement_identity;
    owner->message_revision = command.message_revision;
    owner->command_index = command_index;
    owner->utf16_units = units;
    owner->width = width;
    owner->height = height;
    owner->em_size = text_system.em_size;
    owner->horizontal_alignment = static_cast<u8>(command.horizontal_alignment);
    owner->vertical_alignment = static_cast<u8>(command.vertical_alignment);
    owner->wrapping = static_cast<u8>(command.wrapping);
    saturating_increment(owner->builds);
    return true;
}

} // namespace

MessagePositionHit message_position_hit_test(MessageHitTestCacheOwner* owner,
                                             const MessageTextPresentation& presentation,
                                             const TextSystem& text_system, u32 dpi,
                                             i32 client_x_pixels, i32 client_y_pixels) noexcept {
    if (!owner || !presentation.active || dpi == 0 ||
        client_x_pixels < 0 || client_y_pixels < 0 ||
        presentation.command_count > presentation.commands.count) {
        return MessagePositionHit{};
    }
    const float x = static_cast<float>(client_x_pixels) * 96.0f / static_cast<float>(dpi);
    const float y = static_cast<float>(client_y_pixels) * 96.0f / static_cast<float>(dpi);
    for (u32 cursor = presentation.command_count; cursor != 0; --cursor) {
        const u32 command_index = cursor - 1;
        const MessageTextCommand& command = presentation.commands[command_index];
        if (command.message_id == 0 || command.text.bytes == 0 || !command.text.data ||
            command.measurement_kind == MessageTextMeasurementKind::reaction ||
            !ui_rect_has_area(command.rectangle) || !ui_rect_has_area(command.clip) ||
            x < command.rectangle.left || x >= command.rectangle.right || y < command.rectangle.top ||
            y >= command.rectangle.bottom || x < command.clip.left || x >= command.clip.right ||
            y < command.clip.top || y >= command.clip.bottom ||
            command.text.bytes > static_cast<u32>(INT_MAX)) {
            continue;
        }
        if (!acquire_layout(owner, command, text_system, command_index)) {
            continue;
        }
        // The nearest cluster for trailing whitespace and the unused part of a
        // wrapped line is still a valid browser-style drag-selection position.
        u32 byte_offset=0;
        if(!native_position(*owner,x-command.rectangle.left,y-command.rectangle.top,&byte_offset)){saturating_increment(owner->rejected);continue;}
        return MessagePositionHit{command.message_id,
                                  command.measurement_identity,
                                  command.message_revision,
                                  command_index,
                                  byte_offset,
                                  command.text.bytes,
                                  true};
    }
    return MessagePositionHit{};
}

void message_hit_test_cache_invalidate(MessageHitTestCacheOwner* owner) noexcept {
    if (!owner) {
        return;
    }
    arena_reset(&owner->storage);
    owner->native_format = 0;
    owner->message_id = 0;
    owner->measurement_identity = 0;
    owner->message_revision = 0;
    owner->command_index = 0;
    owner->utf16_units = 0;
    owner->width = 0.0f;
    owner->height = 0.0f;
    owner->em_size = 0.0f;
    owner->horizontal_alignment = 0;
    owner->vertical_alignment = 0;
    owner->wrapping = 0;
    owner->geometry = false;
}

void message_hit_test_cache_release(MessageHitTestCacheOwner* owner) noexcept {
    if (!owner) {
        return;
    }
    message_hit_test_cache_invalidate(owner);
    arena_release(&owner->storage);
    owner->builds = 0;
    owner->hits = 0;
    owner->rejected = 0;
}

} // namespace maicord
