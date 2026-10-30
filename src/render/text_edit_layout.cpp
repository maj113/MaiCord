#include "maicord/render/text_edit_layout.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/utf.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/message_composer_scene.hpp"
#include "maicord/ui/text.hpp"
#include "maicord/ui/native_text_candidate.hpp"
#include "maicord/text/paragraph_hits.hpp"

namespace maicord {
bool text_composer_emoji_rectangle(const TextEditLayoutOwner& owner,
    const MessageComposerScene& scene, u32 index, UiRect* rectangle, UiRect* clip) noexcept {
    if (!rectangle || !clip || !scene.active || !owner.active || index >= scene.projection_span_count) return false;
    const auto& span = scene.projection_spans[index];
    const auto* field = message_composer_scene_layout_node(scene, MessageComposerControl::edit);
    if (!field || (span.kind != MessageComposerProjectionKind::emoji &&
                   span.kind != MessageComposerProjectionKind::regional_flag)) return false;
    const auto text = message_composer_scene_display_text(scene);
    u32 first = 0, last = 0, count = 0;
    TextEditMetric metric{};
    if (!utf8_utf16_position_for_byte_offset(text, span.display_first, &first) ||
        !utf8_utf16_position_for_byte_offset(text, span.display_last, &last) ||
        !text_edit_layout_range_geometry(owner, first, last, {&metric, 1}, &count) || !count) return false;
    const float extent = owner.em_size;
    const float x = metric.left + metric.width * .5f, y = metric.top + metric.height * .5f;
    *rectangle = {x - extent * .5f, y - extent * .5f, x + extent * .5f, y + extent * .5f};
    *clip = field->content_rectangle;
    return ui_rect_has_area(ui_rect_intersection(*rectangle, *clip));
}
namespace {

constexpr u8 kKnownOptions = text_edit_layout_wrap |
                             text_edit_layout_center_vertically |
                             text_edit_layout_scroll_to_caret;

struct NativeEditGeometry {NativeParagraphHits hits;TextMeasureResult measure;};
constexpr u32 native_storage_bytes(u32 units,u32 bytes,u32 styles=0,u32 runs=0) noexcept {
    return sizeof(NativeEditGeometry)+units*sizeof(NativeParagraphHitCluster)+(units+1)*sizeof(NativeParagraphHitLine)+(bytes*4+16)*sizeof(float)+styles*sizeof(SfntFontLineParts)+runs*sizeof(NativeParagraphHitRun);
}
// Storage is counted in 8-byte words.
constexpr u32 storage_words(u32 bytes) noexcept {return (bytes+7)/8;}
constexpr u32 edit_storage_words=storage_words(native_storage_bytes(kTextEditTextCapacity,kTextEditTextCapacity,kTextEditTextCapacity,kTextEditTextCapacity));
const NativeEditGeometry* native_geometry(const TextEditLayoutOwner& owner) noexcept {
    return owner.geometry?reinterpret_cast<const NativeEditGeometry*>(owner.storage.arena.base):nullptr;
}
bool position_metric(const TextEditLayoutOwner& owner,u32 position,float* x,float* y,TextEditMetric* metric) noexcept {
    const auto* native=native_geometry(owner);NativeParagraphHitMetric m{};
    if(!native||native_paragraph_hit_position(native->hits,position,false,&m,x)!=SfntStatus::ok)return false;
    *metric=m;*y=m.top;return true;
}
// Returns ok, output_too_small (count holds the required size) or a failure.
SfntStatus range_metrics(const TextEditLayoutOwner& owner,u32 first,u32 length,float x,float y,TextEditMetric* output,u32 capacity,u32* count) noexcept {
    const auto* native=native_geometry(owner);
    if(!native||capacity>kTextEditSelectionCapacity)return SfntStatus::invalid_input;
    return native_paragraph_hit_range(native->hits,first,length,x,y,{output,capacity},count);
}
bool initialize_storage(TextEditLayoutOwner* owner) noexcept {
    return owner->storage.arena.base||stable_storage_initialize(&owner->storage,edit_storage_words,0,8,8);
}
bool prepare_native(TextEditLayoutOwner* owner,const TextEditLayoutInput& input,const TextSystem& system) noexcept {
    if(!system.native_text||!input.text.bytes||input.text.bytes>kTextEditTextCapacity)return false;
    // The edit layout applies font styles and emoji overrides but not tracking
    // or independent-letter typography.
    auto editor_system=system;editor_system.tracking=0;editor_system.independent_letters=false;
    u32 n=input.text.bytes;const u32 advance_capacity=input.text.bytes*4+16;
    // Fields of 256+ bytes size hit clusters/lines by UTF-16 unit count; smaller
    // ones use the byte count as a cheap upper bound. Glyph advances stay byte-based.
    if(n>=256&&!utf8_utf16_position_for_byte_offset(input.text,input.text.bytes,&n))return false;
    const TextShapingView shaping(input.text);
    // Drawing's same-length substitute when the native items reject the text.
    TextNativeFallback fallback;Utf8View source=shaping.text;u32 fallback_stage=0;
    u32 style_capacity=native_text_candidate_style_capacity(shaping.text,input.styles);
    if(!initialize_storage(owner))return false;
    for(u32 attempt=0;attempt<3+TextNativeFallback::stages;++attempt){
    const u32 base_bytes=native_storage_bytes(n,input.text.bytes,style_capacity);
    if(!stable_storage_reserve(&owner->storage,storage_words(base_bytes)))return false;
    auto* geometry=reinterpret_cast<NativeEditGeometry*>(owner->storage.arena.base);
    auto* clusters=reinterpret_cast<NativeParagraphHitCluster*>(geometry+1);
    auto* lines=reinterpret_cast<NativeParagraphHitLine*>(clusters+n);
    auto* advances=reinterpret_cast<float*>(lines+n+1);
    auto* styles=reinterpret_cast<SfntFontLineParts*>(advances+advance_capacity);
    auto* runs=reinterpret_cast<NativeParagraphHitRun*>(styles+style_capacity);
    const u32 run_capacity=minimum(input.text.bytes,(owner->storage.capacity*8-base_bytes)/static_cast<u32>(sizeof(NativeParagraphHitRun)));u32 required_runs=0;
    NativeParagraphHitStorage storage{{clusters,n},{lines,n+1},{advances,advance_capacity},&geometry->hits,{styles,style_capacity},{runs,run_capacity},&required_runs};
    const auto wrapping=(input.options&text_edit_layout_wrap)?TextWrapping::wrap:TextWrapping::no_wrap;
    const auto fit=[&]() noexcept {return input.styles.count?
        native_text_candidate_styled(editor_system,source,input.styles,input.width,input.maximum_layout_height,nullptr,&geometry->measure,wrapping,TextOverflow::clip,{},&storage):
        native_text_candidate_hits(editor_system,source,input.width,input.maximum_layout_height,wrapping,storage,&geometry->measure);};
    if(!fit()){
        const u32 required=native_text_candidate_hit_style_capacity(system);
        if(required>input.text.bytes||required_runs>input.text.bytes||(required<=style_capacity&&required_runs<=run_capacity)){
            while(fallback_stage<TextNativeFallback::stages&&!fallback.build(editor_system,shaping.text,fallback_stage))++fallback_stage;
            if(fallback_stage++>=TextNativeFallback::stages)return false;source=fallback.text;continue;}
        style_capacity=maximum(style_capacity,required);
        if(!stable_storage_reserve(&owner->storage,storage_words(native_storage_bytes(n,input.text.bytes,style_capacity,required_runs))))return false;continue;
    }
    owner->geometry=true;return true;
    }return false;
}

u64 style_signature(ConstSlice<TextStyleSpan> styles) noexcept {
    return hash64_counted_bytes(kHash64Seed, styles.data, styles.count * sizeof(TextStyleSpan));
}
bool cache_matches(const TextEditLayoutOwner& owner, const TextEditLayoutInput& input, u64 format, u64 styles,
                   float em_size) noexcept {
    return owner.active && owner.format == format &&
           owner.text_signature == input.text_signature && owner.style_signature == styles && owner.text_bytes == input.text.bytes &&
           owner.anchor == input.anchor_utf16 && owner.caret_offset == input.caret_utf16 &&
           owner.viewport_left == input.origin_x && owner.viewport_top == input.viewport_top &&
           owner.width == input.width && owner.height == input.height &&
           owner.maximum_layout_height == input.maximum_layout_height &&
           owner.options == input.options && owner.em_size == em_size;
}

bool measure_large_selection(const TextEditLayoutOwner& layout, const TextEditLayoutInput& input, float origin_y,
                             u32 first, u32 last, TextEditMetric* selections, u32 capacity,
                             u32* selection_count) noexcept {
    // The public edit projection is viewport bounded. Subdivide oversized
    // ranges instead of retaining thousands of off-screen boxes.
    // At most log2(16K UTF-16 units) right halves are pending at once.
    struct Range { u32 first, last; } pending[16];
    u32 pending_count = 0;
    TextEditMetric batch[kTextEditSelectionCapacity];
    u32 count = 0;
    for (;;) {
        u32 actual = 0;
        const SfntStatus status = range_metrics(layout,first,last-first,layout.origin_x,origin_y,
            batch, kTextEditSelectionCapacity, &actual);
        if (status == SfntStatus::output_too_small && actual > kTextEditSelectionCapacity) {
            const u32 middle = first + (last - first) / 2;
            if (middle <= first || middle >= last || pending_count == 16) return false;
            pending[pending_count++] = {middle, last};
            last = middle;
            continue;
        }
        if (status != SfntStatus::ok || actual > kTextEditSelectionCapacity) return false;
        for (u32 i = 0; i < actual; ++i) {
            auto metric = batch[i];
            const float right = minimum(metric.left + metric.width, input.origin_x + input.width);
            const float bottom = minimum(metric.top + metric.height, input.viewport_top + input.height);
            metric.left = maximum(metric.left, input.origin_x);
            metric.top = maximum(metric.top, input.viewport_top);
            metric.width = right - metric.left;
            metric.height = bottom - metric.top;
            if (metric.width <= 0 || metric.height <= 0) continue;
            // Adjacent bidi runs or a split inside a shaping cluster can share
            // one visual interval. Merge only touching/overlapping rectangles
            // on the same line; gaps in a bidi selection remain unpainted.
            for (u32 j = 0; j < count;) {
                const auto prior = selections[j];
                if (metric.top == prior.top && metric.height == prior.height &&
                    metric.left <= prior.left + prior.width && prior.left <= metric.left + metric.width) {
                    const float merged_right = maximum(metric.left + metric.width, prior.left + prior.width);
                    metric.left = minimum(metric.left, prior.left);
                    metric.width = merged_right - metric.left;
                    const u32 end = maximum<u32>(metric.source_first + metric.source_count, prior.source_first + prior.source_count);
                    metric.source_first = minimum(metric.source_first, prior.source_first);
                    metric.source_count = static_cast<u16>(end - metric.source_first);
                    selections[j] = selections[--count];
                    j = 0;
                } else ++j;
            }
            if (count == capacity) return false;
            selections[count++] = metric;
        }
        if (!pending_count) {
            *selection_count = count;
            return true;
        }
        const auto range = pending[--pending_count];
        first = range.first;
        last = range.last;
    }
}

// Selection rectangles of [first, last) for a layout drawn at `origin_y`, clipped to its viewport.
bool range_geometry_at(const TextEditLayoutOwner& owner, float origin_y, u32 first, u32 last,
                       Slice<TextEditMetric> output, u32* count) noexcept {
    if (!count || !owner.active || first > last || last > owner.utf16_units ||
        !output.data || !output.count || output.count > kTextEditSelectionCapacity) return false;
    if (first == last) { *count = 0; return true; }
    if (!owner.geometry) return false;
    TextEditLayoutInput input{};
    input.origin_x = owner.viewport_left; input.viewport_top = owner.viewport_top;
    input.width = owner.width; input.height = owner.height;
    return measure_large_selection(owner, input, origin_y, first, last, output.data, output.count, count);
}

void commit_key(TextEditLayoutOwner* owner, const TextEditLayoutInput& input, u64 format, u64 styles,
                float em_size, float origin_y) noexcept {
    owner->format = format;
    owner->text_signature = input.text_signature;
    owner->style_signature = styles;
    owner->text_bytes = input.text.bytes;
    owner->anchor = input.anchor_utf16;
    owner->caret_offset = input.caret_utf16;
    owner->viewport_left = input.origin_x;
    owner->origin_y = origin_y;
    owner->viewport_top = input.viewport_top;
    owner->width = input.width;
    owner->height = input.height;
    owner->maximum_layout_height = input.maximum_layout_height;
    owner->em_size = em_size;
    owner->options = input.options;
    owner->active = true;
    saturating_increment(owner->builds);
}

} // namespace

u64 text_edit_signature(Utf8View text) noexcept {
    return hash64_counted_bytes(kHash64Seed, text.data, text.bytes);
}

bool text_edit_layout_measure(TextEditLayoutOwner* owner,
                              const TextEditLayoutInput& input,
                              const TextSystem& text_system) noexcept {
    const u64 format = native_text_format_identity(text_system);
    if (!owner || !format ||
        !owner->selections || owner->selection_capacity == 0 ||
        owner->selection_capacity > kTextEditSelectionCapacity ||
        (!input.text.data && input.text.bytes != 0) ||
        input.text.bytes > kTextEditTextCapacity || input.text_signature == 0 ||
        input.width <= 0.0f || input.height <= 0.0f ||
        input.maximum_layout_height <= 0.0f || (input.options & ~kKnownOptions) != 0 ||
        input.styles.count > kTextStyleSpanMaximum || (!input.styles.data && input.styles.count)) {
        if (owner) {
            text_edit_layout_invalidate(owner);
            saturating_increment(owner->rejected);
        }
        return false;
    }
    const u64 styles = style_signature(input.styles);
    if (cache_matches(*owner, input, format, styles, text_system.em_size)) {
        saturating_increment(owner->hits);
        return true;
    }

    const float retained_scroll = owner->scroll_y;
    const float retained_scroll_x = owner->scroll_x;
    const bool manual_scroll = owner->manual_scroll && owner->text_signature == input.text_signature &&
        owner->anchor == input.anchor_utf16 && owner->caret_offset == input.caret_utf16;
    // A caret/selection/viewport-origin change does not change text shaping.
    const bool retain_layout = owner->active && owner->geometry && owner->format == format &&
        owner->text_signature == input.text_signature && owner->style_signature == styles && owner->text_bytes == input.text.bytes &&
        owner->width == input.width && owner->maximum_layout_height == input.maximum_layout_height &&
        owner->em_size == text_system.em_size && ((owner->options ^ input.options) & text_edit_layout_wrap) == 0;
    u32 units = retain_layout ? owner->utf16_units : 0;
    text_edit_layout_invalidate(owner);
    owner->geometry = retain_layout;
    if (input.text.bytes == 0) {
        if (input.anchor_utf16 != 0 || input.caret_utf16 != 0) {
            saturating_increment(owner->rejected);
            return false;
        }
        const float caret_height = minimum(text_system.em_size * 1.25f, input.height);
        const float origin_y = (input.options & text_edit_layout_center_vertically) != 0
                                   ? input.viewport_top + (input.height - caret_height) * 0.5f
                                   : input.viewport_top;
        owner->origin_x = input.origin_x;
        owner->scroll_x = 0;
        owner->caret.left = input.origin_x;
        owner->caret.top = origin_y;
        owner->caret.width = 1.0f;
        owner->caret.height = caret_height;
        owner->layout_height = caret_height;
        commit_key(owner, input, format, styles, text_system.em_size, origin_y);
        return true;
    }

    if (!retain_layout) {
        if (!prepare_native(owner, input, text_system)) {
            saturating_increment(owner->rejected);
            return false;
        }
        units = native_geometry(*owner)->hits.source_units;
    }
    if (units == 0 || input.anchor_utf16 > units || input.caret_utf16 > units) {
        owner->geometry = false;
        saturating_increment(owner->rejected);
        return false;
    }

    float caret_x = 0.0f;
    float caret_y = 0.0f;
    TextEditMetric caret_metrics{};
    if (!position_metric(*owner, input.caret_utf16, &caret_x, &caret_y, &caret_metrics)) {
        owner->geometry = false;
        saturating_increment(owner->rejected);
        return false;
    }
    const auto& measure = native_geometry(*owner)->measure;
    const float caret_height = caret_metrics.height > 0.0f ? caret_metrics.height : 20.0f;
    const float content_height = maximum(maximum(measure.height, caret_y + caret_height), 1.0f);
    float scroll_y = 0.0f;
    if ((input.options & text_edit_layout_scroll_to_caret) != 0) {
        scroll_y = minimum(maximum(retained_scroll, 0.0f),
                           maximum(0.0f, content_height - input.height));
        if (!manual_scroll && caret_y < scroll_y) {
            scroll_y = caret_y;
        } else if (!manual_scroll && caret_y + caret_height > scroll_y + input.height) {
            scroll_y = caret_y + caret_height - input.height;
        }
        scroll_y = minimum(maximum(scroll_y, 0.0f),
                           maximum(0.0f, content_height - input.height));
    }
    const float origin_y =
        (input.options & text_edit_layout_center_vertically) != 0 &&
                content_height <= input.height
            ? input.viewport_top + (input.height - content_height) * 0.5f
            : input.viewport_top - scroll_y;
    float scroll_x = 0;
    if ((input.options & text_edit_layout_scroll_to_caret) && !(input.options & text_edit_layout_wrap)) {
        const float visible_width = maximum(1.0f, input.width - 1.0f);
        scroll_x = minimum(maximum(0.0f, retained_scroll_x), maximum(0.0f, measure.width_including_trailing_whitespace - visible_width));
        if (caret_x < scroll_x) scroll_x = caret_x;
        else if (caret_x > scroll_x + visible_width) scroll_x = caret_x - visible_width;
    }
    owner->scroll_x = scroll_x;
    owner->origin_x = input.origin_x - scroll_x;
    owner->scroll_y = scroll_y;
    owner->manual_scroll = manual_scroll;
    owner->layout_height = content_height;
    owner->caret = caret_metrics;
    owner->caret.left = owner->origin_x + caret_x;
    owner->caret.top = origin_y + caret_y;
    owner->caret.width = 1.0f;
    owner->caret.height = caret_height;

    const u32 selection_first = minimum(input.anchor_utf16, input.caret_utf16);
    const u32 selection_last = maximum(input.anchor_utf16, input.caret_utf16);
    if (selection_first != selection_last) {
        u32 actual = 0;
        const SfntStatus status = range_metrics(*owner, selection_first, selection_last - selection_first,
                                                owner->origin_x, origin_y, owner->selections,
                                                owner->selection_capacity, &actual);
        const bool oversized = status == SfntStatus::output_too_small && actual > owner->selection_capacity;
        if (!oversized || !measure_large_selection(*owner, input, origin_y, selection_first, selection_last,
                                                   owner->selections, owner->selection_capacity,
                                                   &owner->selection_count)) {
            if (status != SfntStatus::ok || actual > owner->selection_capacity) {
                owner->geometry = false;
                saturating_increment(owner->rejected);
                return false;
            }
            owner->selection_count = actual;
        }
    }
    owner->utf16_units = units;
    commit_key(owner, input, format, styles, text_system.em_size, origin_y);
    return true;
}

bool text_composer_layout_current(const TextEditLayoutOwner& owner,
                                  const MessageComposerScene& scene) noexcept {
    if (!owner.active || !scene.active || !scene.model || !scene.model->text ||
        scene.model->bytes > scene.model->capacity || scene.observed_revision != scene.model->revision ||
        scene.styles_document != scene.model->document_identity ||
        scene.styles_revision != scene.model->text_revision ||
        scene.display_bytes > kMessageComposerDisplayBytes ||
        scene.display_first > scene.display_source_last || scene.display_source_last > scene.model->bytes ||
        scene.projection_span_count > kMessageComposerProjectionSpanCapacity ||
        (scene.projection_span_count && !scene.projection_spans) ||
        scene.display_style_count > kTextStyleSpanMaximum ||
        (scene.display_style_count && !scene.display_styles))
        return false;
    const auto* edit = message_composer_scene_layout_node(scene, MessageComposerControl::edit);
    const auto source = message_composer_text(*scene.model);
    const auto display = message_composer_scene_display_text(scene);
    u32 anchor{}, caret{};
    if (!edit || !ui_rect_has_area(edit->content_rectangle) || !utf8_validate(source).valid ||
        !utf8_validate(display).valid ||
        !utf8_is_boundary(source, scene.model->anchor) ||
        !utf8_is_boundary(source, scene.model->caret))
        return false;
    const u32 display_anchor = message_composer_scene_display_offset_from_model(scene, scene.model->anchor);
    const u32 display_caret = message_composer_scene_display_offset_from_model(scene, scene.model->caret);
    if (scene.display_anchor != display_anchor || scene.display_caret != display_caret ||
        !utf8_utf16_position_for_byte_offset(display, display_anchor, &anchor) ||
        !utf8_utf16_position_for_byte_offset(display, display_caret, &caret))
        return false;
    return owner.text_bytes == display.bytes && owner.text_signature == scene.display_signature &&
           owner.style_signature == style_signature({scene.display_styles, scene.display_style_count}) &&
           owner.anchor == anchor && owner.caret_offset == caret &&
           owner.viewport_left == edit->content_rectangle.left &&
           owner.viewport_top == edit->content_rectangle.top &&
           owner.width == ui_rect_width(edit->content_rectangle) &&
           owner.height == ui_rect_height(edit->content_rectangle);
}

bool text_composer_layout_current(const TextEditLayoutOwner& owner, const MessageComposerScene& scene,
                                  const TextSystem& system) noexcept {
    return text_edit_layout_current_font(owner, system) && text_composer_layout_current(owner, scene);
}
bool text_edit_layout_current_font(const TextEditLayoutOwner& owner,
                                   const TextSystem& system) noexcept {
    const auto identity = native_text_format_identity(system);
    return owner.active && identity && owner.format == identity && owner.em_size == system.em_size;
}

bool text_composer_layout_measure(TextEditLayoutOwner* owner, const MessageComposerScene* scene,
                                  const TextSystem& text_system) noexcept {
    if (!owner) {
        return false;
    }
    if (!scene || !scene->active || !scene->model) {
        text_edit_layout_invalidate(owner);
        return true;
    }
    const UiLayoutNode* edit = message_composer_scene_layout_node(*scene, MessageComposerControl::edit);
    const Utf8View text = message_composer_scene_display_text(*scene);
    u32 anchor_utf16 = 0;
    u32 caret_utf16 = 0;
    if (!edit || scene->display_anchor > text.bytes || scene->display_caret > text.bytes ||
        !utf8_utf16_position_for_byte_offset(text, scene->display_anchor, &anchor_utf16) ||
        !utf8_utf16_position_for_byte_offset(text, scene->display_caret, &caret_utf16)) {
        text_edit_layout_invalidate(owner);
        saturating_increment(owner->rejected);
        return false;
    }
    constexpr float kComposerLayoutHeight = 262'144.0f;
    const TextEditLayoutInput input{
        text,
        scene->display_signature,
        anchor_utf16,
        caret_utf16,
        edit->content_rectangle.left,
        edit->content_rectangle.top,
        maximum(1.0f, ui_rect_width(edit->content_rectangle)),
        maximum(1.0f, ui_rect_height(edit->content_rectangle)),
        kComposerLayoutHeight,
        text_edit_layout_wrap | text_edit_layout_center_vertically |
            text_edit_layout_scroll_to_caret,
        {scene->display_styles, scene->display_style_count},
    };
    return text_edit_layout_measure(owner, input, text_system);
}

bool text_edit_layout_point_geometry(const TextEditLayoutOwner& owner, float x, float y,
    bool round_nearest, u32* position, bool* inside) noexcept {
    if (!position || !inside || !owner.active) return false;
    if (!owner.text_bytes) { *position = 0; *inside = false; return true; }
    const auto* native = native_geometry(owner);
    if (!native) return false;
    NativeParagraphHitPoint point{};
    u32 character = 0;
    if (native_paragraph_hit_point(native->hits, x-owner.origin_x, y-owner.origin_y,
            &point, &character) != SfntStatus::ok) return false;
    *inside = point.inside;
    *position = round_nearest || !point.inside ? point.source_position : character;
    return true;
}

bool text_edit_layout_hit_test(const TextEditLayoutOwner& owner, float x, float y,
    u32* position) noexcept {
    bool inside = false;
    return text_edit_layout_point_geometry(owner, x, y, true, position, &inside);
}

bool text_edit_layout_move_vertical(const TextEditLayoutOwner& owner, bool down,
                                    u32* utf16_position) noexcept {
    if (!utf16_position || !owner.active) {
        return false;
    }
    if (owner.text_bytes == 0) {
        *utf16_position = 0;
        return true;
    }
    const float line_height = maximum(1.0f, owner.caret.height);
    const float x = owner.caret.left - owner.origin_x;
    const float y = owner.caret.top - owner.origin_y +
                    (down ? line_height * 1.5f : -line_height * 0.5f);
    const auto* native = native_geometry(owner);
    NativeParagraphHitPoint point{};
    if (!native || native_paragraph_hit_point(native->hits, x, y, &point) != SfntStatus::ok) return false;
    *utf16_position = point.source_position;
    return true;
}

bool text_edit_layout_range_geometry(const TextEditLayoutOwner& owner, u32 first, u32 last,
    Slice<TextEditMetric> output, u32* count) noexcept {
    return range_geometry_at(owner, owner.origin_y, first, last, output, count);
}

bool text_edit_layout_position_geometry(const TextEditLayoutOwner& owner,u32 position,TextEditMetric* metric) noexcept {
    if(!metric||!owner.active||position>owner.utf16_units)return false;
    if(!owner.text_bytes){*metric=owner.caret;return position==0;}
    float x=0,y=0;if(!position_metric(owner,position,&x,&y,metric))return false;
    metric->left=owner.origin_x+x;metric->top=owner.origin_y+y;metric->width=1;return true;
}
bool text_edit_layout_raw_range_geometry(const TextEditLayoutOwner& owner,u32 first,u32 last,Slice<TextEditMetric> output,u32* count) noexcept {
    if(!count||!owner.active||first>last||last>owner.utf16_units||!output.data||!output.count||output.count>kTextEditSelectionCapacity)return false;
    return range_metrics(owner,first,last-first,owner.origin_x,owner.origin_y,output.data,output.count,count)==SfntStatus::ok;
}
u32 text_edit_layout_line_count(const TextEditLayoutOwner& owner) noexcept {
    if(!owner.active)return 0;if(!owner.text_bytes)return 1;
    const auto* native=native_geometry(owner);return native?native->measure.line_count:0;
}

bool text_edit_layout_scroll(TextEditLayoutOwner* owner, float delta) noexcept {
    if (!owner || !owner->active || !ui_coordinate_valid(delta)) return false;
    const float next = minimum(maximum(0.0f, owner->scroll_y + delta), maximum(0.0f, owner->layout_height - owner->height));
    if (next == owner->scroll_y) return true;
    const float offset = owner->scroll_y - next;
    TextEditMetric metrics[kTextEditSelectionCapacity];
    u32 count = 0;
    if (!range_geometry_at(*owner, owner->origin_y + offset, minimum(owner->anchor, owner->caret_offset),
        maximum(owner->anchor, owner->caret_offset), {metrics, owner->selection_capacity}, &count)) return false;
    owner->scroll_y = next; owner->origin_y += offset; owner->caret.top += offset;
    owner->manual_scroll = true;
    for (u32 i = 0; i < count; ++i) owner->selections[i] = metrics[i];
    owner->selection_count = count;
    return true;
}

void text_edit_layout_invalidate(TextEditLayoutOwner* owner) noexcept {
    if (!owner) {
        return;
    }
    owner->format = 0;
    owner->text_signature = 0;
    owner->style_signature = 0;
    owner->manual_scroll = false;
    owner->text_bytes = 0;
    owner->anchor = 0;
    owner->caret_offset = 0;
    owner->utf16_units = 0;
    owner->selection_count = 0;
    reset_value<TextEditMetric>(&owner->caret);
    owner->origin_x = 0.0f;
    owner->origin_y = 0.0f;
    owner->width = 0.0f;
    owner->height = 0.0f;
    owner->viewport_top = 0.0f;
    owner->layout_height = 0.0f;
    owner->scroll_y = 0.0f;
    owner->scroll_x = owner->viewport_left = 0.0f;
    owner->em_size = 0.0f;
    owner->maximum_layout_height = 0.0f;
    owner->active = false;
    owner->options = text_edit_layout_none;
    owner->geometry = false;
}

void text_edit_layout_release(TextEditLayoutOwner* owner) noexcept {
    if (!owner) {
        return;
    }
    text_edit_layout_invalidate(owner);
    stable_storage_release(&owner->storage);
    owner->selections = nullptr;
    owner->selection_capacity = 0;
    owner->builds = 0;
    owner->hits = 0;
    owner->rejected = 0;
}

} // namespace maicord
