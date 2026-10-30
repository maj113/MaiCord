#include "maicord/ui/ansi_text.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/profile_appearance.hpp"
#include "maicord/ui/message_author.hpp"
#include "maicord/ui/message_interaction.hpp"
#include "maicord/ui/native_text_candidate.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/render/colr_paint.hpp"
#include "picker_glyph_cache.hpp"
#include "shaped_text_cache.hpp"

#include "maicord/core/log.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/text/font_session.hpp"
#include "maicord/ui/audio_presentation.hpp"
#include "maicord/ui/poll_presentation.hpp"
#include "maicord/render/glyph_quad_bounds.hpp"
#include "maicord/ui/login_text_presentation.hpp"
#include "maicord/ui/message_action_bar_presentation.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/ui/media_picker_presentation.hpp"
#include "maicord/ui/reaction_viewer_presentation.hpp"
#include "maicord/ui/shell_text_presentation.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/ui/inline_flag_projection.hpp"
#include "maicord/render/inline_media.hpp"
#include "maicord/ui/thread_header.hpp"

#include "d3d11_renderer_edit.hpp"
#include "d3d11_renderer_interaction_state.hpp"
#include "d3d11_renderer_login_form.hpp"

#include <stddef.h>

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord {
struct RendererTypographyOwner {
    TextSystem system{};
    TextSystemTypography policy{};
    u32 native_code_format{};
};
namespace {

struct TextRasterScope {
    D3D11GlyphTextOwner& owner;
    ColrPaintBatch* previous;
    ColrPaintBatch batch;
    explicit TextRasterScope(D3D11Renderer& renderer) noexcept
        : owner(renderer.glyph_text), previous(owner.color_batch), batch(renderer.graphics.context) {
        if (previous)
            previous->reset();
        owner.color_batch = &batch;
    }
    ~TextRasterScope() noexcept {
        batch.reset();
        owner.color_batch = previous;
    }
};

bool resolve_role_mention_color(const D3D11Renderer* renderer, u64 message_id, u64 role_id,
                                Color* color) noexcept {
    // Message id zero is the composer: its projected role spans carry colours.
    if (renderer && color && message_id == 0 && role_id != 0 && renderer->message_composer_scene) {
        const auto& composer = *renderer->message_composer_scene;
        for (u32 index = 0; composer.projection_spans && index < composer.projection_span_count; ++index) {
            const auto& span = composer.projection_spans[index];
            if (span.kind == MessageComposerProjectionKind::role && span.target == role_id && span.color) {
                *color = color_from_rgb24(span.color);
                return true;
            }
        }
        return false;
    }
    if (!renderer || !renderer->chat_state || !renderer->message_scene ||
        !renderer->message_scene->timeline || !renderer->message_scene->timeline->store || !color ||
        message_id == 0 || role_id == 0) {
        return false;
    }
    const MessageStoreResult found =
        message_store_find(*renderer->message_scene->timeline->store, message_id);
    const MessageRecord* message =
        found.ok() ? message_store_resolve(*renderer->message_scene->timeline->store, found.handle)
                   : nullptr;
    const ChannelRecord* channel =
        message ? chat_state_store_find_channel(*renderer->chat_state, message->channel_id) : nullptr;
    const RoleRecord* role =
        channel && channel->guild_id != 0
            ? chat_state_store_find_role(*renderer->chat_state, channel->guild_id, role_id)
            : nullptr;
    if (!role || role->color == 0) {
        return false;
    }
    *color = color_from_rgb24(role->color);
    return true;
}

bool resolve_role_mention_color_callback(void* context, u64 message_id, u64 role_id,
                                         Color* color) noexcept {
    return resolve_role_mention_color(static_cast<const D3D11Renderer*>(context), message_id, role_id,
                                      color);
}

float aligned_text_origin(UiTextAlignment alignment, float start, float available, float used) noexcept {
    const float remaining = maximum(0.0f, available - used);
    if (alignment == UiTextAlignment::center) {
        return start + remaining * 0.5f;
    }
    if (alignment == UiTextAlignment::end) {
        return start + remaining;
    }
    return start;
}

bool append_login_text_command(D3D11Renderer* renderer, const UiTextCommand& command,
                               bool* atlas_invalidated) noexcept {
    if (command.text.bytes == 0) {
        return true;
    }
    const TextStyleSpan span{0, command.text.bytes, command.style_flags};
    const float maximum_width = maximum(ui_rect_width(command.rectangle), 1.0f);
    const float maximum_height = maximum(ui_rect_height(command.rectangle), 1.0f);
    const TextShapeResult shaped =
        text_shape_utf8(&renderer->glyph_text.system, command.text,
                        {&span, command.style_flags == text_style_none ? 0u : 1u}, maximum_width,
                        maximum_height, Slice<wchar_t>{renderer->glyph_text.utf16, kRendererGlyphCapacity},
                        &renderer->glyph_text.title, command.wrapping, command.overflow);
    if (!shaped.ok()) {
        return false;
    }
    const float origin_x = aligned_text_origin(command.horizontal_alignment, command.rectangle.left,
                                               maximum_width, renderer->glyph_text.title.width);
    const float origin_y = aligned_text_origin(command.vertical_alignment, command.rectangle.top,
                                               maximum_height, renderer->glyph_text.title.height);
    const D3D11GlyphTextAppendRequest request{
        &renderer->glyph_text.title,
        renderer->glyph_text.title_quads,
        &renderer->glyph_text.title_quad_count,
        atlas_invalidated,
        {},
        {},
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        command.color,
        origin_x,
        origin_y,
        0,
        0,
        kRendererGlyphCapacity,
        renderer->dpi,
    };
    const bool appended =
        d3d11_glyph_text_append(&renderer->glyph_text, renderer->graphics.context, request);
    return appended;
}

// Appends the login commands to a fresh title stream; a full atlas is reset and the
// stream rebuilt, up to `attempts` times.
bool append_login_commands(D3D11Renderer* renderer, const UiTextCommand* commands, u32 count,
                           u32 attempts) noexcept {
    for (u32 attempt = 0; attempt < attempts; ++attempt) {
        bool invalidated = false;
        bool valid = true;
        TextRasterScope raster_scope(*renderer);
        renderer->glyph_text.title_quad_count = 0;
        for (u32 i = 0; valid && i < count; ++i)
            valid = append_login_text_command(renderer, commands[i], &invalidated) && !invalidated;
        if (valid)
            return true;
        if (!invalidated)
            return false;
        d3d11_glyph_text_reset_atlas(&renderer->glyph_text);
    }
    return false;
}

} // namespace

bool d3d11_renderer_rebuild_login_text(D3D11Renderer* renderer, const RendererLoginView& view,
                                       const Theme& theme) noexcept {
    UiTextCommand commands[kMaximumLoginTextCommands]{};
    UiTextCommandStream stream{};
    return ui_text_command_stream_initialize(&stream,
                                             Slice<UiTextCommand>{commands, kMaximumLoginTextCommands}) &&
           login_text_presentation_build(*renderer->login_scene, view, theme, &stream) &&
           append_login_commands(renderer, stream.storage.data, stream.count, 2) &&
           renderer->glyph_text.title_quad_count != 0;
}

bool d3d11_renderer_capture_login_form_text(D3D11Renderer* renderer, const LoginView& view,
                                            const Theme& theme) noexcept {
    auto* owned = renderer ? renderer->login_form_text : nullptr;
    if (!owned || !renderer->login_scene)
        return false;
    UiTextCommand commands[kMaximumLoginTextCommands]{};
    UiTextCommandStream source{};
    if (!ui_text_command_stream_initialize(&source, {commands, kMaximumLoginTextCommands}) ||
        !login_text_presentation_build(*renderer->login_scene, view, theme, &source))
        return false;
    u32 bytes = 0;
    for (u32 i = 0; i < source.count; ++i) {
        if (source.storage[i].styles.count || source.storage[i].text.bytes > sizeof(owned->display) - bytes)
            return false;
        bytes += source.storage[i].text.bytes;
    }
    const u64 previous = owned->projection.signature;
    if (!ui_text_command_stream_initialize(&owned->projection, {owned->commands, kMaximumLoginTextCommands},
                                           {owned->display, sizeof(owned->display)}))
        return false;
    for (u32 i = 0; i < source.count; ++i)
        if (!ui_text_command_stream_push_owned(&owned->projection, source.storage[i])) {
            owned->valid = false;
            return false;
        }
    if (!owned->valid || previous != owned->projection.signature)
        content_text_retention_invalidate(&renderer->content_text_retention);
    owned->valid = true;
    return true;
}

bool d3d11_renderer_restore_login_form_base(D3D11Renderer* renderer) noexcept {
    const auto* owned = renderer ? renderer->login_form_text : nullptr;
    if (!owned || !owned->valid)
        return false;
    return append_login_commands(renderer, owned->projection.storage.data, owned->projection.count, 4);
}

namespace {

bool append_text_batch(RendererTextStorage* storage, const UiRect* clip, u32 first_quad,
                       bool force_new_batch, const UiRect* batch_clip) noexcept {
    if (first_quad > storage->quad_count) {
        return false;
    }
    const u32 count = storage->quad_count - first_quad;
    if (count == 0) {
        return true;
    }
    // A row/control scissor does no work when all its ink is inside it: only
    // the shared clip still bounds it, so adjacent runs can occupy one batch.
    if (batch_clip && !ui_rect_equal(*clip, *batch_clip)) {
        const UiRect bounds = *clip;
        u32 i = first_quad;
        for (; i < storage->quad_count; ++i) {
            const auto& q = storage->quads[i];
            if (q.left < bounds.left || q.top < bounds.top || q.right > bounds.right ||
                q.bottom > bounds.bottom)
                break;
        }
        if (i == storage->quad_count)
            clip = batch_clip;
    }
    if (!force_new_batch && storage->batch_count != 0) {
        GlyphDrawBatch& last = storage->batches[storage->batch_count - 1];
        if (ui_rect_equal(last.clip, *clip) && last.first_quad + last.quad_count == first_quad) {
            last.quad_count += count;
            return true;
        }
    }
    if (storage->batch_count >= storage->batch_capacity) {
        return false;
    }
    storage->batches[storage->batch_count++] = GlyphDrawBatch{*clip, first_quad, count};
    return true;
}

struct FontAsset {
    const wchar_t* family;
    const wchar_t* file;
    float tracking;
};
constexpr FontAsset text_fonts[]{{L"Segoe UI", nullptr, 0},
                                 {L"Bahnschrift", nullptr, 0},
                                 {L"Georgia", nullptr, 0},
                                 {L"Arial Rounded MT Bold", nullptr, 0},
                                 {L"Consolas", nullptr, 0},
                                 {L"Segoe Print", nullptr, 0},
                                 {L"Orbitron", L"Orbitron.ttf", .03f},
                                 {L"Bangers", L"Bangers.ttf", 0},
                                 {L"MaiCord BioRhyme", L"BioRhyme.ttf", 0},
                                 {L"Cherry Bomb One", L"CherryBomb.ttf", .04f},
                                 {L"Chicle", L"Chicle.ttf", 0},
                                 {L"Compagnon", L"Compagnon.otf", 0},
                                 {L"MaiCord MuseoModerno", L"MuseoModerno.ttf", .01f},
                                 {L"MaiCord Medieval", L"NeoCastel.otf", .02f},
                                 {L"MaiCord Pixelify", L"Pixelify.ttf", .02f},
                                 {L"Ribes", L"Ribes.otf", 0},
                                 {L"Sinistre", L"Sinistre.otf", .01f},
                                 {L"Zilla Slab", L"ZillaSlab.ttf", .03f},
                                 {L"MaiCord Playpen", L"Playpen.ttf", 0},
                                 {L"New Rocker", L"NewRocker.ttf", 0},
                                 {L"Kalam", L"Kalam.ttf", 0}};
static_assert(sizeof(text_fonts) / sizeof(text_fonts[0]) == static_cast<u32>(UiTextFace::count));
bool profile_font_path(u32 index, wchar_t* path) noexcept {
    const auto* file_name = text_fonts[index].file;
    if (!file_name)
        return false;
    const DWORD count = GetModuleFileNameW(nullptr, path, 1024);
    if (!count || count >= 900)
        return false;
    u32 at = count;
    while (at && path[at - 1] != L'\\')
        --at;
    const wchar_t prefix[] = L"fonts\\";
    for (u32 i = 0; i < 6; ++i)
        path[at++] = prefix[i];
    for (u32 i = 0; file_name[i]; ++i)
        path[at++] = file_name[i];
    path[at] = 0;
    return true;
}
void release_typography_owner(RendererTypographyOwner* owner) noexcept {
    if (!owner)
        return;
    text_system_release(&owner->system);
    VirtualFree(owner, 0, MEM_RELEASE);
}
u64 typography_source_identity(const TextSystem& system) noexcept {
    return native_text_format_identity(system);
}
// The renderer's body-weight format for one family, in the scope its font file registered.
bool create_face_format(const TextSystem& system, const wchar_t* family, u32 scope, u32* format) noexcept {
    u32 units = 0;
    while (family[units])
        ++units;
    return native_text_format_create(system.native_text,
                                     {{reinterpret_cast<const u16*>(family), units},
                                      scope,
                                      {400, 5, 0},
                                      system.em_size,
                                      system.em_size * 4,
                                      true},
                                     format);
}
bool prepare_typography_face(RendererTypographyOwner& owner, UiTextFace face, u32* native_format) noexcept {
    const u32 index = static_cast<u32>(face);
    auto& system = owner.system;
    const auto& asset = text_fonts[index];
    wchar_t path[1024]{};
    const bool private_font = asset.file != nullptr;
    if (private_font &&
        (!profile_font_path(index, path) || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES))
        return false;
    if (!system.native_text)
        return false;
    u32 scope{};
    return (!private_font || native_text_register_font_path(system.native_text, path, &scope)) &&
           create_face_format(system, asset.family, scope, native_format);
}
TextSystem command_text_system(D3D11Renderer* renderer, UiTextFace face) noexcept {
    auto system = renderer->glyph_text.system;
    const u32 index = static_cast<u32>(face);
    if (!index || index >= static_cast<u32>(UiTextFace::count))
        return system;
    if (face == UiTextFace::mono && renderer->typography_owner) {
        const auto& owner = *renderer->typography_owner;
        system.native_format = owner.native_code_format;
        const auto selected = static_cast<u32>(owner.policy.value.code_face);
        system.tracking = text_fonts[selected].tracking;
        system.fixed_weight = text_fonts[selected].file != nullptr;
        return system;
    }
    system.native_format = renderer->native_text_faces[index];
    if (!system.native_format && system.native_text) {
        wchar_t path[1024];
        const bool private_path = profile_font_path(index, path);
        u32 scope = 0;
        // Missing optional assets retain system-family fallback; existing files
        // are registered directly.
        const bool available = private_path && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
        if ((!available || native_text_register_font_path(system.native_text, path, &scope)) &&
            create_face_format(system, text_fonts[index].family, scope, &system.native_format))
            renderer->native_text_faces[index] = system.native_format;
    }
    system.tracking = text_fonts[index].tracking;
    system.fixed_weight = text_fonts[index].file != nullptr;
    return system;
}

// Everything append_content_text shapes and places for one text run.
struct ContentTextRequest {
    Utf8View input;
    float maximum_width, maximum_height, origin_x, origin_y;
    const UiRect* clip;
    const UiRect* batch_clip;
    const Color* color;
    ConstSlice<TextStyleSpan> styles;
    const Theme* style_theme;
    const Color* inline_role_color;
    ConstSlice<MessageMarkdownSemantic> semantics;
    UiWidgetReference layout_owner;
    u64 message_id;
    u32 message_revision, selection_anchor, selection_caret;
    UiTextAlignment horizontal_alignment, vertical_alignment;
    TextWrapping wrapping;
    TextOverflow overflow;
    UiTextFace face;
    bool force_new_batch, selection_active, cluster_effect, plain_flags, publish_semantics;
};

bool append_content_text(D3D11Renderer* renderer, const ContentTextRequest& request, float* shaped_height,
                         bool* atlas_invalidated) noexcept {
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
    const bool cluster_effect = request.cluster_effect, plain_flags = request.plain_flags,
               publish_semantics = request.publish_semantics;
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
    UiInlineFlagProjection flag_projection;
    RendererInlineMediaLayer flag_layer{};
    if (plain_flags && !semantics.count && !selection_active &&
        renderer_inline_media_owner(*renderer, layout_owner, &flag_layer) &&
        ui_widget_manager_resolve(*layout_owner.manager, layout_owner.handle)->kind !=
            UiWidgetKind::text_field &&
        (!styles.count ||
         (styles.count == 1 && !styles[0].byte_offset && styles[0].byte_count == input.bytes))) {
        const bool projected =
            ui_inline_flag_project(input, styles.count ? styles[0].flags : 0, &flag_projection);
        if (!projected || flag_projection.count > storage->hit_span_capacity ||
            flag_projection.count > kRendererInlineFlagCapacity - renderer->inline_media.count) {
            flag_projection.count = 0;
            saturating_increment(renderer->inline_media.rejected);
        }
        if (flag_projection.count && !renderer->inline_media.flags) {
            renderer->inline_media.flags = static_cast<RendererInlineFlag*>(
                VirtualAlloc(nullptr, sizeof(RendererInlineFlag) * kRendererInlineFlagCapacity,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!renderer->inline_media.flags) {
                flag_projection.count = 0;
                saturating_increment(renderer->inline_media.rejected);
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
    saturating_increment_if(semantic_count != semantics.count, storage->semantic_truncations);
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
            saturating_increment(renderer->inline_media.rejected);
        }
        interactive_count = 0;
        semantic_count = 0;
        selection_active = false;
        saturating_increment(storage->semantic_truncations);
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
        resolve_role_mention_color_callback,
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
            if (renderer->inline_media.count == kRendererInlineFlagCapacity) {
                saturating_increment(renderer->inline_media.rejected);
                break;
            }
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
                                 bool* atlas_invalidated, bool force_new_batch = false,
                                 const UiRect* batch_clip = nullptr) noexcept {
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
        composer_edit || bio ? &theme : nullptr, nullptr, bio_semantics, command.owner, 0, 0,
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
                                 bool* atlas_invalidated, bool search_result = false,
                                 bool force_new_batch = false) noexcept {
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
    // A link chip's server without an icon shows its initials in the cell (the
    // official acronym icon); the round plate is painted beneath the text.
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

// Glyph inputs beyond the projection (appearance and thread header revisions,
// a hovered name effect, composer styles and selection) key the shell exactly.
u64 retention_shell_signature(const D3D11Renderer& renderer, u64 shell_signature) noexcept {
    const bool settings_active = renderer_settings_page_visible(renderer);
    const bool friends_active =
        !settings_active && d3d11_renderer_active(renderer.friends_scene);
    // A hovered header chip or quote restyles its message's retained text.
    if (renderer.message_scene && renderer.message_scene->header_hover_message_id)
        shell_signature = hash64_mix(hash64_mix(shell_signature, renderer.message_scene->header_hover_message_id),
                                     renderer.message_scene->header_hover_target);
    const bool browser_active = d3d11_renderer_thread_browser_visible(renderer);
    const auto* composer =
        settings_active || friends_active || browser_active ? nullptr : renderer.message_composer_scene;
    if (!settings_active && renderer.user_store)
        shell_signature = hash64_mix(shell_signature, renderer.user_store->appearance.revision);
    if (!settings_active && !friends_active && !browser_active && renderer.thread_header)
        shell_signature = hash64_mix(shell_signature, renderer.thread_header->revision);
    if (!settings_active && !friends_active && !browser_active && renderer.message_scene &&
        renderer.user_store && renderer.chat_state) {
        // Ordinary row hover changes paint and the action-bar suffix. Only a
        // visible author name effect changes the retained base glyphs; grouped
        // rows have no author header to animate.
        const auto& scene = *renderer.message_scene;
        const auto* hovered = message_scene_resolve(scene, scene.widgets.hovered);
        const auto* record = hovered && !hovered->grouped && scene.timeline && scene.timeline->store
                                 ? message_store_resolve(*scene.timeline->store, hovered->message)
                                 : nullptr;
        if (record) {
            const auto& users = *renderer.user_store;
            const auto* global = user_appearance_find(users.appearance, record->author_id);
            const auto* channel =
                chat_state_store_find_channel(*renderer.chat_state, record->channel_id);
            const auto* guild =
                channel ? user_appearance_find(users.appearance, record->author_id, channel->guild_id)
                        : nullptr;
            // Match message_author_layout's guild/global style source.
            const auto* styled = guild && (guild->name.font_id || guild->name.effect_id) ? guild : global;
            if (styled && profile_name_effect(styled->name, true).kind) {
                shell_signature = hash64_mix(shell_signature, record->id);
                shell_signature = hash64_mix(shell_signature, scene.widgets.hover_started_ms);
            }
        }
    }
    if (composer && composer->active)
        shell_signature = hash64_counted_bytes(shell_signature, composer->display_styles,
                                               composer->display_style_count * sizeof(TextStyleSpan));
    if (composer && composer->model && composer->model->focused &&
        composer->display_anchor != composer->display_caret) {
        shell_signature = hash64_mix(shell_signature, composer->display_anchor);
        shell_signature = hash64_mix(shell_signature, composer->display_caret);
    }
    return shell_signature;
}

ContentTextRetentionKey content_text_retention_key(const D3D11Renderer& renderer, const Theme& theme,
                                                   u64 shell_signature, u64 popup_signature,
                                                   bool include_messages) noexcept {
    const bool login_form = d3d11_renderer_login_form_text(renderer);
    if (!login_form)
        shell_signature = retention_shell_signature(renderer, shell_signature);
    include_messages = include_messages && !login_form;
    const MessageTextPresentation* presentation = renderer.message_text;
    return ContentTextRetentionKey{
        shell_signature,
        include_messages && presentation ? presentation->projection_signature : 0,
        popup_signature,
        hash64_mix(content_text_theme_signature(theme),
                   typography_source_identity(renderer.glyph_text.system)),
        include_messages ? renderer.message_text_selection_message_id : 0,
        include_messages ? renderer.message_text_selection_command_identity : 0,
        include_messages ? renderer.message_text_selection_revision : 0,
        include_messages ? renderer.message_text_selection_command_index : 0,
        include_messages ? renderer.message_text_selection_anchor : 0,
        include_messages ? renderer.message_text_selection_caret : 0,
        renderer.dpi,
        renderer.glyph_text.allocator.evicted_entries,
        renderer.glyph_text.allocator.full_resets,
        include_messages,
    };
}

// Friends search borrows composer_layout while the main chat content is
// covered. The thread browser has no field: the header search bar serves it.
bool measure_content_search(D3D11Renderer* renderer) noexcept {
    reset_value(&renderer->friends_search_geometry);
    const auto* scene = renderer->friends_scene;
    if (!scene || !scene->active || !scene->content || !scene->content->search.active ||
        d3d11_renderer_active(renderer->friends_requests_scene)) {
        text_edit_layout_invalidate(&renderer->composer_layout);
        return true;
    }
    auto& layout = renderer->composer_layout;
    if (renderer->edit_selection_owner != &layout) {
        text_edit_layout_invalidate(&renderer->login_edit);
        renderer->edit_selection_owner = &layout;
    }
    return d3d11_renderer_measure_search_edit(&layout, renderer->edit_selections, scene->content->search,
                                              ui_friends_scene_search_geometry(*scene).text,
                                              renderer->glyph_text.system, true, &renderer->friends_search_geometry);
}

bool measure_message_search(D3D11Renderer* renderer) noexcept {
    reset_value(&renderer->search_edit_geometry);
    if (!d3d11_renderer_search_visible(*renderer)) {
        text_edit_layout_invalidate(&renderer->search_layout);
        return true;
    }
    const auto& scene = *renderer->search_scene;
    return d3d11_renderer_measure_search_edit(&renderer->search_layout, renderer->search_edit_selections,
                                              scene.content->edit, scene.text, renderer->glyph_text.system, false,
                                              &renderer->search_edit_geometry);
}
bool measure_media_picker(D3D11Renderer* renderer) noexcept {
    reset_value(&renderer->media_picker_edit_geometry);
    auto& layout = renderer->media_picker_layout;
    const bool quick = d3d11_renderer_quick_switcher_visible(*renderer);
    const bool forward = d3d11_renderer_message_forward_visible(*renderer);
    if (!quick && !forward && !d3d11_renderer_media_picker_visible(*renderer)) {
        text_edit_layout_invalidate(&layout);
        return true;
    }
    const auto& edit = forward ? renderer->message_forward_scene->content->edit
                       : quick ? renderer->quick_switcher_scene->content->edit
                               : renderer->media_picker_scene->content->edit;
    const auto bounds = forward ? renderer->message_forward_scene->text
                        : quick ? renderer->quick_switcher_scene->text
                                : renderer->media_picker_scene->text;
    return d3d11_renderer_measure_search_edit(&layout, renderer->media_picker_edit_selections, edit, bounds,
                                              renderer->glyph_text.system, true,
                                              &renderer->media_picker_edit_geometry);
}
// Boundary ordinals within the overlay text stream. The search message
// boundaries come last: the raster pass does not compare them.
struct TextOverlayBoundaries {
    u32 friends_end, media_picker_start, media_picker_end, menu_start, form_start, forum_end;
    u32 popup_start, popup_end, settings_choice_start, settings_choice_end, theme_editor_start,
        theme_editor_end, profile_start, profile_end;
    u32 audio_start{}, audio_end{};
    u32 poll_start{}, poll_end{};
    // Profile editor overlays on a settings page (~0u: none this frame).
    u32 settings_preview_start{~0u}, settings_preview_end{~0u}, settings_dialog_start{~0u}, settings_dialog_end{~0u};
    // The toast's text closes the stream (~0u: none this frame).
    u32 toast_start{~0u};
    u32 search_rich_start{}, search_rich_end{};
};
constexpr u32 kComparedOverlayBoundaryBytes = offsetof(TextOverlayBoundaries, search_rich_start);
static_assert(kComparedOverlayBoundaryBytes == 23 * sizeof(u32) &&
              sizeof(TextOverlayBoundaries) == kComparedOverlayBoundaryBytes + 2 * sizeof(u32));

// Where a boundary publishes: the renderer's text batch at that ordinal. The
// form start and forum end are gated on a form or settings page and handled
// beside this table; the menu start, last, has no end-of-stream endpoint.
struct OverlayBatchBoundary {
    u32 boundary, batch;
};
constexpr OverlayBatchBoundary kOverlayBatchBoundaries[]{
    {offsetof(TextOverlayBoundaries, friends_end), offsetof(D3D11Renderer, friends_requests_text_end_batch)},
    {offsetof(TextOverlayBoundaries, media_picker_start), offsetof(D3D11Renderer, media_picker_text_first_batch)},
    {offsetof(TextOverlayBoundaries, media_picker_end), offsetof(D3D11Renderer, media_picker_text_end_batch)},
    {offsetof(TextOverlayBoundaries, profile_start), offsetof(D3D11Renderer, profile_text_first_batch)},
    {offsetof(TextOverlayBoundaries, profile_end), offsetof(D3D11Renderer, profile_text_end_batch)},
    {offsetof(TextOverlayBoundaries, theme_editor_start), offsetof(D3D11Renderer, theme_editor_text_first_batch)},
    {offsetof(TextOverlayBoundaries, theme_editor_end), offsetof(D3D11Renderer, theme_editor_text_end_batch)},
    {offsetof(TextOverlayBoundaries, popup_start), offsetof(D3D11Renderer, popup_text_first_batch)},
    {offsetof(TextOverlayBoundaries, popup_end), offsetof(D3D11Renderer, popup_text_end_batch)},
    {offsetof(TextOverlayBoundaries, settings_choice_start),
     offsetof(D3D11Renderer, settings_choice_text_first_batch)},
    {offsetof(TextOverlayBoundaries, settings_choice_end), offsetof(D3D11Renderer, settings_choice_text_end_batch)},
    {offsetof(TextOverlayBoundaries, settings_preview_start),
     offsetof(D3D11Renderer, settings_preview_text_first_batch)},
    {offsetof(TextOverlayBoundaries, settings_preview_end), offsetof(D3D11Renderer, settings_preview_text_end_batch)},
    {offsetof(TextOverlayBoundaries, settings_dialog_start),
     offsetof(D3D11Renderer, settings_dialog_text_first_batch)},
    {offsetof(TextOverlayBoundaries, settings_dialog_end), offsetof(D3D11Renderer, settings_dialog_text_end_batch)},
    {offsetof(TextOverlayBoundaries, toast_start), offsetof(D3D11Renderer, toast_text_first_batch)},
    {offsetof(TextOverlayBoundaries, menu_start), offsetof(D3D11Renderer, menu_text_first_batch)},
};
constexpr u32 kOverlayEndBoundaryCount = sizeof(kOverlayBatchBoundaries) / sizeof(kOverlayBatchBoundaries[0]) - 1;

u32 overlay_boundary(const TextOverlayBoundaries& boundaries, const OverlayBatchBoundary& entry) noexcept {
    return *reinterpret_cast<const u32*>(reinterpret_cast<const u8*>(&boundaries) + entry.boundary);
}
u32& overlay_batch(D3D11Renderer* renderer, const OverlayBatchBoundary& entry) noexcept {
    return *reinterpret_cast<u32*>(reinterpret_cast<u8*>(renderer) + entry.batch);
}
bool project_toast_text(const D3D11Renderer* renderer, const Theme& theme, UiTextOutput* output,
                        TextOverlayBoundaries* boundaries) noexcept {
    if (!d3d11_renderer_toast_visible(*renderer))
        return true;
    boundaries->toast_start = output->emitter->count;
    return ui_toast_text_output(*renderer->toast_scene, theme, output);
}

bool search_message_command(const D3D11Renderer& renderer, u32 index, MessageTextCommand* native,
                            UiTextCommand* neutral) noexcept {
    const auto* presentation = renderer.search_message_text;
    if (!presentation || !presentation->active || !d3d11_renderer_search_visible(renderer) ||
        index >= presentation->command_count)
        return false;
    *native = presentation->commands[index];
    native->clip = ui_search_scene_content_clip(*renderer.search_scene, native->clip);
    if (!native->text.bytes || !ui_rect_has_area(native->rectangle) || !ui_rect_has_area(native->clip))
        return false;
    reset_value(neutral);
    neutral->text = native->text;
    neutral->owner = native->owner;
    neutral->rectangle = native->rectangle;
    neutral->clip = native->clip;
    neutral->color = native->color;
    neutral->styles = native->styles;
    neutral->horizontal_alignment = native->horizontal_alignment;
    neutral->vertical_alignment = native->vertical_alignment;
    neutral->wrapping = native->wrapping;
    neutral->overflow = native->overflow;
    return true;
}
bool project_search_message_text(D3D11Renderer* renderer, UiTextEmitter* emitter) noexcept {
    const auto* presentation = renderer->search_message_text;
    if (!d3d11_renderer_search_visible(*renderer) || !presentation || !presentation->active)
        return true;
    if (presentation->command_count > presentation->commands.count)
        return false;
    UiTextOutput output{nullptr, emitter};
    for (u32 i = 0; i < presentation->command_count; ++i) {
        MessageTextCommand native{};
        UiTextCommand neutral{};
        if (search_message_command(*renderer, i, &native, &neutral) &&
            !ui_text_output_push(&output, neutral))
            return false;
    }
    // Semantic/reveal changes can preserve identical displayed bytes.
    emitter->signature = hash64_mix(emitter->signature, presentation->projection_signature);
    return true;
}
void reset_motion_text_ranges(D3D11Renderer* renderer, u32 batch) noexcept {
    renderer->forum_text_end_batch = batch;
    renderer->profile_text_first_batch = renderer->profile_text_end_batch = batch;
    renderer->theme_editor_text_first_batch = renderer->theme_editor_text_end_batch = batch;
    renderer->settings_page_text_first_batch = renderer->settings_page_text_end_batch = batch;
    renderer->popup_text_first_batch = renderer->popup_text_end_batch = batch;
    renderer->settings_choice_text_first_batch = renderer->settings_choice_text_end_batch = batch;
    renderer->settings_preview_text_first_batch = renderer->settings_preview_text_end_batch = batch;
    renderer->settings_dialog_text_first_batch = renderer->settings_dialog_text_end_batch = batch;
}
struct OverlayReservation {
    u32 capacity, quads;
    u32 batches{};
    const UiWidgetManager* picker_owner{};
    UiRect picker_clip{};
};
bool reserve_overlay_text(void* context, const UiTextCommand& command, u32) noexcept {
    auto& reserve = *static_cast<OverlayReservation*>(context);
    const u64 requested = static_cast<u64>(command.text.bytes) + 2u;
    reserve.quads += static_cast<u32>(minimum<u64>(requested, reserve.capacity - reserve.quads));
    // Picker emoji are CPU-clipped to their cells and share a scissor within
    // one scene. Reserve their merged batch, not one batch per emoji.
    const auto* picker = (command.style_flags & text_style_picker_emoji) ? command.owner.manager : nullptr;
    if (!picker || picker != reserve.picker_owner || !ui_rect_equal(command.clip, reserve.picker_clip))
        ++reserve.batches;
    reserve.picker_owner = picker;
    reserve.picker_clip = command.clip;
    return true;
}
struct BrowserShellTextFilter {
    UiTextEmitter* target;
    UiRect panel;
};
bool emit_browser_shell_text(void* context, const UiTextCommand& command, u32) noexcept {
    const auto& filter = *static_cast<BrowserShellTextFilter*>(context);
    const auto visible = ui_rect_intersection(command.rectangle, command.clip);
    if (visible.left >= filter.panel.left && visible.top >= filter.panel.top &&
        visible.right <= filter.panel.right && visible.bottom <= filter.panel.bottom)
        return true;
    UiTextOutput output{nullptr, filter.target};
    return ui_text_output_push(&output, command);
}
// The shell before the member list rows, which end the shell's commands.
bool project_shell_static_text(D3D11Renderer* renderer, const Theme& theme,
                               UiTextEmitter* emitter) noexcept {
    if (d3d11_renderer_login_form_text(*renderer)) {
        const auto& cached = *renderer->login_form_text;
        if (!cached.valid)
            return false;
        UiTextOutput output{nullptr, emitter};
        for (u32 i = 0; i < cached.projection.count; ++i)
            if (!ui_text_output_push(&output, cached.projection.storage[i]))
                return false;
        return true;
    }
    const bool settings = renderer_settings_page_visible(*renderer);
    const bool friends = !settings && d3d11_renderer_active(renderer->friends_scene);
    const bool browser = d3d11_renderer_thread_browser_visible(*renderer);
    const bool hidden = friends || browser;
    BrowserShellTextFilter filter{emitter, browser ? renderer->thread_browser_scene->panel : UiRect{}};
    UiTextEmitter filtered{};
    if (browser && !ui_text_emitter_initialize(&filtered, emit_browser_shell_text, &filter))
        return false;
    UiTextOutput output{nullptr, browser ? &filtered : emitter};
    UiRect composer_rectangle{};
    const UiRect* composer = nullptr;
    if (!settings && !hidden && renderer->composer_layout.active &&
        renderer->composer_layout.layout_height > 0) {
        const auto& layout = renderer->composer_layout;
        composer_rectangle = {layout.origin_x, layout.origin_y, layout.origin_x + layout.width,
                              layout.origin_y + layout.layout_height};
        composer = &composer_rectangle;
    }
    if (!settings &&
        !shell_text_presentation_build_output(
            *renderer->ui_scene, *renderer->chat_state, *renderer->navigation_view, theme,
            *renderer->shell_navigation_scene, hidden ? nullptr : renderer->message_composer_scene, nullptr,
            composer, &output, renderer->user_store, renderer->guild_rail_scene,
            browser ? renderer->thread_browser_scene->view->parent_id : 0))
        return false;
    output.emitter = emitter;
    const bool projected =
        (settings || hidden || !renderer->thread_header ||
         ui_thread_header_text(*renderer->thread_header, theme, &output)) &&
        (!friends || ui_friends_scene_text_output(*renderer->friends_scene, theme, &output,
                                                  &renderer->friends_search_geometry)) &&
        (!browser || ui_thread_browser_scene_text_output(*renderer->thread_browser_scene, theme, &output));
    const auto mix_owner = [emitter](const auto& owner) noexcept {
        emitter->signature = hash64_mix(emitter->signature, reinterpret_cast<usize>(owner.manager));
        emitter->signature = hash64_mix(emitter->signature, owner.handle.slot);
        emitter->signature = hash64_mix(emitter->signature, owner.handle.generation);
    };
    // Selected audio replaces this attachment's static labels. Guard that
    // omission in the retained base; elapsed time remains suffix-only.
    if (projected && d3d11_renderer_audio_scene_visible(*renderer))
        mix_owner(renderer->audio_scene->owner);
    if (projected && d3d11_renderer_poll_scene_visible(*renderer))
        mix_owner(renderer->poll_scene->owner);
    return projected;
}
// The member list rows, appended after the static shell in the same emitter.
bool project_shell_member_text(D3D11Renderer* renderer, const Theme& theme,
                               UiTextEmitter* emitter) noexcept {
    if (d3d11_renderer_login_form_text(*renderer) || renderer_settings_page_visible(*renderer) ||
        d3d11_renderer_active(renderer->friends_scene) || d3d11_renderer_thread_browser_visible(*renderer))
        return true;
    UiTextOutput output{nullptr, emitter};
    // The DM profile panel's text goes with the member slot's.
    return (!renderer->member_list_scene ||
            shell_text_presentation_member_output(*renderer->member_list_scene, theme, &output)) &&
           (!d3d11_renderer_active(renderer->dm_profile_panel) ||
            profile_scene_text_output(*renderer->dm_profile_panel, theme, &output, nullptr));
}
bool project_shell_text(D3D11Renderer* renderer, const Theme& theme, UiTextEmitter* emitter) noexcept {
    return project_shell_static_text(renderer, theme, emitter) &&
           project_shell_member_text(renderer, theme, emitter);
}
bool project_overlay_text(D3D11Renderer* renderer, const Theme& theme, UiTextEmitter* emitter,
                          TextOverlayBoundaries* boundaries) noexcept {
    if (d3d11_renderer_login_form_text(*renderer)) {
        reset_value(boundaries);
        UiTextOutput output{nullptr, emitter};
        return !d3d11_renderer_text_form_active(*renderer) ||
               (renderer->text_form->scene->content.confirmation_only &&
                text_form_text_output(*renderer->text_form->scene, renderer->text_form->geometry, theme,
                                      &output));
    }
    const bool settings = renderer_settings_page_visible(*renderer);
    const bool friends = !settings && d3d11_renderer_active(renderer->friends_scene);
    const bool requests =
        !settings && d3d11_renderer_active(renderer->friends_requests_scene);
    const bool form = d3d11_renderer_text_form_active(*renderer);
    const bool forum = d3d11_renderer_forum_form_active(*renderer);
    UiTextOutput output{nullptr, emitter};
    if (settings) {
        boundaries->friends_end = boundaries->menu_start = 0;
        if (!ui_settings_scene_text_output(*renderer->settings_scene, theme, &output))
            return false;
        // The profile editor's preview card draws its text over its media.
        const auto* preview = renderer->settings_profile_preview;
        boundaries->settings_preview_start = emitter->count;
        if (preview && preview->active && !profile_scene_text_output(*preview, theme, &output, nullptr))
            return false;
        boundaries->settings_preview_end = emitter->count;
        boundaries->form_start = emitter->count;
        boundaries->popup_start = boundaries->popup_end = emitter->count;
        boundaries->settings_choice_start = emitter->count;
        const auto* popup = d3d11_renderer_settings_popup(*renderer);
        if (popup && !popup_text_presentation_build_output(*popup, theme, &output,
                                                          renderer->message_measure_cache))
            return false;
        boundaries->settings_choice_end = emitter->count;
        const auto* dialog = renderer->settings_profile_dialog;
        boundaries->settings_dialog_start = emitter->count;
        if (dialog && dialog->active && !ui_profile_dialog_text_output(*dialog, theme, &output))
            return false;
        boundaries->settings_dialog_end = emitter->count;
        boundaries->theme_editor_start = boundaries->theme_editor_end = emitter->count;
        boundaries->form_start = emitter->count;
        if (forum && !forum_post_text_output(*renderer->forum_form->scene, renderer->forum_form->geometry,
                                             theme, &output))
            return false;
        boundaries->forum_end = emitter->count;
        if (form && !text_form_text_output(*renderer->text_form->scene, renderer->text_form->geometry,
                                           theme, &output))
            return false;
        return project_toast_text(renderer, theme, &output, boundaries);
    }
    // The call view leads the side-panel text range, above the base content.
    if (d3d11_renderer_call_visible(*renderer) &&
        !ui_call_scene_text_output(*renderer->call_scene, theme, &output))
        return false;
    if (d3d11_renderer_unread_banner_visible(*renderer) &&
        !ui_unread_banner_text_output(*renderer->unread_banner_scene, theme, &output))
        return false;
    if (d3d11_renderer_video_scene_visible(*renderer)) {
        const auto* video_owner = renderer->video_scene_search_result && renderer->search_scene
                                      ? ui_search_scene_message_scene(*renderer->search_scene)
                                      : renderer->message_scene;
        if (!video_owner || !ui_video_scene_text_output(*renderer->video_scene, theme, &output,
                                                        {&video_owner->widgets, video_owner->root}))
            return false;
    }
    if (d3d11_renderer_search_visible(*renderer) &&
        !ui_search_scene_text_output(*renderer->search_scene, theme, &output,
                                     &renderer->search_edit_geometry))
        return false;
    boundaries->search_rich_start = emitter->count;
    if (!project_search_message_text(renderer, emitter))
        return false;
    boundaries->search_rich_end = emitter->count;
    if (d3d11_renderer_pinned_messages_visible(*renderer) &&
        !ui_pinned_messages_scene_text_output(*renderer->pinned_messages_scene, theme, &output))
        return false;
    if (requests &&
        !ui_friends_scene_text_output(*renderer->friends_requests_scene, theme, &output, nullptr))
        return false;
    boundaries->friends_end = emitter->count;
    boundaries->media_picker_start = emitter->count;
    if (d3d11_renderer_media_picker_visible(*renderer) &&
        !ui_media_picker_scene_text_output(*renderer->media_picker_scene, theme, &output,
                                           &renderer->media_picker_edit_geometry))
        return false;
    boundaries->media_picker_end = emitter->count;
    // The action bar's opaque panel is painted after the gallery range. Its
    // quick-reaction glyphs must belong to the following text range as well.
    // Beside a call its text chat keeps the toolbar (as painted in layout).
    if (!friends && !d3d11_renderer_thread_browser_visible(*renderer) &&
        (!d3d11_renderer_call_visible(*renderer) || renderer->call_scene->chat_open) &&
        !d3d11_renderer_active(renderer->profile_scene) &&
        !d3d11_renderer_media_picker_visible(*renderer) && renderer->message_action_bar &&
        !d3d11_renderer_message_modal_active(*renderer) && renderer->message_action_bar->active &&
        !message_action_bar_text_presentation_build_output(*renderer->message_action_bar, theme, &output))
        return false;
    boundaries->profile_start = emitter->count;
    if (!settings && renderer->profile_scene) {
        const auto profile_edit = d3d11_renderer_profile_edit_geometry(*renderer);
        if (!profile_scene_text_output(*renderer->profile_scene, theme, &output, &profile_edit))
            return false;
    }
    boundaries->profile_end = emitter->count;
    boundaries->menu_start = emitter->count;
    boundaries->popup_start = emitter->count;
    if (d3d11_renderer_download_visible(*renderer) &&
        !ui_download_scene_text_output(*renderer->download_scene, theme, &output))
        return false;
    if (d3d11_renderer_message_forward_visible(*renderer)) {
        const auto popup_edit = d3d11_renderer_popup_edit_geometry(*renderer);
        if (!ui_message_forward_scene_text_output(*renderer->message_forward_scene, theme, &output,
                                                  &renderer->media_picker_edit_geometry, &popup_edit))
            return false;
    }
    boundaries->audio_start = emitter->count;
    if (d3d11_renderer_audio_scene_visible(*renderer) &&
        !ui_audio_scene_text_output(*renderer->audio_scene, &output))
        return false;
    boundaries->audio_end = emitter->count;
    boundaries->poll_start = emitter->count;
    if (d3d11_renderer_poll_scene_visible(*renderer) &&
        !ui_poll_scene_text_output(*renderer->poll_scene, &output))
        return false;
    if (d3d11_renderer_poll_voters_visible(*renderer) &&
        !ui_poll_voters_scene_text_output(*renderer->poll_voters_scene, &output))
        return false;
    boundaries->poll_end = emitter->count;
    if (d3d11_renderer_inbox_visible(*renderer) &&
        !ui_inbox_scene_text_output(*renderer->inbox_scene, theme, &output))
        return false;
    if (d3d11_renderer_stream_card_visible(*renderer) &&
        !ui_stream_card_text_output(*renderer->stream_card_scene, theme, &output))
        return false;
    if (d3d11_renderer_guild_card_visible(*renderer) &&
        !ui_guild_card_text_output(*renderer->guild_card_scene, theme, &output))
        return false;
    if (d3d11_renderer_expression_card_visible(*renderer) &&
        !ui_expression_card_text_output(*renderer->expression_card_scene, theme, &output))
        return false;
    if (d3d11_renderer_quick_switcher_visible(*renderer) &&
        !ui_quick_switcher_scene_text_output(*renderer->quick_switcher_scene, theme, &output,
                                             &renderer->media_picker_edit_geometry))
        return false;
    if (d3d11_renderer_shortcuts_visible(*renderer) &&
        !ui_shortcuts_scene_text_output(*renderer->shortcuts_scene, theme, &output))
        return false;
    if (d3d11_renderer_active(renderer->popup_scene) &&
        !popup_text_presentation_build_output(*renderer->popup_scene, theme, &output,
                                               renderer->message_measure_cache))
        return false;
    boundaries->popup_end = emitter->count;
    boundaries->settings_choice_start = boundaries->settings_choice_end = emitter->count;
    if (d3d11_renderer_active(renderer->reaction_viewer) &&
        !reaction_viewer_text_presentation_build_output(*renderer->reaction_viewer, theme, &output))
        return false;
    if (!settings && renderer->image_viewer &&
        !ui_image_viewer_text_output(*renderer->image_viewer, theme, &output))
        return false;
    boundaries->theme_editor_start = emitter->count;
    // Hosted video time glyphs follow their bar in the viewer's upper layer.
    if (!settings && d3d11_renderer_viewer_video_visible(*renderer) &&
        !ui_video_scene_text_output(*renderer->video_scene, theme, &output, renderer->image_viewer->owner))
        return false;

    if (renderer_theme_editor_visible(*renderer) &&
        !ui_theme_editor_text_output(*renderer->theme_editor_scene, theme, &output))
        return false;
    boundaries->theme_editor_end = emitter->count;
    boundaries->form_start = emitter->count;
    if (forum && !forum_post_text_output(*renderer->forum_form->scene, renderer->forum_form->geometry,
                                         theme, &output))
        return false;
    boundaries->forum_end = emitter->count;
    return (!form || text_form_text_output(*renderer->text_form->scene, renderer->text_form->geometry,
                                           theme, &output)) &&
           (!renderer_settings_page_visible(*renderer) ||
            ui_settings_scene_text_output(*renderer->settings_scene, theme, &output)) &&
           (!d3d11_renderer_tooltip_visible(*renderer) ||
            ui_tooltip_text_output(*renderer->tooltip_scene, theme, &output)) &&
           project_toast_text(renderer, theme, &output, boundaries);
}
struct TextRasterConsumer {
    D3D11Renderer* renderer;
    const Theme* theme;
    bool* atlas_invalidated;
    const TextOverlayBoundaries* boundaries;
    bool overlay;
    bool pending_boundary{}, settings_page_started{}, previous_settings_page{};
    u32 search_command_cursor{};
};
bool raster_text_command(void* context, const UiTextCommand& command, u32 ordinal) noexcept {
    auto& raster = *static_cast<TextRasterConsumer*>(context);
    auto* renderer = raster.renderer;
    bool boundary = false;
    if (raster.overlay) {
        const auto& bounds = *raster.boundaries;
        const bool settings = renderer_settings_page_visible(*renderer);
        const bool form = d3d11_renderer_form_active(*renderer) || d3d11_renderer_tooltip_visible(*renderer);
        const u32 batch = renderer->content_text->batch_count;
        boundary = ordinal == 0;
        for (const auto& entry : kOverlayBatchBoundaries) {
            if (ordinal == overlay_boundary(bounds, entry)) {
                overlay_batch(renderer, entry) = batch;
                boundary = true;
            }
        }
        if (ordinal == bounds.forum_end)
            renderer->forum_text_end_batch = batch;
        if (form || settings) {
            if (ordinal == bounds.form_start) {
                renderer->form_text_first_batch = batch;
                boundary = true;
            }
            boundary = boundary || ordinal == bounds.forum_end;
        }
    }
    const bool page = raster.overlay && renderer_settings_page_visible(*renderer) &&
                      ui_settings_scene_page_owner(*renderer->settings_scene, command.owner);
    boundary = boundary || page != raster.previous_settings_page;
    if (page && !raster.settings_page_started) {
        renderer->settings_page_text_first_batch = renderer->content_text->batch_count;
        raster.settings_page_started = true;
    }
    raster.previous_settings_page = page;
    raster.pending_boundary = raster.pending_boundary || boundary;
    const u32 previous_batches = renderer->content_text->batch_count;
    bool appended{};
    if (raster.overlay && ordinal >= raster.boundaries->search_rich_start &&
        ordinal < raster.boundaries->search_rich_end) {
        const auto* presentation = renderer->search_message_text;
        MessageTextCommand native{};
        UiTextCommand neutral{};
        bool found = false;
        while (presentation && raster.search_command_cursor < presentation->command_count) {
            const u32 index = raster.search_command_cursor++;
            if (!search_message_command(*renderer, index, &native, &neutral))
                continue;
            // The emitter validates every command but omits fully clipped labels.
            // Advance this source cursor over the same omitted commands.
            if (!ui_rect_intersects(neutral.rectangle, neutral.clip))
                continue;
            // The hashing and raster visitors walk the same immutable projection.
            if (neutral.text.data != command.text.data || neutral.text.bytes != command.text.bytes ||
                neutral.owner.manager != command.owner.manager ||
                neutral.owner.handle.slot != command.owner.handle.slot ||
                !ui_rect_equal(neutral.clip, command.clip))
                return false;
            appended = append_message_text_command(renderer, presentation, native, index, *raster.theme,
                                                   raster.atlas_invalidated, true, raster.pending_boundary);
            found = true;
            break;
        }
        if (!found)
            return false;
    } else {
        const bool selected_audio_text = raster.overlay && ordinal >= raster.boundaries->audio_start &&
                                         ordinal < raster.boundaries->audio_end;
        const bool selected_poll_text = raster.overlay && ordinal >= raster.boundaries->poll_start &&
                                        ordinal < raster.boundaries->poll_end;
        appended = ((!selected_audio_text && d3d11_renderer_audio_owner(*renderer, command.owner)) ||
                    (!selected_poll_text && d3d11_renderer_poll_owner(*renderer, command.owner)))
                       ? true
                       : append_content_text_command(renderer, command, *raster.theme,
                                                     raster.atlas_invalidated, raster.pending_boundary);
    }
    // Culled or zero-glyph commands must not consume a layer boundary: the
    // next emitted batch still belongs wholly to the new layer.
    if (renderer->content_text->batch_count != previous_batches)
        raster.pending_boundary = false;
    if (page)
        renderer->settings_page_text_end_batch = renderer->content_text->batch_count;
    return appended && !*raster.atlas_invalidated;
}

u32 float_word(float value) noexcept {
    union {
        float number;
        u32 word;
    } bits{value};
    return bits.word;
}

// The widget manager state the shell text depends on.
__declspec(noinline) u64 mix_widget_state(u64 hash, const UiWidgetManager& manager) noexcept {
    hash = hash64_mix(hash, manager.hovered.slot);
    hash = hash64_mix(hash, manager.hovered.generation);
    hash = hash64_mix(hash, manager.hover_started_ms);
    hash = hash64_mix(hash, manager.focused.slot);
    hash = hash64_mix(hash, manager.focused.generation);
    hash = hash64_mix(hash, manager.count);
    return hash64_mix(hash, manager.generation);
}

// Revisions and positions of every scene and store the shell projection
// reads. The host only offers a scroll_hint when no account source changed;
// this stamp still catches pointer, layout and composer changes in between.
u64 shell_scene_stamp(const D3D11Renderer& renderer) noexcept {
    u64 hash = kHash64Seed;
    const auto mix = [&hash](u64 value) noexcept { hash = hash64_mix(hash, value); };
    const auto widgets = [&hash](const UiWidgetManager& manager) noexcept { hash = mix_widget_state(hash, manager); };
    const auto scroll = [&mix](float value) noexcept { mix(float_word(value)); };
    if (const auto* scene = renderer.ui_scene) {
        mix(reinterpret_cast<usize>(scene));
        mix(scene->layout_passes);
        mix(scene->layout_rebuilds);
        mix(scene->layout_metrics_revision);
        mix(scene->generation);
        mix(static_cast<u8>(scene->view));
        mix(scene->guild_menu_expanded);
        widgets(scene->widgets);
    }
    if (const auto* scene = renderer.shell_navigation_scene) {
        mix(scene->rebuilds);
        mix(scene->layout_passes);
        mix(scene->source_revision);
        mix(scene->first);
        mix(scene->count);
        scroll(scene->scroll_dips);
        widgets(scene->widgets);
    }
    if (const auto* scene = renderer.member_list_scene) {
        mix(scene->rebuilds);
        mix(scene->layout_passes);
        mix(scene->source_revision);
        mix(scene->first);
        mix(scene->count);
        mix(scene->active);
        scroll(scene->scroll_dips);
        widgets(scene->widgets);
    }
    if (const auto* scene = renderer.guild_rail_scene) {
        mix(scene->rebuilds);
        mix(scene->layout_passes);
        mix(scene->data_updates);
        mix(scene->source_revision);
        mix(scene->first);
        mix(scene->count);
        mix(scene->active);
        scroll(scene->scroll_dips);
        widgets(scene->widgets);
    }
    if (const auto* composer = renderer.message_composer_scene) {
        mix(composer->active);
        mix(composer->display_signature);
        mix(composer->observed_revision);
        mix(composer->composition_revision);
        mix(composer->observed_geometry_revision);
        mix(composer->display_anchor);
        mix(composer->display_caret);
        widgets(composer->widgets);
    }
    const auto& layout = renderer.composer_layout;
    scroll(layout.origin_x);
    scroll(layout.origin_y);
    scroll(layout.width);
    scroll(layout.layout_height);
    if (const auto* state = renderer.chat_state) {
        mix(state->guild_mutations);
        mix(state->channel_mutations);
        mix(state->permission_mutations);
        mix(state->activity_mutations);
        mix(state->emoji_mutations);
        mix(state->compactions);
        mix(state->name_used);
        mix(state->selection_revision);
        mix(state->selected_guild_id);
        mix(state->selected_channel_id);
        mix(state->restore_channel_id);
        mix(state->home_selected);
        mix(state->private_channels ? state->private_channels->revision : 0u);
    }
    if (renderer.navigation_view)
        mix(renderer.navigation_view->revision);
    if (const auto* users = renderer.user_store) {
        mix(users->revision);
        mix(users->appearance.revision);
    }
    mix(renderer.thread_header ? renderer.thread_header->revision : 0u);
    // The DM profile panel's text is shell text: showing, hiding, laying out
    // or scrolling it must not keep a retained shell.
    mix(reinterpret_cast<usize>(renderer.dm_profile_panel));
    if (const auto* panel = renderer.dm_profile_panel) {
        mix(panel->active);
        mix(panel->revision);
        scroll(panel->scroll);
        widgets(panel->widgets);
    }
    return hash;
}

// Scroll positions of the shell's text lists (members, channels). Any change
// moves shell glyphs; the server rail is left out, its rows carry little text.
u64 shell_list_scroll(u64 hash, bool active, u32 first, u32 count, float scroll) noexcept {
    if (!active || !count)
        return hash;
    hash = hash64_mix(hash, (static_cast<u64>(first) << 32) | count);
    return hash64_mix(hash, float_word(scroll));
}
u64 shell_navigation_scroll_key(const D3D11Renderer& renderer) noexcept {
    const auto* scene = renderer.shell_navigation_scene;
    return scene ? shell_list_scroll(kHash64Seed, scene->active, scene->first, scene->count,
                                     scene->scroll_dips)
                 : kHash64Seed;
}
u64 shell_scroll_key(const D3D11Renderer& renderer) noexcept {
    u64 hash = kHash64Seed;
    if (const auto* scene = renderer.member_list_scene)
        hash = shell_list_scroll(hash, scene->active, scene->first, scene->count, scene->scroll_dips);
    if (const auto* scene = renderer.shell_navigation_scene)
        hash = shell_list_scroll(hash, scene->active, scene->first, scene->count, scene->scroll_dips);
    return hash | 1;
}
// The static shell (everything before the member rows) as one build left it.
RendererTextShellStatic shell_static_marks(const D3D11Renderer& renderer, const UiTextEmitter& emitter, u64 key,
                                    u32 serial) noexcept {
    const RendererTextStorage& storage = *renderer.content_text;
    return {emitter.signature,
            key,
            emitter.count,
            storage.quad_count,
            storage.batch_count,
            storage.content_semantic_count,
            storage.content_hit_region_count,
            renderer.text_effect_count,
            renderer.inline_media.count,
            storage.batch_count ? storage.batches[storage.batch_count - 1].quad_count : 0u,
            serial,
            renderer.text_effect_animated,
            true};
}

// While a scroll runs, message rows within this distance of the viewport are
// rastered so its steps can move their glyphs; moving further rasters again.
// Other builds raster only the viewport.
constexpr float kMessageTextRasterMarginDips = 256.0f;

// Scroll steps move the projection's commands without rebuilding it. Move the
// glyphs rastered from them by the same whole device pixels, or drop them so
// the build rasters the moved commands.
void sync_message_scroll(D3D11Renderer* renderer) noexcept {
    const MessageTextPresentation& presentation = *renderer->message_text;
    RendererTextStorage* storage = renderer->content_text;
    auto& prefix = renderer->content_text_prefix;
    if (prefix.storage != storage || prefix.build_serial != storage->rebuilds)
        return;
    const float scale = static_cast<float>(renderer->dpi) / 96.0f;
    const float moved = presentation.scroll_shift_dips - renderer->text_message_shift_base;
    const bool same_build = presentation.builds == renderer->text_message_builds;
    // Glyphs rastered from an earlier build that a later, identical build kept
    // still sit where that build's commands were; once either moved, raster.
    if (moved == 0.0f && renderer->text_message_shift_px == 0)
        return;
    const i32 target = static_cast<i32>(moved * scale + (moved < 0 ? -0.5f : 0.5f));
    if (same_build && presentation.scroll_moving && target == renderer->text_message_shift_px)
        return;
    const auto& shell = prefix.shell;
    // A settled scroll rasters at its exact position: whole-pixel moves round
    // each line's baseline differently than rastering there would.
    if (!same_build || !presentation.scroll_moving || !renderer->text_message_shiftable ||
        shell.storage != storage || shell.build_serial != storage->rebuilds ||
        !(moved <= kMessageTextRasterMarginDips) || !(moved >= -kMessageTextRasterMarginDips)) {
        // The build below rasters the messages again over the retained shell.
        prefix.storage = nullptr;
        renderer->content_text_retention.key.message_signature = ~presentation.projection_signature;
        renderer->text_message_shift_px = 0;
        return;
    }
    const float shift = static_cast<float>(target - renderer->text_message_shift_px) / scale;
    for (u32 i = shell.quads; i < prefix.quads; ++i) {
        storage->quads[i].top += shift;
        storage->quads[i].bottom += shift;
    }
    for (u32 i = shell.hits; i < prefix.hits; ++i) {
        storage->content_hit_regions[i].top += shift;
        storage->content_hit_regions[i].bottom += shift;
    }
    for (u32 i = shell.inline_flags; i < prefix.inline_flags; ++i) {
        RendererInlineFlag& flag = renderer->inline_media.flags[i];
        flag.rectangle.top += shift;
        flag.rectangle.bottom += shift;
        flag.clip.top += shift;
        flag.clip.bottom += shift;
    }
    renderer->text_message_shift_px = target;
    // A new stream: the shell's glyphs stay where the glyph ring holds them,
    // the moved messages are written again.
    prefix.build_serial = prefix.shell.build_serial = ++storage->rebuilds;
    renderer->text_message_serial = storage->rebuilds;
    renderer->text_stable_quads = shell.quads;
    renderer->text_stable_serial = renderer->text_shell_serial;
}

bool build_content_text_once(D3D11Renderer* renderer, const Theme& theme, bool* atlas_invalidated,
                             bool* storage_modified, bool* prefix_reused, bool include_messages) noexcept {
    const bool login_form = d3d11_renderer_login_form_text(*renderer);
    const bool settings_active = !login_form && renderer_settings_page_visible(*renderer);
    const bool friends_active = !login_form && !settings_active && renderer->friends_scene &&
                                renderer->friends_scene->active;
    const bool browser_active = !login_form && d3d11_renderer_thread_browser_visible(*renderer);
    include_messages = include_messages && !login_form && !settings_active && !friends_active &&
                       !browser_active && d3d11_renderer_active(renderer->message_scene);
    RendererTextStorage* storage = renderer->content_text;
    const MessageTextPresentation* presentation = renderer->message_text;
    if (include_messages && (!presentation || !presentation->active ||
                             presentation->command_count > presentation->commands.count ||
                             presentation->measurement_count > presentation->measurements.count)) {
        return false;
    }
    *atlas_invalidated = false;
    *storage_modified = false;
    *prefix_reused = false;
    storage->last_error = RendererTextBuildError::layout;
    storage->last_shape_error = TextShapeError::none;
    storage->native_error = S_OK;
    if (!login_form && !settings_active &&
        (!renderer->ui_scene || renderer->ui_scene->count == 0 || !renderer->shell_navigation_scene)) {
        return false;
    }
    UiTextEmitter shell{}, overlay{};
    OverlayReservation reservation{storage->quad_capacity, 0};
    TextOverlayBoundaries boundaries{};
    // A scroll-only rebuild over unchanged shell scenes keeps the shell's
    // projection (and, below, its glyphs) from the build that made them.
    auto& retained_shell = renderer->content_text_prefix.shell;
    const u64 scene_stamp = login_form || settings_active ? 0 : shell_scene_stamp(*renderer);
    bool shell_retained = retained_shell.scroll_hint && !login_form && !settings_active && !friends_active &&
                          !browser_active && retained_shell.storage == storage &&
                          retained_shell.build_serial == storage->rebuilds &&
                          retained_shell.retention_generation == renderer->content_text_retention.generation &&
                          retained_shell.scene_stamp == scene_stamp;
    // A shell list (members, channels, servers) scrolled since the shell was
    // rastered: its glyphs moved, so a signature-only projection could only
    // confirm the miss. Project it once, rastering as it goes.
    const u64 shell_scroll = login_form || settings_active ? 0 : shell_scroll_key(*renderer);
    const u64 navigation_scroll =
        login_form || settings_active ? 0 : shell_navigation_scroll_key(*renderer);
    const bool shell_moved = !shell_retained && !login_form && !settings_active && !friends_active &&
                             !browser_active && retained_shell.storage == storage &&
                             retained_shell.build_serial == storage->rebuilds &&
                             renderer->text_shell_scroll_key && renderer->text_shell_scroll_key != shell_scroll;
    if (!ui_text_emitter_initialize(&shell) ||
        !ui_text_emitter_initialize(&overlay, reserve_overlay_text, &reservation))
        return false;
    // The projection's static part (signature and count before the member
    // rows), when this build knows it.
    u64 static_signature = 0;
    u32 static_count = 0;
    bool static_known = false;
    const auto project_static = [&]() noexcept {
        if (!project_shell_static_text(renderer, theme, &shell))
            return false;
        static_signature = shell.signature;
        static_count = shell.count;
        static_known = true;
        return true;
    };
    if (shell_retained) {
        shell.signature = retained_shell.projection_signature;
        shell.count = retained_shell.projection_count;
        static_signature = retained_shell.static_marks.signature;
        static_count = retained_shell.static_marks.count;
        static_known = retained_shell.static_marks.valid;
    } else if (shell_moved) {
        // Only the member list scrolled: confirm the static shell by its
        // signature so its glyphs can stay; the rows are rastered below.
        if (renderer->text_shell_navigation_scroll_key == navigation_scroll && !project_static())
            return false;
    } else if (!project_static() || !project_shell_member_text(renderer, theme, &shell)) {
        return false;
    }
    if (!project_overlay_text(renderer, theme, &overlay, &boundaries))
        return false;
    ContentTextRetentionKey retention_key =
        content_text_retention_key(*renderer, theme, shell.signature, overlay.signature, include_messages);
    if (shell_retained && (retention_key.shell_signature != retained_shell.signature ||
                           retention_key.theme_signature != retained_shell.theme_signature ||
                           retention_key.dpi != retained_shell.dpi ||
                           retention_key.atlas_evicted_entries != retained_shell.atlas_evicted_entries ||
                           retention_key.atlas_full_resets != retained_shell.atlas_full_resets)) {
        // Glyph inputs beyond the scenes moved (appearance, theme, atlas):
        // project the shell for real.
        shell_retained = false;
        if (!ui_text_emitter_initialize(&shell) || !project_static() ||
            !project_shell_member_text(renderer, theme, &shell))
            return false;
        retention_key =
            content_text_retention_key(*renderer, theme, shell.signature, overlay.signature, include_messages);
    }
    if (!shell_moved && content_text_retention_probe(&renderer->content_text_retention, retention_key)) {
        d3d11_renderer_commit_interaction_text_state(renderer);
        storage->last_error = RendererTextBuildError::none;
        return true;
    }

    auto& prefix = renderer->content_text_prefix;
    // Nothing outside this build invalidated the stream since the shell
    // prefix was committed (bindings, theme consumers, reduced motion).
    const bool shell_generation_current =
        prefix.shell.retention_generation == renderer->content_text_retention.generation;
    const auto* messages = renderer->message_scene;
    const bool reuse =
        !shell_moved && include_messages && messages && prefix.storage == storage &&
        prefix.build_serial == storage->rebuilds && prefix.message_root.manager == &messages->widgets &&
        prefix.message_root.handle.slot == messages->root.slot &&
        prefix.message_root.handle.generation == messages->root.generation &&
        ui_widget_manager_resolve(messages->widgets, messages->root) &&
        prefix.quads <= storage->quad_count && prefix.batches <= storage->batch_count &&
        prefix.semantics <= storage->content_semantic_count &&
        prefix.hits <= storage->content_hit_region_count && prefix.effects <= renderer->text_effect_count &&
        prefix.inline_flags <= renderer->inline_media.count &&
        content_text_retention_base_current(renderer->content_text_retention, retention_key);

    // The last complete stream stays live until presentation is known to be
    // valid, so transient clipped geometry cannot blank every glyph during a
    // live resize.
    TextRasterScope raster_scope(*renderer);
    *storage_modified = true;
    // Any failure after a suffix mutation must retry from a full build; the old
    // key cannot describe partially rewritten overlay tails.
    content_text_retention_invalidate(&renderer->content_text_retention);
    *prefix_reused = reuse;
    TextRasterConsumer raster{renderer, &theme, atlas_invalidated, &boundaries, false};
    if (reuse) {
        storage->quad_count = prefix.quads;
        storage->batch_count = prefix.batches;
        storage->content_semantic_count = prefix.semantics;
        storage->content_hit_region_count = prefix.hits;
        renderer->inline_media.count = prefix.inline_flags;
        renderer->text_effect_count = prefix.effects;
        renderer->text_effect_animated = prefix.animated;
        // Shell and message glyphs stay as the message build wrote them.
        renderer->text_stable_quads = prefix.quads;
        renderer->text_stable_serial = renderer->text_message_serial;
    } else {
        prefix.storage = nullptr;
        // Messages changed (a scroll, a new message) under an unchanged shell:
        // keep the shell's glyphs instead of projecting and rastering it again.
        auto& shell_prefix = prefix.shell;
        // Glyph inputs beyond the projection (composer styles and selection,
        // appearance and thread header revisions, a hovered name effect) key
        // the static part exactly as they key the whole shell.
        const u64 static_key =
            static_known
                ? content_text_retention_key(*renderer, theme, static_signature, 0, false).shell_signature
                : 0;
        const RendererTextShellStatic previous_static = shell_prefix.static_marks;
        RendererTextShellStatic static_marks = previous_static;
        const bool shell_storage_current =
            shell_prefix.storage == storage && shell_prefix.build_serial == storage->rebuilds;
        const bool shell_reuse =
            !shell_moved && shell_generation_current && shell_storage_current &&
            shell_prefix.signature == retention_key.shell_signature &&
            shell_prefix.theme_signature == retention_key.theme_signature && shell_prefix.dpi == retention_key.dpi &&
            shell_prefix.atlas_evicted_entries == retention_key.atlas_evicted_entries &&
            shell_prefix.atlas_full_resets == retention_key.atlas_full_resets &&
            shell_prefix.quads <= storage->quad_capacity && shell_prefix.batches <= storage->batch_capacity &&
            shell_prefix.semantics <= storage->content_semantic_capacity &&
            shell_prefix.hits <= storage->content_hit_region_capacity;
        shell_prefix.storage = nullptr;
        if (shell_reuse) {
            storage->quad_count = shell_prefix.quads;
            storage->batch_count = shell_prefix.batches;
            storage->content_semantic_count = shell_prefix.semantics;
            storage->content_hit_region_count = shell_prefix.hits;
            renderer->inline_media.count = shell_prefix.inline_flags;
            renderer->text_effect_count = shell_prefix.effects;
            renderer->text_effect_animated = shell_prefix.animated;
            saturating_increment(shell_prefix.reuses);
            renderer->text_stable_quads = shell_prefix.quads;
            renderer->text_stable_serial = renderer->text_shell_serial;
        } else {
            // Only the member rows changed: the static shell's glyphs stay
            // and the rows are rastered after them.
            const bool static_reuse =
                static_known && previous_static.valid && shell_generation_current &&
                previous_static.signature == static_signature && previous_static.count == static_count &&
                previous_static.key == static_key && shell_storage_current &&
                shell_prefix.theme_signature == retention_key.theme_signature &&
                shell_prefix.dpi == retention_key.dpi &&
                shell_prefix.atlas_evicted_entries == retention_key.atlas_evicted_entries &&
                shell_prefix.atlas_full_resets == retention_key.atlas_full_resets &&
                previous_static.quads <= storage->quad_capacity &&
                previous_static.batches <= storage->batch_capacity &&
                previous_static.semantics <= storage->content_semantic_capacity &&
                previous_static.hits <= storage->content_hit_region_capacity;
            UiTextEmitter shell_raster{};
            if (!ui_text_emitter_initialize(&shell_raster, raster_text_command, &raster))
                return false;
            if (static_reuse) {
                storage->quad_count = previous_static.quads;
                storage->batch_count = previous_static.batches;
                storage->content_semantic_count = previous_static.semantics;
                storage->content_hit_region_count = previous_static.hits;
                // A row batch sharing the last static clip was merged into it.
                if (previous_static.batches)
                    storage->batches[previous_static.batches - 1].quad_count =
                        previous_static.last_batch_quads;
                renderer->inline_media.count = previous_static.inline_flags;
                renderer->text_effect_count = previous_static.effects;
                renderer->text_effect_animated = previous_static.animated;
                shell_raster.signature = previous_static.signature;
                shell_raster.count = previous_static.count;
            } else {
                renderer->text_effect_count = 0;
                renderer->text_effect_animated = false;
                storage->quad_count = 0;
                storage->batch_count = 0;
                storage->content_semantic_count = 0;
                storage->content_hit_region_count = 0;
                renderer->inline_media.count = 0;
                if (!project_shell_static_text(renderer, theme, &shell_raster) ||
                    (static_known &&
                     (shell_raster.signature != static_signature || shell_raster.count != static_count)))
                    return false;
                // This build's serial is rebuilds + 1.
                static_marks = shell_static_marks(
                    *renderer, shell_raster,
                    static_known
                        ? static_key
                        : content_text_retention_key(*renderer, theme, shell_raster.signature, 0, false)
                              .shell_signature,
                    storage->rebuilds + 1);
            }
            if (!project_shell_member_text(renderer, theme, &shell_raster) ||
                (!shell_moved &&
                 (shell_raster.signature != shell.signature || shell_raster.count != shell.count)))
                return false;
            if (shell_moved) {
                shell.signature = shell_raster.signature;
                shell.count = shell_raster.count;
                retention_key = content_text_retention_key(*renderer, theme, shell.signature, overlay.signature,
                                                           include_messages);
            }
            renderer->text_shell_scroll_key = shell_scroll;
            renderer->text_shell_navigation_scroll_key = navigation_scroll;
            renderer->text_shell_serial = storage->rebuilds + 1;
            renderer->text_stable_quads = static_reuse ? static_marks.quads : storage->quad_count;
            renderer->text_stable_serial = static_reuse ? static_marks.serial : renderer->text_shell_serial;
        }
        shell_prefix = RendererTextShellPrefix{storage,
                                               retention_key.shell_signature,
                                               retention_key.theme_signature,
                                               shell.signature,
                                               scene_stamp,
                                               shell.count,
                                               storage->quad_count,
                                               storage->batch_count,
                                               storage->content_semantic_count,
                                               storage->content_hit_region_count,
                                               renderer->text_effect_count,
                                               renderer->inline_media.count,
                                               retention_key.dpi,
                                               retention_key.atlas_evicted_entries,
                                               retention_key.atlas_full_resets,
                                               ~0u, // committed with the build below
                                               0,
                                               shell_prefix.reuses,
                                               renderer->text_effect_animated,
                                               shell_prefix.scroll_hint};
        shell_prefix.static_marks = static_marks;
        const u32 full_quad_capacity = storage->quad_capacity;
        const u32 full_batch_capacity = storage->batch_capacity;
        const u32 popup_quad_reserve = reservation.quads;
        if (popup_quad_reserve < full_quad_capacity - storage->quad_count) {
            storage->quad_capacity = maximum(storage->quad_count, full_quad_capacity - popup_quad_reserve);
        } else {
            storage->quad_capacity = storage->quad_count;
        }
        if (reservation.batches < full_batch_capacity - storage->batch_count) {
            storage->batch_capacity = maximum(storage->batch_count, full_batch_capacity - reservation.batches);
        } else {
            storage->batch_capacity = storage->batch_count;
        }
        if (include_messages) {
            // Text projection also contains offscreen overscan commands so
            // their real heights are known before scrolling reveals them. Only
            // those visible, or near the viewport while scrolling, are shaped
            // and uploaded.
            const UiRect viewport = renderer->message_scene ? renderer->message_scene->viewport : UiRect{};
            const float margin = presentation->scroll_moving ? kMessageTextRasterMarginDips : 0.0f;
            const UiRect band{viewport.left, viewport.top - margin, viewport.right, viewport.bottom + margin};
            const u32 first_batch = storage->batch_count;
            const u32 effects = renderer->text_effect_count;
            for (u32 index = 0; index < presentation->command_count; ++index) {
                const MessageTextCommand& command = presentation->commands[index];
                if (renderer->message_scene && !ui_rect_intersects(command.clip, band))
                    continue;
                if (!append_message_text_command(renderer, presentation, command, index, theme,
                                                 atlas_invalidated) ||
                    *atlas_invalidated) {
                    storage->quad_capacity = full_quad_capacity;
                    storage->batch_capacity = full_batch_capacity;
                    return false;
                }
            }
            // Row clips extend past the viewport; it bounds every message batch.
            bool viewport_only = effects == renderer->text_effect_count;
            for (u32 index = first_batch; renderer->message_scene && index < storage->batch_count; ++index) {
                UiRect& clip = storage->batches[index].clip;
                viewport_only = viewport_only && ui_rect_equal(clip, viewport);
                clip = ui_rect_intersection(clip, viewport);
            }
            renderer->text_message_builds = presentation->builds;
            renderer->text_message_shift_base = presentation->scroll_shift_dips;
            renderer->text_message_shift_px = 0;
            renderer->text_message_shiftable = viewport_only && presentation->scroll_moving;
        }
        storage->quad_capacity = full_quad_capacity;
        storage->batch_capacity = full_batch_capacity;
        renderer->text_message_serial = storage->rebuilds + 1;
        if (include_messages && messages && ui_widget_manager_resolve(messages->widgets, messages->root)) {
            prefix.storage = storage;
            prefix.message_root = {&messages->widgets, messages->root};
            prefix.quads = storage->quad_count;
            prefix.batches = storage->batch_count;
            prefix.semantics = storage->content_semantic_count;
            prefix.hits = storage->content_hit_region_count;
            prefix.effects = renderer->text_effect_count;
            prefix.inline_flags = renderer->inline_media.count;
            prefix.animated = renderer->text_effect_animated;
        }
    }
    renderer->overlay_text_first_batch = storage->batch_count;
    renderer->form_text_first_batch = ~0u;
    renderer->menu_text_first_batch = ~0u;
    renderer->toast_text_first_batch = ~0u;
    renderer->friends_requests_text_end_batch = storage->batch_count;
    renderer->media_picker_text_first_batch = renderer->media_picker_text_end_batch = storage->batch_count;
    reset_motion_text_ranges(renderer, storage->batch_count);
    raster.overlay = true;
    UiTextEmitter overlay_raster{};
    TextOverlayBoundaries raster_boundaries{};
    if (!ui_text_emitter_initialize(&overlay_raster, raster_text_command, &raster) ||
        !project_overlay_text(renderer, theme, &overlay_raster, &raster_boundaries) ||
        overlay_raster.signature != overlay.signature || overlay_raster.count != overlay.count ||
        !bytes_equal(&raster_boundaries, &boundaries, kComparedOverlayBoundaryBytes))
        return false;
    for (u32 index = 0; index < kOverlayEndBoundaryCount; ++index) {
        const auto& entry = kOverlayBatchBoundaries[index];
        if (overlay_boundary(boundaries, entry) == overlay.count)
            overlay_batch(renderer, entry) = storage->batch_count;
    }
    // A boundary at end-of-stream has no command callback. Publish its batch
    // endpoint, including settings without an active popup.
    if (boundaries.form_start == overlay.count)
        renderer->form_text_first_batch = storage->batch_count;
    if (boundaries.forum_end == overlay.count)
        renderer->forum_text_end_batch = storage->batch_count;
    if (prefix.storage)
        prefix.build_serial = storage->rebuilds + 1;
    if (prefix.shell.storage) {
        prefix.shell.build_serial = storage->rebuilds + 1;
        // An overlay-only rebuild keeps the shell; its scenes may have moved
        // without changing a glyph (a hover with no text effect).
        prefix.shell.scene_stamp = scene_stamp;
    }
    if (reuse)
        saturating_increment(prefix.overlay_builds);
    else
        saturating_increment(prefix.base_builds);
    storage->last_error = RendererTextBuildError::none;
    content_text_retention_commit(&renderer->content_text_retention, retention_key);
    prefix.shell.retention_generation = renderer->content_text_retention.generation;
    d3d11_renderer_commit_interaction_text_state(renderer);
    return true;
}

void discard_content_text(D3D11Renderer* renderer) noexcept {
    renderer->content_text->quad_count = 0;
    renderer->content_text->batch_count = 0;
    renderer->content_text->content_semantic_count = 0;
    renderer->content_text->content_hit_region_count = 0;
    renderer->text_effect_count = 0;
    renderer->text_effect_animated = false;
    renderer->content_text_prefix.storage = nullptr;
    renderer->content_text_prefix.shell.storage = nullptr;
    renderer->overlay_text_first_batch = 0;
    renderer->text_stable_quads = 0;
    reset_motion_text_ranges(renderer, 0);
    content_text_retention_invalidate(&renderer->content_text_retention);
}

void discard_retained_overlay_text(D3D11Renderer* renderer) noexcept {
    RendererTextStorage* storage = renderer->content_text;
    if (renderer->overlay_text_first_batch > storage->batch_count) {
        return;
    }
    storage->batch_count = renderer->overlay_text_first_batch;
    storage->quad_count = storage->batch_count == 0
                              ? 0
                              : storage->batches[storage->batch_count - 1].first_quad +
                                    storage->batches[storage->batch_count - 1].quad_count;
    const auto& prefix = renderer->content_text_prefix;
    if (prefix.storage == storage && prefix.batches == storage->batch_count &&
        prefix.quads == storage->quad_count && prefix.semantics <= storage->content_semantic_count &&
        prefix.hits <= storage->content_hit_region_count) {
        storage->content_semantic_count = prefix.semantics;
        storage->content_hit_region_count = prefix.hits;
        renderer->inline_media.count = prefix.inline_flags;
        renderer->text_effect_count = prefix.effects;
        renderer->text_effect_animated = prefix.animated;
    } else {
        renderer->inline_media.count = 0;
    }
    renderer->overlay_text_first_batch = storage->batch_count;
    renderer->form_text_first_batch = ~0u;
    reset_motion_text_ranges(renderer, storage->batch_count);
    content_text_retention_invalidate(&renderer->content_text_retention);
}

} // namespace

namespace {
// The fields holding the committed revision of each interaction text source,
// in the order interaction_text_source_values computes them.
constexpr u32 kInteractionTextSourceOffsets[]{
    offsetof(D3D11Renderer, interaction_text_forum_form_revision),
    offsetof(D3D11Renderer, interaction_text_theme_editor_revision),
    offsetof(D3D11Renderer, interaction_text_settings_revision),
    offsetof(D3D11Renderer, interaction_text_active_mask),
    offsetof(D3D11Renderer, interaction_text_thread_browser_revision),
    offsetof(D3D11Renderer, interaction_text_thread_browser_publication),
    offsetof(D3D11Renderer, interaction_text_video_revision),
    offsetof(D3D11Renderer, interaction_text_audio_revision),
    offsetof(D3D11Renderer, interaction_text_poll_revision),
    offsetof(D3D11Renderer, interaction_text_poll_voters_revision),
    offsetof(D3D11Renderer, interaction_text_tooltip_revision),
    offsetof(D3D11Renderer, interaction_text_unread_banner_revision),
    offsetof(D3D11Renderer, interaction_text_toast_revision),
    offsetof(D3D11Renderer, interaction_text_search_revision),
    offsetof(D3D11Renderer, interaction_text_pinned_messages_revision),
    offsetof(D3D11Renderer, interaction_text_quick_switcher_revision),
    offsetof(D3D11Renderer, interaction_text_inbox_revision),
    offsetof(D3D11Renderer, interaction_text_message_forward_revision),
    offsetof(D3D11Renderer, interaction_text_download_revision),
    offsetof(D3D11Renderer, interaction_text_shortcuts_revision),
    offsetof(D3D11Renderer, interaction_text_call_revision),
    offsetof(D3D11Renderer, interaction_text_stream_card_revision),
    offsetof(D3D11Renderer, interaction_text_guild_card_revision),
    offsetof(D3D11Renderer, interaction_text_media_picker_revision),
    offsetof(D3D11Renderer, interaction_text_expression_card_revision),
    offsetof(D3D11Renderer, interaction_text_friends_revision),
    offsetof(D3D11Renderer, interaction_text_friends_requests_revision),
    offsetof(D3D11Renderer, interaction_text_profile_revision),
    offsetof(D3D11Renderer, interaction_text_image_viewer_revision),
    offsetof(D3D11Renderer, interaction_text_action_bar_revision),
    offsetof(D3D11Renderer, interaction_text_popup_revision),
    offsetof(D3D11Renderer, interaction_text_reaction_viewer_revision),
    offsetof(D3D11Renderer, interaction_text_form_revision),
};
constexpr u32 kInteractionTextSourceCount = sizeof(kInteractionTextSourceOffsets) / sizeof(u32);
// Sources 0 to 24 apply on every page; 25 to 32 are friends, friends requests, profile, image viewer,
// action bar, popup, reaction viewer and form.
constexpr u32 kInteractionTextProfileSource = 27, kInteractionTextActionBarSource = 29,
              kInteractionTextPopupSource = 30, kInteractionTextFormSource = 32;
constexpr u64 kInteractionTextStaticSources = (1ull << 25) - 1,
              kInteractionTextAllSources = (1ull << kInteractionTextSourceCount) - 1;
// Sources 5 to 9 are 64-bit fields; the rest are 32-bit.
constexpr u32 kInteractionTextWideSources = 0x3E0u;
static_assert(sizeof(D3D11Renderer::interaction_text_thread_browser_publication) == sizeof(u64) &&
              sizeof(D3D11Renderer::interaction_text_video_revision) == sizeof(u64) &&
              sizeof(D3D11Renderer::interaction_text_audio_revision) == sizeof(u64) &&
              sizeof(D3D11Renderer::interaction_text_poll_revision) == sizeof(u64) &&
              sizeof(D3D11Renderer::interaction_text_poll_voters_revision) == sizeof(u64) &&
              sizeof(D3D11Renderer::interaction_text_media_picker_revision) == sizeof(u32));

// Fills the revision of every interaction text source and returns the mask of the sources that
// apply: the settings page tracks only the popup, profile overlay and form of the page-dependent ones.
u64 interaction_text_source_values(const D3D11Renderer& renderer, u64* values) noexcept {
    const bool thread_browser = d3d11_renderer_thread_browser_visible(renderer);
    values[0] = renderer.forum_form && renderer.forum_form->scene ? renderer.forum_form->scene->revision : 0;
    values[1] = renderer_theme_editor_visible(renderer) ? renderer.theme_editor_scene->revision : 0;
    values[2] = renderer.settings_scene ? renderer.settings_scene->revision : 0;
    values[3] = d3d11_renderer_interaction_text_active_mask(renderer);
    values[4] = thread_browser ? renderer.thread_browser_scene->revision : 0;
    values[5] = thread_browser ? renderer.thread_browser_scene->source_publication : 0;
    values[6] = d3d11_renderer_video_text_revision(renderer);
    values[7] = d3d11_renderer_audio_text_revision(renderer);
    values[8] = d3d11_renderer_poll_text_revision(renderer);
    values[9] = d3d11_renderer_poll_voters_text_revision(renderer);
    values[10] = d3d11_renderer_tooltip_revision(renderer);
    values[11] = d3d11_renderer_unread_banner_revision(renderer);
    values[12] = d3d11_renderer_toast_revision(renderer);
    values[13] = d3d11_renderer_search_visible(renderer) ? renderer.search_scene->revision : 0;
    values[14] = d3d11_renderer_pinned_messages_visible(renderer) ? renderer.pinned_messages_scene->revision : 0;
    values[15] = d3d11_renderer_quick_switcher_visible(renderer) ? renderer.quick_switcher_scene->revision : 0;
    values[16] = d3d11_renderer_inbox_visible(renderer) ? renderer.inbox_scene->revision : 0;
    values[17] = d3d11_renderer_message_forward_visible(renderer) ? renderer.message_forward_scene->revision : 0;
    values[18] = d3d11_renderer_download_visible(renderer) ? renderer.download_scene->revision : 0;
    values[19] = d3d11_renderer_shortcuts_visible(renderer) ? renderer.shortcuts_scene->revision : 0;
    values[20] = d3d11_renderer_call_visible(renderer) ? renderer.call_scene->revision : 0;
    values[21] = d3d11_renderer_stream_card_visible(renderer) ? renderer.stream_card_scene->revision : 0;
    values[22] = d3d11_renderer_guild_card_visible(renderer) ? renderer.guild_card_scene->revision : 0;
    values[23] = d3d11_renderer_media_picker_visible(renderer) ? renderer.media_picker_scene->revision : 0;
    values[24] = d3d11_renderer_expression_card_visible(renderer) ? renderer.expression_card_scene->revision : 0;
    values[kInteractionTextFormSource] =
        renderer.text_form && renderer.text_form->scene ? renderer.text_form->scene->revision : 0;
    if (renderer_settings_page_visible(renderer)) {
        const auto* popup = d3d11_renderer_settings_popup(renderer);
        values[kInteractionTextPopupSource] = popup ? popup->revision : 0;
        values[kInteractionTextProfileSource] = d3d11_renderer_settings_overlay_revision(renderer);
        return kInteractionTextStaticSources | 1ull << kInteractionTextProfileSource |
               1ull << kInteractionTextPopupSource | 1ull << kInteractionTextFormSource;
    }
    values[25] = renderer.friends_scene ? renderer.friends_scene->revision : 0;
    values[26] = renderer.friends_requests_scene ? renderer.friends_requests_scene->revision : 0;
    values[kInteractionTextProfileSource] = renderer.profile_scene ? renderer.profile_scene->revision : 0;
    values[28] = renderer.image_viewer ? renderer.image_viewer->revision : 0;
    values[kInteractionTextActionBarSource] =
        renderer.message_action_bar ? renderer.message_action_bar->revision : 0;
    values[kInteractionTextPopupSource] = renderer.popup_scene ? renderer.popup_scene->revision : 0;
    values[31] = renderer.reaction_viewer ? renderer.reaction_viewer->revision : 0;
    return kInteractionTextAllSources;
}
} // namespace

void d3d11_renderer_commit_interaction_text_state(D3D11Renderer* renderer) noexcept {
    u64 values[kInteractionTextSourceCount];
    const u64 used = interaction_text_source_values(*renderer, values);
    for (u32 i = 0; i < kInteractionTextSourceCount; ++i) {
        if (!(used >> i & 1u))
            continue;
        u8* field = reinterpret_cast<u8*>(renderer) + kInteractionTextSourceOffsets[i];
        if (kInteractionTextWideSources >> i & 1u)
            *reinterpret_cast<u64*>(field) = values[i];
        else
            *reinterpret_cast<u32*>(field) = static_cast<u32>(values[i]);
    }
}

bool d3d11_renderer_interaction_text_state_current(const D3D11Renderer& renderer) noexcept {
    u64 values[kInteractionTextSourceCount];
    u64 used = interaction_text_source_values(renderer, values);
    // Friends and the thread browser hide the action bar.
    if (d3d11_renderer_active(renderer.friends_scene) || d3d11_renderer_thread_browser_visible(renderer))
        used &= ~(1ull << kInteractionTextActionBarSource);
    for (u32 i = 0; i < kInteractionTextSourceCount; ++i) {
        if (!(used >> i & 1u))
            continue;
        const u8* field = reinterpret_cast<const u8*>(&renderer) + kInteractionTextSourceOffsets[i];
        if ((kInteractionTextWideSources >> i & 1u ? *reinterpret_cast<const u64*>(field)
                                                    : *reinterpret_cast<const u32*>(field)) != values[i])
            return false;
    }
    return true;
}

bool renderer_project_accessible_text(D3D11Renderer* renderer, const Theme& theme, UiTextConsumeFunction consume,
                                      void* context) noexcept {
    if (!renderer || !consume || renderer->login_visible)
        return false;
    const bool shell_ready = d3d11_renderer_login_form_text(*renderer) || renderer_settings_page_visible(*renderer) ||
                             (renderer->ui_scene && renderer->ui_scene->count && renderer->shell_navigation_scene &&
                              renderer->chat_state && renderer->navigation_view);
    UiTextEmitter shell{}, overlay{};
    TextOverlayBoundaries boundaries{};
    if (!ui_text_emitter_initialize(&shell, consume, context) ||
        !ui_text_emitter_initialize(&overlay, consume, context))
        return false;
    const bool projected_shell = shell_ready && project_shell_text(renderer, theme, &shell);
    const bool projected_overlay = project_overlay_text(renderer, theme, &overlay, &boundaries);
    return projected_shell || projected_overlay;
}

bool renderer_refresh_interaction_text(D3D11Renderer* renderer, const Theme& theme) noexcept {
    if (!renderer)
        return false;
    if (renderer->login_visible || !renderer->content_text ||
        d3d11_renderer_interaction_text_state_current(*renderer))
        return true;
    return renderer_rebuild_content_text(renderer, theme);
}

TextSystem renderer_text_system(D3D11Renderer* renderer, UiTextFace face) noexcept {
    return renderer ? command_text_system(renderer, face) : TextSystem{};
}

bool renderer_prepare_typography(D3D11Renderer* renderer, const UiThemeTypography& value,
                                 RendererTypographyCandidate* candidate) noexcept {
    if (!renderer || !candidate || candidate->prepared || !renderer->glyph_text.ready ||
        !ui_theme_typography_valid(value))
        return false;
    const auto& source = renderer->glyph_text.system;
    const UiThemeTypography defaults{};
    const auto& current = renderer->typography_owner ? renderer->typography_owner->policy.value : defaults;
    RendererTypographyCandidate prepared{nullptr, renderer, typography_source_identity(source),
                                         renderer->typography_revision, true};
    if (ui_theme_typography_equal(current, value)) {
        *candidate = prepared;
        return true;
    }
    if (renderer->typography_revision == ~u32{0})
        return false;
    auto* owner = static_cast<RendererTypographyOwner*>(
        VirtualAlloc(nullptr, sizeof(RendererTypographyOwner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!owner)
        return false;
    // Fresh zeroed storage supplies the system fields; policy defaults are replaced below.
    static_assert(is_trivial_v<TextSystem>);
    owner->policy.value = value;
    owner->policy.code_fixed_weight = text_fonts[static_cast<u32>(value.code_face)].file != nullptr;
    auto& system = owner->system;
    system.em_size = value.body_size;
    system.typography = &owner->policy;
    system.tracking = text_fonts[static_cast<u32>(value.body_face)].tracking;
    system.fixed_weight = text_fonts[static_cast<u32>(value.body_face)].file != nullptr;
    if (!source.native_fonts || !native_font_session_retain(source.native_fonts)) {
        release_typography_owner(owner);
        return false;
    }
    system.native_fonts = source.native_fonts;
    if (!native_text_context_initialize(&system.native_text, system.native_fonts)) {
        release_typography_owner(owner);
        return false;
    }
    const bool ready_faces =
        prepare_typography_face(*owner, value.body_face, &system.native_format) &&
        prepare_typography_face(*owner, value.code_face, &owner->native_code_format);
    bool ready = ready_faces &&
                 native_text_context_set_code_format(system.native_text, owner->native_code_format) &&
                 native_text_format_prepare(system.native_text, system.native_format) &&
                 native_text_format_prepare(system.native_text, owner->native_code_format);
    // Prepare the actual shaping/measurement path before the snapshot can publish.
    wchar_t scratch[64]{};
    if (ready) {
        const auto body = text_measure_utf8(&system, utf8_literal("Typography 012"), {}, 4096, 4096,
                                            {scratch, 64}, TextWrapping::no_wrap);
        const TextStyleSpan code_style{0, 14, text_style_monospace};
        const auto code =
            text_measure_utf8(&system, utf8_literal("Typography 012"), {&code_style, 1}, 4096, 4096,
                              {scratch, 64}, TextWrapping::no_wrap);
        ready = body.ok() && code.ok();
    }
    if (!ready) {
        release_typography_owner(owner);
        return false;
    }
    prepared.owner = owner;
    *candidate = prepared;
    return true;
}

void renderer_discard_typography(RendererTypographyCandidate* candidate) noexcept {
    if (!candidate)
        return;
    release_typography_owner(candidate->owner);
    reset_value(candidate);
}

bool renderer_commit_typography(D3D11Renderer* renderer, RendererTypographyCandidate* candidate) noexcept {
    if (!renderer || !candidate || !candidate->prepared || candidate->renderer != renderer ||
        candidate->source_revision != renderer->typography_revision ||
        candidate->source_identity != typography_source_identity(renderer->glyph_text.system))
        return false;
    if (!candidate->owner) {
        reset_value(candidate);
        return true;
    }
    auto* previous_owner = renderer->typography_owner;
    auto previous_system = renderer->glyph_text.system;
    auto* owner = candidate->owner;
    // Edit layouts keep font-independent geometry and owned COM references
    // until their next measurement, where font guards reject stale ones;
    // remeasuring preserves the logical selection and clamped manual scroll.
    message_hit_test_cache_invalidate(&renderer->message_hit_test);
    shaped_text_reset(&renderer->glyph_text.title);
    renderer->glyph_text.title_quad_count = 0;
    if (renderer->content_text) {
        auto& text = *renderer->content_text;
        shaped_text_reset(&text.scratch);
        text.quad_count = text.batch_count = text.content_semantic_count = text.content_hit_region_count =
            0;
    }
    for (auto& handle : renderer->native_text_faces)
        handle = 0;
    if (renderer->picker_glyph_cache)
        reset_value(renderer->picker_glyph_cache);
    shaped_text_cache_clear(renderer->shaped_text_cache);
    renderer->glyph_text.system = owner->system;
    reset_value(&owner->system);
    renderer->typography_owner = owner;
    ++renderer->typography_revision;
    // The public measure revision is 16-bit; it wraps to 1, never 0.
    renderer->text_measure_revision = renderer->text_measure_revision == 65535
                                          ? 1
                                          : static_cast<u16>(renderer_text_measure_revision(*renderer) + 1);
    if (renderer->message_measure_cache)
        ui_text_measure_cache_invalidate(renderer->message_measure_cache);
    if (renderer->search_message_measure_cache &&
        renderer->search_message_measure_cache != renderer->message_measure_cache)
        ui_text_measure_cache_invalidate(renderer->search_message_measure_cache);
    content_text_retention_invalidate(&renderer->content_text_retention);
    renderer->interaction_text_active_mask = ~u32{0};
    renderer->uploaded_text_storage = nullptr;
    renderer->uploaded_text_rebuild = renderer->uploaded_text_quad_count = 0;
    renderer->retained_presentation.valid = false;
    presentation_damage_invalidate_full(&renderer->presentation_damage);
    text_system_release(&previous_system);
    release_typography_owner(previous_owner);
    reset_value(candidate);
    return true;
}

void renderer_release_typography(D3D11Renderer* renderer) noexcept {
    if (!renderer || (renderer->typography_owner &&
                      renderer->glyph_text.system.typography == &renderer->typography_owner->policy))
        return;
    release_typography_owner(renderer->typography_owner);
    renderer->typography_owner = nullptr;
}
u32 renderer_typography_revision(const D3D11Renderer& renderer) noexcept {
    return renderer.typography_revision;
}

bool renderer_bind_thread_browser_scene(D3D11Renderer* renderer, UiThreadBrowserScene* scene) noexcept {
    if (!renderer)
        return false;
    if (renderer->thread_browser_scene == scene)
        return true;
    renderer->thread_browser_scene = scene;
    text_edit_layout_invalidate(&renderer->composer_layout);
    content_text_retention_invalidate(&renderer->content_text_retention);
    return true;
}

bool d3d11_renderer_rebuild_text_mode(D3D11Renderer* renderer, const Theme& theme,
                                      bool include_messages) noexcept {
    const bool login_form = renderer && d3d11_renderer_login_form_text(*renderer);
    const bool settings_active = renderer && !login_form && renderer_settings_page_visible(*renderer);
    const bool friends_active = renderer && !login_form && !settings_active && renderer->friends_scene &&
                                renderer->friends_scene->active;
    const bool browser_active = renderer && !login_form && d3d11_renderer_thread_browser_visible(*renderer);
    include_messages = include_messages && !login_form && !settings_active && !friends_active &&
                       !browser_active && renderer && renderer->message_scene &&
                       renderer->message_scene->active;
    if (!renderer || !renderer->content_text ||
        (!login_form && !settings_active && (!renderer->chat_state || !renderer->navigation_view)) ||
        !renderer->glyph_text.ready ||
        (include_messages && (!renderer->message_text || !renderer->message_text->active))) {
        return false;
    }
    if (!friends_active)
        reset_value(&renderer->friends_search_geometry);
    if (settings_active || login_form)
        text_edit_layout_invalidate(&renderer->composer_layout);
    if (include_messages)
        sync_message_scroll(renderer);
    for (u32 attempt = 0; attempt < 8; ++attempt) {
        bool atlas_invalidated = false;
        bool storage_modified = false;
        bool prefix_reused = false;
        const bool composer_measured =
            login_form ? (!renderer->text_form ||
                          text_form_surface_update(renderer->text_form, renderer->glyph_text.system))
                       : (!renderer->forum_form ||
                          forum_post_surface_update(renderer->forum_form, renderer->glyph_text.system)) &&
                             (settings_active
                                  ? d3d11_renderer_measure_settings_edit(renderer) &&
                                        (!renderer->text_form ||
                                         text_form_surface_update(renderer->text_form,
                                                                  renderer->glyph_text.system))
                                  : (((friends_active || browser_active)
                                          ? measure_content_search(renderer)
                                          : d3d11_renderer_measure_composer_edit(renderer)) &&
                                     measure_message_search(renderer) && measure_media_picker(renderer) &&
                                     d3d11_renderer_measure_popup_edit(renderer) &&
                                     d3d11_renderer_measure_profile_edit(renderer) &&
                                     (!renderer->text_form ||
                                      text_form_surface_update(renderer->text_form,
                                                               renderer->glyph_text.system))));
        const bool content_built =
            composer_measured &&
            build_content_text_once(renderer, theme, &atlas_invalidated, &storage_modified, &prefix_reused,
                                    include_messages);
        if (content_built) {
            if (storage_modified) {
                ++renderer->content_text->rebuilds;
            }
            return true;
        }
        if (atlas_invalidated) {
            // Eviction invalidates geometry, not the surviving rasterized tiles.
            // Reserve a full reset for the last attempt if eviction keeps invalidating the build.
            if (attempt == 6)
                d3d11_glyph_text_reset_atlas(&renderer->glyph_text);
            continue;
        }
        // A longer overlay can exhaust the retained suffix's capacity. Rebuild
        // the base once with the current reservation rather than keeping tails
        // from the previous time/status label or allocating another text bank.
        if (prefix_reused)
            continue;
        ++renderer->content_text->rejected;
        if (!storage_modified) {
            // Shell/message glyphs may survive a transient scene rejection, but
            // interaction overlays are tied to short-lived widget handles; drop
            // them so no stale hover/popup emoji batch outlives its owner.
            discard_retained_overlay_text(renderer);
            return false;
        }
        // A content rebuild writes retained buffers in place; after a
        // capacity, shape or composer failure the partial command stream is
        // dropped so a retry cannot see stale indices.
        discard_content_text(renderer);
        return false;
    }
    ++renderer->content_text->rejected;
    renderer->content_text->last_error = RendererTextBuildError::atlas;
    discard_content_text(renderer);
    return false;
}

} // namespace maicord
