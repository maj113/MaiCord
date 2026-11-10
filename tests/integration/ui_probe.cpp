// Native UI probe (MaiCordUiProbe). cmake/test_registry.cmake runs the full
// sequence as maicord_ui_probe (MAICORD_UI_PROBE_ONCE=1) and every isolated scene
// gate (MAICORD_*_UI_PROBE=1, MAICORD_*_RENDER_PROBE=1) as its own CTest case;
// register a new gate there together with the code that adds it.
//
// Manual modes, deliberately not in the suite:
//   MAICORD_UI_PROBE_CAPTURE_DIR / _CAPTURE_PREFIX  write BMP frames.
//   MAICORD_UI_SHOWCASE_PROBE  website screenshots (label showcase, opt-in).
//   MAICORD_REGIONAL_FLAG_SURFACES_PROBE  the bundled flags on message, composer and reaction surfaces.
#include "ui_probe_internal.hpp"
#include "maicord/ui/shell/notice_band.hpp"
#include "../support/model_store_fixtures.hpp"
#include "../support/application_downloads_inactive.hpp"
#include "../support/warp_device.hpp"
#include "probe_draw_pipeline.hpp"
#include "regional_flag_surfaces_probe.hpp"
#include "inline_flags_probe.hpp"
#include "guild_rail_icon_probe.hpp"
#include "background_theme_probe.hpp"
#include "picker_glyph_cache_probe.hpp"
#include "colr_binding_probe.hpp"
#include "maicord/ui/widgets/presence.hpp"
#include "maicord/ui/guild/context_policy.hpp"
#include "maicord/ui/chat/private_channel_rows.hpp"
#include "maicord/ui/account/dm_profile_panel.hpp"
#include "maicord/ui/widgets/activity_status.hpp"
#include "maicord/core/decimal.hpp"
#include "../../src/render/text/shaped_text_cache.hpp"
#include "font_identity_probe.hpp"
#include "../fixtures/jpeg_reference_fixture.hpp"
#include "../fixtures/jpeg_progressive_fixture.hpp"
#include "../fixtures/jpeg_cmyk_fixture.hpp"
#include "../support/media_fixture.hpp"
#include "video_retention_probe.hpp"
#include "video_scene_ui_probe.hpp"
#include "audio_scene_ui_probe.hpp"
#include "poll_scene_ui_probe.hpp"
#include "profile_editor_ui_probe.hpp"
#include "navigation_gaps_ui_probe.hpp"
#include "theme_typography_ui_probe.hpp"
#include "tooltip_ui_probe.hpp"
#include "toast_ui_probe.hpp"
#include "account_menu_ui_probe.hpp"
#include "search_ui_probe.hpp"
#include "composer_workflow_ui_probe.hpp"
#include "quick_switcher_ui_probe.hpp"
#include "message_forward_ui_probe.hpp"
#include "download_ui_probe.hpp"
#include "unread_boundary_ui_probe.hpp"
#include "reaction_intent_ui_probe.hpp"
#include "thread_membership_ui_probe.hpp"
#include "thread_browser_ui_probe.hpp"
#include "thread_creation_ui_probe.hpp"
#include "forwarded_messages_ui_probe.hpp"
#include "menu_row_ui_probe.hpp"
#include "animation_preview_probe.hpp"
#include "empty_decoration_probe.hpp"
#include "guild_roles_probe.hpp"
#include "native_pane_glyph_stress_probe.hpp"
#include "search_polish_ui_probe.hpp"
#include "hover_retention_ui_probe.hpp"
#include "message_components_ui_probe.hpp"
#include "message_header_clip_ui_probe.hpp"
#include "edited_label_ui_probe.hpp"
#include "unified_picker_ui_probe.hpp"
#include "maicord/model/guild/permission_bits.hpp"
#include "maicord/gpu/api.hpp"

namespace maicord::app_detail {
// Search rendering fixtures bind caller-owned scenes directly. They do not
// select the application search editor or its authenticated REST owner.
MessageComposer* application_search_edit_model() noexcept {
    return nullptr;
}
bool application_search_edit_focused() noexcept {
    return false;
}
TextInputTarget application_search_edit_target() noexcept {
    return {};
}
UiRect application_search_edit_bounds() noexcept {
    return {};
}
bool application_search_edit_changed(bool) noexcept {
    return true;
}
void application_search_edit_cancel_native_group(const TextInputTarget&) noexcept {
}

} // namespace maicord::app_detail

namespace maicord {
bool gif_scroll_probe(D3D11Renderer& renderer, bool unpaced = false) noexcept;
bool media_memory_probe(D3D11Renderer& renderer) noexcept;
bool text_form_probe(D3D11Renderer& renderer, UiTextMeasureCache& cache) noexcept;
bool application_text_form_probe(D3D11Renderer& renderer, UiTextMeasureCache& cache, HWND window) noexcept;
bool application_media_animation_ui_probe(D3D11Renderer& renderer,
                                          bool (*capture)(const wchar_t*) noexcept) noexcept;
bool profile_media_sequence_ui_probe(D3D11Renderer& renderer, UiTextMeasureCache* cache, ChatStateStore* state,
                                     UserStore* users) noexcept;

bool vp8_reconstruct_probe(D3D11Renderer& renderer) noexcept;
bool video_frame_reference_probe(D3D11Renderer& renderer) noexcept;

bool shortcuts_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept;
bool call_scene_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept;
bool user_settings_ui_probe(D3D11Renderer& renderer, UiTextMeasureCache& cache,
                            bool (*capture)(const wchar_t*) noexcept) noexcept;
// Glyph placement: settings and caption titles across one-pixel width steps, and
// adjacent-glyph gaps of common UI words against the font's advances.
bool resize_text_ui_probe(D3D11Renderer& renderer, UiTextMeasureCache& cache, bool (*resize)(u32, u32) noexcept,
                          bool (*capture)(const wchar_t*) noexcept) noexcept;

bool store_ui_probe(D3D11Renderer& renderer, PopupScene& popup, UiTextMeasureCache& cache, bool (*relayout)() noexcept,
                    bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept;

bool notification_settings_ui_probe(D3D11Renderer& renderer,
                                    bool (*capture)(const wchar_t*) noexcept) noexcept;

bool inbox_ui_probe(D3D11Renderer& renderer, PopupScene& popup, bool (*capture)(const wchar_t*) noexcept) noexcept;
extern u32 inbox_ui_probe_stage;
bool caption_ui_probe(D3D11Renderer& renderer, bool (*relayout)(u32 content_height) noexcept,
                      bool (*show_login)(bool visible) noexcept) noexcept;

bool showcase_ui_probe(D3D11Renderer& renderer, MessageTextPresentation& presentation) noexcept;
bool login_recovery_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*) noexcept,
                             bool (*text_pixels)(ConstSlice<UiRect>) noexcept) noexcept;

bool login_reset_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*) noexcept,
                          bool (*text_pixels)(ConstSlice<UiRect>) noexcept) noexcept;

bool login_phone_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*) noexcept,
                          bool (*text_pixels)(ConstSlice<UiRect>) noexcept) noexcept;

bool login_browser_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*) noexcept,
                            bool (*text_pixels)(ConstSlice<UiRect>) noexcept) noexcept;

bool member_paging_probe(D3D11Renderer& renderer, UiTextMeasureCache& measure,
                         bool (*capture)(const wchar_t*) noexcept) noexcept;

bool thread_members_probe(D3D11Renderer& renderer, UiTextMeasureCache& measure,
                          bool (*capture)(const wchar_t*) noexcept) noexcept;

bool read_state_probe(D3D11Renderer& renderer, UiTextMeasureCache& measure,
                      bool (*capture)(const wchar_t*) noexcept) noexcept;

bool regional_flag_ui_probe(D3D11Renderer& renderer, PopupScene& popup,
                            bool (*capture)(const wchar_t*) noexcept) noexcept;

namespace ui_probe_detail {

constinit ProbeState g_probe{};

} // namespace ui_probe_detail

namespace {

int finish_probe(int result) noexcept {
    renderer_shutdown(&g_probe.renderer);
    DestroyWindow(g_probe.window);
    return result;
}

// Logs a gate's verdict and ends the probe with its exit code.
int finish_gate(bool valid, const char* passed, const char* failed, int failure) noexcept {
    log_write(valid ? LogLevel::info : LogLevel::error, "probe", valid ? passed : failed);
    return finish_probe(valid ? 0 : failure);
}

bool initialize_probe_message_scene() noexcept {
    message_scene_release(&g_probe.message_scene);
    return message_scene_initialize_owned(
        &g_probe.message_scene, &g_probe.message_view.timeline, kProbeMessageSceneRowCapacity, 8,
        {g_probe.message_scene_widgets, kProbeMessageSceneRowCapacity * kMessageSceneWidgetsPerRow + 1},
        {g_probe.message_scene_layout_nodes,
         kProbeMessageSceneRowCapacity * kMessageSceneWidgetsPerRow + 1});
}

bool media_pins_match_visible_quads() noexcept {
    const D3D11Renderer& renderer = g_probe.renderer;
    for (u32 batch_index = 0; batch_index < renderer.media_batch_count; ++batch_index) {
        const MediaDrawBatch& batch = renderer.media_draw.batch_data[batch_index];
        for (u32 offset = 0; offset < batch.quad_count; ++offset) {
            const MediaQuad& quad = renderer.media_draw.quad_data[batch.first_quad + offset];
            if (!ui_rect_has_area(ui_rect_intersection(UiRect{quad.left, quad.top, quad.right, quad.bottom},
                                                       batch.clip))) {
                return false;
            }
        }
    }
    for (u32 domain = 0; domain < 2; ++domain) {
        const MediaAtlas& atlas = domain == 0 ? g_probe.media_atlas : g_probe.preview_media_atlas;
        for (u32 slot = 0; slot < atlas.entry_capacity; ++slot) {
            const MediaAtlasEntry& entry = atlas.entries[slot];
            if (!entry.occupied || !glyph_atlas_is_pinned(atlas.allocations, entry.allocation)) {
                continue;
            }
            bool drawn = false;
            for (u32 index = 0; index < renderer.media_quad_count; ++index) {
                const MediaAtlasHandle handle = renderer.media_draw.handle_data[index];
                drawn = drawn || (renderer.media_draw.domain_data[index] == domain && handle.slot == slot &&
                                  handle.generation == entry.generation);
            }
            if (!drawn)
                return false;
        }
    }
    return true;
}

u32 probe_float_bits(float value) noexcept {
    union FloatBits {
        float value;
        u32 bits;
    } converted{};
    converted.value = value;
    return converted.bits;
}

bool probe_has_role_mention_colors(u32 packed) noexcept {
    const RendererTextStorage* text = g_probe.renderer.content_text;
    if (!text || packed == 0) {
        return false;
    }
    const Color expected = color_from_rgb24(packed);
    bool foreground = false;
    bool background = false;
    for (u32 index = 0; index < text->quad_count; ++index) {
        const Color& actual = text->quads[index].color;
        if (actual.red == expected.red && actual.green == expected.green && actual.blue == expected.blue) {
            foreground = foreground || actual.alpha == 1.0f;
            background = background || actual.alpha == 0.18f;
        }
    }
    return foreground && background;
}

void probe_hash_u32(u64* hash, u32 value) noexcept {
    *hash ^= value;
    *hash *= 1'099'511'628'211ull;
}

} // namespace

namespace ui_probe_detail {

u64 probe_resize_geometry_signature() noexcept {
    u64 hash = 14'695'981'039'346'656'037ull;
    const RendererTextStorage* text = g_probe.renderer.content_text;
    probe_hash_u32(&hash, text ? text->quad_count : 0);
    probe_hash_u32(&hash, text ? text->batch_count : 0);
    if (text) {
        for (u32 index = 0; index < text->quad_count; ++index) {
            const GlyphQuad& quad = text->quads[index];
            probe_hash_u32(&hash, probe_float_bits(quad.left));
            probe_hash_u32(&hash, probe_float_bits(quad.top));
            probe_hash_u32(&hash, probe_float_bits(quad.right));
            probe_hash_u32(&hash, probe_float_bits(quad.bottom));
        }
        for (u32 index = 0; index < text->batch_count; ++index) {
            const GlyphDrawBatch& batch = text->batches[index];
            probe_hash_u32(&hash, probe_float_bits(batch.clip.left));
            probe_hash_u32(&hash, probe_float_bits(batch.clip.top));
            probe_hash_u32(&hash, probe_float_bits(batch.clip.right));
            probe_hash_u32(&hash, probe_float_bits(batch.clip.bottom));
            probe_hash_u32(&hash, batch.first_quad);
            probe_hash_u32(&hash, batch.quad_count);
        }
    }
    const PaintStream& paint = g_probe.renderer.paint_stream;
    probe_hash_u32(&hash, paint.count);
    probe_hash_u32(&hash, paint.clip_count);
    probe_hash_u32(&hash, paint.batch_count);
    for (u32 index = 0; index < paint.count; ++index) {
        const PaintQuad& quad = paint.quads[index];
        probe_hash_u32(&hash, probe_float_bits(quad.left));
        probe_hash_u32(&hash, probe_float_bits(quad.top));
        probe_hash_u32(&hash, probe_float_bits(quad.right));
        probe_hash_u32(&hash, probe_float_bits(quad.bottom));
    }
    for (u32 index = 0; index < paint.clip_count; ++index) {
        const PaintClip& clip = paint.clips[index];
        probe_hash_u32(&hash, probe_float_bits(clip.left));
        probe_hash_u32(&hash, probe_float_bits(clip.top));
        probe_hash_u32(&hash, probe_float_bits(clip.right));
        probe_hash_u32(&hash, probe_float_bits(clip.bottom));
    }
    return hash;
}

} // namespace ui_probe_detail

namespace {

// The DM profile panel stage: a one-to-one DM gives its slot to the profile.
bool g_probe_dm_profile = false;

} // namespace

namespace ui_probe_detail {

bool synchronize_probe_shell_layout(Utf8View typing_label,
                                    MessageComposerAttachmentInput attachments,
                                    float navigation_width, float member_width,
                                    bool members_visible) noexcept {
    const float width_dip = static_cast<float>(g_probe.renderer.width) * 96.0f /
                            static_cast<float>(maximum<u32>(g_probe.renderer.dpi, 1));
    const float height_dip = static_cast<float>(g_probe.renderer.height) * 96.0f /
                             static_cast<float>(maximum<u32>(g_probe.renderer.dpi, 1));
    const bool friends = g_probe.renderer.friends_scene && g_probe.renderer.friends_scene->active;
    UiShellLayoutMetrics metrics = friends ? kFriendsShellLayoutMetrics : kGuildChannelShellLayoutMetrics;
    if (!friends && !members_visible) {
        metrics.flags = static_cast<u16>(metrics.flags & ~ui_shell_layout_members);
        metrics.member_sidebar_width = 0;
    }
    if (!friends && g_probe.chat_state.home_selected) {
        const auto* selected =
            chat_state_store_find_channel(g_probe.chat_state, g_probe.chat_state.selected_channel_id);
        // A group DM keeps the member pane when the fixture sizes it (as the
        // application does); one-to-one DMs and Home never show it.
        if (!selected || selected->type != 3 || member_width <= 0) {
            metrics.flags = static_cast<u16>(metrics.flags & ~ui_shell_layout_members);
            metrics.member_sidebar_width = 0;
        }
        if (!g_probe.chat_state.selected_channel_id)
            metrics.flags = ui_shell_layout_navigation;
        if (selected && channel_is_private(selected->type))
            metrics.flags = static_cast<u16>(metrics.flags | ui_shell_layout_private);
        g_probe.shell_scene.side_panel_profile = g_probe_dm_profile && selected && selected->type == 1;
        if (g_probe.shell_scene.side_panel_profile) {
            metrics.flags = static_cast<u16>(metrics.flags | ui_shell_layout_members);
            metrics.member_sidebar_width = kUiDmProfilePanelWidth;
            metrics.revision += 300000u;
        }
    }
    if (!friends && probe_option_enabled(L"MAICORD_UI_PINNED_MESSAGES_PROBE"))
        metrics.flags = static_cast<u16>(metrics.flags | ui_shell_layout_pins);
    // As the application: Invite People while a channel of the server takes invites.
    if (!friends && !g_probe.chat_state.home_selected && g_probe.chat_state.selected_guild_id &&
        guild_context_invite_channel(g_probe.chat_state, g_probe.chat_state.selected_guild_id,
                                     g_probe.chat_state.selected_channel_id))
        metrics.flags = static_cast<u16>(metrics.flags | ui_shell_layout_guild_invite);
    if (navigation_width > 0) {
        metrics.navigation_width = navigation_width;
        metrics.revision += 100000u + ui_extent_units(navigation_width);
    }
    if (!friends && member_width > 0) {
        metrics.member_sidebar_width = member_width;
        metrics.revision += 200000u + ui_extent_units(member_width) * 3u;
    }
    metrics.composer_height =
        friends ? 0.f
        : attachments.label.bytes != 0
            ? message_composer_scene_preferred_height(g_probe.message_composer,
                                                      ui_scene_composer_width(metrics, width_dip), false,
                                                      message_composer_attachment_row(attachments))
            : metrics.composer_height +
                  message_composer_scene_toolbar_height(ui_scene_composer_width(metrics, width_dip)) +
                  message_composer_scene_chrome_height(false, 0);
    // The typing strip draws over the list's bottom space, as in the application.
    metrics.composer_overlap = friends ? 0.f : message_composer_scene_chrome_height(false, 0);
    if (!friends && (metrics.composer_height != kGuildChannelShellLayoutMetrics.composer_height ||
                     attachments.label.bytes != 0))
        metrics.revision += 10'000u + ui_extent_units(metrics.composer_height);
    metrics.revision += static_cast<u32>(metrics.flags) * 7u;
    if (g_probe.shell_scene.notice) {
        metrics.notice_height = g_probe.shell_scene.notice->height;
        metrics.revision += 300'000u + ui_extent_units(metrics.notice_height);
    }
    if (!ui_scene_layout_with_metrics(&g_probe.shell_scene,
                                      friends                         ? UiShellView::friends
                                      : g_probe.chat_state.home_selected ? UiShellView::private_channels
                                                                      : UiShellView::guild_channel,
                                      metrics, width_dip, height_dip)) {
        log_write(LogLevel::error, "probe", "shell_scene_layout_failed");
        return false;
    }
    const UiLayoutNode* guild_rail = ui_scene_node(g_probe.shell_scene, UiSemantic::guild_navigation);
    const UiLayoutNode* channel_navigation = ui_scene_node(g_probe.shell_scene, UiSemantic::channel_navigation);
    const UiLayoutNode* message_view =
        ui_scene_node(g_probe.shell_scene, UiSemantic::message_view);
    const UiLayoutNode* composer =
        ui_scene_node(g_probe.shell_scene, UiSemantic::composer);
    const NavigationView* navigation = g_probe.renderer.navigation_view;
    const GuildRailSceneResult guild_result = guild_rail_scene_update(
        &g_probe.guild_rail_scene, navigation, guild_rail ? guild_rail->rectangle : UiRect{},
        g_probe.guild_rail_scene.scroll_dips);
    if (!guild_result.ok()) {
        log_u64(LogLevel::error, "probe", "guild_scene_error", static_cast<u64>(guild_result.error));
        return false;
    }
    const ShellNavigationSceneResult channel_result =
        shell_navigation_scene_update(&g_probe.shell_navigation_scene, navigation,
                                      channel_navigation ? channel_navigation->rectangle : UiRect{},
                                      g_probe.shell_navigation_scene.scroll_dips);
    if (!channel_result.ok()) {
        log_u64(LogLevel::error, "probe", "channel_scene_error", static_cast<u64>(channel_result.error));
        return false;
    }
    const MessageSceneResult message_result =
        message_scene_update(&g_probe.message_scene, message_view ? message_view->rectangle : UiRect{});
    if (!message_result.ok()) {
        log_u64(LogLevel::error, "probe", "message_scene_error", static_cast<u64>(message_result.error));
        return false;
    }
    const ChannelViewRow* selected =
        navigation ? navigation_view_find_channel(*navigation, navigation->selected_channel_id)
                   : nullptr;
    const MessageComposerProjectionContext composer_projection{
        &g_probe.users, &g_probe.chat_state, navigation ? navigation->selected_guild_id : 0};
    const MessageComposerSceneResult composer_result = message_composer_scene_update(
        &g_probe.message_composer_scene, &g_probe.message_composer,
        composer ? composer->rectangle : UiRect{},
        selected && (selected->actions & channel_action_send_messages) != 0, 0, {},
        &composer_projection, typing_label, attachments);
    if (!composer_result.ok()) {
        log_u64(LogLevel::error, "probe", "message_composer_scene_error",
                static_cast<u64>(composer_result.error));
        return false;
    }
    if (!renderer_bind_ui_scene(&g_probe.renderer, &g_probe.shell_scene)) {
        log_write(LogLevel::error, "probe", "shell_scene_bind_failed");
        return false;
    }
    if (!renderer_bind_guild_rail_scene(&g_probe.renderer, &g_probe.guild_rail_scene)) {
        log_write(LogLevel::error, "probe", "guild_scene_bind_failed");
        return false;
    }
    if (!renderer_bind_shell_navigation_scene(&g_probe.renderer, &g_probe.shell_navigation_scene)) {
        log_write(LogLevel::error, "probe", "channel_scene_bind_failed");
        return false;
    }
    if (!renderer_bind_message_scene(&g_probe.renderer, &g_probe.message_scene)) {
        log_write(LogLevel::error, "probe", "message_scene_bind_failed");
        return false;
    }
    if (!renderer_bind_message_composer_scene(&g_probe.renderer, &g_probe.message_composer_scene)) {
        log_write(LogLevel::error, "probe", "message_composer_scene_bind_failed");
        return false;
    }
    return true;
}

} // namespace ui_probe_detail

namespace {

// Failure-only QA: every renderer face holds a native run, so slot reuse
// cannot make unrelated font selections appear identical during the dump.
void probe_font_identities() noexcept {
    auto& owner = g_probe.renderer.glyph_text;
    auto* session = owner.system.native_fonts;
    if (!session)
        return;
    NativeFontInstanceHandle resolved[kRendererFontFaceCapacity]{};
    auto* instances = native_font_session_instances(session);
    auto* fonts = native_font_session_fonts(session);
    for (u32 i = 0; i < owner.font_face_count; ++i) {
        const auto& entry = owner.font_faces[i];
        u32 simulations = 0;
        if (const auto* run = native_font_run_get(*native_font_session_runs(session), entry.native_face)) {
            resolved[i] = run->instance;
            simulations = run->simulations;
        }
        log_u64(LogLevel::error, "font_identity", "renderer_id", entry.id);
        log_u64(LogLevel::error, "font_identity", "flags", entry.flags);
        log_u64(LogLevel::error, "font_identity", "instance_slot", resolved[i].slot);
        log_u64(LogLevel::error, "font_identity", "instance_generation", resolved[i].generation);
        log_u64(LogLevel::error, "font_identity", "simulations", simulations);
        const auto* instance = native_font_instance_get(*instances, resolved[i]);
        if (!instance)
            continue;
        log_u64(LogLevel::error, "font_identity", "face_slot", instance->face.slot);
        const auto& face = fonts->faces[instance->face.slot - 1u];
        log_u64(LogLevel::error, "font_identity", "collection_index", face.face.geometry.face.face_index);
        wchar_t path[1024]{};
        char utf8[4096]{};
        const DWORD length =
            GetFinalPathNameByHandleW(fonts->files[face.file_slot].file, path, 1024, FILE_NAME_NORMALIZED);
        if (length && length < 1024 &&
            WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, 4096, nullptr, nullptr))
            log_write(LogLevel::error, "font_identity", utf8);
    }
}

// Simulates the next REST response overwriting all transient shaping storage. Subsequent retained
// draws and rebuilds must remain valid.
void overwrite_probe_response_scratch() noexcept {
    for (auto& byte : g_probe.response_shape_scratch.response)
        byte = 0xa5;
}

} // namespace

