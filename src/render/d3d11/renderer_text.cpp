#include "renderer_text_internal.hpp"

namespace maicord {
struct RendererTypographyOwner {
    TextSystem system{};
    TextSystemTypography policy{};
    u32 native_code_format{};
};
namespace renderer_text_detail {

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

} // namespace renderer_text_detail
using namespace renderer_text_detail;

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

namespace renderer_text_detail {

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

// Plain text of a flag-capable owner is projected into object cells (a flag
// per cell); the projection is large, so only that path keeps one.
bool flag_projection_wanted(const D3D11Renderer& renderer, const ContentTextRequest& request,
                            RendererInlineMediaLayer* layer) noexcept {
    const ConstSlice<TextStyleSpan> styles = request.styles;
    return request.plain_flags && !request.semantics.count && !request.selection_active &&
           renderer_inline_media_owner(renderer, request.layout_owner, layer) &&
           ui_widget_manager_resolve(*request.layout_owner.manager, request.layout_owner.handle)->kind !=
               UiWidgetKind::text_field &&
           (!styles.count || (styles.count == 1 && !styles[0].byte_offset && styles[0].byte_count == request.input.bytes));
}
} // namespace renderer_text_detail
namespace {

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
    // Inlined so the running hash stays in a register instead of being reloaded
    // and stored through its capture at each of the ~60 mixes.
    const auto mix = [&hash](u64 value) __attribute__((always_inline)) noexcept { hash = hash64_mix(hash, value); };
    const auto widgets = [&hash](const UiWidgetManager& manager) __attribute__((always_inline)) noexcept {
        hash = mix_widget_state(hash, manager);
    };
    const auto scroll = [&mix](float value) __attribute__((always_inline)) noexcept { mix(float_word(value)); };
    if (const auto* scene = renderer.ui_scene) {
        mix(reinterpret_cast<usize>(scene));
        mix(scene->layout_passes);
        mix(scene->layout_rebuilds);
        mix(scene->layout_metrics_revision);
        mix(scene->widgets.generation);
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
