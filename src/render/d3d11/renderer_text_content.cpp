#include "renderer_text_internal.hpp"
#include "maicord/ui/text/ansi_text.hpp"
#include "maicord/ui/chat/messages/author.hpp"
#include "../text/picker_glyph_cache.hpp"
#include "../text/shaped_text_cache.hpp"
#include "maicord/core/log.hpp"
#include "maicord/render/text/glyph_quad_bounds.hpp"
#include "maicord/ui/shell/text_presentation.hpp"
#include "maicord/ui/text/inline_flag_projection.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord {
namespace renderer_text_detail {

bool append_content_text_with(D3D11Renderer* renderer, const ContentTextRequest& request, float* shaped_height,
                              bool* atlas_invalidated, UiInlineFlagProjection& flag_projection) noexcept {
    const Utf8View& input = request.input;
    const float maximum_width = request.maximum_width, maximum_height = request.maximum_height;
    float origin_x = request.origin_x, origin_y = request.origin_y;
    const UiRect& clip = *request.clip;
    const UiRect* batch_clip = request.batch_clip;
    const Color& color = *request.color;
    ConstSlice<TextStyleSpan> styles = request.styles;
    const Theme* style_theme = request.style_theme;
    const Color* inline_role_color = request.inline_role_color;
    const ConstSlice<MessageMarkdownSemantic>& semantics = request.semantics;
    const UiWidgetReference& layout_owner = request.layout_owner;
    const u64 message_id = request.message_id;
    const u32 message_revision = request.message_revision;
    const UiTextAlignment horizontal_alignment = request.horizontal_alignment,
                          vertical_alignment = request.vertical_alignment;
    const TextWrapping wrapping = request.wrapping;
    const TextOverflow overflow = request.overflow;
    const bool force_new_batch = request.force_new_batch;
    bool selection_active = request.selection_active;
    const u32 selection_anchor = request.selection_anchor, selection_caret = request.selection_caret;
    const UiTextFace face = request.face;
    const bool cluster_effect = request.cluster_effect, publish_semantics = request.publish_semantics;
    auto text_system = command_text_system(renderer, face);
    text_system.independent_letters = cluster_effect;
    RendererTextStorage* storage = renderer->content_text;
    if (storage->quad_count > storage->quad_capacity || (!input.data && input.bytes != 0) ||
        maximum_width <= 0.0f || maximum_height <= 0.0f) {
        return false;
    }
    *shaped_height = 0.0f;
    if (input.bytes == 0) {
        return true;
    }
    const auto original_styles = styles;
    RendererInlineMediaLayer flag_layer{};
    if (flag_projection_wanted(*renderer, request, &flag_layer)) {
        const bool projected =
            ui_inline_flag_project(input, styles.count ? styles[0].flags : 0, &flag_projection);
        if (!projected || flag_projection.count > storage->hit_span_capacity ||
            flag_projection.count > kRendererInlineFlagCapacity - renderer->inline_media.count) {
            flag_projection.count = 0;
        }
        if (flag_projection.count && !renderer->inline_media.flags) {
            renderer->inline_media.flags = static_cast<RendererInlineFlag*>(
                VirtualAlloc(nullptr, sizeof(RendererInlineFlag) * kRendererInlineFlagCapacity,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!renderer->inline_media.flags) {
                flag_projection.count = 0;
            }
        }
        if (flag_projection.count)
            styles = {flag_projection.styles, flag_projection.style_count};
    }
    const UiRect command_rectangle{origin_x, origin_y, origin_x + maximum_width, origin_y + maximum_height};
    bool picker_emoji = false;
    for (u32 index = 0; index < styles.count; ++index) {
        picker_emoji = picker_emoji || (styles[index].flags & text_style_picker_emoji) != 0;
    }
    // Standalone emoji previews behave like images: center all native color
    // layers together by their ink. Inline and wrapped jumbo message text
    // retain their normal baseline and line-box alignment.
    const bool centered_jumbo_emoji =
        styles.count == 1 && !styles[0].byte_offset && styles[0].byte_count == input.bytes &&
        styles[0].flags == text_style_jumbo_emoji && !flag_projection.count && !semantics.count &&
        !selection_active && !cluster_effect && face == UiTextFace::normal &&
        horizontal_alignment == UiTextAlignment::center && vertical_alignment == UiTextAlignment::center &&
        wrapping == TextWrapping::no_wrap;
    const bool center_emoji_ink = picker_emoji || centered_jumbo_emoji;
    const bool cache_picker =
        picker_emoji && input.bytes <= 64 && styles.count == 1 && !styles[0].byte_offset &&
        styles[0].byte_count == input.bytes &&
        (styles[0].flags == text_style_picker_emoji || styles[0].flags == text_style_picker_grid_emoji ||
         styles[0].flags == text_style_picker_large_emoji) &&
        !flag_projection.count && !semantics.count && !selection_active && !cluster_effect &&
        face == UiTextFace::normal && horizontal_alignment == UiTextAlignment::center &&
        vertical_alignment == UiTextAlignment::center && wrapping == TextWrapping::no_wrap;
    const u32 picker_cache_flags = cache_picker ? styles[0].flags | (static_cast<u32>(overflow) << 24) : 0;
    auto& glyph_owner = renderer->glyph_text;
    if (cache_picker && !renderer->picker_glyph_cache)
        renderer->picker_glyph_cache = static_cast<PickerGlyphCache*>(
            VirtualAlloc(nullptr, sizeof(PickerGlyphCache), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* picker_cache = cache_picker ? renderer->picker_glyph_cache : nullptr;
    if (picker_cache) {
        if (const auto* cached = picker_glyph_cache_find(
                *picker_cache, glyph_owner, text_system, input, maximum_width, maximum_height, color,
                renderer->dpi, picker_cache_flags, storage->quad_capacity - storage->quad_count)) {
            const auto& e = *cached;
            const u32 first = storage->quad_count;
            for (u32 i = 0; i < e.count; ++i) {
                storage->quads[storage->quad_count++] =
                    e.quads[i].placed(command_rectangle.left, command_rectangle.top, e.color);
            }
            if (!glyph_quad_clip_range(storage->quads, first, &storage->quad_count,
                                       ui_rect_intersection(command_rectangle, clip)) ||
                !append_text_batch(storage, &clip, first, force_new_batch, batch_clip)) {
                storage->quad_count = first;
                storage->last_error = RendererTextBuildError::capacity;
                return false;
            }
            *shaped_height = e.shaped_height;
            ++picker_cache->hits;
            return true;
        }
    }
    shaped_text_reset(&storage->scratch);
    constexpr Utf8View overflow_text = utf8_literal("…");
    bool substituted = false;
    constexpr u16 selection_semantic_index = 0xFFFFu;
    const u32 selection_first = minimum(selection_anchor, selection_caret);
    const u32 selection_last = maximum(selection_anchor, selection_caret);
    selection_active =
        selection_active && selection_first < selection_last && selection_last <= input.bytes;
    u32 semantic_count = semantics.count;
    const u32 semantic_remaining = storage->content_semantic_capacity - storage->content_semantic_count;
    semantic_count = minimum(semantic_count, semantic_remaining);
    const u32 semantic_span_capacity =
        storage->hit_span_capacity - (selection_active && storage->hit_span_capacity != 0 ? 1u : 0u);
    semantic_count = minimum(semantic_count, semantic_span_capacity);
    const u32 semantic_base = storage->content_semantic_count;
    // Hit spans carry command-local semantic indices so a shaped result does
    // not depend on where it lands in the frame; publication adds semantic_base.
    for (u32 index = 0; index < semantic_count; ++index) {
        storage->hit_spans[index] = TextHitSpan{semantics[index].byte_offset, semantics[index].byte_count,
                                                static_cast<u16>(index), 0};
    }
    u32 interactive_count = semantic_count;
    if (flag_projection.count) {
        for (u32 i = 0; i < flag_projection.count; ++i) {
            const auto& flag = flag_projection.flags[i];
            storage->hit_spans[i] = {flag.first_byte, flag.end_byte - flag.first_byte, static_cast<u16>(i),
                                     0};
        }
        interactive_count = flag_projection.count;
    }
    if (selection_active && interactive_count < storage->hit_span_capacity) {
        storage->hit_spans[interactive_count++] =
            TextHitSpan{selection_first, selection_last - selection_first, selection_semantic_index, 0};
    } else {
        selection_active = false;
    }
    // Effects rewrite glyph data; flags and truncated semantics are rare. They shape directly.
    ShapedTextCache* shape_cache = nullptr;
    // ANSI foregrounds are applied per glyph from the cluster map, which
    // cached shapes do not keep.
    bool ansi_colors = false;
    for (u32 index = 0; index < styles.count && !ansi_colors; ++index)
        ansi_colors = ansi_span_foreground(styles[index]) != kAnsiColorNone;
    if (!renderer->shaped_text_cache_disabled && !flag_projection.count && !cluster_effect && !picker_emoji &&
        !ansi_colors &&
        semantic_count == semantics.count) {
        if (!renderer->shaped_text_cache)
            renderer->shaped_text_cache = shaped_text_cache_create();
        shape_cache = renderer->shaped_text_cache;
    }
    const ShapedTextCacheRequest cache_request{&text_system,
                                               input,
                                               styles,
                                               {storage->hit_spans, interactive_count},
                                               maximum_width,
                                               maximum_height,
                                               renderer->dpi,
                                               renderer->typography_revision,
                                               static_cast<u8>(wrapping),
                                               static_cast<u8>(overflow)};
    u64 cache_hash = 0;
    ShapedText cached{};
    bool from_cache = false;
    if (shape_cache && shaped_text_cache_find(shape_cache, cache_request, &cache_hash, &cached)) {
        const u32 room = storage->quad_capacity - storage->quad_count;
        // A result that no longer fits takes the substitution path below.
        from_cache = cached.glyph_count <= room && cached.decoration_count <= room - cached.glyph_count;
    }
    const auto shape = [&](Utf8View text, ConstSlice<TextStyleSpan> spans, u32 hits) noexcept {
        return text_shape_utf8(&text_system, text, spans, maximum_width, maximum_height,
                               Slice<wchar_t>{storage->utf16, storage->utf16_capacity}, &storage->scratch,
                               wrapping, overflow, {storage->hit_spans, hits});
    };
    bool primary_shape = true;
    TextShapeResult shaped = from_cache ? TextShapeResult{} : shape(input, styles, interactive_count);
    if (interactive_count != 0 && shaped.error == TextShapeError::output_too_small) {
        primary_shape = false;
        shaped_text_reset(&storage->scratch);
        if (flag_projection.count) {
            styles = original_styles;
            flag_projection.count = 0;
        }
        interactive_count = 0;
        semantic_count = 0;
        selection_active = false;
        shaped = shape(input, styles, 0);
    }
    if (shaped.error == TextShapeError::output_too_small) {
        primary_shape = false;
        flag_projection.count = 0;
        substituted = true;
        if (storage->substitutions != 0xFFFF) {
            ++storage->substitutions;
        }
        shaped = shape(overflow_text, {}, 0);
        interactive_count = 0;
        semantic_count = 0;
        selection_active = false;
    }
    if (!shaped.ok()) {
        storage->last_error = RendererTextBuildError::shape;
        storage->last_shape_error = shaped.error;
        storage->native_error = shaped.native_error;
        shaped_text_reset(&storage->scratch);
        return false;
    }
    const u32 remaining_quads = storage->quad_capacity - storage->quad_count;
    if (!from_cache && (storage->scratch.glyph_count > remaining_quads ||
                        storage->scratch.decoration_count >
                            remaining_quads - minimum(storage->scratch.glyph_count, remaining_quads))) {
        primary_shape = false;
        flag_projection.count = 0;
        if (!substituted && storage->substitutions != 0xFFFF) {
            ++storage->substitutions;
        }
        shaped = shape(overflow_text, {}, 0);
        interactive_count = 0;
        semantic_count = 0;
        selection_active = false;
        if (!shaped.ok()) {
            storage->last_error = RendererTextBuildError::shape;
            storage->last_shape_error = shaped.error;
            storage->native_error = shaped.native_error;
            shaped_text_reset(&storage->scratch);
            return false;
        }
        if (storage->scratch.glyph_count > remaining_quads) {
            shaped_text_reset(&storage->scratch);
            return true;
        }
    }
    const ShapedText& layout = from_cache ? cached : storage->scratch;
    *shaped_height = layout.height;
    origin_x = aligned_text_origin(horizontal_alignment, origin_x, maximum_width, layout.width);
    origin_y = aligned_text_origin(vertical_alignment, origin_y, maximum_height, layout.height);
    const u32 first_quad = storage->quad_count;
    const D3D11GlyphTextAppendRequest glyph_request{
        &layout,
        storage->quads,
        &storage->quad_count,
        atlas_invalidated,
        input,
        semantics,
        style_theme,
        inline_role_color,
        renderer,
        resolve_mention_color_callback,
        color,
        origin_x,
        origin_y,
        message_id,
        0,
        storage->quad_capacity,
        renderer->dpi,
        selection_semantic_index,
        selection_active,
        cluster_effect,
        styles,
    };
    const bool appended =
        d3d11_glyph_text_append(&renderer->glyph_text, renderer->graphics.context, glyph_request);
    if (!appended) {
        shaped_text_reset(&storage->scratch);
        storage->last_error =
            *atlas_invalidated ? RendererTextBuildError::atlas : RendererTextBuildError::capacity;
        return false;
    }
    if (center_emoji_ink) {
        if (!glyph_quad_center_range(storage->quads, first_quad, storage->quad_count, command_rectangle)) {
            storage->quad_count = first_quad;
            storage->last_error = RendererTextBuildError::layout;
            shaped_text_reset(&storage->scratch);
            return false;
        }
        if (picker_cache && !substituted && storage->quad_count - first_quad <= 4) {
            auto& e = picker_cache->entries[picker_cache->cursor++ % 256];
            e.native_text = text_system.native_text;
            e.native_format = text_system.native_format;
            e.em_size = text_system.em_size;
            e.tracking = text_system.tracking;
            e.fixed_weight = text_system.fixed_weight;
            e.color = color;
            e.bytes = static_cast<u8>(input.bytes);
            copy_bytes(e.text, input.data, input.bytes);
            e.width = maximum_width;
            e.height = maximum_height;
            e.shaped_height = *shaped_height;
            e.dpi = renderer->dpi;
            e.flags = picker_cache_flags;
            e.evictions = glyph_owner.allocator.evicted_entries;
            e.resets = glyph_owner.allocator.full_resets;
            e.count = static_cast<u8>(storage->quad_count - first_quad);
            for (u32 i = 0; i < e.count; ++i) {
                const auto& q = storage->quads[first_quad + i];
                e.quads[i] = {q.left - command_rectangle.left, q.top - command_rectangle.top,
                              q.right - command_rectangle.left, q.bottom - command_rectangle.top,
                              q.u0, q.v0, q.u1, q.v1};
            }
        }
        const UiRect bounds = ui_rect_intersection(command_rectangle, clip);
        if (!glyph_quad_clip_range(storage->quads, first_quad, &storage->quad_count, bounds)) {
            storage->quad_count = first_quad;
            storage->last_error = RendererTextBuildError::layout;
            log_write(LogLevel::error, "render", "emoji_quad_bounds_error");
            shaped_text_reset(&storage->scratch);
            return false;
        }
    }
    if (!append_text_batch(storage, &clip, first_quad, force_new_batch, batch_clip)) {
        shaped_text_reset(&storage->scratch);
        storage->last_error = RendererTextBuildError::capacity;
        return false;
    }
    if (flag_projection.count) {
        for (u32 i = 0; i < layout.hit_region_count; ++i) {
            const auto& hit = layout.hit_regions[i];
            if (hit.semantic_index >= flag_projection.count)
                continue;
            const float extent = minimum(hit.right - hit.left, hit.bottom - hit.top);
            const float x = origin_x + (hit.left + hit.right) * .5f;
            const float y = origin_y + (hit.top + hit.bottom) * .5f;
            const UiRect rectangle{x - extent * .5f, y - extent * .5f, x + extent * .5f, y + extent * .5f};
            if (!ui_rect_intersects(rectangle, clip))
                continue;
            if (renderer->inline_media.count == kRendererInlineFlagCapacity)
                break;
            renderer->inline_media.flags[renderer->inline_media.count++] = {
                layout_owner, rectangle, clip, flag_projection.flags[hit.semantic_index].sequence,
                flag_layer};
        }
        shaped_text_reset(&storage->scratch);
        return true;
    }
    // Paint-only semantics (composer role colours) never become hit targets.
    if (!publish_semantics) {
        if (shape_cache && !from_cache && primary_shape)
            shaped_text_cache_insert(shape_cache, cache_request, cache_hash, &storage->scratch);
        shaped_text_reset(&storage->scratch);
        return true;
    }
    u32 semantic_hit_region_count = 0;
    for (u32 index = 0; index < layout.hit_region_count; ++index) {
        if (layout.hit_regions[index].semantic_index != selection_semantic_index) {
            ++semantic_hit_region_count;
        }
    }
    // Message id zero denotes semantics owned by a retained overlay widget.
    if (semantic_hit_region_count != 0 && message_id == 0 && !layout_owner.manager) {
        shaped_text_reset(&storage->scratch);
        storage->last_error = RendererTextBuildError::invalid_state;
        return false;
    }
    if (semantic_hit_region_count >
        storage->content_hit_region_capacity - storage->content_hit_region_count) {
        shaped_text_reset(&storage->scratch);
        storage->last_error = RendererTextBuildError::capacity;
        return false;
    }
    for (u32 index = 0; index < semantic_count; ++index) {
        const MessageMarkdownSemantic& semantic = semantics[index];
        storage->content_semantics[storage->content_semantic_count++] =
            MessageTextSemantic{message_id,
                                semantic.target,
                                message_revision,
                                semantic.source_offset,
                                semantic.source_bytes,
                                semantic.kind,
                                semantic.reserved};
    }
    for (u32 index = 0; index < layout.hit_region_count; ++index) {
        const TextHitRegion& local = layout.hit_regions[index];
        if (local.semantic_index == selection_semantic_index) {
            continue;
        }
        const u16 semantic_index = static_cast<u16>(semantic_base + local.semantic_index);
        const float left = maximum(clip.left, origin_x + local.left);
        const float top = maximum(clip.top, origin_y + local.top);
        const float right = minimum(clip.right, origin_x + local.right);
        const float bottom = minimum(clip.bottom, origin_y + local.bottom);
        if (right > left && bottom > top) {
            const MessageMarkdownSemanticKind kind = semantic_index < storage->content_semantic_count &&
                                                             storage->content_semantics[semantic_index].message_id != 0
                                                         ? storage->content_semantics[semantic_index].kind
                                                         : MessageMarkdownSemanticKind::none;
            const bool flag_cell = kind == MessageMarkdownSemanticKind::regional_flag ||
                                   kind == MessageMarkdownSemanticKind::chip_icon ||
                                   kind == MessageMarkdownSemanticKind::guild_icon;
            // Flag and chip icon ranges keep the full cell so the viewport
            // scissor clips the artwork without shrinking or recentering it;
            // other hit rectangles are clipped here.
            storage->content_hit_regions[storage->content_hit_region_count++] =
                flag_cell
                    ? TextHitRegion{origin_x + local.left,   origin_y + local.top, origin_x + local.right,
                                    origin_y + local.bottom, semantic_index, 0}
                    : TextHitRegion{left, top, right, bottom, semantic_index, 0};
        }
    }
    if (shape_cache && !from_cache && primary_shape)
        shaped_text_cache_insert(shape_cache, cache_request, cache_hash, &storage->scratch);
    shaped_text_reset(&storage->scratch);
    return true;
}

__declspec(noinline) bool append_flag_text(D3D11Renderer* renderer, const ContentTextRequest& request,
                                           float* shaped_height, bool* atlas_invalidated) noexcept {
    // About 6 KiB, read only within this call: statics, not a guard-page walk.
    static UiInlineFlagProjection projection;
    projection.count = projection.style_count = 0;
    return append_content_text_with(renderer, request, shaped_height, atlas_invalidated, projection);
}

bool append_content_text(D3D11Renderer* renderer, const ContentTextRequest& request, float* shaped_height,
                         bool* atlas_invalidated) noexcept {
    RendererInlineMediaLayer layer{};
    if (flag_projection_wanted(*renderer, request, &layer))
        return append_flag_text(renderer, request, shaped_height, atlas_invalidated);
    // Without flags the projection is only ever read as empty and reset to empty.
    static UiInlineFlagProjection none;
    return append_content_text_with(renderer, request, shaped_height, atlas_invalidated, none);
}

Color profile_tone(const Color& c, float light_scale, float floor = 0, float saturation_scale = 1,
                   float light_add = 0, float ceiling = 1) noexcept {
    const float hi = maximum(c.red, maximum(c.green, c.blue)),
                lo = minimum(c.red, minimum(c.green, c.blue));
    const float old_l = (hi + lo) * .5f, old_c = hi - lo;
    const float denominator = 1 - (old_l > .5f ? 2 * old_l - 1 : 1 - 2 * old_l);
    const float saturation = denominator > 0 ? minimum(1.f, old_c / denominator * saturation_scale) : 0;
    const float l = minimum(ceiling, maximum(floor, old_l * light_scale + light_add));
    const float chroma = (1 - (l > .5f ? 2 * l - 1 : 1 - 2 * l)) * saturation;
    const float scale = old_c > 0 ? chroma / old_c : 0, minimum_channel = l - chroma * .5f;
    return {(c.red - lo) * scale + minimum_channel, (c.green - lo) * scale + minimum_channel,
            (c.blue - lo) * scale + minimum_channel, 1};
}

bool append_content_text_command(D3D11Renderer* renderer, const UiTextCommand& command, const Theme& theme,
                                 bool* atlas_invalidated, bool force_new_batch,
                                 const UiRect* batch_clip) noexcept {
    // A partially visible virtual row can have a label wholly outside the
    // scroll clip; skip it rather than shape it or pass an empty intersection
    // to glyph clipping.
    if (!ui_rect_intersects(command.rectangle, command.clip))
        return true;
    const TextStyleSpan span{0, command.text.bytes, command.style_flags};
    const auto* composer = renderer->message_composer_scene;
    const bool composer_edit = composer && composer->model && composer->model->bytes &&
                               command.owner.manager == &composer->widgets &&
                               command.owner.handle.slot == composer->edit.slot;
    const ConstSlice<TextStyleSpan> styles =
        composer_edit ? ConstSlice<TextStyleSpan>{composer->display_styles, composer->display_style_count}
        : command.styles.count                   ? command.styles
        : command.style_flags == text_style_none ? ConstSlice<TextStyleSpan>{}
                                                 : ConstSlice<TextStyleSpan>{&span, 1};
    const auto* profile = renderer->profile_scene;
    const bool bio = profile && profile->active && command.owner.manager == &profile->widgets &&
                     command.owner.handle.slot == profile->bio_widget.slot;
    auto bio_semantics =
        bio ? ConstSlice<MessageMarkdownSemantic>{profile->bio_semantics, profile->bio_semantic_count}
            : ConstSlice<MessageMarkdownSemantic>{};
    // Coloured role pills in the draft paint like message role mentions.
    MessageMarkdownSemantic composer_roles[kMessageComposerProjectionSpanCapacity];
    u32 composer_role_count = 0;
    for (u32 index = 0; composer_edit && composer->projection_spans && index < composer->projection_span_count &&
                        composer_role_count < kMessageComposerProjectionSpanCapacity;
         ++index) {
        const auto& projection = composer->projection_spans[index];
        if (projection.kind != MessageComposerProjectionKind::role || !projection.color ||
            projection.display_last <= projection.display_first || projection.display_last > command.text.bytes)
            continue;
        composer_roles[composer_role_count++] = MessageMarkdownSemantic{
            projection.target, projection.display_first, projection.display_last - projection.display_first, 0, 0,
            MessageMarkdownSemanticKind::role_mention, 0};
    }
    if (composer_role_count)
        bio_semantics = {composer_roles, composer_role_count};
    float shaped_height = 0.0f;
    const u32 first_quad = renderer->content_text->quad_count;
    // UI labels whose ink fits their own clip can share the window scissor.
    const UiRect window_clip{0, 0, renderer->ui_scene ? renderer->ui_scene->width : 0,
                             renderer->ui_scene ? renderer->ui_scene->height : 0};
    if (!batch_clip && renderer->ui_scene)
        batch_clip = &window_clip;
    // Name effects can expand or animate their quads after batching.
    const ContentTextRequest request{
        command.text, ui_rect_width(command.rectangle), ui_rect_height(command.rectangle),
        command.rectangle.left, command.rectangle.top, &command.clip,
        command.effect.kind ? nullptr : batch_clip, &command.color, styles,
        composer_edit || bio || command.effect.themed_styles ? &theme : nullptr, nullptr, bio_semantics, command.owner, 0, 0,
        composer_edit ? composer->display_anchor : 0, composer_edit ? composer->display_caret : 0,
        command.horizontal_alignment, command.vertical_alignment, command.wrapping, command.overflow,
        command.face, force_new_batch,
        composer_edit && composer->model->focused && composer->display_anchor != composer->display_caret,
        command.effect.kind == 8,
        !composer_edit && !bio &&
            !(renderer->text_form && renderer->text_form->scene &&
              command.owner.manager == &renderer->text_form->scene->widgets) &&
            !(renderer->forum_form && renderer->forum_form->scene &&
              command.owner.manager == &renderer->forum_form->scene->widgets) &&
            !(renderer->search_scene && command.owner.manager == &renderer->search_scene->widgets &&
              command.owner.handle.slot == renderer->search_scene->edit.slot) &&
            !(renderer->media_picker_scene &&
              command.owner.manager == &renderer->media_picker_scene->widgets &&
              command.owner.handle.slot == renderer->media_picker_scene->edit.slot),
        !composer_edit};
    const bool appended = append_content_text(renderer, request, &shaped_height, atlas_invalidated);
    if (appended && profile && profile->active && command.owner.manager == &profile->widgets) {
        UiRect ink{};
        bool have_ink = false;
        // Capture native glyph geometry before name effects add shadow copies.
        // Solid decoration quads (including hover underline) never enlarge the hit.
        for (u32 i = first_quad; i < renderer->content_text->quad_count; ++i) {
            const auto& glyph = renderer->content_text->quads[i];
            if (glyph.u0 < 0 || !glyph.color.alpha)
                continue;
            const UiRect bounds{glyph.left, glyph.top, glyph.right, glyph.bottom};
            if (!ui_rect_has_area(bounds))
                continue;
            if (!have_ink) {
                ink = bounds;
                have_ink = true;
            } else {
                ink.left = minimum(ink.left, bounds.left);
                ink.top = minimum(ink.top, bounds.top);
                ink.right = maximum(ink.right, bounds.right);
                ink.bottom = maximum(ink.bottom, bounds.bottom);
            }
        }
        profile_scene_publish_text_hit(*profile, command.owner, command.rectangle, ink, command.clip);
    }
    if (appended && command.effect.kind && command.effect.count &&
        renderer->text_effect_count < kTextEffectCapacity) {
        const u32 slot = renderer->text_effect_count++;
        auto& material = renderer->text_effects.materials[slot];
        renderer->text_effect_uploaded_count = 0;
        reset_value(&material);
        UiRect effect_ink = command.rectangle;
        for (u32 i = first_quad; i < renderer->content_text->quad_count; ++i) {
            const auto& quad = renderer->content_text->quads[i];
            effect_ink = i == first_quad ? UiRect{quad.left, quad.top, quad.right, quad.bottom}
                                         : UiRect{minimum(effect_ink.left, quad.left),
                                                  minimum(effect_ink.top, quad.top),
                                                  maximum(effect_ink.right, quad.right),
                                                  maximum(effect_ink.bottom, quad.bottom)};
        }
        material.left = effect_ink.left;
        material.width = maximum(1.f, ui_rect_width(effect_ink));
        material.kind = command.effect.kind;
        material.count = minimum<u8>(command.effect.count, 8);
        // Static effects keep the zero initialized above; only motion needs
        // the live Windows animation preference.
        if (!renderer->reduced_motion && command.effect.animate && command.effect.kind != 1 &&
            command.effect.kind != 2 && command.effect.kind != 6) {
            BOOL animation = TRUE;
            SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0);
            material.animate = animation ? 1.f : 0.f;
        }
        material.reserved[0] = static_cast<float>(command.effect.started_ms % 60000) / 1000.f;
        material.reserved[1] = text_system_font_size(renderer->glyph_text.system, command.style_flags);
        material.reserved[2] = command.rectangle.top;
        renderer->text_effect_animated |= material.animate != 0 && command.effect.kind != 1;
        // Covers the neon halo (4 + .12 em), pop offset (.13 em), gummy scale
        // and the later outline/shadow passes (< 1 + .02 em).
        const float reach = 4.f + .5f * material.reserved[1];
        renderer->text_effect_bounds[slot] = {effect_ink.left - reach, effect_ink.top - reach,
                                              effect_ink.right + reach, effect_ink.bottom + reach};
        for (u32 i = 0; i < static_cast<u32>(material.count); ++i) {
            material.colors[i] = color_from_rgb24(command.effect.colors[i]);
        }
        if (command.effect.kind == 3 || command.effect.kind == 4 || command.effect.kind == 5) {
            const auto base = material.colors[0];
            material.colors[1] = profile_tone(base, 1.2f);
            material.colors[2] = profile_tone(base, 1.6f);
            material.colors[3] = profile_tone(base, .6f);
            material.colors[4] = profile_tone(base, .2f);
            material.colors[5] = profile_tone(base, .4f, .12f);
            material.colors[6] = profile_tone(base, 1, 0, 1.2f, .1f, .6f);
        }
        if (command.effect.kind == 7) {
            auto system = command_text_system(renderer, command.face);
            wchar_t scratch[4]{};
            const TextStyleSpan zero_style{0, 1, command.style_flags};
            const auto zero = text_measure_utf8(&system, utf8_literal("0"), {&zero_style, 1}, 256,
                                                64, {scratch, 4}, TextWrapping::no_wrap);
            u32 units = 0;
            utf8_utf16_position_for_byte_offset(command.text, command.text.bytes, &units);
            material.colors[7].alpha =
                maximum(38.f, units * (zero.ok() ? zero.width : material.reserved[1] * .5f));
        }
        if (command.effect.kind == 8) {
            // Combining glyphs share their shaped cluster's transform origin.
            auto* quads = renderer->content_text->quads;
            for (u32 i = first_quad; i < renderer->content_text->quad_count; ++i) {
                if (quads[i].u0 >= 2.f)
                    continue;
                float left = quads[i].left, right = quads[i].right;
                for (u32 j = first_quad; j < renderer->content_text->quad_count; ++j)
                    if (quads[j].color.green == quads[i].color.green && quads[j].u0 >= 0) {
                        left = minimum(left, quads[j].left);
                        right = maximum(right, quads[j].right);
                    }
                quads[i].color.blue = (left + right) * .5f;
            }
        }
        for (u32 i = first_quad; i < renderer->content_text->quad_count; ++i) {
            if (renderer->content_text->quads[i].u0 >= 2.f)
                continue;
            renderer->content_text->quads[i].color.red = -1.f - static_cast<float>(slot);
            if (command.effect.kind != 8)
                renderer->content_text->quads[i].color.green = static_cast<float>(i - first_quad);
        }
    }
    if (appended && first_quad < renderer->content_text->quad_count &&
        renderer->content_text->quads[first_quad].color.red < 0 &&
        (command.effect.kind == 4 || command.effect.kind == 5)) {
        auto* storage = renderer->content_text;
        const u32 count = storage->quad_count - first_quad, passes = command.effect.kind == 5 ? 5 : 4,
                  extra = count * passes;
        if (count && extra <= storage->quad_capacity - storage->quad_count) {
            for (u32 i = count; i > 0; --i)
                storage->quads[first_quad + extra + i - 1] = storage->quads[first_quad + i - 1];
            const float dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
            const float em = text_system_font_size(renderer->glyph_text.system, command.style_flags);
            const float outline_offset = (command.effect.kind == 5 ? .6f : .8f) + .02f * em;
            for (u32 pass = 0; pass < passes; ++pass) {
                const bool shadow = command.effect.kind == 5 && pass == 0;
                const u32 direction = (pass - (command.effect.kind == 5 ? 1 : 0)) % 4;
                const float offset = shadow ? 0.f : outline_offset;
                const float x = dx[direction] * offset, y = dy[direction] * offset;
                for (u32 i = 0; i < count; ++i) {
                    auto q = storage->quads[first_quad + extra + i];
                    if (q.u0 >= 2.f)
                        q.color.alpha = 0.f;
                    q.left += x;
                    q.right += x;
                    q.top += y;
                    q.bottom += y;
                    q.color.blue = -1;
                    q.color.green = shadow ? 2.f : 1.f;
                    storage->quads[first_quad + pass * count + i] = q;
                }
            }
            storage->quad_count += extra;
            if (storage->batch_count)
                storage->batches[storage->batch_count - 1].quad_count += extra;
        }
    }
    return appended;
}

bool append_message_text_command(D3D11Renderer* renderer, const MessageTextPresentation* presentation,
                                 const MessageTextCommand& command, u32 command_index, const Theme& theme,
                                 bool* atlas_invalidated, bool search_result,
                                 bool force_new_batch) noexcept {
    if (command.measurement_kind > MessageTextMeasurementKind::notice ||
        (command.measurement_kind != MessageTextMeasurementKind::none &&
         command.measurement_index >= presentation->measurement_count)) {
        return false;
    }
    if (d3d11_renderer_audio_owner(*renderer, command.owner))
        return true;
    if (d3d11_renderer_poll_owner(*renderer, command.owner))
        return true;
    const auto* scene = search_result && renderer->search_scene
                            ? ui_search_scene_message_scene(*renderer->search_scene)
                            : renderer->message_scene;
    const auto* users = search_result ? renderer->search_message_users : renderer->user_store;
    auto* cache = search_result ? renderer->search_message_measure_cache : renderer->message_measure_cache;
    const UiRect* batch_clip = scene ? &scene->viewport : nullptr;
    MessageInteractionLayout interaction{};
    if (scene && users && message_interaction_layout(*scene, command, *users, cache, theme, &interaction)) {
        // "<user> used <command>" is laid out piece by piece: the name and
        // server tag chip muted, "used" and the command chip at full strength.
        const UiTextCommand* parts[]{&interaction.name, &interaction.used, &interaction.command};
        bool first = force_new_batch;
        for (const auto* part : parts) {
            if (part->text.bytes && !append_content_text_command(renderer, *part, theme, atlas_invalidated, first,
                                                                 batch_clip))
                return false;
            first = false;
        }
        if (!interaction.tag.label.bytes)
            return true;
        UiTextCommand tag_command{};
        UiTextCommandStream stream{};
        return ui_text_command_stream_initialize(&stream, {&tag_command, 1}) &&
               ui_badge_text(interaction.tag, scene->widgets, theme, &stream) &&
               append_content_text_command(renderer, tag_command, theme, atlas_invalidated, false, batch_clip);
    }
    MessageAuthorLayout header{};
    if (scene && renderer->chat_state && users &&
        message_author_layout(*scene, command, *renderer->chat_state, *users, cache, theme, &header,
                              presentation->guild_members)) {
        if (!append_content_text_command(renderer, header.name, theme, atlas_invalidated,
                                          force_new_batch, batch_clip))
            return false;
        const UiTextCommand* decorations[]{&header.name_prefix, &header.name_suffix};
        for (const auto* decoration : decorations)
            if (decoration->text.bytes &&
                !append_content_text_command(renderer, *decoration, theme, atlas_invalidated,
                                              false, batch_clip))
                return false;
        const UiBadgeWidget* badges[2 + kUiPluginRenderBadges]{&header.badge, &header.plugin.badge};
        for (u32 d = 0; d < header.decoration_count; ++d)
            badges[2 + d] = &header.decorations[d].badge;
        for (const auto* badge : badges) {
            if (!badge || !badge->label.bytes)
                continue;
            UiTextCommand badge_command{};
            UiTextCommandStream stream{};
            if (!ui_text_command_stream_initialize(&stream, {&badge_command, 1}) ||
                !ui_badge_text(*badge, scene->widgets, theme, &stream) ||
                !append_content_text_command(renderer, badge_command, theme, atlas_invalidated,
                                              false, batch_clip))
                return false;
        }
        return !header.timestamp.text.bytes ||
               append_content_text_command(renderer, header.timestamp, theme, atlas_invalidated,
                                            false, batch_clip);
    }
    float shaped_height = 0.0f;
    const bool selected =
        !search_result && renderer->message_text_selection_message_id == command.message_id &&
        renderer->message_text_selection_command_identity == command.measurement_identity &&
        renderer->message_text_selection_revision == command.message_revision &&
        renderer->message_text_selection_command_index == command_index;
    const u32 first_semantic = renderer->content_text->content_semantic_count;
    const u32 first_region = renderer->content_text->content_hit_region_count;
    const ContentTextRequest request{
        command.text, ui_rect_width(command.rectangle), ui_rect_height(command.rectangle),
        command.rectangle.left, command.rectangle.top, &command.clip, batch_clip, &command.color,
        command.styles, command.themed_styles ? &theme : nullptr, &command.inline_role_color,
        command.semantics, command.owner, command.message_id, command.message_revision,
        renderer->message_text_selection_anchor, renderer->message_text_selection_caret,
        command.horizontal_alignment, command.vertical_alignment, command.wrapping, command.overflow,
        UiTextFace::normal, force_new_batch, selected, false, false, true};
    if (!append_content_text(renderer, request, &shaped_height, atlas_invalidated)) {
        return false;
    }
    {
        auto& storage = *renderer->content_text;
        for (u32 i = first_semantic; i < storage.content_semantic_count; ++i) {
            auto& semantic = storage.content_semantics[i];
            if (search_result)
                semantic.reserved |= kRendererSearchSemanticFlag;
            const u32 source_index = i - first_semantic;
            if (source_index >= command.semantics.count)
                continue;
            const auto& source = command.semantics[source_index];
            for (u32 j = 0; j < command.styles.count; ++j) {
                const auto& span = command.styles[j];
                if ((span.flags & text_style_spoiler) && source.byte_offset >= span.byte_offset &&
                    source.byte_offset - span.byte_offset < span.byte_count) {
                    semantic.reserved |= 2u;
                    break;
                }
            }
        }
    }
    // A link chip's server without an icon shows its initials in the cell; the
    // round plate is painted beneath the text.
    auto& storage = *renderer->content_text;
    for (u32 i = first_region; i < storage.content_hit_region_count; ++i) {
        const TextHitRegion region = storage.content_hit_regions[i];
        if (region.semantic_index >= storage.content_semantic_count || !renderer->chat_state)
            continue;
        const MessageTextSemantic& semantic = storage.content_semantics[region.semantic_index];
        const GuildRecord* guild = semantic.kind == MessageMarkdownSemanticKind::guild_icon &&
                                           (semantic.reserved & kMessageMarkdownChipInitials) &&
                                           !(semantic.reserved & 2u)
                                       ? chat_state_store_find_guild(*renderer->chat_state, semantic.target)
                                       : nullptr;
        if (!guild)
            continue;
        char letters[24];
        u32 characters = 0;
        const float side = region.right - region.left, middle = (region.top + region.bottom) * .5f;
        UiTextCommand initials{};
        initials.text = ui_guild_acronym(chat_state_store_guild_name(*renderer->chat_state, *guild),
                                         {letters, sizeof(letters)}, &characters);
        initials.owner = command.owner;
        initials.rectangle = {region.left, middle - side * .5f, region.right, middle + side * .5f};
        initials.clip = ui_rect_intersection(command.clip, initials.rectangle);
        initials.color = theme.tokens.text.normal;
        initials.style_flags = kTextStyleSizeCodeXs;
        initials.horizontal_alignment = initials.vertical_alignment = UiTextAlignment::center;
        initials.wrapping = TextWrapping::no_wrap;
        if (initials.text.bytes && ui_rect_has_area(initials.clip) &&
            !append_content_text_command(renderer, initials, theme, atlas_invalidated, false, batch_clip))
            return false;
    }
    return true;
}

} // namespace renderer_text_detail
} // namespace maicord