namespace ui_probe_detail {

bool rebuild_probe_content_text() noexcept {
    MessageTextCoordinatorTrace trace{};
    const MessageTextCoordinatorResult coordinated =
        message_text_coordinator_converge(MessageTextCoordinatorRequest{
            &g_probe.message_scene, &g_probe.message_text, &g_probe.text_measure_cache, &g_probe.chat_state,
            &g_probe.users, &g_probe.navigation, &kDefaultTheme,
            +[](void* opaque, const Theme& theme) noexcept {
                return renderer_rebuild_content_text(static_cast<D3D11Renderer*>(opaque), theme);
            },
            &g_probe.renderer, 1'725'021'300ull, 1, &trace});
    if (!coordinated.ok()) {
        g_probe.failure_stage = coordinated.error == MessageTextCoordinatorError::finalization
                                    ? g_probe.text.last_error == RendererTextBuildError::atlas
                                          ? (g_probe.renderer.glyph_text.allocator.entry_count ==
                                                     g_probe.renderer.glyph_text.allocator.entry_capacity
                                                 ? 100u
                                             : g_probe.renderer.glyph_text.allocator.row_count >=
                                                     g_probe.renderer.glyph_text.allocator.row_capacity
                                                 ? 101u
                                                 : 102u)
                                          : 90u + static_cast<u32>(g_probe.text.last_error)
                                    : 43u + static_cast<u32>(coordinated.error);
        log_u64(LogLevel::error, "probe", "content_text_coordinator_error",
                static_cast<u64>(coordinated.error));
        log_u64(LogLevel::error, "probe", "content_text_build_error",
                static_cast<u64>(g_probe.text.last_error));
        log_u64(LogLevel::error, "probe", "content_text_font_face_count",
                g_probe.renderer.glyph_text.font_face_count);
        probe_font_identities();
        log_u64(LogLevel::error, "probe", "content_text_coordinator_scene_error",
                static_cast<u64>(coordinated.scene_error));
        log_u64(LogLevel::error, "probe", "content_text_coordinator_presentation_error",
                static_cast<u64>(coordinated.presentation_error));
        log_u64(LogLevel::error, "probe", "content_text_coordinator_passes", coordinated.passes);
        log_u64(LogLevel::error, "probe", "content_text_commands", g_probe.message_text.command_count);
        log_u64(LogLevel::error, "probe", "content_text_measurements",
                g_probe.message_text.measurement_count);
        log_u64(LogLevel::error, "probe", "text_measure_backend_calls",
                g_probe.text_measure_cache.backend_calls);
        log_u64(LogLevel::error, "probe", "text_measure_shape_error",
                static_cast<u64>(g_probe.text.last_shape_error));
        for (u32 command_index = 0; command_index < g_probe.message_text.command_count; ++command_index) {
            const MessageTextCommand& command = g_probe.message_text.commands[command_index];
            UiTextMeasureValue diagnostic_value{};
            const UiTextMeasureRequest diagnostic_request{
                UiTextMeasureKey{1, command_index + 1u, 1, 0, 1}, command.text, command.styles,
                UiSize{ui_rect_width(command.rectangle), ui_rect_height(command.rectangle)}};
            if (!renderer_text_measure_backend(&g_probe.renderer, diagnostic_request, &diagnostic_value)) {
                log_u64(LogLevel::error, "probe", "text_measure_failed_command", command_index);
                log_u64(LogLevel::error, "probe", "text_measure_failed_bytes", command.text.bytes);
                log_u64(LogLevel::error, "probe", "text_measure_failed_styles", command.styles.count);
                break;
            }
        }
        for (u32 index = 0; index < trace.count; ++index) {
            const MessageTextCoordinatorPass& pass = trace.passes[index];
            log_u64(LogLevel::error, "probe", "content_text_coordinator_trace_index", index);
            log_u64(LogLevel::error, "probe", "content_text_coordinator_trace_scroll", pass.scroll_units);
            log_u64(LogLevel::error, "probe", "content_text_coordinator_trace_total", pass.total_units);
            log_u64(LogLevel::error, "probe", "content_text_coordinator_trace_first", pass.first);
            log_u64(LogLevel::error, "probe", "content_text_coordinator_trace_count", pass.count);
            log_u64(LogLevel::error, "probe", "content_text_coordinator_trace_commands",
                    pass.command_count);
            log_u64(LogLevel::error, "probe", "content_text_coordinator_trace_measurements",
                    pass.measurement_count);
            for (u32 item = 0; item < 4; ++item) {
                log_u64(LogLevel::error, "probe", "content_text_coordinator_trace_item_units",
                        pass.leading_item_units[item]);
            }
            log_u64(LogLevel::error, "probe", "content_text_coordinator_trace_flags", pass.flags);
        }
    }
    if (coordinated.ok()) {
        if (g_probe.text.scratch.run_count != 0 || g_probe.text.scratch.glyph_count != 0)
            return false;
        overwrite_probe_response_scratch();
    }
    return coordinated.ok();
}
bool probe_sync_shell_and_text() noexcept {
    return synchronize_probe_shell_layout() && rebuild_probe_content_text();
}

} // namespace ui_probe_detail

namespace {

// The content below a window caption: same buffers, a shorter shell.
bool probe_caption_relayout(u32 content_height) noexcept {
    return renderer_resize(&g_probe.renderer, g_probe.renderer.width, content_height) &&
           probe_sync_shell_and_text();
}

} // namespace

namespace ui_probe_detail {

bool set_probe_login_view(RendererLoginView view) noexcept {
    LoginScene& scene = g_probe.login_scene;
    scene.restoring_session = view.status == RendererLoginStatus::restoring_session;
    scene.back_only = login_requires_back(view.status, view.remote_status);
    scene.focus = view.focus;
    scene.mfa_capabilities = view.mfa_capabilities;
    scene.selected_mfa_capability = view.selected_mfa_capability;
    scene.credential_fields_hidden = view.status == RendererLoginStatus::mfa_backup_acknowledgement;
    scene.action_disabled = login_view_action_disabled(view);
    scene.recovery_disabled = login_view_recovery_disabled(view);
    scene.submit_disabled = login_view_submit_disabled(view);
    scene.captcha_action = login_view_captcha_action(view);
    scene.password_reveal_available = login_view_password_reveal_available(view);
    scene.password_revealed = scene.password_reveal_available && view.password_revealed;
    const float width_dip = static_cast<float>(g_probe.renderer.width) * 96.0f /
                            static_cast<float>(maximum<u32>(g_probe.renderer.dpi, 1));
    const float height_dip = static_cast<float>(g_probe.renderer.height) * 96.0f /
                             static_cast<float>(maximum<u32>(g_probe.renderer.dpi, 1));
    if (view.visible && !login_scene_layout(&scene, width_dip, height_dip)) {
        return false;
    }
    if (view.visible)
        view.focus = scene.focus;
    if (!renderer_set_login_view(&g_probe.renderer, view.visible ? &scene : nullptr, view, kDefaultTheme)) {
        return false;
    }
    g_probe.login_view = view;
    g_probe.login_view_bound = view.visible;
    return true;
}

} // namespace ui_probe_detail

namespace {

// The sign-in screen (a QR code, no credentials) under the window caption.
bool probe_caption_login(bool visible) noexcept {
    if (!visible)
        return set_probe_login_view(RendererLoginView{}) && probe_sync_shell_and_text();
    constexpr Utf8View payload =
        utf8_literal("https://chat_state.com/ra/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    static QrCode qr{};
    if (!qr_code_encode_m(ConstSlice<u8>{reinterpret_cast<const u8*>(payload.data), payload.bytes}, 5, &qr))
        return false;
    const RendererLoginView view{{},    {},    {},    0,     0,     false, LoginControl::qr,
                                 RendererLoginStatus::remote_qr_ready, login_mfa_capability_none,
                                 login_mfa_capability_none, 0, false, &qr, false, false, false, true};
    return set_probe_login_view(view);
}

bool add_guild(u64 id, Utf8View name) noexcept {
    return chat_state_store_upsert_guild(&g_probe.chat_state,
                                            GuildInput{id, name, GuildMutationKind::create})
        .ok();
}

bool add_channel(u64 id, u64 guild_id, u64 parent_id, u32 position, u8 type, Utf8View name) noexcept {
    return chat_state_store_upsert_channel(
               &g_probe.chat_state, ChannelInput{id, guild_id, parent_id, name, position, 0, type})
        .ok();
}

} // namespace

namespace ui_probe_detail {

bool add_message(u64 id, Utf8View content,
                 MessageContentAvailability availability) noexcept {
    return message_store_upsert(&g_probe.message_view.store,
                                MessageInput{id, 111, id + 10'000, snowflake_timestamp_ms(id), 0,
                                             content, 0, availability})
               .ok() &&
           user_store_upsert(&g_probe.users, id + 10'000, utf8_literal("Render Service")).ok();
}

} // namespace ui_probe_detail

namespace {

// Decodes `bytes` into `image` and uploads it as attachment `id`; `decoded` reports the decode.
MediaAtlasGpuResult probe_upload_jpeg(ConstSlice<u8> bytes, u64 id, JpegImage* image,
                                      JpegDecodeResult* decoded) noexcept {
    *decoded = jpeg_decode_coefficients(bytes, image, &g_probe.jpeg_workspace);
    return decoded->ok() ? renderer_upload_jpeg(&g_probe.renderer, media_asset_attachment(id), *image,
                                                g_probe.jpeg_workspace.coefficients)
                         : MediaAtlasGpuResult{};
}

bool jpeg_icc_applied(const JpegImage& image) noexcept {
    return image.tables && image.tables->icc && image.tables->icc->profile.disposition == JpegIccDisposition::applied;
}

bool apply_probe_permissions() noexcept {
    constexpr u64 actions = kPermissionViewChannel | kPermissionSendMessages |
                            kPermissionReadMessageHistory | kPermissionAddReactions |
                            kPermissionEmbedLinks | kPermissionAttachFiles;
    if (!chat_state_store_upsert_role(&g_probe.chat_state,
                                         RoleInput{100, 100, actions, utf8_literal("@everyone")}) ||
        !chat_state_store_upsert_role(
            &g_probe.chat_state, RoleInput{55, 100, 0, utf8_literal("Performance"), 1, 0xC46BEFu}) ||
        !chat_state_store_set_self_roles(&g_probe.chat_state, 100, 999, 1, ConstSlice<u64>{nullptr, 0}) ||
        !chat_state_store_append_channel_overwrite(
            &g_probe.chat_state, 112,
            PermissionOverwriteInput{100, 0, kPermissionViewChannel, 0})) {
        return false;
    }
    chat_state_store_recompute_guild_permissions(&g_probe.chat_state, 100);
    return true;
}

bool initialize_fixture(bool focused_text = false) noexcept {
    g_probe.failure_stage = 1;
    if (!chat_state_store_initialize(
            &g_probe.chat_state, Slice<GuildRecord>{g_probe.guilds, kProbeGuildCapacity},
            Slice<u16>{g_probe.guild_order, kProbeGuildCapacity},
            Slice<ChannelRecord>{g_probe.channels, kProbeChannelCapacity},
            Slice<u16>{g_probe.channel_order, kProbeChannelCapacity},
            Slice<u8>{g_probe.names, kProbeNameBytes}) ||
        !chat_state_store_bind_permissions(
            &g_probe.chat_state, Slice<RoleRecord>{g_probe.roles, kProbeRoleCapacity},
            Slice<PermissionOverwriteRecord>{g_probe.overwrites, kProbeOverwriteCapacity}) ||
        !user_store_initialize(&g_probe.users,
                                       Slice<UserRecord>{g_probe.user_records, kProbeUserCapacity},
                                       Slice<u32>{g_probe.user_order, kProbeUserCapacity},
                                       Slice<u8>{g_probe.user_names, kProbeUserNameBytes}) ||
        !user_store_upsert(&g_probe.users, 123, utf8_literal("Mai")).ok() ||
        !message_rich_store_initialize(&g_probe.rich_store,
                                       Slice<MessageRichRecord>{g_probe.rich_records, kProbeRichCapacity},
                                       Slice<u16>{g_probe.rich_order, kProbeRichCapacity},
                                       Slice<u8>{g_probe.rich_text, kProbeRichTextBytes}) ||
        !media_atlas_initialize(&g_probe.media_atlas, kProbeMediaExtent, kProbeMediaExtent, 1,
                                Slice<MediaAtlasEntry>{g_probe.media_entries, kProbeMediaEntryCapacity},
                                Slice<u32>{g_probe.media_buckets, kProbeMediaEntryCapacity * 2},
                                Slice<GlyphAtlasEntry>{g_probe.media_allocations, kProbeMediaEntryCapacity},
                                Slice<GlyphAtlasRow>{g_probe.media_rows, kProbeMediaRowCapacity}) ||
        !renderer_bind_media_atlas(&g_probe.renderer, &g_probe.media_atlas) ||
        !media_atlas_initialize(
            &g_probe.preview_media_atlas, kProbeMediaExtent, kProbeMediaExtent, 1,
            Slice<MediaAtlasEntry>{g_probe.preview_media_entries, kProbeMediaEntryCapacity},
            Slice<u32>{g_probe.preview_media_buckets, kProbeMediaEntryCapacity * 2},
            Slice<GlyphAtlasEntry>{g_probe.preview_media_allocations, kProbeMediaEntryCapacity},
            Slice<GlyphAtlasRow>{g_probe.preview_media_rows, kProbeMediaRowCapacity}) ||
        !renderer_bind_preview_media_atlas(&g_probe.renderer, &g_probe.preview_media_atlas) ||
        !add_guild(100, utf8_literal("MaiCord Lab")) || !add_guild(200, utf8_literal("Graphics Guild")) ||
        !add_channel(109, 100, 0, 0, 0, utf8_literal("announcements")) ||
        !add_channel(110, 100, 0, 0, 4, utf8_literal("PROJECT")) ||
        !add_channel(111, 100, 110, 0, 0, utf8_literal("general")) ||
        !add_channel(112, 100, 110, 1, 0, utf8_literal("performance")) ||
        !add_channel(113, 100, 110, 2, 2, utf8_literal("Voice Lab")) ||
        !add_channel(120, 100, 0, 1, 4, utf8_literal("RENDERING")) ||
        !add_channel(121, 100, 120, 0, 0, utf8_literal("gpu-text")) ||
        !add_channel(122, 100, 121, 0, 11, utf8_literal("atlas-debug")) || !apply_probe_permissions() ||
        !chat_state_store_note_message(&g_probe.chat_state, 121, 1'270'000'000'000'010'000ull, 2, true,
                                          false) ||
        !chat_state_store_select_channel(&g_probe.chat_state, 111).ok() ||
        !navigation_view_initialize(
            &g_probe.navigation, Slice<GuildViewRow>{g_probe.guild_rows, kProbeGuildCapacity},
            Slice<ChannelViewRow>{g_probe.channel_rows, kProbeChannelCapacity},
            Slice<u16>{g_probe.channel_scratch, kProbeChannelCapacity},
            Slice<u8>{g_probe.channel_visibility_scratch, kProbeChannelCapacity},
            Slice<ChannelSortKey>{g_probe.channel_sort_keys, kProbeChannelCapacity}) ||
        !navigation_view_rebuild(&g_probe.navigation, g_probe.chat_state).ok() ||
        navigation_view_find_channel(g_probe.navigation, 112)) {
        return false;
    }
    const ChannelViewRow* activity = navigation_view_find_channel(g_probe.navigation, 121);
    if (!activity || !activity->unread || activity->mention_count != 1 ||
        (activity->actions & channel_action_send_messages) == 0 ||
        g_probe.navigation.guild_rows[0].mention_count != 1) {
        return false;
    }

    g_probe.failure_stage = 2;
    const u64 message_base = 1'270'000'000'000'000'000ull;
    for (u32 index = 0; index < static_cast<u32>(sizeof(g_probe.oversized_text)); ++index) {
        g_probe.oversized_text[index] = 'x';
    }
    if (!add_message(message_base, focused_text
                                       ? utf8_literal("Native wrapped text probe")
                                       : Utf8View{g_probe.oversized_text,
                                                  static_cast<u32>(sizeof(g_probe.oversized_text))})) {
        return false;
    }
    g_probe.failure_stage = 3;
    if (!message_view_runtime_reindex(
             &g_probe.message_view, ui_extent_units(kDefaultUiBootstrapMetrics.message_row_height), 1, true)
             .ok() ||
        !message_view_runtime_bind_rich_store(&g_probe.message_view, &g_probe.rich_store).ok()) {
        return false;
    }
    if (!initialize_probe_message_scene()) {
        return false;
    }

    g_probe.text.quads = g_probe.text_quads;
    g_probe.text.batches = g_probe.text_batches;
    g_probe.text.glyphs = g_probe.response_shape_scratch.shape.glyphs;
    g_probe.text.runs = g_probe.response_shape_scratch.shape.runs;
    g_probe.text.clusters = g_probe.response_shape_scratch.shape.clusters;
    g_probe.text.hit_spans = g_probe.hit_spans;
    g_probe.text.hit_region_scratch = g_probe.hit_region_scratch;
    g_probe.text.decorations = g_probe.text_decorations;
    g_probe.text.content_semantics = g_probe.content_semantics;
    g_probe.text.content_hit_regions = g_probe.content_hit_regions;
    g_probe.text.quad_capacity = kRendererContentGlyphCapacity;
    g_probe.text.batch_capacity = kRendererContentBatchCapacity;
    g_probe.text.glyph_capacity = kRendererContentTextUnitCapacity;
    g_probe.text.run_capacity = kRendererContentRunCapacity;
    g_probe.text.cluster_capacity = kRendererContentTextUnitCapacity;
    g_probe.text.hit_span_capacity = kTextHitSpanMaximum;
    g_probe.text.hit_region_scratch_capacity = kRendererContentHitRegionCapacity;
    g_probe.text.decoration_capacity = kRendererTextDecorationCapacity;
    g_probe.text.content_semantic_capacity = kRendererContentSemanticCapacity;
    g_probe.text.content_hit_region_capacity = kRendererContentHitRegionCapacity;
    renderer_bind_navigation_view(&g_probe.renderer, &g_probe.chat_state, &g_probe.users, &g_probe.navigation);
    g_probe.failure_stage = 4;
    if (!synchronize_probe_shell_layout() ||
        !message_text_presentation_initialize(
            &g_probe.message_text,
            Slice<MessageTextCommand>{g_probe.message_text_commands, kMaximumMessageTextCommands},
            Slice<char>{g_probe.message_text_utf8, kMaximumMessageTextUtf8Bytes},
            Slice<TextStyleSpan>{g_probe.message_text_styles, kMaximumMessageTextStyleSpans},
            Slice<MessageMarkdownSemantic>{g_probe.message_text_semantics, kMaximumMessageTextSemantics},
            Slice<MessageTextMeasurement>{g_probe.message_text_measurements,
                                          kMaximumMessageTextMeasurements},
            Slice<MessageTextSpoilerReveal>{g_probe.message_text_spoiler_reveals,
                                            kMaximumMessageTextSpoilerReveals},
            Slice<u32>{g_probe.message_text_byte_styles, kMessageTextByteStyleScratchCapacity}) ||
        !renderer_bind_text_storage(&g_probe.renderer, &g_probe.text) ||
        !ui_text_measure_cache_initialize(
            &g_probe.text_measure_cache,
            Slice<UiTextMeasureRecord>{g_probe.message_text_measure_cache_records,
                                       kMaximumMessageTextMeasureCacheRecords},
            renderer_text_measure_backend, &g_probe.renderer) ||
        !renderer_bind_message_text_presentation(&g_probe.renderer, &g_probe.message_text)) {
        return false;
    }
    g_probe.renderer.message_measure_cache = &g_probe.text_measure_cache;
    // Focused render probes need the actual bound message/profile renderer,
    // but not the unrelated media, retention and interaction fixture sweeps.
    if (focused_text)
        return rebuild_probe_content_text();
    g_probe.failure_stage = 5;
    if (!rebuild_probe_content_text()) {
        return false;
    }
    if (g_probe.message_text.command_count == 0 || g_probe.message_text.projection_signature == 0 ||
        g_probe.message_text.projection_signature !=
            message_text_presentation_projection_signature(g_probe.message_text)) {
        return false;
    }
    MessageTextCommand& signature_command = g_probe.message_text.commands[0];
    const Color signature_color = signature_command.color;
    signature_command.color.red = signature_color.red + 0.03125f;
    const u64 recolored_signature = message_text_presentation_projection_signature(g_probe.message_text);
    signature_command.color = signature_color;
    if (recolored_signature == g_probe.message_text.projection_signature ||
        message_text_presentation_projection_signature(g_probe.message_text) !=
            g_probe.message_text.projection_signature) {
        return false;
    }
    const u32 composer_builds = g_probe.renderer.composer_layout.builds;
    const u32 composer_hits = g_probe.renderer.composer_layout.hits;
    const u32 composer_rejected = g_probe.renderer.composer_layout.rejected;
    const u32 content_builds = g_probe.renderer.content_text_retention.builds;
    const u32 content_hits = g_probe.renderer.content_text_retention.hits;
    const u32 content_misses = g_probe.renderer.content_text_retention.misses;
    const u32 text_rebuilds = g_probe.text.rebuilds;
    const u32 text_quads = g_probe.text.quad_count;
    if (composer_builds == 0 || content_builds == 0 || content_misses == 0 ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.composer_layout.builds != composer_builds ||
        g_probe.renderer.composer_layout.hits != composer_hits + 1u ||
        g_probe.renderer.composer_layout.rejected != composer_rejected ||
        g_probe.renderer.content_text_retention.builds != content_builds ||
        g_probe.renderer.content_text_retention.hits != content_hits + 1u ||
        g_probe.renderer.content_text_retention.misses != content_misses ||
        g_probe.text.rebuilds != text_rebuilds || g_probe.text.quad_count != text_quads) {
        return false;
    }
    const u32 composer_builds_before_selection = g_probe.renderer.composer_layout.builds;
    const bool composer_geometry_before_selection = g_probe.renderer.composer_layout.geometry;
    const u32 composer_hits_before_selection = g_probe.renderer.composer_layout.hits;
    const u32 content_builds_before_selection = g_probe.renderer.content_text_retention.builds;
    const u32 content_hits_before_selection = g_probe.renderer.content_text_retention.hits;
    const u32 text_rebuilds_before_selection = g_probe.text.rebuilds;
    const bool selection_retained =
        message_composer_set_selection(&g_probe.message_composer, 0, g_probe.message_composer.bytes) ==
            MessageComposerResult::changed &&
        synchronize_probe_shell_layout() &&
        renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
        g_probe.renderer.composer_layout.builds == composer_builds_before_selection + 1u &&
        g_probe.renderer.composer_layout.geometry == composer_geometry_before_selection &&
        g_probe.renderer.composer_layout.hits == composer_hits_before_selection &&
        g_probe.renderer.composer_layout.rejected == composer_rejected &&
        g_probe.renderer.content_text_retention.builds == content_builds_before_selection + 1u &&
        g_probe.renderer.content_text_retention.hits == content_hits_before_selection &&
        g_probe.text.rebuilds == text_rebuilds_before_selection + 1u;
    if (!selection_retained)
        return false;
    constexpr u32 content_hit_iterations = 10'000;
    constexpr u32 content_rebuild_iterations = 100;
    LARGE_INTEGER content_frequency{};
    LARGE_INTEGER content_begin{};
    LARGE_INTEGER content_end{};
    const u32 measured_content_builds = g_probe.renderer.content_text_retention.builds;
    const u32 measured_content_hits = g_probe.renderer.content_text_retention.hits;
    const u32 measured_text_rebuilds = g_probe.text.rebuilds;
    if (!QueryPerformanceFrequency(&content_frequency) || !QueryPerformanceCounter(&content_begin)) {
        return false;
    }
    for (u32 iteration = 0; iteration < content_hit_iterations; ++iteration) {
        if (!renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme)) {
            return false;
        }
    }
    if (!QueryPerformanceCounter(&content_end) || content_end.QuadPart <= content_begin.QuadPart ||
        g_probe.renderer.content_text_retention.builds != measured_content_builds ||
        g_probe.renderer.content_text_retention.hits != measured_content_hits + content_hit_iterations ||
        g_probe.text.rebuilds != measured_text_rebuilds) {
        return false;
    }
    const u64 content_hit_rate = static_cast<u64>(content_hit_iterations) *
                                 static_cast<u64>(content_frequency.QuadPart) /
                                 static_cast<u64>(content_end.QuadPart - content_begin.QuadPart);
    if (!QueryPerformanceCounter(&content_begin)) {
        return false;
    }
    for (u32 iteration = 0; iteration < content_rebuild_iterations; ++iteration) {
        content_text_retention_invalidate(&g_probe.renderer.content_text_retention);
        if (!renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme)) {
            return false;
        }
    }
    if (!QueryPerformanceCounter(&content_end) || content_end.QuadPart <= content_begin.QuadPart ||
        g_probe.renderer.content_text_retention.builds !=
            measured_content_builds + content_rebuild_iterations ||
        g_probe.text.rebuilds != measured_text_rebuilds + content_rebuild_iterations) {
        return false;
    }
    const u64 content_rebuild_rate = static_cast<u64>(content_rebuild_iterations) *
                                     static_cast<u64>(content_frequency.QuadPart) /
                                     static_cast<u64>(content_end.QuadPart - content_begin.QuadPart);
    log_u64(LogLevel::info, "probe", "content_text_retained_runs_per_second", content_hit_rate);
    log_u64(LogLevel::info, "probe", "content_text_rebuild_runs_per_second", content_rebuild_rate);
    g_probe.failure_stage = 45;
    const UiLayoutNode* composer_edit =
        message_composer_scene_layout_node(g_probe.message_composer_scene, MessageComposerControl::edit);
    const UiRect composer_content = composer_edit ? composer_edit->content_rectangle : UiRect{};
    const float composer_scale = static_cast<float>(g_probe.renderer.dpi) / 96.0f;
    const i32 composer_y =
        static_cast<i32>((composer_content.top + ui_rect_height(composer_content) * 0.5f) * composer_scale);
    u32 composer_start = ~0u;
    u32 composer_end = 0;
    if (!composer_edit || !g_probe.renderer.composer_layout.active ||
        g_probe.renderer.composer_layout.selection_count == 0 ||
        g_probe.renderer.composer_layout.caret.height <= 0.0f ||
        !renderer_message_composer_edit_hit_test(g_probe.renderer,
                                                 static_cast<i32>(composer_content.left * composer_scale),
                                                 composer_y, &composer_start) ||
        !renderer_message_composer_edit_hit_test(
            g_probe.renderer, static_cast<i32>((composer_content.right - 2.0f) * composer_scale),
            composer_y, &composer_end) ||
        composer_start != 0 || composer_end != g_probe.message_composer.bytes) {
        return false;
    }
    g_probe.failure_stage = 121;
    message_composer_reset(&g_probe.message_composer, 111, false);
    if (!synchronize_probe_shell_layout(utf8_literal("QuillFern is typing…")) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme)) {
        return false;
    }
    const UiLayoutNode* typing = message_composer_scene_layout_node(
        g_probe.message_composer_scene, MessageComposerControl::typing_indicator);
    const UiLayoutNode* composer_surface = ui_widget_manager_layout_node(
        g_probe.message_composer_scene.widgets, g_probe.message_composer_scene.surface);
    bool typing_glyphs = false;
    bool surface_painted = false;
    if (!typing || !composer_surface || typing->rectangle.bottom >= composer_surface->rectangle.top ||
        ui_rect_height(composer_surface->rectangle) != 48.0f)
        return false;
    // Uncut labels may share a viewport batch; validate their visible glyphs.
    for (u32 batch_index = 0; batch_index < g_probe.text.batch_count; ++batch_index) {
        const GlyphDrawBatch& batch = g_probe.text.batches[batch_index];
        for (u32 offset = 0; offset < batch.quad_count; ++offset) {
            const GlyphQuad& quad = g_probe.text.quads[batch.first_quad + offset];
            const UiRect bounds{quad.left, quad.top, quad.right, quad.bottom};
            const UiRect visible = ui_rect_intersection(bounds, batch.clip);
            if (!ui_rect_has_area(ui_rect_intersection(visible, typing->rectangle)))
                continue;
            // The label's line sits kUiTypingLabelDrop below the row's middle; its clip grows by it.
            UiRect text_clip = typing->clip_rectangle;
            text_clip.bottom += kUiTypingLabelDrop;
            if (!ui_rect_equal(ui_rect_intersection(visible, text_clip), visible))
                return false;
            const Color expected = kDefaultTheme.text_primary;
            if (quad.top < typing->rectangle.top || quad.bottom > typing->rectangle.bottom + kUiTypingLabelDrop ||
                quad.color.red != expected.red || quad.color.green != expected.green ||
                quad.color.blue != expected.blue || quad.color.alpha != expected.alpha)
                return false;
            typing_glyphs = true;
        }
    }
    for (u32 index = 0; index < g_probe.renderer.paint_stream.count; ++index) {
        const PaintQuad& quad = g_probe.renderer.paint_stream.quads[index];
        surface_painted =
            surface_painted || ui_rect_equal(UiRect{quad.left, quad.top, quad.right, quad.bottom},
                                             composer_surface->rectangle);
    }
    if (!typing_glyphs || !surface_painted)
        return false;
    log_write(LogLevel::info, "probe", "composer_typing_separate_surface_glyphs_pass");
    // Reproduce a child refresh consuming the typing change before the parent
    // checks its allocation. The label is gone but its 24 DIP strip stays
    // reserved: the composer keeps its height, so nothing above it moves.
    const float typing_surface_height = ui_rect_height(composer_surface->rectangle);
    const float composer_bottom = composer_surface->rectangle.bottom;
    auto& typing_scene = g_probe.message_composer_scene;
    typing_scene.measured_row_height = kMessageComposerMinimumHeight;
    for (u32 cycle = 0; cycle < 3; ++cycle) {
        if (cycle && !synchronize_probe_shell_layout(utf8_literal("QuillFern is typing…")))
            return false;
        const UiRect old_allocation = typing_scene.viewport;
        if (!message_composer_scene_update(&typing_scene, &g_probe.message_composer, old_allocation, true)
                 .ok() ||
            typing_scene.typing_label_bytes)
            return false;
        const float required = message_composer_scene_cached_height(typing_scene, false, 0);
        if (ui_rect_height(typing_scene.viewport) != required || !synchronize_probe_shell_layout() ||
            !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
            !probe_present(&g_probe.renderer, kDefaultTheme))
            return false;
        const auto* restored = ui_widget_manager_layout_node(typing_scene.widgets, typing_scene.surface);
        if (!restored || ui_rect_height(restored->rectangle) != typing_surface_height ||
            restored->rectangle.bottom != composer_bottom)
            return false;
    }
    if (!probe_capture_frame(L"composer-typing-expired.bmp"))
        return false;
    log_write(LogLevel::info, "probe",
              "composer_typing_expiry_after_child_refresh_preserves_row_height_passed");
    constexpr Utf8View multiline_composer = utf8_literal("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight");
    if (message_composer_insert_utf8(&g_probe.message_composer, multiline_composer) !=
        MessageComposerResult::changed) {
        return false;
    }
    g_probe.message_composer.focused = true;
    if (!synchronize_probe_shell_layout() ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme)) {
        return false;
    }
    composer_edit =
        message_composer_scene_layout_node(g_probe.message_composer_scene, MessageComposerControl::edit);
    const UiLayoutNode* composer_emoji =
        message_composer_scene_layout_node(g_probe.message_composer_scene, MessageComposerControl::emoji);
    const UiLayoutNode* composer_send =
        message_composer_scene_layout_node(g_probe.message_composer_scene, MessageComposerControl::send);
    const TextEditLayoutOwner& multiline_layout = g_probe.renderer.composer_layout;
    if (!composer_edit || !composer_emoji || !composer_send || !multiline_layout.active ||
        multiline_layout.layout_height <= multiline_layout.height || !(multiline_layout.scroll_y > 0.0f) ||
        multiline_layout.caret.top < composer_edit->content_rectangle.top ||
        multiline_layout.caret.top + multiline_layout.caret.height >
            composer_edit->content_rectangle.bottom + 0.01f ||
        ui_rect_height(composer_emoji->rectangle) != 32.0f ||
        ui_rect_height(composer_send->rectangle) != 32.0f) {
        return false;
    }
    g_probe.failure_stage = 6;
    if (!text_form_probe(g_probe.renderer, g_probe.text_measure_cache))
        return false;
    if (!application_text_form_probe(g_probe.renderer, g_probe.text_measure_cache, g_probe.window))
        return false;
    if (g_probe.text.substitutions == 0) {
        return false;
    }
    g_probe.failure_stage = 7;
    if (!message_timeline_reset(&g_probe.message_view.timeline) ||
        !add_message(message_base + 1, utf8_literal("**Bold** *italic* __underline__ and ~~strike~~.")) ||
        !add_message(message_base + 2,
                     utf8_literal("Font fallback: "
                                  "\xCE\x9A\xCE\xB1\xCE\xBB\xCE\xB7\xCE\xBC\xCE\xAD\xCF\x81\xCE\xB1"
                                  " / "
                                  "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C"
                                  ".")) ||
        !add_message(message_base + 3, utf8_literal("# GPU markdown\n- <@123> <#112> <@&55> "
                                                    "<t:1725021296:R> and "
                                                    "[links](https://chat_state.com)\n"
                                                    "  * nested list\n```cpp\n"
                                                    "if (ready) return 42; // GPU\n```\n"
                                                    "||spoiler||")) ||
        !add_message(message_base + 4, Utf8View{}, MessageContentAvailability::restricted) ||
        !message_view_runtime_reindex(
             &g_probe.message_view, ui_extent_units(kDefaultUiBootstrapMetrics.message_row_height), 1, true)
             .ok() ||
        !message_view_runtime_bind_rich_store(&g_probe.message_view, &g_probe.rich_store).ok()) {
        return false;
    }
    GatewayMessageReaction reactions[1]{};
    reactions[0].emoji_id = 44;
    reactions[0].name = utf8_literal("party");
    reactions[0].count = 7;
    reactions[0].normal_count = 7;
    reactions[0].me = true;
    reactions[0].animated = true;
    GatewayMessageAttachment attachments[1]{};
    attachments[0].id = 108;
    attachments[0].size = 12'345;
    attachments[0].filename = utf8_literal("m49-gpu-preview.jpg");
    attachments[0].content_type = utf8_literal("image/jpeg");
    attachments[0].url = utf8_literal("https://cdn.discordapp.com/attachments/111/108/m49-gpu-preview.jpg"
                                      "?ex=ffffffff&is=1&hm="
                                      "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
    attachments[0].width = 16;
    attachments[0].height = 8;
    attachments[0].has_width = true;
    attachments[0].has_height = true;
    GatewayMessageEmbed embeds[1]{};
    embeds[0].author_name = utf8_literal("<@&55>");
    embeds[0].title = utf8_literal("M38 encoded PNG atlas");
    embeds[0].description = utf8_literal("Bounded Deflate, PNG filters and native GPU expansion");
    embeds[0].footer_text = utf8_literal("Exact D3D11 readback fixture");
    embeds[0].type = GatewayMessageEmbedType::rich;
    embeds[0].color = 0x20B7B7u;
    embeds[0].has_color = true;
    embeds[0].field_count = 1;
    GatewayMessageEmbedField embed_fields[1]{};
    embed_fields[0].name = utf8_literal("Status");
    embed_fields[0].value = utf8_literal("Rendered on GPU");
    GatewayMessageSticker stickers[1]{};
    stickers[0].id = 88;
    stickers[0].name = utf8_literal("Animated sticker placeholder");
    stickers[0].format_type = 2;
    GatewayMessageComponent components[3]{};
    // Application-command responses retain an empty structural action
    // row followed by its visual children. This is the production shape that
    // used to reject the entire text projection and freeze old glyphs.
    components[0].type = 1;
    components[0].parent = 0xFFFFu;
    components[1].type = 10;
    components[1].parent = 0;
    components[1].content = utf8_literal("Components V2 text display");
    components[2].type = 2;
    components[2].parent = 0;
    components[2].style = 1;
    components[2].label = utf8_literal("Finish your submission");
    GatewayMessageEvent rich_message{};
    rich_message.id = message_base + 3;
    rich_message.fields = gateway_message_field_message_reference |
                          gateway_message_field_referenced_message | gateway_message_field_reactions |
                          gateway_message_field_attachments | gateway_message_field_embeds |
                          gateway_message_field_sticker_items | gateway_message_field_components;
    rich_message.reference.message_id = message_base + 2;
    rich_message.reference.channel_id = 111;
    rich_message.reference.author_id = message_base + 2 + 10'000;
    rich_message.reference.author_name = utf8_literal("Render Service");
    rich_message.reference.content = utf8_literal("Font fallback message");
    rich_message.reference.state = GatewayMessageReferenceState::available;
    rich_message.reactions = ConstSlice<GatewayMessageReaction>{reactions, 1};
    rich_message.attachments = ConstSlice<GatewayMessageAttachment>{attachments, 1};
    rich_message.embeds = ConstSlice<GatewayMessageEmbed>{embeds, 1};
    rich_message.embed_fields = ConstSlice<GatewayMessageEmbedField>{embed_fields, 1};
    rich_message.stickers = ConstSlice<GatewayMessageSticker>{stickers, 1};
    rich_message.components = ConstSlice<GatewayMessageComponent>{components, 3};
    const MessageStoreResult rich_base = message_store_find(g_probe.message_view.store, rich_message.id);
    const MessageRecord* rich_record =
        rich_base.ok() ? message_store_resolve(g_probe.message_view.store, rich_base.handle) : nullptr;
    if (!rich_record ||
        !message_rich_store_apply(&g_probe.rich_store, rich_message.id, rich_record->revision, rich_message)
             .ok()) {
        return false;
    }
    g_probe.failure_stage = 15;
    const MediaAtlasGpuResult rejected_upload = renderer_upload_media_rgba8(
        &g_probe.renderer, MediaAssetKey{0, 0, MediaAssetKind::none, 0}, 0, 0, 0, ConstSlice<u8>{});
    if (rejected_upload.ok() || g_probe.renderer.media_gpu.surface) {
        return false;
    }
    PngFixtureResult png_fixture{};
    PngImage png_image{};
    DeflateWorkspace deflate_workspace{};
    if (!build_rgba8_png_fixture(kProbeStickerExtent, kProbeStickerExtent,
                                 Slice<u8>{g_probe.png_encoded, sizeof(g_probe.png_encoded)},
                                 Slice<u8>{g_probe.png_zlib, sizeof(g_probe.png_zlib)}, &png_fixture) ||
        !png_decode_scanlines(ConstSlice<u8>{g_probe.png_encoded, png_fixture.encoded_bytes}, &png_image,
                              &deflate_workspace,
                              Slice<u8>{g_probe.png_scanlines, sizeof(g_probe.png_scanlines)})
             .ok() ||
        !renderer_upload_png_scanlines(&g_probe.renderer, media_asset_sticker(88, 2), png_image,
                                       ConstSlice<u8>{g_probe.png_scanlines, png_image.inflated_bytes})
             .ok()) {
        return false;
    }
    if (!renderer_upload_png_scanlines(&g_probe.renderer, media_asset_custom_emoji(44, true), png_image,
                                       ConstSlice<u8>{g_probe.png_scanlines, png_image.inflated_bytes})
             .ok()) {
        return false;
    }
    ApngFixtureResult apng_fixture{};
    ApngWorkspace apng_workspace{};
    ApngImage apng_image{};
    if (!build_rgba8_apng_fixture(Slice<u8>{g_probe.png_encoded, sizeof(g_probe.png_encoded)},
                                  Slice<u8>{g_probe.png_zlib, sizeof(g_probe.png_zlib)}, &apng_fixture) ||
        !apng_parse(ConstSlice<u8>{g_probe.png_encoded, apng_fixture.encoded_bytes}, &apng_image,
                    &apng_workspace)
             .ok()) {
        return false;
    }
    const MediaAssetKey apng_key = media_asset_sticker(99, 2);
    ApngFrame previous_frame{};
    for (u32 frame_index = 0; frame_index < apng_image.frame_count; ++frame_index) {
        PngImage frame_image{};
        const ApngDecodeResult decoded =
            apng_decode_parsed_frame(ConstSlice<u8>{g_probe.png_encoded, apng_fixture.encoded_bytes},
                                     apng_image, frame_index, &deflate_workspace, &frame_image,
                                     Slice<u8>{g_probe.png_scanlines, sizeof(g_probe.png_scanlines)});
        if (!decoded.ok()) {
            return false;
        }
        const MediaAtlasGpuResult composed =
            frame_index == 0
                ? renderer_begin_apng(
                      &g_probe.renderer, apng_key, static_cast<u16>(apng_image.canvas.width),
                      static_cast<u16>(apng_image.canvas.height), decoded.frame, frame_image,
                      ConstSlice<u8>{g_probe.png_scanlines, frame_image.inflated_bytes})
                : renderer_advance_apng(&g_probe.renderer, apng_key, previous_frame, decoded.frame,
                                        frame_image,
                                        ConstSlice<u8>{g_probe.png_scanlines, frame_image.inflated_bytes});
        if (!composed.ok() || !apng_atlas_pixels_exact(composed.allocation, frame_index)) {
            return false;
        }
        previous_frame = decoded.frame;
    }
    GifFixtureResult gif_fixture{};
    GifWorkspace gif_workspace{};
    GifImage gif_image{};
    if (!build_indexed_gif_fixture(Slice<u8>{g_probe.png_encoded, sizeof(g_probe.png_encoded)},
                                   Slice<u8>{g_probe.png_zlib, sizeof(g_probe.png_zlib)}, &gif_fixture) ||
        !gif_parse(ConstSlice<u8>{g_probe.png_encoded, gif_fixture.encoded_bytes}, &gif_image,
                   &gif_workspace)
             .ok()) {
        return false;
    }
    const MediaAssetKey gif_key = media_asset_sticker(100, 4);
    GifFrame previous_gif_frame{};
    for (u32 frame_index = 0; frame_index < gif_image.frame_count; ++frame_index) {
        const GifDecodeResult decoded = gif_decode_parsed_frame(
            ConstSlice<u8>{g_probe.png_encoded, gif_fixture.encoded_bytes}, gif_image, frame_index,
            &gif_workspace, Slice<u8>{g_probe.png_scanlines, sizeof(g_probe.png_scanlines)});
        if (!decoded.ok()) {
            return false;
        }
        const MediaAtlasGpuResult composed =
            frame_index == 0
                ? renderer_begin_gif(&g_probe.renderer, gif_key, static_cast<u16>(gif_image.canvas_width),
                                     static_cast<u16>(gif_image.canvas_height), decoded.frame,
                                     decoded.palette, ConstSlice<u8>{g_probe.png_scanlines, decoded.pixels})
                : renderer_advance_gif(&g_probe.renderer, gif_key, previous_gif_frame, decoded.frame,
                                       decoded.palette,
                                       ConstSlice<u8>{g_probe.png_scanlines, decoded.pixels});
        const AnimationFrameCacheDescriptor cached_frame{AnimationFrameCacheKey{gif_key, frame_index, 7, 0},
                                                         AnimationFrameCacheControl{.gif = decoded.frame},
                                                         100'000,
                                                         gif_image.background_rgba,
                                                         static_cast<u16>(gif_image.canvas_width),
                                                         static_cast<u16>(gif_image.canvas_height),
                                                         AnimationFrameCacheKind::gif,
                                                         {0, 0, 0}};
        if (!composed.ok() || !gif_atlas_pixels_exact(composed.allocation, frame_index) ||
            !renderer_capture_animation_frame(&g_probe.renderer, cached_frame)) {
            return false;
        }
        previous_gif_frame = decoded.frame;
    }
    AnimationFrameCacheResult restored_gif{};
    GlyphAtlasAllocation selected_gif{};
    const MediaAtlasResult live_gif = media_atlas_find(&g_probe.media_atlas, gif_key);
    if (!renderer_restore_animation_frame(&g_probe.renderer, AnimationFrameCacheKey{gif_key, 0, 7, 0},
                                          &restored_gif) ||
        !restored_gif.ok() || restored_gif.duration_us != 100'000 ||
        restored_gif.kind != AnimationFrameCacheKind::gif ||
        !animation_frame_cache_gpu_resolve(&g_probe.renderer.animation_gpu, live_gif, &selected_gif) ||
        selected_gif.handle.slot != restored_gif.allocation.handle.slot ||
        selected_gif.handle.generation != restored_gif.allocation.handle.generation ||
        !renderer_materialize_animation_frame(&g_probe.renderer, gif_key) ||
        !gif_atlas_pixels_exact(media_atlas_find(&g_probe.media_atlas, gif_key), 0)) {
        return false;
    }
    AnimatedWebpFixtureResult animated_webp_fixture{};
    AnimatedWebpImage animated_webp_image{};
    if (!build_lossless_animated_webp_fixture(Slice<u8>{g_probe.png_encoded, sizeof(g_probe.png_encoded)},
                                              &animated_webp_fixture) ||
        !animated_webp_parse(ConstSlice<u8>{g_probe.png_encoded, animated_webp_fixture.encoded_bytes},
                             &animated_webp_image, &g_probe.animated_webp_workspace)
             .ok()) {
        return false;
    }
    const MediaAssetKey animated_webp_key = media_asset_custom_emoji(101, true);
    AnimatedWebpFrame previous_webp_frame{};
    for (u32 frame_index = 0; frame_index < animated_webp_image.frame_count; ++frame_index) {
        const AnimatedWebpDecodeResult decoded = animated_webp_decode_frame_rgba8(
            ConstSlice<u8>{g_probe.png_encoded, animated_webp_fixture.encoded_bytes}, frame_index,
            &animated_webp_image, &g_probe.animated_webp_workspace, &g_probe.vp8l_workspace,
            Slice<u8>{g_probe.png_scanlines, sizeof(g_probe.png_scanlines)});
        if (!decoded.ok()) {
            return false;
        }
        const MediaAtlasGpuResult composed =
            frame_index == 0
                ? renderer_begin_animated_webp(
                      &g_probe.renderer, animated_webp_key, animated_webp_image.width,
                      animated_webp_image.height, animated_webp_image.background_rgba, decoded.frame,
                      ConstSlice<u8>{g_probe.png_scanlines, decoded.frame_decode.rgba_bytes})
                : renderer_advance_animated_webp(
                      &g_probe.renderer, animated_webp_key, animated_webp_image.background_rgba,
                      previous_webp_frame, decoded.frame,
                      ConstSlice<u8>{g_probe.png_scanlines, decoded.frame_decode.rgba_bytes});
        if (!composed.ok() || !animated_webp_atlas_pixels_exact(composed.allocation, frame_index)) {
            return false;
        }
        previous_webp_frame = decoded.frame;
    }
    AnimatedWebpFixtureResult mixed_webp_fixture{};
    AnimatedWebpImage mixed_webp_image{};
    if (!build_mixed_animated_webp_fixture(Slice<u8>{g_probe.png_encoded, sizeof(g_probe.png_encoded)},
                                           &mixed_webp_fixture) ||
        !animated_webp_parse(ConstSlice<u8>{g_probe.png_encoded, mixed_webp_fixture.encoded_bytes},
                             &mixed_webp_image, &g_probe.animated_webp_workspace)
             .ok()) {
        return false;
    }
    const MediaAssetKey mixed_webp_key = media_asset_custom_emoji(107, true);
    previous_webp_frame = AnimatedWebpFrame{};
    g_probe.failure_stage = 29;
    for (u32 frame_index = 0; frame_index < mixed_webp_image.frame_count; ++frame_index) {
        Vp8Keyframe vp8_frame{};
        Vp8ControlHeader vp8_header{};
        const AnimatedWebpDecodeResult decoded = animated_webp_decode_frame(
            ConstSlice<u8>{g_probe.png_encoded, mixed_webp_fixture.encoded_bytes}, frame_index,
            &mixed_webp_image, &g_probe.animated_webp_workspace, &g_probe.vp8l_workspace,
            &g_probe.lossy_webp_workspace, &vp8_frame, &vp8_header,
            Slice<u8>{g_probe.png_scanlines, sizeof(g_probe.png_scanlines)},
            Slice<u8>{g_probe.png_scanlines, 64},
            Slice<u8>{g_probe.png_scanlines + 64, sizeof(g_probe.png_scanlines) - 64});
        if (!decoded.ok()) {
            return false;
        }
        const bool lossless = decoded.frame.codec == AnimatedWebpCodec::vp8l_lossless;
        const ConstSlice<u8> pixels =
            lossless ? ConstSlice<u8>{g_probe.png_scanlines, decoded.frame_decode.rgba_bytes}
            : decoded.frame.has_alpha
                ? ConstSlice<u8>{g_probe.png_scanlines + 64, decoded.lossy_decode.alpha_bytes}
                : ConstSlice<u8>{};
        MediaAtlasGpuResult composed{};
        if (lossless) {
            composed =
                frame_index == 0
                    ? renderer_begin_animated_webp(&g_probe.renderer, mixed_webp_key,
                                                   mixed_webp_image.width, mixed_webp_image.height,
                                                   mixed_webp_image.background_rgba, decoded.frame, pixels)
                    : renderer_advance_animated_webp(&g_probe.renderer, mixed_webp_key,
                                                     mixed_webp_image.background_rgba, previous_webp_frame,
                                                     decoded.frame, pixels);
        } else {
            composed = frame_index == 0
                           ? renderer_begin_animated_webp_vp8(
                                 &g_probe.renderer, mixed_webp_key, mixed_webp_image.width,
                                 mixed_webp_image.height, mixed_webp_image.background_rgba, decoded.frame,
                                 vp8_frame, vp8_header, g_probe.lossy_webp_workspace.modes,
                                 g_probe.lossy_webp_workspace.coefficients, pixels)
                           : renderer_advance_animated_webp_vp8(
                                 &g_probe.renderer, mixed_webp_key, mixed_webp_image.background_rgba,
                                 previous_webp_frame, decoded.frame, vp8_frame, vp8_header,
                                 g_probe.lossy_webp_workspace.modes,
                                 g_probe.lossy_webp_workspace.coefficients, pixels);
        }
        if (!composed.ok() || !mixed_animated_webp_atlas_pixels_exact(composed.allocation, frame_index)) {
            return false;
        }
        previous_webp_frame = decoded.frame;
    }
    JpegFixtureResult jpeg_fixture{};
    JpegImage jpeg_image{};
    u8 visible_oriented_jpeg[320]{};
    u32 visible_oriented_jpeg_bytes = 0;
    g_probe.failure_stage = 30;
    if (!build_baseline_jpeg_fixture(Slice<u8>{g_probe.png_encoded, sizeof(g_probe.png_encoded)}, true,
                                     &jpeg_fixture) ||
        !build_jpeg_exif_orientation_fixture(
            ConstSlice<u8>{g_probe.png_encoded, jpeg_fixture.encoded_bytes},
            Slice<u8>{visible_oriented_jpeg, sizeof(visible_oriented_jpeg)},
            static_cast<u8>(JpegOrientation::right_top), false, &visible_oriented_jpeg_bytes) ||
        !rebuild_probe_content_text()) {
        return false;
    }
    const u32 rich_height_before_orientation =
        ui_variable_list_item_units(g_probe.message_view.list, rich_base.order_index);
    const JpegDecodeResult jpeg_decoded =
        jpeg_decode_coefficients(ConstSlice<u8>{visible_oriented_jpeg, visible_oriented_jpeg_bytes},
                                 &jpeg_image, &g_probe.jpeg_workspace);
    const MediaAtlasGpuResult jpeg_uploaded =
        jpeg_decoded.ok() ? renderer_upload_jpeg(&g_probe.renderer,
                                                 media_asset_attachment(108, MediaAttachmentFormat::png,
                                                                        MediaResolutionTier::px64),
                                                 jpeg_image, g_probe.jpeg_workspace.coefficients)
                          : MediaAtlasGpuResult{};
    bool attachment_dimensions_changed = false;
    if (!jpeg_decoded.ok() || jpeg_image.orientation != JpegOrientation::right_top) {
        g_probe.failure_stage = 60;
        return false;
    }
    if (!jpeg_uploaded.ok() || !g_probe.renderer.media_gpu.preview_surface) {
        g_probe.failure_stage = 61;
        return false;
    }
    if (!media_atlas_find(
             &g_probe.preview_media_atlas,
             media_asset_attachment(108, MediaAttachmentFormat::png, MediaResolutionTier::px64))
             .ok()) {
        g_probe.failure_stage = 62;
        return false;
    }
    if (!jpeg_atlas_pixels_exact(jpeg_uploaded.allocation, 8, 16, true)) {
        g_probe.failure_stage = 63;
        return false;
    }
    if (!message_rich_store_set_attachment_dimensions(&g_probe.rich_store, rich_message.id,
                                                      rich_record->revision, attachments[0].id, 8, 16,
                                                      &attachment_dimensions_changed) ||
        !attachment_dimensions_changed || !rebuild_probe_content_text() ||
        ui_variable_list_item_units(g_probe.message_view.list, rich_base.order_index) <=
            rich_height_before_orientation) {
        g_probe.failure_stage = 64;
        return false;
    }
    JpegImage invalid_jpeg_image = jpeg_image;
    ++invalid_jpeg_image.components[0].block_columns;
    if (renderer_upload_jpeg(&g_probe.renderer, media_asset_attachment(110), invalid_jpeg_image,
                             g_probe.jpeg_workspace.coefficients)
            .ok()) {
        return false;
    }
    g_probe.failure_stage = 31;
    JpegImage jpeg_420_image{};
    JpegDecodeResult jpeg_420_decoded{};
    const MediaAtlasGpuResult jpeg_420_uploaded = probe_upload_jpeg(
        ConstSlice<u8>{kJpeg420Reference, sizeof(kJpeg420Reference)}, 109, &jpeg_420_image, &jpeg_420_decoded);
    if (!jpeg_420_decoded.ok() || !jpeg_420_uploaded.ok() ||
        !jpeg_atlas_pixels_reference_tolerance(
            jpeg_420_uploaded.allocation, 16, 16,
            ConstSlice<u8>{reinterpret_cast<const u8*>(kJpeg420ReferenceRgb),
                           sizeof(kJpeg420ReferenceRgb)})) {
        return false;
    }
    g_probe.failure_stage = 32;
    JpegImage progressive_jpeg_image{};
    JpegDecodeResult progressive_jpeg_decoded{};
    const MediaAtlasGpuResult progressive_jpeg_uploaded =
        probe_upload_jpeg(ConstSlice<u8>{reinterpret_cast<const u8*>(kJpegProgressive420Reference),
                                         kJpegProgressive420ReferenceBytes},
                          111, &progressive_jpeg_image, &progressive_jpeg_decoded);
    if (!progressive_jpeg_decoded.ok() || progressive_jpeg_decoded.decoded_scans != 10 ||
        !progressive_jpeg_uploaded.ok() ||
        !jpeg_atlas_pixels_reference_tolerance(
            progressive_jpeg_uploaded.allocation, 16, 16,
            ConstSlice<u8>{reinterpret_cast<const u8*>(kJpegProgressive420ReferenceRgb),
                           kJpegProgressive420ReferenceRgbBytes})) {
        return false;
    }
    g_probe.failure_stage = 33;
    JpegImage cmyk_jpeg_image{};
    JpegDecodeResult cmyk_jpeg_decoded{};
    const MediaAtlasGpuResult cmyk_jpeg_uploaded =
        probe_upload_jpeg(ConstSlice<u8>{kJpegCmykAdobeReference, sizeof(kJpegCmykAdobeReference)}, 112,
                          &cmyk_jpeg_image, &cmyk_jpeg_decoded);
    if (!cmyk_jpeg_decoded.ok() || !cmyk_jpeg_uploaded.ok() ||
        !jpeg_atlas_pixels_reference_tolerance(
            cmyk_jpeg_uploaded.allocation, 16, 16,
            ConstSlice<u8>{kJpegCmykAdobeReferenceRgb, sizeof(kJpegCmykAdobeReferenceRgb)})) {
        return false;
    }
    g_probe.failure_stage = 34;
    JpegImage ycck_jpeg_image{};
    JpegDecodeResult ycck_jpeg_decoded{};
    const MediaAtlasGpuResult ycck_jpeg_uploaded =
        probe_upload_jpeg(ConstSlice<u8>{kJpegYcckProgressiveReference, sizeof(kJpegYcckProgressiveReference)},
                          113, &ycck_jpeg_image, &ycck_jpeg_decoded);
    if (!ycck_jpeg_decoded.ok() || ycck_jpeg_decoded.decoded_scans <= 1 || !ycck_jpeg_uploaded.ok() ||
        !jpeg_atlas_pixels_reference_tolerance(
            ycck_jpeg_uploaded.allocation, 16, 16,
            ConstSlice<u8>{kJpegYcckProgressiveReferenceRgb, sizeof(kJpegYcckProgressiveReferenceRgb)})) {
        return false;
    }
    g_probe.failure_stage = 35;
    for (u8 orientation_value = 1; orientation_value <= 8; ++orientation_value) {
        u32 oriented_bytes = 0;
        if (!build_jpeg_exif_orientation_fixture(
                ConstSlice<u8>{kJpeg420Reference, sizeof(kJpeg420Reference)},
                Slice<u8>{g_probe.png_encoded, sizeof(g_probe.png_encoded)}, orientation_value,
                (orientation_value & 1u) == 0, &oriented_bytes)) {
            return false;
        }
        JpegImage oriented_image{};
        JpegDecodeResult oriented_decoded{};
        const MediaAtlasGpuResult oriented_uploaded =
            probe_upload_jpeg(ConstSlice<u8>{g_probe.png_encoded, oriented_bytes}, 114 + orientation_value,
                              &oriented_image, &oriented_decoded);
        const JpegOrientation orientation = static_cast<JpegOrientation>(orientation_value);
        if (!oriented_decoded.ok() || oriented_image.orientation != orientation ||
            !oriented_uploaded.ok() ||
            !jpeg_atlas_pixels_reference_tolerance(
                oriented_uploaded.allocation, 16, 16,
                ConstSlice<u8>{reinterpret_cast<const u8*>(kJpeg420ReferenceRgb),
                               sizeof(kJpeg420ReferenceRgb)},
                orientation)) {
            return false;
        }
    }
    g_probe.failure_stage = 36;
    u8 rectangular_jpeg[256]{};
    u8 rectangular_oriented[320]{};
    JpegFixtureResult rectangular_fixture{};
    u32 rectangular_oriented_bytes = 0;
    JpegImage rectangular_image{};
    if (!build_baseline_jpeg_fixture(Slice<u8>{rectangular_jpeg, sizeof(rectangular_jpeg)}, true,
                                     &rectangular_fixture) ||
        !build_jpeg_exif_orientation_fixture(
            ConstSlice<u8>{rectangular_jpeg, rectangular_fixture.encoded_bytes},
            Slice<u8>{rectangular_oriented, sizeof(rectangular_oriented)},
            static_cast<u8>(JpegOrientation::right_top), false, &rectangular_oriented_bytes)) {
        return false;
    }
    JpegDecodeResult rectangular_decoded{};
    const MediaAtlasGpuResult rectangular_uploaded =
        probe_upload_jpeg(ConstSlice<u8>{rectangular_oriented, rectangular_oriented_bytes}, 123,
                          &rectangular_image, &rectangular_decoded);
    if (!rectangular_decoded.ok() || rectangular_image.width != 16 || rectangular_image.height != 8 ||
        rectangular_image.orientation != JpegOrientation::right_top || !rectangular_uploaded.ok() ||
        rectangular_uploaded.allocation.width != 8 || rectangular_uploaded.allocation.height != 16 ||
        !jpeg_atlas_pixels_exact(rectangular_uploaded.allocation, 8, 16)) {
        return false;
    }
    g_probe.failure_stage = 37;
    JpegIccFixtureResult icc_fixture{};
    JpegImage icc_image{};
    JpegDecodeResult icc_decoded{};
    if (!build_jpeg_icc_matrix_trc_fixture(ConstSlice<u8>{kJpeg420Reference, sizeof(kJpeg420Reference)},
                                           Slice<u8>{g_probe.png_encoded, sizeof(g_probe.png_encoded)},
                                           JpegIccFixtureCurve::parametric_srgb, true, true,
                                           &icc_fixture)) {
        return false;
    }
    const MediaAtlasGpuResult icc_uploaded =
        probe_upload_jpeg(ConstSlice<u8>{g_probe.png_encoded, icc_fixture.encoded_bytes}, 124, &icc_image,
                          &icc_decoded);
    if (!icc_decoded.ok() || !jpeg_icc_applied(icc_image) || !icc_uploaded.ok()) {
        return false;
    }
    g_probe.failure_stage = 39;
    if (!jpeg_atlas_pixels_reference_tolerance(
            icc_uploaded.allocation, 16, 16,
            ConstSlice<u8>{reinterpret_cast<const u8*>(kJpeg420ReferenceRgb), sizeof(kJpeg420ReferenceRgb)},
            JpegOrientation::top_left, true)) {
        return false;
    }
    g_probe.failure_stage = 38;
    if (!build_jpeg_icc_matrix_trc_fixture(ConstSlice<u8>{kJpeg420Reference, sizeof(kJpeg420Reference)},
                                           Slice<u8>{g_probe.png_encoded, sizeof(g_probe.png_encoded)},
                                           JpegIccFixtureCurve::sampled_srgb, false, false, &icc_fixture)) {
        return false;
    }
    const MediaAtlasGpuResult icc_table_uploaded =
        probe_upload_jpeg(ConstSlice<u8>{g_probe.png_encoded, icc_fixture.encoded_bytes}, 125, &icc_image,
                          &icc_decoded);
    if (!icc_decoded.ok() || !jpeg_icc_applied(icc_image) ||
        icc_image.tables->icc->profile.entry_count != 195 || !icc_table_uploaded.ok() ||
        !jpeg_atlas_pixels_reference_tolerance(
            icc_table_uploaded.allocation, 16, 16,
            ConstSlice<u8>{reinterpret_cast<const u8*>(kJpeg420ReferenceRgb), sizeof(kJpeg420ReferenceRgb)},
            JpegOrientation::top_left, true)) {
        return false;
    }
    g_probe.failure_stage = 40;
    if (!build_jpeg_icc_lut_fixture(ConstSlice<u8>{kJpeg420Reference, sizeof(kJpeg420Reference)},
                                    Slice<u8>{g_probe.png_encoded, sizeof(g_probe.png_encoded)},
                                    JpegIccLutFixtureSpace::rgb, JpegIccLutFixturePrecision::mft2, true,
                                    &icc_fixture)) {
        return false;
    }
    const MediaAtlasGpuResult icc_lut_rgb_uploaded =
        probe_upload_jpeg(ConstSlice<u8>{g_probe.png_encoded, icc_fixture.encoded_bytes}, 126, &icc_image,
                          &icc_decoded);
    if (!icc_decoded.ok() || !jpeg_icc_applied(icc_image) ||
        icc_image.tables->icc->profile.transform_kind != JpegIccTransformKind::lut ||
        icc_image.tables->icc->profile.entry_count != 36 || !icc_lut_rgb_uploaded.ok() ||
        !jpeg_icc_rgb_lut_pixels_tolerance(icc_lut_rgb_uploaded.allocation,
                                           ConstSlice<u8>{reinterpret_cast<const u8*>(kJpeg420ReferenceRgb),
                                                          sizeof(kJpeg420ReferenceRgb)})) {
        return false;
    }
    g_probe.failure_stage = 41;
    if (!build_jpeg_icc_lut_fixture(
            ConstSlice<u8>{kJpegCmykAdobeReference, sizeof(kJpegCmykAdobeReference)},
            Slice<u8>{g_probe.png_encoded, sizeof(g_probe.png_encoded)}, JpegIccLutFixtureSpace::cmyk,
            JpegIccLutFixturePrecision::mft2, true, &icc_fixture)) {
        return false;
    }
    const MediaAtlasGpuResult icc_lut_cmyk_uploaded =
        probe_upload_jpeg(ConstSlice<u8>{g_probe.png_encoded, icc_fixture.encoded_bytes}, 127, &icc_image,
                          &icc_decoded);
    if (!icc_decoded.ok() || !jpeg_icc_applied(icc_image) ||
        icc_image.tables->icc->profile.data_color_space != JpegIccDataColorSpace::cmyk ||
        icc_image.tables->icc->profile.entry_count != 62 || !icc_lut_cmyk_uploaded.ok() ||
        !jpeg_atlas_differs_from_reference(
            icc_lut_cmyk_uploaded.allocation, 16, 16,
            ConstSlice<u8>{kJpegCmykAdobeReferenceRgb, sizeof(kJpegCmykAdobeReferenceRgb)}, 8)) {
        return false;
    }
    g_probe.failure_stage = 42;
    if (!build_jpeg_icc_lut_fixture(
            ConstSlice<u8>{kJpegYcckProgressiveReference, sizeof(kJpegYcckProgressiveReference)},
            Slice<u8>{g_probe.png_encoded, sizeof(g_probe.png_encoded)}, JpegIccLutFixtureSpace::cmyk,
            JpegIccLutFixturePrecision::mft2, false, &icc_fixture)) {
        return false;
    }
    const MediaAtlasGpuResult icc_lut_ycck_uploaded =
        probe_upload_jpeg(ConstSlice<u8>{g_probe.png_encoded, icc_fixture.encoded_bytes}, 128, &icc_image,
                          &icc_decoded);
    if (!icc_decoded.ok() || icc_image.color_transform != JpegColorTransform::ycck ||
        !jpeg_icc_applied(icc_image) || !icc_lut_ycck_uploaded.ok() ||
        !jpeg_atlas_differs_from_reference(
            icc_lut_ycck_uploaded.allocation, 16, 16,
            ConstSlice<u8>{kJpegYcckProgressiveReferenceRgb, sizeof(kJpegYcckProgressiveReferenceRgb)},
            8)) {
        return false;
    }
    g_probe.failure_stage = 43;
    u8 gray_jpeg[192]{};
    JpegFixtureResult gray_fixture{};
    if (!build_grayscale_jpeg_fixture(Slice<u8>{gray_jpeg, sizeof(gray_jpeg)}, &gray_fixture) ||
        !build_jpeg_icc_gray_trc_fixture(ConstSlice<u8>{gray_jpeg, gray_fixture.encoded_bytes},
                                         Slice<u8>{g_probe.png_encoded, sizeof(g_probe.png_encoded)}, true,
                                         true, &icc_fixture)) {
        return false;
    }
    const MediaAtlasGpuResult icc_gray_uploaded =
        probe_upload_jpeg(ConstSlice<u8>{g_probe.png_encoded, icc_fixture.encoded_bytes}, 129, &icc_image,
                          &icc_decoded);
    u8 gray_expected[3]{};
    probe_icc_lab_to_srgb(128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f, gray_expected);
    if (!icc_decoded.ok() || !jpeg_icc_applied(icc_image) ||
        icc_image.tables->icc->profile.transform_kind != JpegIccTransformKind::gray_trc ||
        !icc_gray_uploaded.ok() ||
        !jpeg_atlas_uniform_rgb_tolerance(icc_gray_uploaded.allocation, 8, 8, gray_expected, 2)) {
        return false;
    }
    if (!vp8_gpu_reconstruction_exact()) {
        return false;
    }
    g_probe.failure_stage = 28;
    if (!vp8_real_fixture_gpu_exact()) {
        return false;
    }
    g_probe.failure_stage = 8;
    if (!rebuild_probe_content_text()) {
        return false;
    }
    const u64 rich_row_top =
        ui_variable_list_prefix_units(g_probe.message_view.list, rich_base.order_index);
    if (!message_timeline_set_scroll(&g_probe.message_view.timeline, rich_row_top, false) ||
        !rebuild_probe_content_text()) {
        return false;
    }
    g_probe.failure_stage = 9;
    u32 analytic_bullets = 0;
    for (u32 index = 0; index < g_probe.text.quad_count; ++index) {
        analytic_bullets += g_probe.text.quads[index].u0 == -3.0f ? 1u : 0u;
    }
    g_probe.failure_stage = 46;
    const MessageTextCommand* selectable = nullptr;
    for (u32 index = 0; index < g_probe.message_text.command_count; ++index) {
        const MessageTextCommand& command = g_probe.message_text.commands[index];
        const UiRect visible_clip = ui_rect_intersection(command.clip, g_probe.message_scene.viewport);
        const float visible_left = maximum(command.rectangle.left, visible_clip.left);
        const float visible_top = maximum(command.rectangle.top, visible_clip.top);
        const float visible_right = minimum(command.rectangle.right, visible_clip.right);
        const float visible_bottom = minimum(command.rectangle.bottom, visible_clip.bottom);
        if (command.measurement_kind == MessageTextMeasurementKind::content && command.text.bytes != 0 &&
            command.text.bytes <= kRendererContentTextUnitCapacity && ui_rect_has_area(command.rectangle) &&
            ui_rect_has_area(visible_clip) && visible_right > visible_left &&
            visible_bottom > visible_top) {
            selectable = &command;
            break;
        }
    }
    if (!selectable) {
        return false;
    }
    g_probe.failure_stage = 47;
    const UiRect selectable_clip = ui_rect_intersection(selectable->clip, g_probe.message_scene.viewport);
    const float selectable_x = (maximum(selectable->rectangle.left, selectable_clip.left) +
                                minimum(selectable->rectangle.right, selectable_clip.right)) *
                               0.5f;
    const float selectable_y = (maximum(selectable->rectangle.top, selectable_clip.top) +
                                minimum(selectable->rectangle.bottom, selectable_clip.bottom)) *
                               0.5f;
    const float message_scale = static_cast<float>(g_probe.renderer.dpi) / 96.0f;
    message_hit_test_cache_invalidate(&g_probe.renderer.message_hit_test);
    const u32 selection_builds_before = g_probe.renderer.message_hit_test.builds;
    const u32 selection_hits_before = g_probe.renderer.message_hit_test.hits;
    const u32 selection_rejected_before = g_probe.renderer.message_hit_test.rejected;
    const RendererMessageTextPositionHit selection_hit = renderer_message_text_position_hit_test(
        g_probe.renderer, static_cast<i32>(selectable_x * message_scale + 0.5f),
        static_cast<i32>(selectable_y * message_scale + 0.5f));
    const RendererMessageTextPositionHit retained_selection_hit = renderer_message_text_position_hit_test(
        g_probe.renderer, static_cast<i32>(selectable_x * message_scale + 0.5f),
        static_cast<i32>(selectable_y * message_scale + 0.5f));
    const u32 unselected_quad_count = g_probe.text.quad_count;
    if (!selection_hit.valid || !retained_selection_hit.valid ||
        retained_selection_hit.message_id != selection_hit.message_id ||
        retained_selection_hit.command_identity != selection_hit.command_identity ||
        retained_selection_hit.command_index != selection_hit.command_index ||
        retained_selection_hit.byte_offset != selection_hit.byte_offset ||
        g_probe.renderer.message_hit_test.builds != selection_builds_before + 1u ||
        g_probe.renderer.message_hit_test.hits != selection_hits_before + 1u ||
        g_probe.renderer.message_hit_test.rejected != selection_rejected_before) {
        return false;
    }
    g_probe.failure_stage = 48;
    const u32 selection_content_builds = g_probe.renderer.content_text_retention.builds;
    const u32 selection_content_hits = g_probe.renderer.content_text_retention.hits;
    const u32 selection_text_rebuilds = g_probe.text.rebuilds;
    renderer_set_message_text_selection(&g_probe.renderer, selection_hit, 0, selection_hit.text_bytes);
    if (!rebuild_probe_content_text() || g_probe.text.quad_count <= unselected_quad_count ||
        g_probe.renderer.content_text_retention.builds != selection_content_builds + 1u ||
        g_probe.renderer.content_text_retention.hits != selection_content_hits ||
        g_probe.text.rebuilds != selection_text_rebuilds + 1u) {
        log_u64(LogLevel::error, "probe", "selection_unselected_quads", unselected_quad_count);
        log_u64(LogLevel::error, "probe", "selection_selected_quads", g_probe.text.quad_count);
        log_u64(LogLevel::error, "probe", "selection_hit_text_bytes", selection_hit.text_bytes);
        log_u64(LogLevel::error, "probe", "selection_hit_command_index", selection_hit.command_index);
        log_u64(LogLevel::error, "probe", "selection_hit_identity", selection_hit.command_identity);
        log_u64(LogLevel::error, "probe", "selection_renderer_identity",
                g_probe.renderer.message_text_selection_command_identity);
        if (selection_hit.command_index < g_probe.message_text.command_count) {
            const MessageTextCommand& selected_command =
                g_probe.message_text.commands[selection_hit.command_index];
            log_u64(LogLevel::error, "probe", "selection_command_identity",
                    selected_command.measurement_identity);
            log_u64(LogLevel::error, "probe", "selection_command_message", selected_command.message_id);
            log_u64(LogLevel::error, "probe", "selection_renderer_message",
                    g_probe.renderer.message_text_selection_message_id);
        }
        log_u64(LogLevel::error, "probe", "selection_content_builds_before", selection_content_builds);
        log_u64(LogLevel::error, "probe", "selection_content_builds_after",
                g_probe.renderer.content_text_retention.builds);
        log_u64(LogLevel::error, "probe", "selection_content_hits_before", selection_content_hits);
        log_u64(LogLevel::error, "probe", "selection_content_hits_after",
                g_probe.renderer.content_text_retention.hits);
        log_u64(LogLevel::error, "probe", "selection_text_rebuilds_before", selection_text_rebuilds);
        log_u64(LogLevel::error, "probe", "selection_text_rebuilds_after", g_probe.text.rebuilds);
        return false;
    }
    const u32 retained_builds = g_probe.renderer.message_hit_test.builds;
    const u32 retained_hits = g_probe.renderer.message_hit_test.hits;
    const RendererMessageTextPositionHit post_rebuild_hit = renderer_message_text_position_hit_test(
        g_probe.renderer, static_cast<i32>(selectable_x * message_scale + 0.5f),
        static_cast<i32>(selectable_y * message_scale + 0.5f));
    if (!post_rebuild_hit.valid || post_rebuild_hit.command_identity != selection_hit.command_identity ||
        g_probe.renderer.message_hit_test.builds != retained_builds ||
        g_probe.renderer.message_hit_test.hits != retained_hits + 1u) {
        return false;
    }
    g_probe.failure_stage = 49;
    const u32 clear_content_builds = g_probe.renderer.content_text_retention.builds;
    const u32 clear_content_hits = g_probe.renderer.content_text_retention.hits;
    const u32 clear_text_rebuilds = g_probe.text.rebuilds;
    renderer_clear_message_text_selection(&g_probe.renderer);
    if (!rebuild_probe_content_text() || g_probe.text.quad_count != unselected_quad_count ||
        g_probe.renderer.content_text_retention.builds != clear_content_builds + 1u ||
        g_probe.renderer.content_text_retention.hits != clear_content_hits ||
        g_probe.text.rebuilds != clear_text_rebuilds + 1u) {
        return false;
    }
    g_probe.failure_stage = 9;
    if (g_probe.message_text.markdown_parses == 0 || g_probe.message_text.markdown_transformations == 0 ||
        g_probe.message_text.markdown_spans_emitted == 0 ||
        g_probe.message_text.markdown_syntax_tokens == 0 ||
        g_probe.message_text.markdown_syntax_styled_bytes == 0 ||
        g_probe.message_text.markdown_list_items < 2 ||
        g_probe.message_text.markdown_resolved_entities < 3 ||
        g_probe.message_text.markdown_timestamps == 0) {
        return false;
    }
    g_probe.failure_stage = 10;
    if (g_probe.text.content_semantic_count < 6) {
        return false;
    }
    g_probe.failure_stage = 14;
    if (g_probe.text.content_hit_region_count < 6) {
        return false;
    }
    g_probe.failure_stage = 11;
    if (analytic_bullets != 2) {
        return false;
    }
    g_probe.failure_stage = 12;
    if (g_probe.message_text.markdown_syntax_truncated != 0 ||
        g_probe.message_text.markdown_rejected != 0) {
        return false;
    }
    g_probe.failure_stage = 13;
    if (g_probe.message_text.rich.replies != 1 || g_probe.message_text.rich.reactions != 1 ||
        g_probe.message_text.rich.attachments != 1 || g_probe.message_text.rich.embeds != 1 ||
        g_probe.message_text.rich.stickers != 1 || g_probe.message_text.rich.components != 2 ||
        g_probe.message_text.rich.skipped_components != 1 || !probe_has_role_mention_colors(0xC46BEFu)) {
        return false;
    }
    g_probe.failure_stage = 0;
    return true;
}

#include "thai_wrapped_probe.inc"
#include "catalog_apng_probe.inc"

int run_probe() noexcept {
    struct SceneRelease {
        ~SceneRelease() noexcept {
            message_scene_release(&g_probe.message_scene);
        }
    } scene_release;
    (void)log_open();
    const bool native_bootstrap = probe_option_enabled(L"MAICORD_NATIVE_BOOTSTRAP_PROBE");
    // The client's startup order: the device and draw pipeline start on a
    // worker before the model and window exist.
    renderer_prestart();
    if (!message_view_runtime_initialize(
             &g_probe.message_view, ui_extent_units(kDefaultUiBootstrapMetrics.message_row_height), 1, true)
             .ok() ||
        !create_probe_window() || !initialize_probe_renderer()) {
        renderer_prestart_cancel();
        return 2;
    }
    g_probe.renderer_ready = true;
    if (g_probe.renderer.glyph_text.title.run_count || g_probe.renderer.glyph_text.title_quad_count ||
        g_probe.renderer.glyph_text.cache_builds || g_probe.renderer.glyph_text.gpu.dispatch_count) {
        return finish_probe(246);
    }
    // The prestarted device, draw pipeline and glyph atlas were adopted, not
    // recreated on this thread; the content glyph ring exists before content
    // text binds, so binding it never reallocates the ring.
    if (g_probe.renderer.startup_prestart != 7 ||
        g_probe.renderer.pipeline.glyph_instance_capacity != kRendererContentGlyphCapacity) {
        return finish_probe(229);
    }
    if (probe_option_enabled(L"MAICORD_COLR_BINDING_PROBE")) {
        const bool valid = colr_binding_probe(g_probe.renderer.graphics.device, g_probe.renderer.graphics.context);
        return finish_probe(valid ? 0 : 245);
    }
    const bool device_ready = device_owner_ready(g_probe.renderer);
    const bool pipeline_ready = draw_pipeline_ready(g_probe.renderer);
    const bool glyph_ready = glyph_text_owner_ready(g_probe.renderer);
    if (!device_ready || !pipeline_ready || !glyph_ready) {
        log_write(LogLevel::error, "probe", "post_bootstrap_readiness_failed");
        log_u64(LogLevel::error, "probe", "bootstrap_device_ready", device_ready);
        log_u64(LogLevel::error, "probe", "bootstrap_pipeline_ready", pipeline_ready);
        log_u64(LogLevel::error, "probe", "bootstrap_glyph_ready", glyph_ready);
        return finish_probe(9);
    }
    if (native_bootstrap) {
        const auto& system = g_probe.renderer.glyph_text.system;
        if (!system.native_fonts || !system.native_text || !system.native_format ||
            !g_probe.renderer.glyph_text.color_glyphs) {
            return finish_probe(243);
        }
        log_write(LogLevel::info, "probe", "native_renderer_bootstrap_passed");
    }
    const bool caption_requested = probe_option_enabled(L"MAICORD_UI_CAPTION_PROBE");
    const bool inbox_requested = probe_option_enabled(L"MAICORD_INBOX_UI_PROBE");
    const bool notification_settings_requested =
        probe_option_enabled(L"MAICORD_NOTIFICATION_SETTINGS_UI_PROBE");
    const bool user_settings_requested = probe_option_enabled(L"MAICORD_USER_SETTINGS_UI_PROBE");
    const bool resize_text_requested = probe_option_enabled(L"MAICORD_UI_RESIZE_TEXT_PROBE");
    const bool store_requested = probe_option_enabled(L"MAICORD_STORE_UI_PROBE");
    const bool flag_surfaces_requested = probe_option_enabled(L"MAICORD_REGIONAL_FLAG_SURFACES_PROBE");
    if (probe_option_enabled(L"MAICORD_REGIONAL_FLAG_PROBE"))
        return finish_gate(regional_flag_ui_probe(g_probe.renderer, g_probe.popup_scene, probe_capture_frame),
                           "regional_flag_gpu_probe_passed", "regional_flag_gpu_probe_failed", 242);
    wchar_t title_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_BOOTSTRAP_TITLE_PROBE", title_only, 2) == 1)
        return finish_gate(title_only[0] == L'2' || probe_bootstrap_title(), "bootstrap_title_passed",
                           "bootstrap_title_failed", 246);
    if (probe_option_enabled(L"MAICORD_STARTUP_GLYPH_ATLAS_PROBE"))
        return finish_gate(probe_startup_glyph_atlas(), "startup_glyph_atlas_passed", "startup_glyph_atlas_failed",
                           247);
    MediaQuad* initial_media_quads = g_probe.renderer.media_draw.quad_data;
    MediaAtlasHandle* initial_media_handles = g_probe.renderer.media_draw.handle_data;
    u8* initial_media_domains = g_probe.renderer.media_draw.domain_data;
    MediaDrawBatch* initial_media_batches = g_probe.renderer.media_draw.batch_data;
    if (g_probe.renderer.media_draw.quad_capacity != kRendererMediaInitialQuadCapacity ||
        g_probe.renderer.media_draw.batch_capacity != kRendererMediaInitialBatchCapacity ||
        !renderer_media_draw_storage_reserve_quads(&g_probe.renderer.media_draw,
                                                   kRendererMediaInitialQuadCapacity + 1u) ||
        !renderer_media_draw_storage_reserve_batches(&g_probe.renderer.media_draw,
                                                     kRendererMediaInitialBatchCapacity + 1u) ||
        g_probe.renderer.media_draw.quad_data != initial_media_quads ||
        g_probe.renderer.media_draw.handle_data != initial_media_handles ||
        g_probe.renderer.media_draw.domain_data != initial_media_domains ||
        g_probe.renderer.media_draw.batch_data != initial_media_batches ||
        g_probe.renderer.media_draw.quad_capacity != kRendererMediaInitialQuadCapacity * 2u ||
        g_probe.renderer.media_draw.batch_capacity != kRendererMediaInitialBatchCapacity * 2u) {
        log_write(LogLevel::error, "probe", "initial_media_storage_growth_failed");
        log_u64(LogLevel::error, "probe", "initial_media_quad_capacity",
                g_probe.renderer.media_draw.quad_capacity);
        log_u64(LogLevel::error, "probe", "initial_media_batch_capacity",
                g_probe.renderer.media_draw.batch_capacity);
        return finish_probe(9);
    }
    ID3D11Buffer* initial_glyph_buffer = g_probe.renderer.pipeline.glyph_instance_buffer;
    const u32 initial_glyph_capacity = g_probe.renderer.pipeline.glyph_instance_capacity;
    const D3D11DrawPipelineResult glyph_growth = d3d11_draw_pipeline_reserve_glyph_instances(
        &g_probe.renderer.pipeline, g_probe.renderer.graphics.device, sizeof(GlyphQuad),
        initial_glyph_capacity + 1u);
    ID3D11Buffer* grown_glyph_buffer = g_probe.renderer.pipeline.glyph_instance_buffer;
    const D3D11DrawPipelineResult rejected_glyph_growth = d3d11_draw_pipeline_reserve_glyph_instances(
        &g_probe.renderer.pipeline, g_probe.renderer.graphics.device, sizeof(GlyphQuad), 0);
    if (!glyph_growth.ok() || grown_glyph_buffer == initial_glyph_buffer ||
        g_probe.renderer.pipeline.glyph_instance_capacity != initial_glyph_capacity + 1u ||
        rejected_glyph_growth.error != D3D11DrawPipelineError::invalid_input ||
        g_probe.renderer.pipeline.glyph_instance_buffer != grown_glyph_buffer ||
        g_probe.renderer.pipeline.glyph_instance_capacity != initial_glyph_capacity + 1u) {
        log_write(LogLevel::error, "probe", "initial_glyph_storage_growth_failed");
        return finish_probe(9);
    }
    if (!message_composer_initialize(
            &g_probe.message_composer,
            Slice<char>{g_probe.message_composer_text, sizeof(g_probe.message_composer_text)})) {
        return finish_probe(2);
    }
    message_composer_reset(&g_probe.message_composer, 111, false);
    if (message_composer_insert_utf8(&g_probe.message_composer, Utf8View{"a\xF0\x9F\x9A\x80"
                                                                         "b",
                                                                         6}) !=
            MessageComposerResult::changed ||
        message_composer_set_selection(&g_probe.message_composer, 1, 5) != MessageComposerResult::changed) {
        return finish_probe(2);
    }
    g_probe.message_composer.focused = true;
    if (!ui_scene_initialize_bootstrap(&g_probe.shell_scene) ||
        !guild_rail_scene_initialize(
            &g_probe.guild_rail_scene,
            Slice<GuildRailRowWidget>{g_probe.guild_rail_rows, kProbeGuildCapacity},
            Slice<UiWidget>{g_probe.guild_rail_widgets, kProbeGuildCapacity + 3 + 2 * kGuildRailPluginTiles},
            Slice<UiLayoutNode>{g_probe.guild_rail_layout_nodes, kProbeGuildCapacity + 3 + 2 * kGuildRailPluginTiles}) ||
        !shell_navigation_scene_initialize(
            &g_probe.shell_navigation_scene,
            Slice<ShellNavigationRowWidget>{g_probe.shell_navigation_rows, kProbeChannelCapacity},
            Slice<UiWidget>{g_probe.shell_navigation_widgets, kProbeChannelCapacity * 4 + 2},
            Slice<UiLayoutNode>{g_probe.shell_navigation_layout_nodes, kProbeChannelCapacity * 4 + 2}) ||
        !message_composer_scene_initialize_owned(&g_probe.message_composer_scene) ||
        !initialize_probe_message_scene() || !synchronize_probe_shell_layout()) {
        return finish_probe(2);
    }
    if (probe_option_enabled(L"MAICORD_SIGNOUT_GLYPH_ATLAS_PROBE"))
        return finish_gate(initialize_fixture(true) && probe_signout_glyph_atlas(), "signout_glyph_atlas_passed",
                           "signout_glyph_atlas_failed", 248);
    if (probe_option_enabled(L"MAICORD_ACCOUNT_MENU_UI_PROBE"))
        return finish_gate(
            initialize_fixture(true) && account_menu_ui_probe(g_probe.renderer, probe_capture_frame),
            "account_menu_native_ui_passed", "account_menu_native_ui_failed", 255);
    if (probe_option_enabled(L"MAICORD_UI_BACKGROUND_THEME_PROBE"))
        return finish_gate(initialize_fixture(true) &&
                           background_theme_ui_probe({g_probe.renderer,
                                                      +[]() noexcept {
                                                          return synchronize_probe_shell_layout() &&
                                                                 rebuild_probe_content_text();
                                                      },
                                                      probe_capture_frame}),
                           "background_theme_native_ui_passed", "background_theme_native_ui_failed", 270);
    if (probe_option_enabled(L"MAICORD_UI_FRAME_DAMAGE_PROBE"))
        return finish_gate(initialize_fixture(true) && frame_damage_ui_probe(), "frame_damage_native_ui_passed",
                           "frame_damage_native_ui_failed", 307);
    if (probe_option_enabled(L"MAICORD_UI_SHOWCASE_PROBE"))
        return finish_gate(
            initialize_fixture(true) && showcase_ui_probe(g_probe.renderer, g_probe.message_text),
            "showcase_native_ui_passed", "showcase_native_ui_failed", 304);
    if (probe_option_enabled(L"MAICORD_UI_INLINE_FLAGS_PROBE"))
        return finish_gate(initialize_fixture(true) &&
                           inline_flags_ui_probe({g_probe.renderer, g_probe.shell_scene, g_probe.chat_state,
                                                  g_probe.navigation, +[]() noexcept {
                                                      return synchronize_probe_shell_layout() &&
                                                             rebuild_probe_content_text();
                                                  }}),
                           "inline_flags_native_ui_passed", "inline_flags_native_ui_failed", 269);
    if (probe_option_enabled(L"MAICORD_UI_GUILD_RAIL_ICON_PROBE"))
        return finish_gate(initialize_fixture(true) &&
                           guild_rail_icon_ui_probe({g_probe.renderer, g_probe.guild_rail_scene, g_probe.chat_state,
                                                     g_probe.navigation,
                                                     +[]() noexcept {
                                                         return synchronize_probe_shell_layout() &&
                                                                rebuild_probe_content_text();
                                                     },
                                                     probe_capture_frame}),
                           "guild_rail_icons_native_ui_passed", "guild_rail_icons_native_ui_failed", 275);
    if (probe_option_enabled(L"MAICORD_UI_POST_RESIZE_PACING_PROBE")) {
        if (post_resize_probe_detail::compositor_asleep()) {
            log_write(LogLevel::info, "probe", "post_resize_pacing_skipped_display_asleep");
            return finish_probe(77);
        }
        const bool valid =
            initialize_fixture(true) && post_resize_pacing_ui_probe(
                                            g_probe.renderer, g_probe.window, g_probe.message_scene,
                                            +[]() noexcept { return synchronize_probe_shell_layout(); },
                                            +[]() noexcept { return rebuild_probe_content_text(); });
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "post_resize_pacing_native_ui_passed" : "post_resize_pacing_native_ui_failed");
        return finish_probe(valid ? 0 : 268);
    }
    if (probe_option_enabled(L"MAICORD_UI_UNIFIED_PICKER_PROBE"))
        return finish_gate(
            initialize_fixture(true) && unified_picker_ui_probe(g_probe.renderer, probe_capture_frame),
            "unified_picker_native_ui_passed", "unified_picker_native_ui_failed", 266);
    if (probe_option_enabled(L"MAICORD_UI_HOVER_RETENTION_PROBE"))
        return finish_gate(
            initialize_fixture(true) &&
            hover_retention_ui_probe(g_probe.renderer, g_probe.message_view, g_probe.message_scene,
                                     g_probe.users, rebuild_probe_content_text),
            "hover_retention_native_ui_passed", "hover_retention_native_ui_failed", 267);
    if (probe_option_enabled(L"MAICORD_UI_MESSAGE_COMPONENTS_PROBE"))
        return finish_gate(initialize_fixture(true) &&
                               message_components_ui_probe(g_probe.renderer, g_probe.message_view, g_probe.rich_store,
                                                           g_probe.message_scene, g_probe.users,
                                                           rebuild_probe_content_text,
                                                           +[]() noexcept { return synchronize_probe_shell_layout(); }),
                           "message_components_native_ui_passed", "message_components_native_ui_failed", 305);
    if (probe_option_enabled(L"MAICORD_UI_MESSAGE_HEADER_CLIP_PROBE"))
        return finish_gate(initialize_fixture(true) &&
                               message_header_clip_ui_probe(g_probe.renderer, g_probe.message_view, g_probe.rich_store,
                                                            g_probe.message_scene, g_probe.users,
                                                            rebuild_probe_content_text),
                           "message_header_clip_native_ui_passed", "message_header_clip_native_ui_failed", 306);
    if (probe_option_enabled(L"MAICORD_UI_EDITED_LABEL_PROBE"))
        return finish_gate(initialize_fixture(true) &&
                               edited_label_ui_probe(g_probe.renderer, g_probe.message_view, g_probe.rich_store,
                                                     g_probe.message_scene, g_probe.users, rebuild_probe_content_text,
                                                     +[](const wchar_t* name) noexcept { return probe_capture_frame(name); }),
                           "edited_label_native_ui_passed", "edited_label_native_ui_failed", 308);
    if (probe_option_enabled(L"MAICORD_UI_MEDIA_UNDERLAY_PROBE"))
        return finish_gate(initialize_fixture(true) && probe_media_underlay_and_quick_reactions(),
                           "media_underlay_native_ui_passed", "media_underlay_native_ui_failed", 264);
    if (probe_option_enabled(L"MAICORD_UI_SEARCH_POLISH_PROBE"))
        return finish_gate(initialize_fixture(true) && search_polish_ui_probe(g_probe.renderer),
                           "search_polish_native_ui_passed", "search_polish_native_ui_failed", 265);
    if (probe_option_enabled(L"MAICORD_SCOPED_SEARCH_UI_PROBE"))
        return finish_gate(initialize_fixture(true) && scoped_search_ui_probe(g_probe.renderer),
                           "scoped_search_native_ui_passed", "scoped_search_native_ui_failed", 263);
    if (probe_option_enabled(L"MAICORD_UI_PANE_GLYPH_STRESS_PROBE"))
        return finish_gate(initialize_fixture(true) && native_pane_glyph_stress_probe(g_probe.renderer),
                           "pane_glyph_stress_native_ui_passed", "pane_glyph_stress_native_ui_failed", 262);
    if (resize_stress_requested()) {
        // Live-resize stress over the rich-message fixture in a shown window:
        // one WM_SIZE and paint per step, clipped updates without buffer
        // reallocation, and the same geometry on the way back.
        bool valid = initialize_fixture() && probe_present(&g_probe.renderer, kDefaultTheme);
        if (valid) {
            probe_window::show(g_probe.window);
            valid = UpdateWindow(g_probe.window) && run_resize_stress();
        }
        if (!valid)
            log_u64(LogLevel::error, "probe", "resize_stress_failure_stage", g_probe.failure_stage);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "resize_stress_passed" : "resize_stress_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : static_cast<int>(g_probe.failure_stage != 0 ? g_probe.failure_stage : 7u);
    }
    if (probe_option_enabled(L"MAICORD_UI_GUILD_ROLES_PROBE"))
        return finish_gate(initialize_fixture(true) && guild_roles_ui_probe(g_probe.renderer),
                           "guild_roles_native_ui_passed", "guild_roles_native_ui_failed", 279);
    if (probe_option_enabled(L"MAICORD_UI_CHANNEL_OVERVIEW_PROBE"))
        return finish_gate(initialize_fixture(true) &&
                           channel_overview_ui_probe(g_probe.renderer, g_probe.text_measure_cache),
                           "channel_overview_native_ui_passed", "channel_overview_native_ui_failed", 261);
    if (probe_option_enabled(L"MAICORD_LONG_EMBED_UI_PROBE"))
        return finish_gate(initialize_fixture(true) && probe_long_embed_text() && probe_embed_code_block(),
                           "long_embed_native_ui_passed", "long_embed_native_ui_failed", 263);
    if (probe_option_enabled(L"MAICORD_UI_SURFACE_MOTION_PROBE"))
        return finish_gate(initialize_fixture(true) && probe_ui_surface_motion(),
                           "surface_motion_native_ui_passed", "surface_motion_native_ui_failed", 260);
    if (probe_option_enabled(L"MAICORD_MEDIA_FAVORITES_UI_PROBE"))
        return finish_gate(initialize_fixture(true) && probe_media_gallery(true),
                           "media_favorites_native_ui_passed", "media_favorites_native_ui_failed", 259);
    if (probe_option_enabled(L"MAICORD_MEDIA_PICKER_UI_PROBE"))
        return finish_gate(initialize_fixture(true) && probe_media_gallery(),
                           "media_gallery_native_ui_passed", "media_gallery_native_ui_failed", 258);
    if (probe_option_enabled(L"MAICORD_UI_FORWARDED_MESSAGES_PROBE"))
        return finish_gate(initialize_fixture(true) && forwarded_messages_ui_probe(
            g_probe.renderer, g_probe.text_measure_cache, probe_capture_frame),
                           "forwarded_messages_native_ui_passed", "forwarded_messages_native_ui_failed", 289);
    if (probe_option_enabled(L"MAICORD_UI_SHORTCUTS_PROBE")) {
        const bool valid = initialize_fixture(true) &&
                           shortcuts_ui_probe(g_probe.renderer, +[](const wchar_t* filename, const Theme& theme) noexcept {
                               return probe_capture_frame(filename, theme);
                           });
        return finish_probe(valid ? 0 : 397);
    }
    if (probe_option_enabled(L"MAICORD_MEDIA_MEMORY_PROBE")) {
        // Atlas growth/trim pixels and GPU memory of synthetic media workloads.
        const bool valid = initialize_fixture(true) && media_memory_probe(g_probe.renderer);
        return finish_probe(valid ? 0 : 156);
    }
    if (probe_option_enabled(L"MAICORD_UI_CALL_PROBE")) {
        const bool valid = initialize_fixture(true) &&
                           call_scene_ui_probe(g_probe.renderer, +[](const wchar_t* filename, const Theme& theme) noexcept {
                               return probe_capture_frame(filename, theme);
                           });
        return finish_probe(valid ? 0 : 398);
    }
    if (probe_option_enabled(L"MAICORD_UI_DOWNLOAD_PROBE")) {
        const auto capture_download = +[](const wchar_t* filename, const Theme& theme) noexcept {
            const auto* scene = g_probe.renderer.download_scene;
            if (scene && scene->active && theme.text_primary.red > .7f) {
                const auto p = scene->panel;
                const UiRect heading{p.left + 48,p.top + 10,p.right - 48,p.top + 40};
                if (!probe_text_pixels({&heading,1})) return false;
            }
            return probe_capture_frame(filename,theme);
        };
        const bool valid = initialize_fixture(true) && probe_attachment_save_affordances() &&
                           download_ui_probe(g_probe.renderer, capture_download);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "downloads_native_ui_passed" : "downloads_native_ui_failed");
        return finish_probe(valid ? 0 : 291);
    }
    if (probe_option_enabled(L"MAICORD_UI_REPLY_DRAFT_PROBE"))
        return finish_gate(initialize_fixture(true) && reply_draft_ui_probe(
            g_probe.renderer, g_probe.text_measure_cache, g_probe.window, probe_capture_frame),
                           "reply_draft_native_ui_passed", "reply_draft_native_ui_failed", 296);
    if (probe_option_enabled(L"MAICORD_UI_FORUM_MEDIA_PROBE"))
        return finish_gate(
            initialize_fixture(true) && forum_post_ui_probe(g_probe.renderer, g_probe.text_measure_cache,
                                                            g_probe.window, probe_capture_frame, true),
            "forum_media_native_ui_passed", "forum_media_native_ui_failed", 301);
    if (probe_option_enabled(L"MAICORD_UI_FORUM_POST_PROBE"))
        return finish_gate(
            initialize_fixture(true) && forum_post_ui_probe(g_probe.renderer, g_probe.text_measure_cache,
                                                            g_probe.window, probe_capture_frame),
            "forum_post_native_ui_passed", "forum_post_native_ui_failed", 299);
    if (probe_option_enabled(L"MAICORD_UI_THREAD_CREATION_PROBE"))
        return finish_gate(initialize_fixture(true) && thread_creation_ui_probe(
            g_probe.renderer, g_probe.text_measure_cache, g_probe.window, probe_capture_frame),
                           "thread_creation_native_ui_passed", "thread_creation_native_ui_failed", 298);
    if (probe_option_enabled(L"MAICORD_UI_THREAD_BROWSER_PROBE"))
        return finish_gate(initialize_fixture(true) && thread_browser_ui_probe(
            g_probe.renderer, g_probe.text_measure_cache, g_probe.window, probe_capture_frame),
                           "thread_browser_native_ui_passed", "thread_browser_native_ui_failed", 297);
    if (probe_option_enabled(L"MAICORD_UI_THREAD_MEMBERSHIP_PROBE"))
        return finish_gate(initialize_fixture(true) && thread_membership_ui_probe(
            g_probe.renderer, g_probe.text_measure_cache, g_probe.window, probe_capture_frame),
                           "thread_membership_native_ui_passed", "thread_membership_native_ui_failed", 295);
    if (probe_option_enabled(L"MAICORD_UI_REACTION_INTENT_PROBE"))
        return finish_gate(initialize_fixture(true) && reaction_intent_ui_probe(
            g_probe.renderer,g_probe.text_measure_cache,g_probe.window,probe_capture_frame),
                           "reaction_intent_native_ui_passed", "reaction_intent_native_ui_failed", 294);
    if (probe_option_enabled(L"MAICORD_UI_RICH_SEARCH_PROBE"))
        return finish_gate(initialize_fixture(true) && rich_search_ui_probe(
            g_probe.renderer,g_probe.text_measure_cache,probe_capture_frame),
                           "rich_search_native_ui_passed", "rich_search_native_ui_failed", 293);
    if (probe_option_enabled(L"MAICORD_UI_UNREAD_BOUNDARY_PROBE"))
        return finish_gate(initialize_fixture(true) && unread_boundary_ui_probe(
            g_probe.renderer,g_probe.text_measure_cache,probe_capture_frame),
                           "unread_boundary_native_ui_passed", "unread_boundary_native_ui_failed", 292);
    if (probe_option_enabled(L"MAICORD_UI_MESSAGE_FORWARD_PROBE"))
        return finish_gate(initialize_fixture(true) && message_forward_ui_probe(g_probe.renderer, probe_capture_frame),
                           "message_forward_native_ui_passed", "message_forward_native_ui_failed", 290);
    if (probe_option_enabled(L"MAICORD_UI_QUICK_SWITCHER_PROBE"))
        return finish_gate(initialize_fixture(true) && quick_switcher_ui_probe(g_probe.renderer, probe_capture_frame),
                           "quick_switcher_native_ui_passed", "quick_switcher_native_ui_failed", 288);
    if (probe_option_enabled(L"MAICORD_UI_COMPOSER_WORKFLOW_PROBE")) {
        const auto publish = +[](const ComposerWorkflowProbeState& state) noexcept {
            auto& renderer = g_probe.renderer;
            const float width = renderer.width * 96.f / maximum(renderer.dpi, 1u);
            const float height = renderer.height * 96.f / maximum(renderer.dpi, 1u);
            auto metrics = kGuildChannelShellLayoutMetrics;
            metrics.composer_height = message_composer_scene_preferred_height(
                g_probe.message_composer, ui_scene_composer_width(metrics, width), state.reply.bytes != 0);
            metrics.revision = 200'000u + ui_extent_units(metrics.composer_height);
            if (!ui_scene_layout_with_metrics(&g_probe.shell_scene, UiShellView::guild_channel, metrics,
                                              width, height))
                return false;
            const auto* composer = ui_scene_node(g_probe.shell_scene, UiSemantic::composer);
            const auto* messages = ui_scene_node(g_probe.shell_scene, UiSemantic::message_view);
            if (!composer || !messages ||
                !message_scene_update(&g_probe.message_scene, messages->rectangle).ok() ||
                !message_composer_scene_update(&g_probe.message_composer_scene, &g_probe.message_composer,
                                               composer->rectangle, true, state.reply.bytes ? 77u : 0u,
                                               state.reply, nullptr, state.status,
                                               {{}, false, true, state.retained}, false, state.mention,
                                               state.mention_visible, state.error)
                     .ok())
                return false;
            return rebuild_probe_content_text();
        };
        const bool valid = initialize_fixture(true) && composer_workflow_ui_probe(
            g_probe.renderer, g_probe.message_composer, g_probe.message_composer_scene,
            publish, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "composer_workflow_native_ui_passed" : "composer_workflow_native_ui_failed");
        return finish_probe(valid ? 0 : 287);
    }
    if (probe_option_enabled(L"MAICORD_UI_PINNED_MESSAGES_PROBE"))
        return finish_gate(initialize_fixture(true) &&
                           pinned_messages_ui_probe(g_probe.renderer, probe_capture_frame),
                           "pinned_messages_native_ui_passed", "pinned_messages_native_ui_failed", 286);
    if (probe_option_enabled(L"MAICORD_SEARCH_UI_PROBE"))
        return finish_gate(
            initialize_fixture(true) && search_ui_probe(g_probe.renderer, probe_capture_frame),
            "search_native_ui_passed", "search_native_ui_failed", 256);
    if (probe_option_enabled(L"MAICORD_TOOLTIP_UI_PROBE"))
        return finish_gate(
            initialize_fixture(true) && tooltip_ui_probe(g_probe.renderer, probe_capture_frame),
            "tooltip_native_ui_passed", "tooltip_native_ui_failed", 254);
    if (GetEnvironmentVariableW(L"MAICORD_TOAST_UI_PROBE", nullptr, 0))
        return finish_gate(initialize_fixture(true) && toast_ui_probe(g_probe.renderer, probe_capture_frame),
                           "toast_native_ui_passed", "toast_native_ui_failed", 276);
    if (GetEnvironmentVariableW(L"MAICORD_CATALOG_APNG_UI_PROBE", nullptr, 0))
        return finish_gate(initialize_fixture(true) && probe_catalog_apng(), "catalog_apng_native_ui_passed",
                           "catalog_apng_native_ui_failed", 273);
    if (probe_option_enabled(L"MAICORD_TRANSPARENT_GUILD_UI_PROBE"))
        return finish_gate(initialize_fixture(true) && probe_transparent_guild_icon(),
                           "transparent_guild_native_ui_passed", "transparent_guild_native_ui_failed", 271);
    if (probe_option_enabled(L"MAICORD_PROFILE_EDITOR_UI_PROBE")) {
        const bool valid = initialize_fixture(true) &&
                           profile_editor_ui_probe(g_probe.renderer, &g_probe.text_measure_cache, &g_probe.chat_state,
                                                   &g_probe.users, probe_capture_frame);
        return finish_probe(valid ? 0 : 272);
    }
    if (probe_option_enabled(L"MAICORD_PROFILE_ACTIVITY_UI_PROBE")) {
        const bool valid = initialize_fixture(true) && probe_profile_activity_card();
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "profile_activity_native_ui_passed" : "profile_activity_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : static_cast<int>(g_probe.failure_stage ? g_probe.failure_stage : 742);
    }
    if (probe_option_enabled(L"MAICORD_PROFILE_BRAILLE_UI_PROBE")) {
        const bool valid = initialize_fixture(true) && probe_braille_profile() && probe_profile_member_modal();
        if (!valid)
            log_u64(LogLevel::error, "probe", "braille_profile_failure_stage", g_probe.failure_stage);
        return finish_gate(valid, "braille_profile_native_ui_passed", "braille_profile_native_ui_failed", 270);
    }
    if (probe_option_enabled(L"MAICORD_ANIMATION_HANDOFF_UI_PROBE")) {
        bool valid = initialize_fixture(true);
        valid = valid && application_media_animation_ui_probe(g_probe.renderer, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "animation_handoff_native_ui_passed" : "animation_handoff_native_ui_failed");
        return finish_probe(valid ? 0 : 252);
    }
    if (probe_option_enabled(L"MAICORD_UI_PROFILE_MEDIA_SEQUENCE_PROBE"))
        return finish_gate(initialize_fixture(true) &&
                               profile_media_sequence_ui_probe(g_probe.renderer, &g_probe.text_measure_cache,
                                                               &g_probe.chat_state, &g_probe.users),
                           "profile_media_sequence_native_ui_passed", "profile_media_sequence_native_ui_failed", 280);
    if (probe_option_enabled(L"MAICORD_MENU_ROW_UI_PROBE")) {
        bool valid = initialize_fixture(true);
        if (!valid) {
            log_u64(LogLevel::error, "probe", "menu_rows_fixture_failure_stage", g_probe.failure_stage);
        }
        valid = valid && menu_row_ui_probe(g_probe.renderer, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "shared_menu_rows_ui_passed" : "shared_menu_rows_ui_failed");
        return finish_probe(valid ? 0 : 251);
    }
    if (probe_option_enabled(L"MAICORD_VIDEO_CONTROLS_UI_PROBE")) {
        bool valid = initialize_fixture(true);
        if (!valid)
            log_u64(LogLevel::error, "probe", "video_controls_fixture_failure_stage",
                    g_probe.failure_stage);
        if (valid) {
            const LONG width = static_cast<LONG>(1280u * g_probe.renderer.dpi / 96u);
            const LONG height = static_cast<LONG>(970u * g_probe.renderer.dpi / 96u);
            RECT bounds{0, 0, width, height};
            valid = AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, kProbeWindowExStyle) &&
                    SetWindowPos(g_probe.window, nullptr, 0, 0, bounds.right - bounds.left,
                                 bounds.bottom - bounds.top, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER) &&
                    IsWindow(g_probe.window) && synchronize_probe_shell_layout() &&
                    rebuild_probe_content_text();
        }
        valid = valid && video_scene_ui_probe(g_probe.renderer, probe_capture_frame);
        valid = valid && theme_typography_ui_probe(g_probe.renderer);
        valid = valid && chat_text_size_churn_ui_probe(g_probe.renderer, g_probe.message_scene,
                                                       g_probe.text_measure_cache, rebuild_probe_content_text);
        valid = valid && audio_scene_ui_probe(g_probe.renderer);
        valid = valid && poll_scene_ui_probe(g_probe.renderer, probe_capture_frame);
        valid = valid && navigation_gaps_ui_probe(g_probe.renderer, &g_probe.navigation, g_probe.chat_state,
                                                  probe_sync_shell_and_text, probe_capture_frame);
        // The app-wide notice band (Application Test Mode's) over the shell, which lays out below it.
        if (valid) {
            static constexpr Utf8View kNoticeText = utf8_literal(
                "Test mode is currently active for Starfall Tactics Dev. Unpublished SKUs will appear and you will "
                "not be charged for any related payments.");
            static UiNoticeBand notice{};
            notice = {kNoticeText, {}, {{34, 20, text_style_bold}}, 1, UiNoticeKind::warning, true,
                      kUiNoticeBandHeight};
            g_probe.shell_scene.notice = &notice;
            const bool shown = probe_sync_shell_and_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
                               probe_capture_frame(L"notice-band-test-mode.bmp");
            const auto* rail = ui_scene_node(g_probe.shell_scene, UiSemantic::guild_rail);
            log_u64(LogLevel::info, "probe", "notice_band_rail_top", rail ? static_cast<u64>(rail->rectangle.top) : 0);
            g_probe.shell_scene.notice = nullptr;
            valid = shown && rail && rail->rectangle.top >= kUiNoticeBandHeight && probe_sync_shell_and_text();
        }
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "video_controls_injected_ui_passed" : "video_controls_injected_ui_failed");
        return finish_probe(valid ? 0 : 250);
    }
    if (caption_requested) {
        bool valid = initialize_fixture(true);
        if (!valid)
            log_u64(LogLevel::error, "probe", "caption_fixture_failure_stage", g_probe.failure_stage);
        valid = valid && caption_ui_probe(g_probe.renderer, probe_caption_relayout, probe_caption_login);
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        log_close();
        return valid ? 0 : 303;
    }
    if (inbox_requested) {
        bool valid = initialize_fixture(true);
        const bool initialize_failed = !valid;
        if (!valid)
            log_u64(LogLevel::error, "probe", "inbox_fixture_failure_stage", g_probe.failure_stage);
        valid = valid && inbox_ui_probe(g_probe.renderer, g_probe.popup_scene, probe_capture_frame);
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        log_close();
        return valid ? 0 : 220 + (initialize_failed ? 10 : inbox_ui_probe_stage);
    }
    if (notification_settings_requested) {
        const bool valid = initialize_fixture(true) &&
                           notification_settings_ui_probe(g_probe.renderer, probe_capture_frame);
        return finish_probe(valid ? 0 : 249);
    }
    if (store_requested) {
        const bool valid = initialize_fixture(true) && store_ui_probe(g_probe.renderer, g_probe.popup_scene, g_probe.text_measure_cache,
                                                              []() noexcept { return synchronize_probe_shell_layout(); }, probe_capture_frame);
        return finish_probe(valid ? 0 : 303);
    }
    if (user_settings_requested) {
        const bool valid = initialize_fixture(true) &&
                           user_settings_ui_probe(g_probe.renderer, g_probe.text_measure_cache, probe_capture_frame);
        return finish_probe(valid ? 0 : 302);
    }
    if (resize_text_requested) {
        const auto resize = [](u32 width, u32 height) noexcept {
            return renderer_resize(&g_probe.renderer, width, height) && probe_sync_shell_and_text();
        };
        const bool valid = initialize_fixture(true) &&
                           resize_text_ui_probe(g_probe.renderer, g_probe.text_measure_cache, resize, probe_capture_frame);
        return finish_probe(valid ? 0 : 304);
    }
    const char password_mask[] = {static_cast<char>(0xE2u), static_cast<char>(0x80u),
                                  static_cast<char>(0xA2u), static_cast<char>(0xE2u),
                                  static_cast<char>(0x80u), static_cast<char>(0xA2u)};
    constexpr Utf8View qr_payload =
        utf8_literal("https://chat_state.com/ra/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    QrCode login_qr{};
    if (!qr_code_encode_m(ConstSlice<u8>{reinterpret_cast<const u8*>(qr_payload.data), qr_payload.bytes}, 5,
                          &login_qr)) {
        return finish_probe(8);
    }
    const RendererLoginView login_view{utf8_literal("native.user@example.test"),
                                       Utf8View{password_mask, sizeof(password_mask)},
                                       {},
                                       0,
                                       0,
                                       false,
                                       LoginControl::qr,
                                       RendererLoginStatus::remote_qr_ready,
                                       login_mfa_capability_none,
                                       login_mfa_capability_none,
                                       0,
                                       false,
                                       &login_qr,
                                       false,
                                       false,
                                       false,
                                       true};
    struct WrappedRenderProbe {
        const wchar_t* option;
        WrappedProbeScript script;
        const char* passed;
        const char* failed;
    };
    static constexpr WrappedRenderProbe wrapped_render_probes[]{
        {L"MAICORD_TIBETAN_WRAP_RENDER_PROBE", WrappedProbeScript::tibetan, "tibetan_wrap_render_probe_passed",
         "tibetan_wrap_render_probe_failed"},
        {L"MAICORD_MYANMAR_WRAP_RENDER_PROBE", WrappedProbeScript::myanmar, "myanmar_wrap_render_probe_passed",
         "myanmar_wrap_render_probe_failed"},
        {L"MAICORD_KHMER_WRAP_RENDER_PROBE", WrappedProbeScript::khmer, "khmer_wrap_render_probe_passed",
         "khmer_wrap_render_probe_failed"},
        {L"MAICORD_SINHALA_WRAP_RENDER_PROBE", WrappedProbeScript::sinhala, "sinhala_wrap_render_probe_passed",
         "sinhala_wrap_render_probe_failed"},
        {L"MAICORD_MALAYALAM_WRAP_RENDER_PROBE", WrappedProbeScript::malayalam,
         "malayalam_wrap_render_probe_passed", "malayalam_wrap_render_probe_failed"},
        {L"MAICORD_KANNADA_WRAP_RENDER_PROBE", WrappedProbeScript::kannada, "kannada_wrap_render_probe_passed",
         "kannada_wrap_render_probe_failed"},
        {L"MAICORD_TELUGU_WRAP_RENDER_PROBE", WrappedProbeScript::telugu, "telugu_wrap_render_probe_passed",
         "telugu_wrap_render_probe_failed"},
        {L"MAICORD_ODIA_WRAP_RENDER_PROBE", WrappedProbeScript::odia, "odia_wrap_render_probe_passed",
         "odia_wrap_render_probe_failed"},
        {L"MAICORD_GURMUKHI_WRAP_RENDER_PROBE", WrappedProbeScript::gurmukhi, "gurmukhi_wrap_render_probe_passed",
         "gurmukhi_wrap_render_probe_failed"},
        {L"MAICORD_GUJARATI_WRAP_RENDER_PROBE", WrappedProbeScript::gujarati, "gujarati_wrap_render_probe_passed",
         "gujarati_wrap_render_probe_failed"},
        {L"MAICORD_BENGALI_WRAP_RENDER_PROBE", WrappedProbeScript::bengali, "bengali_wrap_render_probe_passed",
         "bengali_wrap_render_probe_failed"},
        {L"MAICORD_TAMIL_WRAP_RENDER_PROBE", WrappedProbeScript::tamil, "tamil_wrap_render_probe_passed",
         "tamil_wrap_render_probe_failed"},
        {L"MAICORD_THAI_WRAP_RENDER_PROBE", WrappedProbeScript::thai, "thai_wrap_render_probe_passed",
         "thai_wrap_render_probe_failed"},
    };
    for (const auto& wrapped : wrapped_render_probes) {
        if (!probe_option_enabled(wrapped.option))
            continue;
        const bool valid = initialize_fixture(true) && probe_script_wrapped(wrapped.script);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe", valid ? wrapped.passed : wrapped.failed);
        renderer_shutdown(&g_probe.renderer);
        d3d11_draw_pipeline_release(&g_probe.renderer.pipeline);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 241;
    }
    static constexpr Utf8View hangul_samples[]{utf8_literal("한국어 메시지 😀"), utf8_literal("한글 각 각"),
                                               utf8_literal("ᄓᅡᇫ ᅟᅠᆨ 가〮 각〯"), utf8_literal("〮 가〮〯 가〮")};
    static constexpr Utf8View extended_samples[]{utf8_literal("ሰላም ዓለም 😀"), utf8_literal("ሀ፝ ሁ፞ ሂ፟ ፩፪፫ ፲፻፼"),
                                                 utf8_literal("ᎣᏏᏲ ᏣᎳᎩ ꭰꭱꭲ"), utf8_literal("Ꭰ́ Ꭱ̄ שלום ሰላም")};
    static constexpr Utf8View standard_samples[]{utf8_literal("Բարև աշխարհ 😀"), utf8_literal("և եւ ﬓ ﬔ ﬕ ﬖ ﬗ ա́"),
                                                 utf8_literal("ქართული ᲥᲐᲠᲗᲣᲚᲘ"), utf8_literal("ႠႡႢ ⴀⴁⴂ ა́ ա̄")};
    static constexpr Utf8View deva_samples[]{utf8_literal("नमस्ते दुनिया 😀"), utf8_literal("र्कि किं कृ त्र ज्ञ"),
                                             utf8_literal("क्‍ष क्‌ष क़ि"), utf8_literal("ि ़ ॑ क॑ कः")};
    static constexpr Utf8View deva_direction_samples[]{
        utf8_literal("A \u202eर्कि हिन्दी 😀\u202c B"), utf8_literal("\u202eक्‍ष क्‌ष क्षेत्र\u202c"),
        utf8_literal("אב हिन्दी \u202eर्कि\u202c"), utf8_literal("\u2067हिन्दी \u202eर्कि\u202c\u2069")};
    static constexpr Utf8View direction_tracking_samples[]{
        utf8_literal("A \u202eर्कि हिन्दी 😀\u202c B"), utf8_literal("אָבּ גּד हिन्दी"),
        utf8_literal("سَلَامٌ \u202dب\u034fب بَ\u034fب\u202c"), utf8_literal("سَّلَام 👩‍👩‍👧‍👦 A")};
    // How a probe sets the text system's tracking: leave it, none, or alternate by sample.
    enum class RenderTracking : u8 { keep, none, alternate };
    struct RenderProbe {
        const wchar_t* option;
        const Utf8View* samples;
        wchar_t capture[32];
        u32 digit;
        RenderTracking tracking;
        const char* passed;
        const char* failed;
    };
    static constexpr RenderProbe render_probes[]{
        {L"MAICORD_EXTENDED_STANDARD_RENDER_PROBE", extended_samples, L"extended-standard-0-0-0.bmp", 18,
         RenderTracking::alternate, "extended_standard_render_probe_passed", "extended_standard_render_probe_failed"},
        {L"MAICORD_DIRECTION_TRACKING_RENDER_PROBE", direction_tracking_samples, L"direction-tracking-0-0-0.bmp",
         19, RenderTracking::alternate, "direction_tracking_render_probe_passed",
         "direction_tracking_render_probe_failed"},
        {L"MAICORD_DEVANAGARI_DIRECTION_RENDER_PROBE", deva_direction_samples, L"devanagari-direction-0-0-0.bmp",
         21, RenderTracking::none, "devanagari_direction_render_probe_passed",
         "devanagari_direction_render_probe_failed"},
        {L"MAICORD_DEVANAGARI_TRACKING_RENDER_PROBE", deva_samples, L"devanagari-tracking-0-0-0.bmp", 20,
         RenderTracking::alternate, "devanagari_tracking_render_probe_passed",
         "devanagari_tracking_render_probe_failed"},
        {L"MAICORD_DEVANAGARI_RENDER_PROBE", deva_samples, L"devanagari-0-0-0.bmp", 11, RenderTracking::keep,
         "devanagari_render_probe_passed", "devanagari_render_probe_failed"},
        {L"MAICORD_STANDARD_RENDER_PROBE", standard_samples, L"standard-0-0-0.bmp", 9, RenderTracking::keep,
         "standard_render_probe_passed", "standard_render_probe_failed"},
        {L"MAICORD_HANGUL_RENDER_PROBE", hangul_samples, L"hangul-0-0-0.bmp", 7, RenderTracking::keep,
         "hangul_render_probe_passed", "hangul_render_probe_failed"},
    };
    for (const auto& render : render_probes) {
        if (!probe_option_enabled(render.option))
            continue;
        const Utf8View* samples = render.samples;
        auto& system = g_probe.renderer.glyph_text.system;
        const float saved_tracking = system.tracking;
        const bool saved_independent = system.independent_letters;
        // Field drawing uses the native shaping of the renderer's text system.
        bool valid = true;
        constexpr u32 dpis[]{96, 144};
        for (u32 scale = 0; valid && scale < 2; ++scale) {
            renderer_set_dpi(&g_probe.renderer, dpis[scale]);
            for (u32 i = 0; valid && i < 4; ++i)
                for (u32 mode = 0; valid && mode < 3; ++mode) {
                    if (render.tracking != RenderTracking::keep) {
                        system.tracking = render.tracking == RenderTracking::none ? 0 : (i & 1) ? .13f : .03f;
                        system.independent_letters = i >= 2;
                    }
                    u32 characters = 0;
                    for (u32 byte = 0; byte < samples[i].bytes; ++byte)
                        characters += (static_cast<u8>(samples[i].data[byte]) & 0xc0) != 0x80;
                    auto view = login_view;
                    view.login = samples[i];
                    view.password_mask = {};
                    view.qr_code = nullptr;
                    view.focus = LoginControl::login;
                    view.edit_active = mode != 0;
                    view.edit_anchor = mode == 1 ? 0 : characters;
                    view.edit_caret = characters;
                    TextCompositionSpan composition{};
                    composition.start = 1;
                    composition.end = characters - 1;
                    composition.line_style = TextCompositionLineStyle::solid;
                    view.edit_composition_active = mode == 2;
                    view.edit_composition = mode == 2 ? ConstSlice<TextCompositionSpan>{&composition, 1}
                                                      : ConstSlice<TextCompositionSpan>{};
                    wchar_t capture[32];
                    copy_bytes(capture, render.capture, sizeof(capture));
                    capture[render.digit] += static_cast<wchar_t>(i);
                    capture[render.digit + 2] += static_cast<wchar_t>(mode);
                    capture[render.digit + 4] += static_cast<wchar_t>(scale);
                    valid = set_probe_login_view(view) && probe_prepare_capture();
                    if (mode)
                        valid = valid && g_probe.renderer.login_edit.active &&
                                g_probe.renderer.login_edit.caret.height > 0;
                    if (mode == 1)
                        valid = valid && g_probe.renderer.login_edit.selection_count > 0;
                    if (mode)
                        valid = valid && g_probe.renderer.login_edit.geometry;
                    valid = valid && probe_capture_frame(capture);
                }
        }
        system.tracking = saved_tracking;
        system.independent_letters = saved_independent;
        log_write(valid ? LogLevel::info : LogLevel::error, "probe", valid ? render.passed : render.failed);
        renderer_shutdown(&g_probe.renderer);
        d3d11_draw_pipeline_release(&g_probe.renderer.pipeline);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 240;
    }
    // Empty-owner contract: waiting is immediate and releasing is idempotent.
    D3D11DeviceOwner invalid_owner{};
    if (!gpu_frame_wait(&invalid_owner.surface, 0)) {
        return 9;
    }
    d3d11_device_owner_release(&invalid_owner);
    d3d11_device_owner_release(&invalid_owner);
    if (!device_owner_empty(invalid_owner)) {
        return 9;
    }
    D3D11DrawPipelineOwner invalid_pipeline{};
    const D3D11DrawPipelineConfig invalid_pipeline_config{};
    if (d3d11_draw_pipeline_initialize(&invalid_pipeline, nullptr, nullptr, 0, 0, 0,
                                       invalid_pipeline_config)
                .error != D3D11DrawPipelineError::invalid_input ||
        d3d11_draw_pipeline_reserve_glyph_instances(&invalid_pipeline, nullptr, 0, 0).error !=
            D3D11DrawPipelineError::invalid_input ||
        d3d11_draw_pipeline_update_transform(invalid_pipeline, nullptr, 0, 0, 0)) {
        return 9;
    }
    d3d11_draw_pipeline_release(&invalid_pipeline);
    d3d11_draw_pipeline_release(&invalid_pipeline);
    if (!draw_pipeline_empty(invalid_pipeline)) {
        return 9;
    }
    D3D11GlyphTextOwner invalid_glyph_text{};
    if (d3d11_glyph_text_initialize(&invalid_glyph_text, nullptr, nullptr, 0).error !=
        D3D11GlyphTextInitializeError::invalid_input) {
        return 9;
    }
    d3d11_glyph_text_release(&invalid_glyph_text);
    d3d11_glyph_text_release(&invalid_glyph_text);
    if (!glyph_text_owner_empty(invalid_glyph_text)) {
        return 9;
    }
    TextEditLayoutOwner invalid_composer{};
    MessageHitTestCacheOwner invalid_message_hit{};
    ContentTextRetentionOwner invalid_content_text{};
    ContentTextRetentionKey invalid_content_key{};
    text_edit_layout_release(&invalid_composer);
    text_edit_layout_release(&invalid_composer);
    message_hit_test_cache_release(&invalid_message_hit);
    message_hit_test_cache_release(&invalid_message_hit);
    if (content_text_retention_probe(&invalid_content_text, invalid_content_key) ||
        invalid_content_text.misses != 1) {
        return 9;
    }
    content_text_retention_commit(&invalid_content_text, invalid_content_key);
    if (!content_text_retention_probe(&invalid_content_text, invalid_content_key) ||
        invalid_content_text.builds != 1 || invalid_content_text.hits != 1) {
        return 9;
    }
    content_text_retention_invalidate(&invalid_content_text);
    content_text_retention_reset(&invalid_content_text);
    content_text_retention_reset(&invalid_content_text);
    if (!composer_layout_empty(invalid_composer) ||
        !message_hit_cache_empty(invalid_message_hit) ||
        !content_text_retention_empty(invalid_content_text)) {
        return 9;
    }
    const bool login_view_set = set_probe_login_view(login_view);
    const bool login_presented = login_view_set && probe_present(&g_probe.renderer, kDefaultTheme);
    if (!login_view_set || !login_presented || !g_probe.renderer.login_visible ||
        g_probe.renderer.glyph_text.title_quad_count < 32 || g_probe.renderer.paint_stream.count < 5 ||
        !probe_login_pixels()) {
        log_u64(LogLevel::error, "probe", "login_view_set", login_view_set);
        log_u64(LogLevel::error, "probe", "login_presented", login_presented);
        log_u64(LogLevel::error, "probe", "login_visible", g_probe.renderer.login_visible);
        log_u64(LogLevel::error, "probe", "login_glyph_quads",
                g_probe.renderer.glyph_text.title_quad_count);
        log_u64(LogLevel::error, "probe", "login_paint_quads", g_probe.renderer.paint_stream.count);
        log_u64(LogLevel::error, "probe", "login_present_required", g_probe.renderer.present_required);
        return finish_probe(8);
    }
    RendererLoginView restoring_view = login_view;
    restoring_view.status = RendererLoginStatus::restoring_session;
    restoring_view.qr_code = nullptr;
    if (!set_probe_login_view(restoring_view) || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_login_pixels(true) || !set_probe_login_view(login_view) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) || !probe_login_pixels() ||
        !probe_capture_frame(L"login-text.bmp")) {
        log_write(LogLevel::error, "probe", "login_restore_text_pixels_failed");
        return finish_probe(8);
    }
    log_write(LogLevel::info, "probe", "login_and_restore_text_pixels_passed");
    char color_emoji_login[30 * 4]{};
    constexpr u8 grinning_face[4]{0xF0, 0x9F, 0x98, 0x80};
    for (u32 emoji = 0; emoji < 30; ++emoji) {
        for (u32 byte = 0; byte < 4; ++byte) {
            color_emoji_login[emoji * 4u + byte] = static_cast<char>(grinning_face[byte]);
        }
    }
    RendererLoginView color_emoji_view = login_view;
    color_emoji_view.login = Utf8View{color_emoji_login, sizeof(color_emoji_login)};
    color_emoji_view.focus = LoginControl::login;
    color_emoji_view.qr_code = nullptr;
    constexpr u32 color_emoji_iterations = 2;
    const u32 glyph_cache_builds_before_color = g_probe.renderer.glyph_text.cache_builds;
    u32 warm_color_cache_builds = 0;
    for (u32 iteration = 0; iteration < color_emoji_iterations; ++iteration) {
        if (!set_probe_login_view(color_emoji_view)) {
            log_u64(LogLevel::error, "probe", "color_emoji_login_failed_iteration", iteration);
            return finish_probe(8);
        }
        if (iteration == 0)
            warm_color_cache_builds = g_probe.renderer.glyph_text.cache_builds;
        else if (g_probe.renderer.glyph_text.cache_builds != warm_color_cache_builds) {
            log_write(LogLevel::error, "probe", "cached_colrv1_glyph_was_rasterized_again");
            return finish_probe(8);
        }
    }
    const bool color_factory_ready = g_probe.renderer.glyph_text.color_glyphs;
    if (!probe_color_foreground_cache()) {
        log_write(LogLevel::error, "probe", "probe_color_foreground_cache()_failed");
        return 8;
    }
    // These two native-only oracle checks create extra atlas entries.
    if (!probe_empty_outline_cache()) {
        log_write(LogLevel::error, "probe", "probe_empty_outline_cache()_failed");
        return 8;
    }
    if (!probe_color_foreground_cache(true)) {
        log_write(LogLevel::error, "probe", "probe_color_foreground_cache(true)_failed");
        return 8;
    }
    const bool color_glyph_cached =
        g_probe.renderer.glyph_text.cache_builds > glyph_cache_builds_before_color;
    const bool login_restored = set_probe_login_view(login_view);
    if (!color_factory_ready || !color_glyph_cached || !login_restored) {
        log_u64(LogLevel::error, "probe", "color_emoji_factory_ready", color_factory_ready);
        log_u64(LogLevel::error, "probe", "color_emoji_cache_before", glyph_cache_builds_before_color);
        log_u64(LogLevel::error, "probe", "color_emoji_cache_after",
                g_probe.renderer.glyph_text.cache_builds);
        log_u64(LogLevel::error, "probe", "color_emoji_login_restored", login_restored);
        return finish_probe(8);
    }
    const LoginScene& login_scene = g_probe.login_scene;
    const UiRect password_field = login_scene_rectangle(login_scene, LoginElement::password_field);
    const i32 password_x = static_cast<i32>(((password_field.left + password_field.right) * 0.5f) *
                                            static_cast<float>(g_probe.renderer.dpi) / 96.0f);
    const i32 password_y = static_cast<i32>(((password_field.top + password_field.bottom) * 0.5f) *
                                            static_cast<float>(g_probe.renderer.dpi) / 96.0f);
    if (login_scene_hit_test(
            login_scene, static_cast<float>(password_x) * 96.0f / static_cast<float>(g_probe.renderer.dpi),
            static_cast<float>(password_y) * 96.0f / static_cast<float>(g_probe.renderer.dpi)) !=
        LoginControl::password) {
        return finish_probe(9);
    }
    RendererLoginView edit_view = login_view;
    edit_view.edit_anchor = 6;
    edit_view.edit_caret = 2;
    edit_view.edit_active = true;
    edit_view.focus = LoginControl::login;
    edit_view.status = RendererLoginStatus::idle;
    edit_view.qr_code = nullptr;
    if (!set_probe_login_view(edit_view) || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !g_probe.renderer.login_edit.active || g_probe.renderer.login_edit.selection_count == 0 ||
        g_probe.renderer.login_edit.caret.height <= 0.0f || !probe_login_edit_centered()) {
        return finish_probe(9);
    }
    const u32 original_login_dpi = g_probe.renderer.dpi;
    constexpr u32 login_dpis[]{96, 120, 144, 192};
    for (u32 dpi : login_dpis) {
        renderer_set_dpi(&g_probe.renderer, dpi);
        for (u32 sample = 0; sample < 4; ++sample) {
            RendererLoginView centered = edit_view;
            centered.focus = sample < 2 ? LoginControl::login : LoginControl::password;
            centered.login = sample == 1 ? utf8_literal("native") : Utf8View{};
            centered.password_mask = sample == 3 ? utf8_literal("••••") : Utf8View{};
            centered.edit_anchor = 0;
            centered.edit_caret = sample == 1 ? 4 : sample == 3 ? 4 : 0;
            TextCompositionSpan composition{};
            composition.start = 1;
            composition.end = 4;
            composition.line_style = TextCompositionLineStyle::solid;
            centered.edit_composition_active = sample == 1;
            centered.edit_composition = sample == 1 ? ConstSlice<TextCompositionSpan>{&composition, 1}
                                                    : ConstSlice<TextCompositionSpan>{};
            if (!set_probe_login_view(centered) || !probe_present(&g_probe.renderer, kDefaultTheme) ||
                !probe_login_edit_centered() ||
                (sample == 0 && dpi == 96 && !probe_capture_frame(L"login-caret-empty.bmp")) ||
                (sample == 1 && dpi == 96 && !probe_capture_frame(L"login-caret-selection.bmp"))) {
                log_write(LogLevel::error, "probe", "login_caret_alignment_failed");
                return finish_probe(9);
            }
        }
    }
    renderer_set_dpi(&g_probe.renderer, original_login_dpi);
    if (!set_probe_login_view(edit_view) || !probe_present(&g_probe.renderer, kDefaultTheme))
        return 9;
    log_write(LogLevel::info, "probe", "login_empty_password_selection_ime_carets_centered_at_all_dpis");
    const u32 login_edit_builds = g_probe.renderer.login_edit.builds;
    const u32 login_edit_hits = g_probe.renderer.login_edit.hits;
    if (!set_probe_login_view(edit_view) || g_probe.renderer.login_edit.builds != login_edit_builds ||
        g_probe.renderer.login_edit.hits != login_edit_hits + 1u) {
        return finish_probe(9);
    }
    const UiRect edit_field = login_scene_content_rectangle(g_probe.login_scene, LoginElement::login_field);
    const i32 edit_start_x =
        static_cast<i32>(edit_field.left * static_cast<float>(g_probe.renderer.dpi) / 96.0f);
    const i32 edit_end_x =
        static_cast<i32>((edit_field.right - 4.0f) * static_cast<float>(g_probe.renderer.dpi) / 96.0f);
    const i32 edit_y = static_cast<i32>((edit_field.top + ui_rect_height(edit_field) * 0.5f) *
                                        static_cast<float>(g_probe.renderer.dpi) / 96.0f);
    u32 edit_start = ~0u;
    u32 edit_end = 0;
    if (!renderer_login_edit_hit_test(g_probe.renderer, edit_field, edit_view.login, edit_start_x, edit_y,
                                      &edit_start) ||
        !renderer_login_edit_hit_test(g_probe.renderer, edit_field, edit_view.login, edit_end_x, edit_y,
                                      &edit_end) ||
        edit_start != 0 || edit_end != utf8_validate(edit_view.login).codepoints) {
        return finish_probe(9);
    }
    const u32 edit_paint_with_caret = g_probe.renderer.paint_stream.count;
    renderer_set_login_caret_visible(&g_probe.renderer, false);
    if (g_probe.renderer.login_caret_visible || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.paint_stream.count + 1 != edit_paint_with_caret) {
        return finish_probe(9);
    }
    renderer_set_login_caret_visible(&g_probe.renderer, true);
    if (!g_probe.renderer.login_caret_visible || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.paint_stream.count != edit_paint_with_caret) {
        return finish_probe(9);
    }
    // Synthetic scene projections exercise the shipping renderer/policy only;
    // they do not claim a live provider response or authentication cancellation.
    {
        RendererLoginView blocked{};
        blocked.visible = true;
        blocked.focus = LoginControl::back;
        blocked.status = RendererLoginStatus::captcha_required;
        const wchar_t* names[]{L"login-flow-password-captcha.bmp", L"login-flow-qr-captcha.bmp"};
        for (u32 sample = 0; sample < 2; ++sample) {
            if (sample == 1) {
                blocked.status = RendererLoginStatus::mfa_required;
                blocked.mfa_capabilities = login_mfa_capability_sms;
                blocked.selected_mfa_capability = login_mfa_capability_sms;
                blocked.remote_status = LoginRemoteStatus::captcha_required;
            }
            g_probe.login_scene.focus_visible = true;
            bool valid = set_probe_login_view(blocked);
            const auto& scene = g_probe.login_scene;
            const auto back = login_scene_rectangle(scene, LoginElement::forgot_password);
            const auto submit = login_scene_rectangle(scene, LoginElement::submit);
            const auto field = login_scene_rectangle(scene, LoginElement::login_field);
            const auto qr = login_scene_rectangle(scene, LoginElement::qr);
            // A challenge offers Verify in browser beside Back; credentials stay blocked.
            valid = valid && scene.back_only && !scene.submit_disabled && scene.captcha_action &&
                    scene.focus == LoginControl::back &&
                    login_scene_hit_test(scene, back.left + 1, back.top + 1) == LoginControl::back &&
                    login_scene_hit_test(scene, submit.left + 1, submit.top + 1) == LoginControl::submit &&
                    login_scene_hit_test(scene, field.left + 1, field.top + 1) == LoginControl::none &&
                    (!scene.qr_visible ||
                     login_scene_hit_test(scene, qr.left + 1, qr.top + 1) == LoginControl::none) &&
                    login_control_action(LoginControl::submit, blocked.status, blocked.remote_status) ==
                        LoginAction::captcha_verify &&
                    login_control_action(LoginControl::back, blocked.status, blocked.remote_status) ==
                        LoginAction::back;
            if (!valid || !probe_present(&g_probe.renderer, kDefaultTheme) ||
                !probe_capture_frame(names[sample])) {
                log_write(LogLevel::error, "probe", "login_blocked_flow_projection_failed");
                return finish_probe(9);
            }
        }
        RendererLoginView restored{};
        restored.visible = true;
        restored.focus = LoginControl::login;
        if (!set_probe_login_view(restored))
            return 9;
        const auto& scene = g_probe.login_scene;
        const auto recovery = login_scene_rectangle(scene, LoginElement::forgot_password);
        const auto field = login_scene_rectangle(scene, LoginElement::login_field);
        if (scene.back_only || scene.submit_disabled || scene.focus != LoginControl::login ||
            login_scene_hit_test(scene, field.left + 1, field.top + 1) != LoginControl::login ||
            login_scene_hit_test(scene, recovery.left + 1, recovery.top + 1) !=
                LoginControl::forgot_password ||
            login_control_action(LoginControl::forgot_password, restored.status, restored.remote_status) !=
                LoginAction::recovery ||
            !probe_present(&g_probe.renderer, kDefaultTheme) ||
            !probe_capture_frame(L"login-flow-restored-credentials.bmp")) {
            log_write(LogLevel::error, "probe", "login_restored_flow_projection_failed");
            return finish_probe(9);
        }
        log_write(LogLevel::info, "probe", "login_blocked_back_and_restored_projections_passed");
    }
    RendererLoginView mobile_view = login_view;
    mobile_view.remote_user = utf8_literal("doodles");
    mobile_view.status = RendererLoginStatus::remote_mobile_pending;
    mobile_view.focus = LoginControl::submit;
    if (!set_probe_login_view(mobile_view) || !probe_present(&g_probe.renderer, kDefaultTheme)) {
        return finish_probe(9);
    }
    RendererLoginView expired_view = mobile_view;
    expired_view.remote_user = {};
    expired_view.qr_code = nullptr;
    expired_view.status = RendererLoginStatus::remote_expired;
    expired_view.focus = LoginControl::qr;
    if (!set_probe_login_view(expired_view) || !probe_present(&g_probe.renderer, kDefaultTheme)) {
        return finish_probe(9);
    }
    const RendererLoginView mfa_view{utf8_literal("native.user@example.test"),
                                     {},
                                     {},
                                     0,
                                     0,
                                     false,
                                     LoginControl::mfa_sms,
                                     RendererLoginStatus::mfa_required,
                                     static_cast<u8>(login_mfa_capability_totp | login_mfa_capability_sms |
                                                     login_mfa_capability_backup |
                                                     login_mfa_capability_webauthn),
                                     login_mfa_capability_sms,
                                     0,
                                     false,
                                     nullptr,
                                     false,
                                     true,
                                     false,
                                     true};
    if (!set_probe_login_view(mfa_view)) {
        return finish_probe(9);
    }
    const LoginScene& mfa_scene = g_probe.login_scene;
    const UiRect mfa_backup = login_scene_rectangle(mfa_scene, LoginElement::mfa_backup);
    const i32 backup_x = static_cast<i32>(((mfa_backup.left + mfa_backup.right) * 0.5f) *
                                          static_cast<float>(g_probe.renderer.dpi) / 96.0f);
    const i32 backup_y = static_cast<i32>(((mfa_backup.top + mfa_backup.bottom) * 0.5f) *
                                          static_cast<float>(g_probe.renderer.dpi) / 96.0f);
    if (login_scene_hit_test(
            mfa_scene, static_cast<float>(backup_x) * 96.0f / static_cast<float>(g_probe.renderer.dpi),
            static_cast<float>(backup_y) * 96.0f / static_cast<float>(g_probe.renderer.dpi)) !=
        LoginControl::mfa_backup) {
        return finish_probe(9);
    }
    RendererLoginView backup_acknowledgement_view = mfa_view;
    backup_acknowledgement_view.focus = LoginControl::submit;
    backup_acknowledgement_view.status = RendererLoginStatus::mfa_backup_acknowledgement;
    backup_acknowledgement_view.mfa_capabilities = login_mfa_capability_backup;
    backup_acknowledgement_view.selected_mfa_capability = login_mfa_capability_backup;
    backup_acknowledgement_view.backup_acknowledgement_seconds = 10;
    backup_acknowledgement_view.sms_sent = false;
    if (!set_probe_login_view(backup_acknowledgement_view) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) || !g_probe.login_scene.credential_fields_hidden ||
        login_scene_element(g_probe.login_scene, LoginElement::password_field) ||
        !g_probe.login_scene.action_disabled) {
        return finish_probe(9);
    }
    if (!set_probe_login_view(RendererLoginView{{},
                                                {},
                                                {},
                                                0,
                                                0,
                                                false,
                                                LoginControl::none,
                                                RendererLoginStatus::idle,
                                                login_mfa_capability_none,
                                                login_mfa_capability_none,
                                                0,
                                                false,
                                                nullptr,
                                                false,
                                                false,
                                                false,
                                                false})) {
        return finish_probe(9);
    }
    // Recovery uses the standalone login renderer before account text is bound.
    if (!login_recovery_ui_probe(g_probe.renderer, probe_capture_frame, probe_login_recovery_text_pixels)) {
        log_write(LogLevel::error, "probe", "login_recovery_standalone_pixel_probe_failed");
        return finish_probe(9);
    }
    if (!login_reset_ui_probe(g_probe.renderer, probe_capture_frame, probe_login_recovery_text_pixels)) {
        log_write(LogLevel::error, "probe", "login_reset_standalone_pixel_probe_failed");
        return finish_probe(9);
    }
    if (!login_phone_ui_probe(g_probe.renderer, probe_capture_frame, probe_login_recovery_text_pixels)) {
        log_write(LogLevel::error, "probe", "login_phone_standalone_pixel_probe_failed");
        return finish_probe(9);
    }
    if (!login_browser_ui_probe(g_probe.renderer, probe_capture_frame, probe_login_recovery_text_pixels)) {
        log_write(LogLevel::error, "probe", "login_browser_standalone_pixel_probe_failed");
        return finish_probe(9);
    }
    if (!initialize_fixture(flag_surfaces_requested)) {
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return static_cast<int>(g_probe.failure_stage);
    }
    if (flag_surfaces_requested) {
        const bool popup_ready =
            popup_scene_initialize(&g_probe.popup_scene, {g_probe.popup_items, 1}, {g_probe.popup_rows, 1},
                                   {g_probe.popup_widgets, 2 + kPopupWidgetsPerItem},
                                   {g_probe.popup_layout_nodes, 2 + kPopupWidgetsPerItem}) &&
            renderer_bind_popup_scene(&g_probe.renderer, &g_probe.popup_scene);
        const bool valid =
            popup_ready &&
            regional_flag_surfaces_ui_probe(
                {g_probe.renderer, g_probe.shell_scene, g_probe.message_view, g_probe.rich_store,
                 g_probe.message_text, g_probe.message_scene, g_probe.message_composer,
                 g_probe.message_composer_scene, g_probe.popup_scene, g_probe.chat_state, g_probe.users,
                 g_probe.navigation, +[](u64 id, Utf8View text) noexcept { return add_message(id, text); },
                 +[]() noexcept {
                     return synchronize_probe_shell_layout() && rebuild_probe_content_text();
                 },
                 probe_capture_frame});
        renderer_shutdown(&g_probe.renderer);
        popup_scene_release(&g_probe.popup_scene);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 248;
    }
    if (!probe_reaction_picker_server_icon()) {
        log_write(LogLevel::error, "probe", "reaction_picker_server_icon_probe_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return static_cast<int>(g_probe.failure_stage);
    }
    if (!probe_popup_overlays()) {
        log_write(LogLevel::error, "probe", "popup_overlay_probe_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return static_cast<int>(g_probe.failure_stage);
    }
    MemberListScene stamp_member_scene{};
    ReactionViewer stamp_reaction_viewer{};
    const MemberListScene* bound_member_scene = g_probe.renderer.member_list_scene;
    const ReactionViewer* bound_reaction_viewer = g_probe.renderer.reaction_viewer;
    g_probe.renderer.member_list_scene = &stamp_member_scene;
    g_probe.renderer.reaction_viewer = &stamp_reaction_viewer;
    const RendererPresentationStamp complete_stamp =
        renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme);
    const RendererPresentationStamp repeated_stamp =
        renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme);
    ++stamp_member_scene.source_revision;
    const RendererPresentationStamp changed_stamp =
        renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme);
    --stamp_member_scene.source_revision;
    ++stamp_member_scene.widgets.layout.revision;
    const RendererPresentationStamp scrolled_widget_stamp =
        renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme);
    g_probe.renderer.member_list_scene = bound_member_scene;
    g_probe.renderer.reaction_viewer = bound_reaction_viewer;
    if (!complete_stamp.valid || complete_stamp.count <= 192u ||
        !renderer_presentation_stamp_equal(complete_stamp, repeated_stamp) ||
        renderer_presentation_stamp_equal(complete_stamp, changed_stamp) ||
        renderer_presentation_stamp_equal(complete_stamp, scrolled_widget_stamp)) {
        log_u64(LogLevel::error, "probe", "presentation_stamp_count", complete_stamp.count);
        return finish_probe(4);
    }
    if (!probe_present(&g_probe.renderer, kDefaultTheme)) {
        return finish_probe(4);
    }
    if (!media_pins_match_visible_quads()) {
        log_u64(LogLevel::error, "probe", "offscreen_media_pinned_or_queued", 1);
        return finish_probe(4);
    }
    const u32 retained_text_uploads = g_probe.renderer.text_instance_uploads;
    const u32 retained_paint_uploads = g_probe.renderer.paint_instance_uploads;
    const u32 retained_media_uploads = g_probe.renderer.media_instance_uploads;
    const u32 retained_paint_builds = g_probe.renderer.paint_stream_builds;
    const u32 retained_paint_reuses = g_probe.renderer.paint_stream_reuses;
    const u32 retained_media_refreshes = g_probe.renderer.media_source_refreshes;
    const u32 retained_media_upload_skips = g_probe.renderer.media_source_upload_skips;
    const u32 retained_full_presents = g_probe.renderer.full_present_draws;
    const u32 retained_partial_presents = g_probe.renderer.partial_present_draws;
    const u32 retained_unchanged_skips = g_probe.renderer.unchanged_present_skips;
    const u32 media_draws_total_before = g_probe.renderer.media_draws_total;
    if (!g_probe.renderer.pipeline.media_instance_buffer ||
        g_probe.renderer.pipeline.media_instance_buffer ==
            g_probe.renderer.pipeline.glyph_instance_buffer ||
        g_probe.renderer.uploaded_text_storage != &g_probe.text || retained_text_uploads == 0 ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.text_instance_uploads != retained_text_uploads ||
        g_probe.renderer.paint_instance_uploads != retained_paint_uploads ||
        g_probe.renderer.media_instance_uploads != retained_media_uploads ||
        g_probe.renderer.paint_stream_builds != retained_paint_builds ||
        g_probe.renderer.paint_stream_reuses != retained_paint_reuses ||
        g_probe.renderer.media_source_refreshes != retained_media_refreshes ||
        g_probe.renderer.media_source_upload_skips != retained_media_upload_skips ||
        g_probe.renderer.media_draws_total != media_draws_total_before ||
        g_probe.renderer.media_draws_last_frame != 0 ||
        g_probe.renderer.full_present_draws != retained_full_presents ||
        g_probe.renderer.partial_present_draws != retained_partial_presents ||
        g_probe.renderer.unchanged_present_skips != retained_unchanged_skips + 1u) {
        log_u64(LogLevel::error, "probe", "retained_presentation_stage", 1);
        log_u64(LogLevel::error, "probe", "retained_reuses", g_probe.renderer.paint_stream_reuses);
        log_u64(LogLevel::error, "probe", "retained_refreshes", g_probe.renderer.media_source_refreshes);
        log_u64(LogLevel::error, "probe", "retained_unchanged_skips",
                g_probe.renderer.unchanged_present_skips);
        return finish_probe(4);
    }
    MediaAtlasResult cached_visible_source{};
    MediaAssetKey cached_visible_key{};
    for (u32 index = 0; index < g_probe.renderer.media_quad_count; ++index) {
        MediaAtlasResult candidate{};
        const MediaAtlasHandle handle = g_probe.renderer.media_draw.handle_data[index];
        if (g_probe.renderer.media_draw.quad_data[index].u0 >= 0.0f &&
            media_atlas_validate(g_probe.media_atlas, handle, &candidate) &&
            candidate.width <= kAnimationFrameCacheGpuMaximumFrameExtent &&
            candidate.height <= kAnimationFrameCacheGpuMaximumFrameExtent) {
            cached_visible_source = candidate;
            cached_visible_key = g_probe.media_atlas.entries[handle.slot].key;
            break;
        }
    }
    const AnimationFrameCacheDescriptor retained_frame{AnimationFrameCacheKey{cached_visible_key, 0, 61, 0},
                                                       AnimationFrameCacheControl{},
                                                       100'000,
                                                       0,
                                                       cached_visible_source.width,
                                                       cached_visible_source.height,
                                                       AnimationFrameCacheKind::gif,
                                                       {0, 0, 0}};
    const u32 media_only_uploads = g_probe.renderer.media_instance_uploads;
    const u32 media_only_reuses = g_probe.renderer.paint_stream_reuses;
    const u32 media_only_refreshes = g_probe.renderer.media_source_refreshes;
    const u32 media_only_full_presents = g_probe.renderer.full_present_draws;
    const u32 media_only_partial_presents = g_probe.renderer.partial_present_draws;
    const u64 media_only_partial_pixels = g_probe.renderer.partial_present_pixels;
    if (!cached_visible_source.ok() ||
        !renderer_capture_animation_frame(&g_probe.renderer, retained_frame) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.paint_stream_builds != retained_paint_builds ||
        g_probe.renderer.paint_stream_reuses != media_only_reuses + 1u ||
        g_probe.renderer.paint_instance_uploads != retained_paint_uploads ||
        g_probe.renderer.text_instance_uploads != retained_text_uploads ||
        g_probe.renderer.media_instance_uploads != media_only_uploads + 1u ||
        g_probe.renderer.media_source_refreshes != media_only_refreshes + 1u ||
        // The previous structural frame was full. The first media-only frame
        // must also be full so the second flip-sequential back buffer catches
        // up before partial damage begins.
        g_probe.renderer.full_present_draws != media_only_full_presents + 1u ||
        g_probe.renderer.partial_present_draws != media_only_partial_presents ||
        g_probe.renderer.partial_present_pixels != media_only_partial_pixels ||
        g_probe.renderer.media_draws_last_frame == 0) {
        log_u64(LogLevel::error, "probe", "retained_presentation_stage", 2);
        log_u64(LogLevel::error, "probe", "media_only_full_presents", g_probe.renderer.full_present_draws);
        log_u64(LogLevel::error, "probe", "media_only_partial_presents",
                g_probe.renderer.partial_present_draws);
        log_u64(LogLevel::error, "probe", "media_only_partial_pixels",
                g_probe.renderer.partial_present_pixels);
        log_u64(LogLevel::error, "probe", "media_only_draws_last", g_probe.renderer.media_draws_last_frame);
        return finish_probe(4);
    }
    const u32 stable_media_uploads = g_probe.renderer.media_instance_uploads;
    const u32 stable_media_skips = g_probe.renderer.media_source_upload_skips;
    const u32 stable_unchanged_skips = g_probe.renderer.unchanged_present_skips;
    const u32 stable_partial_presents = g_probe.renderer.partial_present_draws;
    const u32 stable_media_draws_total = g_probe.renderer.media_draws_total;
    if (!probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.paint_stream_builds != retained_paint_builds ||
        g_probe.renderer.paint_instance_uploads != retained_paint_uploads ||
        g_probe.renderer.text_instance_uploads != retained_text_uploads ||
        g_probe.renderer.media_instance_uploads != stable_media_uploads ||
        g_probe.renderer.media_source_upload_skips != stable_media_skips ||
        g_probe.renderer.partial_present_draws != stable_partial_presents ||
        g_probe.renderer.media_draws_total != stable_media_draws_total ||
        g_probe.renderer.media_draws_last_frame != 0 ||
        g_probe.renderer.unchanged_present_skips != stable_unchanged_skips + 1u) {
        log_u64(LogLevel::error, "probe", "retained_presentation_stage", 3);
        return finish_probe(4);
    }
    MediaAtlasResult live_visible_source{};
    MediaAssetKey live_visible_key{};
    for (u32 index = 0; index < g_probe.renderer.media_quad_count; ++index) {
        MediaAtlasResult candidate{};
        const MediaAtlasHandle handle = g_probe.renderer.media_draw.handle_data[index];
        if (g_probe.renderer.media_draw.quad_data[index].u0 >= 0.0f &&
            media_atlas_validate(g_probe.media_atlas, handle, &candidate) &&
            candidate.width <= kAnimationFrameCacheGpuMaximumFrameExtent &&
            candidate.height <= kAnimationFrameCacheGpuMaximumFrameExtent) {
            live_visible_source = candidate;
            live_visible_key = g_probe.media_atlas.entries[handle.slot].key;
            break;
        }
    }
    u8 live_pixels[kAnimationFrameCacheGpuMaximumFrameExtent *
                   kAnimationFrameCacheGpuMaximumFrameExtent * 4]{};
    const u32 live_row_pitch = static_cast<u32>(live_visible_source.width) * 4u;
    const u32 live_pixel_bytes = live_row_pitch * static_cast<u32>(live_visible_source.height);
    for (u32 index = 0; index < live_pixel_bytes; index += 4) {
        live_pixels[index + 0] = 32;
        live_pixels[index + 1] = 96;
        live_pixels[index + 2] = 160;
        live_pixels[index + 3] = 255;
    }
    const u32 live_media_uploads = g_probe.renderer.media_instance_uploads;
    const u32 live_partial_presents = g_probe.renderer.partial_present_draws;
    const u32 live_full_presents = g_probe.renderer.full_present_draws;
    const u64 live_partial_pixels = g_probe.renderer.partial_present_pixels;
    const u32 live_gpu_uploads = g_probe_rgba8_uploads;
    const MediaAtlasGpuResult live_uploaded =
        live_visible_source.ok()
            ? renderer_upload_media_rgba8(&g_probe.renderer, live_visible_key, live_visible_source.width,
                                          live_visible_source.height, live_row_pitch,
                                          ConstSlice<u8>{live_pixels, live_pixel_bytes})
            : MediaAtlasGpuResult{};
    if (!live_visible_source.ok() || !live_uploaded.ok() ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.media_instance_uploads != live_media_uploads ||
        g_probe.renderer.partial_present_draws != live_partial_presents + 1u ||
        g_probe.renderer.full_present_draws != live_full_presents ||
        g_probe.renderer.partial_present_pixels <= live_partial_pixels ||
        g_probe.renderer.partial_present_pixels - live_partial_pixels >=
            static_cast<u64>(g_probe.renderer.width) * g_probe.renderer.height ||
        g_probe.renderer.media_draws_last_frame == 0 ||
        g_probe_rgba8_uploads != live_gpu_uploads + 1u) {
        log_u64(LogLevel::error, "probe", "retained_presentation_stage", 4);
        return finish_probe(4);
    }
    const u32 catch_up_partial_presents = g_probe.renderer.partial_present_draws;
    const u32 catch_up_idle_presents = g_probe.renderer.idle_begin_frame_presents;
    const u64 catch_up_partial_pixels = g_probe.renderer.partial_present_pixels;
    renderer_accept_begin_frame(&g_probe.renderer);
    if (!probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.partial_present_draws != catch_up_partial_presents + 1u ||
        g_probe.renderer.idle_begin_frame_presents != catch_up_idle_presents + 1u ||
        g_probe.renderer.partial_present_pixels <= catch_up_partial_pixels ||
        g_probe.renderer.media_draws_last_frame == 0 || g_probe.renderer.previous_buffer_damage_valid) {
        log_u64(LogLevel::error, "probe", "retained_presentation_stage", 5);
        return finish_probe(4);
    }
    const u32 post_live_unchanged_skips = g_probe.renderer.unchanged_present_skips;
    const u32 post_live_media_draws_total = g_probe.renderer.media_draws_total;
    if (!probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.unchanged_present_skips != post_live_unchanged_skips + 1u ||
        g_probe.renderer.media_draws_total != post_live_media_draws_total ||
        g_probe.renderer.media_draws_last_frame != 0) {
        log_u64(LogLevel::error, "probe", "retained_presentation_stage", 6);
        return finish_probe(4);
    }
    if (g_probe.text.message_geometry_quads < 6) {
        return finish_probe(5);
    }
    // One live media upload and the reaction picker's server-icon fixture.
    if (g_probe.renderer.media_draws_last_frame != 0 || g_probe_rgba8_uploads != 2 ||
        g_probe.renderer.media_gpu.png_uploads != 2 ||
        g_probe.renderer.media_gpu.png_uploaded_bytes !=
            (kProbeStickerExtent * 4 + 1) * kProbeStickerExtent * 2 ||
        g_probe.renderer.media_gpu.png_dispatched_pixels != kProbeStickerExtent * kProbeStickerExtent * 2 ||
        g_probe.renderer.media_gpu.apng_frames != 4 || g_probe.renderer.media_gpu.apng_source_blends != 3 ||
        g_probe.renderer.media_gpu.apng_over_blends != 1 ||
        g_probe.renderer.media_gpu.apng_background_disposals != 1 ||
        g_probe.renderer.media_gpu.apng_previous_disposals != 1 ||
        g_probe.renderer.media_gpu.apng_dispatched_pixels != 25 ||
        g_probe.renderer.media_gpu.gif_frames != 4 || g_probe.renderer.media_gpu.gif_uploaded_bytes != 29 ||
        g_probe.renderer.media_gpu.gif_dispatched_pixels != 29 ||
        g_probe.renderer.media_gpu.gif_background_disposals != 1 ||
        g_probe.renderer.media_gpu.gif_previous_disposals != 1 ||
        g_probe.renderer.media_gpu.animated_webp_frames != 7 ||
        g_probe.renderer.media_gpu.animated_webp_source_blends != 4 ||
        g_probe.renderer.media_gpu.animated_webp_over_blends != 3 ||
        g_probe.renderer.media_gpu.animated_webp_uploaded_bytes != 208 ||
        g_probe.renderer.media_gpu.animated_webp_dispatched_pixels != 80 ||
        g_probe.renderer.media_gpu.animated_webp_background_disposals != 2 ||
        g_probe.renderer.media_gpu.jpeg_uploads != 20 ||
        g_probe.renderer.media_gpu.jpeg_uploaded_bytes != 29'254 ||
        g_probe.renderer.media_gpu.jpeg_reconstructed_pixels != 9'152 ||
        g_probe.renderer.media_gpu.jpeg_idct_dispatches != 20 ||
        g_probe.renderer.media_gpu.jpeg_atlas_dispatches != 20 ||
        !g_probe.renderer.media_gpu.backup) {
        log_u64(LogLevel::error, "probe", "media_draws_last_frame",
                g_probe.renderer.media_draws_last_frame);
        log_u64(LogLevel::error, "probe", "media_uploads", g_probe_rgba8_uploads);
        log_u64(LogLevel::error, "probe", "png_uploads", g_probe.renderer.media_gpu.png_uploads);
        log_u64(LogLevel::error, "probe", "png_uploaded_bytes",
                g_probe.renderer.media_gpu.png_uploaded_bytes);
        log_u64(LogLevel::error, "probe", "png_dispatched_pixels",
                g_probe.renderer.media_gpu.png_dispatched_pixels);
        log_u64(LogLevel::error, "probe", "apng_frames", g_probe.renderer.media_gpu.apng_frames);
        log_u64(LogLevel::error, "probe", "gif_frames", g_probe.renderer.media_gpu.gif_frames);
        log_u64(LogLevel::error, "probe", "animated_webp_frames",
                g_probe.renderer.media_gpu.animated_webp_frames);
        log_u64(LogLevel::error, "probe", "jpeg_uploads", g_probe.renderer.media_gpu.jpeg_uploads);
        log_u64(LogLevel::error, "probe", "jpeg_uploaded_bytes",
                g_probe.renderer.media_gpu.jpeg_uploaded_bytes);
        log_u64(LogLevel::error, "probe", "jpeg_reconstructed_pixels",
                g_probe.renderer.media_gpu.jpeg_reconstructed_pixels);
        PaintStream media_probe_paint{};
        paint_stream_reset(&media_probe_paint, static_cast<float>(g_probe.renderer.width),
                           static_cast<float>(g_probe.renderer.height));
        MessageMediaCommand media_probe_commands[8]{};
        MessageMediaCommandStream media_probe_stream{};
        MessagePresentationResult media_probe_result{};
        if (message_media_command_stream_initialize(&media_probe_stream,
                                                    Slice<MessageMediaCommand>{media_probe_commands, 8}) &&
            message_presentation_build(g_probe.message_scene, kDefaultTheme, &media_probe_paint,
                                       &media_probe_stream, &media_probe_result)) {
            log_u64(LogLevel::error, "probe", "message_media_commands", media_probe_result.media_commands);
            for (u32 index = 0; index < media_probe_stream.count; ++index) {
                log_u64(LogLevel::error, "probe", "message_media_kind",
                        static_cast<u64>(media_probe_commands[index].kind));
                log_u64(LogLevel::error, "probe", "message_media_id", media_probe_commands[index].id);
                log_u64(LogLevel::error, "probe", "message_media_format",
                        media_probe_commands[index].format);
                log_u64(LogLevel::error, "probe", "message_media_animated",
                        media_probe_commands[index].animated ? 1u : 0u);
            }
        }
        for (u32 index = 0; index < g_probe.media_atlas.entry_capacity; ++index) {
            const MediaAtlasEntry& entry = g_probe.media_atlas.entries[index];
            if (!entry.occupied) {
                continue;
            }
            log_u64(LogLevel::error, "probe", "media_atlas_kind", static_cast<u64>(entry.key.kind));
            log_u64(LogLevel::error, "probe", "media_atlas_id", entry.key.id);
            log_u64(LogLevel::error, "probe", "media_atlas_variant", entry.key.variant);
            log_u64(LogLevel::error, "probe", "media_atlas_flags", entry.key.flags);
        }
        return finish_probe(6);
    }
    if (!video_retention_probe(g_probe.renderer, live_visible_key, live_visible_source.width,
                               live_visible_source.height)) {
        return finish_probe(10);
    }
    if (!animation_preview_probe(g_probe.renderer)) {
        return finish_probe(11);
    }
    if (once_requested()) {
        {
            // The picker glyph cache's lookup hint, exact-key matching and cursor wrap.
            auto* picker_cache = static_cast<PickerGlyphCache*>(
                VirtualAlloc(nullptr, sizeof(PickerGlyphCache), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            const bool lookup_valid = picker_cache && picker_glyph_cache_lookup_probe(*picker_cache);
            if (picker_cache)
                VirtualFree(picker_cache, 0, MEM_RELEASE);
            if (!lookup_valid) {
                log_write(LogLevel::error, "probe", "picker_glyph_cache_lookup_failed");
                return 14;
            }
        }
        if (!shaped_text_cache_equivalence())
            return 18;
        if (!vp8_reconstruct_probe(g_probe.renderer))
            return 1;
        if (!video_frame_reference_probe(g_probe.renderer))
            return 1;
        if (!gif_scroll_probe(g_probe.renderer, true)) {
            return finish_probe(12);
        }
        if (!message_scene_navigate(&g_probe.message_scene, UiListMove::first).message_id ||
            !rebuild_probe_content_text())
            return 13;
        renderer_bind_focus_owner(&g_probe.renderer, &g_probe.message_scene.widgets);
        if (!probe_present(&g_probe.renderer, kDefaultTheme))
            return 13;
        const auto message_page = message_scene_page(&g_probe.message_scene, false);
        UiRect page_focus{};
        if (!message_page.message_id || !rebuild_probe_content_text() ||
            !probe_present(&g_probe.renderer, kDefaultTheme) ||
            !ui_widget_manager_context_anchor(g_probe.message_scene.widgets, &page_focus))
            return 13;
        renderer_bind_focus_owner(&g_probe.renderer, nullptr);
        log_write(LogLevel::info, "probe", "message_page_focus_hardware_passed");
        if (!probe_chat_name_styles())
            return 236;
        if (!probe_message_display())
            return 731;
        if (!probe_plugin_surfaces())
            return 7321;
        if (!member_paging_probe(g_probe.renderer, g_probe.text_measure_cache, probe_capture_frame))
            return 237;
        if (!thread_members_probe(g_probe.renderer, g_probe.text_measure_cache, probe_capture_frame))
            return 238;
        if (!read_state_probe(g_probe.renderer, g_probe.text_measure_cache, probe_capture_frame))
            return 239;
        log_u64(LogLevel::info, "member_paging", "account_ui_bytes", sizeof(app_detail::AccountUiRuntime));
        log_u64(LogLevel::info, "member_paging", "account_ui_added_bytes",
                app_detail::kAccountUiMemberPagingBytes);
        log_u64(LogLevel::info, "thread_members", "thread_owner_bytes",
                app_detail::kAccountUiThreadMemberBytes);
        log_u64(LogLevel::info, "thread_members", "thread_store_bytes", sizeof(ThreadMemberStore));
        log_u64(LogLevel::info, "thread_members", "subscription_bytes", sizeof(GatewayGuildSubscription));
        log_u64(LogLevel::info, "read_state", "fixed_ui_added_bytes", app_detail::kAccountUiReadStateBytes);
        // Exercise empty Home and a selected private conversation with the real
        // D3D11 chrome, text cache, and pixel-to-DIP hit-test path before teardown.
        message_composer_reset(&g_probe.message_composer, 0, false);
        if (!chat_state_store_select_home(&g_probe.chat_state).ok() ||
            !navigation_view_rebuild(&g_probe.navigation, g_probe.chat_state).ok() ||
            !message_timeline_reset(&g_probe.message_view.timeline) || !synchronize_probe_shell_layout() ||
            !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
            g_probe.message_composer_scene.active)
            return 13;
        const auto* home = ui_scene_node(g_probe.shell_scene, UiSemantic::home_button);
        if (!home)
            return 13;
        const float pixel_scale = static_cast<float>(g_probe.renderer.dpi) / 96.0f;
        const i32 home_x =
            static_cast<i32>((home->rectangle.left + home->rectangle.right) * 0.5f * pixel_scale);
        const i32 home_y =
            static_cast<i32>((home->rectangle.top + home->rectangle.bottom) * 0.5f * pixel_scale);
        const auto home_hit = renderer_navigation_hit_test(g_probe.renderer, home_x, home_y);
        if (home_hit.kind != RendererNavigationHitKind::home || !home_hit.selectable ||
            renderer_guild_rail_hit_test(g_probe.renderer, home_x, home_y))
            return 13;
        const u32 unfocused_quads = g_probe.renderer.paint_stream.count;
        if (!ui_widget_manager_focus(
                &g_probe.shell_scene.widgets,
                g_probe.shell_scene.semantics[static_cast<u32>(UiSemantic::home_button)]))
            return 13;
        renderer_bind_focus_owner(&g_probe.renderer, &g_probe.shell_scene.widgets);
        if (!probe_present(&g_probe.renderer, kDefaultTheme) ||
            g_probe.renderer.paint_stream.count != unfocused_quads + 1)
            return 13;
        bool focus_outline = false;
        for (u32 i = 0; i < g_probe.renderer.paint_stream.count; ++i) {
            const auto& quad = g_probe.renderer.paint_stream.quads[i];
            if (quad.left == home->rectangle.left && quad.top == home->rectangle.top &&
                quad.right == home->rectangle.right && quad.bottom == home->rectangle.bottom &&
                quad.fill.alpha == 0 && quad.border_width == 2 &&
                quad.border.blue == kDefaultTheme.accent.blue)
                focus_outline = true;
        }
        const u32 focus_builds = g_probe.renderer.paint_stream_builds;
        const u32 focus_uploads = g_probe.renderer.paint_instance_uploads;
        if (!focus_outline || !probe_present(&g_probe.renderer, kDefaultTheme) ||
            g_probe.renderer.paint_stream_builds != focus_builds ||
            g_probe.renderer.paint_instance_uploads != focus_uploads)
            return 13;
        renderer_bind_focus_owner(&g_probe.renderer, nullptr);
        if (!probe_present(&g_probe.renderer, kDefaultTheme) ||
            g_probe.renderer.paint_stream.count != unfocused_quads)
            return 13;
        log_write(LogLevel::info, "probe", "focus_outline_hardware_and_retention_passed");
        message_composer_reset(&g_probe.message_composer, 9999, false);
        if (!chat_state_store_upsert_channel(&g_probe.chat_state,
                                                {9999, 0, 0, utf8_literal("Native group chat"), 0, 0, 3})
                 .ok() ||
            !navigation_view_rebuild(&g_probe.navigation, g_probe.chat_state).ok() ||
            !synchronize_probe_shell_layout() || !rebuild_probe_content_text() ||
            !probe_present(&g_probe.renderer, kDefaultTheme) || !g_probe.message_composer_scene.active ||
            g_probe.shell_navigation_scene.count != 1)
            return 13;
        log_write(LogLevel::info, "probe", "private_home_chrome_and_hit_test_passed");
        {
            message_composer_insert_utf8(
                &g_probe.message_composer,
                utf8_literal("\xE3\x81\x8B\xF0\x9F\x98\x80\nRTL \xD7\x90\xD7\x91"));
            g_probe.message_composer.focused = true;
            ++g_probe.message_composer.revision;
            message_composer_scene_set_caret_visible(&g_probe.message_composer_scene, true);
            if (!synchronize_probe_shell_layout() || !rebuild_probe_content_text() ||
                !probe_present(&g_probe.renderer, kDefaultTheme))
                return 13;
            const u32 builds = g_probe.renderer.composer_layout.builds;
            const u32 paints = g_probe.renderer.paint_stream_builds;
            TextCompositionSpan span{};
            span.start = 0;
            span.end = 3;
            span.line_style = TextCompositionLineStyle::solid;
            span.line_rgb = 0x12E43F;
            span.background_rgb = 0x020408;
            span.color_flags = text_composition_color_line | text_composition_color_background;
            g_probe.message_composer_scene.composition = {&span, 1};
            ++g_probe.message_composer_scene.composition_revision;
            if (!probe_present(&g_probe.renderer, kDefaultTheme) ||
                g_probe.renderer.paint_stream_builds != paints + 1 ||
                g_probe.renderer.composer_layout.builds != builds)
                return 13;
            bool underline = false;
            u32 background_index = ~0u, caret_index = ~0u;
            for (u32 i = 0; i < g_probe.renderer.paint_stream.count; ++i) {
                const auto& quad = g_probe.renderer.paint_stream.quads[i];
                if (quad.fill.red > 0.007f && quad.fill.red < 0.008f && quad.fill.green > 0.015f &&
                    quad.fill.green < 0.016f && quad.fill.blue > 0.031f && quad.fill.blue < 0.032f)
                    background_index = i;
                const float caret_pixels = (quad.right - quad.left) * g_probe.renderer.dpi / 96.0f;
                if (caret_pixels > .999f && caret_pixels < 1.001f &&
                    quad.fill.red == kDefaultTheme.text_primary.red &&
                    quad.fill.green == kDefaultTheme.text_primary.green &&
                    quad.fill.blue == kDefaultTheme.text_primary.blue && quad.bottom - quad.top > 5)
                    caret_index = i;
                underline = underline || (quad.fill.red > 0.070f && quad.fill.red < 0.071f &&
                                          quad.fill.green > 0.894f && quad.fill.green < 0.895f &&
                                          quad.fill.blue > 0.247f && quad.fill.blue < 0.248f &&
                                          quad.bottom - quad.top == 1 && quad.right > quad.left);
            }
            if (!underline || background_index == ~0u || caret_index == ~0u ||
                background_index >= caret_index)
                log_write(LogLevel::error, "probe", "composer_ime_marks_or_caret_order_failed");
            if (!underline || background_index == ~0u || caret_index == ~0u ||
                background_index >= caret_index || !probe_present(&g_probe.renderer, kDefaultTheme) ||
                g_probe.renderer.paint_stream_builds != paints + 1)
                return 13;
            g_probe.message_composer_scene.composition = {};
            ++g_probe.message_composer_scene.composition_revision;
            if (!probe_present(&g_probe.renderer, kDefaultTheme) ||
                g_probe.renderer.paint_stream_builds != paints + 2 ||
                g_probe.renderer.composer_layout.builds != builds)
                return 13;
            message_composer_reset(&g_probe.message_composer, 9999, false);
            g_probe.message_composer.focused = false;
            if (!synchronize_probe_shell_layout() || !rebuild_probe_content_text())
                return 13;
            log_write(LogLevel::info, "probe", "composer_ime_underlines_and_retained_layout_passed");
        }
        const MessageComposerControl media_controls[]{
            MessageComposerControl::gif, MessageComposerControl::sticker, MessageComposerControl::emoji};
        UiWidgetHandle previous_control = g_probe.message_composer_scene.edit;
        renderer_bind_focus_owner(&g_probe.renderer, &g_probe.message_composer_scene.widgets);
        for (const auto control : media_controls) {
            const auto next = ui_widget_manager_next_focus(g_probe.message_composer_scene.widgets,
                                                           previous_control, false, false);
            const auto expected =
                control == MessageComposerControl::gif       ? g_probe.message_composer_scene.gif
                : control == MessageComposerControl::sticker ? g_probe.message_composer_scene.sticker
                                                             : g_probe.message_composer_scene.emoji;
            if (next.slot != expected.slot || next.generation != expected.generation ||
                !message_composer_scene_focus_control(&g_probe.message_composer_scene, control) ||
                !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme)) {
                log_u64(LogLevel::error, "probe", "composer_media_tab_or_present_failed",
                        static_cast<u8>(control));
                return 13;
            }
            const auto* node = message_composer_scene_layout_node(g_probe.message_composer_scene, control);
            bool focus_painted = false;
            for (u32 i = 0; node && i < g_probe.renderer.paint_stream.count; ++i) {
                const auto& quad = g_probe.renderer.paint_stream.quads[i];
                if (quad.left == node->rectangle.left && quad.top == node->rectangle.top &&
                    quad.right == node->rectangle.right && quad.bottom == node->rectangle.bottom &&
                    quad.fill.alpha == 0 && quad.border_width == 2)
                    focus_painted = true;
            }
            if (!focus_painted) {
                log_u64(LogLevel::error, "probe", "composer_media_focus_outline_missing",
                        static_cast<u8>(control));
                return 13;
            }
            previous_control = expected;
        }
        log_write(LogLevel::info, "probe", "composer_gif_sticker_emoji_tab_and_native_focus_passed");
        if (g_probe.message_composer.bytes != 0 ||
            message_composer_insert_utf8(&g_probe.message_composer, utf8_literal("Keyboard draft")) !=
                MessageComposerResult::changed ||
            !synchronize_probe_shell_layout() || !rebuild_probe_content_text())
            return 13;
        const auto send_tab = ui_widget_manager_next_focus(
            g_probe.message_composer_scene.widgets, g_probe.message_composer_scene.emoji, false, false);
        if (send_tab.slot != g_probe.message_composer_scene.send.slot ||
            !message_composer_scene_focus_control(&g_probe.message_composer_scene,
                                                  MessageComposerControl::send) ||
            !probe_present(&g_probe.renderer, kDefaultTheme))
            return 13;
        renderer_bind_focus_owner(&g_probe.renderer, nullptr);
        log_write(LogLevel::info, "probe", "composer_button_tab_and_focus_paint_passed");
        message_composer_reset(&g_probe.message_composer, 9999, false);
        if (!synchronize_probe_shell_layout(
                {}, {utf8_literal("photo.png · Ready"), true, true}) ||
            !message_composer_scene_can_send(g_probe.message_composer_scene) ||
            !message_composer_scene_focus_control(&g_probe.message_composer_scene,
                                                  MessageComposerControl::attachments) ||
            !rebuild_probe_content_text())
            return 13;
        renderer_bind_focus_owner(&g_probe.renderer, &g_probe.message_composer_scene.widgets);
        if (!probe_present(&g_probe.renderer, kDefaultTheme))
            return 13;
        const auto* attachment_node = message_composer_scene_layout_node(
            g_probe.message_composer_scene, MessageComposerControl::attachments);
        bool attachment_focus_painted = false;
        for (u32 i = 0; attachment_node && i < g_probe.renderer.paint_stream.count; ++i) {
            const auto& quad = g_probe.renderer.paint_stream.quads[i];
            if (quad.left == attachment_node->rectangle.left &&
                quad.top == attachment_node->rectangle.top &&
                quad.right == attachment_node->rectangle.right &&
                quad.bottom == attachment_node->rectangle.bottom && quad.fill.alpha == 0 &&
                quad.border_width == 2)
                attachment_focus_painted = true;
        }
        renderer_bind_focus_owner(&g_probe.renderer, nullptr);
        if (!attachment_focus_painted || !synchronize_probe_shell_layout() ||
            !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
            message_composer_scene_can_send(g_probe.message_composer_scene))
            return 13;
        log_write(LogLevel::info, "probe", "composer_attachment_summary_hardware_focus_and_removal_passed");
        MessageComposerController local_send{};
        MessageDraftStore local_drafts{};
        MessageDraftRecord local_draft_records[2]{};
        u8 local_draft_text[128]{}, local_request_text[kMessageComposerMaximumBytes]{};
        const UploadedAttachment local_file{
            utf8_literal("pending-report.txt"), utf8_literal("upload/pending-report.txt"), {}, 0, false};
        const u64 local_size = 4096;
        if (!message_draft_store_initialize(&local_drafts, {local_draft_records, 2},
                                            {local_draft_text, sizeof(local_draft_text)}) ||
            !message_composer_controller_initialize(&local_send, &g_probe.message_composer, &local_drafts,
                                                    &g_probe.message_view.timeline,
                                                    {local_request_text, sizeof(local_request_text)}))
            return 13;
        const auto pending_file = message_composer_controller_queue_send(
            &local_send, true, 123, 1725012296789ull, 100, 0, {&local_file, 1}, {&local_size, 1});
        bool restored_outbox = false;
        if (!message_timeline_reset(&g_probe.message_view.timeline) ||
            !message_timeline_set_scroll(&g_probe.message_view.timeline, 0, true) ||
            !message_composer_controller_restore_local(&local_send, 9999, &restored_outbox) ||
            !restored_outbox)
            return 13;
        if (!pending_file.ok() || !rebuild_probe_content_text() ||
            !probe_present(&g_probe.renderer, kDefaultTheme))
            return 13;
        auto* local_row = message_scene_find_row(&g_probe.message_scene, 0);
        if (!local_row || local_row->attachment_item_count != 1 ||
            g_probe.message_text.rich.attachments != 1)
            return 13;
        const auto* file_node = message_scene_rich_item_layout_node(
            g_probe.message_scene, *local_row, MessageSceneRichItemKind::attachment, 0);
        const auto* file_widget = ui_widget_manager_resolve(
            g_probe.message_scene.widgets,
            message_scene_rich_item_widget(*local_row, MessageSceneRichItemKind::attachment, 0));
        if (!file_widget || (file_widget->flags & (ui_widget_hit_test | ui_widget_focusable)))
            return 13;
        if (!file_node || ui_rect_height(file_node->rectangle) < 42.0f ||
            !message_composer_controller_mark_submitted(&local_send, pending_file.nonce).ok() ||
            !message_composer_controller_observe_http(&local_send, 400,
                                                      MessageSendHttpDisposition::terminal_failure, 110)
                 .ok() ||
            !message_timeline_reset(&g_probe.message_view.timeline) ||
            !message_composer_controller_restore_local(&local_send, 9999, &restored_outbox) ||
            !restored_outbox || !rebuild_probe_content_text() ||
            !probe_present(&g_probe.renderer, kDefaultTheme) || g_probe.message_text.rich.attachments != 1)
            return 13;
        if (!message_composer_controller_retry(&local_send, pending_file.nonce, 120).ok() ||
            !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
            g_probe.message_text.rich.attachments != 1 ||
            !message_composer_controller_mark_submitted(&local_send, pending_file.nonce).ok() ||
            !message_composer_controller_observe_http(&local_send, 400,
                                                      MessageSendHttpDisposition::terminal_failure, 130)
                 .ok() ||
            !message_composer_controller_discard_failed(&local_send, pending_file.nonce).ok() ||
            !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
            g_probe.message_text.rich.attachments)
            return 13;
        log_write(LogLevel::info, "probe", "pending_attachment_card_failure_retry_discard_hardware_passed");
        log_write(LogLevel::info, "probe", "outbox_pending_and_failed_restoration_hardware_passed");
        PrivateChannelStore private_store{};
        PrivateChannelRecord private_records[4]{};
        u64 private_ids[8]{};
        const u64 peer_ids[] = {123};
        const Utf8View peer_avatar = utf8_literal("abcdefabcdefabcdefabcdefabcdefab");
        if (!private_channel_store_initialize(&private_store, {private_records, 4}, {private_ids, 8}) ||
            !private_channel_store_replace(&private_store,
                                           {9998, 0, {peer_ids, 1}, {}, true, false, false}) ||
            !user_store_upsert(&g_probe.users, 123, utf8_literal("Mai")).ok() ||
            !user_store_set_avatar(&g_probe.users, 123, peer_avatar))
            return 14;
        g_probe.chat_state.private_channels = &private_store;
        u8 avatar_pixels[16 * 16 * 4];
        for (u32 i = 0; i < sizeof(avatar_pixels); i += 4) {
            avatar_pixels[i] = 120;
            avatar_pixels[i + 1] = 80;
            avatar_pixels[i + 2] = 180;
            avatar_pixels[i + 3] = 255;
        }
        const auto avatar_key =
            media_asset_user_avatar(123, user_avatar_variant(peer_avatar), false);
        if (!renderer_upload_media_rgba8(&g_probe.renderer, avatar_key, 16, 16, 64,
                                         {avatar_pixels, sizeof(avatar_pixels)})
                 .ok() ||
            !chat_state_store_upsert_channel(&g_probe.chat_state, {9998, 0, 0, {}, 0, 0, 1}).ok() ||
            !chat_state_store_select_channel(&g_probe.chat_state, 9998).ok() ||
            !navigation_view_rebuild(&g_probe.navigation, g_probe.chat_state).ok())
            return 14;
        message_composer_reset(&g_probe.message_composer, 9998, false);
        if (!synchronize_probe_shell_layout() || !rebuild_probe_content_text() ||
            !probe_present(&g_probe.renderer, kDefaultTheme) || !g_probe.renderer.media_quad_count)
            return 14;
        bool peer_drawn = false;
        const auto avatar_allocation = media_atlas_peek(g_probe.renderer.media_atlas, avatar_key);
        if (!avatar_allocation.ok())
            return 14;
        for (u32 i = 0; i < g_probe.renderer.media_quad_count; ++i) {
            const auto& quad = g_probe.renderer.media_draw.quad_data[i];
            const auto handle = g_probe.renderer.media_draw.handle_data[i];
            peer_drawn =
                peer_drawn ||
                (handle.slot == avatar_allocation.handle.slot &&
                 handle.generation == avatar_allocation.handle.generation &&
                 quad.right - quad.left == kUiPrivateRowAvatar && quad.bottom - quad.top == kUiPrivateRowAvatar &&
                 ui_rect_has_area(ui_rect_intersection({quad.left, quad.top, quad.right, quad.bottom},
                                                       g_probe.shell_navigation_scene.viewport)));
        }
        if (!peer_drawn)
            return 14;
        log_write(LogLevel::info, "probe", "private_peer_avatar_and_generated_label_passed");
        // The DM profile panel beside the one-to-one DM (a Nitro-themed
        // synthetic profile) and the header's activity after the name.
        {
            static UserProfile panel_profile{};
            static ProfileScene panel{};
            static RelationshipRecord online_records[2]{};
            static RelationshipStore online{};
            static u32 activity_structurals[256];
            const auto copy = [](auto* to, Utf8View from) noexcept {
                to->bytes = static_cast<u16>(from.bytes);
                copy_bytes(to->data, from.data, from.bytes);
            };
            panel_profile = {};
            panel_profile.user_id = 123;
            copy(&panel_profile.display_name, utf8_literal("Mai"));
            copy(&panel_profile.username, utf8_literal("mai.native"));
            copy(&panel_profile.pronouns, utf8_literal("they/them"));
            copy(&panel_profile.bio, utf8_literal("Synthetic profile beside a direct message."));
            panel_profile.has_theme = true;
            panel_profile.premium_type = 2;
            panel_profile.theme_colors[0] = 0x2F4A6D;
            panel_profile.theme_colors[1] = 0x6A3D7A;
            panel_profile.mutual_guild_count = 1;
            panel_profile.mutual_guilds[0].id = 1;
            RelationshipRecord friend_record{};
            friend_record.user_id = 123;
            friend_record.type = RelationshipType::friend_;
            friend_record.presence = RelationshipPresence::online;
            // Listening (timed) and a game; the game leads the header.
            static char presence_text[512];
            u32 presence_bytes = 0;
            const auto put = [&](Utf8View v) noexcept {
                copy_bytes(presence_text + presence_bytes, v.data, v.bytes);
                presence_bytes += v.bytes;
            };
            const u64 unix_now = clock_unix_milliseconds();
            put(utf8_literal(R"({"activities":[{"type":2,"name":"Spotify","details":"Track","state":"Panchiko",)"
                             R"("party":{"id":"spotify:1"},"timestamps":{"start":)"));
            presence_bytes += decimal_write(presence_text + presence_bytes, unix_now - 65'000);
            put(utf8_literal(R"(,"end":)"));
            presence_bytes += decimal_write(presence_text + presence_bytes, unix_now + 210'000);
            put(utf8_literal(R"(}},{"type":0,"name":"Synthetic Quest","application_id":"777","details":"Lv.12",)"
                             R"("state":"Region"}]})"));
            const Utf8View presence{presence_text, presence_bytes};
            const auto scan = json_scan_structurals_avx1(presence, {activity_structurals, 256});
            UserActivitySet listening{};
            bool has = false;
            const bool sourced =
                relationship_store_initialize(&online, {online_records, 2}) &&
                relationship_store_upsert(&online, friend_record).ok() && scan.ok() &&
                user_activities_decode({presence, {activity_structurals, scan.count}},
                                       {presence, 0, JsonValueKind::object}, 123, &listening, &has) &&
                user_activity_upsert(&g_probe.users.activities, listening);
            ++g_probe.users.revision;
            const UiPrivateRowSource rows{&g_probe.chat_state, &g_probe.users, &online, nullptr,
                                          &g_probe.text_measure_cache};
            g_probe.navigation.private_rows = &rows;
            g_probe_dm_profile = true;
            // A narrower channel list keeps a readable chat beside the panel at the probe's DPI.
            const UiLayoutNode* slot = sourced && synchronize_probe_shell_layout({}, {}, 200)
                                     ? ui_scene_node(g_probe.shell_scene, UiSemantic::member_sidebar)
                                     : nullptr;
            panel.presentation = ProfilePresentation::panel;
            panel.phase = ProfilePhase::ready;
            panel.reduced_motion = true;
            panel.presence = UiPresence::online;
            panel.relation = 1;
            panel.measure = &g_probe.text_measure_cache;
            const float width_dip = static_cast<float>(g_probe.renderer.width) * 96.0f /
                                    static_cast<float>(maximum<u32>(g_probe.renderer.dpi, 1));
            const float height_dip = static_cast<float>(g_probe.renderer.height) * 96.0f /
                                     static_cast<float>(maximum<u32>(g_probe.renderer.dpi, 1));
            const bool opened = slot && profile_scene_open(&panel, &panel_profile, &g_probe.chat_state, &g_probe.users,
                                                           g_probe.chat_state.self_user_id, slot->rectangle,
                                                           {0, 0, width_dip, height_dip});
            g_probe.renderer.dm_profile_panel = opened ? &panel : nullptr;
            char activity_line[192];
            UiPrivateHeaderActivity activity{};
            const bool header = ui_private_header_activity(g_probe.shell_scene, rows, 9998,
                                                           {activity_line, sizeof(activity_line)}, &activity);
            const bool presented = opened && rebuild_probe_content_text() &&
                                   probe_present(&g_probe.renderer, kDefaultTheme) &&
                                   probe_capture_frame(L"dm-profile-panel.bmp");
            bool view_full = false;
            for (u32 i = 0; opened && i < panel.part_count; ++i)
                view_full = view_full || (panel.parts[i].action == ProfileAction::expand &&
                                          panel.parts[i].look == ProfileLook::row);
            g_probe.renderer.dm_profile_panel = nullptr;
            profile_scene_release(&panel);
            g_probe.navigation.private_rows = nullptr;
            g_probe_dm_profile = false;
            user_activity_upsert(&g_probe.users.activities, {123, 0, 0, {}});
            ++g_probe.users.revision;
            const u64 outcome = (sourced ? 1u : 0u) | (slot ? 2u : 0u) | (opened ? 4u : 0u) |
                                (presented ? 8u : 0u) | (view_full ? 16u : 0u) | (header ? 32u : 0u) |
                                (activity.glyph == UiActivityGlyph::game &&
                                         utf8_equal(activity.line, utf8_literal("Synthetic Quest +1"))
                                     ? 64u
                                     : 0u) |
                                (g_probe.chat_state.home_selected ? 128u : 0u);
            if (outcome != 255 || !synchronize_probe_shell_layout()) {
                log_u64(LogLevel::error, "probe", "dm_profile_panel_outcome", outcome);
                return 14;
            }
            log_write(LogLevel::info, "probe", "dm_profile_panel_and_header_activity_passed");
        }
        // Group DM: generated icon from member avatars, member count, presence and header actions.
        RelationshipRecord relation_records[2]{};
        RelationshipStore relations{};
        RelationshipRecord friend_record{};
        friend_record.user_id = 123;
        friend_record.type = RelationshipType::friend_;
        friend_record.presence = RelationshipPresence::online;
        const u64 group_ids[] = {123, 124, 125};
        if (!private_channel_store_replace(&private_store, {9997, 123, {group_ids, 3}, {}, true, true, false}) ||
            !user_store_upsert(&g_probe.users, 124, utf8_literal("Kit")).ok() ||
            !user_store_upsert(&g_probe.users, 125, utf8_literal("Rowan")).ok() ||
            !chat_state_store_upsert_channel(&g_probe.chat_state,
                                                {9997, 0, 0, utf8_literal("Weekend crew"), 0, 0, 3})
                 .ok() ||
            !relationship_store_initialize(&relations, {relation_records, 2}) ||
            !relationship_store_upsert(&relations, friend_record).ok() ||
            !chat_state_store_select_channel(&g_probe.chat_state, 9997).ok() ||
            !navigation_view_rebuild(&g_probe.navigation, g_probe.chat_state).ok())
            return 15;
        const UiPrivateRowSource private_rows{&g_probe.chat_state, &g_probe.users, &relations, nullptr};
        g_probe.navigation.private_rows = &private_rows;
        const bool group_presented = synchronize_probe_shell_layout() && rebuild_probe_content_text() &&
                                     probe_present(&g_probe.renderer, kDefaultTheme) &&
                                     probe_capture_frame(L"private-conversations.bmp");
        bool group_avatar = false, header_avatar = false;
        for (u32 i = 0; group_presented && i < g_probe.renderer.media_quad_count; ++i) {
            const auto& quad = g_probe.renderer.media_draw.quad_data[i];
            const auto handle = g_probe.renderer.media_draw.handle_data[i];
            if (handle.slot != avatar_allocation.handle.slot ||
                handle.generation != avatar_allocation.handle.generation)
                continue;
            group_avatar = group_avatar || quad.right - quad.left == kUiPrivateRowAvatar * .625f;
            header_avatar = header_avatar || quad.right - quad.left == kUiPrivateHeaderAvatar * .625f;
        }
        char subtitle[32];
        const auto members = ui_private_row_subtitle(private_rows, 9997, {subtitle, sizeof(subtitle)});
        g_probe.navigation.private_rows = nullptr;
        if (!group_presented || !group_avatar || !header_avatar ||
            !ui_scene_node(g_probe.shell_scene, UiSemantic::private_add) ||
            !ui_scene_node(g_probe.shell_scene, UiSemantic::private_call) ||
            !ui_scene_node(g_probe.shell_scene, UiSemantic::private_create) ||
            !utf8_equal(members, utf8_literal("4 Members")) ||
            ui_private_row_status(private_rows, 9998) != UiPrivateRowStatus::online)
            return 15;
        log_write(LogLevel::info, "probe", "group_conversation_icon_members_presence_and_actions_passed");
        // An unread conversation: pinned in the rail with the peer's avatar and
        // its count, counted on its conversation row, and Home's request badge.
        {
            g_probe.navigation.private_rows = &private_rows;
            g_probe.navigation.home_badge = 2;
            ++g_probe.navigation.requests_revision;
            const bool pinned =
                chat_state_store_note_message(&g_probe.chat_state, 9998, 1'270'000'000'000'020'000ull, 123, true,
                                              false) &&
                chat_state_store_note_message(&g_probe.chat_state, 9998, 1'270'000'000'000'020'001ull, 123, true,
                                              false) &&
                navigation_view_rebuild(&g_probe.navigation, g_probe.chat_state).ok() &&
                g_probe.navigation.guild_count && g_probe.navigation.guild_rows[0].id == 9998 &&
                g_probe.navigation.guild_rows[0].folder_role == GuildFolderRole::conversation &&
                g_probe.navigation.guild_rows[0].mention_count == 2 && synchronize_probe_shell_layout() &&
                rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
                probe_capture_frame(L"dm-pings.bmp");
            bool rail_avatar = false;
            for (u32 i = 0; pinned && i < g_probe.renderer.media_quad_count; ++i) {
                const auto& quad = g_probe.renderer.media_draw.quad_data[i];
                const auto handle = g_probe.renderer.media_draw.handle_data[i];
                rail_avatar = rail_avatar ||
                              (handle.slot == avatar_allocation.handle.slot &&
                               handle.generation == avatar_allocation.handle.generation &&
                               quad.right - quad.left == 40.f &&
                               ui_rect_has_area(ui_rect_intersection({quad.left, quad.top, quad.right, quad.bottom},
                                                                     g_probe.guild_rail_scene.viewport)));
            }
            g_probe.navigation.private_rows = nullptr;
            g_probe.navigation.home_badge = 0;
            ++g_probe.navigation.requests_revision;
            // Reading the conversation unpins it for the scenes that follow.
            if (!pinned || !rail_avatar ||
                !chat_state_store_note_message(&g_probe.chat_state, 9998, 1'270'000'000'000'020'002ull, 123, false,
                                               true) ||
                !navigation_view_rebuild(&g_probe.navigation, g_probe.chat_state).ok() ||
                g_probe.navigation.guild_rows[0].folder_role == GuildFolderRole::conversation)
                return 16;
            log_write(LogLevel::info, "probe", "conversation_rail_pin_passed");
        }
        // Group DM member list: the recipients beside the conversation with
        // the owner's crown, laid out as the application shows it.
        {
            MemberListRow group_rows[8]{};
            MemberListView group_members{};
            group_members.rows = {group_rows, 8};
            static MemberListScene group_scene{};
            static MemberListRowWidget group_row_widgets[8]{};
            static UiWidget group_widgets[40]{};
            static UiLayoutNode group_nodes[40]{};
            GuildMemberStore no_guild_members{};
            g_probe.navigation.private_rows = &private_rows;
            bool listed = member_list_view_private(&group_members, g_probe.users, g_probe.chat_state, 9997,
                                                   {private_rows.relationships, GuildMemberStatus::online})
                              .ok() &&
                          group_members.member_count >= 3 &&
                          member_list_scene_initialize(&group_scene, {group_row_widgets, 8}, {group_widgets, 40},
                                                       {group_nodes, 40}) &&
                          synchronize_probe_shell_layout({}, {}, 0, 240);
            const auto* pane = ui_scene_node(g_probe.shell_scene, UiSemantic::member_sidebar);
            group_scene.measure = &g_probe.text_measure_cache;
            listed = listed && pane && ui_rect_width(pane->rectangle) >= 200 &&
                     member_list_scene_update(&group_scene, &group_members, &no_guild_members, &g_probe.users,
                                              &g_probe.chat_state, pane->rectangle, 0)
                         .ok() &&
                     group_scene.count >= 4;
            g_probe.renderer.member_list_scene = listed ? &group_scene : nullptr;
            listed = listed && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
                     probe_capture_frame(L"private-group-members.bmp");
            g_probe.renderer.member_list_scene = nullptr;
            g_probe.navigation.private_rows = nullptr;
            if (!listed || !synchronize_probe_shell_layout())
                return 19;
            log_write(LogLevel::info, "probe", "group_dm_member_list_passed");
        }
        if (!probe_settings_scene() || !probe_settings_backdrop() || !probe_settings_entrance_frames())
            return static_cast<int>(g_probe.failure_stage);
        if (!probe_account_settings_pages())
            return 732;
        if (!probe_friends_scene())
            return static_cast<int>(g_probe.failure_stage);
        if (!probe_friends_requests_scene())
            return static_cast<int>(g_probe.failure_stage);
        if (!probe_image_viewer())
            return static_cast<int>(g_probe.failure_stage);
        if (!probe_rich_composer())
            return static_cast<int>(g_probe.failure_stage);
        if (!probe_voice_message_bar())
            return static_cast<int>(g_probe.failure_stage);
        if (!probe_autocomplete_identity())
            return static_cast<int>(g_probe.failure_stage);
        if (!probe_glyph_atlas_gutters())
            return static_cast<int>(g_probe.failure_stage);
        if (!probe_dm_name_styles())
            return 237;
        if (!probe_profile_card())
            return static_cast<int>(g_probe.failure_stage);
        if (!probe_empty_decorations(g_probe.renderer))
            return 239;
        log_u64(LogLevel::info, "probe", "final_renderer_font_faces",
                g_probe.renderer.glyph_text.font_face_count);
        if (!probe_renderer_font_aliases(g_probe.renderer))
            return 238;
        // The renderer types hold no DirectWrite objects, and the probe does
        // not link dwrite.lib.
        log_write(LogLevel::info, "probe", "native_application_text_objects_absent_passed");
        renderer_shutdown(&g_probe.renderer);
        d3d11_draw_pipeline_release(&g_probe.renderer.pipeline);
        const bool released = !g_probe.renderer.shell_text_storage.base &&
                              device_owner_empty(g_probe.renderer.graphics) &&
                              draw_pipeline_empty(g_probe.renderer.pipeline) &&
                              glyph_text_owner_empty(g_probe.renderer.glyph_text) &&
                              composer_layout_empty(g_probe.renderer.composer_layout) &&
                              message_hit_cache_empty(g_probe.renderer.message_hit_test) &&
                              content_text_retention_empty(g_probe.renderer.content_text_retention);
        DestroyWindow(g_probe.window);
        if (released)
            log_write(LogLevel::info, "probe", "application_renderer_release_passed");
        return released ? 0 : 9;
    }
    probe_window::show(g_probe.window);
    UpdateWindow(g_probe.window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
        secure_text_store_process_deferred();
    }
    secure_text_store_process_deferred();
    renderer_shutdown(&g_probe.renderer);
    d3d11_draw_pipeline_release(&g_probe.renderer.pipeline);
    return device_owner_empty(g_probe.renderer.graphics) &&
                   draw_pipeline_empty(g_probe.renderer.pipeline) &&
                   glyph_text_owner_empty(g_probe.renderer.glyph_text) &&
                   composer_layout_empty(g_probe.renderer.composer_layout) &&
                   message_hit_cache_empty(g_probe.renderer.message_hit_test) &&
                   content_text_retention_empty(g_probe.renderer.content_text_retention)
               ? static_cast<int>(message.wParam)
               : 9;
}

} // namespace
bool probe_resize_surface(u32 width, u32 height) noexcept {
    return renderer_resize(&g_probe.renderer, width, height) && synchronize_probe_shell_layout();
}

} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_ui_probe_entry() noexcept {
    // MAICORD_UI_PROBE_WARP=1 (cmake/test_registry.cmake: the GPU hang reproductions): every device the probe makes,
    // the renderer's included, is WARP with the debug layer where it is installed, so the case never reaches the
    // hardware adapter; a debug-layer message other than NULL_TILE_MAPPING_ACCESS_WARNING fails it.
    const bool warp = maicord::probe_option_enabled(L"MAICORD_UI_PROBE_WARP");
    if (warp && !maicord::warp_device::install(true))
        ExitProcess(2);
    int result = maicord::run_probe();
    if (warp) {
        maicord::warp_device::report(+[](const char* text, maicord::u32 bytes) noexcept {
            DWORD written = 0;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, bytes, &written, nullptr);
        });
        if (!result && maicord::warp_device::failures())
            result = 253;
    }
    ExitProcess(static_cast<UINT>(result));
}

namespace maicord::app_detail {
// This rendering probe binds a borrowed Quick switcher scene directly; it
// never selects the application modal editor or its native text-input owner.
bool application_quick_switcher_active() noexcept { return false; }
MessageComposer* application_quick_switcher_edit_model() noexcept { return nullptr; }
bool application_quick_switcher_edit_focused() noexcept { return false; }
TextInputTarget application_quick_switcher_edit_target() noexcept { return {}; }
UiRect application_quick_switcher_edit_bounds() noexcept { return {}; }
bool application_quick_switcher_edit_changed(bool) noexcept { return true; }
bool application_quick_switcher_filter(const WindowEvent&) noexcept { return false; }
void application_quick_switcher_edit_cancel_native_group(const TextInputTarget&) noexcept {}
bool application_message_forward_active() noexcept { return false; }
MessageComposer* application_message_forward_edit_model() noexcept { return nullptr; }
bool application_message_forward_edit_focused() noexcept { return false; }
bool application_message_forward_caption_focused() noexcept { return false; }
TextInputTarget application_message_forward_edit_target() noexcept { return {}; }
UiRect application_message_forward_edit_bounds() noexcept { return {}; }
bool application_message_forward_edit_changed(bool) noexcept { return true; }
bool application_message_forward_filter(const WindowEvent&) noexcept { return false; }
void application_message_forward_edit_cancel_native_group(const TextInputTarget&) noexcept {}

} // namespace maicord::app_detail
