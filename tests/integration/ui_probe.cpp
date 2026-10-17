// Native UI probe (MaiCordUiProbe). cmake/test_registry.cmake runs the full
// sequence as maicord_ui_probe (MAICORD_UI_PROBE_ONCE=1) and every isolated scene
// gate (MAICORD_*_UI_PROBE=1, MAICORD_*_RENDER_PROBE=1) as its own CTest case;
// register a new gate there together with the code that adds it.
//
// Manual modes, deliberately not in the suite:
//   MAICORD_UI_PROBE_CAPTURE_DIR / _CAPTURE_PREFIX  write BMP frames that
//     tools/verify_hangul_render.py, verify_tamil_render.py and
//     verify_native_cjk_pixels.py check; MAICORD_CJK_PIXEL_*, MAICORD_CJK_LONG_BIO
//     select their text.
//   MAICORD_GLYPH_CACHE_METRICS, MAICORD_SCENE_MEMORY, MAICORD_SCENE_FONT_RESIDENCY,
//     MAICORD_EMOJI_PAGE_CONTROL  measurement runs (tools/measure_*.py).
//   MAICORD_SCROLL_BENCHMARK, MAICORD_PANE_RESIZE_BENCHMARK  benchmarks
//     (tools/benchmark_scroll.ps1).
//   MAICORD_LOTTIE_FIXTURE=<file>, MAICORD_LOTTIE_*, MAICORD_WEBP_PROBE=<file>,
//     MAICORD_WEBP_EXPECTED_YUV_HASH, MAICORD_JPEG_CORPUS=<dir>  offline runs over
//     external files.
//   MAICORD_REGIONAL_FLAG_REFERENCE_DIRECTORY (with MAICORD_REGIONAL_FLAG_SURFACES_PROBE)
//     the pinned external flag corpus; registered only when the CMake option names it.
#include "../application_downloads_inactive.hpp"
#include "../../tools/perf/probe_window.hpp"
#include "probe_present.hpp"
#include "maicord/app/account_media_worker.hpp"
#include "maicord/protocol/writer.hpp"
#include "standalone_title_geometry.hpp"
#include "regional_flag_surfaces_probe.hpp"
#include "media_picker_ui_adapter.hpp"
#include "../../src/app/application_media_pump.hpp"
#include "inline_flags_probe.hpp"
#include "guild_rail_icon_probe.hpp"
#include "background_theme_probe.hpp"
#include "emoji_gpu_measurement.hpp"
#include "../../src/render/picker_glyph_cache.hpp"
#include "picker_glyph_cache_probe.hpp"
#include "glyph_cache_measurement.hpp"
#include "scene_memory_measurement.hpp"
#include "colr_binding_probe.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/render/lottie_program.hpp"
#include "../../src/app/account_ui_runtime.hpp"
#include "../../src/app/application_state.hpp"
#include "../../src/render/d3d11_renderer_draw.hpp"
#include "../../src/render/d3d11_renderer_layout.hpp"
#include "../../src/render/d3d11_renderer_interaction_state.hpp"
#include "maicord/render/renderer_presentation.hpp"
#include "maicord/ui/message_text_coordinator.hpp"
#include "maicord/ui/message_presentation.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/ui/attachment_menu.hpp"
#include "maicord/ui/text_edit_menu.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/ui/scroll_host.hpp"
#include "maicord/ui/secure_text_store.hpp"
#include "maicord/ui/profile_media.hpp"
#include "maicord/ui/profile_appearance.hpp"
#include "maicord/ui/shell_text_presentation.hpp"
#include "maicord/ui/private_channel_title.hpp"
#include "maicord/ui/private_channel_rows.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/friends_media.hpp"
#include "maicord/app/message_view_runtime.hpp"
#include "maicord/app/message_send_tracker.hpp"
#include "maicord/app/message_composer_controller.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/json_scan.hpp"
#include "../../src/render/shaped_text_cache.hpp"
#include "maicord/ui/native_text_candidate.hpp"
#include "../../tools/font_reference/dwrite_text_system.hpp"
#include "maicord/text/font_run.hpp"
#include "../../tools/font_reference/native_run_reference.hpp"
#include "font_identity_probe.hpp"
#include "native_renderer_formats_probe.hpp"
#include "maicord/render/native_glyph_outlines.hpp"
#include "maicord/media/jpeg.hpp"
#include "maicord/media/lossy_webp.hpp"
#include "maicord/media/presentation_policy.hpp"
#include "../jpeg_reference_fixture.hpp"
#include "../jpeg_progressive_fixture.hpp"
#include "../jpeg_cmyk_fixture.hpp"
#include "../media_fixture.hpp"
#include "../webp_fixture.hpp"
#include "video_retention_probe.hpp"
#include "video_scene_ui_probe.hpp"
#include "audio_scene_ui_probe.hpp"
#include "poll_scene_ui_probe.hpp"
#include "profile_editor_ui_probe.hpp"
#include "navigation_gaps_ui_probe.hpp"
#include "theme_typography_ui_probe.hpp"
#include "tooltip_ui_probe.hpp"
#include "account_menu_ui_probe.hpp"
#include "search_ui_probe.hpp"
#include "pinned_messages_ui_probe.hpp"
#include "composer_workflow_ui_probe.hpp"
#include "quick_switcher_ui_probe.hpp"
#include "message_forward_ui_probe.hpp"
#include "download_ui_probe.hpp"
#include "unread_boundary_ui_probe.hpp"
#include "rich_search_ui_probe.hpp"
#include "reaction_intent_ui_probe.hpp"
#include "thread_membership_ui_probe.hpp"
#include "thread_browser_ui_probe.hpp"
#include "thread_creation_ui_probe.hpp"
#include "forum_post_ui_probe.hpp"
#include "reply_draft_ui_probe.hpp"
#include "forwarded_messages_ui_probe.hpp"
#include "menu_row_ui_probe.hpp"
#include "animation_preview_probe.hpp"
#include "empty_decoration_probe.hpp"
#include "ui_surface_motion_measurement.hpp"
#include "channel_overview_probe.hpp"
#include "native_pane_glyph_stress_probe.hpp"
#include "scoped_search_ui_probe.hpp"
#include "search_polish_ui_probe.hpp"
#include "hover_retention_ui_probe.hpp"
#include "post_resize_pacing_ui_probe.hpp"
#include "unified_picker_ui_probe.hpp"
#include "maicord/ui/settings_choice.hpp"

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
bool webp_gpu_probe(D3D11Renderer& renderer) noexcept;
bool jpeg_gpu_probe(D3D11Renderer& renderer) noexcept;
bool video_frame_reference_probe(D3D11Renderer& renderer) noexcept;
bool plugin_ui_probe(D3D11Renderer& renderer, PopupScene& popup,
                     bool (*capture)(const wchar_t*) noexcept) noexcept;
bool plugin_navigation_ui_probe(D3D11Renderer& renderer, PopupScene& popup,
                                bool (*capture)(const wchar_t*) noexcept) noexcept;
bool plugin_catalog_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*) noexcept) noexcept;
bool settings_choice_ui_probe(D3D11Renderer& renderer, PopupScene& popup,
                              bool (*capture)(const wchar_t*) noexcept) noexcept;
bool shortcuts_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept;
bool call_scene_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept;
bool notification_settings_ui_probe(D3D11Renderer& renderer,
                                    bool (*capture)(const wchar_t*) noexcept) noexcept;
bool inbox_ui_probe(D3D11Renderer& renderer, PopupScene& popup, bool (*capture)(const wchar_t*) noexcept) noexcept;
extern u32 inbox_ui_probe_stage;
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
namespace {
// One popup bank for every probed menu, autocomplete and popover.
inline constexpr u32 kProbePopupItemCapacity = 72;

bool probe_capture_frame(const wchar_t* filename) noexcept;
bool probe_capture_frame(const wchar_t* filename, const Theme& theme) noexcept;
bool probe_prepare_capture() noexcept;
SceneMemoryMeasurement* scene_memory = nullptr;

inline constexpr u32 kProbeGuildCapacity = 4;
inline constexpr u32 kProbeChannelCapacity = 16;
inline constexpr u32 kProbeRoleCapacity = 16;
inline constexpr u32 kProbeOverwriteCapacity = 32;
inline constexpr u32 kProbeNameBytes = 16 * 1024;
inline constexpr u32 kProbeUserCapacity = 8;
inline constexpr u32 kProbeUserNameBytes = 2 * 1024;
inline constexpr u32 kProbeRichCapacity = kMessageViewItemCapacity;
inline constexpr u32 kProbeRichTextBytes = 16 * 1024;
inline constexpr u32 kProbeMediaEntryCapacity = 32;
inline constexpr u32 kProbeGalleryMediaEntryCapacity = 64;
inline constexpr u32 kProbeMediaRowCapacity = 16;
inline constexpr u32 kProbeMessageSceneRowCapacity = 32;
inline constexpr u16 kProbeMediaExtent = 256;
inline constexpr u16 kProbeStickerExtent = 64;

struct ProbeState {
    MessageViewRuntime message_view;
    D3D11Renderer renderer;
    LoginScene login_scene;
    RendererLoginView login_view;
    UiScene shell_scene;
    GuildRailScene guild_rail_scene;
    GuildRailRowWidget guild_rail_rows[kProbeGuildCapacity];
    UiWidget guild_rail_widgets[kProbeGuildCapacity + 2];
    UiLayoutNode guild_rail_layout_nodes[kProbeGuildCapacity + 2];
    ShellNavigationScene shell_navigation_scene;
    ShellNavigationRowWidget shell_navigation_rows[kProbeChannelCapacity];
    UiWidget shell_navigation_widgets[kProbeChannelCapacity * 4 + 2];
    UiLayoutNode shell_navigation_layout_nodes[kProbeChannelCapacity * 4 + 2];
    MessageComposer message_composer;
    MessageComposerScene message_composer_scene;
    char message_composer_text[128];
    UiWidget message_composer_widgets[kMessageComposerWidgetCapacity];
    UiLayoutNode message_composer_layout_nodes[kMessageComposerWidgetCapacity];
    MessageScene message_scene;
    UiWidget message_scene_widgets[kProbeMessageSceneRowCapacity * kMessageSceneWidgetsPerRow + 1];
    UiLayoutNode message_scene_layout_nodes[kProbeMessageSceneRowCapacity * kMessageSceneWidgetsPerRow + 1];
    MessageActionBar message_action_bar;
    PopupScene popup_scene;
    PopupItem popup_items[kProbePopupItemCapacity];
    PopupRowWidget popup_rows[kProbePopupItemCapacity];
    UiWidget popup_widgets[2 + kProbePopupItemCapacity * kPopupWidgetsPerItem];
    UiLayoutNode popup_layout_nodes[2 + kProbePopupItemCapacity * kPopupWidgetsPerItem];
    ChatStateStore chat_state;
    UserStore users;
    NavigationView navigation;
    MessageTextPresentation message_text;
    UiTextMeasureCache text_measure_cache;
    RendererTextStorage text;
    MessageRichStore rich_store;
    MediaAtlas media_atlas;
    MediaAtlas preview_media_atlas;
    GuildRecord guilds[kProbeGuildCapacity];
    ChannelRecord channels[kProbeChannelCapacity];
    RoleRecord roles[kProbeRoleCapacity];
    PermissionOverwriteRecord overwrites[kProbeOverwriteCapacity];
    u16 guild_order[kProbeGuildCapacity];
    u16 channel_order[kProbeChannelCapacity];
    u8 names[kProbeNameBytes];
    UserRecord user_records[kProbeUserCapacity];
    u32 user_order[kProbeUserCapacity];
    u8 user_names[kProbeUserNameBytes];
    GuildViewRow guild_rows[kProbeGuildCapacity];
    ChannelViewRow channel_rows[kProbeChannelCapacity];
    u16 channel_scratch[kProbeChannelCapacity];
    u8 channel_visibility_scratch[kProbeChannelCapacity];
    ChannelSortKey channel_sort_keys[kProbeChannelCapacity];
    GlyphQuad text_quads[kRendererContentGlyphCapacity];
    GlyphDrawBatch text_batches[kRendererContentBatchCapacity];
    app_detail::AccountResponseShapeScratch response_shape_scratch;
    MessageTextCommand message_text_commands[kMaximumMessageTextCommands];
    char message_text_utf8[kMaximumMessageTextUtf8Bytes];
    TextStyleSpan message_text_styles[kMaximumMessageTextStyleSpans];
    MessageMarkdownSemantic message_text_semantics[kMaximumMessageTextSemantics];
    MessageTextMeasurement message_text_measurements[kMaximumMessageTextMeasurements];
    UiTextMeasureRecord message_text_measure_cache_records[kMaximumMessageTextMeasureCacheRecords];
    MessageTextSpoilerReveal message_text_spoiler_reveals[kMaximumMessageTextSpoilerReveals];
    u32 message_text_byte_styles[kMessageTextByteStyleScratchCapacity];
    TextHitSpan hit_spans[kTextHitSpanMaximum];
    TextHitRegion hit_region_scratch[kRendererContentHitRegionCapacity];
    TextDecoration text_decorations[kRendererTextDecorationCapacity];
    MessageTextSemantic content_semantics[kRendererContentSemanticCapacity];
    TextHitRegion content_hit_regions[kRendererContentHitRegionCapacity];
    MessageRichRecord rich_records[kProbeRichCapacity];
    u16 rich_order[kProbeRichCapacity];
    u8 rich_text[kProbeRichTextBytes];
    MediaAtlasEntry media_entries[kProbeMediaEntryCapacity];
    u32 media_buckets[kProbeMediaEntryCapacity * 2];
    GlyphAtlasEntry media_allocations[kProbeMediaEntryCapacity];
    GlyphAtlasRow media_rows[kProbeMediaRowCapacity];
    MediaAtlasEntry preview_media_entries[kProbeGalleryMediaEntryCapacity];
    u32 preview_media_buckets[kProbeGalleryMediaEntryCapacity * 2];
    GlyphAtlasEntry preview_media_allocations[kProbeGalleryMediaEntryCapacity];
    GlyphAtlasRow preview_media_rows[kProbeMediaRowCapacity];
    u8 png_encoded[17'000];
    u8 png_zlib[17'000];
    u8 png_scanlines[17'000];
    AnimatedWebpWorkspace animated_webp_workspace;
    Vp8lWorkspace vp8l_workspace;
    LossyWebpWorkspace lossy_webp_workspace;
    JpegWorkspace jpeg_workspace;
    Vp8ModeWorkspace vp8_modes;
    Vp8CoefficientWorkspace vp8_coefficients;
    char oversized_text[kRendererContentTextUnitCapacity + 1];
    HWND window;
    u32 failure_stage;
    u32 resize_messages;
    u32 paint_messages;
    u32 resize_min_text_quads;
    bool renderer_ready;
    bool login_view_bound;
};

constinit ProbeState g_probe{};

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
    constexpr float scale = 1.0f / 255.0f;
    const Color expected{static_cast<float>((packed >> 16u) & 0xFFu) * scale,
                         static_cast<float>((packed >> 8u) & 0xFFu) * scale,
                         static_cast<float>(packed & 0xFFu) * scale, 1.0f};
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
    if (!hash) {
        return;
    }
    *hash ^= value;
    *hash *= 1'099'511'628'211ull;
}

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

bool synchronize_probe_shell_layout(Utf8View typing_label = {},
                                    MessageComposerAttachmentInput attachments = {},
                                    float navigation_width = 0, float member_width = 0,
                                    bool members_visible = true) noexcept {
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
    }
    wchar_t pins_option[2]{};
    if (!friends && GetEnvironmentVariableW(L"MAICORD_UI_PINNED_MESSAGES_PROBE", pins_option, 2) == 1 &&
        pins_option[0] == L'1')
        metrics.flags = static_cast<u16>(metrics.flags | ui_shell_layout_pins);
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
        : typing_label.bytes != 0 || attachments.label.bytes != 0
            ? message_composer_scene_preferred_height(g_probe.message_composer,
                                                      ui_scene_composer_width(metrics, width_dip), false,
                                                      typing_label.bytes != 0, message_composer_attachment_row(attachments))
            : metrics.composer_height +
                  message_composer_scene_toolbar_height(ui_scene_composer_width(metrics, width_dip));
    if (!friends && (metrics.composer_height != kGuildChannelShellLayoutMetrics.composer_height ||
                     typing_label.bytes != 0 || attachments.label.bytes != 0))
        metrics.revision += 10'000u + ui_extent_units(metrics.composer_height);
    metrics.revision += static_cast<u32>(metrics.flags) * 7u;
    if (!ui_scene_layout_with_metrics(&g_probe.shell_scene,
                                      friends                         ? UiShellView::friends
                                      : g_probe.chat_state.home_selected ? UiShellView::private_channels
                                                                      : UiShellView::guild_channel,
                                      metrics, width_dip, height_dip)) {
        log_write(LogLevel::error, "probe", "shell_scene_layout_failed");
        return false;
    }
    const UiNode* guild_rail = ui_scene_resolve(
        g_probe.shell_scene, ui_scene_find(g_probe.shell_scene, UiSemantic::guild_navigation));
    const UiNode* channel_navigation = ui_scene_resolve(
        g_probe.shell_scene, ui_scene_find(g_probe.shell_scene, UiSemantic::channel_navigation));
    const UiNode* message_view =
        ui_scene_resolve(g_probe.shell_scene, ui_scene_find(g_probe.shell_scene, UiSemantic::message_view));
    const UiNode* composer =
        ui_scene_resolve(g_probe.shell_scene, ui_scene_find(g_probe.shell_scene, UiSemantic::composer));
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
                                             : g_probe.renderer.glyph_text.allocator.next_y >=
                                                     g_probe.renderer.glyph_text.allocator.height
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
        log_u64(LogLevel::error, "probe", "text_measure_rejected", g_probe.text_measure_cache.rejected);
        log_u64(LogLevel::error, "probe", "text_measure_shape_error",
                static_cast<u64>(g_probe.text.last_shape_error));
        log_u64(LogLevel::error, "probe", "text_measure_native_error",
                static_cast<u64>(static_cast<u32>(g_probe.text.native_error)));
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
        // Simulate the next REST response overwriting all transient shaping
        // storage. Subsequent retained draws and rebuilds must remain valid.
        for (auto& byte : g_probe.response_shape_scratch.response)
            byte = 0xa5;
    }
    return coordinated.ok();
}
bool probe_sync_shell_and_text() noexcept {
    return synchronize_probe_shell_layout() && rebuild_probe_content_text();
}

bool pane_resize_benchmark() noexcept {
    wchar_t enabled[2]{};
    if (!GetEnvironmentVariableW(L"MAICORD_PANE_RESIZE_BENCHMARK", enabled, 2))
        return true;
    const u32 old_width = g_probe.renderer.width, old_height = g_probe.renderer.height;
    if (!renderer_resize(&g_probe.renderer, 2400, 1200))
        return false;
    const auto clock = clock_initialize();
    constexpr u32 samples = 960, burst = 8;
    for (u32 pane = 0; pane < 2; ++pane) {
        u64 elapsed[2]{};
        for (u32 mode = 0; mode < 2; ++mode) {
            if (!synchronize_probe_shell_layout() || !rebuild_probe_content_text())
                return false;
            const i64 begin = clock_now();
            for (u32 sample = 0; sample < samples; ++sample) {
                const float width = 180.0f + static_cast<float>(sample % 40);
                if (mode && (sample + 1) % burst)
                    continue;
                if (!synchronize_probe_shell_layout({}, {}, pane == 0 ? width : 0, pane == 1 ? width : 0) ||
                    !rebuild_probe_content_text())
                    return false;
            }
            elapsed[mode] = clock_microseconds(clock, begin, clock_now());
        }
        log_u64(LogLevel::info, "probe",
                pane ? "member_resize_960_reflows_us" : "channel_resize_960_reflows_us", elapsed[0]);
        log_u64(LogLevel::info, "probe",
                pane ? "member_resize_120_reflows_us" : "channel_resize_120_reflows_us", elapsed[1]);
    }
    return renderer_resize(&g_probe.renderer, old_width, old_height) && synchronize_probe_shell_layout() &&
           rebuild_probe_content_text();
}
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

bool add_message(u64 id, Utf8View content,
                 MessageContentAvailability availability = MessageContentAvailability::available) noexcept {
    return message_store_upsert(&g_probe.message_view.store,
                                MessageInput{id, 111, id + 10'000, snowflake_timestamp_ms(id), 0,
                                             content, 0, availability})
               .ok() &&
           user_store_upsert(&g_probe.users, id + 10'000, utf8_literal("Render Service")).ok();
}

// Opt-in (MAICORD_SCROLL_BENCHMARK=1): scrolls a 200-message channel the way
// smooth-scroll ticks do and logs text-rebuild/present costs, then exits.
bool scroll_benchmark(bool* ran) noexcept {
    wchar_t enabled[2]{};
    *ran = GetEnvironmentVariableW(L"MAICORD_SCROLL_BENCHMARK", enabled, 2) != 0;
    if (!*ran)
        return true;
    static constexpr const char* words[] = {"the", "message", "reply", "channel", "server", "voice", "emoji",
                                            "thread", "pinned", "hello", "world", "thanks", "screenshot",
                                            "attachment", "update", "really", "native", "renderer", "scroll"};
    u64 seed = 151;
    const auto next = [&]() noexcept {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<u32>(seed >> 33);
    };
    constexpr u64 base = 1'420'070'400'000ull << 22;
    // Reuse an existing author; the probe's user store is bounded.
    const u64 author = g_probe.message_view.records[0].author_id;
    char text[900];
    // The view's 16 KiB text arena and the probe's 32 scene rows bound the
    // corpus: 90 lines that mostly wrap once, every third under a new header.
    const u64 second_author = g_probe.message_view.records[1].author_id;
    for (u32 i = 0; i < 90; ++i) {
        u32 bytes = 0;
        const u32 count = 8 + next() % (i % 9 == 0 ? 20 : 14);
        for (u32 w = 0; w < count && bytes < 820; ++w) {
            const char* word = w == 3 && i % 7 == 0 ? "**bold**" : w == 5 && i % 11 == 0 ? "<@123>"
                             : w == 2 && i % 13 == 0 ? "[link](https://chat_state.com)"
                                                     : words[next() % (sizeof(words) / sizeof(words[0]))];
            if (bytes)
                text[bytes++] = ' ';
            for (u32 c = 0; word[c]; ++c)
                text[bytes++] = word[c];
        }
        const u64 id = base + (u64{i} << 22);
        const auto stored = message_store_upsert(
            &g_probe.message_view.store, MessageInput{id, 111, i % 3 ? author : second_author,
                                                      snowflake_timestamp_ms(id), 0,
                                                      Utf8View{text, bytes}, 0, MessageContentAvailability::available});
        if (stored.error == MessageStoreError::text_capacity)
            break;
        if (!stored.ok()) {
            log_u64(LogLevel::error, "probe", "scroll_benchmark_store_error", static_cast<u64>(stored.error));
            return false;
        }
    }
    if (!message_view_runtime_reindex(&g_probe.message_view,
                                      ui_extent_units(kDefaultUiBootstrapMetrics.message_row_height), 1, false)
             .ok() ||
        !rebuild_probe_content_text()) {
        log_write(LogLevel::error, "probe", "scroll_benchmark_reindex_error");
        return false;
    }
    auto& timeline = g_probe.message_view.timeline;
    constexpr u32 steps = 480, stride = 3 * kUiExtentUnitsPerDip / 2;
    static u32 text_us[steps], present_us[steps];
    const auto clock = clock_initialize();
    const auto report = [&](u32* samples, const char* name, bool cached) noexcept {
        for (u32 i = 1; i < steps; ++i)
            for (u32 j = i; j > 0 && samples[j - 1] > samples[j]; --j) {
                const u32 t = samples[j];
                samples[j] = samples[j - 1];
                samples[j - 1] = t;
            }
        const u32 values[] = {samples[steps / 2], samples[steps * 95 / 100], samples[steps - 1]};
        const char* const statistics[] = {"median", "p95", "max"};
        for (u32 i = 0; i < 3; ++i) {
            char key[64]{};
            u32 used = 0;
            const char* const parts[] = {"scroll_", name, cached ? "_cached_" : "_uncached_", statistics[i]};
            for (const char* part : parts)
                for (u32 c = 0; part[c] && used + 1 < sizeof(key); ++c)
                    key[used++] = part[c];
            log_u64(LogLevel::info, "probe", key, values[i]);
        }
    };
    // Uncached first, then the shaped-text cache, over identical sweeps.
    for (u32 mode = 0; mode < 2; ++mode) {
        g_probe.renderer.shaped_text_cache_disabled = mode == 0;
        u64 position = 0;
        bool down = true;
        for (u32 step = 0; step < steps * 2; ++step) {
            const u64 maximum_scroll = timeline.list->total_units > timeline.viewport_units
                                           ? timeline.list->total_units - timeline.viewport_units
                                           : 0;
            if (down && position + stride > maximum_scroll)
                down = false;
            else if (!down && position < stride)
                down = true;
            position = down ? position + stride : position - stride;
            if (!message_timeline_set_scroll(&timeline, position, false))
                return false;
            const i64 begin = clock_now();
            if (!rebuild_probe_content_text()) {
                log_u64(LogLevel::error, "probe", "scroll_benchmark_rebuild_error_step", step);
                return false;
            }
            const i64 rebuilt = clock_now();
            if (!probe_present(&g_probe.renderer, kDefaultTheme))
                return false;
            // The first sweep measures row heights; record the steady second sweep.
            if (step >= steps) {
                text_us[step - steps] = static_cast<u32>(clock_microseconds(clock, begin, rebuilt));
                present_us[step - steps] = static_cast<u32>(clock_microseconds(clock, rebuilt, clock_now()));
            }
        }
        report(text_us, "text_us", mode != 0);
        report(present_us, "present_us", mode != 0);
    }
    g_probe.renderer.shaped_text_cache_disabled = false;
    if (const auto* cache = g_probe.renderer.shaped_text_cache) {
        log_u64(LogLevel::info, "probe", "shaped_text_cache_hits", cache->stats.hits);
        log_u64(LogLevel::info, "probe", "shaped_text_cache_misses", cache->stats.misses);
        log_u64(LogLevel::info, "probe", "shaped_text_cache_promotions", cache->stats.promotions);
        log_u64(LogLevel::info, "probe", "shaped_text_cache_flips", cache->stats.flips);
    }
    log_u64(LogLevel::info, "probe", "scroll_messages", g_probe.message_view.store.count);
    log_u64(LogLevel::info, "probe", "scroll_text_bytes", g_probe.message_view.store.text_used);
    log_u64(LogLevel::info, "probe", "scroll_message_commands", g_probe.message_text.command_count);
    log_u64(LogLevel::info, "probe", "scroll_quads", g_probe.text.quad_count);
    return true;
}

// Shaped-text reuse must not change any emitted quad, batch, semantic or hit
// region when cached strings land at new (fractional) origins.
bool shaped_text_cache_equivalence() noexcept {
    auto& renderer = g_probe.renderer;
    auto& text = g_probe.text;
    auto& timeline = g_probe.message_view.timeline;
    static GlyphQuad quads[kRendererContentGlyphCapacity];
    static GlyphDrawBatch batches[kRendererContentBatchCapacity];
    static MessageTextSemantic semantics[kRendererContentSemanticCapacity];
    static TextHitRegion hits[kRendererContentHitRegionCapacity];
    const u64 start = timeline.scroll_units;
    const u64 maximum_scroll =
        timeline.list->total_units > timeline.viewport_units ? timeline.list->total_units - timeline.viewport_units : 0;
    const u32 hits_before = renderer.shaped_text_cache ? renderer.shaped_text_cache->stats.hits : 0;
    bool equal = true;
    for (u32 step = 0; step < 6 && equal; ++step) {
        // 37/64 DIP steps move every cached command to a new subpixel phase.
        const u64 position = minimum(maximum_scroll, start + step * 37u);
        renderer.shaped_text_cache_disabled = false;
        content_text_retention_invalidate(&renderer.content_text_retention);
        if (!message_timeline_set_scroll(&timeline, position, false) || !rebuild_probe_content_text())
            return false;
        const u32 quad_count = text.quad_count, batch_count = text.batch_count;
        const u32 semantic_count = text.content_semantic_count, hit_count = text.content_hit_region_count;
        copy_bytes(quads, text.quads, quad_count * sizeof(GlyphQuad));
        copy_bytes(batches, text.batches, batch_count * sizeof(GlyphDrawBatch));
        copy_bytes(semantics, text.content_semantics, semantic_count * sizeof(MessageTextSemantic));
        copy_bytes(hits, text.content_hit_regions, hit_count * sizeof(TextHitRegion));
        renderer.shaped_text_cache_disabled = true;
        content_text_retention_invalidate(&renderer.content_text_retention);
        if (!rebuild_probe_content_text())
            return false;
        equal = text.quad_count == quad_count && text.batch_count == batch_count &&
                text.content_semantic_count == semantic_count && text.content_hit_region_count == hit_count &&
                bytes_equal(quads, text.quads, quad_count * sizeof(GlyphQuad)) &&
                bytes_equal(batches, text.batches, batch_count * sizeof(GlyphDrawBatch)) &&
                bytes_equal(semantics, text.content_semantics, semantic_count * sizeof(MessageTextSemantic)) &&
                bytes_equal(hits, text.content_hit_regions, hit_count * sizeof(TextHitRegion));
    }
    renderer.shaped_text_cache_disabled = false;
    const bool reused = renderer.shaped_text_cache && renderer.shaped_text_cache->stats.hits > hits_before;
    if (!equal || !reused) {
        log_u64(LogLevel::error, "probe", "shaped_text_cache_equivalence_equal", equal ? 1 : 0);
        log_u64(LogLevel::error, "probe", "shaped_text_cache_equivalence_reused", reused ? 1 : 0);
        return false;
    }
    content_text_retention_invalidate(&renderer.content_text_retention);
    return message_timeline_set_scroll(&timeline, start, false) && rebuild_probe_content_text();
}

bool apng_atlas_pixels_exact(const MediaAtlasResult& allocation,u32 presented_frame,bool preview=false) noexcept {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = 4;
    description.Height = 4;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    HRESULT result = g_probe.renderer.graphics.device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(result)) {
        return false;
    }
    const D3D11_BOX box{allocation.x,allocation.y,0,allocation.x+4u,allocation.y+4u,1};
    g_probe.renderer.graphics.context->CopySubresourceRegion(staging,0,0,0,0,
        preview?g_probe.renderer.media_gpu.preview_texture:g_probe.renderer.media_gpu.texture,0,&box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    bool exact = SUCCEEDED(result);
    for (u32 y = 0; y < 4 && exact; ++y) {
        const u8* row =
            static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch;
        for (u32 x = 0; x < 4; ++x) {
            const u8* pixel = row + x * 4;
            const u32 actual = static_cast<u32>(pixel[0]) | static_cast<u32>(pixel[1]) << 8 |
                               static_cast<u32>(pixel[2]) << 16 | static_cast<u32>(pixel[3]) << 24;
            exact = exact && actual == apng_fixture_canvas_rgba(presented_frame, x, y);
        }
    }
    if (SUCCEEDED(result)) {
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    com_release(staging);
    return exact;
}

bool gif_atlas_pixels_exact(const MediaAtlasResult& allocation, u32 presented_frame) noexcept {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kProbeMediaExtent;
    description.Height = kProbeMediaExtent;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    HRESULT result = g_probe.renderer.graphics.device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(result)) {
        return false;
    }
    g_probe.renderer.graphics.context->CopySubresourceRegion(
        staging, 0, 0, 0, 0, g_probe.renderer.media_gpu.texture, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    bool exact = SUCCEEDED(result);
    for (u32 y = 0; y < 4 && exact; ++y) {
        const u8* row =
            static_cast<const u8*>(mapped.pData) + (allocation.y + y) * mapped.RowPitch + allocation.x * 4;
        for (u32 x = 0; x < 4; ++x) {
            const u8* pixel = row + x * 4;
            const u32 actual = static_cast<u32>(pixel[0]) | static_cast<u32>(pixel[1]) << 8 |
                               static_cast<u32>(pixel[2]) << 16 | static_cast<u32>(pixel[3]) << 24;
            exact = exact && actual == gif_fixture_canvas_rgba(presented_frame, x, y);
        }
    }
    if (SUCCEEDED(result)) {
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    com_release(staging);
    return exact;
}

bool animated_webp_atlas_pixels_exact(const MediaAtlasResult& allocation, u32 presented_frame,
                                      bool preview = false) noexcept {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = preview ? g_probe.renderer.media_gpu.preview_width : kProbeMediaExtent;
    description.Height = preview ? g_probe.renderer.media_gpu.preview_height : kProbeMediaExtent;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    HRESULT result = g_probe.renderer.graphics.device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(result)) {
        return false;
    }
    g_probe.renderer.graphics.context->CopyResource(
        staging, preview ? g_probe.renderer.media_gpu.preview_texture : g_probe.renderer.media_gpu.texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    bool exact = SUCCEEDED(result);
    for (u32 y = 0; y < 4 && exact; ++y) {
        const u8* row =
            static_cast<const u8*>(mapped.pData) + (allocation.y + y) * mapped.RowPitch + allocation.x * 4;
        for (u32 x = 0; x < 4; ++x) {
            const u8* pixel = row + x * 4;
            const u32 actual = static_cast<u32>(pixel[0]) | static_cast<u32>(pixel[1]) << 8 |
                               static_cast<u32>(pixel[2]) << 16 | static_cast<u32>(pixel[3]) << 24;
            exact = exact && actual == animated_webp_fixture_canvas_rgba(presented_frame, x, y);
        }
    }
    if (SUCCEEDED(result)) {
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    com_release(staging);
    return exact;
}

bool mixed_animated_webp_atlas_pixels_exact(const MediaAtlasResult& allocation,
                                            u32 presented_frame) noexcept {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kProbeMediaExtent;
    description.Height = kProbeMediaExtent;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    HRESULT result = g_probe.renderer.graphics.device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(result)) {
        return false;
    }
    g_probe.renderer.graphics.context->CopyResource(staging, g_probe.renderer.media_gpu.texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    bool exact = SUCCEEDED(result);
    for (u32 y = 0; y < 4 && exact; ++y) {
        const u8* row =
            static_cast<const u8*>(mapped.pData) + (allocation.y + y) * mapped.RowPitch + allocation.x * 4;
        for (u32 x = 0; x < 4; ++x) {
            const u8* pixel = row + x * 4;
            const u32 actual = static_cast<u32>(pixel[0]) | static_cast<u32>(pixel[1]) << 8 |
                               static_cast<u32>(pixel[2]) << 16 | static_cast<u32>(pixel[3]) << 24;
            exact = exact && actual == mixed_animated_webp_fixture_canvas_rgba(presented_frame, x, y);
        }
    }
    if (SUCCEEDED(result)) {
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    com_release(staging);
    return exact;
}

bool jpeg_atlas_pixels_exact(const MediaAtlasResult& allocation, u16 width, u16 height,
                             bool preview = false) noexcept {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kProbeMediaExtent;
    description.Height = kProbeMediaExtent;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    HRESULT result = g_probe.renderer.graphics.device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(result)) {
        return false;
    }
    g_probe.renderer.graphics.context->CopyResource(
        staging, preview ? g_probe.renderer.media_gpu.preview_texture : g_probe.renderer.media_gpu.texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    bool exact = SUCCEEDED(result);
    for (u32 y = 0; y < height && exact; ++y) {
        const u8* row =
            static_cast<const u8*>(mapped.pData) + (allocation.y + y) * mapped.RowPitch + allocation.x * 4u;
        for (u32 x = 0; x < width; ++x) {
            const u8* pixel = row + x * 4u;
            const u32 actual = static_cast<u32>(pixel[0]) | static_cast<u32>(pixel[1]) << 8u |
                               static_cast<u32>(pixel[2]) << 16u | static_cast<u32>(pixel[3]) << 24u;
            exact = exact && actual == jpeg_fixture_rgba();
        }
    }
    if (SUCCEEDED(result)) {
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    com_release(staging);
    return exact;
}

bool jpeg_atlas_pixels_reference_tolerance(const MediaAtlasResult& allocation, u16 width, u16 height,
                                           ConstSlice<u8> reference_rgb,
                                           JpegOrientation orientation = JpegOrientation::top_left,
                                           bool swap_red_blue = false) noexcept {
    if (!reference_rgb.data || reference_rgb.count != static_cast<u32>(width) * height * 3u ||
        !jpeg_orientation_valid(orientation)) {
        return false;
    }
    const bool swapped = jpeg_orientation_swaps_dimensions(orientation);
    const u32 output_width = swapped ? height : width;
    const u32 output_height = swapped ? width : height;
    if (allocation.width != output_width || allocation.height != output_height) {
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kProbeMediaExtent;
    description.Height = kProbeMediaExtent;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    HRESULT result = g_probe.renderer.graphics.device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(result)) {
        return false;
    }
    g_probe.renderer.graphics.context->CopyResource(staging, g_probe.renderer.media_gpu.texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    u32 maximum_error = 0;
    bool alpha_exact = SUCCEEDED(result);
    if (SUCCEEDED(result)) {
        for (u32 y = 0; y < output_height; ++y) {
            const u8* row = static_cast<const u8*>(mapped.pData) + (allocation.y + y) * mapped.RowPitch +
                            allocation.x * 4u;
            for (u32 x = 0; x < output_width; ++x) {
                u32 source_x = x;
                u32 source_y = y;
                switch (orientation) {
                case JpegOrientation::top_right:
                    source_x = width - 1u - x;
                    break;
                case JpegOrientation::bottom_right:
                    source_x = width - 1u - x;
                    source_y = height - 1u - y;
                    break;
                case JpegOrientation::bottom_left:
                    source_y = height - 1u - y;
                    break;
                case JpegOrientation::left_top:
                    source_x = y;
                    source_y = x;
                    break;
                case JpegOrientation::right_top:
                    source_x = y;
                    source_y = height - 1u - x;
                    break;
                case JpegOrientation::right_bottom:
                    source_x = width - 1u - y;
                    source_y = height - 1u - x;
                    break;
                case JpegOrientation::left_bottom:
                    source_x = width - 1u - y;
                    source_y = x;
                    break;
                case JpegOrientation::top_left:
                    break;
                }
                for (u32 channel = 0; channel < 4; ++channel) {
                    const u8 actual = row[x * 4u + channel];
                    if (channel < 3) {
                        const u32 reference_channel =
                            swap_red_blue && channel != 1 ? 2u - channel : channel;
                        const u8 expected =
                            reference_rgb[(source_y * width + source_x) * 3u + reference_channel];
                        const u32 error = actual > expected ? actual - expected : expected - actual;
                        maximum_error = maximum<u32>(maximum_error, error);
                    } else {
                        alpha_exact = alpha_exact && actual == 255;
                    }
                }
            }
        }
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    com_release(staging);
    return SUCCEEDED(result) && alpha_exact && maximum_error <= 2;
}

float probe_clamp_unit(float value) noexcept {
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

float probe_twelfth_root(float value) noexcept {
    float root = 1.0f;
    for (u32 iteration = 0; iteration < 24; ++iteration) {
        float power = root;
        for (u32 exponent = 1; exponent < 11; ++exponent) {
            power *= root;
        }
        root = (11.0f * root + value / power) / 12.0f;
    }
    return root;
}

u8 probe_srgb_byte(float linear) noexcept {
    linear = probe_clamp_unit(linear);
    float encoded = linear * 12.920000076f;
    if (linear >= 0.003130805f) {
        const float root = probe_twelfth_root(1.137283325f * linear);
        const float square = root * root;
        encoded = square * square * root - 0.054969788f;
    }
    const float byte = probe_clamp_unit(encoded) * 255.0f;
    return static_cast<u8>(byte + 0.5f);
}

void probe_icc_lab_to_srgb(float lightness_normalized, float a_normalized, float b_normalized,
                           u8 expected[3]) noexcept {
    const float lightness = lightness_normalized * 100.0f;
    const float a = a_normalized * 255.0f - 128.0f;
    const float b = b_normalized * 255.0f - 128.0f;
    const float root_y = (lightness + 16.0f) / 116.0f;
    const float roots[3] = {root_y + a / 500.0f, root_y, root_y - b / 200.0f};
    float xyz[3]{};
    for (u32 channel = 0; channel < 3; ++channel) {
        const float cube = roots[channel] * roots[channel] * roots[channel];
        xyz[channel] = cube > 0.008856f ? cube : (roots[channel] - 16.0f / 116.0f) / 7.787f;
    }
    xyz[0] *= 0.9642f;
    xyz[2] *= 0.8249f;
    constexpr float matrix[9] = {
        3.13411215f,   -1.61739242f,  -0.490633398f, -0.978787303f, 1.91627955f,
        0.0334547125f, 0.0719830394f, -0.228985846f, 1.40538514f,
    };
    for (u32 row = 0; row < 3; ++row) {
        const float linear =
            matrix[row * 3u] * xyz[0] + matrix[row * 3u + 1u] * xyz[1] + matrix[row * 3u + 2u] * xyz[2];
        expected[row] = probe_srgb_byte(linear);
    }
}

void probe_icc_rgb_lut_expected(const u8* source, u8 expected[3]) noexcept {
    const float red = static_cast<float>(source[0]) / 255.0f;
    const float green = static_cast<float>(source[1]) / 255.0f;
    const float blue = static_cast<float>(source[2]) / 255.0f;
    probe_icc_lab_to_srgb(0.25f + 0.50f * red, 0.375f + 0.25f * green, 0.375f + 0.25f * blue, expected);
}

bool jpeg_icc_rgb_lut_pixels_tolerance(const MediaAtlasResult& allocation,
                                       ConstSlice<u8> reference_rgb) noexcept {
    if (!reference_rgb.data || reference_rgb.count != 16u * 16u * 3u || allocation.width != 16 ||
        allocation.height != 16) {
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kProbeMediaExtent;
    description.Height = kProbeMediaExtent;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    HRESULT result = g_probe.renderer.graphics.device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(result)) {
        return false;
    }
    g_probe.renderer.graphics.context->CopyResource(staging, g_probe.renderer.media_gpu.texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    u32 maximum_error = 0;
    bool alpha_exact = SUCCEEDED(result);
    if (SUCCEEDED(result)) {
        for (u32 y = 0; y < 16; ++y) {
            const u8* row = static_cast<const u8*>(mapped.pData) + (allocation.y + y) * mapped.RowPitch +
                            allocation.x * 4u;
            for (u32 x = 0; x < 16; ++x) {
                u8 expected[3]{};
                probe_icc_rgb_lut_expected(reference_rgb.data + (y * 16u + x) * 3u, expected);
                for (u32 channel = 0; channel < 3; ++channel) {
                    const u8 actual = row[x * 4u + channel];
                    const u32 error = actual > expected[channel] ? actual - expected[channel]
                                                                 : expected[channel] - actual;
                    maximum_error = maximum(maximum_error, error);
                }
                alpha_exact = alpha_exact && row[x * 4u + 3u] == 255;
            }
        }
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    com_release(staging);
    // D3D11 shader interpolation/rounding differs slightly across drivers.
    // The current hardware reaches 8/255; larger errors still fail the probe.
    return SUCCEEDED(result) && alpha_exact && maximum_error <= 8;
}

bool jpeg_atlas_uniform_rgb_tolerance(const MediaAtlasResult& allocation, u16 width, u16 height,
                                      const u8 expected[3], u8 tolerance) noexcept {
    if (!expected || allocation.width != width || allocation.height != height) {
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kProbeMediaExtent;
    description.Height = kProbeMediaExtent;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    HRESULT result = g_probe.renderer.graphics.device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(result)) {
        return false;
    }
    g_probe.renderer.graphics.context->CopyResource(staging, g_probe.renderer.media_gpu.texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    bool matches = SUCCEEDED(result);
    if (SUCCEEDED(result)) {
        for (u32 y = 0; y < height && matches; ++y) {
            const u8* row = static_cast<const u8*>(mapped.pData) + (allocation.y + y) * mapped.RowPitch +
                            allocation.x * 4u;
            for (u32 x = 0; x < width; ++x) {
                for (u32 channel = 0; channel < 3; ++channel) {
                    const u8 actual = row[x * 4u + channel];
                    const u32 error = actual > expected[channel] ? actual - expected[channel]
                                                                 : expected[channel] - actual;
                    matches = matches && error <= tolerance;
                }
                matches = matches && row[x * 4u + 3u] == 255;
            }
        }
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    com_release(staging);
    return matches;
}

bool jpeg_atlas_differs_from_reference(const MediaAtlasResult& allocation, u16 width, u16 height,
                                       ConstSlice<u8> reference_rgb, u8 minimum_error) noexcept {
    if (!reference_rgb.data || reference_rgb.count != static_cast<u32>(width) * height * 3u ||
        allocation.width != width || allocation.height != height) {
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kProbeMediaExtent;
    description.Height = kProbeMediaExtent;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    HRESULT result = g_probe.renderer.graphics.device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(result)) {
        return false;
    }
    g_probe.renderer.graphics.context->CopyResource(staging, g_probe.renderer.media_gpu.texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    u32 maximum_error = 0;
    bool alpha_exact = SUCCEEDED(result);
    if (SUCCEEDED(result)) {
        for (u32 y = 0; y < height; ++y) {
            const u8* row = static_cast<const u8*>(mapped.pData) + (allocation.y + y) * mapped.RowPitch +
                            allocation.x * 4u;
            for (u32 x = 0; x < width; ++x) {
                for (u32 channel = 0; channel < 3; ++channel) {
                    const u8 actual = row[x * 4u + channel];
                    const u8 expected = reference_rgb[(y * width + x) * 3u + channel];
                    const u32 error = actual > expected ? actual - expected : expected - actual;
                    maximum_error = maximum(maximum_error, error);
                }
                alpha_exact = alpha_exact && row[x * 4u + 3u] == 255;
            }
        }
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    com_release(staging);
    return SUCCEEDED(result) && alpha_exact && maximum_error >= minimum_error;
}

void reset_vp8_gpu_probe(Vp8IntraMode luma, Vp8IntraMode chroma) noexcept {
    g_probe.vp8_modes.columns = 1;
    g_probe.vp8_modes.rows = 1;
    g_probe.vp8_modes.macroblock_count = 1;
    Vp8MacroblockMode& mode = g_probe.vp8_modes.macroblocks[0];
    for (u32 block = 0; block < 16; ++block) {
        mode.luma_subblock[block] = Vp8SubblockMode::dc;
    }
    mode.luma = luma;
    mode.chroma = chroma;
    mode.segment_id = 0;
    mode.skip_coefficients = 0;

    g_probe.vp8_coefficients.columns = 1;
    g_probe.vp8_coefficients.rows = 1;
    g_probe.vp8_coefficients.macroblock_count = 1;
    Vp8MacroblockCoefficients& coefficient = g_probe.vp8_coefficients.macroblocks[0];
    for (u32 block = 0; block < 24; ++block) {
        for (u32 index = 0; index < 16; ++index) {
            coefficient.values[block][index] = 0;
        }
    }
    coefficient.has_coefficients = 0;
    coefficient.reserved = 0;
}

bool vp8_gpu_first_block_exact(const u8 expected_luma[16], u8 expected_chroma) noexcept {
    const MediaAtlasGpu& gpu = g_probe.renderer.media_gpu;
    if (!gpu.vp8_yuv_buffer || gpu.vp8_yuv_capacity == 0 || gpu.vp8_luma_stride != 16 ||
        gpu.vp8_luma_rows != 16 || gpu.vp8_chroma_stride != 8 || gpu.vp8_chroma_rows != 8) {
        return false;
    }
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = gpu.vp8_yuv_capacity;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* staging = nullptr;
    HRESULT result = g_probe.renderer.graphics.device->CreateBuffer(&description, nullptr, &staging);
    if (FAILED(result)) {
        return false;
    }
    g_probe.renderer.graphics.context->CopyResource(staging, gpu.vp8_yuv_buffer);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    bool exact = SUCCEEDED(result);
    if (exact) {
        const u32* samples = static_cast<const u32*>(mapped.pData);
        for (u32 y = 0; y < 4 && exact; ++y) {
            for (u32 x = 0; x < 4; ++x) {
                exact = samples[y * 16u + x] == expected_luma[y * 4u + x];
                if (!exact) {
                    break;
                }
            }
        }
        constexpr u32 u_offset = 16u * 16u;
        constexpr u32 v_offset = u_offset + 8u * 8u;
        for (u32 y = 0; y < 8 && exact; ++y) {
            for (u32 x = 0; x < 8; ++x) {
                exact = samples[u_offset + y * 8u + x] == expected_chroma &&
                        samples[v_offset + y * 8u + x] == expected_chroma;
                if (!exact) {
                    break;
                }
            }
        }
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    com_release(staging);
    return exact;
}

bool vp8_gpu_luma_block_exact(u32 origin_x, u32 origin_y, const u8 expected[16]) noexcept {
    const MediaAtlasGpu& gpu = g_probe.renderer.media_gpu;
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = gpu.vp8_yuv_capacity;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* staging = nullptr;
    HRESULT result = g_probe.renderer.graphics.device->CreateBuffer(&description, nullptr, &staging);
    if (FAILED(result)) {
        return false;
    }
    g_probe.renderer.graphics.context->CopyResource(staging, gpu.vp8_yuv_buffer);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    bool exact = SUCCEEDED(result);
    if (exact) {
        const u32* samples = static_cast<const u32*>(mapped.pData);
        for (u32 y = 0; y < 4 && exact; ++y) {
            for (u32 x = 0; x < 4; ++x) {
                exact =
                    samples[(origin_y + y) * gpu.vp8_luma_stride + origin_x + x] == expected[y * 4u + x];
                if (!exact) {
                    log_u64(LogLevel::error, "probe", "vp8_block_mismatch_x", origin_x + x);
                    log_u64(LogLevel::error, "probe", "vp8_block_mismatch_y", origin_y + y);
                    log_u64(LogLevel::error, "probe", "vp8_block_actual",
                            samples[(origin_y + y) * gpu.vp8_luma_stride + origin_x + x]);
                    log_u64(LogLevel::error, "probe", "vp8_block_expected", expected[y * 4u + x]);
                    break;
                }
            }
        }
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    com_release(staging);
    return exact;
}

bool vp8_gpu_luma_row_exact(u32 origin_x, u32 y, const u8* expected, u32 count) noexcept {
    const MediaAtlasGpu& gpu = g_probe.renderer.media_gpu;
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = gpu.vp8_yuv_capacity;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* staging = nullptr;
    HRESULT result = g_probe.renderer.graphics.device->CreateBuffer(&description, nullptr, &staging);
    if (FAILED(result)) {
        return false;
    }
    g_probe.renderer.graphics.context->CopyResource(staging, gpu.vp8_yuv_buffer);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    bool exact = SUCCEEDED(result);
    if (exact) {
        const u32* samples = static_cast<const u32*>(mapped.pData);
        for (u32 index = 0; index < count; ++index) {
            if (samples[y * gpu.vp8_luma_stride + origin_x + index] != expected[index]) {
                exact = false;
                break;
            }
        }
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    com_release(staging);
    return exact;
}

u8 vp8_test_clip_rgb(i32 value) noexcept {
    return static_cast<u8>(minimum<i32>(255, maximum<i32>(0, value >> 6)));
}

void vp8_test_rgb(u8 y, u8 u, u8 v, u8 output[3]) noexcept {
    const i32 scaled_y = (static_cast<i32>(y) * 19077) >> 8;
    output[0] = vp8_test_clip_rgb(scaled_y + (static_cast<i32>(v) * 26149 >> 8) - 14234);
    output[1] = vp8_test_clip_rgb(scaled_y - (static_cast<i32>(u) * 6419 >> 8) -
                                  (static_cast<i32>(v) * 13320 >> 8) + 8708);
    output[2] = vp8_test_clip_rgb(scaled_y + (static_cast<i32>(u) * 33050 >> 8) - 17685);
}

bool vp8_atlas_row_exact(const MediaAtlasResult& allocation, u32 origin_x, const u8* expected_gray,
                         u32 count) noexcept {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kProbeMediaExtent;
    description.Height = kProbeMediaExtent;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    HRESULT result = g_probe.renderer.graphics.device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(result)) {
        return false;
    }
    g_probe.renderer.graphics.context->CopyResource(staging, g_probe.renderer.media_gpu.texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    bool exact = SUCCEEDED(result);
    if (exact) {
        const u8* row = static_cast<const u8*>(mapped.pData) + allocation.y * mapped.RowPitch +
                        (allocation.x + origin_x) * 4u;
        for (u32 x = 0; x < count; ++x) {
            const u8* pixel = row + x * 4u;
            if (pixel[0] != expected_gray[x] || pixel[1] != expected_gray[x] ||
                pixel[2] != expected_gray[x] || pixel[3] != 255) {
                exact = false;
                break;
            }
        }
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    com_release(staging);
    return exact;
}

bool vp8_alpha_atlas_exact(const MediaAtlasResult& allocation) noexcept {
    constexpr u8 positions[7][2] = {
        {0, 0}, {7, 0}, {8, 0}, {0, 7}, {0, 8}, {7, 7}, {8, 8},
    };
    constexpr u8 expected_u[7] = {124, 126, 130, 127, 133, 128, 126};
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kProbeMediaExtent;
    description.Height = kProbeMediaExtent;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    HRESULT result = g_probe.renderer.graphics.device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(result)) {
        return false;
    }
    g_probe.renderer.graphics.context->CopyResource(staging, g_probe.renderer.media_gpu.texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    bool exact = SUCCEEDED(result);
    if (exact) {
        for (u32 index = 0; index < 7 && exact; ++index) {
            const u32 x = positions[index][0];
            const u32 y = positions[index][1];
            const u8* pixel = static_cast<const u8*>(mapped.pData) + (allocation.y + y) * mapped.RowPitch +
                              (allocation.x + x) * 4u;
            u8 rgb[3]{};
            vp8_test_rgb(128, expected_u[index], 128, rgb);
            const u8 alpha = g_probe.png_scanlines[y * 16u + x];
            for (u32 channel = 0; channel < 3; ++channel) {
                const u8 premultiplied =
                    static_cast<u8>((static_cast<u32>(rgb[channel]) * alpha + 127u) / 255u);
                exact = exact && pixel[channel] == premultiplied;
            }
            exact = exact && pixel[3] == alpha;
        }
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    com_release(staging);
    return exact;
}

bool media_surface_rgba_exact(ID3D11Texture2D* texture, const MediaAtlasResult& allocation,
                              ConstSlice<u8> expected_rgba) noexcept {
    if (!texture || !expected_rgba.data || expected_rgba.count == 0 || (expected_rgba.count & 3u) != 0) {
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kProbeMediaExtent;
    description.Height = kProbeMediaExtent;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    HRESULT result = g_probe.renderer.graphics.device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(result)) {
        return false;
    }
    g_probe.renderer.graphics.context->CopyResource(staging, texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    bool exact = SUCCEEDED(result);
    if (exact) {
        const u32 pixel_count = expected_rgba.count / 4u;
        const u32 width = allocation.width;
        const u32 height = width != 0 ? pixel_count / width : 0;
        exact = width != 0 && pixel_count % width == 0 && height == allocation.height;
        for (u32 y = 0; y < height && exact; ++y) {
            const u8* actual = static_cast<const u8*>(mapped.pData) + (allocation.y + y) * mapped.RowPitch +
                               allocation.x * 4u;
            const u8* expected = expected_rgba.data + y * width * 4u;
            for (u32 byte = 0; byte < width * 4u; ++byte) {
                if (actual[byte] != expected[byte]) {
                    exact = false;
                    break;
                }
            }
        }
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    com_release(staging);
    return exact;
}

bool vp8_atlas_rgba_exact(const MediaAtlasResult& allocation, ConstSlice<u8> expected_rgba) noexcept {
    return media_surface_rgba_exact(g_probe.renderer.media_gpu.texture, allocation, expected_rgba);
}

bool vp8_atlas_rgba_hash(const MediaAtlasResult& allocation, u64* output) noexcept {
    if (!output || allocation.width == 0 || allocation.height == 0) {
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kProbeMediaExtent;
    description.Height = kProbeMediaExtent;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    HRESULT result = g_probe.renderer.graphics.device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(result)) {
        return false;
    }
    g_probe.renderer.graphics.context->CopyResource(staging, g_probe.renderer.media_gpu.texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    u64 hash = 1'469'598'103'934'665'603ull;
    if (SUCCEEDED(result)) {
        for (u32 y = 0; y < allocation.height; ++y) {
            const u8* row = static_cast<const u8*>(mapped.pData) + (allocation.y + y) * mapped.RowPitch +
                            allocation.x * 4u;
            for (u32 byte = 0; byte < allocation.width * 4u; ++byte) {
                hash = (hash ^ row[byte]) * 1'099'511'628'211ull;
            }
        }
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    com_release(staging);
    if (FAILED(result)) {
        return false;
    }
    *output = hash;
    return true;
}

void setup_vp8_inner_filter_probe(bool high_edge_variance) noexcept {
    reset_vp8_gpu_probe(Vp8IntraMode::dc, Vp8IntraMode::dc);
    Vp8MacroblockCoefficients& coefficient = g_probe.vp8_coefficients.macroblocks[0];
    for (u32 block = 0; block < 16; ++block) {
        const u32 block_x = block & 3u;
        if (block_x == 0u) {
            if (high_edge_variance) {
                coefficient.values[block][1] = 16;
            } else {
                coefficient.values[block][0] = -32;
            }
        } else {
            coefficient.values[block][0] = 32;
        }
    }
    coefficient.has_coefficients = 1;
}

void setup_vp8_outer_filter_probe() noexcept {
    g_probe.vp8_modes.columns = 2;
    g_probe.vp8_modes.rows = 1;
    g_probe.vp8_modes.macroblock_count = 2;
    g_probe.vp8_coefficients.columns = 2;
    g_probe.vp8_coefficients.rows = 1;
    g_probe.vp8_coefficients.macroblock_count = 2;
    for (u32 macroblock = 0; macroblock < 2; ++macroblock) {
        Vp8MacroblockMode& mode = g_probe.vp8_modes.macroblocks[macroblock];
        for (u32 block = 0; block < 16; ++block) {
            mode.luma_subblock[block] = Vp8SubblockMode::dc;
        }
        mode.luma = macroblock == 0 ? Vp8IntraMode::dc : Vp8IntraMode::horizontal;
        mode.chroma = Vp8IntraMode::dc;
        mode.segment_id = 0;
        mode.skip_coefficients = 0;
        Vp8MacroblockCoefficients& coefficient = g_probe.vp8_coefficients.macroblocks[macroblock];
        for (u32 block = 0; block < 24; ++block) {
            for (u32 index = 0; index < 16; ++index) {
                coefficient.values[block][index] = 0;
            }
        }
        for (u32 block = 0; block < 16; ++block) {
            coefficient.values[block][0] = macroblock == 0 ? -32 : 64;
        }
        coefficient.has_coefficients = 1;
        coefficient.reserved = 0;
    }
}

bool vp8_gpu_reconstruction_exact() noexcept {
    const Vp8Keyframe frame{{}, {}, 0, 0, 16, 16, 0, 0, 0, 0};
    const u32 rejected_before = g_probe.renderer.media_gpu.rejected;
    const u32 reconstructions_before = g_probe.renderer.media_gpu.vp8_reconstructions;
    const u32 dispatches_before = g_probe.renderer.media_gpu.vp8_reconstruction_dispatches;
    const u64 uploaded_before = g_probe.renderer.media_gpu.vp8_uploaded_bytes;
    const u64 pixels_before = g_probe.renderer.media_gpu.vp8_reconstructed_pixels;
    const u32 filter_dispatches_before = g_probe.renderer.media_gpu.vp8_filter_dispatches;
    const u32 filtered_edges_before = g_probe.renderer.media_gpu.vp8_filtered_edges;
    const u32 filter_skips_before = g_probe.renderer.media_gpu.vp8_filter_skips;
    const u64 filtered_pixels_before = g_probe.renderer.media_gpu.vp8_filtered_pixels;
    const u32 atlas_uploads_before = g_probe.renderer.media_gpu.vp8_atlas_uploads;
    const u32 alpha_uploads_before = g_probe.renderer.media_gpu.vp8_alpha_uploads;
    const u64 atlas_pixels_before = g_probe.renderer.media_gpu.vp8_atlas_pixels;
    const u64 alpha_bytes_before = g_probe.renderer.media_gpu.vp8_alpha_uploaded_bytes;
    Vp8Keyframe invalid_frame = frame;
    invalid_frame.width = 0;
    if (renderer_reconstruct_vp8_keyframe(&g_probe.renderer, invalid_frame, g_probe.vp8_modes,
                                          g_probe.vp8_coefficients)
            .ok() ||
        g_probe.renderer.media_gpu.rejected != rejected_before + 1u ||
        g_probe.renderer.media_gpu.vp8_reconstructions != reconstructions_before) {
        return false;
    }

    constexpr u8 macroblock_values[4] = {128, 129, 127, 129};
    u8 expected[16]{};
    for (u32 mode_index = 0; mode_index < 4; ++mode_index) {
        reset_vp8_gpu_probe(static_cast<Vp8IntraMode>(mode_index), static_cast<Vp8IntraMode>(mode_index));
        for (u32 index = 0; index < 16; ++index) {
            expected[index] = macroblock_values[mode_index];
        }
        if (!renderer_reconstruct_vp8_keyframe(&g_probe.renderer, frame, g_probe.vp8_modes,
                                               g_probe.vp8_coefficients)
                 .ok() ||
            !vp8_gpu_first_block_exact(expected, macroblock_values[mode_index])) {
            log_u64(LogLevel::error, "probe", "vp8_plane_mode_failed", mode_index);
            return false;
        }
    }

    constexpr u8 subblock_expected[10][16] = {
        {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128},
        {129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129},
        {127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127},
        {129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129},
        {128, 127, 127, 127, 129, 128, 127, 127, 129, 129, 128, 127, 129, 129, 129, 128},
        {127, 127, 127, 127, 128, 127, 127, 127, 129, 127, 127, 127, 129, 128, 127, 127},
        {127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127},
        {127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127},
        {128, 128, 127, 127, 129, 129, 128, 128, 129, 129, 129, 129, 129, 129, 129, 129},
        {129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129},
    };
    for (u32 mode_index = 0; mode_index < 10; ++mode_index) {
        reset_vp8_gpu_probe(Vp8IntraMode::subblocks, Vp8IntraMode::dc);
        for (u32 block = 0; block < 16; ++block) {
            g_probe.vp8_modes.macroblocks[0].luma_subblock[block] =
                static_cast<Vp8SubblockMode>(mode_index);
        }
        if (!renderer_reconstruct_vp8_keyframe(&g_probe.renderer, frame, g_probe.vp8_modes,
                                               g_probe.vp8_coefficients)
                 .ok() ||
            !vp8_gpu_first_block_exact(subblock_expected[mode_index], 128)) {
            log_u64(LogLevel::error, "probe", "vp8_subblock_mode_failed", mode_index);
            return false;
        }
    }

    reset_vp8_gpu_probe(Vp8IntraMode::dc, Vp8IntraMode::dc);
    g_probe.vp8_coefficients.macroblocks[0].values[0][1] = 16;
    g_probe.vp8_coefficients.macroblocks[0].has_coefficients = 1;
    constexpr u8 transform_expected[16] = {
        131, 129, 127, 126, 131, 129, 127, 126, 131, 129, 127, 126, 131, 129, 127, 126,
    };
    if (!renderer_reconstruct_vp8_keyframe(&g_probe.renderer, frame, g_probe.vp8_modes,
                                           g_probe.vp8_coefficients)
             .ok() ||
        !vp8_gpu_first_block_exact(transform_expected, 128)) {
        log_write(LogLevel::error, "probe", "vp8_transform_failed");
        return false;
    }

    g_probe.vp8_modes.columns = 2;
    g_probe.vp8_modes.rows = 2;
    g_probe.vp8_modes.macroblock_count = 4;
    g_probe.vp8_coefficients.columns = 2;
    g_probe.vp8_coefficients.rows = 2;
    g_probe.vp8_coefficients.macroblock_count = 4;
    for (u32 macroblock = 0; macroblock < 4; ++macroblock) {
        Vp8MacroblockMode& mode = g_probe.vp8_modes.macroblocks[macroblock];
        for (u32 block = 0; block < 16; ++block) {
            mode.luma_subblock[block] = Vp8SubblockMode::vertical_edge;
        }
        mode.luma = Vp8IntraMode::dc;
        mode.chroma = Vp8IntraMode::dc;
        mode.segment_id = 0;
        mode.skip_coefficients = 0;
        Vp8MacroblockCoefficients& coefficient = g_probe.vp8_coefficients.macroblocks[macroblock];
        for (u32 block = 0; block < 24; ++block) {
            for (u32 index = 0; index < 16; ++index) {
                coefficient.values[block][index] = 0;
            }
        }
        coefficient.has_coefficients = 0;
        coefficient.reserved = 0;
    }
    g_probe.vp8_modes.macroblocks[0].luma = Vp8IntraMode::vertical;
    g_probe.vp8_modes.macroblocks[2].luma = Vp8IntraMode::subblocks;
    g_probe.vp8_coefficients.macroblocks[1].values[12][1] = 16;
    g_probe.vp8_coefficients.macroblocks[1].has_coefficients = 1;
    Vp8Keyframe schedule_frame = frame;
    schedule_frame.width = 32;
    schedule_frame.height = 32;
    constexpr u8 top_right_dependency[16] = {
        127, 127, 127, 128, 127, 127, 127, 128, 127, 127, 127, 128, 127, 127, 127, 128,
    };
    if (!renderer_reconstruct_vp8_keyframe(&g_probe.renderer, schedule_frame, g_probe.vp8_modes,
                                           g_probe.vp8_coefficients)
             .ok() ||
        !vp8_gpu_luma_block_exact(12, 16, top_right_dependency)) {
        log_write(LogLevel::error, "probe", "vp8_top_right_dependency_failed");
        return false;
    }

    Vp8ControlHeader filter_header{};
    g_probe.failure_stage = 16;
    if (!renderer_filter_vp8_keyframe(&g_probe.renderer, schedule_frame, filter_header, g_probe.vp8_modes,
                                      g_probe.vp8_coefficients)
             .ok() ||
        g_probe.renderer.media_gpu.vp8_filter_skips != filter_skips_before + 1u ||
        renderer_filter_vp8_keyframe(&g_probe.renderer, schedule_frame, filter_header, g_probe.vp8_modes,
                                     g_probe.vp8_coefficients)
            .ok()) {
        return false;
    }

    setup_vp8_inner_filter_probe(false);
    g_probe.failure_stage = 17;
    if (!renderer_reconstruct_vp8_keyframe(&g_probe.renderer, frame, g_probe.vp8_modes,
                                           g_probe.vp8_coefficients)
             .ok()) {
        return false;
    }
    constexpr u8 inner_unfiltered_expected[8] = {
        124, 124, 124, 124, 132, 132, 132, 132,
    };
    if (!vp8_gpu_luma_row_exact(0, 0, inner_unfiltered_expected, sizeof(inner_unfiltered_expected))) {
        g_probe.failure_stage = 22;
        return false;
    }
    filter_header = {};
    filter_header.loop_filter.simple = 1;
    filter_header.segmentation.enabled = 1;
    filter_header.segmentation.absolute_delta = 1;
    filter_header.segmentation.filter_strength[0] = 10;
    constexpr u8 simple_inner_expected[8] = {
        124, 124, 124, 126, 130, 132, 132, 132,
    };
    if (!renderer_filter_vp8_keyframe(&g_probe.renderer, frame, filter_header, g_probe.vp8_modes,
                                      g_probe.vp8_coefficients)
             .ok() ||
        !vp8_gpu_luma_row_exact(0, 0, simple_inner_expected, sizeof(simple_inner_expected))) {
        return false;
    }

    setup_vp8_inner_filter_probe(false);
    g_probe.failure_stage = 18;
    if (!renderer_reconstruct_vp8_keyframe(&g_probe.renderer, frame, g_probe.vp8_modes,
                                           g_probe.vp8_coefficients)
             .ok()) {
        return false;
    }
    filter_header = {};
    filter_header.loop_filter.level = 7;
    filter_header.loop_filter.use_delta = 1;
    filter_header.loop_filter.reference_delta[0] = 3;
    constexpr u8 normal_inner_expected[8] = {
        124, 124, 126, 127, 129, 130, 132, 132,
    };
    if (!renderer_filter_vp8_keyframe(&g_probe.renderer, frame, filter_header, g_probe.vp8_modes,
                                      g_probe.vp8_coefficients)
             .ok() ||
        !vp8_gpu_luma_row_exact(0, 0, normal_inner_expected, sizeof(normal_inner_expected))) {
        return false;
    }

    setup_vp8_inner_filter_probe(true);
    g_probe.failure_stage = 19;
    if (!renderer_reconstruct_vp8_keyframe(&g_probe.renderer, frame, g_probe.vp8_modes,
                                           g_probe.vp8_coefficients)
             .ok()) {
        return false;
    }
    filter_header = {};
    filter_header.loop_filter.level = 10;
    constexpr u8 normal_hev_expected[8] = {
        131, 129, 127, 128, 130, 132, 132, 132,
    };
    if (!renderer_filter_vp8_keyframe(&g_probe.renderer, frame, filter_header, g_probe.vp8_modes,
                                      g_probe.vp8_coefficients)
             .ok() ||
        !vp8_gpu_luma_row_exact(0, 0, normal_hev_expected, sizeof(normal_hev_expected))) {
        return false;
    }

    setup_vp8_outer_filter_probe();
    g_probe.failure_stage = 20;
    Vp8Keyframe outer_frame = frame;
    outer_frame.width = 32;
    if (!renderer_reconstruct_vp8_keyframe(&g_probe.renderer, outer_frame, g_probe.vp8_modes,
                                           g_probe.vp8_coefficients)
             .ok()) {
        return false;
    }
    filter_header = {};
    filter_header.loop_filter.level = 10;
    constexpr u8 normal_outer_expected[8] = {
        124, 125, 126, 127, 129, 130, 131, 132,
    };
    if (!renderer_filter_vp8_keyframe(&g_probe.renderer, outer_frame, filter_header, g_probe.vp8_modes,
                                      g_probe.vp8_coefficients)
             .ok() ||
        !vp8_gpu_luma_row_exact(12, 0, normal_outer_expected, sizeof(normal_outer_expected))) {
        return false;
    }

    constexpr u8 normal_outer_gray[8] = {
        126, 127, 128, 129, 132, 133, 134, 135,
    };
    g_probe.failure_stage = 23;
    const MediaAtlasGpuResult opaque_atlas = media_atlas_gpu_upload_vp8_keyframe(
        &g_probe.renderer.media_gpu, &g_probe.media_atlas, g_probe.renderer.graphics.device,
        g_probe.renderer.graphics.context1, media_asset_custom_emoji(102, true), outer_frame, {});
    if (!opaque_atlas.ok()) {
        g_probe.failure_stage = 25;
        return false;
    }
    if (!vp8_atlas_row_exact(opaque_atlas.allocation, 12, normal_outer_gray, sizeof(normal_outer_gray))) {
        return false;
    }
    if (media_atlas_gpu_upload_vp8_keyframe(
            &g_probe.renderer.media_gpu, &g_probe.media_atlas, g_probe.renderer.graphics.device,
            g_probe.renderer.graphics.context1, media_asset_custom_emoji(103, true), outer_frame, {})
            .ok()) {
        g_probe.failure_stage = 27;
        return false;
    }

    reset_vp8_gpu_probe(Vp8IntraMode::dc, Vp8IntraMode::dc);
    g_probe.failure_stage = 24;
    Vp8MacroblockCoefficients& alpha_coefficients = g_probe.vp8_coefficients.macroblocks[0];
    alpha_coefficients.values[16][0] = -32;
    alpha_coefficients.values[17][0] = 32;
    alpha_coefficients.values[18][0] = 64;
    alpha_coefficients.values[19][0] = -64;
    alpha_coefficients.has_coefficients = 1;
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            g_probe.png_scanlines[y * 16u + x] = static_cast<u8>((x * 13u + y * 17u) & 255u);
        }
    }
    filter_header = {};
    const MediaAtlasGpuResult alpha_atlas = renderer_upload_vp8_keyframe(
        &g_probe.renderer, media_asset_custom_emoji(104, true), frame, filter_header, g_probe.vp8_modes,
        g_probe.vp8_coefficients, ConstSlice<u8>{g_probe.png_scanlines, 16u * 16u});
    if (!alpha_atlas.ok() || !vp8_alpha_atlas_exact(alpha_atlas.allocation)) {
        return false;
    }

    const MediaAtlasGpu& gpu = g_probe.renderer.media_gpu;
    g_probe.failure_stage = 21;
    constexpr u32 single_macroblock_reconstructions = 19;
    constexpr u64 single_macroblock_bytes = sizeof(Vp8MacroblockMode) + sizeof(Vp8MacroblockCoefficients);
    return gpu.vp8_reconstructions == reconstructions_before + single_macroblock_reconstructions + 2u &&
           gpu.vp8_reconstruction_dispatches ==
               dispatches_before + (single_macroblock_reconstructions + 2u) * 2u &&
           gpu.vp8_uploaded_bytes == uploaded_before +
                                         single_macroblock_bytes * single_macroblock_reconstructions +
                                         single_macroblock_bytes * 6u &&
           gpu.vp8_reconstructed_pixels == pixels_before +
                                               16ull * 16ull * single_macroblock_reconstructions +
                                               32ull * 32ull + 32ull * 16ull &&
           gpu.vp8_filter_dispatches == filter_dispatches_before + 4u &&
           gpu.vp8_filtered_edges == filtered_edges_before + 49u &&
           gpu.vp8_filter_skips == filter_skips_before + 2u &&
           gpu.vp8_filtered_pixels == filtered_pixels_before + 1'280u &&
           gpu.vp8_atlas_uploads == atlas_uploads_before + 2u &&
           gpu.vp8_alpha_uploads == alpha_uploads_before + 1u &&
           gpu.vp8_atlas_pixels == atlas_pixels_before + 768u &&
           gpu.vp8_alpha_uploaded_bytes == alpha_bytes_before + 256u &&
           gpu.vp8_last_filtered_reconstruction == gpu.vp8_reconstructions &&
           gpu.vp8_last_atlas_reconstruction == gpu.vp8_reconstructions && gpu.vp8_width == 16 &&
           gpu.vp8_height == 16;
}

bool vp8_real_fixture_gpu_exact() noexcept {
    WebpImage image{};
    Vp8Keyframe frame{};
    Vp8ControlHeader header{};
    Vp8ProbabilityWorkspace probabilities{};
    if (!lossy_webp_decode_keyframe(ConstSlice<u8>{kWebpLossy4x4, sizeof(kWebpLossy4x4)}, &image, &frame,
                                    &header, &probabilities, &g_probe.vp8_modes, &g_probe.vp8_coefficients,
                                    nullptr, {}, {})
             .ok()) {
        return false;
    }
    const MediaAtlasGpuResult uploaded =
        renderer_upload_vp8_keyframe(&g_probe.renderer, media_asset_custom_emoji(105, true), frame, header,
                                     g_probe.vp8_modes, g_probe.vp8_coefficients, {});
    // Oracle bytes are the authored fixture decoded by libwebp 1.6.0
    // through Pillow 12.3.0. Alpha is opaque, so atlas premultiplication
    // preserves RGB exactly.
    constexpr u8 expected_rgba[] = {
        4,  2,   0,  255, 59,  30,  40,  255, 130, 44,  83,  255, 185, 72,  124, 255,
        32, 59,  34, 255, 87,  86,  75,  255, 157, 100, 117, 255, 212, 127, 158, 255,
        42, 126, 57, 255, 97,  154, 98,  255, 166, 168, 140, 255, 221, 196, 181, 255,
        69, 182, 89, 255, 124, 209, 130, 255, 194, 225, 173, 255, 248, 252, 215, 255,
    };
    if (!uploaded.ok() || uploaded.allocation.width != frame.width ||
        uploaded.allocation.height != frame.height ||
        !vp8_atlas_rgba_exact(uploaded.allocation, ConstSlice<u8>{expected_rgba, sizeof(expected_rgba)})) {
        return false;
    }

    // Large message WebP must land in the preview atlas used by message draw
    // commands. Routing it into the identity atlas makes the placeholder stay
    // blank and causes the visible-media pump to decode/upload the same cached
    // image once per frame.
    const MediaAssetKey preview_vp8_key =
        media_asset_embed(10'008, 2, 0, MediaEmbedResource::image, MediaAttachmentFormat::webp, false,
                          MediaResolutionTier::px64);
    const MediaAtlasGpuResult preview_vp8 = renderer_upload_vp8_keyframe(
        &g_probe.renderer, preview_vp8_key, frame, header, g_probe.vp8_modes, g_probe.vp8_coefficients, {});
    if (!preview_vp8.ok() || !media_atlas_find(&g_probe.preview_media_atlas, preview_vp8_key).ok() ||
        media_atlas_find(&g_probe.media_atlas, preview_vp8_key).ok() ||
        !media_surface_rgba_exact(g_probe.renderer.media_gpu.preview_texture, preview_vp8.allocation,
                                  ConstSlice<u8>{expected_rgba, sizeof(expected_rgba)})) {
        return false;
    }
    const MediaAssetKey preview_lossless_key =
        media_asset_embed(10'009, 3, 0, MediaEmbedResource::image, MediaAttachmentFormat::webp, false,
                          MediaResolutionTier::px64);
    const MediaAtlasGpuResult preview_lossless =
        renderer_upload_media_straight_rgba8(&g_probe.renderer, preview_lossless_key, 4, 4, 16,
                                             ConstSlice<u8>{expected_rgba, sizeof(expected_rgba)});
    if (!preview_lossless.ok() ||
        !media_atlas_find(&g_probe.preview_media_atlas, preview_lossless_key).ok() ||
        media_atlas_find(&g_probe.media_atlas, preview_lossless_key).ok() ||
        !media_surface_rgba_exact(g_probe.renderer.media_gpu.preview_texture, preview_lossless.allocation,
                                  ConstSlice<u8>{expected_rgba, sizeof(expected_rgba)})) {
        return false;
    }

    if (!lossy_webp_decode_keyframe(ConstSlice<u8>{kWebpLossy32x24, sizeof(kWebpLossy32x24)}, &image,
                                    &frame, &header, &probabilities, &g_probe.vp8_modes,
                                    &g_probe.vp8_coefficients, nullptr, {}, {})
             .ok()) {
        return false;
    }
    const MediaAtlasGpuResult multi_uploaded =
        renderer_upload_vp8_keyframe(&g_probe.renderer, media_asset_custom_emoji(10'007, true), frame,
                                     header, g_probe.vp8_modes, g_probe.vp8_coefficients, {});
    u64 multi_hash = 0;
    if (!multi_uploaded.ok() || frame.width != 32 || frame.height != 24 ||
        !vp8_atlas_rgba_hash(multi_uploaded.allocation, &multi_hash) ||
        multi_hash != kWebpLossy32x24RgbaFnv1a) {
        log_u64(LogLevel::error, "probe", "vp8_multi_fixture_hash", multi_hash);
        log_u64(LogLevel::error, "probe", "vp8_multi_fixture_expected_hash", kWebpLossy32x24RgbaFnv1a);
        return false;
    }

    constexpr u32 alpha_encoded_bytes = 136;
    auto write_u32 = [](u8* output, u32 value) noexcept {
        output[0] = static_cast<u8>(value);
        output[1] = static_cast<u8>(value >> 8);
        output[2] = static_cast<u8>(value >> 16);
        output[3] = static_cast<u8>(value >> 24);
    };
    for (u32 index = 0; index < alpha_encoded_bytes; ++index) {
        g_probe.png_encoded[index] = 0;
    }
    write_u32(g_probe.png_encoded, 0x46464952u);
    write_u32(g_probe.png_encoded + 4, alpha_encoded_bytes - 8);
    write_u32(g_probe.png_encoded + 8, 0x50424557u);
    write_u32(g_probe.png_encoded + 12, 0x58385056u);
    write_u32(g_probe.png_encoded + 16, 10);
    g_probe.png_encoded[20] = 0x10;
    g_probe.png_encoded[24] = 3;
    g_probe.png_encoded[27] = 3;
    write_u32(g_probe.png_encoded + 30, 0x48504C41u);
    write_u32(g_probe.png_encoded + 34, 17);
    g_probe.png_encoded[38] = 0;
    for (u32 index = 0; index < 16; ++index) {
        g_probe.png_encoded[39 + index] = static_cast<u8>(index * 13u + 7u);
    }
    write_u32(g_probe.png_encoded + 56, 0x20385056u);
    write_u32(g_probe.png_encoded + 60, sizeof(kWebpLossy4x4) - 20);
    for (u32 index = 20; index < sizeof(kWebpLossy4x4); ++index) {
        g_probe.png_encoded[64 + index - 20] = kWebpLossy4x4[index];
    }
    const LossyWebpDecodeResult alpha_decoded = lossy_webp_decode_keyframe(
        ConstSlice<u8>{g_probe.png_encoded, alpha_encoded_bytes}, &image, &frame, &header, &probabilities,
        &g_probe.vp8_modes, &g_probe.vp8_coefficients, &g_probe.vp8l_workspace,
        Slice<u8>{g_probe.png_scanlines, 16u * 4u}, Slice<u8>{g_probe.png_scanlines + 16u * 4u, 16u});
    if (!alpha_decoded.ok() || alpha_decoded.alpha_bytes != 16) {
        return false;
    }
    const MediaAtlasGpuResult alpha_uploaded = renderer_upload_vp8_keyframe(
        &g_probe.renderer, media_asset_custom_emoji(106, true), frame, header, g_probe.vp8_modes,
        g_probe.vp8_coefficients, ConstSlice<u8>{g_probe.png_scanlines + 16u * 4u, 16u});
    u8 expected_premultiplied[sizeof(expected_rgba)]{};
    for (u32 pixel = 0; pixel < 16; ++pixel) {
        const u8 alpha = static_cast<u8>(pixel * 13u + 7u);
        for (u32 channel = 0; channel < 3; ++channel) {
            expected_premultiplied[pixel * 4u + channel] = static_cast<u8>(
                (static_cast<u32>(expected_rgba[pixel * 4u + channel]) * alpha + 127u) / 255u);
        }
        expected_premultiplied[pixel * 4u + 3u] = alpha;
    }
    return alpha_uploaded.ok() &&
           vp8_atlas_rgba_exact(alpha_uploaded.allocation,
                                ConstSlice<u8>{expected_premultiplied, sizeof(expected_premultiplied)});
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
    g_probe.text.utf16 = g_probe.response_shape_scratch.shape.utf16;
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
    g_probe.text.utf16_capacity = kRendererContentTextUnitCapacity;
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
    for (u32 batch_index = 0; batch_index < g_probe.text.batch_count; ++batch_index) {
        const GlyphDrawBatch& batch = g_probe.text.batches[batch_index];
        if (!ui_rect_equal(batch.clip, typing->clip_rectangle))
            continue;
        for (u32 offset = 0; offset < batch.quad_count; ++offset) {
            const GlyphQuad& quad = g_probe.text.quads[batch.first_quad + offset];
            const Color expected = kDefaultTheme.text_primary;
            if (quad.top < typing->rectangle.top || quad.bottom > typing->rectangle.bottom ||
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
    // checks its allocation. The label is gone but its 24 DIP still belong to
    // the shell; it must shrink even though the label now agrees with the store.
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
        const float required = message_composer_scene_cached_height(typing_scene, false, false, false);
        if (ui_rect_height(typing_scene.viewport) != required + 24 || !synchronize_probe_shell_layout() ||
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
    if (rejected_upload.ok() || g_probe.renderer.media_gpu.texture) {
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
    if (!jpeg_uploaded.ok() || !g_probe.renderer.media_gpu.preview_texture) {
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
    const JpegDecodeResult jpeg_420_decoded =
        jpeg_decode_coefficients(ConstSlice<u8>{kJpeg420Reference, sizeof(kJpeg420Reference)},
                                 &jpeg_420_image, &g_probe.jpeg_workspace);
    const MediaAtlasGpuResult jpeg_420_uploaded =
        jpeg_420_decoded.ok() ? renderer_upload_jpeg(&g_probe.renderer, media_asset_attachment(109),
                                                     jpeg_420_image, g_probe.jpeg_workspace.coefficients)
                              : MediaAtlasGpuResult{};
    if (!jpeg_420_decoded.ok() || !jpeg_420_uploaded.ok() ||
        !jpeg_atlas_pixels_reference_tolerance(
            jpeg_420_uploaded.allocation, 16, 16,
            ConstSlice<u8>{reinterpret_cast<const u8*>(kJpeg420ReferenceRgb),
                           sizeof(kJpeg420ReferenceRgb)})) {
        return false;
    }
    g_probe.failure_stage = 32;
    JpegImage progressive_jpeg_image{};
    const JpegDecodeResult progressive_jpeg_decoded =
        jpeg_decode_coefficients(ConstSlice<u8>{reinterpret_cast<const u8*>(kJpegProgressive420Reference),
                                                kJpegProgressive420ReferenceBytes},
                                 &progressive_jpeg_image, &g_probe.jpeg_workspace);
    const MediaAtlasGpuResult progressive_jpeg_uploaded =
        progressive_jpeg_decoded.ok()
            ? renderer_upload_jpeg(&g_probe.renderer, media_asset_attachment(111), progressive_jpeg_image,
                                   g_probe.jpeg_workspace.coefficients)
            : MediaAtlasGpuResult{};
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
    const JpegDecodeResult cmyk_jpeg_decoded =
        jpeg_decode_coefficients(ConstSlice<u8>{kJpegCmykAdobeReference, sizeof(kJpegCmykAdobeReference)},
                                 &cmyk_jpeg_image, &g_probe.jpeg_workspace);
    const MediaAtlasGpuResult cmyk_jpeg_uploaded =
        cmyk_jpeg_decoded.ok() ? renderer_upload_jpeg(&g_probe.renderer, media_asset_attachment(112),
                                                      cmyk_jpeg_image, g_probe.jpeg_workspace.coefficients)
                               : MediaAtlasGpuResult{};
    if (!cmyk_jpeg_decoded.ok() || !cmyk_jpeg_uploaded.ok() ||
        !jpeg_atlas_pixels_reference_tolerance(
            cmyk_jpeg_uploaded.allocation, 16, 16,
            ConstSlice<u8>{kJpegCmykAdobeReferenceRgb, sizeof(kJpegCmykAdobeReferenceRgb)})) {
        return false;
    }
    g_probe.failure_stage = 34;
    JpegImage ycck_jpeg_image{};
    const JpegDecodeResult ycck_jpeg_decoded = jpeg_decode_coefficients(
        ConstSlice<u8>{kJpegYcckProgressiveReference, sizeof(kJpegYcckProgressiveReference)},
        &ycck_jpeg_image, &g_probe.jpeg_workspace);
    const MediaAtlasGpuResult ycck_jpeg_uploaded =
        ycck_jpeg_decoded.ok() ? renderer_upload_jpeg(&g_probe.renderer, media_asset_attachment(113),
                                                      ycck_jpeg_image, g_probe.jpeg_workspace.coefficients)
                               : MediaAtlasGpuResult{};
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
        const JpegDecodeResult oriented_decoded = jpeg_decode_coefficients(
            ConstSlice<u8>{g_probe.png_encoded, oriented_bytes}, &oriented_image, &g_probe.jpeg_workspace);
        const MediaAtlasGpuResult oriented_uploaded =
            oriented_decoded.ok()
                ? renderer_upload_jpeg(&g_probe.renderer, media_asset_attachment(114 + orientation_value),
                                       oriented_image, g_probe.jpeg_workspace.coefficients)
                : MediaAtlasGpuResult{};
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
    const JpegDecodeResult rectangular_decoded =
        jpeg_decode_coefficients(ConstSlice<u8>{rectangular_oriented, rectangular_oriented_bytes},
                                 &rectangular_image, &g_probe.jpeg_workspace);
    const MediaAtlasGpuResult rectangular_uploaded =
        rectangular_decoded.ok()
            ? renderer_upload_jpeg(&g_probe.renderer, media_asset_attachment(123), rectangular_image,
                                   g_probe.jpeg_workspace.coefficients)
            : MediaAtlasGpuResult{};
    if (!rectangular_decoded.ok() || rectangular_image.width != 16 || rectangular_image.height != 8 ||
        rectangular_image.orientation != JpegOrientation::right_top || !rectangular_uploaded.ok() ||
        rectangular_uploaded.allocation.width != 8 || rectangular_uploaded.allocation.height != 16 ||
        !jpeg_atlas_pixels_exact(rectangular_uploaded.allocation, 8, 16)) {
        return false;
    }
    g_probe.failure_stage = 37;
    JpegIccFixtureResult icc_fixture{};
    JpegImage icc_image{};
    if (!build_jpeg_icc_matrix_trc_fixture(ConstSlice<u8>{kJpeg420Reference, sizeof(kJpeg420Reference)},
                                           Slice<u8>{g_probe.png_encoded, sizeof(g_probe.png_encoded)},
                                           JpegIccFixtureCurve::parametric_srgb, true, true,
                                           &icc_fixture)) {
        return false;
    }
    const JpegDecodeResult icc_decoded =
        jpeg_decode_coefficients(ConstSlice<u8>{g_probe.png_encoded, icc_fixture.encoded_bytes}, &icc_image,
                                 &g_probe.jpeg_workspace);
    const MediaAtlasGpuResult icc_uploaded =
        icc_decoded.ok() ? renderer_upload_jpeg(&g_probe.renderer, media_asset_attachment(124), icc_image,
                                                g_probe.jpeg_workspace.coefficients)
                         : MediaAtlasGpuResult{};
    if (!icc_decoded.ok() || !icc_image.tables || !icc_image.tables->icc ||
        icc_image.tables->icc->profile.disposition != JpegIccDisposition::applied || !icc_uploaded.ok()) {
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
    const JpegDecodeResult icc_table_decoded =
        jpeg_decode_coefficients(ConstSlice<u8>{g_probe.png_encoded, icc_fixture.encoded_bytes}, &icc_image,
                                 &g_probe.jpeg_workspace);
    const MediaAtlasGpuResult icc_table_uploaded =
        icc_table_decoded.ok() ? renderer_upload_jpeg(&g_probe.renderer, media_asset_attachment(125),
                                                      icc_image, g_probe.jpeg_workspace.coefficients)
                               : MediaAtlasGpuResult{};
    if (!icc_table_decoded.ok() || !icc_image.tables || !icc_image.tables->icc ||
        icc_image.tables->icc->profile.disposition != JpegIccDisposition::applied ||
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
    const JpegDecodeResult icc_lut_rgb_decoded =
        jpeg_decode_coefficients(ConstSlice<u8>{g_probe.png_encoded, icc_fixture.encoded_bytes}, &icc_image,
                                 &g_probe.jpeg_workspace);
    const MediaAtlasGpuResult icc_lut_rgb_uploaded =
        icc_lut_rgb_decoded.ok() ? renderer_upload_jpeg(&g_probe.renderer, media_asset_attachment(126),
                                                        icc_image, g_probe.jpeg_workspace.coefficients)
                                 : MediaAtlasGpuResult{};
    if (!icc_lut_rgb_decoded.ok() || !icc_image.tables || !icc_image.tables->icc ||
        icc_image.tables->icc->profile.disposition != JpegIccDisposition::applied ||
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
    const JpegDecodeResult icc_lut_cmyk_decoded =
        jpeg_decode_coefficients(ConstSlice<u8>{g_probe.png_encoded, icc_fixture.encoded_bytes}, &icc_image,
                                 &g_probe.jpeg_workspace);
    const MediaAtlasGpuResult icc_lut_cmyk_uploaded =
        icc_lut_cmyk_decoded.ok() ? renderer_upload_jpeg(&g_probe.renderer, media_asset_attachment(127),
                                                         icc_image, g_probe.jpeg_workspace.coefficients)
                                  : MediaAtlasGpuResult{};
    if (!icc_lut_cmyk_decoded.ok() || !icc_image.tables || !icc_image.tables->icc ||
        icc_image.tables->icc->profile.disposition != JpegIccDisposition::applied ||
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
    const JpegDecodeResult icc_lut_ycck_decoded =
        jpeg_decode_coefficients(ConstSlice<u8>{g_probe.png_encoded, icc_fixture.encoded_bytes}, &icc_image,
                                 &g_probe.jpeg_workspace);
    const MediaAtlasGpuResult icc_lut_ycck_uploaded =
        icc_lut_ycck_decoded.ok() ? renderer_upload_jpeg(&g_probe.renderer, media_asset_attachment(128),
                                                         icc_image, g_probe.jpeg_workspace.coefficients)
                                  : MediaAtlasGpuResult{};
    if (!icc_lut_ycck_decoded.ok() || icc_image.color_transform != JpegColorTransform::ycck ||
        !icc_image.tables || !icc_image.tables->icc ||
        icc_image.tables->icc->profile.disposition != JpegIccDisposition::applied ||
        !icc_lut_ycck_uploaded.ok() ||
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
    const JpegDecodeResult icc_gray_decoded =
        jpeg_decode_coefficients(ConstSlice<u8>{g_probe.png_encoded, icc_fixture.encoded_bytes}, &icc_image,
                                 &g_probe.jpeg_workspace);
    const MediaAtlasGpuResult icc_gray_uploaded =
        icc_gray_decoded.ok() ? renderer_upload_jpeg(&g_probe.renderer, media_asset_attachment(129),
                                                     icc_image, g_probe.jpeg_workspace.coefficients)
                              : MediaAtlasGpuResult{};
    u8 gray_expected[3]{};
    probe_icc_lab_to_srgb(128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f, gray_expected);
    if (!icc_gray_decoded.ok() || !icc_image.tables || !icc_image.tables->icc ||
        icc_image.tables->icc->profile.disposition != JpegIccDisposition::applied ||
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

bool probe_login_edit_centered() noexcept {
    const auto& edit = g_probe.renderer.login_edit;
    const auto field =
        login_scene_content_rectangle(g_probe.login_scene, g_probe.login_view.focus == LoginControl::login
                                                               ? LoginElement::login_field
                                                               : LoginElement::password_field);
    const float center = (field.top + field.bottom) * 0.5f;
    const auto near_center = [&](float top, float height) noexcept {
        const float offset = top + height * 0.5f - center;
        return offset >= -0.25f && offset <= 0.25f;
    };
    if (!edit.active || edit.width != ui_rect_width(field) ||
        !near_center(edit.caret.top, edit.caret.height))
        return false;
    for (u32 n = 0; n < edit.selection_count; ++n)
        if (!near_center(edit.selections[n].top, edit.selections[n].height))
            return false;
    for (u32 n = 0; n < g_probe.renderer.login_composition_metric_count; ++n) {
        const auto& metric = g_probe.renderer.login_composition_metrics[n].metric;
        if (!near_center(metric.top, metric.height))
            return false;
    }
    return true;
}

bool probe_text_pixels(ConstSlice<UiRect> rectangles, u8 threshold = 160) noexcept {
    // Glyph counts alone do not prove a draw occurred. Read the actual render
    // target, excluding the QR region, before the flip-model buffer advances.
    if (!d3d11_renderer_draw_stream(&g_probe.renderer, kDefaultTheme, true, true, nullptr))
        return false;
    ID3D11Resource* resource = nullptr;
    g_probe.renderer.graphics.target->GetResource(&resource);
    ID3D11Texture2D* source = nullptr;
    const HRESULT queried =
        resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source));
    resource->Release();
    if (FAILED(queried))
        return false;
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = description.MiscFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    const HRESULT created =
        g_probe.renderer.graphics.device->CreateTexture2D(&description, nullptr, &staging);
    if (SUCCEEDED(created))
        g_probe.renderer.graphics.context->CopyResource(staging, source);
    source->Release();
    if (FAILED(created))
        return false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    bool visible =
        SUCCEEDED(g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
    if (visible) {
        const float scale = g_probe.renderer.dpi / 96.0f;
        for (u32 index = 0; index < rectangles.count; ++index) {
            const auto rectangle = rectangles[index];
            const u32 left = static_cast<u32>(maximum(0.0f, rectangle.left * scale));
            const u32 top = static_cast<u32>(maximum(0.0f, rectangle.top * scale));
            const u32 right =
                minimum(description.Width, static_cast<u32>(maximum(0.0f, rectangle.right * scale)));
            const u32 bottom =
                minimum(description.Height, static_cast<u32>(maximum(0.0f, rectangle.bottom * scale)));
            u32 ink = 0;
            for (u32 y = top; y < bottom; ++y)
                for (u32 x = left; x < right; ++x) {
                    const u8* pixel = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                    if (pixel[0] > threshold && pixel[1] > threshold && pixel[2] > threshold)
                        ++ink;
                }
            visible = visible && ink >= 8;
            if (ink < 8) {
                log_u64(LogLevel::error, "probe", "missing_text_rectangle", index);
                log_u64(LogLevel::error, "probe", "missing_text_width", right - left);
                log_u64(LogLevel::error, "probe", "missing_text_height", bottom - top);
            }
        }
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    staging->Release();
    return visible;
}

bool probe_login_recovery_text_pixels(ConstSlice<UiRect> rectangles) noexcept {
    if (!rectangles.data || rectangles.count != 4)
        return false;
    const UiRect neutral[]{rectangles[0], rectangles[3]};
    // Keep heading/button ink distinct from the button fill. Back and challenge
    // guidance use accent/error colors against the dark card background.
    return probe_text_pixels({neutral, 2}) && probe_text_pixels({rectangles.data + 1, 2}, 70);
}

bool probe_empty_outline_cache() noexcept {
    auto& renderer = g_probe.renderer;
    auto& owner = renderer.glyph_text;
    u32 selected = owner.font_face_count;
    for (u32 i = 0; i < owner.font_face_count; ++i)
        if (!(owner.font_faces[i].flags & 2u)) {
            selected = i;
            break;
        }
    if (selected == owner.font_face_count)
        return false;
    auto& retained = owner.font_faces[selected];
    TextRun run{};
    run.em_size = 16;
    run.glyph_count = 1;
    ShapedText text{};
    text.runs = &run;
    text.run_count = 1;
    u16 space = 0, letter = 0;
    run.native_font = true;
    run.native_face = retained.native_face;
    text.set_native_fonts(owner.system.native_fonts);
    const auto* record =
        native_font_run_get(*native_font_session_runs(owner.system.native_fonts), retained.native_face);
    const auto* font =
        record ? native_font_get(*native_font_session_fonts(owner.system.native_fonts), record->source.face)
               : nullptr;
    if (!font || sfnt_cmap_glyph(font->cmap, 32, 0, &space) != SfntStatus::ok ||
        sfnt_cmap_glyph(font->cmap, 'A', 0, &letter) != SfntStatus::ok)
        return false;
    if (!space || !letter)
        return false;
    TextGlyph glyphs[]{{9.25f, 0, 0, space, 0}, {11.f, 0, 0, letter, 0}};
    text.glyphs = glyphs;
    text.glyph_count = 1;
    GlyphQuad quads[2]{};
    u32 count = 0;
    bool invalidated = false;
    D3D11GlyphTextAppendRequest request{};
    request.text = &text;
    request.output = quads;
    request.capacity = 2;
    request.count = &count;
    request.atlas_invalidated = &invalidated;
    request.dpi = renderer.dpi;
    request.color = {1, 1, 1, 1};
    retained.empty_glyph_plus_one = 0;
    const u32 entries = owner.allocator.entry_count, builds = owner.cache_builds;
    const auto dispatches = owner.gpu.dispatch_count;
    if (!d3d11_glyph_text_append(&owner, renderer.graphics.context, request) || count || invalidated ||
        retained.empty_glyph_plus_one != static_cast<u32>(space) + 1 ||
        owner.allocator.entry_count != entries || owner.cache_builds != builds ||
        owner.gpu.dispatch_count != dispatches)
        return false;
    // A cache hit must not even instantiate an outline provider. Restore the
    // real provider on both success and failure, releasing any unexpected one.
    auto* saved = owner.native_outlines;
    owner.native_outlines = nullptr;
    run.em_size = 32;
    request.dpi = renderer.dpi * 2;
    glyphs[0].advance_offset = -1024;
    glyphs[0].ascender_offset = 4096;
    const bool appended = d3d11_glyph_text_append(&owner, renderer.graphics.context, request);
    const bool skipped = owner.native_outlines == nullptr;
    native_glyph_outlines_release(&owner.native_outlines);
    owner.native_outlines = saved;
    if (!appended || !skipped || count || invalidated || owner.allocator.entry_count != entries ||
        owner.cache_builds != builds || owner.gpu.dispatch_count != dispatches)
        return false;
    // Positive advance on an empty glyph still moves the following ink.
    run.em_size = 16;
    request.dpi = renderer.dpi;
    glyphs[0].advance_offset = glyphs[0].ascender_offset = 0;
    text.glyph_count = run.glyph_count = 2;
    retained.empty_glyph_plus_one = 0;
    if (!d3d11_glyph_text_append(&owner, renderer.graphics.context, request) || count != 1 || invalidated)
        return false;
    const auto expected = quads[0];
    count = 0;
    if (!d3d11_glyph_text_append(&owner, renderer.graphics.context, request) || count != 1 || invalidated ||
        !bytes_equal(&expected, quads, sizeof(expected)))
        return false;
    log_write(LogLevel::info, "probe", "native_empty_outline_skips_provider_and_atlas");
    return true;
}
bool probe_color_foreground_cache(bool native = false) noexcept {
    auto& renderer = g_probe.renderer;
    auto& owner = renderer.glyph_text;
    NativeFontRunHandle color_face{};
    for (u32 i = 0; i < owner.font_face_count && !color_face.slot; ++i)
        if (owner.font_faces[i].flags & 2u)
            color_face = owner.font_faces[i].native_face;
    if (!color_face.slot)
        return false;
    auto* fonts = owner.system.native_fonts;
    const auto* record = native_font_run_get(*native_font_session_runs(fonts), color_face);
    if (!record)
        return false;
    // Glyph zero in the selected emoji font has ordinary contours. Exercise
    // foreground-dependent output without asking the shaper to change fonts.
    TextGlyph glyph{32, 0, 0, 0, 0};
    TextRun run{};
    run.native_font = true;
    run.native_face = color_face;
    run.em_size = 32;
    run.glyph_count = 1;
    ShapedText text{};
    text.glyphs = &glyph;
    text.glyph_count = 1;
    text.runs = &run;
    text.run_count = 1;
    // Borrowed view; the renderer owns the session and its face record.
    text.set_native_fonts(fonts);
    GlyphQuad quads[5]{};
    u32 count = 0;
    bool invalidated = false;
    D3D11GlyphTextAppendRequest request{};
    request.text = &text;
    request.output = quads;
    request.capacity = 5;
    request.count = &count;
    request.atlas_invalidated = &invalidated;
    request.dpi = renderer.dpi;
    const u32 before = owner.cache_builds;
    request.color = {native ? .9375f : .875f, 0, 0, 1};
    if (!d3d11_glyph_text_append(&owner, renderer.graphics.context, request) || count != 1 ||
        owner.cache_builds != before + 1)
        return false;
    request.color = {0, 0, native ? .8125f : .75f, 1};
    if (!d3d11_glyph_text_append(&owner, renderer.graphics.context, request) || count != 2 ||
        owner.cache_builds != before + 2)
        return false;
    request.color = {native ? .9375f : .875f, 0, 0, 1};
    if (!d3d11_glyph_text_append(&owner, renderer.graphics.context, request) || count != 3 ||
        owner.cache_builds != before + 2 || invalidated || quads[0].u0 != quads[2].u0 ||
        quads[0].v0 != quads[2].v0)
        return false;
    if (native) {
        const auto* font = native_font_get(*native_font_session_fonts(fonts), record->source.face);
        if (!font || sfnt_cmap_glyph(font->cmap, 0x1f604, 0, &glyph.index) != SfntStatus::ok ||
            !glyph.index)
            return false;
        // A real COLR glyph builds once; repeating it is an atlas hit.
        if (!d3d11_glyph_text_append(&owner, renderer.graphics.context, request) || count != 4)
            return false;
        const u32 native_builds = owner.cache_builds, native_faces = owner.font_face_count;
        if (!d3d11_glyph_text_append(&owner, renderer.graphics.context, request) || count != 5 ||
            owner.cache_builds != native_builds || owner.font_face_count != native_faces ||
            quads[3].u0 != quads[4].u0 || quads[3].v0 != quads[4].v0 || quads[3].u1 != quads[4].u1 ||
            quads[3].v1 != quads[4].v1)
            return false;
    }
    ID3D11Resource* resource = nullptr;
    ID3D11Texture2D* source = nullptr;
    ID3D11Texture2D* staging = nullptr;
    struct Release {
        ID3D11Resource*& r;
        ID3D11Texture2D*& s;
        ID3D11Texture2D*& t;
        ~Release() noexcept {
            com_release(t);
            com_release(s);
            com_release(r);
        }
    } release{resource, source, staging};
    auto* view = d3d11_glyph_text_color_view(owner);
    if (!view)
        return false;
    view->GetResource(&resource);
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source))))
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
        return false;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(renderer.graphics.device->CreateTexture2D(&desc, nullptr, &staging)))
        return false;
    renderer.graphics.context->CopyResource(staging, source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    bool valid = true;
    for (u32 i = 0; i < 2; ++i) {
        const auto& q = quads[i];
        u32 ink = 0;
        const u32 left = static_cast<u32>((q.u0 - 2) * desc.Width),
                  right = static_cast<u32>((q.u1 - 2) * desc.Width);
        const u32 top = static_cast<u32>(q.v0 * desc.Height), bottom = static_cast<u32>(q.v1 * desc.Height);
        if (right > desc.Width || bottom > desc.Height) {
            valid = false;
            break;
        }
        for (u32 y = top; y < bottom; ++y)
            for (u32 x = left; x < right; ++x) {
                const auto* p = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                if (p[i ? 2 : 0] > 128)
                    ++ink;
                if (p[i ? 0 : 2] || p[1])
                    valid = false;
            }
        valid = valid && ink >= 8;
    }
    if (native) {
        const auto& a = quads[3];
        const auto& b = quads[4];
        valid = valid && a.u0 >= 2 && b.u0 >= 2 && a.u1 >= a.u0 && b.u1 >= b.u0 && a.v0 >= 0 && b.v0 >= 0 &&
                a.v1 >= a.v0 && b.v1 >= b.v0 && a.v1 <= 1 && b.v1 <= 1 &&
                bytes_equal(&a.left, &b.left, 4 * sizeof(float)) &&
                bytes_equal(&a.color, &b.color, sizeof(Color));
        if (!valid) {
            renderer.graphics.context->Unmap(staging, 0);
            return false;
        }
        const u32 ax = static_cast<u32>((a.u0 - 2) * desc.Width), ay = static_cast<u32>(a.v0 * desc.Height),
                  bx = static_cast<u32>((b.u0 - 2) * desc.Width), by = static_cast<u32>(b.v0 * desc.Height);
        const u32 aw = static_cast<u32>((a.u1 - 2) * desc.Width) - ax,
                  ah = static_cast<u32>(a.v1 * desc.Height) - ay,
                  bw = static_cast<u32>((b.u1 - 2) * desc.Width) - bx,
                  bh = static_cast<u32>(b.v1 * desc.Height) - by;
        valid = valid && aw && ah && aw == bw && ah == bh && ax + aw <= desc.Width &&
                bx + bw <= desc.Width && ay + ah <= desc.Height && by + bh <= desc.Height;
        if (valid)
            for (u32 y = 0; y < ah; ++y)
                valid =
                    valid &&
                    bytes_equal(static_cast<const u8*>(mapped.pData) + (ay + y) * mapped.RowPitch + ax * 4,
                                static_cast<const u8*>(mapped.pData) + (by + y) * mapped.RowPitch + bx * 4,
                                aw * 4);
    }
    renderer.graphics.context->Unmap(staging, 0);
    if (valid)
        log_write(LogLevel::info, "probe",
                  native ? "native_colr_run_pixels_and_cache_reuse_passed"
                         : "color_foreground_pixels_and_cache_reuse_passed");
    return valid;
}

struct ProbePixelSample {
    UiPoint point;
    Color expected;
    u32 tolerance;
};
bool probe_exact_pixels(ConstSlice<ProbePixelSample> samples) noexcept {
    if (!d3d11_renderer_draw_stream(&g_probe.renderer, kDefaultTheme, true, true, nullptr))
        return false;
    ID3D11Resource* resource = nullptr;
    g_probe.renderer.graphics.target->GetResource(&resource);
    ID3D11Texture2D* source = nullptr;
    const HRESULT queried =
        resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source));
    resource->Release();
    if (FAILED(queried))
        return false;
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    const bool bgra = description.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    if (!bgra && description.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        source->Release();
        return false;
    }
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = description.MiscFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    const HRESULT created =
        g_probe.renderer.graphics.device->CreateTexture2D(&description, nullptr, &staging);
    if (SUCCEEDED(created))
        g_probe.renderer.graphics.context->CopyResource(staging, source);
    source->Release();
    if (FAILED(created))
        return false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    bool ok = SUCCEEDED(g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
    if (ok) {
        const float scale = g_probe.renderer.dpi / 96.f;
        for (u32 i = 0; i < samples.count; ++i) {
            const auto& sample = samples[i];
            const i32 x = static_cast<i32>(sample.point.x * scale),
                      y = static_cast<i32>(sample.point.y * scale);
            if (x < 0 || y < 0 || static_cast<u32>(x) >= description.Width ||
                static_cast<u32>(y) >= description.Height) {
                ok = false;
                continue;
            }
            const u8* pixel = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch + x * 4;
            const u32 expected[]{static_cast<u32>(sample.expected.red * 255.f + .5f),
                                 static_cast<u32>(sample.expected.green * 255.f + .5f),
                                 static_cast<u32>(sample.expected.blue * 255.f + .5f)};
            for (u32 channel = 0; channel < 3; ++channel) {
                const u32 actual = pixel[bgra ? 2 - channel : channel];
                if (maximum(actual, expected[channel]) - minimum(actual, expected[channel]) >
                    sample.tolerance) {
                    ok = false;
                    log_u64(LogLevel::error, "probe", "layer_pixel_sample", i);
                    log_u64(LogLevel::error, "probe", "layer_pixel_expected", expected[channel]);
                    log_u64(LogLevel::error, "probe", "layer_pixel_actual", actual);
                }
            }
        }
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    staging->Release();
    return ok;
}

bool probe_login_pixels(bool restoring = false) noexcept {
    const LoginElement elements[]{LoginElement::heading, LoginElement::login_field, LoginElement::submit};
    UiRect rectangles[3]{};
    for (u32 i = 0; i < (restoring ? 1u : 3u); ++i)
        rectangles[i] = login_scene_content_rectangle(g_probe.login_scene,
                                                      restoring ? LoginElement::root : elements[i]);
    return probe_text_pixels({rectangles, restoring ? 1u : 3u});
}

bool probe_prepare_capture() noexcept {
    // Pixel fixtures read the fully drawn target directly. Display pacing and
    // swap-chain presentation add no coverage here and can stall an occluded
    // or low-refresh desktop. Keep the production layout/paint path intact.
    return d3d11_renderer_paint_layout(
        &g_probe.renderer, kDefaultTheme,
        renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme));
}
bool probe_capture_frame(const wchar_t* filename) noexcept {
    return probe_capture_frame(filename, kDefaultTheme);
}
bool probe_capture_frame(const wchar_t* filename, const Theme& theme) noexcept {
    wchar_t prefix[80]{};
    const DWORD prefix_chars = GetEnvironmentVariableW(L"MAICORD_UI_PROBE_CAPTURE_PREFIX", prefix, 80);
    if (prefix_chars >= 80)
        return false;
    for (u32 i = 0; i < prefix_chars; ++i)
        if (filename[i] != prefix[i])
            return true;
    if (scene_memory && !scene_memory->snapshot(filename, &g_probe.renderer))
        return false;
    wchar_t directory[1024]{};
    const DWORD chars = GetEnvironmentVariableW(L"MAICORD_UI_PROBE_CAPTURE_DIR", directory, 900);
    if (!chars)
        return true;
    if (chars >= 900)
        return false;
    // Capture every animated text material at the same 420 ms phase. Keep
    // animation enabled; restoring the start times leaves the live probe's
    // scheduling/retention checks intact.
    struct RestoreEffectStarts {
        D3D11Renderer& renderer;
        float starts[kTextEffectCapacity];
        ~RestoreEffectStarts() noexcept {
            for (u32 i = 0; i < renderer.text_effect_count; ++i)
                renderer.text_effects.materials[i].reserved[0] = starts[i];
        }
    } restore{g_probe.renderer, {}};
    for (u32 i = 0; i < g_probe.renderer.text_effect_count; ++i) {
        auto& start = g_probe.renderer.text_effects.materials[i].reserved[0];
        restore.starts[i] = start;
        start = 9.58f;
    }
    // Read a fully drawn frame before the flip-model swapchain advances.
    if (!d3d11_renderer_draw_stream(&g_probe.renderer, theme, true, true, nullptr, 10000))
        return false;
    u32 end = chars;
    directory[end++] = L'/';
    for (u32 i = 0; filename[i]; ++i)
        directory[end++] = filename[i];
    ID3D11Resource* resource = nullptr;
    g_probe.renderer.graphics.target->GetResource(&resource);
    ID3D11Texture2D* source = nullptr;
    HRESULT result = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source));
    resource->Release();
    if (FAILED(result))
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    result = g_probe.renderer.graphics.device->CreateTexture2D(&desc, nullptr, &staging);
    if (SUCCEEDED(result))
        g_probe.renderer.graphics.context->CopyResource(staging, source);
    source->Release();
    if (FAILED(result))
        return false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    bool ok = SUCCEEDED(result);
    if (ok) {
        HANDLE file = CreateFileW(directory, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        BITMAPFILEHEADER header{};
        const u32 capture_width = minimum(desc.Width, g_probe.renderer.width);
        const u32 capture_height = minimum(desc.Height, g_probe.renderer.height);
        header.bfType = 0x4D42;
        header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
        header.bfSize = header.bfOffBits + capture_width * capture_height * 4;
        BITMAPINFOHEADER info{};
        info.biSize = sizeof(info);
        info.biWidth = capture_width;
        info.biHeight = -static_cast<LONG>(capture_height);
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        DWORD written = 0;
        ok = file != INVALID_HANDLE_VALUE && WriteFile(file, &header, sizeof(header), &written, nullptr) &&
             written == sizeof(header) && WriteFile(file, &info, sizeof(info), &written, nullptr) &&
             written == sizeof(info);
        for (u32 y = 0; ok && y < capture_height; ++y)
            ok = WriteFile(file, static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch,
                           capture_width * 4, &written, nullptr) &&
                 written == capture_width * 4;
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    staging->Release();
    return ok;
}

struct MediaGalleryProbeSource {
    UiMediaPickerItemKind kind = UiMediaPickerItemKind::category;
    u64 first_asset = 0x6f000;
    u32 calls = 0;
    bool favorites = false;
};
bool media_gallery_probe_resolve(void* context, u32 index, UiMediaPickerItem* item) noexcept {
    auto& fixture = *static_cast<MediaGalleryProbeSource*>(context);
    ++fixture.calls;
    const Utf8View categories[]{
        utf8_literal("Favorites"),    utf8_literal("Trending"), utf8_literal("Hello"),
        utf8_literal("Celebrate"),    utf8_literal("Laugh"),    utf8_literal("Thank you"),
        utf8_literal("Good morning"), utf8_literal("Wow"),      utf8_literal("Happy dance"),
        utf8_literal("Thinking"),     utf8_literal("Love"),     utf8_literal("Good night")};
    const bool sticker = fixture.kind == UiMediaPickerItemKind::sticker;
    const bool disabled = (sticker && index == 10) || (fixture.favorites && index == 5);
    const auto key = media_asset_catalog_preview(fixture.first_asset + index, 1,
                                                 fixture.kind == UiMediaPickerItemKind::gif && !index &&
                                                         !fixture.favorites
                                                     ? MediaAttachmentFormat::webp
                                                     : MediaAttachmentFormat::png);
    *item = {fixture.first_asset + index,
             disabled ? 0 : key.id,
             fixture.kind == UiMediaPickerItemKind::category ? categories[index % 12]
             : disabled                                      ? utf8_literal("Vector sticker")
             : sticker                                       ? utf8_literal("Little comet")
                                                             : utf8_literal("Hello, friend"),
             disabled  ? utf8_literal("Vector stickers cannot be sent")
             : sticker ? utf8_literal("Original native fixture pack")
                       : utf8_literal("Original synthetic gallery fixture"),
             key.variant,
             sticker ? 64u : 96u,
             sticker ? 64u : 48u,
             fixture.kind,
             !disabled,
             key.flags};
    if (fixture.favorites) {
        const Utf8View labels[]{utf8_literal("Saved little comet"),   utf8_literal("Favorite this garden"),
                                utf8_literal("Saving favorite"),      utf8_literal("Removal failed"),
                                utf8_literal("Preview not supplied"), utf8_literal("Unsupported preview")};
        const Utf8View details[]{utf8_literal("Confirmed favorite - original offline artwork"),
                                 utf8_literal("Available favorite action - original offline artwork"),
                                 utf8_literal("Pending addition - confirmation has not arrived"),
                                 utf8_literal("Failed removal - confirmed favorite is retained"),
                                 utf8_literal("Neutral source placeholder - no residency claim"),
                                 utf8_literal("Known unavailable format - Send and Favorite disabled")};
        if (index >= 6)
            return false;
        item->label = labels[index];
        item->detail = details[index];
        item->favoritable = true;
        item->favorite = index == 0 || index == 3;
        item->favorite_pending = index == 2;
        item->favorite_failed = index == 3;
        if (index >= 4)
            item->asset_id = 0;
    }
    return true;
}

bool probe_ui_surface_motion() noexcept {
    auto& renderer = g_probe.renderer;
    if (!renderer.content_text || !renderer.ui_scene || !synchronize_probe_shell_layout())
        return false;
    UiMediaPickerScene gallery{};
    PopupScene popup{};
    PopupItem items[4]{};
    PopupRowWidget rows[4]{};
    UiWidget widgets[10]{};
    UiLayoutNode nodes[10]{};
    UiSettingsScene settings{};
    UiPageRegistry registry{};
    UiPage page{};
    const auto* old_gallery = renderer.media_picker_scene;
    const auto* old_popup = renderer.popup_scene;
    const auto* old_settings = renderer.settings_scene;
    const auto* old_focus = renderer.focus_owner;
    const auto old_opacity = renderer.layer_opacity;
    struct Restore {
        D3D11Renderer& renderer;
        UiMediaPickerScene& gallery;
        PopupScene& popup;
        UiSettingsScene& settings;
        const UiMediaPickerScene* old_gallery;
        const PopupScene* old_popup;
        const UiSettingsScene* old_settings;
        const UiWidgetManager* old_focus;
        RendererLayerOpacity opacity;
        ~Restore() noexcept {
            renderer.media_picker_scene = old_gallery;
            renderer_bind_popup_scene(&renderer, old_popup);
            renderer_bind_settings_scene(&renderer, old_settings);
            renderer_bind_focus_owner(&renderer, old_focus);
            renderer.layer_opacity = opacity;
            renderer.retained_presentation.valid = false;
            ui_media_picker_scene_release(&gallery);
            popup_scene_release(&popup);
            ui_settings_scene_release(&settings);
        }
    } restore{renderer,  gallery,      popup,     settings,   old_gallery,
              old_popup, old_settings, old_focus, old_opacity};
    renderer.layer_opacity = {1, 1, 1};
    const float scale = static_cast<float>(renderer.dpi) / 96.f;
    const UiRect viewport{0, 0, renderer.width / scale, renderer.height / scale};
    const auto content_viewport = ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::content);
    const auto* anchor =
        message_composer_scene_layout_node(g_probe.message_composer_scene, MessageComposerControl::gif);
    if (!anchor || !popup_scene_initialize(&popup, {items, 4}, {rows, 4}, {widgets, 10}, {nodes, 10}))
        return false;
    // Original solid artwork has deliberately distinctive channels. Native
    // media raster pixels can be identified independently of panels and ink.
    u8 pixels[32 * 32 * 4]{};
    for (u32 i = 0; i < 32 * 32; ++i) {
        pixels[i * 4] = 240;
        pixels[i * 4 + 1] = 64;
        pixels[i * 4 + 2] = 180;
        pixels[i * 4 + 3] = 255;
    }
    MediaGalleryProbeSource fixture{UiMediaPickerItemKind::gif, 0xaf000, 0, true};
    UiMediaPickerSource source{&fixture, 29, 1, 4, 1, {}, media_gallery_probe_resolve};
    UiMediaPickerContent content{};
    content.account_scope = source.account_scope;
    content.query_generation = source.query_generation;
    content.revision = 1;
    content.kind = UiMediaPickerKind::gif;
    content.phase = UiMediaPickerPhase::ready;
    content.edit.active = true;
    content.placeholder = utf8_literal("Search original fixtures");
    content.available_tabs = ui_media_picker_tab_bit(UiMediaPickerKind::gif);
    for (u32 i = 0; i < source.count; ++i) {
        UiMediaPickerItem item{};
        if (!source.resolve(source.context, i, &item) ||
            !renderer_upload_media_straight_rgba8(
                 &renderer,
                 {item.asset_id, item.variant, MediaAssetKind::catalog_preview, item.asset_flags}, 32, 32,
                 128, {pixels, sizeof(pixels)})
                 .ok())
            return false;
    }
    if (!ui_media_picker_scene_update(&gallery, &source, &content, content_viewport, anchor->rectangle) ||
        !gallery.cell_count)
        return false;
    renderer.media_picker_scene = &gallery;
    renderer_bind_focus_owner(&renderer, &gallery.widgets);
    const auto cell = gallery.projection->cells[0];
    const float hit_x = (cell.rectangle.left + cell.rectangle.right) * .5f;
    const float hit_y = (cell.rectangle.top + cell.rectangle.bottom) * .5f;
    const auto target = ui_media_picker_scene_hit_test(gallery, hit_x, hit_y);
    if (target.action != UiMediaPickerAction::item || target.source_index != 0)
        return false;
    const u32 calls = fixture.calls, revision = gallery.revision, generation = gallery.generation;
    const auto gallery_guard = [&]() noexcept {
        return renderer.layer_opacity.popup == 1 && renderer.layer_opacity.settings_choice == 1 &&
               fixture.calls == calls && gallery.revision == revision && gallery.generation == generation &&
               ui_media_picker_scene_current(gallery) &&
               ui_rect_equal(cell.rectangle, gallery.projection->cells[0].rectangle) &&
               ui_rect_equal(cell.clip, gallery.projection->cells[0].clip) &&
               ui_media_picker_target_equal(target, ui_media_picker_scene_hit_test(gallery, hit_x, hit_y));
    };
    const wchar_t* gallery_names[]{L"surface-motion-gallery-zero.bmp", L"surface-motion-gallery-half.bmp",
                                   L"surface-motion-gallery-one.bmp"};
    if (!surface_motion_probe_detail::measure(renderer, RendererLayerSurface::gallery, gallery.panel, true,
                                              gallery_names, gallery_guard))
        return false;
    renderer_bind_focus_owner(&renderer, nullptr);
    renderer.retained_presentation.valid = false;
    constexpr u64 emoji_id = 0xaf099;
    if (!renderer_upload_media_straight_rgba8(&renderer, media_asset_custom_emoji(emoji_id, false), 32, 32,
                                              128, {pixels, sizeof(pixels)})
             .ok())
        return false;
    items[0] = {utf8_literal("Original native preview"), 7, popup_item_enabled | popup_item_emoji_preview,
                0, emoji_id};
    items[1] = {utf8_literal("Second native action"), 8, popup_item_enabled};
    const UiRect popup_anchor{content_viewport.left + 80, content_viewport.top + 120,
                              content_viewport.left + 100, content_viewport.top + 140};
    if (!popup_scene_open(&popup, PopupKind::popover, 1, {items, 2}, popup_anchor, viewport, 280, true)
             .ok() ||
        !renderer_bind_popup_scene(&renderer, &popup))
        return false;
    PopupItemLayout row{};
    if (!popup_scene_resolve_item_layout(popup, 0, &row) || !row.visible)
        return false;
    const float popup_x = (row.visible_rectangle.left + row.visible_rectangle.right) * .5f;
    const float popup_y = (row.visible_rectangle.top + row.visible_rectangle.bottom) * .5f;
    const auto popup_hit = popup_scene_hit_test(popup, popup_x, popup_y);
    if (!popup_hit.inside || !popup_hit.enabled || popup_hit.action != 7)
        return false;
    const UiRect popup_rectangle = popup.rectangle;
    const u32 popup_revision = popup.revision;
    const auto popup_guard = [&]() noexcept {
        const auto hit = popup_scene_hit_test(popup, popup_x, popup_y);
        return renderer.layer_opacity.gallery == 1 && renderer.layer_opacity.settings_choice == 1 &&
               fixture.calls == calls && gallery.revision == revision && gallery.generation == generation &&
               popup.revision == popup_revision && ui_rect_equal(popup.rectangle, popup_rectangle) &&
               hit.inside == popup_hit.inside && hit.enabled == popup_hit.enabled &&
               hit.action == popup_hit.action && hit.source_index == popup_hit.source_index;
    };
    const wchar_t* popup_names[]{L"surface-motion-popup-zero.bmp", L"surface-motion-popup-half.bmp",
                                 L"surface-motion-popup-one.bmp"};
    if (!surface_motion_probe_detail::measure(renderer, RendererLayerSurface::popup, popup.rectangle, true,
                                              popup_names, popup_guard))
        return false;
    renderer.media_picker_scene = nullptr;
    ui_media_picker_scene_close(&gallery);
    popup_scene_close(&popup);
    const Utf8View choices[]{utf8_literal("Original option"), utf8_literal("Second option"),
                             utf8_literal("Third option")};
    UiSettingsControl control{1,
                              utf8_literal("Native choice"),
                              utf8_literal("The page remains opaque."),
                              UiSettingsControlKind::choice,
                              true,
                              false,
                              {choices, 3},
                              0,
                              {}};
    UiSettingsPageContent page_content{utf8_literal("Surface motion fixture"),
                                       utf8_literal("Original offline native page and choice popup."),
                                       {&control, 1},
                                       1};
    if (!ui_page_registry_initialize(&registry, {&page, 1}) ||
        !ui_page_registry_register(&registry, {{1, 7}, utf8_literal("Native choices"), &page_content})
             .ok() ||
        !ui_settings_scene_update(&settings, &registry, viewport))
        return false;
    renderer_bind_settings_scene(&renderer, &settings);
    UiRect choice_anchor{};
    if (!ui_settings_scene_control_anchor(settings, {UiSettingsTargetKind::control, {1, 7}, 1},
                                          &choice_anchor))
        return false;
    const u32 count = ui_settings_choice_items(control, 0, {items, 4});
    if (!count ||
        !popup_scene_open(&popup, PopupKind::settings_choice, 1, {items, count}, choice_anchor, viewport,
                          240, true)
             .ok() ||
        !popup_scene_resolve_item_layout(popup, 0, &row) || !row.visible)
        return false;
    const float choice_x = (row.visible_rectangle.left + row.visible_rectangle.right) * .5f;
    const float choice_y = (row.visible_rectangle.top + row.visible_rectangle.bottom) * .5f;
    const auto choice_hit = popup_scene_hit_test(popup, choice_x, choice_y);
    if (!choice_hit.inside || !choice_hit.enabled || choice_hit.action != kUiSettingsChoiceActionFirst)
        return false;
    const u32 choice_revision = popup.revision, settings_revision = settings.revision;
    const UiRect choice_rectangle = popup.rectangle;
    const auto choice_guard = [&]() noexcept {
        UiRect current_anchor{};
        const auto hit = popup_scene_hit_test(popup, choice_x, choice_y);
        return renderer.layer_opacity.gallery == 1 && renderer.layer_opacity.popup == 1 &&
               settings.revision == settings_revision && popup.revision == choice_revision &&
               ui_rect_equal(popup.rectangle, choice_rectangle) &&
               ui_settings_scene_control_anchor(settings, {UiSettingsTargetKind::control, {1, 7}, 1},
                                                &current_anchor) &&
               ui_rect_equal(choice_anchor, current_anchor) && hit.inside == choice_hit.inside &&
               hit.enabled == choice_hit.enabled && hit.action == choice_hit.action &&
               hit.source_index == choice_hit.source_index;
    };
    const wchar_t* choice_names[]{L"surface-motion-settings-choice-zero.bmp",
                                  L"surface-motion-settings-choice-half.bmp",
                                  L"surface-motion-settings-choice-one.bmp"};
    return surface_motion_probe_detail::measure(renderer, RendererLayerSurface::settings_choice,
                                                popup.rectangle, false, choice_names, choice_guard);
}

bool probe_media_underlay_and_quick_reactions() noexcept {
    using namespace surface_motion_probe_detail;
    auto& renderer = g_probe.renderer;
    const auto draw_changed = [&]() noexcept {
        const float clear[]{kDefaultTheme.base_lowest.red, kDefaultTheme.base_lowest.green,
                            kDefaultTheme.base_lowest.blue, 1};
        renderer.graphics.context->ClearRenderTargetView(renderer.graphics.target, clear);
        return d3d11_renderer_draw_stream(&renderer, kDefaultTheme, true, true, nullptr, 10000);
    };
    u32 stage = 1;
    struct Failure {
        u32& stage;
        ~Failure() noexcept {
            if (stage)
                log_u64(LogLevel::error, "probe", "media_underlay_failure_stage", stage);
        }
    } failure{stage};
    if (!g_probe.message_scene.count)
        return false;
    const auto first = g_probe.message_scene.rows[0];
    const auto* record = message_store_resolve(g_probe.message_view.store, first.message);
    const auto hash = utf8_literal("0123456789abcdef0123456789abcdef");
    if (!record || !user_store_set_avatar(&g_probe.users, record->author_id, hash))
        return false;
    const auto avatar_key =
        media_asset_user_avatar(record->author_id, user_avatar_variant(hash), false);
    GatewayMessageAttachment attachments[2]{};
    for (u32 i = 0; i < 2; ++i) {
        attachments[i].id = 0xba000 + i;
        attachments[i].filename = i ? utf8_literal("native-poster.mp4") : utf8_literal("native-alpha.png");
        attachments[i].content_type = i ? utf8_literal("video/mp4") : utf8_literal("image/png");
        attachments[i].url =
            i ? utf8_literal("https://cdn.discordapp.com/attachments/111/761857/native-poster.mp4")
              : utf8_literal("https://cdn.discordapp.com/attachments/111/761856/native-alpha.png");
        attachments[i].width = 192;
        attachments[i].height = 64;
        attachments[i].has_width = attachments[i].has_height = true;
    }
    GatewayMessageSticker sticker{};
    sticker.id = 0xba010;
    sticker.name = utf8_literal("Original transparent sticker");
    sticker.format_type = 1;
    GatewayMessageEvent event{};
    event.id = record->id;
    event.fields = gateway_message_field_attachments | gateway_message_field_sticker_items;
    event.attachments = {attachments, 2};
    event.stickers = {&sticker, 1};
    if (!message_rich_store_apply(&g_probe.rich_store, record->id, record->revision, event).ok() ||
        !rebuild_probe_content_text() || !probe_prepare_capture())
        return false;
    stage = 2;
    PaintStream paint{};
    struct ReleasePaint {
        PaintStream& paint;
        ~ReleasePaint() noexcept {
            paint_stream_release(&paint);
        }
    } release_paint{paint};
    MessageMediaCommand commands[8]{};
    MessageMediaCommandStream stream{};
    MessagePresentationResult presentation{};
    if (!paint_stream_initialize(&paint) || !paint_stream_reset(&paint) ||
        !message_media_command_stream_initialize(&stream, {commands, 8}) ||
        !message_presentation_build(g_probe.message_scene, kDefaultTheme, &paint, &stream, &presentation) ||
        stream.count != 3)
        return false;
    const auto& row = g_probe.message_scene.rows[0];
    const auto* avatar = message_scene_layout_node(g_probe.message_scene, row.avatar);
    if (!avatar || row.grouped)
        return false;
    const UiRect avatar_rectangle = avatar->rectangle;
    const auto fallback_alpha = [&](UiRect rectangle) noexcept {
        for (u32 i = 0; i < renderer.paint_stream.count; ++i) {
            const auto& q = renderer.paint_stream.quads[i];
            if (q.border_width == 0 && ui_rect_equal({q.left, q.top, q.right, q.bottom}, rectangle))
                return q.fill.alpha;
        }
        return -1.f;
    };
    if (fallback_alpha(avatar_rectangle) != 1 || renderer.media_quad_count)
        return false;
    for (u32 i = 0; i < stream.count; ++i)
        if (fallback_alpha(commands[i].rectangle) != 1)
            return false;
    Readback readback{};
    if (!readback.initialize(renderer) || !draw_changed() || !readback.read(renderer, 0))
        return false;
    stage = 3;
    // Original straight RGBA bands: absent, half-magenta, solid magenta.
    // The native upload path premultiplies them before the final draw.
    // The MP4 item exercises native poster composition, not an alpha video codec.
    u8 rgba[24 * 24 * 4]{};
    for (u32 y = 0; y < 24; ++y)
        for (u32 x = 0; x < 24; ++x) {
            const u8 alpha = x < 8 ? 0 : x < 16 ? 128 : 255;
            const u32 p = (y * 24 + x) * 4;
            rgba[p] = rgba[p + 2] = 255;
            rgba[p + 3] = alpha;
        }
    MediaAssetKey keys[3]{};
    for (u32 i = 0; i < stream.count; ++i) {
        const auto& command = commands[i];
        keys[i] =
            command.kind == MessageMediaCommandKind::sticker
                ? media_asset_sticker(command.id, command.format)
                : media_asset_attachment(
                      command.id, static_cast<MediaAttachmentFormat>(command.format),
                      media_resolution_tier_for_display(
                          ui_rect_width(command.rectangle), ui_rect_height(command.rectangle), renderer.dpi,
                          media_preview_extent_cap(static_cast<MediaAttachmentFormat>(command.format))));
        if (!renderer_upload_media_straight_rgba8(&renderer, keys[i], 24, 24, 24 * 4, {rgba, sizeof(rgba)})
                 .ok())
            return false;
    }
    if (!renderer_upload_media_straight_rgba8(&renderer, avatar_key, 24, 24, 24 * 4, {rgba, sizeof(rgba)})
             .ok() ||
        !probe_prepare_capture() || renderer.media_quad_count != 4)
        return false;
    if (fallback_alpha(avatar_rectangle) != 0)
        return false;
    for (u32 i = 0; i < stream.count; ++i)
        if (fallback_alpha(commands[i].rectangle) != 0)
            return false;
    if (!draw_changed() || !readback.read(renderer, 1))
        return false;
    // Obtain the actual native underlay with the admitted media hidden. No
    // theme/color assumption can stand in for the final composited pixels.
    for (u32 i = 0; i < renderer.media_quad_count; ++i)
        renderer.media_draw.quad_data[i].opacity = 0;
    const float underlay_clear[]{kDefaultTheme.base_lowest.red, kDefaultTheme.base_lowest.green,
                                 kDefaultTheme.base_lowest.blue, 1};
    renderer.graphics.context->ClearRenderTargetView(renderer.graphics.target, underlay_clear);
    if (!d3d11_renderer_draw_stream(&renderer, kDefaultTheme, false, true, nullptr, 10000) ||
        !readback.read(renderer, 2))
        return false;
    for (u32 i = 0; i < renderer.media_quad_count; ++i)
        renderer.media_draw.quad_data[i].opacity = 1;
    stage = 4;
    const UiRect rectangles[]{avatar_rectangle, commands[0].rectangle, commands[1].rectangle,
                              commands[2].rectangle};
    const float scale = renderer.dpi / 96.f;
    for (u32 item = 0; item < 4; ++item) {
        const auto rectangle = rectangles[item];
        const bool poster = item && commands[item - 1].ordinary_video;
        const auto play = poster ? ui_video_play_affordance_rectangle(rectangle, {}) : UiRect{};
        // The real poster retains its translucent Play control. Check media
        // composition in the exposed strip above that foreground control.
        const float sample_y = poster && ui_rect_has_area(play) ? (rectangle.top + play.top) * .5f
                                                                : (rectangle.top + rectangle.bottom) * .5f;
        for (u32 band = 0; band < 3; ++band) {
            const u32 x = static_cast<u32>(
                (rectangle.left + ui_rect_width(rectangle) * (1 + band * 2) / 6.f) * scale);
            const u32 y = static_cast<u32>(sample_y * scale);
            if (poster && ui_rect_contains(play, x / scale, y / scale))
                return false;
            if (x >= readback.width || y >= readback.height)
                return false;
            const u32 p = (y * readback.width + x) * 4;
            const auto* loaded = readback.images + readback.bytes + p;
            const auto* underlay = readback.images + readback.bytes * 2 + p;
            const u32 alpha = band == 0 ? 0 : band == 1 ? 128 : 255;
            for (u32 channel = 0; channel < 3; ++channel) {
                const u32 expected =
                    (channel == 1 ? 0 : alpha) + (underlay[channel] * (255 - alpha) + 127) / 255;
                if (maximum<u32>(loaded[channel], expected) - minimum<u32>(loaded[channel], expected) > 2) {
                    log_u64(LogLevel::error, "probe", "media_underlay_pixel_band", band);
                    log_u64(LogLevel::error, "probe", "media_underlay_pixel_item", item);
                    log_u64(LogLevel::error, "probe", "media_underlay_pixel_channel", channel);
                    log_u64(LogLevel::error, "probe", "media_underlay_pixel_expected", expected);
                    log_u64(LogLevel::error, "probe", "media_underlay_pixel_actual", loaded[channel]);
                    log_u64(LogLevel::error, "probe", "media_underlay_pixel_underlay", underlay[channel]);
                    log_u64(LogLevel::error, "probe", "media_underlay_pixel_width",
                            static_cast<u32>(ui_rect_width(rectangle)));
                    return false;
                }
            }
            if (!band && distance(loaded, readback.images + p) < 6) {
                log_u64(LogLevel::error, "probe", "media_underlay_missing_loaded_distance",
                        distance(loaded, readback.images + p));
                return false;
            }
        }
    }
    stage = 5;
    if (!message_action_bar_open(&g_probe.message_action_bar, g_probe.message_scene,
                                 {row.message_id, row.widget, row.source_index}, *record)
             .ok() ||
        !renderer_bind_message_action_bar(&renderer, &g_probe.message_action_bar) ||
        !renderer_refresh_interaction_text(&renderer, kDefaultTheme) || !probe_prepare_capture() ||
        !draw_changed() || !readback.read(renderer, 3))
        return false;
    for (u32 cell = 0; cell < kMessageActionBarQuickReactionCount; ++cell) {
        const auto clip = raster_clip_rectangle(g_probe.message_action_bar.items[cell].rectangle, scale,
                                                readback.width, readback.height);
        u32 ink = 0;
        const u8 background[]{static_cast<u8>(kDefaultTheme.base_lower.blue * 255.f + .5f),
                              static_cast<u8>(kDefaultTheme.base_lower.green * 255.f + .5f),
                              static_cast<u8>(kDefaultTheme.base_lower.red * 255.f + .5f)};
        for (i32 y = clip.top + 3; y < clip.bottom - 3; ++y)
            for (i32 x = clip.left + 3; x < clip.right - 3; ++x)
                if (distance(readback.images + readback.bytes * 3 + (y * readback.width + x) * 4,
                             background) > 90)
                    ++ink;
        if (ink < 20)
            return false;
    }
    stage = 6;
    const auto retained = snapshot(renderer);
    for (u32 i = 0; i < 3; ++i)
        if (!probe_prepare_capture() || !draw_fixed(renderer) || !same(retained, snapshot(renderer)))
            return false;
    stage = 7;
    // More only replaces the retained interaction suffix. It must not rerun
    // message preparation, change heights, or move the base native glyphs.
    PopupScene menu{};
    PopupItem menu_items[kMessageContextMaximumItems]{};
    PopupRowWidget menu_rows[kMessageContextMaximumItems]{};
    UiWidget menu_widgets[2 + kMessageContextMaximumItems * kPopupWidgetsPerItem]{};
    UiLayoutNode menu_nodes[2 + kMessageContextMaximumItems * kPopupWidgetsPerItem]{};
    const auto* previous_popup = renderer.popup_scene;
    struct ReleaseMenu {
        D3D11Renderer& renderer;
        PopupScene& menu;
        const PopupScene* previous;
        ~ReleaseMenu() noexcept {
            renderer.popup_scene = previous;
            popup_scene_release(&menu);
        }
    } release_menu{renderer, menu, previous_popup};
    MessageContextMenuModel model{};
    const u32 prefix_quads = renderer.content_text_prefix.quads;
    const u64 prefix_hash =
        bytes_hash(1469598103934665603ull, g_probe.text.quads, prefix_quads * sizeof(GlyphQuad));
    const u32 measurements = g_probe.text_measure_cache.backend_calls;
    const u32 preparations = g_probe.message_scene.preparation_passes;
    const u32 layouts = g_probe.message_scene.layout_passes;
    const auto row_rectangle = message_scene_layout_node(g_probe.message_scene, row.widget)->rectangle;
    if (!message_context_menu_build(&model, *record, false, false, false).ok() ||
        !popup_scene_initialize(&menu, {menu_items, kMessageContextMaximumItems},
                                {menu_rows, kMessageContextMaximumItems},
                                {menu_widgets, sizeof(menu_widgets) / sizeof(menu_widgets[0])},
                                {menu_nodes, sizeof(menu_nodes) / sizeof(menu_nodes[0])}) ||
        !popup_scene_open(&menu, PopupKind::context_menu, record->id, message_context_menu_items(model),
                          g_probe.message_action_bar.items[5].rectangle, g_probe.message_scene.viewport,
                          260)
             .ok() ||
        !renderer_bind_popup_scene(&renderer, &menu))
        return false;
    const auto base_current = [&]() noexcept {
        return renderer.content_text_prefix.quads == prefix_quads &&
               bytes_hash(1469598103934665603ull, g_probe.text.quads, prefix_quads * sizeof(GlyphQuad)) ==
                   prefix_hash &&
               g_probe.text_measure_cache.backend_calls == measurements &&
               g_probe.message_scene.preparation_passes == preparations &&
               g_probe.message_scene.layout_passes == layouts &&
               ui_rect_equal(message_scene_layout_node(g_probe.message_scene, row.widget)->rectangle,
                             row_rectangle);
    };
    for (u32 i = 0; i < 3; ++i) {
        bool changed = false;
        if (!popup_scene_select_index(&menu, i, &changed) ||
            !renderer_rebuild_content_text(&renderer, kDefaultTheme) || !probe_prepare_capture() ||
            !draw_changed() || !base_current())
            return false;
    }
    if (!popup_scene_close(&menu) || !renderer_rebuild_content_text(&renderer, kDefaultTheme) ||
        !probe_prepare_capture() || !draw_changed() || !base_current())
        return false;
    stage = 0;
    log_write(LogLevel::info, "probe",
              "media_underlay_alpha_three_quick_emoji_and_more_retention_native_passed");
    return true;
}

bool probe_media_gallery(bool favorites = false) noexcept {
    auto& renderer = g_probe.renderer;
    if (!renderer.preview_media_atlas || !renderer.content_text || !renderer.ui_scene)
        return false;
    if (!synchronize_probe_shell_layout() || !g_probe.message_composer_scene.active ||
        renderer.message_composer_scene != &g_probe.message_composer_scene) {
        log_write(LogLevel::error, "probe", "media_gallery_bound_composer_setup_failed");
        return false;
    }
    // Only this selected offline gallery probe uses the larger surface/bank.
    // It retains the prior category/GIF identities plus30 sticker records;
    // ordinary codec fixtures still initialize32 entries in256x256 pixels.
    constexpr u16 gallery_preview_extent = 512;
    if (renderer.preview_media_atlas != &g_probe.preview_media_atlas ||
        renderer.media_gpu.preview_texture || g_probe.preview_media_atlas.entry_count ||
        !media_atlas_initialize(&g_probe.preview_media_atlas, gallery_preview_extent,
                                gallery_preview_extent, 1,
                                {g_probe.preview_media_entries, kProbeGalleryMediaEntryCapacity},
                                {g_probe.preview_media_buckets, kProbeGalleryMediaEntryCapacity * 2},
                                {g_probe.preview_media_allocations, kProbeGalleryMediaEntryCapacity},
                                {g_probe.preview_media_rows, kProbeMediaRowCapacity})) {
        log_write(LogLevel::error, "probe", "media_gallery_preview_fixture_storage_not_fresh");
        return false;
    }
    UiMediaPickerScene gallery{};
    MediaGalleryProbeSource fixture{};
    fixture.favorites = favorites;
    if (favorites) {
        fixture.kind = UiMediaPickerItemKind::gif;
        fixture.first_asset = 0x9f000;
    }
    UiMediaPickerSource source{&fixture, 17, 1, 12, 1, {}, media_gallery_probe_resolve};
    if (favorites)
        source.count = 6;
    UiMediaPickerContent content{};
    content.account_scope = source.account_scope;
    content.query_generation = source.query_generation;
    content.revision = 1;
    content.kind = UiMediaPickerKind::gif;
    content.phase = UiMediaPickerPhase::ready;
    content.edit.active = true;
    content.placeholder = utf8_literal("Search GIFs");
    if (favorites) {
        content.placeholder = utf8_literal("Search favorites");
        content.status = utf8_literal("Favorites - original offline fixtures");
    }
    content.available_tabs = ui_media_picker_tab_bit(UiMediaPickerKind::gif) |
                             ui_media_picker_tab_bit(UiMediaPickerKind::sticker) |
                             ui_media_picker_tab_bit(UiMediaPickerKind::emoji);
    const auto* previous_gallery = renderer.media_picker_scene;
    const auto* previous_focus = renderer.focus_owner;
    const u32 original_width = renderer.width, original_height = renderer.height,
              original_dpi = renderer.dpi;
    const auto original_content = ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::content);
    const float minimum_width_dips = original_content.left + app_detail::kMinimumAccountMessageWidthDips;
    u32 stage = 0, failure = 0;
    bool layers = false, resident = false, animated = false, missing = false, stale = false;
    bool outline = false, search = false, disabled = false, bounded = false, narrow = false;
    const auto require = [&](bool value, u32 checkpoint) noexcept {
        if (!value && !failure)
            failure = checkpoint;
        return value;
    };
    const auto update = [&]() noexcept {
        auto viewport = ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::content);
        viewport.right = static_cast<float>(renderer.width) * 96.f / maximum(renderer.dpi, 1u);
        const auto* anchor = message_composer_scene_layout_node(g_probe.message_composer_scene,
                                                                content.kind == UiMediaPickerKind::gif
                                                                    ? MessageComposerControl::gif
                                                                    : MessageComposerControl::sticker);
        bool anchored = anchor && ui_rect_valid(anchor->rectangle) &&
                        ui_widget_manager_input_enabled(g_probe.message_composer_scene.widgets,
                                                        content.kind == UiMediaPickerKind::gif
                                                            ? g_probe.message_composer_scene.gif
                                                            : g_probe.message_composer_scene.sticker);
        if (!anchored) {
            // A compact composer folds the GIF and sticker buttons into the emoji button.
            anchor = message_composer_scene_layout_node(g_probe.message_composer_scene,
                                                        MessageComposerControl::emoji);
            anchored = anchor && ui_rect_valid(anchor->rectangle) &&
                       ui_widget_manager_input_enabled(g_probe.message_composer_scene.widgets,
                                                       g_probe.message_composer_scene.emoji);
        }
        if (!anchored) {
            log_write(LogLevel::error, "probe", "media_gallery_native_anchor_unavailable");
            return false;
        }
        const bool accepted =
            ui_media_picker_scene_update(&gallery, &source, &content, viewport, anchor->rectangle);
        if (!accepted) {
            log_u64(LogLevel::error, "probe", "media_gallery_edit_active", content.edit.active);
            log_u64(LogLevel::error, "probe", "media_gallery_available_width_dips",
                    static_cast<u64>(ui_rect_width(viewport)));
            log_u64(LogLevel::error, "probe", "media_gallery_available_height_dips",
                    static_cast<u64>(ui_rect_height(viewport)));
        }
        return accepted;
    };
    const auto show = [&](const wchar_t* name, bool hover = false) noexcept {
        ++stage;
        if (!require(update(), stage * 10 + 1))
            return false;
        if (hover && gallery.cell_count) {
            const auto r = gallery.projection->cells[0].rectangle;
            ui_media_picker_scene_hover(&gallery, (r.left + r.right) * .5f, (r.top + r.bottom) * .5f, 1000);
        }
        renderer.media_picker_scene = &gallery;
        renderer_bind_focus_owner(&renderer, &gallery.widgets);
        renderer.retained_presentation.valid = false;
        if (!require(renderer_rebuild_content_text(&renderer, kDefaultTheme), stage * 10 + 2) ||
            !require(probe_present(&renderer, kDefaultTheme), stage * 10 + 3))
            return false;
        layers = renderer.overlay_paint_first_batch <= renderer.media_picker_paint_first_batch &&
                 renderer.media_picker_paint_first_batch <= renderer.media_picker_foreground_first_batch &&
                 renderer.media_picker_foreground_first_batch <= renderer.media_picker_paint_end_batch &&
                 renderer.media_picker_paint_end_batch <= renderer.menu_paint_first_batch &&
                 renderer.media_picker_media_first_batch <= renderer.media_picker_media_end_batch &&
                 renderer.media_picker_media_end_batch <= renderer.popup_media_first_batch &&
                 renderer.overlay_text_first_batch <= renderer.media_picker_text_first_batch &&
                 renderer.media_picker_text_first_batch < renderer.media_picker_text_end_batch &&
                 renderer.media_picker_text_end_batch <= renderer.menu_text_first_batch;
        bool catalog_only = true;
        for (u32 b = renderer.media_picker_media_first_batch; b < renderer.media_picker_media_end_batch;
             ++b) {
            const auto& batch = renderer.media_draw.batch_data[b];
            for (u32 q = batch.first_quad; q < batch.first_quad + batch.quad_count; ++q) {
                const auto& quad = renderer.media_draw.quad_data[q];
                catalog_only = catalog_only && renderer.media_draw.domain_data[q] == 1 &&
                               quad.clip_radius == kUiControlCornerRadius &&
                               ui_rect_has_area(quad.rounded_clip) && quad.v0 < 0 && quad.v1 < 0;
            }
        }
        return require(layers && catalog_only && d3d11_renderer_interaction_text_state_current(renderer) &&
                           gallery.cell_count <= kUiMediaPickerVisibleCells &&
                           renderer.media_picker_layout.active,
                       stage * 10 + 4) &&
               require(probe_capture_frame(name), stage * 10 + 5);
    };
    const auto upload_art = [&](u32 count, bool sticker) noexcept {
        // The source phase has changed before this synchronous preload. The
        // previous view releases its visibility pins before the next bank's
        // bounded fixture records are uploaded. No draw runs between phases.
        media_atlas_begin_visibility(renderer.preview_media_atlas);
        for (u32 i = 0; i < count; ++i) {
            // Keep one deliberate missing thumbnail. No file, URL or account is used.
            if ((!favorites && i == 3) || (sticker && i == 10))
                continue;
            UiMediaPickerItem item{};
            if (!source.resolve(source.context, i, &item))
                return false;
            if (!item.enabled || !item.asset_id ||
                (fixture.kind == UiMediaPickerItemKind::gif && !i && !favorites))
                continue;
            for (u32 y = 0; y < 64; ++y)
                for (u32 x = 0; x < 64; ++x) {
                    auto* p = g_probe.png_scanlines + (y * 64 + x) * 4;
                    const i32 dx = static_cast<i32>(x) - 32, dy = static_cast<i32>(y) - 32;
                    const bool ink = !sticker || dx * dx + dy * dy < 760;
                    const bool eye =
                        sticker && y >= 23 && y <= 28 && ((x >= 20 && x <= 25) || (x >= 40 && x <= 45));
                    p[0] = eye ? 32 : static_cast<u8>(80 + (i * 31 + x * 2) % 150);
                    p[1] = eye ? 32 : static_cast<u8>(100 + (i * 53 + y) % 130);
                    p[2] = eye ? 32 : static_cast<u8>(130 + (i * 17 + x + y) % 110);
                    p[3] = ink ? 255 : 0;
                }
            const MediaAssetKey key{item.asset_id, item.variant, MediaAssetKind::catalog_preview,
                                    item.asset_flags};
            const auto uploaded = renderer_upload_media_straight_rgba8(
                &renderer, key, 64, 64, 256, {g_probe.png_scanlines, 64 * 64 * 4});
            if (!uploaded.ok()) {
                log_u64(LogLevel::error, "probe", "media_gallery_upload_error",
                        static_cast<u8>(uploaded.error));
                log_u64(LogLevel::error, "probe", "media_gallery_upload_native_error",
                        static_cast<u32>(uploaded.native_error));
                log_u64(LogLevel::error, "probe", "media_gallery_upload_allocation_error",
                        static_cast<u8>(uploaded.allocation.error));
                log_u64(LogLevel::error, "probe", "media_gallery_upload_key_valid",
                        media_asset_key_valid(key));
                log_u64(LogLevel::error, "probe", "media_gallery_upload_flags", key.flags);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_preview_kind",
                        media_asset_large_preview(key));
                log_u64(LogLevel::error, "probe", "media_gallery_upload_preview_tier",
                        static_cast<u8>(media_asset_resolution_tier(key)));
                log_u64(LogLevel::error, "probe", "media_gallery_upload_dimensions", 64);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_payload_bytes", 64 * 64 * 4);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_primary_atlas_width",
                        renderer.media_atlas ? renderer.media_atlas->allocations.width : 0);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_preview_atlas_width",
                        renderer.preview_media_atlas->allocations.width);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_preview_atlas_height",
                        renderer.preview_media_atlas->allocations.height);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_preview_padding",
                        renderer.preview_media_atlas->allocations.padding);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_preview_entries",
                        renderer.preview_media_atlas->entry_count);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_preview_capacity",
                        renderer.preview_media_atlas->entry_capacity);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_preview_gpu_width",
                        renderer.media_gpu.preview_width);
                log_u64(LogLevel::error, "probe", "media_gallery_upload_preview_gpu_ready",
                        renderer.media_gpu.preview_texture && renderer.media_gpu.preview_target &&
                            renderer.media_gpu.preview_texture_uav);
                return false;
            }
        }
        return true;
    };
    if (favorites) {
        bool confirmed_hover = false, keyboard_favorite = false, pending_no_send = false;
        bool failed_confirmed = false, neutral_source = false, disabled_source = false;
        bool clipped_star = false, stale_favorite = false, scene_bounded = false;
        bool valid = require(upload_art(6, false) && update() && gallery.cell_count == 6, 101);
        UiRect button{}, full_button{};
        if (valid) {
            const auto r = gallery.projection->cells[0].rectangle;
            ui_media_picker_scene_hover(&gallery, (r.left + r.right) * .5f, (r.top + r.bottom) * .5f, 1000);
            valid = require(ui_media_picker_scene_favorite(gallery, 0, &button, &full_button), 102);
        }
        if (valid) {
            const float x = (button.left + button.right) * .5f, y = (button.top + button.bottom) * .5f;
            ui_media_picker_scene_hover(&gallery, x, y, 1000);
            confirmed_hover =
                gallery.projection->cells[0].item.favorite &&
                !gallery.projection->cells[0].item.favorite_pending &&
                ui_media_picker_scene_hit_test(gallery, x, y).action == UiMediaPickerAction::favorite;
            valid = require(confirmed_hover, 103) && show(L"media-favorites-20261003-hover-confirmed.bmp");
            const ProbePixelSample sample{{x, y}, kDefaultTheme.accent, 35};
            valid = valid && require(probe_exact_pixels({&sample, 1}), 104);
        }
        if (valid) {
            ui_media_picker_scene_hover(&gallery, gallery.panel.left - 1, gallery.panel.top - 1, 1100);
            valid = require(ui_media_picker_scene_select(&gallery, 0), 105);
            keyboard_favorite =
                valid &&
                ui_media_picker_scene_selected_favorite_target(gallery).action ==
                    UiMediaPickerAction::favorite &&
                ui_media_picker_scene_selected_target(gallery).action == UiMediaPickerAction::item;
            valid = valid && require(keyboard_favorite, 106) &&
                    show(L"media-favorites-20261003-keyboard-focus.bmp");
        }
        if (valid) {
            valid = require(ui_media_picker_scene_select(&gallery, 2) &&
                                ui_media_picker_scene_favorite(gallery, 2, &button),
                            107);
            if (valid) {
                const auto& cell = gallery.projection->cells[2];
                pending_no_send =
                    cell.item.favorite_pending && !cell.item.favorite &&
                    ui_media_picker_scene_selected_favorite_target(gallery).action ==
                        UiMediaPickerAction::none &&
                    ui_media_picker_scene_hit_test(gallery, (button.left + button.right) * .5f,
                                                   (button.top + button.bottom) * .5f)
                            .action == UiMediaPickerAction::none &&
                    ui_media_picker_scene_hit_test(gallery, cell.rectangle.left + 12,
                                                   cell.rectangle.top + 12)
                            .action == UiMediaPickerAction::item;
                valid = require(pending_no_send, 108) && show(L"media-favorites-20261003-pending.bmp");
            }
        }
        if (valid) {
            valid = require(ui_media_picker_scene_select(&gallery, 3) &&
                                ui_media_picker_scene_favorite(gallery, 3, &button),
                            109);
            if (valid) {
                failed_confirmed = gallery.projection->cells[3].item.favorite &&
                                   gallery.projection->cells[3].item.favorite_failed &&
                                   ui_media_picker_scene_selected_favorite_target(gallery).action ==
                                       UiMediaPickerAction::favorite;
                valid = require(failed_confirmed, 110) &&
                        show(L"media-favorites-20261003-failed-confirmed.bmp");
                const ProbePixelSample sample{
                    {(button.left + button.right) * .5f, (button.top + button.bottom) * .5f},
                    kDefaultTheme.accent,
                    35};
                valid = valid && require(probe_exact_pixels({&sample, 1}), 111);
            }
        }
        if (valid) {
            valid = require(ui_media_picker_scene_select(&gallery, 4), 112);
            UiMediaPickerMediaSlot slot{};
            neutral_source = valid && gallery.projection->cells[4].item.enabled &&
                             !gallery.projection->cells[4].item.asset_id &&
                             !ui_media_picker_scene_media(gallery, 4, &slot);
            const auto& unavailable = gallery.projection->cells[5];
            disabled_source =
                !unavailable.item.enabled && !unavailable.item.asset_id &&
                !ui_media_picker_scene_media(gallery, 5, &slot) &&
                !ui_media_picker_scene_favorite(gallery, 5, &button) &&
                ui_media_picker_scene_hit_test(gallery, unavailable.clip.left + 1, unavailable.clip.top + 1)
                        .action == UiMediaPickerAction::none;
            valid = valid && require(neutral_source && disabled_source, 113) &&
                    show(L"media-favorites-20261003-missing-preview.bmp");
        }
        if (valid) {
            valid = require(ui_media_picker_scene_select(&gallery, 0) &&
                                ui_media_picker_scene_scroll(
                                    &gallery, gallery.style.metrics.content_inset +
                                                  maximum(4.f, gallery.style.metrics.gap) + 10.f) &&
                                ui_media_picker_scene_favorite(gallery, 0, &button, &full_button),
                            114);
            if (valid) {
                const auto& cell = gallery.projection->cells[0];
                clipped_star =
                    ui_rect_equal(button, ui_rect_intersection(full_button, cell.clip)) &&
                    full_button.top < button.top && ui_rect_height(button) < ui_rect_height(full_button) &&
                    ui_media_picker_scene_hit_test(gallery, (button.left + button.right) * .5f,
                                                   (button.top + button.bottom) * .5f)
                            .action == UiMediaPickerAction::favorite &&
                    ui_media_picker_scene_hit_test(gallery, (full_button.left + full_button.right) * .5f,
                                                   button.top - .5f)
                            .action != UiMediaPickerAction::favorite;
                valid = require(clipped_star, 115) && show(L"media-favorites-20261003-partial-clip.bmp");
            }
        }
        if (valid) {
            const float x = (button.left + button.right) * .5f, y = (button.top + button.bottom) * .5f;
            valid = require(ui_media_picker_scene_pointer_down(&gallery, x, y), 116);
            ++source.revision;
            stale_favorite =
                !ui_media_picker_scene_favorite(gallery, 0, &button) &&
                ui_media_picker_scene_selected_favorite_target(gallery).action ==
                    UiMediaPickerAction::none &&
                ui_media_picker_scene_pointer_up(&gallery, x, y).action == UiMediaPickerAction::none;
            // The picker's storage is fixed-capacity (kUiMediaPickerWidgetCapacity), not per item.
            scene_bounded = gallery.cell_count <= 48 && gallery.widgets.count <= 96 &&
                            gallery.storage.arena.committed <= 48 * 1024;
            valid = valid && require(stale_favorite && scene_bounded, 117);
        }
        const u32 favorite_cells = gallery.cell_count, favorite_widgets = gallery.widgets.count;
        const u32 favorite_scene_bytes = static_cast<u32>(gallery.storage.arena.committed);
        renderer.media_picker_scene = previous_gallery;
        renderer_bind_focus_owner(&renderer, previous_focus);
        renderer.retained_presentation.valid = false;
        ui_media_picker_scene_release(&gallery);
        valid = valid && layers && confirmed_hover && keyboard_favorite && pending_no_send &&
                failed_confirmed && neutral_source && disabled_source && clipped_star && stale_favorite &&
                scene_bounded;
        wchar_t directory[1024]{};
        const DWORD chars = GetEnvironmentVariableW(L"MAICORD_UI_PROBE_CAPTURE_DIR", directory, 900);
        if (chars && chars < 900) {
            constexpr wchar_t filename[] = L"/media-favorites-20261003-quality.json";
            CopyMemory(directory + chars, filename, sizeof(filename));
            char report[1024]{};
            const auto yes = [](bool value) noexcept { return value ? "true" : "false"; };
            const int bytes = wsprintfA(
                report,
                "{\"fixture\":\"native-media-favorites\",\"network\":false,\"pass\":%s,\"layer_order\":%s,"
                "\"confirmed_hover\":%s,\"keyboard_favorite\":%s,\"pending_no_send\":%s,\"failed_retains_"
                "confirmed\":%s,"
                "\"neutral_missing_source\":%s,\"disabled_source\":%s,\"clipped_star_geometry\":%s,\"stale_"
                "favorite_guard\":%s,"
                "\"scene_bounded\":%s,\"cells\":%u,\"widgets\":%u,\"scene_bytes\":%u,"
                "\"star_clip_dip16\":[%d,%d,%d,%d],\"star_full_dip16\":[%d,%d,%d,%d],"
                "\"captures\":%u,\"failure_stage\":%u}",
                yes(valid), yes(layers), yes(confirmed_hover), yes(keyboard_favorite), yes(pending_no_send),
                yes(failed_confirmed), yes(neutral_source), yes(disabled_source), yes(clipped_star),
                yes(stale_favorite), yes(scene_bounded), favorite_cells, favorite_widgets,
                favorite_scene_bytes, static_cast<i32>(button.left * 16), static_cast<i32>(button.top * 16),
                static_cast<i32>(button.right * 16), static_cast<i32>(button.bottom * 16),
                static_cast<i32>(full_button.left * 16), static_cast<i32>(full_button.top * 16),
                static_cast<i32>(full_button.right * 16), static_cast<i32>(full_button.bottom * 16), stage,
                failure);
            const HANDLE file = CreateFileW(directory, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD written = 0;
            const bool saved = file != INVALID_HANDLE_VALUE && bytes > 0 &&
                               WriteFile(file, report, static_cast<DWORD>(bytes), &written, nullptr) &&
                               written == static_cast<DWORD>(bytes);
            if (file != INVALID_HANDLE_VALUE)
                CloseHandle(file);
            valid = valid && saved;
        }
        log_u64(valid ? LogLevel::info : LogLevel::error, "probe", "media_favorites_native_failure_stage",
                failure);
        return valid;
    }
    bool valid = require(show(L"media-gallery-20261003-gif-categories-missing.bmp"), 1);
    missing = valid && renderer.media_picker_media_first_batch == renderer.media_picker_media_end_batch;
    valid = valid && require(upload_art(12, false), 2) &&
            show(L"media-gallery-20261003-gif-categories.bmp", true);
    resident = valid && renderer.media_picker_media_first_batch < renderer.media_picker_media_end_batch;
    if (valid) {
        const auto& cell = gallery.projection->cells[0];
        const auto click =
            ui_media_picker_scene_hit_test(gallery, cell.rectangle.left + 12, cell.rectangle.top + 12);
        const auto stamp = renderer_capture_presentation_stamp(renderer, kDefaultTheme);
        ui_media_picker_scene_pointer_down(&gallery, cell.rectangle.left + 12, cell.rectangle.top + 12);
        ++source.query_generation;
        ++content.query_generation;
        ++source.revision;
        ++content.revision;
        UiMediaPickerMediaSlot slot{};
        stale =
            click.action == UiMediaPickerAction::category && !ui_media_picker_scene_current(gallery) &&
            !ui_media_picker_scene_media(gallery, 0, &slot) &&
            !d3d11_renderer_interaction_text_state_current(renderer) &&
            !renderer_presentation_stamp_equal(
                stamp, renderer_capture_presentation_stamp(renderer, kDefaultTheme)) &&
            ui_media_picker_scene_pointer_up(&gallery, cell.rectangle.left + 12, cell.rectangle.top + 12)
                    .action == UiMediaPickerAction::none;
        valid = require(stale, 3);
    }
    fixture.kind = UiMediaPickerItemKind::gif;
    fixture.first_asset = 0x7f000;
    source.count = 40;
    content.edit = {utf8_literal("hello"), 0, 5, true, true, false};
    content.back = true;
    ++content.revision;
    valid = valid && require(upload_art(10, false), 4);
    AnimatedWebpFixtureResult encoded{};
    AnimatedWebpImage animation{};
    AnimatedWebpFrame previous{};
    if (valid)
        valid = require(build_lossless_animated_webp_fixture(
                            {g_probe.png_encoded, sizeof(g_probe.png_encoded)}, &encoded) &&
                            animated_webp_parse({g_probe.png_encoded, encoded.encoded_bytes}, &animation,
                                                &g_probe.animated_webp_workspace)
                                .ok(),
                        5);
    for (u32 frame = 0; valid && frame < 2; ++frame) {
        const auto decoded = animated_webp_decode_frame_rgba8(
            {g_probe.png_encoded, encoded.encoded_bytes}, frame, &animation,
            &g_probe.animated_webp_workspace, &g_probe.vp8l_workspace,
            {g_probe.png_scanlines, sizeof(g_probe.png_scanlines)});
        const auto key =
            media_asset_catalog_preview(fixture.first_asset, 1, MediaAttachmentFormat::webp, true);
        if (!require(decoded.ok(), 6 + frame)) {
            valid = false;
            break;
        }
        const auto uploaded =
            frame == 0
                ? renderer_begin_animated_webp(&renderer, key, animation.width, animation.height,
                                               animation.background_rgba, decoded.frame,
                                               {g_probe.png_scanlines, decoded.frame_decode.rgba_bytes})
                : renderer_advance_animated_webp(&renderer, key, animation.background_rgba, previous,
                                                 decoded.frame,
                                                 {g_probe.png_scanlines, decoded.frame_decode.rgba_bytes});
        valid = require(uploaded.ok() && animated_webp_atlas_pixels_exact(uploaded.allocation, frame, true),
                        6 + frame) &&
                show(frame ? L"media-gallery-20261003-gif-search-frame-1.bmp"
                           : L"media-gallery-20261003-gif-search-frame-0.bmp",
                     true);
        previous = decoded.frame;
    }
    animated = valid && previous.duration_ms == 0 && animation.frame_count >= 2;
    search = valid && renderer.media_picker_edit_geometry.selection_count > 0;
    if (valid) {
        content.edit.focused = false;
        ++content.revision;
        valid = require(update() && ui_media_picker_scene_select(&gallery, 0), 8) &&
                show(L"media-gallery-20261003-gif-keyboard-selected.bmp");
    }
    if (valid) {
        const auto r = gallery.projection->cells[0].rectangle;
        const ProbePixelSample sample{{r.left + .5f, (r.top + r.bottom) * .5f}, kDefaultTheme.accent, 35};
        outline = probe_exact_pixels({&sample, 1});
        valid = require(search && outline, 8);
        const u32 calls = fixture.calls;
        valid = valid &&
                require(ui_media_picker_scene_scroll(&gallery, 600) &&
                            fixture.calls - calls <= kUiMediaPickerVisibleCells,
                        9) &&
                show(L"media-gallery-20261003-gif-search-scroll.bmp");
        bounded = valid && fixture.calls - calls <= 2 * kUiMediaPickerVisibleCells;
    }
    fixture.kind = UiMediaPickerItemKind::sticker;
    fixture.first_asset = 0x8f000;
    source.count = 32;
    UiMediaPickerSection sections[]{{31, 0, 12, utf8_literal("Original comet pack"), true},
                                    {32, 12, 20, utf8_literal("Native garden pack"), true}};
    source.sections = {sections, 2};
    ++source.query_generation;
    ++source.revision;
    content.query_generation = source.query_generation;
    content.kind = UiMediaPickerKind::sticker;
    content.placeholder = utf8_literal("Search stickers");
    content.edit = {};
    content.edit.active = true;
    content.back = false;
    ++content.revision;
    valid = valid && require(upload_art(32, true), 10) &&
            show(L"media-gallery-20261003-sticker-sections.bmp", true);
    if (valid) {
        const auto cells = ui_media_picker_scene_cells(gallery);
        for (u32 i = 0; i < cells.count; ++i)
            if (cells[i].source_index == 10) {
                const auto& cell = cells[i];
                disabled = !cell.item.enabled && !cell.item.asset_id &&
                           ui_media_picker_scene_hit_test(gallery, cell.rectangle.left + 12,
                                                          cell.rectangle.top + 12)
                                   .action == UiMediaPickerAction::none;
            }
        valid = require(disabled, 11);
    }
    content.edit = {utf8_literal("comet"), 5, 5, true, true, false};
    source.sections = {};
    source.count = 16;
    ++source.revision;
    ++content.revision;
    valid = valid && show(L"media-gallery-20261003-sticker-search.bmp", true);
    renderer.media_picker_scene = nullptr;
    renderer_set_dpi(&renderer, 120);
    valid = valid &&
            require(renderer_resize(&renderer, 1125, 1250) && synchronize_probe_shell_layout(), 12) &&
            show(L"media-gallery-20261003-sticker-tall-125.bmp", true);
    renderer.media_picker_scene = nullptr;
    valid = valid &&
            require(renderer_resize(&renderer, static_cast<u32>(minimum_width_dips * 1.25f + .999f), 680) &&
                        synchronize_probe_shell_layout({}, {}, 0, 0, false),
                    13) &&
            show(L"media-gallery-20261003-sticker-narrow-125.bmp", true);
    narrow = valid && gallery.panel.left >= gallery.viewport.left &&
             ui_rect_width(gallery.panel) < kUiMediaPickerWidth &&
             gallery.panel.right <= static_cast<float>(renderer.width) * 96.f / renderer.dpi &&
             gallery.panel.top >= gallery.viewport.top && gallery.panel.bottom <= gallery.viewport.bottom;
    renderer.media_picker_scene = nullptr;
    renderer_set_dpi(&renderer, original_dpi);
    const bool restored =
        renderer_resize(&renderer, original_width, original_height) && synchronize_probe_shell_layout();
    renderer.media_picker_scene = previous_gallery;
    renderer_bind_focus_owner(&renderer, previous_focus);
    renderer.retained_presentation.valid = false;
    ui_media_picker_scene_release(&gallery);
    valid = valid && restored && layers && resident && animated && missing && stale && outline && search &&
            disabled && bounded && narrow;
    wchar_t directory[1024]{};
    const DWORD chars = GetEnvironmentVariableW(L"MAICORD_UI_PROBE_CAPTURE_DIR", directory, 900);
    if (chars && chars < 900) {
        constexpr wchar_t filename[] = L"/media-gallery-20261003-quality.json";
        CopyMemory(directory + chars, filename, sizeof(filename));
        char report[1024]{};
        const auto yes = [](bool value) noexcept { return value ? "true" : "false"; };
        const int bytes = wsprintfA(
            report,
            "{\"fixture\":\"native-media-gallery\",\"network\":false,\"pass\":%s,\"layer_order\":%s,"
            "\"resident_preview\":%s,\"animated_webp\":%s,\"missing_preview\":%s,\"stale_scope_guard\":%s,"
            "\"foreground_outline\":%s,\"search_selection\":%s,\"vector_item_disabled\":%s,\"visible_only_"
            "resolution\":%s,\"narrow_125_clipped\":%s,\"failure_stage\":%u}",
            yes(valid), yes(layers), yes(resident), yes(animated), yes(missing), yes(stale), yes(outline),
            yes(search), yes(disabled), yes(bounded), yes(narrow), failure);
        const HANDLE file =
            CreateFileW(directory, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        const bool saved = file != INVALID_HANDLE_VALUE && bytes > 0 &&
                           WriteFile(file, report, static_cast<DWORD>(bytes), &written, nullptr) &&
                           written == static_cast<DWORD>(bytes);
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        valid = valid && saved;
    }
    log_u64(valid ? LogLevel::info : LogLevel::error, "probe", "media_gallery_native_failure_stage",
            failure);
    log_u64(LogLevel::info, "probe", "media_gallery_visible_source_resolutions", fixture.calls);
    return valid;
}

bool probe_friends_scene() noexcept {
    g_probe.failure_stage = 270;
    UiFriendsScene friends{};
    TextFormScene form{};
    TextFormSurface surface{};
    UiSettingsScene settings{};
    struct RestoreFriendsProbe {
        UiFriendsScene& friends;
        TextFormScene& form;
        TextFormSurface& surface;
        UiSettingsScene& settings;
        const UiFriendsScene* previous_friends;
        TextFormSurface* previous_form;
        const UiSettingsScene* previous_settings;
        u32 width, height, selection_revision, navigation_intent_revision;
        u64 guild, channel, last_private, restore_channel, restore_guild, restore_deadline;
        bool home;
        bool restored = false;
        bool restore() noexcept {
            if (restored)
                return true;
            restored = true;
            renderer_bind_friends_scene(&g_probe.renderer, previous_friends);
            renderer_bind_text_form(&g_probe.renderer, previous_form);
            renderer_bind_settings_scene(&g_probe.renderer, previous_settings);
            g_probe.chat_state.home_selected = home;
            g_probe.chat_state.selected_guild_id = guild;
            g_probe.chat_state.selected_channel_id = channel;
            g_probe.chat_state.selection_revision = selection_revision;
            g_probe.chat_state.navigation_intent_revision = navigation_intent_revision;
            g_probe.chat_state.last_private_channel_id = last_private;
            g_probe.chat_state.restore_channel_id = restore_channel;
            g_probe.chat_state.restore_guild_id = restore_guild;
            g_probe.chat_state.restore_deadline_ms = restore_deadline;
            ui_friends_scene_release(&friends);
            text_form_surface_release(&surface);
            text_form_release(&form);
            ui_settings_scene_release(&settings);
            return renderer_resize(&g_probe.renderer, width, height) &&
                   navigation_view_rebuild(&g_probe.navigation, g_probe.chat_state).ok() &&
                   synchronize_probe_shell_layout() && rebuild_probe_content_text() &&
                   probe_present(&g_probe.renderer, kDefaultTheme);
        }
        ~RestoreFriendsProbe() noexcept {
            if (!restored)
                restore();
        }
    } restore{friends,
              form,
              surface,
              settings,
              g_probe.renderer.friends_scene,
              g_probe.renderer.text_form,
              g_probe.renderer.settings_scene,
              g_probe.renderer.width,
              g_probe.renderer.height,
              g_probe.chat_state.selection_revision,
              g_probe.chat_state.navigation_intent_revision,
              g_probe.chat_state.selected_guild_id,
              g_probe.chat_state.selected_channel_id,
              g_probe.chat_state.last_private_channel_id,
              g_probe.chat_state.restore_channel_id,
              g_probe.chat_state.restore_guild_id,
              g_probe.chat_state.restore_deadline_ms,
              g_probe.chat_state.home_selected};
    if (!chat_state_store_select_home(&g_probe.chat_state).ok())
        return false;
    g_probe.chat_state.selected_channel_id = 0;
    if (!navigation_view_rebuild(&g_probe.navigation, g_probe.chat_state).ok() ||
        !synchronize_probe_shell_layout())
        return false;
    struct Fixture {
        UiFriendsTab tab;
        u32 count, calls;
        bool peer_avatar = false;
    };
    Fixture fixture{UiFriendsTab::online, 2, 0};
    const auto resolve = +[](void* opaque, u32 index, UiFriendsRow* out) noexcept {
        auto& f = *static_cast<Fixture*>(opaque);
        ++f.calls;
        if (index >= f.count)
            return false;
        const auto kind = f.tab == UiFriendsTab::pending
                              ? (index & 1 ? UiFriendsKind::outgoing : UiFriendsKind::incoming)
                          : f.tab == UiFriendsTab::blocked ? UiFriendsKind::blocked
                                                           : UiFriendsKind::friend_user;
        *out = {f.peer_avatar && index == 0 ? 123ull : index + 1ull,
                index + 1 == f.count ? utf8_literal("Last native friend") : utf8_literal("Native friend"),
                utf8_literal("native.friend"),
                {},
                f.tab == UiFriendsTab::online || !(index & 1) ? UiPresence::online : UiPresence::offline,
                kind,
                false};
        return true;
    };
    const auto find = +[](void* opaque, u64 id, u32* index) noexcept {
        auto& f = *static_cast<Fixture*>(opaque);
        if (f.peer_avatar) {
            if (id == 123 && f.count) {
                *index = 0;
                return true;
            }
            return false;
        }
        if (!id || id > f.count)
            return false;
        *index = static_cast<u32>(id) - 1;
        return true;
    };
    UiFriendsSource source{&fixture, fixture.count, 1, resolve, find};
    const auto center = []() noexcept {
        const auto* node = ui_scene_resolve(g_probe.shell_scene,
                                            ui_scene_find(g_probe.shell_scene, UiSemantic::message_view));
        return node ? node->rectangle : UiRect{};
    };
    // Bind an initialized scene before selecting the production Friends shell.
    if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center()))
        return false;
    renderer_bind_friends_scene(&g_probe.renderer, &friends);
    if (!synchronize_probe_shell_layout() || g_probe.shell_scene.view != UiShellView::friends)
        return false;
    const auto pixels = [](const UiFriendsScene& scene, bool rows, bool last) noexcept {
        UiTextCommand commands[kUiFriendsMaximumTextCommands]{};
        UiTextCommandStream text{};
        if (!ui_text_command_stream_initialize(&text, {commands, kUiFriendsMaximumTextCommands}) ||
            !ui_friends_scene_text(scene, kDefaultTheme, &text))
            return false;
        const Utf8View labels[]{utf8_literal("Friends"), utf8_literal("Pending"),
                                last ? utf8_literal("Last native friend") : utf8_literal("Native friend")};
        UiRect rectangles[5]{};
        const u32 count = rows ? 3 : 2;
        for (u32 n = 0; n < count; ++n) {
            bool found = false;
            for (u32 i = 0; i < text.count; ++i)
                if (commands[i].text.bytes == labels[n].bytes &&
                    bytes_equal(commands[i].text.data, labels[n].data, labels[n].bytes)) {
                    rectangles[n] = ui_rect_intersection(commands[i].rectangle, commands[i].clip);
                    found = ui_rect_has_area(rectangles[n]);
                    if (found)
                        break;
                }
            if (!found)
                return false;
        }
        u32 rectangle_count = count;
        bool chat = false;
        for (u32 i = 0; i < text.count; ++i)
            if (commands[i].text.bytes == 4 && bytes_equal(commands[i].text.data, "Chat", 4))
                return false;
        // Friend rows end in round 36-DIP Message/More icon buttons; the
        // Message glyph must reach the target with visible ink.
        for (u32 slot = 0; !chat && slot < ui_friends_scene_visible_count(scene); ++slot) {
            UiFriendsVisibleRow row{};
            if (!ui_friends_scene_visible_row(scene, slot, &row) || row.row.kind != UiFriendsKind::friend_user)
                continue;
            const float y = (row.rectangle.top + row.rectangle.bottom) * .5f;
            for (float x = row.rectangle.right; !chat && x > row.rectangle.left; x -= 2) {
                UiWidgetHandle widget{};
                UiFriendsTarget target{};
                Utf8View tooltip{};
                const auto* node = ui_friends_scene_icon_action(scene, false, x, y, &widget, &target, &tooltip)
                                       ? ui_widget_manager_layout_node(scene.widgets, widget)
                                       : nullptr;
                if (!node || target.action != UiFriendsAction::open_dm)
                    continue;
                const auto r = node->rectangle;
                if (ui_rect_width(r) != 36 || ui_rect_height(r) != 36)
                    return false;
                if (!ui_rect_equal(ui_rect_intersection(r, node->clip_rectangle), r))
                    continue; // a partly scrolled row; probe a fully visible one
                rectangles[rectangle_count++] = {r.left + 8, r.top + 8, r.right - 8, r.bottom - 8};
                chat = true;
            }
        }
        if (rows && (scene.tab == UiFriendsTab::online || scene.tab == UiFriendsTab::all) && !chat)
            return false;
        rectangles[rectangle_count] =
            ui_scene_content_rectangle(g_probe.shell_scene, UiSemantic::channel_header);
        return ui_rect_has_area(rectangles[rectangle_count]) &&
               probe_text_pixels({rectangles, rectangle_count + 1});
    };
    const wchar_t* captures[]{L"friends-online.bmp", L"friends-all.bmp", L"friends-pending.bmp",
                              L"friends-blocked.bmp", L"friends-add-friend.bmp"};
    for (u32 tab = 0; tab < 5; ++tab) {
        fixture.tab = static_cast<UiFriendsTab>(tab);
        fixture.count = tab == 4 ? 0 : tab == 3 ? 1 : tab == 1 ? 4 : 2;
        source.count = fixture.count;
        ++source.revision;
        if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center()))
            return false;
        renderer_bind_friends_scene(&g_probe.renderer, &friends);
        ui_friends_scene_set_focus_visible(&friends, true);
        if (!ui_friends_scene_focus(&friends, {UiFriendsAction::select_tab, 0, fixture.tab}) ||
            g_probe.shell_scene.view != UiShellView::friends)
            return false;
        if (!renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
            !probe_present(&g_probe.renderer, kDefaultTheme) || g_probe.text.message_geometry_quads ||
            !pixels(friends, fixture.count > 0, fixture.count == 1) || !probe_capture_frame(captures[tab]))
            return false;
    }
    // Active Now: the column renders beside the list on a wide content area.
    g_probe.failure_stage = 2741;
    {
        const UiRect wide = center();
        if (ui_rect_width(wide) >= kUiFriendsActiveNowMinimumList + kUiFriendsActiveNowWidth) {
            const UiFriendsActiveCard cards[2]{
                {utf8_literal("Playing Native Quest"), utf8_literal("Native friend and Last native friend"), {}, {1, 2},
                 2, 2, UiFriendsActiveKind::activity},
                {utf8_literal("Lounge"), utf8_literal("Native friend \xC2\xB7 Probe server"), {}, {1}, 1, 1,
                 UiFriendsActiveKind::voice}};
            const UiFriendsActiveNow active{cards, 2, 1, false};
            fixture.tab = UiFriendsTab::online;
            fixture.count = 2;
            source.count = 2;
            ++source.revision;
            if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, wide, nullptr,
                                         nullptr, &active) ||
                !friends.active_visible)
                return false;
            renderer_bind_friends_scene(&g_probe.renderer, &friends);
            const UiRect column = friends.active_rect;
            const UiRect title{column.left + 16, column.top + 12, column.right - 52, column.top + 44};
            const UiRect card{column.left + 30, column.top + 108, column.right - 40, column.top + 150};
            if (!renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
                !probe_present(&g_probe.renderer, kDefaultTheme) || !probe_text_pixels({&title, 1}) ||
                !probe_text_pixels({&card, 1}) || !probe_capture_frame(L"friends-active-now.bmp") ||
                !ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, wide))
                return false;
        }
    }
    g_probe.failure_stage = 271;
    fixture.tab = UiFriendsTab::all;
    fixture.count = 10000;
    source.count = fixture.count;
    ++source.revision;
    fixture.calls = 0;
    if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center()) ||
        friends.count > 32 || fixture.calls > friends.count + 2 ||
        !ui_friends_scene_focus(&friends, {UiFriendsAction::open_dm, 10000, UiFriendsTab::all}) ||
        friends.scroll_dips < 550000 || !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) || !pixels(friends, true, true) ||
        !probe_capture_frame(L"friends-last-row.bmp"))
        return false;
    const auto generation = friends.widgets.generation;
    const auto source_calls = fixture.calls;
    const auto text_builds = g_probe.renderer.content_text_retention.builds;
    const auto paints = g_probe.renderer.paint_stream_builds;
    if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center()) ||
        fixture.calls != source_calls || friends.widgets.generation != generation ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.content_text_retention.builds != text_builds ||
        !probe_present(&g_probe.renderer, kDefaultTheme) || g_probe.renderer.paint_stream_builds != paints)
        return false;
    // Message/composer state is genuinely hidden; sidebar media stays live.
    {
        const auto before = renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme);
        const auto old_updates = g_probe.message_scene.data_updates,
                   old_composer = g_probe.message_composer_scene.observed_revision;
        const auto old_hover = g_probe.message_scene.widgets.hover_started_ms;
        ++g_probe.message_scene.data_updates;
        ++g_probe.message_scene.widgets.hover_started_ms;
        ++g_probe.message_composer_scene.observed_revision;
        const bool retained =
            renderer_presentation_stamp_equal(
                before, renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme)) &&
            renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            g_probe.renderer.content_text_retention.builds == text_builds &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            g_probe.renderer.paint_stream_builds == paints;
        g_probe.message_scene.data_updates = old_updates;
        g_probe.message_scene.widgets.hover_started_ms = old_hover;
        g_probe.message_composer_scene.observed_revision = old_composer;
        if (!retained)
            return false;
    }
    // A real, focused native composer overlaps Friends but is hidden. Verify
    // both retained dependency checks and a forced fresh paint omit its outline.
    {
        g_probe.failure_stage = 275;
        MessageComposer hidden_model{};
        MessageComposerScene hidden_scene{};
        char hidden_text[64]{};
        struct RestoreHiddenFocus {
            MessageComposerScene& scene;
            const MessageComposerScene* previous_composer;
            const UiWidgetManager* previous_focus;
            ~RestoreHiddenFocus() noexcept {
                g_probe.renderer.message_composer_scene = previous_composer;
                g_probe.renderer.focus_owner = previous_focus;
                message_composer_scene_release(&scene);
            }
        } restore_hidden{hidden_scene, g_probe.renderer.message_composer_scene,
                         g_probe.renderer.focus_owner};
        if (!message_composer_initialize(&hidden_model, {hidden_text, 64}) ||
            !message_composer_scene_initialize_owned(&hidden_scene))
            return false;
        message_composer_reset(&hidden_model, 42, false);
        const auto area = center();
        const UiRect hidden_view{area.left + 20, area.top + 120, area.left + 300, area.top + 168};
        if (!message_composer_scene_update(&hidden_scene, &hidden_model, hidden_view, true).ok() ||
            !message_composer_scene_focus_edit(&hidden_scene))
            return false;
        UiRect anchor{};
        if (!ui_widget_manager_context_anchor(hidden_scene.widgets, &anchor) || !ui_rect_has_area(anchor))
            return false;
        g_probe.renderer.message_composer_scene = &hidden_scene;
        g_probe.renderer.focus_owner = nullptr;
        if (!probe_present(&g_probe.renderer, kDefaultTheme))
            return false;
        const auto before = renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme);
        const auto geometry = probe_resize_geometry_signature();
        const auto baseline_paints = g_probe.renderer.paint_stream_builds;
        const UiWidgetManager* owners[]{
            &g_probe.message_scene.widgets, &hidden_scene.widgets,
            g_probe.renderer.member_list_scene ? &g_probe.renderer.member_list_scene->widgets : nullptr};
        for (const auto* owner : owners) {
            if (!owner)
                continue;
            g_probe.renderer.focus_owner = owner;
            if (!renderer_presentation_stamp_equal(
                    before, renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme)) ||
                !probe_present(&g_probe.renderer, kDefaultTheme) ||
                g_probe.renderer.paint_stream_builds != baseline_paints)
                return false;
        }
        g_probe.renderer.focus_owner = &hidden_scene.widgets;
        hidden_scene.widgets.hovered = hidden_scene.widgets.focused;
        if (!renderer_presentation_stamp_equal(
                before, renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme)) ||
            !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
            g_probe.renderer.content_text_retention.builds != text_builds)
            return false;
        g_probe.renderer.retained_presentation.valid = false;
        if (!probe_present(&g_probe.renderer, kDefaultTheme) ||
            g_probe.renderer.paint_stream_builds != baseline_paints + 1 ||
            probe_resize_geometry_signature() != geometry || !pixels(friends, true, true) ||
            !probe_capture_frame(L"friends-hidden-focus.bmp"))
            return false;
        // A visible Friends owner remains a dependency, unlike the hidden owners.
        g_probe.renderer.focus_owner = &friends.widgets;
        if (renderer_presentation_stamp_equal(
                before, renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme)))
            return false;
    }
    g_probe.failure_stage = 272;
    const float scale = static_cast<float>(g_probe.renderer.dpi) / 96.f;
    if (!renderer_resize(&g_probe.renderer, static_cast<u32>((center().left + 320) * scale + .999f),
                         static_cast<u32>(360 * scale + .999f)) ||
        !synchronize_probe_shell_layout() ||
        !ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center()) ||
        !ui_friends_scene_focus(&friends, {UiFriendsAction::open_dm, 10000, UiFriendsTab::all}) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) || !pixels(friends, true, true) ||
        !probe_capture_frame(L"friends-compact.bmp"))
        return false;
    // Restore a roomy center for an actual native editor above the base list.
    if (!renderer_resize(&g_probe.renderer, restore.width, restore.height) ||
        !synchronize_probe_shell_layout() ||
        !ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center()))
        return false;
    g_probe.failure_stage = 273;
    const float dip = 96.f / static_cast<float>(g_probe.renderer.dpi);
    const UiRect viewport{0, 0, g_probe.renderer.width * dip, g_probe.renderer.height * dip};
    const TextFormContent content{utf8_literal("Friend overlay"),
                                  {},
                                  utf8_literal("Exact username"),
                                  {},
                                  utf8_literal("Cancel"),
                                  utf8_literal("Send request"),
                                  utf8_literal("native.friend"),
                                  0,
                                  13,
                                  true,
                                  true};
    if (!text_form_update(&form, content, viewport, &g_probe.text_measure_cache))
        return false;
    surface.scene = &form;
    renderer_bind_text_form(&g_probe.renderer, &surface);
    if (!renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) || !surface.layout.active ||
        g_probe.renderer.form_text_first_batch < g_probe.renderer.overlay_text_first_batch ||
        g_probe.renderer.form_text_first_batch >= g_probe.text.batch_count ||
        !probe_present(&g_probe.renderer, kDefaultTheme))
        return false;
    UiTextCommand form_commands[32]{};
    UiTextCommandStream form_text{};
    UiRect title{};
    if (!ui_text_command_stream_initialize(&form_text, {form_commands, 32}) ||
        !text_form_text(form, surface.geometry, kDefaultTheme, &form_text))
        return false;
    for (u32 i = 0; i < form_text.count; ++i)
        if (form_commands[i].text.bytes == content.title.bytes &&
            bytes_equal(form_commands[i].text.data, content.title.data, content.title.bytes))
            title = ui_rect_intersection(form_commands[i].rectangle, form_commands[i].clip);
    if (!ui_rect_has_area(title) || !probe_text_pixels({&title, 1}) ||
        !probe_capture_frame(L"friends-native-form.bmp"))
        return false;
    renderer_bind_text_form(&g_probe.renderer, restore.previous_form);
    text_form_surface_release(&surface);
    text_form_release(&form);
    g_probe.failure_stage = 274;
    UiPageRegistry registry{};
    UiPage pages[1]{};
    UiSettingsControl control{1,   utf8_literal("Visible setting"), {}, UiSettingsControlKind::toggle, true,
                              true};
    UiSettingsPageContent settings_content{
        utf8_literal("Friends covered by Settings"), {}, {&control, 1}, 1};
    if (!ui_page_registry_initialize(&registry, {pages, 1}) ||
        !ui_page_registry_register(&registry, {{1, 1}, utf8_literal("Appearance"), &settings_content})
             .ok() ||
        !ui_settings_scene_update(&settings, &registry, viewport))
        return false;
    renderer_bind_settings_scene(&g_probe.renderer, &settings);
    if (!renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) || g_probe.renderer.media_quad_count ||
        g_probe.text.message_geometry_quads)
        return false;
    const auto settings_stamp = renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme);
    const auto settings_text = g_probe.renderer.content_text_retention.builds,
               settings_paint = g_probe.renderer.paint_stream_builds;
    const auto old_friends_revision = friends.revision, old_source_revision = source.revision;
    ++friends.revision;
    ++source.revision;
    const bool covered =
        renderer_presentation_stamp_equal(
            settings_stamp, renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme)) &&
        renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
        g_probe.renderer.content_text_retention.builds == settings_text &&
        probe_present(&g_probe.renderer, kDefaultTheme) &&
        g_probe.renderer.paint_stream_builds == settings_paint;
    friends.revision = old_friends_revision;
    source.revision = old_source_revision;
    if (!covered)
        return false;
    UiTextCommand settings_commands[32]{};
    UiTextCommandStream settings_text_stream{};
    UiRect settings_title{};
    if (!ui_text_command_stream_initialize(&settings_text_stream, {settings_commands, 32}) ||
        !ui_settings_scene_text(settings, kDefaultTheme, &settings_text_stream))
        return false;
    for (u32 i = 0; i < settings_text_stream.count; ++i)
        if (settings_commands[i].text.data == settings_content.title.data)
            settings_title =
                ui_rect_intersection(settings_commands[i].rectangle, settings_commands[i].clip);
    if (!ui_rect_has_area(settings_title) || !probe_text_pixels({&settings_title, 1}) ||
        !probe_capture_frame(L"friends-settings-cover.bmp"))
        return false;
    renderer_bind_settings_scene(&g_probe.renderer, restore.previous_settings);
    ui_settings_scene_release(&settings);
    // Six bounded additions use immutable borrowed projections for each
    // synchronous capture. No controller, credentials, CDN or dialog is used.
    g_probe.failure_stage = 276;
    fixture.tab = UiFriendsTab::all;
    fixture.count = 4;
    source.count = 4;
    ++source.revision;
    friends.scroll_dips = 0;
    const auto unicode_query = utf8_literal("\xC5\xBD"
                                            "iva \xF0\x9F\x98\x80");
    UiFriendsContent search_content{{unicode_query, 0, unicode_query.bytes, true, true, true}, 4, 1};
    if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center(),
                                 &search_content) ||
        !ui_friends_scene_focus(&friends, {UiFriendsAction::search, 0, fixture.tab}) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !g_probe.renderer.composer_layout.active || g_probe.renderer.composer_layout.caret_offset != 7 ||
        !g_probe.renderer.friends_search_geometry.selection_count ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_capture_frame(L"friends-search-unicode-selected.bmp"))
        return false;
    const auto search_stamp = renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme);
    search_content.search.caret_visible = false;
    ++search_content.revision;
    if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center(),
                                 &search_content) ||
        renderer_presentation_stamp_equal(
            search_stamp, renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme)))
        return false;
    const auto long_query = utf8_literal("native.search.with.a.long.single.line.query.native.search.with.a."
                                         "long.single.line.query.\xC5\xBD\xF0\x9F\x98\x80");
    search_content.search = {long_query, long_query.bytes, long_query.bytes, true, true, true};
    ++search_content.revision;
    if (!renderer_resize(&g_probe.renderer, static_cast<u32>((center().left + 320) * scale + .999f),
                         static_cast<u32>(360 * scale + .999f)) ||
        !synchronize_probe_shell_layout() ||
        !ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center(),
                                 &search_content) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.composer_layout.scroll_x <= 0 ||
        !ui_rect_has_area(ui_rect_intersection(g_probe.renderer.friends_search_geometry.caret,
                                               ui_friends_scene_search_geometry(friends).text)) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_capture_frame(L"friends-search-compact-scroll.bmp"))
        return false;
    if (!renderer_resize(&g_probe.renderer, restore.width, restore.height) ||
        !synchronize_probe_shell_layout())
        return false;
    fixture.count = 0;
    source.count = 0;
    ++source.revision;
    search_content.total_count = 4;
    search_content.search = {utf8_literal("no.match.\xF0\x9F\x98\x80"), 0, 13, true, false, false};
    ++search_content.revision;
    if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center(),
                                 &search_content) ||
        friends.count_heading_bytes != 5 || !bytes_equal(friends.count_heading, "0 / 4", 5) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_capture_frame(L"friends-search-no-results.bmp"))
        return false;
    fixture.count = 4;
    source.count = 4;
    ++source.revision;
    search_content.search = {{}, 0, 0, true, false, false};
    ++search_content.revision;
    if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center(),
                                 &search_content))
        return false;
    {
        PopupScene menu{};
        PopupItem storage[3]{};
        PopupRowWidget rows[3]{};
        UiWidget widgets[16]{};
        UiLayoutNode nodes[16]{};
        const auto* previous_menu = g_probe.renderer.popup_scene;
        struct RestoreMenu {
            PopupScene& menu;
            const PopupScene* previous;
            ~RestoreMenu() noexcept {
                renderer_bind_popup_scene(&g_probe.renderer, previous);
                popup_scene_release(&menu);
            }
        } restore_menu{menu, previous_menu};
        const PopupItem items[]{{utf8_literal("Profile"), 1, popup_item_enabled},
                                {utf8_literal("Remove friend"), 2,
                                 popup_item_enabled | popup_item_danger | popup_item_separator_before},
                                {utf8_literal("Block"), 3, popup_item_enabled | popup_item_danger}};
        UiFriendsVisibleRow row{};
        if (!ui_friends_scene_visible_row(friends, 0, &row) ||
            !ui_friends_scene_focus(&friends, {UiFriendsAction::more, row.row.user_id, fixture.tab}) ||
            !popup_scene_initialize(&menu, {storage, 3}, {rows, 3}, {widgets, 16}, {nodes, 16}) ||
            !popup_scene_open(
                 &menu, PopupKind::context_menu, row.row.user_id, {items, 3},
                 {row.rectangle.right - 32, row.rectangle.top, row.rectangle.right, row.rectangle.bottom},
                 viewport, 240)
                 .ok() ||
            !renderer_bind_popup_scene(&g_probe.renderer, &menu) ||
            !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
            !probe_present(&g_probe.renderer, kDefaultTheme) ||
            !probe_capture_frame(L"friends-more-menu.bmp"))
            return false;
    }
    TextFormContent confirmation{};
    confirmation.title = utf8_literal("Remove friend");
    confirmation.context = utf8_literal(
        "Remove \xC5\xBD"
        "iva \xF0\x9F\x98\x80 from your friends? This native confirmation keeps the full account name "
        "visible before an action. A long name and explanatory context wrap inside the bounded modal "
        "instead of creating an editor or clipping the choice buttons.");
    confirmation.cancel = utf8_literal("Cancel");
    confirmation.accept = utf8_literal("Remove friend");
    confirmation.can_accept = true;
    confirmation.focus_visible = true;
    confirmation.confirmation_only = true;
    if (!text_form_update(&form, confirmation, viewport, &g_probe.text_measure_cache) ||
        !text_form_focus(&form, TextFormElement::cancel))
        return false;
    surface.scene = &form;
    renderer_bind_text_form(&g_probe.renderer, &surface);
    if (!renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) || surface.layout.active ||
        text_form_focused(form) != TextFormElement::cancel ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_capture_frame(L"friends-confirmation-cancel.bmp"))
        return false;
    renderer_bind_text_form(&g_probe.renderer, restore.previous_form);
    text_form_surface_release(&surface);
    text_form_release(&form);
    fixture.peer_avatar = true;
    fixture.count = 1;
    source.count = 1;
    ++source.revision;
    friends.scroll_dips = 0;
    if (!ui_friends_scene_update(&friends, &source, fixture.tab, UiFriendsPhase::ready, {}, center()) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme))
        return false;
    FriendsAvatarMedia avatar{};
    bool avatar_quad = false;
    if (!friends_avatar_media_resolve(friends, g_probe.users, 0, &avatar) ||
        !media_atlas_peek(&g_probe.media_atlas, avatar.key).ok())
        return false;
    for (u32 i = 0; i < g_probe.renderer.media_quad_count; ++i) {
        const auto& q = g_probe.renderer.media_draw.quad_data[i];
        if (q.left == avatar.rectangle.left && q.top == avatar.rectangle.top &&
            q.right == avatar.rectangle.right && q.bottom == avatar.rectangle.bottom && q.radius == 16)
            avatar_quad = true;
    }
    if (!avatar_quad || !probe_capture_frame(L"friends-native-avatar-presence.bmp"))
        return false;
    if (!restore.restore())
        return false;
    log_write(LogLevel::info, "probe",
              "friends_tabs_virtualization_retention_compact_native_form_hardware_passed");
    return true;
}

bool probe_friends_requests_scene() noexcept {
    g_probe.failure_stage = 277;
    UiFriendsScene main{}, requests{};
    TextFormScene form{};
    TextFormSurface form_surface{};
    ProfileScene profile{};
    struct Fixture {
        u32 count, calls;
        bool hidden;
        char received[32] = "Received native request", sent[32] = "Sent native request";
        char ignored[32] = "Ignored native request", spam[32] = "Spam native request",
             username[32] = "native.request";
    } main_fixture{8, 0, false}, hidden_fixture{4, 0, true};
    const auto resolve = +[](void* opaque, u32 index, UiFriendsRow* out) noexcept {
        auto& f = *static_cast<Fixture*>(opaque);
        ++f.calls;
        if (!out || index >= f.count)
            return false;
        const bool outgoing = !f.hidden && index >= f.count / 2, ignored = f.hidden && index < 2;
        const char* name = f.hidden ? (ignored ? f.ignored : f.spam) : (outgoing ? f.sent : f.received);
        u32 bytes = 0;
        while (name[bytes])
            ++bytes;
        *out = {f.hidden && index == 0 ? 123ull : (f.hidden ? 50000ull : 40000ull) + index,
                {name, bytes},
                {f.username, 14},
                {},
                UiPresence::online,
                outgoing ? UiFriendsKind::outgoing : UiFriendsKind::incoming,
                false,
                f.hidden && !ignored,
                ignored};
        return true;
    };
    const auto find = +[](void* opaque, u64 id, u32* index) noexcept {
        const auto& f = *static_cast<Fixture*>(opaque);
        if (!index)
            return false;
        if (f.hidden && id == 123 && f.count) {
            *index = 0;
            return true;
        }
        const u64 first = f.hidden ? 50000ull : 40000ull;
        if (id < first || id - first >= f.count || (f.hidden && id == first))
            return false;
        *index = static_cast<u32>(id - first);
        return true;
    };
    UiFriendsSource main_source{&main_fixture, 8, 1, resolve, find},
        hidden_source{&hidden_fixture, 4, 1, resolve, find};
    UiFriendsSection ranges[]{{UiFriendsSectionKind::incoming, 0, 4, 4, false, true},
                              {UiFriendsSectionKind::outgoing, 4, 4, 4, false, false}};
    UiFriendsSections sections{ranges, 2, 1, true, false, 4}, hidden_sections{nullptr, 0, 1, true, true, 4};
    struct Restore {
        UiFriendsScene& main;
        UiFriendsScene& requests;
        TextFormScene& form;
        TextFormSurface& surface;
        ProfileScene& profile;
        const UiFriendsScene* old_main;
        const UiFriendsScene* old_requests;
        TextFormSurface* old_form;
        ProfileScene* old_profile;
        const UiWidgetManager* old_focus;
        u32 width, height, selection, navigation;
        u64 guild, channel, last_private, restore_channel, restore_guild, deadline;
        bool home;
        bool restored = false;
        bool finish() noexcept {
            if (restored)
                return true;
            restored = true;
            renderer_bind_friends_requests_scene(&g_probe.renderer, old_requests);
            renderer_bind_friends_scene(&g_probe.renderer, old_main);
            renderer_bind_text_form(&g_probe.renderer, old_form);
            g_probe.renderer.profile_scene = old_profile;
            g_probe.renderer.focus_owner = old_focus;
            g_probe.chat_state.home_selected = home;
            g_probe.chat_state.selected_guild_id = guild;
            g_probe.chat_state.selected_channel_id = channel;
            g_probe.chat_state.selection_revision = selection;
            g_probe.chat_state.navigation_intent_revision = navigation;
            g_probe.chat_state.last_private_channel_id = last_private;
            g_probe.chat_state.restore_channel_id = restore_channel;
            g_probe.chat_state.restore_guild_id = restore_guild;
            g_probe.chat_state.restore_deadline_ms = deadline;
            ui_friends_scene_release(&requests);
            ui_friends_scene_release(&main);
            text_form_surface_release(&surface);
            text_form_release(&form);
            profile_scene_release(&profile);
            return renderer_resize(&g_probe.renderer, width, height) &&
                   navigation_view_rebuild(&g_probe.navigation, g_probe.chat_state).ok() &&
                   synchronize_probe_shell_layout() && rebuild_probe_content_text() &&
                   probe_present(&g_probe.renderer, kDefaultTheme);
        }
        ~Restore() noexcept {
            if (!restored)
                finish();
        }
    } restore{main,
              requests,
              form,
              form_surface,
              profile,
              g_probe.renderer.friends_scene,
              g_probe.renderer.friends_requests_scene,
              g_probe.renderer.text_form,
              g_probe.renderer.profile_scene,
              g_probe.renderer.focus_owner,
              g_probe.renderer.width,
              g_probe.renderer.height,
              g_probe.chat_state.selection_revision,
              g_probe.chat_state.navigation_intent_revision,
              g_probe.chat_state.selected_guild_id,
              g_probe.chat_state.selected_channel_id,
              g_probe.chat_state.last_private_channel_id,
              g_probe.chat_state.restore_channel_id,
              g_probe.chat_state.restore_guild_id,
              g_probe.chat_state.restore_deadline_ms,
              g_probe.chat_state.home_selected};
    const float normal_scale = g_probe.renderer.dpi / 96.f;
    if (!renderer_resize(&g_probe.renderer, static_cast<u32>(1200 * normal_scale + .999f),
                         static_cast<u32>(970 * normal_scale + .999f)))
        return false;
    if (!chat_state_store_select_home(&g_probe.chat_state).ok())
        return false;
    g_probe.chat_state.selected_channel_id = 0;
    if (!navigation_view_rebuild(&g_probe.navigation, g_probe.chat_state).ok() ||
        !synchronize_probe_shell_layout())
        return false;
    const auto center = []() noexcept {
        const auto* node = ui_scene_resolve(g_probe.shell_scene,
                                            ui_scene_find(g_probe.shell_scene, UiSemantic::message_view));
        return node ? node->rectangle : UiRect{};
    };
    const auto viewport = []() noexcept {
        const float dip = 96.f / g_probe.renderer.dpi;
        return UiRect{0, 0, g_probe.renderer.width * dip, g_probe.renderer.height * dip};
    };
    const auto update_main = [&]() noexcept {
        return ui_friends_scene_update(&main, &main_source, UiFriendsTab::pending, UiFriendsPhase::ready,
                                       {}, center(), nullptr, &sections);
    };
    const auto text_rect = [](const UiFriendsScene& scene, Utf8View label, bool prefix = false) noexcept {
        UiTextCommand commands[128]{};
        UiTextCommandStream stream{};
        if (!ui_text_command_stream_initialize(&stream, {commands, 128}) ||
            !ui_friends_scene_text(scene, kDefaultTheme, &stream))
            return UiRect{};
        for (u32 i = 0; i < stream.count; ++i)
            if ((prefix ? commands[i].text.bytes >= label.bytes : commands[i].text.bytes == label.bytes) &&
                bytes_equal(commands[i].text.data, label.data, label.bytes)) {
                const auto rect = ui_rect_intersection(commands[i].rectangle, commands[i].clip);
                if (ui_rect_has_area(rect))
                    return rect;
            }
        return UiRect{};
    };
    const auto paint = [&]() noexcept {
        return renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
               probe_present(&g_probe.renderer, kDefaultTheme) &&
               g_probe.renderer.paint_stream.count <= kMaxPaintQuads &&
               g_probe.renderer.paint_stream.clip_count <= kMaxPaintClips &&
               !g_probe.renderer.paint_stream.rejected && !g_probe.text.message_geometry_quads &&
               d3d11_renderer_interaction_text_state_current(g_probe.renderer);
    };
    if (!update_main())
        return false;
    renderer_bind_friends_scene(&g_probe.renderer, &main);
    if (!synchronize_probe_shell_layout() || !update_main() || !paint())
        return false;
    UiRect headings[]{text_rect(main, utf8_literal("Received"), true),
                      text_rect(main, utf8_literal("Sent"), true)};
    if (!probe_text_pixels({headings, 2}) || !probe_capture_frame(L"friends-pending-sections.bmp"))
        return false;
    g_probe.failure_stage = 279;
    ranges[0].collapsed = true;
    ++sections.revision;
    if (!update_main() || ui_friends_scene_visible_count(main) != 4 || !paint() ||
        !probe_capture_frame(L"friends-pending-collapse.bmp"))
        return false;
    // Page a real bounded Pending projection, preserving O(1) source lookup.
    main_fixture.count = 4096;
    main_source.count = 4096;
    ++main_source.revision;
    ranges[0] = {UiFriendsSectionKind::incoming, 0, 2048, 2048, false, true};
    ranges[1] = {UiFriendsSectionKind::outgoing, 2048, 2048, 2048, false, false};
    ++sections.revision;
    if (!update_main() ||
        !ui_friends_scene_focus(&main, {UiFriendsAction::cancel, 44095, UiFriendsTab::pending}) ||
        main.scroll_dips < 200000 || ui_friends_scene_page(&main, true).action == UiFriendsAction::none)
        return false;
    // Stress both actual scene paint producers at the original shared bounds.
    PaintStream budget{};
    struct ReleasePaint {
        PaintStream& value;
        ~ReleasePaint() noexcept {
            paint_stream_release(&value);
        }
    } release_paint{budget};
    const UiRect tall{0, 0, 1600, 8192};
    main.scroll_dips = 0;
    if (!paint_stream_initialize(&budget) ||
        !ui_friends_scene_update(&main, &main_source, UiFriendsTab::pending, UiFriendsPhase::ready, {},
                                 tall, nullptr, &sections) ||
        !ui_friends_scene_update(&requests, &hidden_source, UiFriendsTab::pending, UiFriendsPhase::ready,
                                 {}, tall, nullptr, &hidden_sections) ||
        ui_friends_scene_visible_count(main) < 140 || !paint_stream_reset(&budget) ||
        !ui_friends_scene_paint(main, kDefaultTheme, &budget) ||
        !ui_friends_scene_paint(requests, kDefaultTheme, &budget) || budget.count > kMaxPaintQuads ||
        budget.clip_count > kMaxPaintClips || budget.rejected)
        return false;
    main_fixture.count = main_source.count = 8;
    ++main_source.revision;
    ranges[0] = {UiFriendsSectionKind::incoming, 0, 4, 4, false, true};
    ranges[1] = {UiFriendsSectionKind::outgoing, 4, 4, 4, false, false};
    ++sections.revision;
    main.scroll_dips = requests.scroll_dips = 0;
    if (!update_main() ||
        !ui_friends_scene_update(&requests, &hidden_source, UiFriendsTab::pending, UiFriendsPhase::ready,
                                 {}, viewport(), nullptr, &hidden_sections))
        return false;
    renderer_bind_friends_requests_scene(&g_probe.renderer, &requests);
    g_probe.failure_stage = 280;
    ui_friends_scene_set_focus_visible(&requests, true);
    if (!ui_friends_scene_focus(&requests, {UiFriendsAction::accept, 123, UiFriendsTab::pending}))
        return false;
    g_probe.renderer.focus_owner = &requests.widgets;
    if (!paint() || g_probe.renderer.composer_layout.active ||
        g_probe.renderer.friends_search_geometry.selection_count)
        return false;
    const auto& renderer = g_probe.renderer;
    if (renderer.overlay_paint_first_batch >= renderer.friends_requests_foreground_first_batch ||
        renderer.friends_requests_foreground_first_batch >= renderer.friends_requests_paint_end_batch ||
        renderer.friends_requests_media_first_batch >= renderer.friends_requests_media_end_batch ||
        renderer.friends_requests_media_end_batch > renderer.popup_media_first_batch ||
        renderer.overlay_text_first_batch >= renderer.friends_requests_text_end_batch ||
        renderer.friends_requests_text_end_batch > renderer.menu_text_first_batch)
        return false;
    UiFriendsVisibleRow row{};
    FriendsAvatarMedia avatar{};
    if (!ui_friends_scene_visible_row(requests, 0, &row) || !row.row.ignored || row.row.spam ||
        !friends_avatar_media_resolve(requests, g_probe.users, 0, &avatar) ||
        !media_atlas_peek(&g_probe.media_atlas, avatar.key).ok())
        return false;
    UiFriendsVisibleRow spam{};
    if (!ui_friends_scene_visible_row(requests, 2, &spam) || !spam.row.spam || spam.row.ignored)
        return false;
    const Color dark{kDefaultTheme.base_lowest.red * .35f, kDefaultTheme.base_lowest.green * .35f,
                     kDefaultTheme.base_lowest.blue * .35f, 1};
    const ProbePixelSample samples[]{
        {{center().right - 4, center().bottom - 4}, dark, 3},
        {{requests.surface.left + 5, requests.surface.top + 5}, kDefaultTheme.base_lowest, 3},
        {{(row.avatar.left + row.avatar.right) * .5f, (row.avatar.top + row.avatar.bottom) * .5f},
         {120.f / 255, 80.f / 255, 180.f / 255, 1},
         3},
        {{(row.presence.left + row.presence.right) * .5f, (row.presence.top + row.presence.bottom) * .5f},
         kDefaultTheme.tokens.status.online,
         3}};
    UiRect modal_title = text_rect(requests, utf8_literal("Other requests"));
    g_probe.failure_stage = 281;
    if (!probe_exact_pixels({samples, 4}) || !probe_text_pixels({&modal_title, 1}) ||
        !probe_capture_frame(L"friends-requests-modal.bmp"))
        return false;
    const u32 calls = hidden_fixture.calls, main_calls = main_fixture.calls,
              builds = renderer.content_text_retention.builds, paints = renderer.paint_stream_builds;
    g_probe.failure_stage = 282;
    if (!paint() || hidden_fixture.calls != calls || main_fixture.calls != main_calls ||
        renderer.content_text_retention.builds != builds || renderer.paint_stream_builds != paints)
        return false;
    const auto stamp = renderer_capture_presentation_stamp(renderer, kDefaultTheme);
    g_probe.renderer.focus_owner = &main.widgets;
    if (d3d11_renderer_visible_focus_owner(renderer) ||
        renderer_presentation_stamp_equal(stamp,
                                          renderer_capture_presentation_stamp(renderer, kDefaultTheme)))
        return false;
    const auto hidden_stamp = renderer_capture_presentation_stamp(renderer, kDefaultTheme);
    g_probe.renderer.focus_owner = &g_probe.message_scene.widgets;
    if (!renderer_presentation_stamp_equal(hidden_stamp,
                                           renderer_capture_presentation_stamp(renderer, kDefaultTheme)))
        return false;
    g_probe.renderer.focus_owner = &requests.widgets;
    ++requests.revision;
    if (d3d11_renderer_interaction_text_state_current(renderer) ||
        renderer_presentation_stamp_equal(stamp,
                                          renderer_capture_presentation_stamp(renderer, kDefaultTheme)))
        return false;
    if (!paint())
        return false;
    g_probe.failure_stage = 278;
    // A real native profile covers Requests; no seventh capture is needed.
    UserProfile profile_data{};
    profile_data.user_id = 123;
    copy_bytes(profile_data.display_name.data, "Requests profile", 16);
    profile_data.display_name.bytes = 16;
    profile.phase = ProfilePhase::ready;
    profile.measure = &g_probe.text_measure_cache;
    if (!profile_scene_open(&profile, &profile_data, &g_probe.chat_state, &g_probe.users, 1, row.avatar,
                            viewport()))
        return false;
    g_probe.renderer.profile_scene = &profile;
    g_probe.renderer.focus_owner = &profile.widgets;
    if (!paint() ||
        g_probe.renderer.popup_media_first_batch < g_probe.renderer.friends_requests_media_end_batch ||
        g_probe.renderer.profile_foreground_first_batch <
            g_probe.renderer.friends_requests_paint_end_batch ||
        d3d11_renderer_visible_focus_owner(renderer) != &profile.widgets)
        return false;
    UiTextCommand profile_commands[128]{};
    UiTextCommandStream profile_text{};
    UiRect profile_name{};
    if (!ui_text_command_stream_initialize(&profile_text, {profile_commands, 128}) ||
        !profile_scene_text(profile, kDefaultTheme, &profile_text))
        return false;
    for (u32 i = 0; i < profile_text.count; ++i)
        if (utf8_equal(profile_commands[i].text, profile_data.display_name.view()))
            profile_name = ui_rect_intersection(profile_commands[i].rectangle, profile_commands[i].clip);
    if (!ui_rect_has_area(profile_name) || !probe_text_pixels({&profile_name, 1}))
        return false;
    g_probe.renderer.profile_scene = restore.old_profile;
    profile_scene_release(&profile);
    {
        PopupScene menu{};
        PopupItem storage[2]{};
        PopupRowWidget menu_rows[2]{};
        UiWidget widgets[12]{};
        UiLayoutNode nodes[12]{};
        struct RestoreMenu {
            PopupScene& scene;
            const PopupScene* previous;
            ~RestoreMenu() noexcept {
                renderer_bind_popup_scene(&g_probe.renderer, previous);
                popup_scene_release(&scene);
            }
        } menu_restore{menu, g_probe.renderer.popup_scene};
        const PopupItem items[]{{utf8_literal("Profile"), 1, popup_item_enabled},
                                {utf8_literal("Block"), 2, popup_item_enabled | popup_item_danger}};
        if (!popup_scene_initialize(&menu, {storage, 2}, {menu_rows, 2}, {widgets, 12}, {nodes, 12}) ||
            !popup_scene_open(&menu, PopupKind::context_menu, 123, {items, 2}, row.rectangle, viewport(),
                              200)
                 .ok() ||
            !renderer_bind_popup_scene(&g_probe.renderer, &menu))
            return false;
        g_probe.renderer.focus_owner = &menu.widgets;
        if (!paint() ||
            g_probe.renderer.menu_paint_first_batch < g_probe.renderer.friends_requests_paint_end_batch ||
            d3d11_renderer_visible_focus_owner(renderer) != &menu.widgets ||
            !probe_capture_frame(L"friends-requests-menu.bmp"))
            return false;
    }
    TextFormContent confirmation{};
    confirmation.title = utf8_literal("Ignore user");
    g_probe.failure_stage = 283;
    confirmation.context = utf8_literal("Ignore this person? Their friend requests will appear in Other "
                                        "requests, and existing friendships are preserved.");
    confirmation.cancel = utf8_literal("Cancel");
    confirmation.accept = utf8_literal("Ignore");
    confirmation.can_accept = true;
    confirmation.focus_visible = true;
    confirmation.confirmation_only = true;
    if (!text_form_update(&form, confirmation, viewport(), &g_probe.text_measure_cache) ||
        !text_form_focus(&form, TextFormElement::cancel))
        return false;
    form_surface.scene = &form;
    renderer_bind_text_form(&g_probe.renderer, &form_surface);
    g_probe.renderer.focus_owner = &form.widgets;
    if (!paint() || form_surface.layout.active ||
        d3d11_renderer_visible_focus_owner(renderer) != &form.widgets ||
        g_probe.renderer.form_paint_first_batch < g_probe.renderer.friends_requests_paint_end_batch ||
        !probe_capture_frame(L"friends-requests-confirmation.bmp"))
        return false;
    const auto* form_title = text_form_node(form, TextFormElement::title);
    const UiRect form_title_rect =
        form_title ? ui_rect_intersection(form_title->content_rectangle, form_title->clip_rectangle)
                   : UiRect{};
    if (!ui_rect_has_area(form_title_rect) || !probe_text_pixels({&form_title_rect, 1}))
        return false;
    renderer_bind_text_form(&g_probe.renderer, restore.old_form);
    text_form_surface_release(&form_surface);
    text_form_release(&form);
    g_probe.renderer.focus_owner = &requests.widgets;
    const float scale = renderer.dpi / 96.f;
    g_probe.failure_stage = 284;
    // Preserve the same 320-DIP center-pane minimum used by the native
    // window/scene contract. The former 540-DIP host left only 228 DIPs after
    // the sidebar and failed before a modal could be rendered.
    const float compact_width = maximum(640.f, center().left + 320.f);
    const auto compact_check = [](bool success, const char* label) noexcept {
        if (!success)
            log_write(LogLevel::error, "probe", label);
        return success;
    };
    if (!compact_check(renderer_resize(&g_probe.renderer, static_cast<u32>(compact_width * scale + .999f),
                                       static_cast<u32>(360 * scale + .999f)),
                       "requests_compact_resize_failed"))
        return false;
    if (!compact_check(synchronize_probe_shell_layout(), "requests_compact_shell_layout_failed"))
        return false;
    if (!compact_check(update_main(), "requests_compact_main_scene_failed")) {
        log_u64(LogLevel::error, "probe", "requests_compact_main_width",
                static_cast<u64>(ui_rect_width(center())));
        log_u64(LogLevel::error, "probe", "requests_compact_main_height",
                static_cast<u64>(ui_rect_height(center())));
        return false;
    }
    if (!compact_check(ui_friends_scene_update(&requests, &hidden_source, UiFriendsTab::pending,
                                               UiFriendsPhase::ready, {}, viewport(), nullptr,
                                               &hidden_sections),
                       "requests_compact_modal_scene_failed"))
        return false;
    if (!compact_check(ui_rect_width(requests.surface) <= 480 && ui_rect_height(requests.surface) <= 328,
                       "requests_compact_card_bounds_failed"))
        return false;
    const auto last_fully_visible = [&]() noexcept {
        UiFriendsVisibleRow last{};
        const u32 count = ui_friends_scene_visible_count(requests);
        for (u32 i = 0; i < count; ++i)
            if (ui_friends_scene_visible_row(requests, i, &last) && last.row.user_id == 50003)
                return ui_rect_equal(last.rectangle, last.clip) && last.row.spam;
        return false;
    };
    const auto initial_scrollbar = ui_friends_scene_scrollbar(requests);
    if (!compact_check(initial_scrollbar.visible && initial_scrollbar.maximum_scroll > 0,
                       "requests_compact_scrollbar_geometry_failed"))
        return false;
    if (!compact_check(ui_friends_scene_page(&requests, false).user_id == 50003 &&
                           requests.scroll_dips > 0 && last_fully_visible(),
                       "requests_compact_page_last_row_failed"))
        return false;
    if (!compact_check(ui_friends_scene_navigate(&requests, UiListMove::first).user_id == 123 &&
                           requests.scroll_dips == 0 &&
                           ui_friends_scene_navigate(&requests, UiListMove::last).user_id == 50003 &&
                           last_fully_visible(),
                       "requests_compact_end_last_row_failed"))
        return false;
    if (!compact_check(renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme),
                       "requests_compact_text_failed"))
        return false;
    if (!compact_check(probe_present(&g_probe.renderer, kDefaultTheme),
                       "requests_compact_gpu_paint_failed"))
        return false;
    if (!compact_check(g_probe.renderer.paint_stream.count <= kMaxPaintQuads &&
                           g_probe.renderer.paint_stream.clip_count <= kMaxPaintClips &&
                           !g_probe.renderer.paint_stream.rejected &&
                           !g_probe.text.message_geometry_quads &&
                           g_probe.renderer.paint_stream.filter_scrollbars &&
                           d3d11_renderer_interaction_text_state_current(g_probe.renderer),
                       "requests_compact_budget_or_currentness_failed"))
        return false;
    const auto compact_scrollbar = ui_friends_scene_scrollbar(requests);
    const ProbePixelSample thumb_pixel{
        {(compact_scrollbar.thumb.left + compact_scrollbar.thumb.right) * .5f,
         (compact_scrollbar.thumb.top + compact_scrollbar.thumb.bottom) * .5f},
        kDefaultTheme.interactive,
        3};
    if (!compact_check(compact_scrollbar.visible && probe_exact_pixels({&thumb_pixel, 1}),
                       "requests_compact_scrollbar_gpu_pixels_failed"))
        return false;
    if (!compact_check(probe_capture_frame(L"friends-requests-compact.bmp"),
                       "requests_compact_capture_failed"))
        return false;
    if (!compact_check(restore.finish(), "requests_compact_restore_failed"))
        return false;
    log_write(LogLevel::info, "probe", "friends_pending_requests_gpu_layers_modal_owners_compact_passed");
    return true;
}

// Account pages as application_account_settings projects them: an avatar
// card, masked contact rows, the current device (accent, no action) before
// other devices (critical Log out), and synced Content & social controls.
bool probe_account_settings_pages() noexcept {
    UiSettingsScene scene{};
    UiPageRegistry registry{};
    UiPage pages[4]{};
    struct Restore {
        UiSettingsScene& scene;
        ~Restore() noexcept {
            renderer_bind_settings_scene(&g_probe.renderer, nullptr);
            ui_settings_scene_release(&scene);
        }
    } restore{scene};
    using Kind = UiSettingsControlKind;
    using Variant = UiSettingsVariant;
    const UiSettingsControl account[]{
        {1, utf8_literal("Rob"), utf8_literal("rob"), Kind::action, true, false, {}, 0, utf8_literal("Edit profile")},
        {2, utf8_literal("Account details"), {}, Kind::category, true, false},
        {3, utf8_literal("Display name"), utf8_literal("Rob"), Kind::action, true, false, {}, 0, utf8_literal("Edit")},
        {4, utf8_literal("Username"), utf8_literal("rob"), Kind::action, true, false, {}, 0,
         utf8_literal("Edit in browser")},
        {5, utf8_literal("Email"), utf8_literal("r**@example.com"), Kind::action, true, false, {}, 0,
         utf8_literal("Reveal")},
        {6, utf8_literal("Phone number"), utf8_literal("Not added"), Kind::action, true, false, {}, 0,
         utf8_literal("Add in browser")},
        {7, utf8_literal("Password and authentication"), {}, Kind::category, true, false},
        {8, utf8_literal("Two-factor authentication is on"),
         utf8_literal("Your account asks for a code when you sign in."), Kind::notice, true, false},
    };
    const UiSettingsControlPresentation account_looks[]{
        {Variant::standard, ui_settings_presentation_avatar, kUiSettingsNoDefault},
        {}, {}, {}, {}, {}, {},
        {Variant::positive, 0, kUiSettingsNoDefault}};
    const UiSettingsControl devices[]{
        {1, utf8_literal("Signed-in devices"), utf8_literal("Log out of any device you don't recognize."),
         Kind::category, true, false},
        {2, utf8_literal("Windows · MaiCord"), utf8_literal("This device · Bergen, Norway"), Kind::action, true,
         false},
        {3, utf8_literal("Android · Discord Android"), utf8_literal("Oslo, Norway · Active 2 hours ago"),
         Kind::action, true, false, {}, 0, utf8_literal("Log out")},
        {4, utf8_literal("Windows · Discord Client"), utf8_literal("Active 3 days ago"), Kind::action, true, false,
         {}, 0, utf8_literal("Log out")},
    };
    const UiSettingsControlPresentation device_looks[]{
        {}, {Variant::standard, ui_settings_presentation_accent, kUiSettingsNoDefault},
        {Variant::critical, 0, kUiSettingsNoDefault}, {Variant::critical, 0, kUiSettingsNoDefault}};
    const Utf8View spam[]{utf8_literal("Filter all"), utf8_literal("Non-friends only"), utf8_literal("Off")};
    const Utf8View media[]{utf8_literal("Show"), utf8_literal("Blur"), utf8_literal("Block")};
    const UiSettingsControl content[]{
        {1, utf8_literal("Direct messages"), {}, Kind::category, true, false},
        {2, utf8_literal("Allow DMs from server members"), utf8_literal("Applies to servers you join from now on."),
         Kind::toggle, true, true},
        {3, utf8_literal("Message requests"),
         utf8_literal("Enable requests from server members you may not know. Applies to servers you join from now on."),
         Kind::toggle, true, true},
        {4, utf8_literal("DM spam filter"), utf8_literal("Messages that look like spam go to a separate inbox."),
         Kind::radio, true, false, {spam, 3}, 1},
        {5, utf8_literal("Sensitive media"), utf8_literal("Media that may be sexually explicit."), Kind::category,
         true, false},
        {6, utf8_literal("DMs from friends"), {}, Kind::choice, true, false, {media, 3}, 0},
        {7, utf8_literal("Friend requests"), utf8_literal("Who can send you a friend request."), Kind::category,
         true, false},
        {8, utf8_literal("Everyone"), {}, Kind::toggle, true, true},
        {9, utf8_literal("Friends of friends"), {}, Kind::toggle, false, true},
    };
    UiSettingsPageContent account_page{utf8_literal("My account"), {}, {account, 8}, 1};
    account_page.presentation = account_looks;
    UiSettingsPageContent device_page{utf8_literal("Devices"), {}, {devices, 4}, 1};
    device_page.presentation = device_looks;
    UiSettingsPageContent content_page{utf8_literal("Content & social"), {}, {content, 9}, 1};
    // The Chat page as settings_catalog lays it out (text_and_images).
    const Utf8View stickers[]{utf8_literal("Always animate"), utf8_literal("Animate on hover"),
                              utf8_literal("Never animate")};
    const Utf8View spoilers[]{utf8_literal("On click"), utf8_literal("On servers I moderate"), utf8_literal("Always")};
    const UiSettingsControl chat[]{
        {1, utf8_literal("Images, videos and GIFs"), {}, Kind::category, true, false},
        {2, utf8_literal("When posted as links to chat"), {}, Kind::toggle, true, true},
        {3, utf8_literal("When uploaded directly to Discord"), utf8_literal("Off shows uploads as file cards."),
         Kind::toggle, true, false},
        {4, utf8_literal("Embeds and link previews"), {}, Kind::category, true, false},
        {5, utf8_literal("Show embeds and link previews"), {}, Kind::toggle, true, true},
        {6, utf8_literal("Emoji"), {}, Kind::category, true, false},
        {7, utf8_literal("Show emoji reactions"), {}, Kind::toggle, true, true},
        {8, utf8_literal("Convert emoticons to emoji"), utf8_literal("For example, :) becomes \xF0\x9F\x99\x82 when you send it."),
         Kind::toggle, true, true},
        {9, utf8_literal("Animation"), utf8_literal("Off keeps media still until you hover or focus its message."),
         Kind::category, true, false},
        {10, utf8_literal("Play GIFs automatically"), {}, Kind::toggle, true, false},
        {11, utf8_literal("Play animated emoji"), {}, Kind::toggle, true, true},
        {12, utf8_literal("Sticker animation"), {}, Kind::radio, true, false, {stickers, 3}, 1},
        {13, utf8_literal("Spoilers"), {}, Kind::category, true, false},
        {14, utf8_literal("Show spoiler content"), {}, Kind::radio, true, false, {spoilers, 3}, 0},
    };
    UiSettingsPageContent chat_page{utf8_literal("Chat"), {}, {chat, 14}, 1};
    const UiPageKey account_key{1, 1}, device_key{1, 2}, content_key{1, 3}, chat_key{1, 4};
    const float scale = 96.0f / g_probe.renderer.dpi;
    const UiRect viewport{0, 0, g_probe.renderer.width * scale, g_probe.renderer.height * scale};
    bool valid = ui_page_registry_initialize(&registry, {pages, 4}) &&
                 ui_page_registry_register(&registry, {account_key, utf8_literal("My account"), &account_page}).ok() &&
                 ui_page_registry_register(&registry, {device_key, utf8_literal("Devices"), &device_page}).ok() &&
                 ui_page_registry_register(&registry, {content_key, utf8_literal("Content & social"), &content_page})
                     .ok() &&
                 ui_page_registry_register(&registry, {chat_key, utf8_literal("Chat"), &chat_page}).ok() &&
                 ui_settings_scene_update(&scene, &registry, viewport);
    renderer_bind_settings_scene(&g_probe.renderer, &scene);
    valid = valid && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) && probe_capture_frame(L"user-settings-account.bmp");
    const auto show = [&](UiPageKey key, const wchar_t* capture) noexcept {
        return ui_page_registry_select(&registry, key).ok() && ui_settings_scene_update(&scene, &registry, viewport) &&
               scene.content.controls.count && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
               probe_present(&g_probe.renderer, kDefaultTheme) && probe_capture_frame(capture);
    };
    valid = valid && show(device_key, L"user-settings-devices.bmp") && show(content_key, L"user-settings-content.bmp") &&
            show(chat_key, L"user-settings-chat.bmp");
    if (valid)
        log_write(LogLevel::info, "probe", "account_settings_pages_passed");
    else
        log_write(LogLevel::error, "probe", "account_settings_pages_failed");
    return valid;
}

// Reads pixels (BGRA) at DIP points from one staging copy: of the drawn
// swap-chain target, or of another texture of the same size.
bool probe_read_pixels(const Theme& theme, ConstSlice<UiPoint> points, u8 (*output)[4],
                       ID3D11Texture2D* texture = nullptr) noexcept {
    ID3D11Texture2D* source = nullptr;
    if (texture) {
        texture->AddRef();
        source = texture;
    } else {
        if (!d3d11_renderer_draw_stream(&g_probe.renderer, theme, true, true, nullptr))
            return false;
        ID3D11Resource* resource = nullptr;
        g_probe.renderer.graphics.target->GetResource(&resource);
        const HRESULT queried =
            resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source));
        resource->Release();
        if (FAILED(queried))
            return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    const float scale = g_probe.renderer.dpi / 96.0f;
    bool inside = true;
    for (u32 i = 0; i < points.count; ++i)
        inside = inside && points[i].x >= 0 && points[i].y >= 0 &&
                 static_cast<u32>(points[i].x * scale) < description.Width &&
                 static_cast<u32>(points[i].y * scale) < description.Height;
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = description.MiscFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    const HRESULT created =
        inside ? g_probe.renderer.graphics.device->CreateTexture2D(&description, nullptr, &staging) : E_INVALIDARG;
    if (SUCCEEDED(created))
        g_probe.renderer.graphics.context->CopyResource(staging, source);
    source->Release();
    if (FAILED(created))
        return false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const bool read = SUCCEEDED(g_probe.renderer.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
    if (read) {
        for (u32 i = 0; i < points.count; ++i)
            copy_bytes(output[i],
                       static_cast<const u8*>(mapped.pData) +
                           static_cast<u32>(points[i].y * scale) * mapped.RowPitch +
                           static_cast<u32>(points[i].x * scale) * 4,
                       4);
        g_probe.renderer.graphics.context->Unmap(staging, 0);
    }
    staging->Release();
    return read;
}

// A modal draws the live shell beneath its scrim. The backdrop follows the
// theme and the window size (identical to the shell's own frame at that
// size, message wrapping included), redraws for each new frame of media it
// shows, and costs nothing while nothing behind changes.
bool probe_settings_backdrop() noexcept {
    g_probe.failure_stage = 733;
    UiSettingsScene scene{};
    UiPageRegistry registry{};
    UiPage pages[1]{};
    const u32 old_width = g_probe.renderer.width, old_height = g_probe.renderer.height;
    struct RestoreBackdropProbe {
        UiSettingsScene& scene;
        u32 width, height;
        ~RestoreBackdropProbe() noexcept {
            renderer_bind_settings_scene(&g_probe.renderer, nullptr);
            ui_settings_scene_release(&scene);
            if ((g_probe.renderer.width != width || g_probe.renderer.height != height) &&
                renderer_resize(&g_probe.renderer, width, height))
                synchronize_probe_shell_layout();
            if (rebuild_probe_content_text())
                probe_present(&g_probe.renderer, kDefaultTheme);
        }
    } restore{scene, old_width, old_height};
    UiSettingsControl control{1, utf8_literal("Backdrop setting"), {}, UiSettingsControlKind::toggle, true, false};
    UiSettingsPageContent content{utf8_literal("Backdrop"), {}, {&control, 1}, 1};
    const auto viewport = []() noexcept -> UiRect {
        const float scale = 96.0f / g_probe.renderer.dpi;
        return {0, 0, g_probe.renderer.width * scale, g_probe.renderer.height * scale};
    };
    ui_settings_scene_set_presentation(&scene, UiSettingsPresentation::modal);
    if (!ui_page_registry_initialize(&registry, {pages, 1}) ||
        !ui_page_registry_register(&registry, {{1, 1}, utf8_literal("Backdrop"), &content}).ok() ||
        !ui_settings_scene_update(&scene, &registry, viewport()))
        return false;
    // The scrim area above the dialog and beside it, where the shell shows.
    const auto samples = [&](const Theme& theme, u8 (*pixels)[4]) noexcept {
        const auto modal = scene.layout.modal;
        const UiPoint points[]{{(modal.left + modal.right) * .5f, modal.top * .5f},
                               {modal.left * .5f, (modal.top + modal.bottom) * .5f},
                               {(modal.right + viewport().right) * .5f, (modal.top + modal.bottom) * .5f}};
        return modal.top > 4 && modal.left > 4 && probe_read_pixels(theme, {points, 3}, pixels);
    };
    const auto luminance = [](const u8 (*pixels)[4]) noexcept {
        u32 sum = 0;
        for (u32 i = 0; i < 3; ++i)
            sum += pixels[i][0] + pixels[i][1] + pixels[i][2];
        return sum;
    };
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    // Mean CPU microseconds per present; animate notes a new frame of media
    // first, exactly as an uploaded GIF, emoji or avatar frame does.
    MediaAtlasHandle frame_handle{};
    u8 frame_domain = 0;
    const auto present_us = [&](u32 count, bool animate) noexcept -> u64 {
        u64 total = 0;
        for (u32 i = 0; i < count; ++i) {
            if (animate &&
                !presentation_damage_note_media(&g_probe.renderer.presentation_damage, frame_handle, frame_domain))
                return ~0ull;
            LARGE_INTEGER begin{}, end{};
            QueryPerformanceCounter(&begin);
            if (!probe_present(&g_probe.renderer, kDefaultTheme))
                return ~0ull;
            QueryPerformanceCounter(&end);
            total += static_cast<u64>(end.QuadPart - begin.QuadPart) * 1'000'000ull /
                     static_cast<u64>(frequency.QuadPart);
        }
        return total / count;
    };
    renderer_bind_settings_scene(&g_probe.renderer, &scene);
    const u32 renders = g_probe.renderer.settings_backdrop_renders;
    u8 dark[3][4]{};
    if (!renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.settings_backdrop_renders != renders + 1 || !g_probe.renderer.settings_backdrop_valid ||
        !samples(kDefaultTheme, dark) || !g_probe.renderer.settings_backdrop_media_count)
        return false;
    frame_handle = g_probe.renderer.settings_backdrop_media[0];
    frame_domain = g_probe.renderer.settings_backdrop_media_domain[0];
    g_probe.failure_stage = 734;
    // Idle: modal-only frames reuse the backdrop and the retained stream.
    const u32 paints = g_probe.renderer.paint_stream_builds;
    const u64 idle_us = present_us(8, false);
    if (idle_us == ~0ull || g_probe.renderer.settings_backdrop_renders != renders + 1 ||
        g_probe.renderer.paint_stream_builds != paints)
        return false;
    g_probe.failure_stage = 735;
    // Animated media behind: every new frame redraws the backdrop once, and
    // nothing is replayed afterwards.
    const u64 animated_us = present_us(8, true);
    if (animated_us == ~0ull || g_probe.renderer.settings_backdrop_renders != renders + 9 ||
        present_us(2, false) == ~0ull || g_probe.renderer.settings_backdrop_renders != renders + 9)
        return false;
    g_probe.failure_stage = 736;
    // A theme change re-themes the shell beneath the modal.
    u8 light[3][4]{};
    if (!renderer_rebuild_content_text(&g_probe.renderer, kLightTheme) ||
        !probe_present(&g_probe.renderer, kLightTheme) ||
        g_probe.renderer.settings_backdrop_renders != renders + 10 || !samples(kLightTheme, light) ||
        luminance(light) < luminance(dark) + 3 * 3 * 24 ||
        !probe_capture_frame(L"settings-backdrop-light.bmp", kLightTheme))
        return false;
    g_probe.failure_stage = 737;
    // A resize lays the whole shell out again beneath the modal, message text
    // re-converged at the new width: the backdrop equals the shell's frame.
    const u32 wide_width = old_width + old_width / 4, wide_height = old_height + old_height / 8;
    if (!renderer_resize(&g_probe.renderer, wide_width, wide_height) || !synchronize_probe_shell_layout() ||
        !ui_settings_scene_update(&scene, &registry, viewport()) || !rebuild_probe_content_text() ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.settings_backdrop_renders != renders + 11 ||
        g_probe.renderer.settings_backdrop_width != wide_width ||
        !probe_capture_frame(L"settings-backdrop-resized.bmp"))
        return false;
    constexpr u32 kColumns = 8, kRows = 6;
    UiPoint grid[kColumns * kRows]{};
    for (u32 row = 0; row < kRows; ++row)
        for (u32 column = 0; column < kColumns; ++column)
            grid[row * kColumns + column] = {viewport().right * (column + .5f) / kColumns,
                                             viewport().bottom * (row + .5f) / kRows};
    u8 backdrop[kColumns * kRows][4]{}, shell[kColumns * kRows][4]{};
    if (!probe_read_pixels(kDefaultTheme, {grid, kColumns * kRows}, backdrop, g_probe.renderer.settings_backdrop))
        return false;
    renderer_bind_settings_scene(&g_probe.renderer, nullptr);
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_read_pixels(kDefaultTheme, {grid, kColumns * kRows}, shell))
        return false;
    u32 matching = 0;
    for (u32 i = 0; i < kColumns * kRows; ++i) {
        u32 worst = 0;
        for (u32 c = 0; c < 3; ++c)
            worst = maximum<u32>(worst, backdrop[i][c] > shell[i][c] ? backdrop[i][c] - shell[i][c]
                                                                     : shell[i][c] - backdrop[i][c]);
        matching += worst <= 2;
    }
    if (matching * 10 < kColumns * kRows * 9)
        return false;
    g_probe.failure_stage = 738;
    // The same media frame with no modal open, for the cost comparison.
    const u64 shell_us = present_us(8, true);
    if (shell_us == ~0ull)
        return false;
    log_u64(LogLevel::info, "probe", "settings_backdrop_idle_present_us", idle_us);
    log_u64(LogLevel::info, "probe", "settings_backdrop_animated_present_us", animated_us);
    log_u64(LogLevel::info, "probe", "shell_animated_present_us", shell_us);
    log_u64(LogLevel::info, "probe", "settings_backdrop_matching_samples", matching);
    log_write(LogLevel::info, "probe", "settings_live_backdrop_theme_resize_animation_retention_passed");
    return true;
}

// Opening Settings fades a scrim in and grows the dialog (or settles a
// full-window layer) over the live shell. Each entrance frame is read as
// presented: two targets stand in for the flip-model back buffers. For one
// progress, a frame that reuses the backdrop, one that redraws it for a new
// media frame, one after the shell's message text converged beneath and one
// whose own stream rebuilt are identical; the first frame equals the shell
// before opening; a dialog's scrim only deepens. Behind an open dialog the
// backdrop's media stay pinned across modal-only frames, so an upload that
// needs the whole atlas is refused instead of resetting it, which made each
// backdrop redraw drop and restore the shell's images (the reported flicker).
bool probe_settings_entrance(UiSettingsPresentation presentation, u32 stage) noexcept {
    g_probe.failure_stage = stage;
    auto& renderer = g_probe.renderer;
    UiSettingsScene scene{};
    UiPageRegistry registry{};
    UiPage pages[1]{};
    ID3D11Texture2D* buffers[2]{};
    ID3D11RenderTargetView* targets[2]{};
    ID3D11RenderTargetView* const presented = renderer.graphics.target;
    struct RestoreEntranceProbe {
        UiSettingsScene& scene;
        ID3D11Texture2D** buffers;
        ID3D11RenderTargetView** targets;
        ID3D11RenderTargetView* presented;
        ~RestoreEntranceProbe() noexcept {
            g_probe.renderer.graphics.target = presented;
            renderer_bind_settings_scene(&g_probe.renderer, nullptr);
            renderer_set_settings_modal_progress(&g_probe.renderer, 1);
            ui_settings_scene_release(&scene);
            for (u32 i = 0; i < 2; ++i) {
                if (targets[i])
                    targets[i]->Release();
                if (buffers[i])
                    buffers[i]->Release();
            }
            if (rebuild_probe_content_text())
                probe_present(&g_probe.renderer, kDefaultTheme);
        }
    } restore{scene, buffers, targets, presented};
    ID3D11Resource* resource = nullptr;
    presented->GetResource(&resource);
    ID3D11Texture2D* back = nullptr;
    const bool typed =
        resource && SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)));
    if (resource)
        resource->Release();
    if (!typed)
        return false;
    D3D11_TEXTURE2D_DESC description{};
    back->GetDesc(&description);
    back->Release();
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.CPUAccessFlags = description.MiscFlags = 0;
    description.MipLevels = description.ArraySize = 1;
    for (u32 i = 0; i < 2; ++i)
        if (FAILED(renderer.graphics.device->CreateTexture2D(&description, nullptr, &buffers[i])) ||
            FAILED(renderer.graphics.device->CreateRenderTargetView(buffers[i], nullptr, &targets[i])))
            return false;
    UiSettingsControl control{1, utf8_literal("Entrance setting"), {}, UiSettingsControlKind::toggle, true, false};
    UiSettingsPageContent content{utf8_literal("Entrance"), {}, {&control, 1}, 1};
    const float dips = 96.0f / renderer.dpi;
    const UiRect viewport{0, 0, renderer.width * dips, renderer.height * dips};
    ui_settings_scene_set_presentation(&scene, presentation);
    if (!ui_page_registry_initialize(&registry, {pages, 1}) ||
        !ui_page_registry_register(&registry, {{1, 1}, utf8_literal("Entrance"), &content}).ok() ||
        !ui_settings_scene_update(&scene, &registry, viewport))
        return false;
    const bool dialog = presentation != UiSettingsPresentation::layer;
    const UiRect modal = scene.layout.modal;
    if (dialog && (modal.left <= 8 || modal.top <= 8))
        return false;
    // A dialog: the shell beside, above and right of it, and its centre. A
    // layer: points across the window.
    constexpr u32 kPoints = 4, kScrimPoints = 3;
    const UiPoint points[kPoints]{
        dialog ? UiPoint{modal.left * .5f, (modal.top + modal.bottom) * .5f} : UiPoint{viewport.right * .1f, viewport.bottom * .5f},
        dialog ? UiPoint{(modal.left + modal.right) * .5f, modal.top * .5f} : UiPoint{viewport.right * .5f, viewport.bottom * .08f},
        dialog ? UiPoint{(modal.right + viewport.right) * .5f, modal.top + 24} : UiPoint{viewport.right * .9f, viewport.bottom * .3f},
        dialog ? UiPoint{(modal.left + modal.right) * .5f, (modal.top + modal.bottom) * .5f}
               : UiPoint{viewport.right * .5f, viewport.bottom * .5f}};
    u32 frame = 0;
    const auto render = [&](u8 (*pixels)[4]) noexcept {
        renderer.graphics.target = targets[frame & 1u];
        const bool presented_frame = probe_present(&renderer, kDefaultTheme);
        renderer.graphics.target = presented;
        const bool read =
            presented_frame && probe_read_pixels(kDefaultTheme, {points, kPoints}, pixels, buffers[frame & 1u]);
        ++frame;
        return read;
    };
    const auto distance = [](const u8 (*a)[4], const u8 (*b)[4], u32 count) noexcept {
        u32 worst = 0;
        for (u32 i = 0; i < count; ++i)
            for (u32 c = 0; c < 3; ++c)
                worst = maximum<u32>(worst, a[i][c] > b[i][c] ? a[i][c] - b[i][c] : b[i][c] - a[i][c]);
        return worst;
    };
    const auto luminance = [](const u8 (*pixels)[4], u32 count) noexcept {
        u32 sum = 0;
        for (u32 i = 0; i < count; ++i)
            sum += pixels[i][0] + pixels[i][1] + pixels[i][2];
        return sum;
    };
    u8 shell[kPoints][4]{};
    renderer.retained_presentation.valid = false;
    presentation_damage_invalidate_full(&renderer.presentation_damage);
    if (!rebuild_probe_content_text() || !render(shell))
        return false;
    renderer_bind_settings_scene(&renderer, &scene);
    renderer_set_settings_modal_progress(&renderer, 0);
    if (!renderer_rebuild_content_text(&renderer, kDefaultTheme))
        return false;
    g_probe.failure_stage = stage + 1;
    constexpr u32 kSteps = 8;
    const u32 renders = renderer.settings_backdrop_renders;
    MediaAtlasHandle media{};
    u8 domain = 0;
    u8 previous[kPoints][4]{}, first[kPoints][4]{};
    u32 mismatches = 0, brighter = 0, invalid = 0, worst = 0, failed = 0;
    for (u32 step = 0; step <= kSteps; ++step) {
        u8 reused[kPoints][4]{}, redrawn[kPoints][4]{}, converged[kPoints][4]{}, rebuilt[kPoints][4]{};
        renderer_set_settings_modal_progress(&renderer, static_cast<float>(step) / kSteps);
        if (!render(reused))
            return false;
        if (step == 0) {
            if (!renderer.settings_backdrop_media_count)
                return false;
            media = renderer.settings_backdrop_media[0];
            domain = renderer.settings_backdrop_media_domain[0];
            copy_bytes(first, reused, sizeof(first));
        }
        // A new frame of animated media behind the dialog redraws the backdrop.
        if (!presentation_damage_note_media(&renderer.presentation_damage, media, domain) || !render(redrawn))
            return false;
        // The shell's text converges beneath (a gateway update): the account
        // path rebuilds content text with the dialog bound.
        if (!rebuild_probe_content_text() || !render(converged))
            return false;
        // The dialog's own stream rebuilds over the reused backdrop.
        renderer.retained_presentation.valid = false;
        presentation_damage_invalidate_full(&renderer.presentation_damage);
        if (!render(rebuilt))
            return false;
        invalid += renderer.settings_backdrop_valid || (!dialog && step == kSteps) ? 0u : 1u;
        failed += renderer.settings_backdrop_failed ? 1u : 0u;
        const u32 frame_worst =
            maximum(distance(reused, redrawn, kPoints),
                    maximum(distance(reused, converged, kPoints), distance(reused, rebuilt, kPoints)));
        worst = maximum(worst, frame_worst);
        mismatches += frame_worst > 2 ? 1u : 0u;
        if (dialog && step && luminance(reused, kScrimPoints) > luminance(previous, kScrimPoints) + 3)
            ++brighter;
        copy_bytes(previous, reused, sizeof(previous));
    }
    const u32 redraws = renderer.settings_backdrop_renders - renders;
    u32 backdrop_media = 0, pinned = 0, survivors = 0;
    bool reset = false, unpinned_reset = false;
    if (dialog) {
        const MediaAtlas& atlas = *renderer.media_atlas;
        if (atlas.allocations.entry_capacity > kProbeMediaEntryCapacity ||
            atlas.allocations.row_capacity > kProbeMediaRowCapacity)
            return false;
        // An upload needing the whole atlas, taken by a copy of the allocator
        // so the probe's atlas stays intact.
        GlyphAtlasEntry entries[kProbeMediaEntryCapacity]{};
        GlyphAtlasRow rows[kProbeMediaRowCapacity]{};
        GlyphAtlasAllocator trial{};
        const auto whole_upload = [&]() noexcept {
            copy_bytes(entries, atlas.allocations.entries, atlas.allocations.entry_capacity * sizeof(GlyphAtlasEntry));
            copy_bytes(rows, atlas.allocations.rows, atlas.allocations.row_capacity * sizeof(GlyphAtlasRow));
            trial = atlas.allocations;
            trial.entries = entries;
            trial.rows = rows;
            return glyph_atlas_allocate(&trial, ~u64{0} - 7, static_cast<u16>(trial.width - 2 * trial.padding),
                                        static_cast<u16>(trial.height - 2 * trial.padding))
                .atlas_reset;
        };
        // How every modal-only frame left the shell's media before: unpinned,
        // so the upload resets the atlas beneath the backdrop.
        media_atlas_begin_visibility(renderer.media_atlas);
        unpinned_reset = whole_upload();
        // Modal-only frames (a caret, a hover, the dialog's own media) begin
        // the next epochs; the backdrop's media stay pinned through them.
        u8 settled[kPoints][4]{};
        for (u32 i = 0; i < 2; ++i) {
            presentation_damage_invalidate_full(&renderer.presentation_damage);
            if (!render(settled))
                return false;
        }
        reset = whole_upload();
        for (u32 i = 0; i < renderer.settings_backdrop_media_count; ++i) {
            const MediaAtlasHandle handle = renderer.settings_backdrop_media[i];
            if (renderer.settings_backdrop_media_domain[i] != 0 || !media_atlas_validate(atlas, handle))
                continue;
            const GlyphAtlasHandle allocation = atlas.entries[handle.slot].allocation;
            GlyphAtlasAllocation kept{};
            ++backdrop_media;
            pinned += glyph_atlas_is_pinned(atlas.allocations, allocation) ? 1u : 0u;
            survivors += glyph_atlas_validate(trial, allocation, &kept) ? 1u : 0u;
        }
    }
    const bool resident = !dialog || (backdrop_media && pinned == backdrop_media &&
                                      survivors == backdrop_media && !reset && unpinned_reset);
    const bool paced = redraws == (dialog ? kSteps + 2 : kSteps + 1);
    const bool opened_on_shell = distance(first, shell, kPoints) <= 2;
    const bool deepened =
        !dialog || luminance(previous, kScrimPoints) + 3 * kScrimPoints * 8 < luminance(first, kScrimPoints);
    log_u64(LogLevel::info, "probe", "settings_entrance_presentation", static_cast<u64>(presentation));
    log_u64(LogLevel::info, "probe", "settings_entrance_frames", frame);
    log_u64(LogLevel::info, "probe", "settings_entrance_backdrop_redraws", redraws);
    log_u64(LogLevel::info, "probe", "settings_entrance_mismatched_steps", mismatches);
    log_u64(LogLevel::info, "probe", "settings_entrance_worst_channel_delta", worst);
    log_u64(LogLevel::info, "probe", "settings_entrance_brighter_steps", brighter);
    log_u64(LogLevel::info, "probe", "settings_entrance_invalid_backdrop_steps", invalid);
    log_u64(LogLevel::info, "probe", "settings_entrance_failed_backdrop_steps", failed);
    log_u64(LogLevel::info, "probe", "settings_entrance_opened_on_shell", opened_on_shell);
    log_u64(LogLevel::info, "probe", "settings_entrance_first_vs_shell", distance(first, shell, kPoints));
    log_u64(LogLevel::info, "probe", "settings_entrance_scrim_deepened", deepened);
    log_u64(LogLevel::info, "probe", "settings_entrance_backdrop_media", backdrop_media);
    log_u64(LogLevel::info, "probe", "settings_entrance_backdrop_media_pinned", pinned);
    log_u64(LogLevel::info, "probe", "settings_entrance_backdrop_media_survivors", survivors);
    log_u64(LogLevel::info, "probe", "settings_entrance_whole_atlas_reset", reset);
    log_u64(LogLevel::info, "probe", "settings_entrance_unpinned_atlas_reset", unpinned_reset);
    if (mismatches || brighter || invalid || failed || !opened_on_shell || !deepened || !resident || !paced)
        return false;
    log_write(LogLevel::info, "probe", "settings_entrance_frames_consistent_passed");
    return true;
}

bool probe_settings_entrance_frames() noexcept {
    return probe_settings_entrance(UiSettingsPresentation::modal, 3739) &&
           probe_settings_entrance(UiSettingsPresentation::layer, 3741);
}

bool probe_settings_scene() noexcept {
    g_probe.failure_stage = 246;
    UiSettingsScene scene{};
    UiPageRegistry registry{};
    UiPage pages[3]{};
    const u32 old_width = g_probe.renderer.width, old_height = g_probe.renderer.height;
    struct RestoreSettingsProbe {
        UiSettingsScene& scene;
        u32 width, height;
        ~RestoreSettingsProbe() noexcept {
            renderer_bind_settings_scene(&g_probe.renderer, nullptr);
            ui_settings_scene_release(&scene);
            renderer_resize(&g_probe.renderer, width, height);
        }
    } restore{scene, old_width, old_height};
    const Utf8View themes[]{utf8_literal("Dark"), utf8_literal("AMOLED")};
    UiSettingsControl appearance_controls[]{
        {1,
         utf8_literal("Theme"),
         utf8_literal("Choose the colors used by the native client."),
         UiSettingsControlKind::choice,
         true,
         false,
         {themes, 2},
         0},
        {2, utf8_literal("Smooth scrolling"), utf8_literal("Animate scrolling between positions."),
         UiSettingsControlKind::toggle, true, true},
        {3, utf8_literal("Member list"), utf8_literal("Show the member pane in server channels."),
         UiSettingsControlKind::toggle, true, true},
        {4, utf8_literal("Unavailable setting"),
         utf8_literal("Disabled controls keep their label and description readable."),
         UiSettingsControlKind::toggle, false, false},
    };
    UiSettingsControl accessibility_controls[]{
        {1, utf8_literal("Reduced motion"),
         utf8_literal("Reduce decorative animations and scrolling motion."), UiSettingsControlKind::toggle,
         true, false},
    };
    UiSettingsControl plugin_controls[70]{};
    for (u32 i = 0; i < 70; ++i)
        plugin_controls[i] = {
            i + 1,
            utf8_literal("Plugin setting with a descriptive label"),
            utf8_literal("A borrowed plugin descriptor uses the same native row and scrolling surface."),
            UiSettingsControlKind::toggle,
            true,
            (i & 1u) != 0};
    plugin_controls[69] = {70,
                           utf8_literal("Last plugin setting"),
                           utf8_literal("Keyboard focus reveals this virtualized row."),
                           UiSettingsControlKind::toggle,
                           true,
                           true};
    UiSettingsPageContent appearance{utf8_literal("Appearance"),
                                     utf8_literal("Make MaiCord look and feel right for you."),
                                     {appearance_controls, 4},
                                     1};
    UiSettingsPageContent accessibility{utf8_literal("Accessibility"),
                                        utf8_literal("Control motion and interaction preferences."),
                                        {accessibility_controls, 1},
                                        1};
    UiSettingsPageContent plugin{
        utf8_literal("Plugin preferences"),
        utf8_literal(
            "A long modular page owned by a plugin. Its controls share the native settings framework."),
        {plugin_controls, 70},
        1};
    const UiPageKey appearance_key{1, 1}, accessibility_key{1, 2}, plugin_key{77, 1};
    const auto viewport = []() noexcept -> UiRect {
        const float scale = 96.0f / g_probe.renderer.dpi;
        return {0, 0, g_probe.renderer.width * scale, g_probe.renderer.height * scale};
    };
    const auto settings_pixels = [](const UiSettingsScene& settings, bool amoled) noexcept {
        UiTextCommand commands[64]{};
        UiTextCommandStream stream{};
        if (!ui_text_command_stream_initialize(&stream, {commands, 64}) ||
            !ui_settings_scene_text(settings, amoled ? kAmoledTheme : kDefaultTheme, &stream))
            return false;
        const Utf8View labels[]{utf8_literal("Theme"), utf8_literal("ESC"),
                                amoled ? utf8_literal("AMOLED") : utf8_literal("Dark"),
                                utf8_literal("Accessibility")};
        UiRect rectangles[4]{};
        for (u32 n = 0; n < 4; ++n) {
            bool found = false;
            for (u32 i = 0; i < stream.count; ++i) {
                const auto& command = commands[i];
                if (command.text.bytes == labels[n].bytes &&
                    bytes_equal(command.text.data, labels[n].data, labels[n].bytes)) {
                    rectangles[n] = ui_rect_intersection(command.rectangle, command.clip);
                    found = true;
                    break;
                }
            }
            if (!found || !ui_rect_has_area(rectangles[n]))
                return false;
        }
        return probe_text_pixels({rectangles, 4});
    };
    if (!ui_page_registry_initialize(&registry, {pages, 3}) ||
        !ui_page_registry_register(&registry, {appearance_key, utf8_literal("Appearance"), &appearance})
             .ok() ||
        !ui_page_registry_register(&registry,
                                   {accessibility_key, utf8_literal("Accessibility"), &accessibility})
             .ok() ||
        !ui_page_registry_register(&registry, {plugin_key, utf8_literal("Plugin preferences"), &plugin})
             .ok() ||
        !ui_settings_scene_update(&scene, &registry, viewport()))
        return false;
    renderer_bind_settings_scene(&g_probe.renderer, &scene);
    ui_settings_scene_set_focus_visible(&scene, true);
    if (!ui_settings_scene_focus(&scene, {UiSettingsTargetKind::control, appearance_key, 1}) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) || !settings_pixels(scene, false) ||
        g_probe.renderer.media_quad_count || g_probe.text.message_geometry_quads ||
        !probe_capture_frame(L"settings-dark.bmp"))
        return false;
    const u32 text_builds = g_probe.renderer.content_text_retention.builds;
    const u32 widget_generation = scene.widgets.generation;
    if (!ui_settings_scene_focus(&scene, {UiSettingsTargetKind::control, appearance_key, 2}) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        scene.widgets.generation != widget_generation ||
        g_probe.renderer.content_text_retention.builds != text_builds ||
        !probe_present(&g_probe.renderer, kDefaultTheme))
        return false;
    UiSettingsActivation activation{};
    if (ui_settings_scene_activation(scene, {UiSettingsTargetKind::control, appearance_key, 4}, false,
                                     &activation))
        return false;
    const u32 paints = g_probe.renderer.paint_stream_builds;
    if (!probe_present(&g_probe.renderer, kDefaultTheme) || g_probe.renderer.paint_stream_builds != paints)
        return false;
    {
        // Pointer state is paint-only: retained text and layout stay reusable,
        // while the native frame must observe hover/capture on the top page.
        UiRect anchor{};
        if (!ui_settings_scene_control_anchor(scene, {UiSettingsTargetKind::control, appearance_key, 2},
                                              &anchor))
            return false;
        const UiPoint point{(anchor.left + anchor.right) * .5f, (anchor.top + anchor.bottom) * .5f};
        const u32 revision = scene.revision, generation = scene.widgets.generation;
        const auto layout = scene.widgets.layout.revision;
        const u32 text_rebuilds = g_probe.renderer.content_text_retention.builds;
        if (!ui_button_pointer(&scene.widgets, UiButtonPointerEvent::move, point).changed ||
            !probe_present(&g_probe.renderer, kDefaultTheme) ||
            g_probe.renderer.paint_stream_builds != paints + 1 ||
            !probe_capture_frame(L"settings-pointer-hover.bmp"))
            return false;
        if (!ui_button_pointer(&scene.widgets, UiButtonPointerEvent::press, point).handled ||
            !probe_present(&g_probe.renderer, kDefaultTheme) ||
            g_probe.renderer.paint_stream_builds != paints + 2 ||
            !probe_capture_frame(L"settings-pointer-pressed.bmp"))
            return false;
        if (!ui_button_pointer(&scene.widgets, UiButtonPointerEvent::release, point).activate ||
            !probe_present(&g_probe.renderer, kDefaultTheme) ||
            g_probe.renderer.paint_stream_builds != paints + 3 || scene.revision != revision ||
            scene.widgets.generation != generation || scene.widgets.layout.revision != layout ||
            g_probe.renderer.content_text_retention.builds != text_rebuilds)
            return false;
        ui_button_pointer(&scene.widgets, UiButtonPointerEvent::cancel, {});
        if (!probe_present(&g_probe.renderer, kDefaultTheme))
            return false;
        const u32 retained = g_probe.renderer.paint_stream_builds;
        if (!probe_present(&g_probe.renderer, kDefaultTheme) ||
            g_probe.renderer.paint_stream_builds != retained)
            return false;
        log_write(LogLevel::info, "probe", "settings_native_pointer_hover_press_retention_passed");
    }
    {
        // Real, bounded native overlays keep valid borrowed storage while
        // Settings owns the screen. Background revisions must not enter its
        // text stream, stamp, edit measurement, or glyph visibility guard.
        const u32 hidden_paints = g_probe.renderer.paint_stream_builds;
        UiImageViewer hidden_image{};
        PopupScene hidden_popup{};
        PopupItem popup_items[1]{};
        PopupRowWidget popup_rows[1]{};
        UiWidget popup_widgets[8]{};
        UiLayoutNode popup_nodes[8]{};
        const PopupItem popup_item{utf8_literal("Hidden background menu"), 1, popup_item_enabled};
        TextFormScene hidden_form{};
        TextFormSurface hidden_surface{};
        ProfileScene hidden_profile{};
        UserProfile hidden_profile_data{};
        hidden_profile_data.user_id = 1;
        hidden_profile.measure = &g_probe.text_measure_cache;
        MessageActionBar hidden_bar{};
        ReactionViewer hidden_reactions{};
        ReactionViewerUser reaction_users[1]{};
        char reaction_text[64]{};
        MessageRichStore reaction_store{};
        MessageRichRecord reaction_record[1]{};
        u16 reaction_order[1]{};
        u8 reaction_names[64]{};
        struct RestoreHidden {
            const UiImageViewer* image;
            const PopupScene* popup;
            TextFormSurface* form;
            ProfileScene* profile;
            const MessageActionBar* bar;
            const ReactionViewer* reactions;
            TextFormScene& form_scene;
            TextFormSurface& surface;
            ProfileScene& profile_scene;
            u32 appearance, navigation, messages, media, preview, composer;
            u64 hover;
            bool message_text_active;
            ~RestoreHidden() noexcept {
                auto& renderer = g_probe.renderer;
                renderer.image_viewer = image;
                renderer.popup_scene = popup;
                renderer.text_form = form;
                renderer.profile_scene = profile;
                renderer.message_action_bar = bar;
                renderer.reaction_viewer = reactions;
                g_probe.users.appearance.revision = appearance;
                g_probe.navigation.revision = navigation;
                g_probe.message_scene.data_updates = messages;
                g_probe.message_scene.widgets.hover_started_ms = hover;
                g_probe.media_atlas.insertions = media;
                g_probe.preview_media_atlas.insertions = preview;
                g_probe.message_composer_scene.observed_revision = composer;
                g_probe.message_text.active = message_text_active;
                text_form_surface_release(&surface);
                text_form_release(&form_scene);
                profile_scene_release(&profile_scene);
            }
        } hidden_restore{g_probe.renderer.image_viewer,
                         g_probe.renderer.popup_scene,
                         g_probe.renderer.text_form,
                         g_probe.renderer.profile_scene,
                         g_probe.renderer.message_action_bar,
                         g_probe.renderer.reaction_viewer,
                         hidden_form,
                         hidden_surface,
                         hidden_profile,
                         g_probe.users.appearance.revision,
                         g_probe.navigation.revision,
                         g_probe.message_scene.data_updates,
                         g_probe.media_atlas.insertions,
                         g_probe.preview_media_atlas.insertions,
                         g_probe.message_composer_scene.observed_revision,
                         g_probe.message_scene.widgets.hover_started_ms,
                         g_probe.message_text.active};
        const TextFormContent form_content{utf8_literal("Hidden background form"),
                                           {},
                                           utf8_literal("Value"),
                                           {},
                                           utf8_literal("Cancel"),
                                           utf8_literal("Save"),
                                           utf8_literal("Borrowed text"),
                                           0,
                                           0,
                                           true,
                                           false};
        if (!ui_image_viewer_open(&hidden_image, viewport(), {64, 64}) ||
            !popup_scene_initialize(&hidden_popup, {popup_items, 1}, {popup_rows, 1}, {popup_widgets, 8},
                                    {popup_nodes, 8}) ||
            !popup_scene_open(&hidden_popup, PopupKind::context_menu, 1, {&popup_item, 1}, {20, 20, 60, 60},
                              viewport(), 240)
                 .ok() ||
            !text_form_update(&hidden_form, form_content, viewport(), &g_probe.text_measure_cache) ||
            !profile_scene_open(&hidden_profile, &hidden_profile_data, &g_probe.chat_state, &g_probe.users, 1,
                                {20, 20, 60, 60}, viewport()) ||
            !reaction_viewer_initialize(&hidden_reactions, {reaction_users, 1}, {reaction_text, 64}) ||
            !message_rich_store_initialize(&reaction_store, {reaction_record, 1}, {reaction_order, 1},
                                           {reaction_names, 64}))
            return false;
        const auto reaction_name = utf8_literal("Hidden");
        copy_bytes(reaction_names, reaction_name.data, reaction_name.bytes);
        reaction_store.text_used = reaction_name.bytes;
        reaction_record[0].occupied = true;
        reaction_record[0].message_id = 1;
        reaction_record[0].reaction_count = 1;
        reaction_record[0].reactions[0] = {19, {0, 6, 0}, 1, 1};
        if (!reaction_viewer_open(&hidden_reactions, 0, 1, 1, reaction_store, reaction_record[0],
                                  viewport())
                 .ok())
            return false;
        hidden_bar.owner = {&hidden_popup.widgets, {0, hidden_popup.widgets.storage[0].generation}};
        hidden_bar.message_id = 1;
        hidden_bar.active = true;
        hidden_bar.item_count = kMessageActionBarItemCount;
        hidden_bar.rectangle = hidden_bar.clip = {20, 20, 220, 52};
        for (u32 i = 0; i < kMessageActionBarQuickReactionCount; ++i)
            hidden_bar.items[i] = {{20.f + i * 28, 20, 48.f + i * 28, 48},
                                   utf8_literal("H"),
                                   0,
                                   MessageActionBarAction::quick_reaction_0,
                                   MessageActionBarItemKind::quick_reaction};
        hidden_surface.scene = &hidden_form;
        const auto settings_stamp = renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme);
        const auto composer_layout = g_probe.renderer.composer_layout;
        const auto popup_layout = g_probe.renderer.popup_edit_layout;
        const auto profile_layout = g_probe.renderer.profile_edit_layout;
        const auto edit_selection = g_probe.renderer.edit_selection_owner;
        const u32 retained_builds = g_probe.renderer.content_text_retention.builds;
        const u32 text_rebuilds = g_probe.text.rebuilds, quads = g_probe.text.quad_count,
                  batches = g_probe.text.batch_count;
        const u32 stale_suppressions = g_probe.renderer.stale_interaction_text_suppressions;
        g_probe.renderer.image_viewer = &hidden_image;
        g_probe.renderer.popup_scene = &hidden_popup;
        // An active bound form is now a foreground Settings confirmation.
        // This unrelated background form stays unbound; Overview's dedicated
        // fixture verifies the foreground modal paint, glyphs and focus.
        g_probe.renderer.profile_scene = &hidden_profile;
        g_probe.renderer.message_action_bar = &hidden_bar;
        g_probe.renderer.reaction_viewer = &hidden_reactions;
        ++g_probe.users.appearance.revision;
        ++g_probe.navigation.revision;
        ++g_probe.message_scene.data_updates;
        ++g_probe.message_scene.widgets.hover_started_ms;
        ++g_probe.message_composer_scene.observed_revision;
        ++g_probe.media_atlas.insertions;
        ++g_probe.preview_media_atlas.insertions;
        g_probe.message_text.active = false;
        if (!renderer_presentation_stamp_equal(
                settings_stamp, renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme)) ||
            !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
            g_probe.renderer.content_text_retention.builds != retained_builds ||
            g_probe.text.rebuilds != text_rebuilds || g_probe.text.quad_count != quads ||
            g_probe.text.batch_count != batches ||
            g_probe.renderer.interaction_text_active_mask != (1u << 6) || hidden_surface.layout.active ||
            !bytes_equal(&composer_layout, &g_probe.renderer.composer_layout, sizeof(composer_layout)) ||
            !bytes_equal(&popup_layout, &g_probe.renderer.popup_edit_layout, sizeof(popup_layout)) ||
            !bytes_equal(&profile_layout, &g_probe.renderer.profile_edit_layout, sizeof(profile_layout)) ||
            g_probe.renderer.edit_selection_owner != edit_selection ||
            !probe_present(&g_probe.renderer, kDefaultTheme) ||
            g_probe.renderer.paint_stream_builds != hidden_paints)
            return false;
        ++hidden_popup.revision;
        ++hidden_image.revision;
        ++hidden_form.revision;
        ++hidden_profile.revision;
        ++hidden_bar.revision;
        ++hidden_reactions.revision;
        if (!settings_pixels(scene, false) ||
            g_probe.renderer.stale_interaction_text_suppressions != stale_suppressions ||
            !renderer_presentation_stamp_equal(
                settings_stamp, renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme)))
            return false;
        log_write(LogLevel::info, "probe",
                  "settings_exclusive_hidden_overlays_text_paint_retained_pixels_passed");
    }
    // Theme-only changes still invalidate the visible Settings text and paint.
    const u32 theme_text_builds = g_probe.renderer.content_text_retention.builds;
    const u32 theme_paints = g_probe.renderer.paint_stream_builds;
    if (!renderer_rebuild_content_text(&g_probe.renderer, kAmoledTheme) ||
        g_probe.renderer.content_text_retention.builds <= theme_text_builds ||
        !probe_present(&g_probe.renderer, kAmoledTheme) ||
        g_probe.renderer.paint_stream_builds <= theme_paints ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme))
        return false;
    const u32 scene_text_builds = g_probe.renderer.content_text_retention.builds;
    const u32 scene_paints = g_probe.renderer.paint_stream_builds;
    appearance_controls[0].selected_choice = 1;
    ++appearance.revision;
    if (!ui_settings_scene_update(&scene, &registry, viewport()) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.content_text_retention.builds <= scene_text_builds ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.paint_stream_builds <= scene_paints ||
        !renderer_rebuild_content_text(&g_probe.renderer, kAmoledTheme) ||
        !probe_present(&g_probe.renderer, kAmoledTheme) || !settings_pixels(scene, true) ||
        !probe_capture_frame(L"settings-amoled.bmp", kAmoledTheme))
        return false;
    bool black_background = false;
    for (u32 i = 0; i < g_probe.renderer.paint_stream.count; ++i) {
        const auto& quad = g_probe.renderer.paint_stream.quads[i];
        black_background =
            black_background || (quad.left == 0 && quad.top == 0 && quad.right == viewport().right &&
                                 quad.bottom == viewport().bottom && quad.fill.red == 0 &&
                                 quad.fill.green == 0 && quad.fill.blue == 0 && quad.fill.alpha == 1);
    }
    if (!black_background)
        return false;
    g_probe.failure_stage = 247;
    const auto allocation = scene.storage.arena.base;
    if (!ui_settings_scene_select(&scene, plugin_key) ||
        !ui_settings_scene_focus(&scene, {UiSettingsTargetKind::control, plugin_key, 70}) ||
        scene.content_offset < 4000 || scene.widgets.count >= 70 ||
        scene.storage.arena.base != allocation ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_capture_frame(L"settings-plugin-scrolled.bmp"))
        return false;
    g_probe.failure_stage = 248;
    const auto resize_stamp = renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme);
    const u32 resize_text_builds = g_probe.renderer.content_text_retention.builds;
    const u32 resize_paints = g_probe.renderer.paint_stream_builds;
    const u32 narrow_width = static_cast<u32>(480.0f * g_probe.renderer.dpi / 96.0f);
    const u32 narrow_height = static_cast<u32>(360.0f * g_probe.renderer.dpi / 96.0f);
    if (!renderer_resize(&g_probe.renderer, narrow_width, narrow_height) ||
        !ui_settings_scene_update(&scene, &registry, viewport()) ||
        !ui_settings_scene_focus(&scene, {UiSettingsTargetKind::control, plugin_key, 70}) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.content_text_retention.builds <= resize_text_builds ||
        renderer_presentation_stamp_equal(
            resize_stamp, renderer_capture_presentation_stamp(g_probe.renderer, kDefaultTheme)) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.paint_stream_builds <= resize_paints ||
        !probe_capture_frame(L"settings-compact.bmp"))
        return false;
    if (ui_page_registry_unregister_owner(&registry, 77) != 1 ||
        ui_settings_scene_focused(scene).kind != UiSettingsTargetKind::none ||
        !ui_settings_scene_update(&scene, &registry, viewport()) ||
        !ui_page_key_equal(scene.selected_page, accessibility_key) ||
        !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_capture_frame(L"settings-owner-removed.bmp"))
        return false;
    renderer_bind_settings_scene(&g_probe.renderer, nullptr);
    ui_settings_scene_close(&scene);
    if (scene.storage.arena.base || scene.registry ||
        !renderer_resize(&g_probe.renderer, old_width, old_height) || !synchronize_probe_shell_layout() ||
        !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme))
        return false;
    log_write(LogLevel::info, "probe",
              "settings_dark_amoled_virtualized_owner_rebind_resize_hardware_passed");
    return true;
}

bool probe_autocomplete_identity() noexcept {
    g_probe.failure_stage = 145;
    const float scale = 96.0f / g_probe.renderer.dpi;
    const UiRect viewport{0, 0, g_probe.renderer.width * scale, g_probe.renderer.height * scale};
    const auto* surface = ui_widget_manager_layout_node(g_probe.message_composer_scene.widgets,
                                                        g_probe.message_composer_scene.surface);
    if (!surface)
        return false;
    const UiRect anchor = surface->rectangle;
    PopupItem item{.label = utf8_literal("𝕼𝖚𝖎𝖑𝖑𝕱𝖊𝖗𝖓"),
                   .action = 1,
                   .flags = popup_item_enabled | popup_item_avatar,
                   .media_id = 9917,
                   .detail = utf8_literal("quillfern"),
                   .media_variant = 117,
                   .status = 1};
    u8 pixels[24 * 24 * 4];
    for (u32 i = 0; i < 24 * 24; ++i) {
        pixels[i * 4] = 80;
        pixels[i * 4 + 1] = (i % 24 < 12) ? 140 : 90;
        pixels[i * 4 + 2] = 220;
        pixels[i * 4 + 3] = 255;
    }
    const auto key = media_asset_user_avatar(item.media_id, item.media_variant, false);
    if (!renderer_upload_media_rgba8(&g_probe.renderer, key, 24, 24, 96, {pixels, sizeof(pixels)}).ok() ||
        !popup_scene_open(&g_probe.popup_scene, PopupKind::autocomplete, 77, {&item, 1}, anchor, viewport,
                          ui_rect_width(anchor), true)
             .ok() ||
        !renderer_bind_popup_scene(&g_probe.renderer, &g_probe.popup_scene) ||
        !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme))
        return false;
    PopupMediaCommand media{};
    g_probe.failure_stage = 146;
    UiTextCommand commands[4]{};
    UiTextCommandStream text{};
    if (popup_media_presentation_resolve(g_probe.popup_scene, 0, &media) !=
            PopupMediaPresentationStatus::ready ||
        !media.avatar || media.media_variant != 117 ||
        !ui_text_command_stream_initialize(&text, {commands, 4}) ||
        !popup_text_presentation_build(g_probe.popup_scene, kDefaultTheme, &text) || text.count != 2 ||
        !utf8_equal(commands[0].text, item.detail) ||
        commands[0].horizontal_alignment != UiTextAlignment::end ||
        !utf8_equal(commands[1].text, item.label) ||
        commands[1].rectangle.right >= commands[0].rectangle.left ||
        commands[1].rectangle.left <= media.rectangle.right || !g_probe.renderer.media_quad_count)
        return false;
    const auto& quad = g_probe.renderer.media_draw.quad_data[g_probe.renderer.media_quad_count - 1];
    g_probe.failure_stage = 147;
    const ProbePixelSample user_status{{media.rectangle.right + 4.5f, media.rectangle.bottom - 3.5f},
                                       kDefaultTheme.tokens.status.online,
                                       3};
    if (quad.left != media.rectangle.left || quad.right != media.rectangle.right ||
        !probe_exact_pixels({&user_status, 1}) || !probe_capture_frame(L"autocomplete.bmp"))
        return false;
    popup_scene_close(&g_probe.popup_scene);
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme))
        return false;
    log_write(LogLevel::info, "probe",
              "autocomplete_avatar_status_and_distinct_name_columns_hardware_passed");
    {
        g_probe.failure_stage = 148;
        // Application artwork has no presence. Once resident, its alpha reveals
        // the actual rail/row surface rather than the loading placeholder.
        PopupItem apps[]{{.label = utf8_literal("Test app"),
                          .action = 200000,
                          .flags = popup_item_enabled | popup_item_command_category | popup_item_avatar,
                          .media_id = 9918,
                          .media_variant = 118},
                         {.label = utf8_literal("/wordle"),
                          .action = 100000,
                          .flags = popup_item_enabled | popup_item_command_row | popup_item_avatar,
                          .media_id = 9918,
                          .detail = utf8_literal("Play wordle"),
                          .media_variant = 118}};
        if (!popup_scene_open(&g_probe.popup_scene, PopupKind::autocomplete, 77, {apps, 2}, anchor,
                              viewport, ui_rect_width(anchor), true)
                 .ok() ||
            !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme))
            return false;
        ProbePixelSample missing[2]{};
        PopupMediaCommand artwork[2]{};
        for (u32 i = 0; i < 2; ++i) {
            if (popup_media_presentation_resolve(g_probe.popup_scene, i, &artwork[i]) !=
                PopupMediaPresentationStatus::ready)
                return false;
            const auto r = artwork[i].rectangle;
            missing[i] = {
                {(r.left + r.right) * .5f, (r.top + r.bottom) * .5f}, kDefaultTheme.interactive, 3};
        }
        if (!probe_exact_pixels({missing, 2}))
            return false;
        for (u32 y = 0; y < 24; ++y)
            for (u32 x = 0; x < 24; ++x) {
                const u32 at = (y * 24 + x) * 4;
                const u8 alpha = x < 8 ? 0 : x < 16 ? 128 : 255;
                // Original premultiplied magenta artwork: transparent, half,
                // opaque thirds, all sampled well inside the circular mask.
                pixels[at] = pixels[at + 2] = static_cast<u8>((240 * alpha + 127) / 255);
                pixels[at + 1] = static_cast<u8>((80 * alpha + 127) / 255);
                pixels[at + 3] = alpha;
            }
        if (!renderer_upload_media_rgba8(&g_probe.renderer, media_asset_user_avatar(9918, 118, false), 24,
                                         24, 96, {pixels, sizeof(pixels)})
                 .ok())
            return false;
        for (u32 selected = 0; selected < 2; ++selected) {
            g_probe.popup_scene.selected_index = selected;
            ++g_probe.popup_scene.revision;
            if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme))
                return false;
            UiPoint points[10]{};
            u8 actual[10][4]{};
            for (u32 i = 0; i < 2; ++i) {
                const auto r = artwork[i].rectangle;
                const float y = (r.top + r.bottom) * .5f;
                points[i * 5] = {r.left - 4, y};
                points[i * 5 + 1] = {r.left + 6, y};
                points[i * 5 + 2] = {r.left + 12, y};
                points[i * 5 + 3] = {r.left + 18, y};
                points[i * 5 + 4] = {r.right + 4.5f, r.bottom - 6};
            }
            if (!probe_read_pixels(kDefaultTheme, {points, 10}, actual))
                return false;
            for (u32 i = 0; i < 2; ++i)
                for (u32 channel = 0; channel < 3; ++channel) {
                    const u32 background = actual[i * 5][channel];
                    const u32 opaque = channel == 1 ? 80 : 240;
                    const u32 half = (opaque * 128 + 127) / 255 + (background * 127 + 127) / 255;
                    const u32 expected[]{background, half, opaque, background};
                    for (u32 sample = 0; sample < 4; ++sample) {
                        const u32 value = actual[i * 5 + sample + 1][channel];
                        if (maximum(value, expected[sample]) - minimum(value, expected[sample]) > 3)
                            return false;
                    }
                }
        }
        popup_scene_close(&g_probe.popup_scene);
    }
    for (u32 pass = 0; pass < 2; ++pass) {
        PopupItem items[]{{utf8_literal("All"), 200000, popup_item_enabled | popup_item_command_category},
                          {pass ? utf8_literal("Command failed. Try again.") : utf8_literal("/wordle"),
                           100000,
                           static_cast<u16>(pass ? popup_item_command_row
                                                 : popup_item_command_row | popup_item_enabled)}};
        if (!pass)
            items[1].detail = utf8_literal("Play wordle");
        if (!popup_scene_open(&g_probe.popup_scene, PopupKind::autocomplete, 77, {items, 2}, anchor,
                              viewport, ui_rect_width(anchor), true)
                 .ok() ||
            !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme))
            return false;
        UiTextCommand text_commands[4]{};
        UiTextCommandStream stream{};
        UiRect rectangles[4]{};
        if (!ui_text_command_stream_initialize(&stream, {text_commands, 4}) ||
            !popup_text_presentation_build(g_probe.popup_scene, kDefaultTheme, &stream) || !stream.count)
            return false;
        for (u32 i = 0; i < stream.count; ++i)
            rectangles[i] = text_commands[i].rectangle;
        if (!probe_capture_frame(pass ? L"commands-error.bmp" : L"commands-picker.bmp") ||
            !probe_text_pixels({rectangles, stream.count}, 70))
            return false;
    }
    popup_scene_close(&g_probe.popup_scene);
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme))
        return false;
    PopupItem emojis[]{{utf8_literal(":saluting_face:"), 1, popup_item_enabled},
                       {utf8_literal(":sad_but_relieved_face:"), 2, popup_item_enabled},
                       {utf8_literal(":santa:"), 3, popup_item_enabled}};
    if (!popup_scene_open(&g_probe.popup_scene, PopupKind::autocomplete, 77, {emojis, 3}, anchor, viewport,
                          ui_rect_width(anchor), true)
             .ok() ||
        !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_capture_frame(L"emoji-autocomplete.bmp"))
        return false;
    popup_scene_close(&g_probe.popup_scene);
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme))
        return false;
    log_write(LogLevel::info, "probe", "command_picker_and_error_text_pixels_passed");
    return true;
}

bool probe_glyph_atlas_gutters() noexcept {
    g_probe.failure_stage = 148;
    GlyphAtlasGpu gpu{};
    auto* device = g_probe.renderer.graphics.device;
    auto* context = g_probe.renderer.graphics.context;
    if (!glyph_atlas_gpu_initialize(&gpu, device, 32, 32, 8).ok())
        return false;
    GlyphPathCommand commands[] = {
        {0, 0, 0, 0, 0, 0, GlyphPathCommandKind::begin_filled},
        {1, 0, 0, 0, 0, 0, GlyphPathCommandKind::line},
        {1, 1, 0, 0, 0, 0, GlyphPathCommandKind::line},
        {0, 1, 0, 0, 0, 0, GlyphPathCommandKind::line},
        {0, 0, 0, 0, 0, 0, GlyphPathCommandKind::end_closed},
    };
    GlyphOutline outline{};
    glyph_outline_initialize(&outline, {commands, 5});
    outline.count = 5;
    outline.has_bounds = true;
    outline.maximum_x = outline.maximum_y = 1;
    GlyphRasterBounds bounds{};
    GlyphAtlasAllocation allocation{};
    allocation.x = allocation.y = 4;
    const float poison[4] = {1, 1, 1, 1};
    context->ClearUnorderedAccessViewFloat(gpu.texture_uav, poison);
    D3D11_TEXTURE2D_DESC description{};
    gpu.texture->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    bool valid = SUCCEEDED(device->CreateTexture2D(&description, nullptr, &staging));
    u8 previous[32 * 32]{};
    for (u32 pass = 0; valid && pass < 3; ++pass) {
        const u32 extent = pass == 2 ? 4 : 8;
        const u16 padding = pass == 0 ? 0 : 1;
        valid = glyph_raster_bounds(outline, static_cast<float>(extent), 1, &bounds);
        allocation.width = bounds.width;
        allocation.height = bounds.height;
        valid = valid && glyph_atlas_gpu_rasterize(&gpu, context, outline, bounds, allocation,
                                                   static_cast<float>(extent), padding)
                             .ok();
        if (!valid)
            break;
        context->CopyResource(staging, gpu.texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        valid = SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
        if (!valid)
            break;
        for (u32 y = 0; y < 32; ++y)
            for (u32 x = 0; x < 32; ++x) {
                const u8 actual = static_cast<const u8*>(mapped.pData)[y * mapped.RowPitch + x];
                const bool inside = x >= 4u - padding && y >= 4u - padding &&
                                    x < 4u + bounds.width + padding && y < 4u + bounds.height + padding;
                const bool ink = x >= 5 && y >= 5 && x < 5 + extent && y < 5 + extent;
                const u8 expected = inside ? (ink ? 255 : 0) : pass ? previous[y * 32 + x] : 255;
                valid &= actual == expected;
                previous[y * 32 + x] = actual;
            }
        context->Unmap(staging, 0);
        // The control pass leaves opaque pixels at the sampling border; the
        // real raster clears it, including when a smaller glyph reuses it.
        valid &= previous[4 * 32 + 3] == (pass ? 0 : 255);
    }
    if (staging)
        staging->Release();
    glyph_atlas_gpu_release(&gpu);
    if (valid)
        log_write(LogLevel::info, "probe", "glyph_gutters_poisoned_and_reused_hardware_readback_passed");
    return valid;
}

// Compact message display and group spacing (settings-proto
// message_display_compact plus the local spacing slider).
bool probe_message_display() noexcept {
    auto& scene = g_probe.message_scene;
    const MessageSceneDisplay restore = scene.display;
    const auto apply = [&](MessageSceneDisplay display) noexcept {
        message_scene_set_display(&scene, display);
        return synchronize_probe_shell_layout() && rebuild_probe_content_text() &&
               probe_present(&g_probe.renderer, kDefaultTheme);
    };
    const auto first_header = [&]() noexcept -> const MessageSceneRowWidget* {
        for (u32 i = 0; i < scene.count; ++i) {
            const auto* row = message_scene_layout_node(scene, scene.rows[i].widget);
            if (!scene.rows[i].grouped && !scene.rows[i].forwarded_snapshot && row &&
                ui_rect_has_area(ui_rect_intersection(row->rectangle, scene.viewport)))
                return &scene.rows[i];
        }
        return nullptr;
    };
    const auto command_for = [&](UiWidgetHandle owner) noexcept -> const MessageTextCommand* {
        for (u32 i = 0; i < g_probe.message_text.command_count; ++i) {
            const auto& command = g_probe.message_text.commands[i];
            if (command.owner.manager == &scene.widgets && command.owner.handle.slot == owner.slot &&
                command.owner.handle.generation == owner.generation)
                return &command;
        }
        return nullptr;
    };
    bool valid = apply({0, true});
    const auto* row = valid ? first_header() : nullptr;
    const auto* record = row ? message_store_resolve(g_probe.message_view.store, row->message) : nullptr;
    const auto* user = record ? user_store_find(g_probe.users, record->author_id) : nullptr;
    const Utf8View name = user ? user_store_name(g_probe.users, *user) : Utf8View{};
    const auto* row_node = row ? message_scene_layout_node(scene, row->widget) : nullptr;
    const auto* author = row ? message_scene_layout_node(scene, row->author) : nullptr;
    const auto* content = row ? message_scene_layout_node(scene, row->content) : nullptr;
    const auto* body = row ? command_for(row->content) : nullptr;
    const auto* stamp = row ? command_for(row->avatar) : nullptr;
    const float em = scene.theme_styles && scene.theme_styles->ready ? scene.theme_styles->typography.body_size : 16.f;
    const float indent = content && row_node ? content->rectangle.left - row_node->rectangle.left -
                                                   ui_theme_metrics(scene.theme_styles).message_left
                                             : 0.f;
    valid = valid && row && name.bytes && author && ui_rect_height(author->rectangle) == 0.f &&
            !message_scene_avatar_visible(scene, *row) && indent > em * kMessageSceneCompactIndentEm - .5f &&
            indent < em * kMessageSceneCompactIndentEm + .5f && body && body->text.bytes > name.bytes &&
            bytes_equal(body->text.data, name.data, name.bytes) && body->text.data[name.bytes] == ' ' && stamp &&
            stamp->text.bytes && probe_capture_frame(L"user-settings-compact.bmp");
    if (!valid)
        log_write(LogLevel::error, "probe", "compact_geometry_failed");
    valid = valid && apply({5, true});
    row = valid ? first_header() : nullptr;
    row_node = row ? message_scene_layout_node(scene, row->widget) : nullptr;
    valid = valid && row_node && row_node->spec.style.padding.top >= 25.f &&
            probe_capture_frame(L"user-settings-compact-spacing.bmp");
    valid = valid && apply({1, false});
    row = valid ? first_header() : nullptr;
    row_node = row ? message_scene_layout_node(scene, row->widget) : nullptr;
    author = row ? message_scene_layout_node(scene, row->author) : nullptr;
    valid = valid && row_node && author && ui_rect_height(author->rectangle) > 0.f &&
            message_scene_avatar_visible(scene, *row) && row_node->spec.style.padding.top >= 1.f &&
            row_node->spec.style.padding.top < 2.f && probe_capture_frame(L"user-settings-cozy-tight.bmp");
    const bool restored = apply(restore);
    if (valid)
        log_write(LogLevel::info, "probe", "message_display_compact_and_spacing_passed");
    return valid && restored;
}

bool probe_chat_name_styles() noexcept {
    const auto& scene = g_probe.message_scene;
    const MessageSceneRowWidget* row = nullptr;
    for (u32 i = 0; i < scene.count; ++i)
        if (!scene.rows[i].grouped) {
            const auto* author = message_scene_layout_node(scene, scene.rows[i].author);
            if (author && ui_rect_has_area(ui_rect_intersection(author->rectangle, scene.viewport))) {
                row = &scene.rows[i];
                break;
            }
        }
    if (!row)
        return false;
    const auto* record = message_store_resolve(g_probe.message_view.store, row->message);
    if (!record)
        return false;
    UserAppearance style{};
    const auto* old = user_appearance_find(g_probe.users.appearance, record->author_id);
    if (old)
        user_appearance_expand(*old, &style);
    else
        style.user_id = record->author_id;
    const auto original = style;
    style.fields |= appearance_name;
    style.name.font_id = 14;
    style.name.effect_id = 5;
    style.name.color_count = 1;
    style.name.colors[0] = 0xAB66FF;
    bool valid = user_appearance_upsert(&g_probe.users.appearance, style) &&
                 ui_widget_manager_hover(&g_probe.message_scene.widgets, ui_widget_invalid_handle()) &&
                 rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
                 probe_capture_frame(L"chat-name-static.bmp");
    valid = valid && ui_widget_manager_hover(&g_probe.message_scene.widgets, row->widget) &&
            rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            g_probe.renderer.text_effect_animated && probe_capture_frame(L"chat-name-hover.bmp");
    const auto rebuilt = g_probe.renderer.content_text->rebuilds,
               uploaded = g_probe.renderer.text_instance_uploads;
    Sleep(50);
    valid = valid && probe_present(&g_probe.renderer, kDefaultTheme) &&
            g_probe.renderer.content_text->rebuilds == rebuilt &&
            g_probe.renderer.text_instance_uploads == uploaded;
    auto restore = original;
    restore.fields |= appearance_name;
    valid = user_appearance_upsert(&g_probe.users.appearance, restore) &&
            ui_widget_manager_hover(&g_probe.message_scene.widgets, ui_widget_invalid_handle()) &&
            rebuild_probe_content_text() && valid;
    return valid;
}
bool probe_dm_name_styles() noexcept {
    const ShellNavigationRowWidget* row = nullptr;
    u64 peer = 0;
    for (u32 i = 0; i < g_probe.shell_navigation_scene.count; ++i) {
        const auto& candidate = g_probe.shell_navigation_scene.rows[i];
        const auto* channel = chat_state_store_find_channel(g_probe.chat_state, candidate.view.id);
        if (channel && channel->type == 1 && (peer = private_channel_peer(g_probe.chat_state, channel->id))) {
            row = &candidate;
            break;
        }
    }
    if (!row)
        return false;
    UserAppearance style{};
    const auto* old = user_appearance_find(g_probe.users.appearance, peer);
    if (old)
        user_appearance_expand(*old, &style);
    else
        style.user_id = peer;
    const auto original = style;
    style.fields |= appearance_name;
    style.name.font_id = 14;
    style.name.effect_id = 7;
    style.name.color_count = 2;
    style.name.colors[0] = 0xE076FF;
    style.name.colors[1] = 0x69B5D0;
    bool valid =
        user_appearance_upsert(&g_probe.users.appearance, style) &&
        ui_widget_manager_hover(&g_probe.shell_navigation_scene.widgets, ui_widget_invalid_handle()) &&
        rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
        probe_capture_frame(L"dm-name-static.bmp");
    bool still = false;
    for (u32 i = 0; i < g_probe.renderer.text_effect_count; ++i)
        still |= g_probe.renderer.text_effects.materials[i].kind == 7 &&
                 !g_probe.renderer.text_effects.materials[i].animate;
    valid = valid && still &&
            ui_widget_manager_hover(&g_probe.shell_navigation_scene.widgets, row->widget) &&
            rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            g_probe.renderer.text_effect_animated && probe_capture_frame(L"dm-name-hover.bmp");
    auto restore = original;
    restore.fields |= appearance_name;
    valid = user_appearance_upsert(&g_probe.users.appearance, restore) &&
            ui_widget_manager_hover(&g_probe.shell_navigation_scene.widgets, ui_widget_invalid_handle()) &&
            rebuild_probe_content_text() && valid;
    return valid;
}
bool probe_profile_hover(ProfileScene& scene) noexcept {
    // The full profile's name and username are plain text (as in the official
    // client); the popout's open the full profile.
    if (scene.presentation == ProfilePresentation::modal)
        return true;
    for (u32 kind = 0; kind < 2; ++kind) {
        UiWidgetHandle target{};
        for (u32 i = 0; i < scene.part_count; ++i) {
            const auto& part = scene.parts[i];
            const bool name = part.widget.slot == scene.name_widget.slot &&
                              part.widget.generation == scene.name_widget.generation;
            if (part.action == ProfileAction::expand && part.text.bytes && name == !kind) {
                target = part.widget;
                break;
            }
        }
        const auto* node = ui_widget_manager_layout_node(scene.widgets, target);
        if (!node)
            return false;
        UiRect ink{};
        for (u32 i = 0; i < scene.text_hit_count; ++i)
            if (scene.text_hits[i].owner.slot == target.slot &&
                scene.text_hits[i].owner.generation == target.generation)
                ink = scene.text_hits[i].ink;
        if (!ui_rect_has_area(ink))
            return false;
        const float center_y = (ink.top + ink.bottom) * .5f;
        // Both points are in the name widget, but only one is over painted text
        // (the popout sizes its name and username to their text, so the
        // outside point exists only when the widget has slack).
        const UiPoint outside{ink.right + 3, center_y};
        if (ink.right + 3 < node->rectangle.right &&
            (profile_scene_hit_test(scene, outside.x, outside.y).generation ||
             ui_widget_manager_hit_test(scene.widgets, outside.x, outside.y).slot != target.slot))
            return false;
        ui_button_pointer_target(&scene.widgets, UiButtonPointerEvent::move,
                                 profile_scene_hit_test(scene, outside.x, outside.y));
        UiTextCommand outside_commands[320]{};
        UiTextCommandStream outside_stream{};
        if (!ui_text_command_stream_initialize(&outside_stream, {outside_commands, 320}) ||
            !profile_scene_text(scene, kDefaultTheme, &outside_stream))
            return false;
        for (u32 i = 0; i < outside_stream.count; ++i)
            if (outside_commands[i].owner.handle.slot == target.slot &&
                (outside_commands[i].style_flags & text_style_underline))
                return false;
        if (ink.top > node->rectangle.top + 1 &&
            profile_scene_hit_test(scene, (ink.left + ink.right) * .5f, ink.top - 1).generation)
            return false;
        const auto moved =
            ui_button_pointer_target(&scene.widgets, UiButtonPointerEvent::move,
                                     profile_scene_hit_test(scene, (ink.left + ink.right) * .5f, center_y));
        if (!moved.changed || moved.target.slot != target.slot)
            return false;
        ++scene.revision;
        UiTextCommand commands[320]{};
        UiTextCommandStream stream{};
        if (!ui_text_command_stream_initialize(&stream, {commands, 320}) ||
            !profile_scene_text(scene, kDefaultTheme, &stream))
            return false;
        bool underline = false;
        for (u32 i = 0; i < stream.count; ++i)
            if (commands[i].owner.handle.slot == target.slot)
                underline |= (commands[i].style_flags & text_style_underline) != 0;
        if (!underline || !rebuild_probe_content_text() ||
            !probe_present(&g_probe.renderer, kDefaultTheme) ||
            !probe_capture_frame(kind ? L"profile-inline-hit-username-hover.bmp"
                                      : L"profile-inline-hit-name-hover.bmp"))
            return false;
    }
    ui_button_pointer(&scene.widgets, UiButtonPointerEvent::move,
                      {scene.rectangle.left - 1, scene.rectangle.top});
    ++scene.revision;
    const bool passed = rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme);
    if (passed)
        log_write(LogLevel::info, "probe", "profile_inline_native_ink_hover_inside_outside_passed");
    return passed;
}

#include "thai_wrapped_probe.inc"
#include "catalog_apng_probe.inc"
#include "lottie_animation_probe.inc"
#include "lottie_probe.inc"
bool probe_transparent_guild_icon() noexcept {
    g_probe.failure_stage = 271;
    if (!chat_state_store_upsert_guild(&g_probe.chat_state,
            {200, {}, GuildMutationKind::update, 0,
             utf8_literal("abcdefabcdefabcdefabcdefabcdefab"), true}).ok() ||
        !navigation_view_rebuild(&g_probe.navigation, g_probe.chat_state).ok() ||
        !probe_sync_shell_and_text())
        return false;
    const auto* guild = chat_state_store_find_guild(g_probe.chat_state, 200);
    if (!guild) return false;
    u8 pixels[32 * 32 * 4]{};
    for (u32 y = 0; y < 32; ++y)
        for (u32 x = 0; x < 32; ++x) {
            const u32 at = (y * 32 + x) * 4;
            const u32 alpha = x < 12 ? 0 : x < 20 ? 255 : 128;
            // Media uploads consume the decoder's premultiplied RGBA output.
            pixels[at] = static_cast<u8>((240 * alpha + 127) / 255);
            pixels[at + 1] = static_cast<u8>((220 * alpha + 127) / 255);
            pixels[at + 2] = static_cast<u8>((20 * alpha + 127) / 255);
            pixels[at + 3] = static_cast<u8>(alpha);
        }
    if (!renderer_upload_media_rgba8(&g_probe.renderer,
            media_asset_guild_icon(200, guild->icon_variant, false), 32, 32, 128,
            {pixels, sizeof(pixels)}).ok())
        return false;
    GuildRailRowWidget* row = nullptr;
    for (u32 i = 0; i < g_probe.guild_rail_scene.count; ++i)
        if (g_probe.guild_rail_scene.rows[i].view.id == 200) row = &g_probe.guild_rail_scene.rows[i];
    const auto* layout = row ? guild_rail_scene_layout_node(g_probe.guild_rail_scene, *row) : nullptr;
    if (!layout) return false;
    const UiRect icon = layout->rectangle;
    const float width = ui_rect_width(icon), y = (icon.top + icon.bottom) * .5f;
    const Color art{240.f / 255, 220.f / 255, 20.f / 255, 1};
    const wchar_t* captures[]{L"transparent-guild-idle.bmp", L"transparent-guild-hover.bmp",
        L"transparent-guild-selected.bmp", L"colored-transparent-guild-idle.bmp",
        L"colored-transparent-guild-hover.bmp", L"colored-transparent-guild-selected.bmp"};
    for (u32 color = 0; color < 2; ++color) {
        Theme theme = kDefaultTheme;
        if (color) theme.base_lowest = {.04f, .25f, .30f, 1};
        const Color back = theme.base_lowest;
        const float alpha = 128.f / 255;
        const Color blend{art.red * alpha + back.red * (1 - alpha),
            art.green * alpha + back.green * (1 - alpha),
            art.blue * alpha + back.blue * (1 - alpha), 1};
        const ProbePixelSample samples[]{
            {{icon.left + width * .25f, y}, back, 3},
            {{icon.left + width * .50f, y}, art, 3},
            {{icon.left + width * .75f, y}, blend, 3}};
        for (u32 state = 0; state < 3; ++state) {
            row->view.selected = state == 2;
            ui_widget_manager_hover(&g_probe.guild_rail_scene.widgets,
                state == 1 ? row->widget : ui_widget_invalid_handle());
            if (!probe_present(&g_probe.renderer, theme) || !probe_exact_pixels({samples, 3}) ||
                !probe_capture_frame(captures[color * 3 + state]))
                return false;
        }
    }
    // Removing artwork must restore the fallback even if the old texture is
    // still resident in the atlas.
    if (!chat_state_store_upsert_guild(&g_probe.chat_state,
            {200, {}, GuildMutationKind::update, 0, {}, true}).ok() ||
        !navigation_view_rebuild(&g_probe.navigation, g_probe.chat_state).ok() ||
        !probe_sync_shell_and_text())
        return false;
    const ProbePixelSample fallback{{icon.left + width * .5f, y}, kDefaultTheme.surface_high, 3};
    return probe_present(&g_probe.renderer, kDefaultTheme) && probe_exact_pixels({&fallback, 1}) &&
        probe_capture_frame(L"guild-icon-removed-fallback.bmp");
}
bool probe_braille_profile() noexcept {
    g_probe.failure_stage = 270;
    UserProfile data{};
    data.user_id = 4242;
    const auto name = utf8_literal("Heroic");
    copy_bytes(data.username.data, name.data, name.bytes);
    data.username.bytes = static_cast<u16>(name.bytes);
    // 44 braille cells and seven line breaks: the failing bio's 139 UTF-8
    // bytes / 51 UTF-16 units. Include the blank braille cell.
    constexpr u8 cells[]{0x0f,0xff,0xff,0x39, 0x1f,0xff,0xff,0xff,0xff,0x3b,
        0x3f,0x00,0xff,0xff,0x00,0x3f, 0x3f,0x00,0xff,0xff,0x00,0x3f,
        0x3f,0xff,0x00,0x00,0xff,0x3f, 0x3f,0xff,0x0e,0x07,0xff,0x3f,
        0x08,0xff,0x36,0x36,0xff,0x01, 0x00,0x36,0x36,0x00};
    constexpr u8 widths[]{4,6,6,6,6,6,6,4};
    u32 cell = 0;
    for (u32 row = 0; row < 8; ++row) {
        if (row) data.bio.data[data.bio.bytes++] = '\n';
        for (u32 col = 0; col < widths[row]; ++col) {
            const u32 cp = 0x2800 + cells[cell++];
            data.bio.data[data.bio.bytes++] = static_cast<char>(0xe0 | (cp >> 12));
            data.bio.data[data.bio.bytes++] = static_cast<char>(0x80 | ((cp >> 6) & 63));
            data.bio.data[data.bio.bytes++] = static_cast<char>(0x80 | (cp & 63));
        }
    }
    ProfileScene scene{};
    scene.phase = ProfilePhase::ready;
    scene.measure = &g_probe.text_measure_cache;
    const UiRect viewport{0, 0, static_cast<float>(g_probe.renderer.width) * 96 / g_probe.renderer.dpi,
        static_cast<float>(g_probe.renderer.height) * 96 / g_probe.renderer.dpi};
    g_probe.renderer.profile_scene = &scene;
    const UiRect shell_labels[]{
        ui_scene_content_rectangle(g_probe.shell_scene, UiSemantic::channel_header),
        ui_scene_content_rectangle(g_probe.shell_scene, UiSemantic::title_label)};
    bool valid = data.bio.bytes == 139 && profile_scene_open(&scene, &data, &g_probe.chat_state,
        &g_probe.users, 1, {viewport.right - 100, 90, viewport.right, 134}, viewport);
    for (u32 pass = 0; valid && pass < 4; ++pass) {
        if (pass == 2) {
            profile_scene_release(&scene);
            scene.phase = ProfilePhase::ready;
            scene.measure = &g_probe.text_measure_cache;
            valid = profile_scene_open(&scene, &data, &g_probe.chat_state, &g_probe.users, 1,
                {viewport.right - 100, 90, viewport.right, 134}, viewport);
        }
        scene.presentation = (pass & 1) != 0 ? ProfilePresentation::modal : ProfilePresentation::popout;
        valid = profile_scene_layout(&scene, viewport, true) && scene.part_count > 5 &&
            utf8_equal(scene.bio_display.view(), data.bio.view()) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(pass & 1 ? (pass == 1 ? L"braille-profile-expanded.bmp" : L"braille-profile-expanded-warm.bmp")
                : (pass == 0 ? L"braille-profile.bmp" : L"braille-profile-warm.bmp"));
        const auto* node = ui_widget_manager_layout_node(scene.widgets, scene.bio_widget);
        valid = valid && node && probe_text_pixels({&node->rectangle, 1});
        if (scene.presentation == ProfilePresentation::popout)
            valid = valid && probe_text_pixels({shell_labels, 2});
    }
    // Inject a biography-only measurement failure while the card is open.
    // Verify real GPU text in the recovery card and the unaffected shell.
    UiTextMeasureRecord records[16]{};
    UiTextMeasureCache failed_measure{};
    const auto backend = [](void* context, const UiTextMeasureRequest& request,
                            UiTextMeasureValue* value) noexcept {
        auto& original = *static_cast<UiTextMeasureCache*>(context);
        return request.key.source_id != 0x42494F &&
            original.backend(original.backend_context, request, value);
    };
    scene.presentation = ProfilePresentation::popout;
    scene.measure = &failed_measure;
    valid = valid && ui_text_measure_cache_initialize(&failed_measure, {records, 16}, backend,
        &g_probe.text_measure_cache) && profile_scene_layout(&scene, viewport, true) &&
        utf8_equal(scene.bio_display.view(), utf8_literal("About me unavailable")) &&
        rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
        probe_text_pixels({shell_labels, 2}) && probe_capture_frame(L"braille-profile-failure-contained.bmp");
    const auto* recovered_bio = ui_widget_manager_layout_node(scene.widgets, scene.bio_widget);
    valid = valid && recovered_bio && probe_text_pixels({&recovered_bio->rectangle, 1});
    scene.measure = &g_probe.text_measure_cache;
    valid = valid && profile_scene_layout(&scene, viewport, true) &&
        utf8_equal(scene.bio_display.view(), data.bio.view()) && rebuild_probe_content_text() &&
        probe_present(&g_probe.renderer, kDefaultTheme) && probe_text_pixels({shell_labels, 2}) &&
        probe_capture_frame(L"braille-profile-recovered.bmp");
    profile_scene_release(&scene);
    g_probe.renderer.profile_scene = nullptr;
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
        probe_text_pixels({shell_labels, 2}) &&
        probe_capture_frame(L"braille-profile-closed.bmp");
    return valid;
}
bool probe_profile_effect_layers(ProfileScene& scene) noexcept {
    // Original solid artwork makes every glyph leaking over the effect
    // observable in a native render-target readback, without saved captures.
    ProfileEffect effect{};
    effect.sku = 987654;
    effect.count = 1;
    effect.layers[0].loop = true;
    effect.layers[0].duration_ms = 60000;
    const auto source = utf8_literal("https://example.invalid/native-profile-effect.png");
    copy_bytes(effect.layers[0].source.data, source.data, source.bytes);
    effect.layers[0].source.bytes = static_cast<u16>(source.bytes);
    const auto* saved_effect = scene.effect;
    const u64 saved_started = scene.effect_started_ms;
    const u64 saved_hover_changed = scene.hover_changed_ms;
    const bool saved_hovered = scene.hovered;
    const auto saved_presentation = scene.presentation;
    scene.effect = &effect;
    scene.effect_started_ms = GetTickCount64();
    scene.hovered = false;
    scene.hover_changed_ms = 0;
    ProfileMedia media{};
    u8 pixels[2 * 8 * 4];
    for (u32 i = 0; i < sizeof(pixels); i += 4) {
        pixels[i] = pixels[i + 2] = pixels[i + 3] = 255;
        pixels[i + 1] = 0;
    }
    bool valid =
        profile_effect_media_resolve(scene, 0, scene.effect_started_ms, &media) &&
        renderer_upload_media_rgba8(&g_probe.renderer, media.key, 2, 8, 8, {pixels, sizeof(pixels)}).ok();
    for (u32 mode = 0; valid && mode < 2; ++mode) {
        scene.presentation = mode ? ProfilePresentation::modal : ProfilePresentation::popout;
        scene.effect = nullptr;
        valid = profile_scene_layout(&scene, scene.viewport, true) && rebuild_probe_content_text() &&
                probe_present(&g_probe.renderer, kDefaultTheme);
        const auto* name = ui_widget_manager_layout_node(scene.widgets, scene.name_widget);
        const auto r = name ? ui_rect_intersection(name->rectangle, name->clip_rectangle) : UiRect{};
        const auto mask = profile_scene_card_mask(scene, kDefaultTheme);
        const UiPoint corners[]{{mask.rectangle.left + .5f, mask.rectangle.top + .5f},
                                {mask.rectangle.left - 1, mask.rectangle.top + mask.radius}};
        u8 below[2][4]{}, above[2][4]{};
        valid = valid && ui_rect_has_area(r) && probe_text_pixels({&r, 1}) &&
                probe_read_pixels(kDefaultTheme, {corners, 2}, below);
        scene.effect = &effect;
        ++scene.revision;
        valid = valid && probe_present(&g_probe.renderer, kDefaultTheme) &&
                probe_read_pixels(kDefaultTheme, {corners, 2}, above) &&
                bytes_equal(below, above, sizeof(below));
        ProbePixelSample samples[32 * 12];
        for (u32 y = 0; y < 12; ++y)
            for (u32 x = 0; x < 32; ++x)
                samples[y * 32 + x] = {{r.left + (x + .5f) * ui_rect_width(r) / 32,
                                        r.top + (y + .5f) * ui_rect_height(r) / 12},
                                       {1, 0, 1, 1},
                                       2};
        valid =
            valid && ui_rect_has_area(r) &&
            g_probe.renderer.profile_effect_media_first_batch >
                g_probe.renderer.profile_media_first_batch &&
            g_probe.renderer.profile_effect_media_first_batch < g_probe.renderer.profile_media_end_batch &&
            g_probe.renderer.profile_controls_first_batch >
                g_probe.renderer.profile_foreground_first_batch &&
            probe_exact_pixels({samples, 32 * 12});
        // The top controls must survive the effect, while avatar media stays below.
        const auto avatar = scene.avatar;
        const ProbePixelSample avatar_sample{
            {(avatar.left + avatar.right) * .5f, (avatar.top + avatar.bottom) * .5f}, {1, 0, 1, 1}, 2};
        valid = valid && probe_exact_pixels({&avatar_sample, 1});
        bool control_visible = false;
        for (u32 i = 0; valid && i < scene.part_count; ++i) {
            const auto& p = scene.parts[i];
            if (p.look != ProfileLook::header_button)
                continue;
            const auto* node = ui_widget_manager_layout_node(scene.widgets, p.widget);
            if (!node)
                continue;
            const UiPoint point{(node->rectangle.left + node->rectangle.right) * .5f,
                                (node->rectangle.top + node->rectangle.bottom) * .5f};
            u8 color[1][4]{};
            valid = probe_read_pixels(kDefaultTheme, {&point, 1}, color);
            control_visible |= color[0][1] > 2 || color[0][0] < 253 || color[0][2] < 253;
        }
        valid = valid && control_visible;
    }
    // A later modal must cover even an opaque profile effect.
    TextFormScene form{};
    TextFormSurface surface{};
    TextFormContent content{};
    content.title = utf8_literal("Profile overlay");
    content.context = utf8_literal("This form appears above the profile effect.");
    content.cancel = utf8_literal("Close");
    content.accept = utf8_literal("Done");
    content.confirmation_only = true;
    auto* saved_form = g_probe.renderer.text_form;
    valid = valid && text_form_update(&form, content, scene.viewport, &g_probe.text_measure_cache);
    surface.scene = &form;
    renderer_bind_text_form(&g_probe.renderer, &surface);
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme);
    const auto* panel = text_form_node(form, TextFormElement::panel);
    const auto overlap = panel
                             ? ui_rect_intersection(panel->rectangle,
                                                    profile_scene_card_mask(scene, kDefaultTheme).rectangle)
                             : UiRect{};
    const UiPoint covered{(overlap.left + overlap.right) * .5f, (overlap.top + overlap.bottom) * .5f};
    u8 covered_pixel[1][4]{};
    valid = valid && ui_rect_width(overlap) > kDefaultTheme.scale.radius_md * 2 &&
            ui_rect_height(overlap) > kDefaultTheme.scale.radius_md * 2 &&
            probe_read_pixels(kDefaultTheme, {&covered, 1}, covered_pixel) &&
            (covered_pixel[0][1] > 2 || covered_pixel[0][0] < 253 || covered_pixel[0][2] < 253);
    renderer_bind_text_form(&g_probe.renderer, saved_form);
    text_form_surface_release(&surface);
    text_form_release(&form);
    scene.effect = saved_effect;
    scene.effect_started_ms = saved_started;
    scene.hover_changed_ms = saved_hover_changed;
    scene.hovered = saved_hovered;
    scene.presentation = saved_presentation;
    const bool restored = profile_scene_layout(&scene, scene.viewport, true) &&
                          rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme);
    if (valid && restored)
        log_write(LogLevel::info, "probe", "profile_effect_above_glyphs_avatar_below_controls_passed");
    return valid && restored;
}
bool probe_profile_card() noexcept {
    g_probe.failure_stage = 151;
    auto* data = static_cast<UserProfile*>(
        VirtualAlloc(nullptr, sizeof(UserProfile), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!data)
        return false;
    u32 scratch[2048];
    const auto json = utf8_literal(
        R"({"user":{"id":"4242","username":"native.test","global_name":"Native Profile","avatar":"abc123","display_name_styles":{"font_id":14,"effect_id":5,"colors":[16736371,6879487]}},"user_profile":{"bio":"A profile card with a wrapped biography, separate guild appearance and account badges. All images use the shared asynchronous media cache.","pronouns":"they/them","theme_colors":[16711680,65535],"banner":"a_testbanner"},"guild_member":{"nick":"Robin-san","roles":["55","56","57"],"joined_at":"2021-03-14T12:00:00.000Z"},"mutual_guilds":[{"id":"1","name":"Native test server"}],"mutual_friends":[{"id":"2","username":"Friend"}],"premium_type":2,"badges":[{"id":"premium","icon":"abcdef","description":"Nitro subscriber"}]})");
    bool valid = user_profile_decode(json, {scratch, 2048}, 4242, 100, data);
    data->user_appearance.primary_enabled = true;
    data->user_appearance.primary_guild_id = 100;
    data->user_appearance.fields |= appearance_guild_tag;
    copy_bytes(data->user_appearance.primary_tag.data, "GEAR", 4);
    data->user_appearance.primary_tag.bytes = 4;
    copy_bytes(data->user_appearance.primary_badge.data, "abc123", 6);
    data->user_appearance.primary_badge.bytes = 6;
    valid =
        valid &&
        chat_state_store_upsert_role(&g_probe.chat_state,
                                        {56, 100, 0, utf8_literal("HELPER"), 1, 0xB88A22}) &&
        chat_state_store_upsert_role(
            &g_probe.chat_state,
            {57, 100, 0,
             utf8_literal("A very long role name that must fit the column and truncate only at its edge"),
             1, 0x5588AA});
    valid = valid && chat_state_store_upsert_role(
                         &g_probe.chat_state, {58, 100, 0, utf8_literal("Assignable role"), 1, 0x5588AA});
    auto* guild = const_cast<GuildRecord*>(chat_state_store_find_guild(g_probe.chat_state, 100));
    const u64 previous_owner = guild ? guild->owner_id : 0;
    if (guild)
        guild->owner_id = g_probe.chat_state.self_user_id;
    ProfileScene scene{};
    scene.phase = ProfilePhase::ready;
    scene.measure = &g_probe.text_measure_cache;
    const UiRect viewport{0, 0, static_cast<float>(g_probe.renderer.width) * 96 / g_probe.renderer.dpi,
                          static_cast<float>(g_probe.renderer.height) * 96 / g_probe.renderer.dpi};
    valid = valid && user_appearance_upsert(&g_probe.users.appearance, data->user_appearance) &&
            profile_scene_open(&scene, data, &g_probe.chat_state, &g_probe.users, 1,
                               {viewport.right - 100, 90, viewport.right, 134}, viewport);
    PngImage image{};
    u8 pixels[32 * (32 * 4 + 1)]{};
    valid = valid && png_image_configure(&image, 32, 32, 8, 6);
    for (u32 y = 0; y < 32; ++y)
        for (u32 x = 0; x < 32; ++x) {
            const u32 at = y * 129 + 1 + x * 4;
            pixels[at] = static_cast<u8>(x * 7);
            pixels[at + 1] = static_cast<u8>(y * 7);
            pixels[at + 2] = 180;
            pixels[at + 3] = 255;
        }
    for (u32 i = 0; valid && i < scene.part_count; ++i) {
        ProfileMedia media{};
        if (profile_media_resolve(scene, i, &media))
            valid =
                renderer_upload_png_scanlines(&g_probe.renderer, media.key, image, {pixels, sizeof(pixels)})
                    .ok();
    }
    const auto saved_banner = data->banner, saved_guild_banner = data->guild_banner;
    data->guild_banner = {};
    copy_bytes(data->banner.data, "staticbanner", 12);
    data->banner.bytes = 12;
    bool static_banner = false, animated_banner = false, gif_banner = false;
    for (u32 i = 0; i < scene.part_count; ++i)
        if (scene.parts[i].media == ProfileMediaKind::banner) {
            ProfileMedia check{};
            static_banner = profile_media_resolve(scene, i, &check) &&
                            media_asset_embed_format(check.key) == MediaAttachmentFormat::png;
            copy_bytes(data->banner.data, "a_animatedbanner", 16);
            data->banner.bytes = 16;
            animated_banner = profile_media_resolve(scene, i, &check) &&
                              media_asset_embed_format(check.key) == MediaAttachmentFormat::webp &&
                              (check.key.flags & media_asset_flag_animated);
            valid = valid && profile_media_use_compatibility_banner(&scene);
            gif_banner = profile_media_resolve(scene, i, &check) &&
                         media_asset_embed_format(check.key) == MediaAttachmentFormat::gif &&
                         (check.key.flags & media_asset_flag_animated);
        }
    valid = valid && static_banner && animated_banner && gif_banner;
    scene.banner_compatibility = false;
    data->banner = saved_banner;
    data->guild_banner = saved_guild_banner;
    scene.presence = UiPresence::busy;
    scene.edit = {utf8_literal("test test test"), 5, 9, true, true, true};
    ++scene.revision;
    g_probe.renderer.profile_scene = &scene;
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"member-profile-card.bmp");
    valid = valid && probe_profile_effect_layers(scene);
    const wchar_t* const profile_motion_captures[]{L"profile-motion-zero.bmp", L"profile-motion-half.bmp", L"profile-motion-full.bmp"};
    const auto profile_motion_rectangle = scene.rectangle;
    const auto motion_saved_user_name = data->user_appearance.name;
    const auto motion_saved_guild_name = data->guild_appearance.name;
    const auto motion_saved_edit = scene.edit;
    data->user_appearance.name.effect_id = 0;
    data->guild_appearance.name.effect_id = 0;
    valid = valid && user_appearance_upsert(&g_probe.users.appearance, data->user_appearance);
    if (data->guild_appearance.user_id)
        valid = valid && user_appearance_upsert(&g_probe.users.appearance, data->guild_appearance);
    scene.edit = {};
    ++scene.revision;
    // The shared pixel check identifies original solid-magenta media ink.
    u8 motion_pixels[sizeof(pixels)]{};
    for (u32 y = 0; y < 32; ++y)
        for (u32 x = 0; x < 32; ++x) {
            const u32 at = y * 129 + 1 + x * 4;
            motion_pixels[at] = 255;
            motion_pixels[at + 2] = 220;
            motion_pixels[at + 3] = 255;
        }
    for (u32 i = 0; valid && i < scene.part_count; ++i) {
        ProfileMedia media{};
        if (profile_media_resolve(scene, i, &media))
            valid = renderer_upload_png_scanlines(&g_probe.renderer, media.key, image,
                                                  {motion_pixels, sizeof(motion_pixels)}).ok();
    }
    valid = valid && surface_motion_probe_detail::measure(g_probe.renderer, RendererLayerSurface::profile,
        scene.rectangle, true, profile_motion_captures, [&]() noexcept {
            return scene.active && ui_rect_equal(scene.rectangle, profile_motion_rectangle);
        });
    data->user_appearance.name = motion_saved_user_name;
    data->guild_appearance.name = motion_saved_guild_name;
    valid = valid && user_appearance_upsert(&g_probe.users.appearance, data->user_appearance);
    if (data->guild_appearance.user_id)
        valid = valid && user_appearance_upsert(&g_probe.users.appearance, data->guild_appearance);
    scene.edit = motion_saved_edit;
    ++scene.revision;
    for (u32 i = 0; valid && i < scene.part_count; ++i) {
        ProfileMedia media{};
        if (profile_media_resolve(scene, i, &media))
            valid = renderer_upload_png_scanlines(&g_probe.renderer, media.key, image, {pixels, sizeof(pixels)}).ok();
    }
    // Exercise an actual painted Arabic line, including a join control, through
    // the same profile-bio path used by the application. Restore the fixture
    // before the existing interaction/retention probes continue.
    const auto arabic_saved_bio = data->bio;
    const auto arabic_bio =
        utf8_literal("\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85 \xd8\xa8\xd8\xa8 \xd8\xa8\xe2\x80\x8d\xd8\xa8");
    const auto arabic_before = native_text_candidate_counts(g_probe.renderer.glyph_text.system);
    copy_bytes(data->bio.data, arabic_bio.data, arabic_bio.bytes);
    data->bio.bytes = static_cast<u16>(arabic_bio.bytes);
    valid = valid && profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme) && probe_capture_frame(L"arabic-profile.bmp");
    const auto arabic_after = native_text_candidate_counts(g_probe.renderer.glyph_text.system);
    // Bios carry text-sm style spans, so a plain Arabic bio shapes as a
    // styled native paragraph.
    valid = valid && (arabic_after.arabic_shaped > arabic_before.arabic_shaped ||
                      arabic_after.paragraphs_shaped > arabic_before.paragraphs_shaped);
    // Paint the native Hebrew path through the real profile-bio renderer.
    // Include a repeated vowel and reordered shin marks in the captured image.
    const auto hebrew_bio = utf8_literal("\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\x90\xd6\xb0\xd6\xb0 "
                                         "\xd7\xa9\xd6\xb8\xd6\xbc\xd7\x81 \xd7\x95\xd6\xb9");
    auto hebrew_system = g_probe.renderer.glyph_text.system;
    TextGlyph hebrew_glyphs[128];
    TextRun hebrew_runs[16];
    u16 hebrew_clusters[128];
    ShapedText hebrew_output{};
    wchar_t hebrew_utf16[128];
    valid = valid &&
            shaped_text_initialize(&hebrew_output, {hebrew_glyphs, 128}, {hebrew_runs, 16},
                                   {hebrew_clusters, 128}) &&
            text_shape_utf8(&hebrew_system, hebrew_bio, 10000, 10000, {hebrew_utf16, 128}, &hebrew_output)
                .ok() &&
            hebrew_output.native_fonts();
    shaped_text_reset(&hebrew_output);
    const auto hebrew_before = native_text_candidate_counts(g_probe.renderer.glyph_text.system);
    copy_bytes(data->bio.data, hebrew_bio.data, hebrew_bio.bytes);
    data->bio.bytes = static_cast<u16>(hebrew_bio.bytes);
    valid = valid && profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme) && probe_capture_frame(L"hebrew-profile.bmp");
    valid = valid && native_text_candidate_counts(g_probe.renderer.glyph_text.system).paragraphs_shaped >
                         hebrew_before.paragraphs_shaped;
    data->bio.bytes = 0;
    for (u32 repeat = 0; repeat < 8; ++repeat) {
        copy_bytes(data->bio.data + data->bio.bytes, hebrew_bio.data, hebrew_bio.bytes);
        data->bio.bytes += static_cast<u16>(hebrew_bio.bytes);
        data->bio.data[data->bio.bytes++] = ' ';
    }
    TextGlyph wrapped_glyphs[512];
    TextRun wrapped_runs[128];
    u16 wrapped_clusters[512];
    ShapedText wrapped_output{};
    wchar_t wrapped_utf16[512];
    valid = valid &&
            shaped_text_initialize(&wrapped_output, {wrapped_glyphs, 512}, {wrapped_runs, 128},
                                   {wrapped_clusters, 512}) &&
            text_shape_utf8(&hebrew_system, {data->bio.data, data->bio.bytes}, 180, 10000,
                            {wrapped_utf16, 512}, &wrapped_output)
                .ok() &&
            wrapped_output.native_fonts() && wrapped_output.line_count > 1;
    shaped_text_reset(&wrapped_output);
    const auto wrapped_before = native_text_candidate_counts(g_probe.renderer.glyph_text.system);
    valid = valid && profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"hebrew-wrapped-profile.bmp");
    valid = valid && native_text_candidate_counts(g_probe.renderer.glyph_text.system).paragraphs_shaped >
                         wrapped_before.paragraphs_shaped;
    const auto mixed_bio = utf8_literal(
        "Hello (\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d) 123 \xce\xb1 \xd0\x91 \xd7\x90\xd6\xb0\xd6\xb0 world ");
    data->bio.bytes = 0;
    for (u32 repeat = 0; repeat < 2; ++repeat) {
        copy_bytes(data->bio.data + data->bio.bytes, mixed_bio.data, mixed_bio.bytes);
        data->bio.bytes += static_cast<u16>(mixed_bio.bytes);
    }
    valid = valid && profile_scene_layout(&scene, viewport, true);
    // Validate the exact draw command, including the real ellipsis bounds.
    // A long unconstrained preflight can succeed while the actual bio falls
    // back for height trimming, so it cannot prove native drawing admission.
    UiTextCommand mixed_commands[320]{};
    UiTextCommandStream mixed_stream{};
    TextDecoration bio_decorations[128];
    TextHitRegion bio_hits[256];
    valid = valid && ui_text_command_stream_initialize(&mixed_stream, {mixed_commands, 320}) &&
            shaped_text_bind_decorations(&wrapped_output, {bio_decorations, 128}) &&
            shaped_text_bind_hit_regions(&wrapped_output, {bio_hits, 256});
    const auto native_bio_command = [&](bool expect_height_trim = false) noexcept {
        ui_text_command_stream_reset(&mixed_stream);
        if (!profile_scene_text(scene, kDefaultTheme, &mixed_stream))
            return false;
        bool found = false;
        for (u32 i = 0; i < mixed_stream.count; ++i) {
            const auto& command = mixed_commands[i];
            if (command.owner.handle.slot != scene.bio_widget.slot)
                continue;
            found = true;
            TextHitSpan hits[64];
            for (u32 h = 0; h < scene.bio_semantic_count; ++h) {
                const auto& semantic = scene.bio_semantics[h];
                hits[h] = {semantic.byte_offset, semantic.byte_count, static_cast<u16>(h), 0};
            }
            const bool native =
                command.face == UiTextFace::normal && !command.effect.kind && !command.style_flags &&
                text_shape_utf8_interactive(
                    &hebrew_system, command.text, command.styles, {hits, scene.bio_semantic_count},
                    ui_rect_width(command.rectangle), ui_rect_height(command.rectangle),
                    {wrapped_utf16, 512}, &wrapped_output,
                    command.wrapping == UiTextWrapping::wrap ? TextWrapping::wrap : TextWrapping::no_wrap,
                    command.overflow == UiTextOverflow::ellipsis ? TextOverflow::ellipsis
                                                                 : TextOverflow::clip)
                    .ok() &&
                wrapped_output.native_fonts() && wrapped_output.line_count > 1 &&
                (!expect_height_trim || wrapped_output.glyph_count < 16) &&
                (!scene.bio_semantic_count || wrapped_output.hit_region_count);
            if (!native)
                log_u64(LogLevel::error, "probe", "native_bio_preflight_lines", wrapped_output.line_count);
            shaped_text_reset(&wrapped_output);
            if (!native)
                return false;
        }
        return found;
    };
    valid = valid && native_bio_command();
    const auto mixed_before = native_text_candidate_counts(g_probe.renderer.glyph_text.system);
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"mixed-bidi-profile.bmp");
    valid = valid && native_text_candidate_counts(g_probe.renderer.glyph_text.system).paragraphs_shaped >
                         mixed_before.paragraphs_shaped;
    const auto decorated_bio = utf8_literal(
        "Hello **\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d** *world* __\xd7\x90\xd7\x91__ ~~xyz~~ `\xd7\x90 abc` ");
    data->bio.bytes = 0;
    for (u32 repeat = 0; repeat < 2; ++repeat) {
        copy_bytes(data->bio.data + data->bio.bytes, decorated_bio.data, decorated_bio.bytes);
        data->bio.bytes += static_cast<u16>(decorated_bio.bytes);
    }
    valid = valid && profile_scene_layout(&scene, viewport, true) && scene.bio_style_count >= 5;
    valid = valid && native_bio_command();
    const auto decorated_before = native_text_candidate_counts(g_probe.renderer.glyph_text.system);
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"mixed-bidi-styles-profile.bmp");
    valid = valid && native_text_candidate_counts(g_probe.renderer.glyph_text.system).paragraphs_shaped >
                         decorated_before.paragraphs_shaped;
    const Utf8View script_bios[]{
        utf8_literal(
            "(**\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7) 12 \\[\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85\\]**"),
        utf8_literal("`(\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7) 12 [\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85]`"),
        utf8_literal("\x2a\x2a\xe4\xb8\xad\xe6\x96\x87\x2a\x2a\x20\xe3\x82\xab\xe3\x82\x99\x20\xef\xbd\xb6"
                     "\xef\xbe\x9e"),
        utf8_literal("\x2a\x2a\xe4\xb8\xad\xf0\xa0\x80\x80\xe6\x96\x87\x2a\x2a\x20\xf0\x9f\x98\x80"),
        utf8_literal("\x2a\x2a\xe3\x82\xab\xe2\x80\x8d\xe3\x82\x99\x20\xe3\x82\xab\xe2\x80\x8c\xe3\x82\x99"
                     "\x20\xe3\x82\xab\xcd\x8f\xe3\x82\x99\x2a\x2a\x20\xef\xbd\xb6\xef\xbe\x9e"),
        utf8_literal("\x2a\x2a\xe4\xb8\xad\xe6\x96\x87\x2a\x2a")};
    wchar_t long_bio_option[2]{};
    const bool long_cjk_bio = GetEnvironmentVariableW(L"MAICORD_CJK_LONG_BIO", long_bio_option, 2) == 1 &&
                              long_bio_option[0] == '1';
    const Utf8View long_cjk_bios[]{
        utf8_literal(
            "\x2a\x2a\xe4\xb8\xad\xe6\x96\x87\xe6\xb6\x88\xe6\x81\xaf\xe6\xb5\x8b\xe8\xaf\x95\x2a\x2a\x20"
            "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\xa1\xe3\x83\x83\xe3\x82\xbb\xe3\x83"
            "\xbc\xe3\x82\xb8\x0a\xe3\x81\x8b\xe3\x82\x99\x20\xe3\x81\xaf\xe3\x82\x9a\x20\xe3\x82\xab\xe3"
            "\x82\x99\x20\xe3\x83\x8f\xe3\x82\x9a\x20\xef\xbd\xb6\xef\xbe\x9e\x20\xef\xbe\x8a\xef\xbe\x9f"),
        utf8_literal("\x2a\x2a\xe4\xb8\xad\xe6\x96\x87\xf0\xa0\x80\x80\xe4\xb8\xad\xe6\x96\x87\x2a\x2a\x20"
                     "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\x0a\xe3\x82\xab\xe3\x83\x8a\x20\xf0\x9f\x98\x80"
                     "\x20\xe4\xb8\xad\xe6\x96\x87")};
    const wchar_t* script_captures[]{L"arabic-bracket-styles-profile.bmp",
                                     L"arabic-bracket-mono-profile.bmp",
                                     L"cjk-styles-profile.bmp",
                                     L"cjk-supplementary-profile.bmp",
                                     L"cjk-controls-profile.bmp",
                                     L"height-overflow-profile.bmp"};
    for (u32 i = 0; valid && i < sizeof(script_bios) / sizeof(script_bios[0]); ++i) {
        const auto text = long_cjk_bio && i >= 2 && i <= 3 ? long_cjk_bios[i - 2] : script_bios[i];
        data->bio.bytes = 0;
        for (u32 repeat = 0; repeat < (i == 5 ? 8u : 2u); ++repeat) {
            if (repeat)
                data->bio.data[data->bio.bytes++] = '\n';
            copy_bytes(data->bio.data + data->bio.bytes, text.data, text.bytes);
            data->bio.bytes += static_cast<u16>(text.bytes);
        }
        valid = profile_scene_layout(&scene, viewport, true) && scene.bio_style_count >= 1;
        valid = valid && native_bio_command(i == 5);
        const auto before = native_text_candidate_counts(g_probe.renderer.glyph_text.system);
        valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
                probe_capture_frame(script_captures[i]);
        valid =
            valid && native_text_candidate_counts(g_probe.renderer.glyph_text.system).paragraphs_shaped >
                         before.paragraphs_shaped;
    }
    const auto joined_bio = utf8_literal("A **\xd7\x90\xe2\x80\x8d\xd7\x91** __\xd7\x90\xcd\x8f \xd7\x91__ "
                                         "*world* `\xd7\x90\xe2\x80\x8d \xd7\x91` ");
    data->bio.bytes = 0;
    for (u32 repeat = 0; repeat < 3; ++repeat) {
        copy_bytes(data->bio.data + data->bio.bytes, joined_bio.data, joined_bio.bytes);
        data->bio.bytes += static_cast<u16>(joined_bio.bytes);
    }
    valid = valid && profile_scene_layout(&scene, viewport, true) && scene.bio_style_count >= 4;
    valid = valid && native_bio_command();
    const auto joined_before = native_text_candidate_counts(g_probe.renderer.glyph_text.system);
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"mixed-join-styles-profile.bmp");
    valid = valid && native_text_candidate_counts(g_probe.renderer.glyph_text.system).paragraphs_shaped >
                         joined_before.paragraphs_shaped;
    const auto emoji_bio =
        utf8_literal("A **\xd7\x90 \xf0\x9f\x98\x80\xf0\x9f\x98\x83 \xd7\x91** __\xd7\x90 "
                     "\xe2\x9d\xa4\xef\xb8\x8f \xd7\x91__ `\xd7\x90 1\xef\xb8\x8f\xe2\x83\xa3 \xd7\x91` ");
    data->bio.bytes = 0;
    for (u32 repeat = 0; repeat < 2; ++repeat) {
        copy_bytes(data->bio.data + data->bio.bytes, emoji_bio.data, emoji_bio.bytes);
        data->bio.bytes += static_cast<u16>(emoji_bio.bytes);
    }
    valid = valid && profile_scene_layout(&scene, viewport, true) && scene.bio_style_count >= 3;
    valid = valid && native_bio_command();
    const auto emoji_before = native_text_candidate_counts(g_probe.renderer.glyph_text.system);
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"mixed-bidi-emoji-profile.bmp");
    valid = valid && native_text_candidate_counts(g_probe.renderer.glyph_text.system).paragraphs_shaped >
                         emoji_before.paragraphs_shaped;
    const auto symbol_bio =
        utf8_literal("A **\xe2\x9d\xa4\xef\xb8\x8e** __\xce\xb1 \xe2\x9d\xa4\xef\xb8\x8e \xd0\x91__ "
                     "`\xe2\x99\xa5\xef\xb8\x8e \xe2\x99\xa6\xef\xb8\x8e` \xe2\x88\xab x \xe2\x89\xa4 y "
                     "\xc2\xa9\xef\xb8\x8e A \xef\xb8\x8f B ");
    data->bio.bytes = 0;
    for (u32 repeat = 0; repeat < 2; ++repeat) {
        copy_bytes(data->bio.data + data->bio.bytes, symbol_bio.data, symbol_bio.bytes);
        data->bio.bytes += static_cast<u16>(symbol_bio.bytes);
    }
    valid = valid && profile_scene_layout(&scene, viewport, true) && scene.bio_style_count >= 3;
    valid = valid && native_bio_command();
    const auto symbol_before = native_text_candidate_counts(g_probe.renderer.glyph_text.system);
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"symbols-profile.bmp");
    valid = valid && native_text_candidate_counts(g_probe.renderer.glyph_text.system).paragraphs_shaped >
                         symbol_before.paragraphs_shaped;
    const auto simulated_color_bio = utf8_literal(
        "Text **\xe2\x9d\xa4\xef\xb8\x8e** *\xe2\x9d\xa4\xef\xb8\x8e* ***\xe2\x9d\xa4\xef\xb8\x8e*** "
        "__\xe2\x98\x80\xef\xb8\x8e \xe2\x98\x82\xef\xb8\x8e__ ");
    data->bio.bytes = 0;
    for (u32 repeat = 0; repeat < 3; ++repeat) {
        copy_bytes(data->bio.data + data->bio.bytes, simulated_color_bio.data, simulated_color_bio.bytes);
        data->bio.bytes += static_cast<u16>(simulated_color_bio.bytes);
    }
    valid = valid && profile_scene_layout(&scene, viewport, true) && scene.bio_style_count >= 4;
    valid = valid && native_bio_command();
    const auto simulated_color_before = native_text_candidate_counts(g_probe.renderer.glyph_text.system);
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"simulated-color-symbols-profile.bmp");
    valid = valid && native_text_candidate_counts(g_probe.renderer.glyph_text.system).paragraphs_shaped >
                         simulated_color_before.paragraphs_shaped;
    const auto interactive_bio =
        utf8_literal("A [\xd7\x90 abc \xe2\x9d\xa4\xef\xb8\x8f \xd7\x91](https://example.com/native-text) "
                     "**[\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d](https://example.com/native-bold)**\n");
    data->bio.bytes = 0;
    for (u32 repeat = 0; repeat < 2; ++repeat) {
        copy_bytes(data->bio.data + data->bio.bytes, interactive_bio.data, interactive_bio.bytes);
        data->bio.bytes += static_cast<u16>(interactive_bio.bytes);
    }
    valid = valid && profile_scene_layout(&scene, viewport, true) && scene.bio_semantic_count == 4;
    valid = valid && native_bio_command();
    const auto interactive_before = native_text_candidate_counts(g_probe.renderer.glyph_text.system);
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"mixed-bidi-links-profile.bmp");
    for (u32 s = 0; valid && s < scene.bio_semantic_count; ++s) {
        const auto& expected = scene.bio_semantics[s];
        bool found = false;
        const auto& storage = *g_probe.renderer.content_text;
        for (u32 h = 0; h < storage.content_hit_region_count; ++h) {
            const auto& region = storage.content_hit_regions[h];
            const auto& semantic = storage.content_semantics[region.semantic_index];
            if (semantic.message_id || semantic.source_offset != expected.source_offset ||
                semantic.source_bytes != expected.source_bytes || semantic.kind != expected.kind)
                continue;
            const auto hit = renderer_content_hit_test(
                g_probe.renderer,
                static_cast<i32>((region.left + region.right) * .5f * g_probe.renderer.dpi / 96.f),
                static_cast<i32>((region.top + region.bottom) * .5f * g_probe.renderer.dpi / 96.f));
            valid = valid && hit.valid && !hit.message_id && hit.source_offset == expected.source_offset &&
                    hit.source_bytes == expected.source_bytes && hit.kind == expected.kind;
            found = true;
        }
        valid = valid && found;
    }
    if (valid)
        log_write(LogLevel::info, "probe", "mixed_bidi_profile_links_hit_geometry_passed");
    valid = valid && native_text_candidate_counts(g_probe.renderer.glyph_text.system).paragraphs_shaped >
                         interactive_before.paragraphs_shaped;
    data->bio = arabic_saved_bio;
    valid = valid && profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme);
    const u32 banner_text_rebuilds = g_probe.text.rebuilds;
    const u32 banner_text_suppressions = g_probe.renderer.stale_interaction_text_suppressions;
    const u32 banner_text_batches = g_probe.text.batch_count;
    valid = valid && profile_media_use_compatibility_banner(&scene) &&
            !profile_media_use_compatibility_banner(&scene) &&
            banner_text_batches > g_probe.renderer.overlay_text_first_batch;
    // Exercise the shipping transition without a text rebuild, then continue
    // presenting media frames. This was an indefinitely blank profile before.
    for (u32 frame = 0; valid && frame < 12; ++frame)
        valid = probe_present(&g_probe.renderer, kDefaultTheme) &&
                g_probe.text.rebuilds == banner_text_rebuilds &&
                g_probe.text.batch_count == banner_text_batches &&
                g_probe.renderer.stale_interaction_text_suppressions == banner_text_suppressions;
    scene.banner_compatibility = false;
    if (valid)
        log_write(LogLevel::info, "probe", "banner_format_switch_preserves_profile_text_passed");
    valid = valid && probe_native_profile_asset(renderer_text_system(&g_probe.renderer, UiTextFace::techno), 14);
    bool rounded_media = false;
    const auto card_mask = profile_scene_card_mask(scene, kDefaultTheme);
    for (u32 i = 0; i < g_probe.renderer.media_quad_count; ++i) {
        const auto& q = g_probe.renderer.media_draw.quad_data[i];
        rounded_media |= ui_rect_equal(q.rounded_clip, card_mask.rectangle) && q.clip_radius == card_mask.radius;
    }
    valid = valid && rounded_media;
    // The official client's footer: 16 DIPs in (plus the theme border) and 16 up.
    const float inset = 16 + (scene.themed ? 2.f : 0.f);
    valid = valid && scene.edit_rectangle.left == scene.rectangle.left + inset &&
            scene.edit_rectangle.right == scene.rectangle.right - inset &&
            scene.edit_rectangle.bottom == scene.rectangle.bottom - inset;
    // Role pills (expanded past the popout's two lines): the removable HELPER
    // pill ends in its always-visible x; the long name fits the content column.
    scene.roles_expanded = true;
    valid = valid && profile_scene_layout(&scene, viewport, true);
    bool helper = false, long_role = false, remove = false;
    for (u32 i = 0; i < scene.part_count; ++i) {
        const auto& part = scene.parts[i];
        const auto* node = ui_widget_manager_layout_node(scene.widgets, part.widget);
        if (!node)
            continue;
        const auto r = node->rectangle;
        if (part.look == ProfileLook::role && part.target == 56)
            helper = ui_rect_width(r) > 40 && ui_rect_height(r) == 24;
        if (part.look == ProfileLook::role && part.target == 57)
            long_role = r.right <= scene.rectangle.right - inset && ui_rect_width(r) > 250;
        if (part.action == ProfileAction::remove_role && part.target == 56) {
            const auto clicked =
                ui_button_pointer(&scene.widgets, UiButtonPointerEvent::press, {r.left + 12, r.top + 12});
            const auto released =
                ui_button_pointer(&scene.widgets, UiButtonPointerEvent::release, {r.left + 12, r.top + 12});
            remove = clicked.handled && released.activate &&
                     profile_scene_action(scene, released.target) == ProfileAction::remove_role;
        }
    }
    valid = valid && helper && long_role && remove;
    scene.roles_expanded = false;
    valid = valid && profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme);
    const auto& edit_layout = g_probe.renderer.profile_edit_layout;
    valid = valid && edit_layout.active && edit_layout.selection_count && edit_layout.text_bytes == 14;
    valid = valid && probe_profile_hover(scene);
    const u32 shaped = g_probe.renderer.content_text->rebuilds,
              uploads = g_probe.renderer.text_instance_uploads;
    const u64 deadline = g_probe.renderer.text_effect_next_ms;
    const u32 effect_builds = g_probe.renderer.paint_stream_builds;
    const bool effect_tick = valid && renderer_note_text_effect_damage(&g_probe.renderer, deadline) &&
                             g_probe.renderer.text_effect_next_ms == deadline + 42 &&
                             probe_present(&g_probe.renderer, kDefaultTheme) &&
                             g_probe.renderer.content_text->rebuilds == shaped &&
                             g_probe.renderer.text_instance_uploads == uploads &&
                             // Shader-time effects repaint through damage, not a scene rebuild.
                             g_probe.renderer.paint_stream_builds == effect_builds &&
                             probe_capture_frame(L"member-profile-name-animated.bmp");
    if (valid && !effect_tick)
        log_write(LogLevel::error, "probe", "profile_name_effect_damage_retention_failed");
    valid = valid && effect_tick;
    {
        // The next tick, with both flip buffers synchronized, repaints only
        // the animated name's bounds.
        const auto& r = g_probe.renderer;
        const u32 full = r.full_present_draws, partial = r.partial_present_draws,
                  builds = r.paint_stream_builds;
        const u64 damaged = r.partial_present_pixels;
        const u64 next_tick = r.text_effect_next_ms;
        const bool partial_tick =
            valid && renderer_note_text_effect_damage(&g_probe.renderer, next_tick) &&
            r.text_effect_next_ms == next_tick + 42 && probe_present(&g_probe.renderer, kDefaultTheme) &&
            r.paint_stream_builds == builds && r.content_text->rebuilds == shaped &&
            r.text_instance_uploads == uploads && r.full_present_draws == full &&
            r.partial_present_draws == partial + 1 &&
            r.partial_present_pixels - damaged < static_cast<u64>(r.width) * r.height / 16;
        if (valid && !partial_tick)
            log_write(LogLevel::error, "probe", "profile_name_effect_partial_damage_failed");
        valid = valid && partial_tick;
    }
    scene.edit.caret_visible = false;
    valid = valid && probe_present(&g_probe.renderer, kDefaultTheme) &&
            g_probe.renderer.content_text->rebuilds == shaped &&
            g_probe.renderer.text_instance_uploads == uploads;
    // Exercise every released font and effect through the native renderer, including
    // loading the intended family rather than silently accepting system fallback.
    const auto original_style = data->user_appearance.name;
    const wchar_t* font_captures[] = {L"font-01.bmp", L"font-02.bmp", L"font-03.bmp", L"font-04.bmp",
                                      L"font-05.bmp", L"font-06.bmp", L"font-07.bmp", L"font-08.bmp",
                                      L"font-09.bmp", L"font-10.bmp", L"font-11.bmp", L"font-12.bmp",
                                      L"font-13.bmp", L"font-14.bmp", L"font-15.bmp", L"font-16.bmp"};
    for (u16 id = 1; valid && id <= 16; ++id) {
        g_probe.failure_stage = 170 + id;
        data->user_appearance.name.font_id = id;
        data->user_appearance.name.effect_id = 1;
        valid = user_appearance_upsert(&g_probe.users.appearance, data->user_appearance) &&
                profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
                probe_present(&g_probe.renderer, kDefaultTheme);
        if (valid)
            g_probe.failure_stage = 190 + id;
        const auto face = profile_name_face(id);
        const auto system = renderer_text_system(&g_probe.renderer, face);
        if (id != 11)
            valid = valid && probe_native_profile_asset(system, id);
        valid = valid && probe_capture_frame(font_captures[id - 1]);
        if (!valid)
            log_u64(LogLevel::error, "probe", "profile_font_failed", id);
    }
    const wchar_t* effect_captures[] = {L"effect-01.bmp", L"effect-02.bmp", L"effect-03.bmp",
                                        L"effect-04.bmp", L"effect-05.bmp", L"effect-06.bmp",
                                        L"effect-07.bmp", L"effect-08.bmp"};
    for (u16 id = 1; valid && id <= 8; ++id) {
        auto& style = data->user_appearance.name;
        style.font_id = 14;
        style.effect_id = id;
        style.color_count = id == 7 ? 5 : id == 8 ? 4 : id == 2 || id == 6 ? 2 : 1;
        const u32 palette[] = {0xAB66FF, 0x55DACC, 0xF5CD79, 0xF392A9, 0x73B5FA};
        for (u32 c = 0; c < style.color_count; ++c)
            style.colors[c] = palette[c];
        scene.name_started_ms = GetTickCount64() - 420;
        valid = user_appearance_upsert(&g_probe.users.appearance, data->user_appearance) &&
                profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
                probe_present(&g_probe.renderer, kDefaultTheme);
        bool found = false;
        for (u32 m = 0; m < g_probe.renderer.text_effect_count; ++m)
            if (g_probe.renderer.text_effects.materials[m].kind == id) {
                const bool moving = id == 3 || id == 4 || id == 5 || id == 7 || id == 8;
                found = true;
                BOOL motion = TRUE;
                SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &motion, 0);
                valid = valid &&
                        (g_probe.renderer.text_effects.materials[m].animate != 0) == (moving && motion);
            }
        const u32 retained = g_probe.renderer.content_text->rebuilds,
                  uploaded = g_probe.renderer.text_instance_uploads;
        valid = valid && found && probe_present(&g_probe.renderer, kDefaultTheme) &&
                g_probe.renderer.content_text->rebuilds == retained &&
                g_probe.renderer.text_instance_uploads == uploaded &&
                probe_capture_frame(effect_captures[id - 1]);
    }
    data->user_appearance.name = original_style;
    valid = valid && user_appearance_upsert(&g_probe.users.appearance, data->user_appearance);
    const auto original_bio = data->bio;
    const auto styled_bio = utf8_literal(
        "A **bold** and *italic* bio with ~~struck text~~, ||hidden words|| and `inline code`.");
    copy_bytes(data->bio.data, styled_bio.data, styled_bio.bytes);
    data->bio.bytes = static_cast<u16>(styled_bio.bytes);
    valid = valid && profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"profile-biography.bmp");
    bool spoiler = false;
    u32 source = 0;
    for (u32 i = 0; i < scene.bio_semantic_count; ++i)
        if (scene.bio_semantics[i].kind == MessageMarkdownSemanticKind::spoiler) {
            spoiler = true;
            source = scene.bio_semantics[i].source_offset;
        }
    valid = valid && spoiler && scene.bio_style_count >= 4;
    scene.bio_revealed[scene.bio_revealed_count++] = source;
    valid = valid && profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"profile-biography-revealed.bmp");
    TextFormScene top_form{};
    TextFormSurface top_surface{};
    const TextFormContent form_content{utf8_literal("Edit profile"),
                                       utf8_literal("The profile remains below this form."),
                                       utf8_literal("Display name"),
                                       {},
                                       utf8_literal("Cancel"),
                                       utf8_literal("Save"),
                                       utf8_literal("Native edited name"),
                                       0,
                                       0,
                                       true,
                                       true,
                                       {}};
    valid = valid && text_form_update(&top_form, form_content, viewport, &g_probe.text_measure_cache);
    top_surface.scene = &top_form;
    renderer_bind_text_form(&g_probe.renderer, &top_surface);
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            g_probe.renderer.form_text_first_batch > g_probe.renderer.overlay_text_first_batch &&
            g_probe.renderer.form_paint_first_batch > g_probe.renderer.profile_foreground_first_batch &&
            g_probe.renderer.media_quad_count > 0 && probe_capture_frame(L"profile-edit-layer.bmp");
    renderer_bind_text_form(&g_probe.renderer, nullptr);
    text_form_surface_release(&top_surface);
    text_form_release(&top_form);
    data->bio = original_bio;
    scene.bio_revealed_count = 0;
    scene.presentation = ProfilePresentation::modal;
    valid = valid && profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"member-profile-expanded.bmp");
    valid = valid && probe_profile_hover(scene);
    UiRect tab_rectangles[3]{};
    for (u32 selected_tab = 0; selected_tab < 3; ++selected_tab) {
        scene.tab = static_cast<u8>(selected_tab);
        valid = valid && profile_scene_layout(&scene, viewport, true);
        for (u32 i = 0; i < scene.part_count; ++i)
            if (scene.parts[i].look == ProfileLook::tab) {
                const auto* node = ui_widget_manager_layout_node(scene.widgets, scene.parts[i].widget);
                const u32 index = scene.parts[i].source;
                if (!selected_tab)
                    tab_rectangles[index] = node->rectangle;
                else
                    valid = valid && ui_rect_equal(tab_rectangles[index], node->rectangle);
            }
    }
    scene.tab = 0;
    valid = valid && profile_scene_layout(&scene, viewport, true);
    const auto fixed_banner = scene.banner, fixed_avatar = scene.avatar;
    // Add enough details to require scrolling without making the fixed header move.
    data->connection_count = 16;
    for (u32 i = 0; i < 16; ++i) {
        copy_bytes(data->connections[i].type.data, "Service", 7);
        data->connections[i].type.bytes = 7;
        copy_bytes(data->connections[i].name.data, "Connected account", 17);
        data->connections[i].name.bytes = 17;
    }
    if (valid && !profile_scene_layout(&scene, viewport, true)) {
        g_probe.failure_stage = 152;
        valid = false;
    }
    if (valid && !profile_scene_scroll(&scene, 100)) {
        g_probe.failure_stage = 153;
        valid = false;
    }
    if (valid && !(scene.scroll > 0)) {
        g_probe.failure_stage = 154;
        valid = false;
    }
    if (valid &&
        !(ui_rect_equal(scene.banner, fixed_banner) && ui_rect_equal(scene.avatar, fixed_avatar))) {
        g_probe.failure_stage = 155;
        valid = false;
    }
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"member-profile-scrolled.bmp");
    // The owner may edit roles: removable pills and the "+" role picker.
    scene.presentation = ProfilePresentation::popout;
    valid = valid && profile_scene_layout(&scene, viewport, true) && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"member-profile-roles.bmp");
    bool assignable = false, removable = false;
    for (u32 i = 0; i < scene.part_count; ++i) {
        assignable |= scene.parts[i].action == ProfileAction::choose_roles;
        removable |= scene.parts[i].action == ProfileAction::remove_role && scene.parts[i].target == 56;
    }
    valid = valid && assignable && removable;
    // A narrow window: the full profile fills it.
    scene.presentation = ProfilePresentation::modal;
    scene.tab = 1;
    valid = valid && profile_scene_layout(&scene, {0, 0, 640, 600}, true) &&
            ui_rect_width(scene.rectangle) == 640 && rebuild_probe_content_text() &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"member-profile-narrow.bmp");
    // Self: the popout's Edit Profile footer.
    scene.presentation = ProfilePresentation::popout;
    scene.self_id = data->user_id;
    bool edit = false;
    valid = valid && profile_scene_layout(&scene, viewport, true);
    for (u32 i = 0; i < scene.part_count; ++i)
        edit |= scene.parts[i].action == ProfileAction::edit;
    valid = valid && edit && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"member-profile-edit.bmp");
    if (guild)
        guild->owner_id = previous_owner;
    scene.active = false;
    g_probe.renderer.profile_scene = nullptr;
    profile_scene_release(&scene);
    const bool was_home = g_probe.chat_state.home_selected;
    g_probe.chat_state.home_selected = false;
    valid = valid && synchronize_probe_shell_layout({}, {}, 0, 320);
    const auto* member_pane = ui_scene_resolve(
        g_probe.shell_scene, ui_scene_find(g_probe.shell_scene, UiSemantic::member_sidebar));
    // Same Mainframe family, plain role color, and a measured adjacent guild tag.
    MemberListScene members_scene{};
    MemberListRowWidget member_rows[2]{};
    UiWidget member_widgets[10]{};
    UiLayoutNode member_nodes[10]{};
    GuildMemberStore member_store{};
    MemberListRow source_row{4242,
                                    kMemberListPrivateGroup,
                                    ~0u,
                                    0xB88A22,
                                    0,
                                    GuildMemberStatus::online,
                                    MemberListRowKind::member};
    MemberListView projection{};
    projection.rows = {&source_row, 1};
    projection.count = projection.member_count = projection.revision = 1;
    valid = valid && user_store_upsert(&g_probe.users, 4242, utf8_literal("Robin-san")).ok() &&
            member_list_scene_initialize(&members_scene, {member_rows, 2}, {member_widgets, 10},
                                         {member_nodes, 10});
    members_scene.measure = &g_probe.text_measure_cache;
    valid = valid &&
            member_list_scene_update(&members_scene, &projection, &member_store, &g_probe.users,
                                     &g_probe.chat_state, member_pane ? member_pane->rectangle : UiRect{}, 0)
                .ok();
    if (!valid)
        g_probe.failure_stage = 231;
    UiTextCommand member_commands[kMaximumShellChromeTextCommands]{};
    UiTextCommandStream member_text{};
    char member_text_bytes[4096]{};
    valid =
        valid &&
        ui_text_command_stream_initialize(&member_text, {member_commands, kMaximumShellChromeTextCommands},
                                          {member_text_bytes, 4096}) &&
        shell_text_presentation_build(g_probe.shell_scene, g_probe.chat_state, g_probe.navigation,
                                      kDefaultTheme, g_probe.shell_navigation_scene,
                                      &g_probe.message_composer_scene, &members_scene, &member_text);
    UiTextMeasureRequest measured_name{
        {0x50524F46494C45, 1, 0, 0, 0}, utf8_literal("Robin-san"), {}, {400, 40}, true, UiTextFace::normal};
    UiTextMeasureValue normal_width{}, custom_width{}, cached_width{};
    valid = valid &&
            ui_text_measure_cache_measure(&g_probe.text_measure_cache, measured_name, &normal_width).ok();
    measured_name.face = UiTextFace::techno;
    valid = valid &&
            ui_text_measure_cache_measure(&g_probe.text_measure_cache, measured_name, &custom_width).ok() &&
            custom_width.size.width != normal_width.size.width;
    measured_name.face = UiTextFace::normal;
    valid = valid &&
            ui_text_measure_cache_measure(&g_probe.text_measure_cache, measured_name, &cached_width).ok() &&
            cached_width.size.width == normal_width.size.width;
    bool member_plain = false;
    for (u32 i = 0; i < member_text.count; ++i)
        if (utf8_equal(member_commands[i].text, utf8_literal("Robin-san"))) {
            const auto& c = member_commands[i];
            const auto badge = member_list_badge(members_scene, 0);
            member_plain = c.face == UiTextFace::techno && !c.effect.kind && c.color.red > 0.71f &&
                           c.color.red < .73f && badge.rectangle.left > c.rectangle.left + 70 &&
                           badge.rectangle.right < viewport.right - 40;
        }
    if (valid)
        g_probe.failure_stage = 230;
    g_probe.renderer.member_list_scene = &members_scene;
    if (valid) {
        g_probe.failure_stage = 232;
        valid = rebuild_probe_content_text();
    }
    if (valid) {
        g_probe.failure_stage = 233;
        valid = probe_present(&g_probe.renderer, kDefaultTheme);
    }
    if (valid) {
        g_probe.failure_stage = 234;
        valid = probe_capture_frame(L"member-name-and-tag.bmp");
    }
    if (valid)
        g_probe.failure_stage = 235;
    valid = valid && member_plain;
    g_probe.renderer.member_list_scene = nullptr;
    g_probe.chat_state.home_selected = was_home;
    valid = synchronize_probe_shell_layout() && valid;
    VirtualFree(data, 0, MEM_RELEASE);
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme);
    if (valid)
        g_probe.failure_stage = 0;
    return valid;
}

// Rich presence: a Spotify card with its progress track and a game card in
// the full profile's Activity tab, from synthetic activities (no network).
bool probe_profile_activity_card() noexcept {
    g_probe.failure_stage = 740;
    auto* data = static_cast<UserProfile*>(
        VirtualAlloc(nullptr, sizeof(UserProfile), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!data)
        return false;
    u32 scratch[2048];
    bool valid = user_profile_decode(
        utf8_literal(R"({"user":{"id":"4343","username":"activity.test","global_name":"Activity Probe"},"user_profile":{"bio":"Listening and playing."}})"),
        {scratch, 2048}, 4343, 0, data);
    const auto presence = utf8_literal(R"({"activities":[
        {"type":2,"name":"Spotify","details":"Native Song Title","state":"First Artist; Second Artist",
         "timestamps":{"start":1700000000000,"end":1700000200000},
         "assets":{"large_image":"mp:external/probe/https/example.invalid/album.png","large_text":"Probe Album"}},
        {"type":0,"name":"Mossrun","application_id":"500000000000000001","details":"Chapter 7","state":"Ascent",
         "timestamps":{"start":1700000000000},"party":{"size":[2,4]},
         "assets":{"large_image":"500000000000000002","small_image":"500000000000000003"}}]})");
    UserActivitySet activities{};
    bool has = false;
    const auto scan = json_scan_structurals_avx1(presence, {scratch, 2048});
    valid = valid && scan.ok() &&
            user_activities_decode({presence, {scratch, scan.count}}, {presence, 0, JsonValueKind::object}, 4343,
                                   &activities, &has) &&
            has && activities.count == 2;
    // Pin the Spotify clock to half way so the capture is deterministic.
    const u64 now = clock_unix_milliseconds();
    activities.items[0].start_ms = now - 100'000;
    activities.items[0].end_ms = now + 100'000;
    activities.items[1].start_ms = now - 754'000;
    valid = valid && user_activity_upsert(&g_probe.users.activities, activities);
    ProfileScene scene{};
    scene.phase = ProfilePhase::ready;
    scene.measure = &g_probe.text_measure_cache;
    const UiRect viewport{0, 0, static_cast<float>(g_probe.renderer.width) * 96 / g_probe.renderer.dpi,
                          static_cast<float>(g_probe.renderer.height) * 96 / g_probe.renderer.dpi};
    valid = valid && profile_scene_open(&scene, data, &g_probe.chat_state, &g_probe.users, 1,
                                        {viewport.right - 100, 90, viewport.right, 134}, viewport);
    PngImage image{};
    u8 pixels[32 * (32 * 4 + 1)]{};
    valid = valid && png_image_configure(&image, 32, 32, 8, 6);
    for (u32 y = 0; y < 32; ++y)
        for (u32 x = 0; x < 32; ++x) {
            const u32 at = y * 129 + 1 + x * 4;
            pixels[at] = static_cast<u8>(40 + x * 6);
            pixels[at + 1] = 160;
            pixels[at + 2] = static_cast<u8>(40 + y * 6);
            pixels[at + 3] = 255;
        }
    u32 cards = 0, progress = 0, artwork = 0;
    const auto upload = [&]() noexcept {
        cards = progress = artwork = 0;
        for (u32 i = 0; i < scene.part_count; ++i) {
            const auto media = scene.parts[i].media;
            progress += media == ProfileMediaKind::activity_progress;
            cards += media == ProfileMediaKind::activity_large;
            ProfileMedia resolved{};
            if ((media == ProfileMediaKind::activity_large || media == ProfileMediaKind::activity_small) &&
                profile_media_resolve(scene, i, &resolved)) {
                ++artwork;
                if (!renderer_upload_png_scanlines(&g_probe.renderer, resolved.key, image, {pixels, sizeof(pixels)}).ok())
                    return false;
            }
        }
        return true;
    };
    valid = valid && upload() && cards == 1 && progress == 1 && artwork == 1 && scene.activity_timed;
    g_probe.renderer.profile_scene = &scene;
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"profile-activity-card.bmp");
    if (valid)
        g_probe.failure_stage = 741;
    // The full profile lists every activity under Activity.
    scene.presentation = ProfilePresentation::modal;
    scene.tab = 0;
    valid = valid && profile_scene_layout(&scene, viewport, true) && upload() && cards == 2 && artwork == 3 &&
            rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"profile-activity-tab.bmp");
    g_probe.renderer.profile_scene = nullptr;
    profile_scene_release(&scene);
    UserActivitySet cleared{};
    cleared.user_id = 4343;
    user_activity_upsert(&g_probe.users.activities, cleared);
    VirtualFree(data, 0, MEM_RELEASE);
    valid = valid && rebuild_probe_content_text() && probe_present(&g_probe.renderer, kDefaultTheme);
    if (valid)
        g_probe.failure_stage = 0;
    return valid;
}

bool probe_rich_composer() noexcept {
    g_probe.failure_stage = 140;
    char buffer[2048]{};
    MessageComposer model{};
    MessageComposerScene scene{};
    bool valid = message_composer_initialize(&model, {buffer, sizeof(buffer)}) &&
                 message_composer_scene_initialize_owned(&scene);
    message_composer_reset(&model, 42, false);
    UiRect viewport{50, 100, 550, 148};
    float preferred = 48;
    const auto refresh = [&]() noexcept {
        if (!message_composer_scene_update(&scene, &model, viewport, true).ok() ||
            !renderer_bind_message_composer_scene(&g_probe.renderer, &scene) ||
            !renderer_measure_message_composer(&g_probe.renderer, &preferred))
            return false;
        viewport.bottom = viewport.top + preferred;
        return message_composer_scene_update(&scene, &model, viewport, true).ok() &&
               renderer_measure_message_composer(&g_probe.renderer, &preferred);
    };
    valid = valid && refresh();
    bool wrapped = false;
    for (u32 i = 0; valid && i < 256 && !wrapped; ++i) {
        valid = message_composer_insert_scalar(&model, 'i') == MessageComposerResult::changed && refresh();
        const u32 line_count = text_edit_layout_line_count(g_probe.renderer.composer_layout);
        valid = valid && line_count;
        wrapped = line_count > 1;
        valid = valid && (wrapped ? preferred > 48 : preferred == 48);
    }
    valid = valid && wrapped;
    if (valid)
        log_write(LogLevel::info, "probe", "composer_height_changes_only_on_native_wrap");
    g_probe.failure_stage = 141;
    const auto rich = utf8_literal("**Bold** *italic* `code` @everyone\n```cpp\nint x = "
                                   "1;\n```\none\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n");
    valid = valid && message_composer_restore(&model, 42, rich, rich.bytes, rich.bytes) && refresh() &&
            preferred == 208;
    auto& layout = g_probe.renderer.composer_layout;
    // Styled composer geometry is native; its parity with DirectWrite edit
    // layouts is covered by the native app-line editor corpora.
    valid = valid && layout.geometry;
    const float bottom = layout.scroll_y;
    g_probe.failure_stage = 142;
    const auto native_measures =
        native_text_candidate_counts(g_probe.renderer.glyph_text.system).paragraphs_measured;
    valid = valid && bottom > 0 && text_edit_layout_scroll(&layout, -bottom) && layout.scroll_y == 0 &&
            refresh() && layout.scroll_y == 0 && layout.geometry;
    valid = valid && native_text_candidate_counts(g_probe.renderer.glyph_text.system).paragraphs_measured ==
                         native_measures;
    valid = valid && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme);
    bool blue_mention = false;
    const auto* edit_node = message_composer_scene_layout_node(scene, MessageComposerControl::edit);
    const auto* text_storage = g_probe.renderer.content_text;
    for (u32 i = 0; valid && edit_node && text_storage && i < text_storage->quad_count; ++i) {
        const auto& quad = text_storage->quads[i];
        blue_mention |= quad.left >= edit_node->content_rectangle.left &&
                        quad.right <= edit_node->content_rectangle.right &&
                        quad.top >= edit_node->content_rectangle.top &&
                        quad.bottom <= edit_node->content_rectangle.bottom &&
                        bytes_equal(&quad.color, &kDefaultTheme.accent, sizeof(Color));
    }
    valid = valid && blue_mention;
    // Moving the viewport without editing must not undo an intentional wheel scroll.
    viewport.top += 4;
    viewport.bottom += 4;
    valid = valid && refresh() && layout.scroll_y == 0 && layout.geometry;
    g_probe.failure_stage = 143;
    u32 hit = ~0u;
    valid = valid && text_edit_layout_hit_test(layout, layout.origin_x, layout.viewport_top + 2, &hit) &&
            hit == 0 && message_composer_set_selection(&model, 0, 0) == MessageComposerResult::changed &&
            refresh() &&
            message_composer_move(&model, MessageComposerMove::document_end, false) ==
                MessageComposerResult::changed &&
            refresh() && layout.scroll_y > 0 &&
            layout.caret.top + layout.caret.height <= layout.viewport_top + layout.height + .01f;
    g_probe.failure_stage = 144;
    if (!valid)
        return false;
    const auto code = utf8_literal("```cpp\nint x = 1;\n```");
    model.focused = true;
    message_composer_scene_set_caret_visible(&scene, true);
    valid = valid && message_composer_restore(&model, 42, code, 7, 10) && refresh() &&
            renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme);
    u32 background = ~0u, selected = ~0u, glyph = ~0u;
    for (u32 i = 0; valid && i < text_storage->quad_count; ++i) {
        const auto& q = text_storage->quads[i];
        if (q.left < viewport.left || q.right > viewport.right || q.top < viewport.top ||
            q.bottom > viewport.bottom)
            continue;
        if (q.u0 == -1 && q.color.alpha == .38f)
            selected = i;
        else if (q.u0 == -1)
            background = i;
        else if (selected != ~0u)
            glyph = i;
    }
    valid = valid && background != ~0u && selected != ~0u && glyph != ~0u && background < selected &&
            selected < glyph &&
            g_probe.renderer.edit_overlay_paint_first_batch < g_probe.renderer.paint_stream.batch_count &&
            probe_capture_frame(L"code-selection.bmp");
    const auto text_builds = g_probe.text.rebuilds;
    message_composer_scene_set_caret_visible(&scene, false);
    valid =
        valid && probe_present(&g_probe.renderer, kDefaultTheme) && g_probe.text.rebuilds == text_builds;
    message_composer_scene_set_caret_visible(&scene, true);
    valid = valid && message_composer_set_selection(&model, 10, 10) == MessageComposerResult::changed &&
            refresh() && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) && probe_capture_frame(L"code-caret.bmp");
    g_probe.failure_stage = 150;
    // A caret blink rebuilds paint only: after one present synchronizes the
    // other flip buffer, each toggle presents just the caret's damage.
    for (u32 toggle = 0; valid && toggle < 3; ++toggle) {
        const auto& r = g_probe.renderer;
        const u32 builds = r.paint_stream_builds, partial = r.partial_present_draws, full = r.full_present_draws,
                  diffs = r.paint_diff.partial_rebuilds, shaped = g_probe.text.rebuilds;
        const u64 pixels = r.partial_present_pixels;
        message_composer_scene_set_caret_visible(&scene, toggle % 2 != 0);
        valid = probe_present(&g_probe.renderer, kDefaultTheme) && r.paint_stream_builds == builds + 1 &&
                r.paint_diff.partial_rebuilds == diffs + 1 && g_probe.text.rebuilds == shaped &&
                (toggle == 0 || (r.full_present_draws == full && r.partial_present_draws == partial + 1 &&
                                 r.partial_present_pixels - pixels < static_cast<u64>(r.width) * r.height / 16));
        log_u64(valid ? LogLevel::info : LogLevel::error, "probe", "caret_blink_partial_present_pixels",
                r.partial_present_pixels - pixels);
        if (!valid) {
            log_u64(LogLevel::error, "probe", "caret_blink_toggle", toggle);
            log_u64(LogLevel::error, "probe", "caret_blink_builds", r.paint_stream_builds - builds);
            log_u64(LogLevel::error, "probe", "caret_blink_diff_partials", r.paint_diff.partial_rebuilds - diffs);
            log_u64(LogLevel::error, "probe", "caret_blink_full_presents", r.full_present_draws - full);
            log_u64(LogLevel::error, "probe", "caret_blink_diff_reject", r.paint_diff.reject_reason);
        }
    }
    g_probe.failure_stage = 151;
    const auto directional_edit =
        utf8_literal("A \xd7\x90\xd7\x91\xd7\x92 \xf0\x9f\x98\x80 B\n\xd7\x90\xd7\x91 123 CD");
    valid = valid && message_composer_restore(&model, 42, directional_edit, 2, 8) && refresh();
    // Native directional editor geometry; its parity with DirectWrite edit
    // layouts is covered by the native app-line directional editor corpora.
    const auto native_editor_geometry = [&]() noexcept {
        if (!layout.geometry || !layout.selection_count)
            return false;
        for (u32 position = 0; position <= layout.utf16_units; ++position) {
            TextEditMetric metric{};
            if (!text_edit_layout_position_geometry(layout, position, &metric) || !(metric.height > 0))
                return false;
        }
        return true;
    };
    valid = valid && native_editor_geometry();
    valid = valid && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"composer-bidi-selection.bmp");
    valid = valid && message_composer_set_selection(&model, 8, 8) == MessageComposerResult::changed &&
            refresh() && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"composer-bidi-caret.bmp");
    TextCompositionSpan directional_composition{};
    directional_composition.start = 2;
    directional_composition.end = 5;
    directional_composition.line_style = TextCompositionLineStyle::solid;
    scene.composition = {&directional_composition, 1};
    ++scene.composition_revision;
    valid = valid && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"composer-bidi-ime.bmp");
    scene.composition = {};
    ++scene.composition_revision;
    g_probe.failure_stage = 152;
    const auto cluster_text = utf8_literal("A \xd7\xa9\xd6\xb8\xd6\xbc\xd7\x81 \xf0\x9f\x98\x80 B");
    valid = valid && message_composer_restore(&model, 42, cluster_text, 0, 6) && refresh() &&
            scene.display_styles;
    // Exercise the renderer's projected-style boundary directly, including a
    // property range inside a grapheme; source parsing is tested separately.
    if (valid) {
        scene.display_styles[0] = {8, cluster_text.bytes - 8, text_style_underline};
        scene.display_style_count = 1;
        valid = renderer_measure_message_composer(&g_probe.renderer, &preferred);
    }
    valid = valid && native_editor_geometry();
    valid = valid && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"composer-cluster-cut-selection.bmp");
    directional_composition.start = 2;
    directional_composition.end = 6;
    scene.composition = {&directional_composition, 1};
    ++scene.composition_revision;
    valid = valid && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"composer-cluster-cut-ime.bmp");
    scene.composition = {};
    ++scene.composition_revision;
    const auto selector_text = utf8_literal("A\xef\xb8\x8f B \xf0\x9f\x98\x80");
    valid = valid && message_composer_restore(&model, 42, selector_text, 0, 4) && refresh() &&
            scene.display_styles;
    if (valid) {
        scene.display_styles[0] = {0, 1, text_style_underline};
        scene.display_style_count = 1;
        valid = renderer_measure_message_composer(&g_probe.renderer, &preferred);
    }
    valid = valid && native_editor_geometry();
    if (valid)
        log_write(LogLevel::info, "probe", "native_composer_cluster_cuts_match_reference_geometry");
    valid = valid && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"composer-selector-cut.bmp");
    g_probe.failure_stage = 153;
    const auto arabic_text =
        utf8_literal("\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7\x20\xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7"
                     "\xd9\x84\xd9\x85\x20\xd9\xa1\xd9\xa2\xd9\xa3\x20\xf0\x9f\x98\x80\x0a\x48\x65\x6c\x6c"
                     "\x6f\x20\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d\x20\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7"
                     "\x20\xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85");
    valid = valid && message_composer_restore(&model, 42, arabic_text, 0, arabic_text.bytes) && refresh();
    valid = valid && renderer_measure_message_composer(&g_probe.renderer, &preferred);
    valid = valid && native_editor_geometry();
    if (valid)
        log_write(LogLevel::info, "probe", "native_arabic_composer_matches_reference_geometry");
    valid = valid && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"composer-arabic-selection.bmp");
    directional_composition.start = 0;
    directional_composition.end = 5;
    scene.composition = {&directional_composition, 1};
    ++scene.composition_revision;
    valid = valid && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme) &&
            probe_capture_frame(L"composer-arabic-ime.bmp");
    scene.composition = {};
    ++scene.composition_revision;
    g_probe.failure_stage = 150;
    const auto inline_emoji = utf8_literal("A <:party:31337> B");
    u8 emoji_pixels[16 * 16 * 4]{};
    for (u32 i = 0; i < 16 * 16; ++i) {
        emoji_pixels[i * 4] = 220;
        emoji_pixels[i * 4 + 1] = 160;
        emoji_pixels[i * 4 + 3] = 255;
    }
    valid = valid &&
            renderer_upload_media_rgba8(&g_probe.renderer, media_asset_custom_emoji(31337, false), 16, 16,
                                        64, {emoji_pixels, sizeof(emoji_pixels)})
                .ok() &&
            message_composer_restore(&model, 42, inline_emoji, inline_emoji.bytes, inline_emoji.bytes) &&
            refresh() && renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) &&
            probe_present(&g_probe.renderer, kDefaultTheme);
    UiRect emoji_rect{}, emoji_clip{};
    valid = valid && scene.projection_span_count == 1 &&
            text_composer_emoji_rectangle(layout, scene, 0, &emoji_rect, &emoji_clip);
    bool drawn_emoji = false;
    for (u32 i = 0; valid && i < g_probe.renderer.media_quad_count; ++i) {
        const auto& q = g_probe.renderer.media_draw.quad_data[i];
        drawn_emoji |= q.left == emoji_rect.left && q.top == emoji_rect.top &&
                       q.right == emoji_rect.right && q.bottom == emoji_rect.bottom;
    }
    valid = valid && drawn_emoji && probe_capture_frame(L"composer-custom-emoji.bmp");
    renderer_bind_message_composer_scene(&g_probe.renderer, &g_probe.message_composer_scene);
    message_composer_scene_release(&scene);
    message_composer_release(&model);
    if (valid)
        log_write(LogLevel::info, "probe",
                  "composer_rich_fonts_manual_scroll_hit_test_and_caret_reveal_hardware_passed");
    return valid;
}

bool probe_attachment_save_affordances() noexcept {
    if (!g_probe.message_scene.count) return false;
    const auto* record = message_store_resolve(g_probe.message_view.store, g_probe.message_scene.rows[0].message);
    if (!record) return false;
    GatewayMessageAttachment attachments[3]{};
    const Utf8View names[]{utf8_literal("release-notes-with-a-long-filename.txt"),
                           utf8_literal("original-native-art.png"), utf8_literal("native-preview.mp4")};
    const Utf8View types[]{utf8_literal("text/plain"),utf8_literal("image/png"),utf8_literal("video/mp4")};
    const Utf8View urls[]{
        utf8_literal("https://cdn.discordapp.com/attachments/111/811001/release.txt?ex=ffffffff&is=1&hm=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
        utf8_literal("https://cdn.discordapp.com/attachments/111/811002/art.png?ex=ffffffff&is=1&hm=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
        utf8_literal("https://cdn.discordapp.com/attachments/111/811003/preview.mp4?ex=ffffffff&is=1&hm=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")};
    u8 pixels[48 * 16 * 4]{};
    for (u32 i = 0; i < 48 * 16; ++i) {
        pixels[i * 4] = static_cast<u8>(40 + i % 48 * 4);
        pixels[i * 4 + 1] = 150; pixels[i * 4 + 2] = 205; pixels[i * 4 + 3] = 255;
    }
    for (u32 i = 0; i < 3; ++i) {
        attachments[i].id = 811001 + i; attachments[i].filename = names[i];
        attachments[i].content_type = types[i]; attachments[i].url = urls[i];
        attachments[i].size = 1024;
        if (i) {
            attachments[i].width = 192; attachments[i].height = 64;
            attachments[i].has_width = attachments[i].has_height = true;
            const auto format = i == 1 ? MediaAttachmentFormat::png : MediaAttachmentFormat::mp4;
            if (!renderer_upload_media_rgba8(&g_probe.renderer,
                media_asset_attachment(attachments[i].id, format,
                    media_resolution_tier_for_display(192,64,g_probe.renderer.dpi,media_preview_extent_cap(format))),
                48,16,192,{pixels,sizeof(pixels)}).ok()) return false;
        }
    }
    GatewayMessageEvent event{}; event.id = record->id; event.fields = gateway_message_field_attachments;
    event.attachments = {attachments,3};
    if (!message_rich_store_apply(&g_probe.rich_store,record->id,record->revision,event).ok() ||
        !rebuild_probe_content_text()) return false;
    for (u32 i = 0; i < 3; ++i) {
        const auto handle = message_scene_rich_item_widget(g_probe.message_scene.rows[0],MessageSceneRichItemKind::attachment,i);
        if (!ui_widget_manager_focus(&g_probe.message_scene.widgets,handle)) return false;
        renderer_bind_focus_owner(&g_probe.renderer,&g_probe.message_scene.widgets);
        if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer,kDefaultTheme) ||
            !probe_capture_frame(i == 0 ? L"attachment-save-file.bmp" : i == 1 ? L"attachment-save-image.bmp" : L"attachment-save-video.bmp")) return false;
    }
    log_write(LogLevel::info,"probe","attachment_save_file_image_video_original_affordances_passed");
    return true;
}

bool probe_image_viewer() noexcept {
    const float scale = 96.0f / g_probe.renderer.dpi;
    const UiRect viewport{0, 0, g_probe.renderer.width * scale, g_probe.renderer.height * scale};
    g_probe.failure_stage = 130;
    UiImageViewer image_viewer{};
    PngImage expanded_image{};
    u8 expanded_pixels[16 * (32 * 4 + 1)]{};
    const auto expanded_key =
        media_asset_attachment(991991, MediaAttachmentFormat::png, MediaResolutionTier::px2048);
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 32; ++x) {
            const u32 offset = y * 129 + 1 + x * 4;
            expanded_pixels[offset] = 240;
            expanded_pixels[offset + 3] = 255;
        }
    if (!png_image_configure(&expanded_image, 32, 16, 8, 6) ||
        !renderer_upload_png_scanlines(&g_probe.renderer, expanded_key, expanded_image,
                                       {expanded_pixels, sizeof(expanded_pixels)})
             .ok() ||
        !ui_image_viewer_open(&image_viewer, viewport, {1024, 512}))
        return false;
    g_probe.failure_stage = 131;
    g_probe.renderer.image_viewer = &image_viewer;
    g_probe.renderer.image_viewer_key = expanded_key;
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.popup_media_first_batch >= g_probe.renderer.media_batch_count ||
        g_probe.renderer.interaction_text_image_viewer_revision != image_viewer.revision)
        return false;
    g_probe.failure_stage = 132;
    const auto viewer_quad = g_probe.renderer.media_draw.quad_data[g_probe.renderer.media_quad_count - 1];
    if (viewer_quad.left != image_viewer.image.left || viewer_quad.bottom != image_viewer.image.bottom)
        return false;
    const ProbePixelSample viewed_image{
        {(image_viewer.image.left + image_viewer.image.right) * .5f,
         (image_viewer.image.top + image_viewer.image.bottom) * .5f}, {240.f / 255.f, 0, 0, 1}, 2};
    const UiRect viewer_footer{image_viewer.body.left, image_viewer.body.bottom + 8,
                               image_viewer.body.right, viewport.bottom};
    if (!probe_exact_pixels({&viewed_image, 1}) || !probe_text_pixels({&viewer_footer, 1}) ||
        !probe_capture_frame(L"viewer-full-brightness.bmp"))
        return false;
    g_probe.failure_stage = 133;
    const u32 viewer_builds = g_probe.renderer.paint_stream_builds;
    if (!probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.paint_stream_builds != viewer_builds || !probe_exact_pixels({&viewed_image, 1}))
        return false;
    const auto button_rectangle = ui_image_viewer_button(image_viewer, 1);
    const UiPoint button_point{button_rectangle.left + 16, button_rectangle.top + 16};
    const auto hover = ui_button_pointer(&image_viewer.widgets, UiButtonPointerEvent::move, button_point);
    if (!hover.changed)
        return false;
    ++image_viewer.revision;
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.paint_stream_builds <= viewer_builds || !probe_exact_pixels({&viewed_image, 1}))
        return false;
    const auto down = ui_button_pointer(&image_viewer.widgets, UiButtonPointerEvent::press, button_point);
    if (!down.handled || down.activate)
        return false;
    ++image_viewer.revision;
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_capture_frame(L"viewer-buttons.bmp"))
        return false;
    if (!ui_button_pointer(&image_viewer.widgets, UiButtonPointerEvent::release, button_point).activate)
        return false;
    ++image_viewer.revision;
    g_probe.failure_stage = 134;
    if (!ui_image_viewer_set_save_available(&image_viewer, true) ||
        !ui_widget_manager_focus(&image_viewer.widgets, image_viewer.buttons[4])) return false;
    image_viewer.keyboard_focus = true;
    ++image_viewer.revision;
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_capture_frame(L"viewer-save-original.bmp")) return false;
    ui_image_viewer_zoom(&image_viewer, 2,
                         {(viewport.left + viewport.right) * .5f, (viewport.top + viewport.bottom) * .5f});
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.paint_stream_builds <= viewer_builds || !probe_exact_pixels({&viewed_image, 1}))
        return false;
    g_probe.failure_stage = 135;
    g_probe.renderer.image_viewer = nullptr;
    if (!rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.interaction_text_image_viewer_revision)
        return false;
    log_write(LogLevel::info, "probe", "image_viewer_texture_controls_zoom_and_retention_hardware_passed");
    return true;
}

bool reaction_picker_probe_resolve(void*, u32 index, UiMediaPickerItem* item) noexcept {
    *item = {};
    item->id = 0x5e000ull + index;
    item->label = utf8_literal("Original grin");
    item->kind = UiMediaPickerItemKind::emoji;
    item->enabled = true;
    item->glyph = utf8_literal("\xF0\x9F\x98\x80");
    return true;
}

// Reactions open the unified media picker's Emoji component (reaction
// variant). A server section shows its icon in the category rail: the icon
// draws from the shared media atlas and stays clipped to the rail while the
// rail scrolls independently of the grid.
bool probe_reaction_picker_server_icon() noexcept {
    auto& renderer = g_probe.renderer;
    g_probe.failure_stage = 72;
    if (!renderer.ui_scene || !renderer.media_atlas || !synchronize_probe_shell_layout())
        return false;
    constexpr u64 guild_id = 0x5e77;
    constexpr u32 icon_variant = 3;
    UiMediaPickerSection sections[20]{};
    for (u32 i = 0; i < 20; ++i) {
        sections[i] = {0x5e100ull + i, i * 8, 8, utf8_literal("Original Emoji"), true};
        sections[i].visual = static_cast<UiMediaPickerSectionVisual>(1 + i % 10);
    }
    sections[0].label = utf8_literal("Native art studio");
    sections[0].visual = UiMediaPickerSectionVisual::guild;
    sections[0].guild_id = guild_id;
    sections[0].icon_variant = icon_variant;
    const UiMediaPickerSource source{
        nullptr, 0x5e, 1, 160, 1, {sections, 20}, reaction_picker_probe_resolve};
    UiMediaPickerContent content{};
    content.edit = {utf8_literal(""), 0, 0, true, false, false};
    content.placeholder = utf8_literal("Find the perfect emoji");
    content.account_scope = source.account_scope;
    content.query_generation = source.query_generation;
    content.revision = 1;
    content.kind = UiMediaPickerKind::emoji;
    content.phase = UiMediaPickerPhase::ready;
    content.available_tabs = ui_media_picker_tab_bit(UiMediaPickerKind::emoji);
    content.variant = UiMediaPickerVariant::reaction;
    // Solid original artwork, distinct from every panel and glyph colour.
    u8 pixels[32 * 32 * 4]{};
    for (u32 i = 0; i < 32 * 32; ++i) {
        pixels[i * 4] = 36;
        pixels[i * 4 + 1] = 196;
        pixels[i * 4 + 2] = 160;
        pixels[i * 4 + 3] = 255;
    }
    const Color icon{36 / 255.f, 196 / 255.f, 160 / 255.f, 1};
    if (!renderer_upload_media_rgba8(&renderer, media_asset_guild_icon(guild_id, icon_variant, false), 32,
                                     32, 128, {pixels, sizeof(pixels)})
             .ok())
        return false;
    UiMediaPickerScene scene{};
    const auto present = [&]() noexcept {
        renderer.media_picker_scene = &scene;
        renderer.retained_presentation.valid = false;
        return rebuild_probe_content_text() && probe_present(&renderer, kDefaultTheme);
    };
    // The rail icon is one primary-atlas quad over exactly its slot, batched
    // under the slot's rail clip, and its pixels show the uploaded artwork.
    const auto drawn = [&](const UiMediaPickerRailMediaSlot& slot) noexcept {
        bool queued = false;
        for (u32 b = renderer.media_picker_media_first_batch; b < renderer.media_picker_media_end_batch;
             ++b) {
            const auto& batch = renderer.media_draw.batch_data[b];
            if (!ui_rect_equal(batch.clip, slot.clip))
                continue;
            for (u32 q = batch.first_quad; q < batch.first_quad + batch.quad_count; ++q) {
                const auto& quad = renderer.media_draw.quad_data[q];
                queued |= renderer.media_draw.domain_data[q] == 0 &&
                          ui_rect_equal({quad.left, quad.top, quad.right, quad.bottom}, slot.rectangle);
            }
        }
        const ProbePixelSample sample{
            {(slot.clip.left + slot.clip.right) * .5f, (slot.clip.top + slot.clip.bottom) * .5f}, icon, 8};
        return queued && probe_exact_pixels({&sample, 1});
    };
    const auto run = [&]() noexcept {
        const auto viewport = ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::content);
        const UiRect anchor{viewport.right - 48, viewport.top + 24, viewport.right - 24, viewport.top + 48};
        g_probe.failure_stage = 73;
        UiMediaPickerRailMediaSlot slot{};
        if (!ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) ||
            scene.variant != UiMediaPickerVariant::reaction || !ui_rect_has_area(scene.rail) ||
            !ui_media_picker_scene_rail_media(scene, 0, &slot) || slot.guild_id != guild_id ||
            slot.variant != icon_variant || slot.animated || !ui_rect_equal(slot.clip, slot.rectangle) ||
            ui_media_picker_scene_rail_media(scene, 1, &slot))
            return false;
        g_probe.failure_stage = 74;
        if (!ui_media_picker_scene_rail_media(scene, 0, &slot) || !present() || !drawn(slot))
            return false;
        // Scroll only the rail, until the icon is cut in half at its top edge.
        g_probe.failure_stage = 75;
        const float grid_scroll = scene.scroll_dips;
        const float cut = slot.rectangle.top - scene.rail.top + ui_rect_height(slot.rectangle) * .5f;
        UiMediaPickerRailMediaSlot clipped{};
        if (!ui_media_picker_scene_scroll(&scene, cut, true) || scene.scroll_dips != grid_scroll ||
            !ui_media_picker_scene_rail_media(scene, 0, &clipped) || clipped.clip.top != scene.rail.top ||
            clipped.rectangle.top >= scene.rail.top || clipped.clip.bottom != clipped.rectangle.bottom)
            return false;
        g_probe.failure_stage = 76;
        return present() && drawn(clipped);
    };
    const auto* previous = renderer.media_picker_scene;
    const bool passed = run();
    renderer.media_picker_scene = previous;
    renderer.retained_presentation.valid = false;
    ui_media_picker_scene_release(&scene);
    if (!passed || !rebuild_probe_content_text() || !probe_present(&renderer, kDefaultTheme))
        return false;
    log_write(LogLevel::info, "probe", "reaction_picker_server_icon_rail_media_and_clip_passed");
    return true;
}

// Popup overlays through the hardware path: keyboard reveal, attachment and
// text edit menus, plugin and settings popups, then the compact hover bar.
bool probe_popup_overlays() noexcept {
    g_probe.failure_stage = 71;
    const bool initialized = popup_scene_initialize_owned(
        &g_probe.popup_scene, kProbePopupItemCapacity,
        Slice<UiWidget>{g_probe.popup_widgets, 2 + kProbePopupItemCapacity * kPopupWidgetsPerItem},
        Slice<UiLayoutNode>{g_probe.popup_layout_nodes, 2 + kProbePopupItemCapacity * kPopupWidgetsPerItem});
    if (!initialized || !renderer_bind_popup_scene(&g_probe.renderer, &g_probe.popup_scene))
        return false;
    const UiRect window{0, 0, g_probe.renderer.width * 96.0f / g_probe.renderer.dpi,
                        g_probe.renderer.height * 96.0f / g_probe.renderer.dpi};
    const UiRect messages = g_probe.message_scene.viewport;
    const UiRect viewport = ui_rect_intersection(
        window, {ui_rect_width(messages) >= 448.0f ? messages.left : g_probe.shell_navigation_scene.viewport.left,
                 messages.top, messages.right, messages.bottom});

    // Keyboard reveal must update the retained popup's clipped text through
    // the real hardware presentation path, including reverse wrap.
    g_probe.failure_stage = 87;
    PopupItem suggestions[8]{};
    for (u32 i = 0; i < 8; ++i) {
        suggestions[i] = {i == 0   ? utf8_literal("First result")
                          : i == 7 ? utf8_literal("Last result")
                                   : utf8_literal("Navigation result"),
                          1000u + i, popup_item_enabled, 0};
    }
    const UiRect compact{viewport.left, viewport.top,
                         viewport.left + minimum(288.0f, ui_rect_width(viewport)),
                         viewport.top + minimum(128.0f, ui_rect_height(viewport))};
    if (!popup_scene_open(&g_probe.popup_scene, PopupKind::autocomplete, 78, {suggestions, 8},
                          {compact.left + 8, compact.bottom - 8, compact.right - 8, compact.bottom - 7},
                          compact, ui_rect_width(compact) - 16, true)
             .ok())
        return false;
    for (u32 pass = 0; pass < 2; ++pass) {
        bool changed = false;
        const bool selected = pass == 0 ? popup_scene_move_selection(&g_probe.popup_scene, -1, &changed)
                                        : popup_scene_select_index(&g_probe.popup_scene, 0, &changed);
        const auto* row = popup_scene_layout_node(g_probe.popup_scene,
                                                  g_probe.popup_scene.rows[pass == 0 ? 7 : 0].widget);
        const auto* panel = popup_scene_layout_node(g_probe.popup_scene, g_probe.popup_scene.panel);
        if (!selected || !changed || !row || !panel || row->rectangle.top < panel->content_rectangle.top ||
            row->rectangle.bottom > panel->content_rectangle.bottom || !rebuild_probe_content_text() ||
            !probe_present(&g_probe.renderer, kDefaultTheme))
            return false;
        bool row_ink = false;
        for (u32 b = g_probe.renderer.overlay_text_first_batch; b < g_probe.text.batch_count; ++b) {
            const auto& batch = g_probe.text.batches[b];
            for (u32 q = batch.first_quad; q < batch.first_quad + batch.quad_count; ++q) {
                const auto& quad = g_probe.text.quads[q];
                if (quad.top < panel->content_rectangle.top ||
                    quad.bottom > panel->content_rectangle.bottom)
                    return false;
                row_ink = row_ink || ui_rect_contains(row->rectangle, (quad.left + quad.right) * 0.5f,
                                                      (quad.top + quad.bottom) * 0.5f);
            }
        }
        if (!row_ink)
            return false;
    }
    popup_scene_close(&g_probe.popup_scene);
    if (!rebuild_probe_content_text())
        return false;

    // Use the same per-file view as the shipping attachment controls.
    PopupItem file_items[kAttachmentFileMenuItems];
    for (u32 spoiler = 0; spoiler < 2; ++spoiler) {
        g_probe.failure_stage = 88;
        if (attachment_file_menu(utf8_literal("旅行の写真.png"), spoiler != 0,
                                 {file_items, kAttachmentFileMenuItems}) != kAttachmentFileMenuItems ||
            !popup_scene_open(
                 &g_probe.popup_scene, PopupKind::popover, 79, {file_items, kAttachmentFileMenuItems},
                 {viewport.right - 24, viewport.bottom - 24, viewport.right - 23, viewport.bottom - 23},
                 viewport, 360, true)
                 .ok() ||
            !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme))
            return false;
        PopupItemLayout control{};
        if (!popup_scene_resolve_item_layout(g_probe.popup_scene, 1, &control) || !control.visible ||
            popup_scene_selected_action(g_probe.popup_scene) != popup_action_attachment_spoiler ||
            ((g_probe.popup_scene.items[1].flags & popup_item_checked) != 0) != (spoiler != 0))
            return false;
        const auto hit = popup_scene_hit_test(
            g_probe.popup_scene, (control.visible_rectangle.left + control.visible_rectangle.right) * 0.5f,
            (control.visible_rectangle.top + control.visible_rectangle.bottom) * 0.5f);
        if (!hit.enabled || hit.action != popup_action_attachment_spoiler)
            return false;
    }
    popup_scene_close(&g_probe.popup_scene);
    if (!rebuild_probe_content_text())
        return false;
    log_write(LogLevel::info, "probe", "attachment_spoiler_menu_hardware_passed");
    PopupItem edit_items[kTextEditMenuItems]{};
    if (!text_edit_menu_build({false, false, true, true, true, true, true, false},
                              {edit_items, kTextEditMenuItems}) ||
        !popup_scene_open(
             &g_probe.popup_scene, PopupKind::context_menu, 80, {edit_items, kTextEditMenuItems},
             {viewport.right - 24, viewport.bottom - 24, viewport.right - 23, viewport.bottom - 23},
             viewport, 260)
             .ok() ||
        !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        popup_scene_selected_action(g_probe.popup_scene) != static_cast<u32>(TextEditCommand::cut))
        return false;
    UiTextCommand edit_commands[14]{};
    UiTextCommandStream edit_stream{};
    ui_text_command_stream_initialize(&edit_stream, {edit_commands, 14});
    if (!popup_text_presentation_build(g_probe.popup_scene, kDefaultTheme, &edit_stream) ||
        edit_stream.count != 14)
        return false;
    for (u32 i = 0; i < edit_stream.count; i += 2) {
        if (edit_commands[i].horizontal_alignment != UiTextAlignment::end ||
            edit_commands[i].rectangle.left < edit_commands[i + 1].rectangle.right)
            return false;
    }
    const auto menu_builds = g_probe.renderer.paint_stream_builds;
    if (!probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.paint_stream_builds != menu_builds)
        return false;
    popup_scene_close(&g_probe.popup_scene);
    if (!rebuild_probe_content_text())
        return false;
    log_write(LogLevel::info, "probe", "text_edit_menu_shortcuts_hardware_and_retention_passed");
    if (!plugin_ui_probe(g_probe.renderer, g_probe.popup_scene, probe_capture_frame))
        return false;
    if (!plugin_navigation_ui_probe(g_probe.renderer, g_probe.popup_scene, probe_capture_frame))
        return false;
    if (!plugin_catalog_ui_probe(g_probe.renderer, probe_capture_frame))
        return false;
    if (!settings_choice_ui_probe(g_probe.renderer, g_probe.popup_scene, probe_capture_frame))
        return false;

    // Exercise the compact hover bar through the same hardware text path.
    // Its 28-DIP targets intentionally retain the same 24-DIP emoji em used
    // by the full picker and the 44-DIP context-menu targets.
    g_probe.failure_stage = 84;
    const MessageSceneRowWidget* bar_row =
        g_probe.message_scene.count != 0 ? &g_probe.message_scene.rows[0] : nullptr;
    const MessageRecord* bar_message =
        bar_row ? message_store_resolve(g_probe.message_view.store, bar_row->message) : nullptr;
    const MessageActionBarResult bar_opened =
        bar_message ? message_action_bar_open(
                          &g_probe.message_action_bar, g_probe.message_scene,
                          MessageSceneHit{bar_row->message_id, bar_row->widget, bar_row->source_index},
                          *bar_message)
                    : MessageActionBarResult{};
    if (!bar_opened.ok() ||
        !renderer_bind_message_action_bar(&g_probe.renderer, &g_probe.message_action_bar) ||
        !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme)) {
        return false;
    }
    g_probe.failure_stage = 85;
    bool hover_glyph = false;
    for (u32 batch_index = g_probe.renderer.overlay_text_first_batch;
         batch_index < g_probe.text.batch_count; ++batch_index) {
        const GlyphDrawBatch& batch = g_probe.text.batches[batch_index];
        if (batch.first_quad > g_probe.text.quad_count ||
            batch.quad_count > g_probe.text.quad_count - batch.first_quad) {
            return false;
        }
        for (u32 index = 0; index < batch.quad_count; ++index) {
            const GlyphQuad& quad = g_probe.text.quads[batch.first_quad + index];
            if (quad.left < g_probe.message_action_bar.rectangle.left ||
                quad.top < g_probe.message_action_bar.rectangle.top ||
                quad.right > g_probe.message_action_bar.rectangle.right ||
                quad.bottom > g_probe.message_action_bar.rectangle.bottom) {
                return false;
            }
            hover_glyph = true;
        }
    }
    for (u32 cell = 0; cell < 3; ++cell) {
        const UiRect owner = g_probe.message_action_bar.items[cell].rectangle;
        UiRect ink{owner.right, owner.bottom, owner.left, owner.top};
        bool found = false;
        for (u32 b = g_probe.renderer.overlay_text_first_batch; b < g_probe.text.batch_count; ++b) {
            const GlyphDrawBatch& batch = g_probe.text.batches[b];
            for (u32 q = batch.first_quad; q < batch.first_quad + batch.quad_count; ++q) {
                const GlyphQuad& quad = g_probe.text.quads[q];
                if (!ui_rect_contains(owner, (quad.left + quad.right) * 0.5f,
                                      (quad.top + quad.bottom) * 0.5f))
                    continue;
                found = true;
                ink.left = minimum(ink.left, quad.left);
                ink.top = minimum(ink.top, quad.top);
                ink.right = maximum(ink.right, quad.right);
                ink.bottom = maximum(ink.bottom, quad.bottom);
            }
        }
        const float dx = (ink.left + ink.right) - (owner.left + owner.right);
        const float dy = (ink.top + ink.bottom) - (owner.top + owner.bottom);
        if (!found || dx < -0.01f || dx > 0.01f || dy < -0.01f || dy > 0.01f)
            return false;
    }
    g_probe.failure_stage = 86;
    bool bar_changed = false;
    const u32 stale_overlay_suppressions = g_probe.renderer.stale_interaction_text_suppressions;
    if (!hover_glyph || ui_rect_height(g_probe.message_action_bar.rectangle) != 32.0f ||
        ui_rect_width(g_probe.message_action_bar.items[0].rectangle) != kMessageActionBarCellExtent ||
        !message_action_bar_close(&g_probe.message_action_bar, &bar_changed) || !bar_changed ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.stale_interaction_text_suppressions != stale_overlay_suppressions + 1u ||
        !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme)) {
        return false;
    }
    g_probe.failure_stage = 0;
    return true;
}

LRESULT CALLBACK probe_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (post_resize_probe_detail::window_message(window, message, wparam, lparam))
        return 0;
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_ENTERSIZEMOVE:
        renderer_set_live_resize(&g_probe.renderer, true);
        return 0;
    case WM_SIZE:
        if (g_probe.renderer_ready && wparam != SIZE_MINIMIZED) {
            ++g_probe.resize_messages;
            const bool resized = renderer_resize(&g_probe.renderer, LOWORD(lparam), HIWORD(lparam));
            const bool shell_ready = resized && synchronize_probe_shell_layout();
            const bool text_ready =
                shell_ready && (!g_probe.renderer.content_text || rebuild_probe_content_text());
            if (text_ready && g_probe.renderer.content_text) {
                g_probe.resize_min_text_quads =
                    minimum(g_probe.resize_min_text_quads, g_probe.renderer.content_text->quad_count);
            }
            if (!text_ready ||
                (g_probe.renderer.content_text && g_probe.renderer.content_text->quad_count == 0) ||
                (g_probe.login_view_bound && !set_probe_login_view(g_probe.login_view)) ||
                !probe_present(&g_probe.renderer, kDefaultTheme)) {
                DestroyWindow(window);
            }
        }
        return 0;
    case WM_EXITSIZEMOVE:
        if (g_probe.renderer_ready) {
            RECT client{};
            if (!GetClientRect(window, &client) ||
                !renderer_finish_resize(&g_probe.renderer,
                                        static_cast<u32>(maximum<LONG>(1, client.right - client.left)),
                                        static_cast<u32>(maximum<LONG>(1, client.bottom - client.top)))) {
                DestroyWindow(window);
            }
        }
        return 0;
    case WM_PAINT: {
        ++g_probe.paint_messages;
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        const bool ok = !g_probe.renderer_ready || probe_present(&g_probe.renderer, kDefaultTheme);
        EndPaint(window, &paint);
        if (!ok) {
            DestroyWindow(window);
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

// Never activated, no taskbar button, opened behind every other window.
constexpr DWORD kProbeWindowExStyle = WS_EX_NOREDIRECTIONBITMAP | probe_window::kExtendedStyle;

bool create_probe_window() noexcept {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t kClassName[] = L"MaiCordUiProbeWindow";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = 0;
    window_class.lpfnWndProc = probe_window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kClassName;
    if (!RegisterClassExW(&window_class)) {
        return false;
    }
    RECT bounds{0, 0, 1200, 970};
    AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, kProbeWindowExStyle);
    g_probe.window = CreateWindowExW(kProbeWindowExStyle, kClassName, L"MaiCord M38 UI Probe",
                                     WS_OVERLAPPEDWINDOW, 32, 32, bounds.right - bounds.left,
                                     bounds.bottom - bounds.top, nullptr, nullptr, instance, nullptr);
    return g_probe.window != nullptr;
}

bool once_requested() noexcept {
    wchar_t value[2]{};
    return GetEnvironmentVariableW(L"MAICORD_UI_PROBE_ONCE", value, 2) == 1 && value[0] == L'1';
}

bool device_owner_empty(const D3D11DeviceOwner& owner) noexcept {
    return !owner.device && !owner.context && !owner.context1 && !owner.swap_chain && !owner.swap_chain2 &&
           !owner.composition_device && !owner.composition_target && !owner.composition_visual &&
           !owner.target && !owner.frame_latency_waitable && owner.buffer_width == 0 &&
           owner.buffer_height == 0 && owner.resize_buffer_reallocations == 0;
}

bool device_owner_ready(const D3D11Renderer& renderer) noexcept {
    const D3D11DeviceOwner& owner = renderer.graphics;
    return owner.device && owner.context && owner.context1 && owner.swap_chain && owner.swap_chain2 &&
           owner.composition_device && owner.composition_target && owner.composition_visual &&
           owner.target && owner.frame_latency_waitable && owner.buffer_width == renderer.width &&
           owner.buffer_height == renderer.height && owner.feature_level >= static_cast<u32>(D3D_FEATURE_LEVEL_11_0);
}

bool draw_pipeline_empty(const D3D11DrawPipelineOwner& owner) noexcept {
    return !owner.ui_vertex_shader && !owner.ui_pixel_shader && !owner.ui_input_layout &&
           !owner.ui_instance_buffer && !owner.ui_frame_buffer && !owner.ui_rasterizer_state &&
           !owner.ui_blend_state && !owner.glyph_vertex_shader && !owner.glyph_pixel_shader &&
           !owner.media_pixel_shader && !owner.glyph_input_layout && !owner.glyph_instance_buffer &&
           !owner.media_instance_buffer && !owner.glyph_sampler && owner.glyph_instance_capacity == 0;
}

bool draw_pipeline_ready(const D3D11Renderer& renderer) noexcept {
    const D3D11DrawPipelineOwner& owner = renderer.pipeline;
    return owner.ui_vertex_shader && owner.ui_pixel_shader && owner.ui_input_layout &&
           owner.ui_instance_buffer && owner.ui_frame_buffer && owner.ui_rasterizer_state &&
           owner.ui_blend_state && owner.glyph_vertex_shader && owner.glyph_pixel_shader &&
           owner.media_pixel_shader && owner.glyph_input_layout && owner.glyph_instance_buffer &&
           owner.media_instance_buffer && owner.glyph_sampler &&
           owner.glyph_instance_capacity >= kRendererGlyphCapacity;
}

bool glyph_text_owner_empty(const D3D11GlyphTextOwner& owner) noexcept {
    bool faces_empty = owner.font_face_count == 0;
    for (u32 index = 0; owner.font_faces && index < owner.font_face_capacity; ++index) {
        faces_empty = faces_empty && !owner.font_faces[index].native_face.slot && owner.font_faces[index].id == 0 &&
                      owner.font_faces[index].flags == 0;
        faces_empty = faces_empty && owner.font_faces[index].empty_glyph_plus_one == 0;
    }
    return !owner.color_batch && !owner.storage.base && !owner.system.native_text && !owner.system.native_fonts &&
           owner.system.em_size == 0.0f && !owner.color_glyphs && !owner.title.glyphs &&
           !owner.title.runs && !owner.title.clusters && !owner.allocator.entries &&
           !owner.allocator.rows && !owner.gpu.texture && !owner.gpu.texture_view &&
           !owner.gpu.texture_uav && !owner.gpu.command_buffer && !owner.gpu.command_view &&
           !owner.gpu.raster_shader && owner.gpu.dispatched_pixels == 0 &&
           owner.gpu.dispatched_commands == 0 && owner.gpu.dispatch_count == 0 && owner.gpu.rejected == 0 &&
           owner.title_quad_count == 0 && owner.cache_builds == 0 && !owner.ready && faces_empty;
}

bool glyph_text_owner_ready(const D3D11Renderer& renderer) noexcept {
    const D3D11GlyphTextOwner& owner = renderer.glyph_text;
    const bool system_ready = owner.system.native_fonts && owner.system.native_text && owner.system.native_format;
    return owner.storage.base && owner.utf16 && owner.glyphs && owner.runs && owner.clusters &&
           owner.path_commands && owner.atlas_entries && owner.atlas_rows && owner.title_quads &&
           owner.font_faces && system_ready && owner.title.glyphs && owner.title.runs &&
           owner.title.clusters && owner.allocator.entries && owner.allocator.rows && owner.gpu.texture &&
           owner.gpu.texture_view && owner.gpu.texture_uav && owner.gpu.command_buffer &&
           owner.gpu.command_view && owner.gpu.raster_shader && owner.ready;
}

bool gpu_timer_empty(const D3D11GpuTimerOwner& owner) noexcept {
    bool empty = !owner.enabled && owner.total_microseconds == 0 && owner.maximum_microseconds == 0 &&
                 owner.last_microseconds == 0 && owner.cursor == 0 && owner.samples == 0 &&
                 owner.skipped == 0 && owner.disjoint == 0 && owner.errors == 0;
    for (u32 index = 0; index < kD3D11GpuTimerFrameCount; ++index) {
        const D3D11GpuTimerFrame& frame = owner.frames[index];
        empty = empty && !frame.disjoint && !frame.begin && !frame.end && !frame.pending;
    }
    return empty;
}

bool message_hit_cache_empty(const MessageHitTestCacheOwner& owner) noexcept {
    return !owner.geometry && !owner.native_format && !owner.storage.base && owner.message_id == 0 &&
           owner.measurement_identity == 0 && owner.message_revision == 0 && owner.command_index == 0 &&
           owner.utf16_units == 0 && owner.builds == 0 && owner.hits == 0 && owner.rejected == 0 &&
           owner.width == 0.0f && owner.height == 0.0f && owner.em_size == 0.0f;
}

bool composer_layout_empty(const TextEditLayoutOwner& owner) noexcept {
    return !owner.geometry && !owner.format && owner.text_signature == 0 && owner.text_bytes == 0 &&
           owner.anchor == 0 && owner.caret_offset == 0 && owner.utf16_units == 0 &&
           owner.selection_count == 0 && owner.builds == 0 && owner.hits == 0 && owner.rejected == 0 &&
           owner.origin_x == 0.0f && owner.origin_y == 0.0f && owner.width == 0.0f &&
           owner.height == 0.0f && owner.em_size == 0.0f && !owner.active;
}

bool content_text_retention_empty(const ContentTextRetentionOwner& owner) noexcept {
    const ContentTextRetentionKey& key = owner.key;
    return !owner.valid && owner.builds == 0 && owner.hits == 0 && owner.misses == 0 &&
           owner.invalidations == 0 && key.shell_signature == 0 && key.message_signature == 0 &&
           key.popup_signature == 0 && key.theme_signature == 0 && key.selection_message_id == 0 &&
           key.selection_command_identity == 0 && key.selection_revision == 0 &&
           key.selection_command_index == 0 && key.selection_anchor == 0 && key.selection_caret == 0 &&
           key.dpi == 0 && key.atlas_evicted_entries == 0 && key.atlas_full_resets == 0 &&
           !key.include_messages;
}

bool gpu_timer_ready(const D3D11Renderer& renderer) noexcept {
    const D3D11GpuTimerOwner& owner = renderer.gpu_timer;
    if (!owner.enabled) {
        return false;
    }
    for (u32 index = 0; index < kD3D11GpuTimerFrameCount; ++index) {
        const D3D11GpuTimerFrame& frame = owner.frames[index];
        if (!frame.disjoint || !frame.begin || !frame.end) {
            return false;
        }
    }
    return true;
}

bool resize_stress_requested() noexcept {
    wchar_t value[2]{};
    return GetEnvironmentVariableW(L"MAICORD_UI_PROBE_RESIZE_STRESS", value, 2) == 1 && value[0] == L'1';
}

bool run_resize_stress() noexcept {
    g_probe.failure_stage = 110;
    constexpr u32 kHalfSteps = 32;
    constexpr u32 kSteps = kHalfSteps * 2;
    constexpr LONG kInitialWidth = 1200;
    constexpr LONG kInitialHeight = 970;
    constexpr LONG kFinalWidth = kInitialWidth;
    constexpr LONG kFinalHeight = 360;
    const u32 clipped_updates_before = g_probe.renderer.resize_clipped_updates;
    const u32 reallocations_before = g_probe.renderer.graphics.resize_buffer_reallocations;
    const u32 resize_messages_before = g_probe.resize_messages;
    const u32 paint_messages_before = g_probe.paint_messages;
    const u32 painted_frames_before = g_probe.renderer.painted_frames;
    const u32 text_rejections_before =
        g_probe.renderer.content_text ? g_probe.renderer.content_text->rejected : 0;
    u64 geometry_signatures[kHalfSteps + 1]{};
    geometry_signatures[0] = probe_resize_geometry_signature();
    g_probe.resize_min_text_quads = ~0u;

    renderer_set_live_resize(&g_probe.renderer, true);
    for (u32 step = 1; step <= kSteps; ++step) {
        const u32 distance_step = step <= kHalfSteps ? step : kSteps - step;
        const LONG client_width =
            kInitialWidth -
            static_cast<LONG>((static_cast<u64>(kInitialWidth - kFinalWidth) * distance_step) / kHalfSteps);
        const LONG client_height =
            kInitialHeight -
            static_cast<LONG>((static_cast<u64>(kInitialHeight - kFinalHeight) * distance_step) /
                              kHalfSteps);
        RECT bounds{0, 0, client_width, client_height};
        if (!AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, kProbeWindowExStyle) ||
            !SetWindowPos(g_probe.window, nullptr, 0, 0, bounds.right - bounds.left,
                          bounds.bottom - bounds.top, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER) ||
            !UpdateWindow(g_probe.window) || !IsWindow(g_probe.window)) {
            log_u64(LogLevel::error, "probe", "resize_stress_failed_step", step);
            return false;
        }
        const u64 signature = probe_resize_geometry_signature();
        if (step <= kHalfSteps) {
            geometry_signatures[distance_step] = signature;
        } else if (geometry_signatures[distance_step] != signature) {
            log_u64(LogLevel::error, "probe", "resize_geometry_mismatch_step", step);
            return false;
        }
    }

    RECT client{};
    g_probe.failure_stage = 111;
    if (!GetClientRect(g_probe.window, &client) || client.right <= client.left ||
        client.bottom <= client.top ||
        !renderer_finish_resize(&g_probe.renderer, static_cast<u32>(client.right - client.left),
                                static_cast<u32>(client.bottom - client.top)) ||
        !probe_present(&g_probe.renderer, kDefaultTheme)) {
        return false;
    }

    g_probe.failure_stage = 112;
    const u32 resize_messages = g_probe.resize_messages - resize_messages_before;
    const u32 paint_messages = g_probe.paint_messages - paint_messages_before;
    if (resize_messages != kSteps) {
        g_probe.failure_stage = 113;
        return false;
    }
    if (paint_messages > resize_messages) {
        g_probe.failure_stage = 114;
        return false;
    }
    // A client that still fits the buffers is clipped, never reallocated:
    // shrinking and returning to the initial size keeps the original chain.
    if (g_probe.renderer.resize_clipped_updates - clipped_updates_before != kSteps ||
        g_probe.renderer.graphics.resize_buffer_reallocations != reallocations_before) {
        log_u64(LogLevel::error, "probe", "resize_clipped_updates",
                g_probe.renderer.resize_clipped_updates - clipped_updates_before);
        log_u64(LogLevel::error, "probe", "resize_buffer_reallocations",
                g_probe.renderer.graphics.resize_buffer_reallocations - reallocations_before);
        g_probe.failure_stage =
            g_probe.renderer.resize_clipped_updates - clipped_updates_before != kSteps ? 115 : 116;
        return false;
    }
    // Each WM_SIZE must paint once. The final present is deliberately allowed
    // to be a retained no-op after returning to the initial geometry.
    if (g_probe.renderer.painted_frames - painted_frames_before != kSteps) {
        log_u64(LogLevel::error, "probe", "resize_painted_frames",
                g_probe.renderer.painted_frames - painted_frames_before);
        log_u64(LogLevel::error, "probe", "resize_expected_painted_frames", kSteps);
        log_u64(LogLevel::error, "probe", "resize_paint_messages", paint_messages);
        g_probe.failure_stage = 117;
        return false;
    }
    if (g_probe.resize_min_text_quads == ~0u || g_probe.resize_min_text_quads == 0) {
        g_probe.failure_stage = 118;
        return false;
    }
    if (g_probe.renderer.content_text &&
        g_probe.renderer.content_text->rejected != text_rejections_before) {
        g_probe.failure_stage = 119;
        return false;
    }
    if (g_probe.renderer.live_resize) {
        g_probe.failure_stage = 120;
        return false;
    }
    g_probe.failure_stage = 0;
    return true;
}

bool initialize_probe_renderer() noexcept {
    LARGE_INTEGER begin{}, end{}, frequency{};
    ULONG64 first_cycles = 0, last_cycles = 0;
    QueryPerformanceFrequency(&frequency);
    QueryThreadCycleTime(GetCurrentThread(), &first_cycles);
    QueryPerformanceCounter(&begin);
    const bool result = renderer_initialize(&g_probe.renderer, g_probe.window);
    QueryPerformanceCounter(&end);
    QueryThreadCycleTime(GetCurrentThread(), &last_cycles);
    log_u64(LogLevel::info, "bootstrap", "renderer_init_us",
            static_cast<u64>(end.QuadPart - begin.QuadPart) * 1'000'000 / frequency.QuadPart);
    log_u64(LogLevel::info, "bootstrap", "renderer_init_cycles", last_cycles - first_cycles);
    return result;
}

bool probe_bootstrap_title() noexcept {
    auto& renderer = g_probe.renderer;
    if (!ui_scene_initialize_bootstrap(&g_probe.shell_scene))
        return false;
    // Use the standalone draw path: the account scene has not been bound yet.
    const auto draw = [&]() noexcept {
        if (!standalone_title_layout(&g_probe.shell_scene,
                                     static_cast<float>(renderer.width) * 96 / renderer.dpi,
                                     static_cast<float>(renderer.height) * 96 / renderer.dpi) ||
            !renderer_bind_ui_scene(&renderer, &g_probe.shell_scene))
            return false;
        const float clear[]{0, 0, 0, 1};
        renderer.graphics.context->ClearRenderTargetView(renderer.graphics.target, clear);
        return d3d11_renderer_draw_stream(&renderer, kDefaultTheme, true, true, nullptr, 10000);
    };
    // Exercise a DPI change before any visible title, then first draw and reuse.
    const bool pending = renderer.glyph_text.title.run_count == 0;
    renderer_set_dpi(&renderer, 144);
    if (pending && (renderer.glyph_text.title.run_count || renderer.glyph_text.title_quad_count ||
                    renderer.glyph_text.cache_builds || renderer.glyph_text.gpu.dispatch_count))
        return false;
    if (!renderer.glyph_text.ready || !draw() || !renderer.glyph_text.title_quad_count ||
        !probe_capture_frame(L"bootstrap-144.bmp"))
        return false;
    const u32 builds = renderer.glyph_text.cache_builds;
    if (!draw() || renderer.glyph_text.cache_builds != builds)
        return false;
    renderer_set_dpi(&renderer, 192);
    if (!renderer.glyph_text.ready || !draw() || !renderer.glyph_text.title_quad_count ||
        !probe_capture_frame(L"bootstrap-192.bmp"))
        return false;
    const u32 builds_192 = renderer.glyph_text.cache_builds;
    if (!draw() || renderer.glyph_text.cache_builds != builds_192)
        return false;
    renderer_set_dpi(&renderer, 96);
    return renderer.glyph_text.ready && draw() && probe_capture_frame(L"bootstrap-96.bmp");
}

bool probe_long_embed_text() noexcept {
    constexpr u64 id = 1'270'000'000'000'000'000ull;
    const auto substitutions = g_probe.text.substitutions;
    constexpr u32 counts[]{150, 300};
    for (u32 count : counts) {
        char source[4096]{};
        ProtocolWriter writer{{reinterpret_cast<u8*>(source), sizeof(source)}, 0};
        for (u32 i = 0; i < count; ++i) {
            if (!protocol_writer_append_byte(&writer, '`') ||
                !protocol_writer_append_u64(&writer, 3'308'051u + i * 19'741u) ||
                !protocol_writer_append_literal(&writer, "`" ) ||
                (i + 1 < count && !protocol_writer_append_literal(&writer, ", "))) return false;
        }
        if (!message_timeline_reset(&g_probe.message_view.timeline) ||
            !add_message(id, utf8_literal("Thanks, see you there :heart:")) ||
            !message_view_runtime_reindex(&g_probe.message_view, ui_extent_units(57), 1, false).ok()) return false;
        const auto found = message_store_find(g_probe.message_view.store, id);
        const auto* record = found.ok() ? message_store_resolve(g_probe.message_view.store, found.handle) : nullptr;
        GatewayMessageEmbed embed{};
        embed.type = GatewayMessageEmbedType::rich;
        embed.title = utf8_literal("Wren042 rolled 150d10000000!");
        embed.description = {source, writer.used};
        embed.has_color = true; embed.color = 0x5865F2;
        GatewayMessageEvent event{}; event.id = id; event.fields = gateway_message_field_embeds;
        event.embeds = {&embed, 1};
        if (!record || !message_rich_store_apply(&g_probe.rich_store, id, record->revision, event).ok() ||
            !synchronize_probe_shell_layout() || !rebuild_probe_content_text() ||
            g_probe.text.substitutions != substitutions) return false;
        bool found_description = false, found_heart = false;
        for (u32 i = 0; i < g_probe.message_text.command_count; ++i) {
            const auto& command = g_probe.message_text.commands[i];
            // Plain runs between the code spans carry size-only spans, so count the code spans.
            u32 code_spans = 0;
            for (u32 span = 0; span < command.styles.count; ++span)
                code_spans += (command.styles[span].flags & (text_style_monospace | text_style_code_block)) != 0;
            found_description = found_description || (code_spans == count && command.text.bytes > count * 8 &&
                command.measured_height > 100);
            found_heart = found_heart || (command.text.bytes == utf8_literal("Thanks, see you there \342\235\244\357\270\217").bytes);
        }
        if (!found_description || !found_heart ||
            !probe_capture_frame(count == 150 ? L"embed-150-code-numbers.bmp" : L"embed-300-code-numbers.bmp")) return false;
    }
    log_write(LogLevel::info, "probe", "long_embed_150_and_300_inline_code_without_cache_substitution_passed");
    return true;
}

int run_probe() noexcept {
    SceneMemoryMeasurement memory_measurement;
    scene_memory = &memory_measurement;
    struct SceneRelease {
        ~SceneRelease() noexcept {
            message_scene_release(&g_probe.message_scene);
        }
    } scene_release;
    (void)log_open();
    wchar_t bootstrap_only[2]{};
    const bool native_bootstrap =
        GetEnvironmentVariableW(L"MAICORD_NATIVE_BOOTSTRAP_PROBE", bootstrap_only, 2) == 1 &&
        bootstrap_only[0] == L'1';
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 246;
    }
    // The prestarted device, draw pipeline and glyph atlas were adopted, not
    // recreated on this thread.
    if (g_probe.renderer.startup_prestart != 7) {
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 229;
    }
    if (!memory_measurement.initialize(g_probe.renderer))
        return 244;
    wchar_t bindings_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_COLR_BINDING_PROBE", bindings_only, 2) == 1 &&
        bindings_only[0] == L'1') {
        const bool valid = colr_binding_probe(g_probe.renderer.graphics.device, g_probe.renderer.graphics.context);
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 245;
    }
    const bool device_ready = device_owner_ready(g_probe.renderer);
    const bool pipeline_ready = draw_pipeline_ready(g_probe.renderer);
    const bool glyph_ready = glyph_text_owner_ready(g_probe.renderer);
    const bool timer_ready = gpu_timer_ready(g_probe.renderer);
    if (!device_ready || !pipeline_ready || !glyph_ready || !timer_ready) {
        log_write(LogLevel::error, "probe", "post_bootstrap_readiness_failed");
        log_u64(LogLevel::error, "probe", "bootstrap_device_ready", device_ready);
        log_u64(LogLevel::error, "probe", "bootstrap_pipeline_ready", pipeline_ready);
        log_u64(LogLevel::error, "probe", "bootstrap_glyph_ready", glyph_ready);
        log_u64(LogLevel::error, "probe", "bootstrap_timer_ready", timer_ready);
        log_u64(LogLevel::error, "probe", "bootstrap_timer_enabled", g_probe.renderer.gpu_timer.enabled);
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 9;
    }
    if (native_bootstrap) {
        const auto& system = g_probe.renderer.glyph_text.system;
        if (!system.native_fonts || !system.native_text || !system.native_format ||
            !g_probe.renderer.glyph_text.color_glyphs) {
            renderer_shutdown(&g_probe.renderer);
            DestroyWindow(g_probe.window);
            return 243;
        }
        log_write(LogLevel::info, "probe", "native_renderer_bootstrap_passed");
    }
    wchar_t plugin_catalog_option[2]{};
    const bool plugin_catalog_requested =
        GetEnvironmentVariableW(L"MAICORD_PLUGIN_CATALOG_UI_PROBE", plugin_catalog_option, 2) == 1 &&
        plugin_catalog_option[0] == L'1';
    wchar_t inbox_option[2]{};
    const bool inbox_requested = GetEnvironmentVariableW(L"MAICORD_INBOX_UI_PROBE", inbox_option, 2) == 1 &&
                                 inbox_option[0] == L'1';
    wchar_t notification_settings_option[2]{};
    const bool notification_settings_requested =
        GetEnvironmentVariableW(L"MAICORD_NOTIFICATION_SETTINGS_UI_PROBE", notification_settings_option,
                                2) == 1 &&
        notification_settings_option[0] == L'1';
    wchar_t flag_surfaces_option[2]{};
    const bool flag_surfaces_requested =
        GetEnvironmentVariableW(L"MAICORD_REGIONAL_FLAG_SURFACES_PROBE", flag_surfaces_option, 2) == 1 &&
        flag_surfaces_option[0] == L'1';
    if (!flag_surfaces_requested &&
        GetEnvironmentVariableW(L"MAICORD_REGIONAL_FLAG_REFERENCE_DIRECTORY", nullptr, 0)) {
        const bool valid =
            regional_flag_ui_probe(g_probe.renderer, g_probe.popup_scene, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "regional_flag_gpu_probe_passed" : "regional_flag_gpu_probe_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 242;
    }
    wchar_t glyph_cache_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_GLYPH_CACHE_METRICS", glyph_cache_only, 2)) {
        const bool valid = probe_glyph_cache(g_probe.renderer, probe_capture_frame);
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 247;
    }
    wchar_t title_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_BOOTSTRAP_TITLE_PROBE", title_only, 2) == 1) {
        const bool valid = title_only[0] == L'2' || probe_bootstrap_title();
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "bootstrap_title_passed" : "bootstrap_title_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 246;
    }
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 9;
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 9;
    }
    if (!message_composer_initialize(
            &g_probe.message_composer,
            Slice<char>{g_probe.message_composer_text, sizeof(g_probe.message_composer_text)})) {
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 2;
    }
    message_composer_reset(&g_probe.message_composer, 111, false);
    if (message_composer_insert_utf8(&g_probe.message_composer, Utf8View{"a\xF0\x9F\x9A\x80"
                                                                         "b",
                                                                         6}) !=
            MessageComposerResult::changed ||
        message_composer_set_selection(&g_probe.message_composer, 1, 5) != MessageComposerResult::changed) {
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 2;
    }
    g_probe.message_composer.focused = true;
    if (!ui_scene_initialize_bootstrap(&g_probe.shell_scene) ||
        !guild_rail_scene_initialize(
            &g_probe.guild_rail_scene,
            Slice<GuildRailRowWidget>{g_probe.guild_rail_rows, kProbeGuildCapacity},
            Slice<UiWidget>{g_probe.guild_rail_widgets, kProbeGuildCapacity + 2},
            Slice<UiLayoutNode>{g_probe.guild_rail_layout_nodes, kProbeGuildCapacity + 2}) ||
        !shell_navigation_scene_initialize(
            &g_probe.shell_navigation_scene,
            Slice<ShellNavigationRowWidget>{g_probe.shell_navigation_rows, kProbeChannelCapacity},
            Slice<UiWidget>{g_probe.shell_navigation_widgets, kProbeChannelCapacity * 4 + 2},
            Slice<UiLayoutNode>{g_probe.shell_navigation_layout_nodes, kProbeChannelCapacity * 4 + 2}) ||
        !message_composer_scene_initialize(
            &g_probe.message_composer_scene,
            Slice<UiWidget>{g_probe.message_composer_widgets, kMessageComposerWidgetCapacity},
            Slice<UiLayoutNode>{g_probe.message_composer_layout_nodes, kMessageComposerWidgetCapacity}) ||
        !initialize_probe_message_scene() || !synchronize_probe_shell_layout()) {
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 2;
    }
    wchar_t account_menu_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_ACCOUNT_MENU_UI_PROBE", account_menu_only, 2) == 1 &&
        account_menu_only[0] == L'1') {
        const bool valid =
            initialize_fixture(true) && account_menu_ui_probe(g_probe.renderer, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "account_menu_native_ui_passed" : "account_menu_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 255;
    }
    wchar_t background_theme_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_BACKGROUND_THEME_PROBE", background_theme_only, 2) == 1 &&
        background_theme_only[0] == L'1') {
        const bool valid = initialize_fixture(true) &&
                           background_theme_ui_probe({g_probe.renderer,
                                                      +[]() noexcept {
                                                          return synchronize_probe_shell_layout() &&
                                                                 rebuild_probe_content_text();
                                                      },
                                                      probe_capture_frame});
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "background_theme_native_ui_passed" : "background_theme_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 270;
    }
    wchar_t inline_flags_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_INLINE_FLAGS_PROBE", inline_flags_only, 2) == 1 &&
        inline_flags_only[0] == L'1') {
        const bool valid = initialize_fixture(true) &&
                           inline_flags_ui_probe({g_probe.renderer, g_probe.shell_scene, g_probe.chat_state,
                                                  g_probe.navigation, +[]() noexcept {
                                                      return synchronize_probe_shell_layout() &&
                                                             rebuild_probe_content_text();
                                                  }});
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "inline_flags_native_ui_passed" : "inline_flags_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 269;
    }
    wchar_t guild_rail_icons_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_GUILD_RAIL_ICON_PROBE", guild_rail_icons_only, 2) == 1 &&
        guild_rail_icons_only[0] == L'1') {
        const bool valid = initialize_fixture(true) &&
                           guild_rail_icon_ui_probe({g_probe.renderer, g_probe.guild_rail_scene, g_probe.chat_state,
                                                     g_probe.navigation, +[]() noexcept {
                                                         return synchronize_probe_shell_layout() &&
                                                                rebuild_probe_content_text();
                                                     }});
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "guild_rail_icons_native_ui_passed" : "guild_rail_icons_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 275;
    }
    wchar_t unified_picker_only[2]{};
    wchar_t post_resize_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_POST_RESIZE_PACING_PROBE", post_resize_only, 2) == 1 &&
        post_resize_only[0] == L'1') {
        if (post_resize_probe_detail::compositor_asleep()) {
            log_write(LogLevel::info, "probe", "post_resize_pacing_skipped_display_asleep");
            renderer_shutdown(&g_probe.renderer);
            DestroyWindow(g_probe.window);
            return 77;
        }
        const bool valid =
            initialize_fixture(true) && post_resize_pacing_ui_probe(
                                            g_probe.renderer, g_probe.window, g_probe.message_scene,
                                            +[]() noexcept { return synchronize_probe_shell_layout(); },
                                            +[]() noexcept { return rebuild_probe_content_text(); });
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "post_resize_pacing_native_ui_passed" : "post_resize_pacing_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 268;
    }
    if (GetEnvironmentVariableW(L"MAICORD_UI_UNIFIED_PICKER_PROBE", unified_picker_only, 2) == 1 &&
        unified_picker_only[0] == L'1') {
        const bool valid =
            initialize_fixture(true) && unified_picker_ui_probe(g_probe.renderer, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "unified_picker_native_ui_passed" : "unified_picker_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 266;
    }
    wchar_t hover_retention_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_HOVER_RETENTION_PROBE", hover_retention_only, 2) == 1 &&
        hover_retention_only[0] == L'1') {
        const bool valid =
            initialize_fixture(true) &&
            hover_retention_ui_probe(g_probe.renderer, g_probe.message_view, g_probe.message_scene,
                                     g_probe.users, rebuild_probe_content_text);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "hover_retention_native_ui_passed" : "hover_retention_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 267;
    }
    wchar_t media_underlay_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_MEDIA_UNDERLAY_PROBE", media_underlay_only, 2) == 1 &&
        media_underlay_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && probe_media_underlay_and_quick_reactions();
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "media_underlay_native_ui_passed" : "media_underlay_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 264;
    }
    wchar_t search_polish_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_SEARCH_POLISH_PROBE", search_polish_only, 2) == 1 &&
        search_polish_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && search_polish_ui_probe(g_probe.renderer);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "search_polish_native_ui_passed" : "search_polish_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 265;
    }
    wchar_t scoped_search_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_SCOPED_SEARCH_UI_PROBE", scoped_search_only, 2) == 1 &&
        scoped_search_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && scoped_search_ui_probe(g_probe.renderer);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "scoped_search_native_ui_passed" : "scoped_search_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 263;
    }
    wchar_t pane_glyph_stress_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_PANE_GLYPH_STRESS_PROBE", pane_glyph_stress_only, 2) == 1 &&
        pane_glyph_stress_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && native_pane_glyph_stress_probe(g_probe.renderer);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "pane_glyph_stress_native_ui_passed" : "pane_glyph_stress_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 262;
    }
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
    wchar_t channel_overview_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_CHANNEL_OVERVIEW_PROBE", channel_overview_only, 2) == 1 &&
        channel_overview_only[0] == L'1') {
        const bool valid = initialize_fixture(true) &&
                           channel_overview_ui_probe(g_probe.renderer, g_probe.text_measure_cache);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "channel_overview_native_ui_passed" : "channel_overview_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 261;
    }
    wchar_t long_embed_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_LONG_EMBED_UI_PROBE", long_embed_only, 2) == 1 &&
        long_embed_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && probe_long_embed_text();
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "long_embed_native_ui_passed" : "long_embed_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 263;
    }
    wchar_t surface_motion_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_SURFACE_MOTION_PROBE", surface_motion_only, 2) == 1 &&
        surface_motion_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && probe_ui_surface_motion();
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "surface_motion_native_ui_passed" : "surface_motion_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 260;
    }
    wchar_t media_favorites_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_MEDIA_FAVORITES_UI_PROBE", media_favorites_only, 2) == 1 &&
        media_favorites_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && probe_media_gallery(true);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "media_favorites_native_ui_passed" : "media_favorites_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 259;
    }
    wchar_t media_picker_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_MEDIA_PICKER_UI_PROBE", media_picker_only, 2) == 1 &&
        media_picker_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && probe_media_gallery();
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "media_gallery_native_ui_passed" : "media_gallery_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 258;
    }
    wchar_t forwarded_messages_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_FORWARDED_MESSAGES_PROBE", forwarded_messages_only, 2) == 1 &&
        forwarded_messages_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && forwarded_messages_ui_probe(
            g_probe.renderer, g_probe.text_measure_cache, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "forwarded_messages_native_ui_passed" : "forwarded_messages_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 289;
    }
    wchar_t shortcuts_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_SHORTCUTS_PROBE", shortcuts_only, 2) == 1 && shortcuts_only[0] == L'1') {
        const bool valid = initialize_fixture(true) &&
                           shortcuts_ui_probe(g_probe.renderer, +[](const wchar_t* filename, const Theme& theme) noexcept {
                               return probe_capture_frame(filename, theme);
                           });
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 397;
    }
    wchar_t media_memory_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_MEDIA_MEMORY_PROBE", media_memory_only, 2) == 1 &&
        media_memory_only[0] == L'1') {
        // Atlas growth/trim pixels and GPU memory of synthetic media workloads.
        const bool valid = initialize_fixture(true) && media_memory_probe(g_probe.renderer);
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 156;
    }
    wchar_t call_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_CALL_PROBE", call_only, 2) == 1 && call_only[0] == L'1') {
        const bool valid = initialize_fixture(true) &&
                           call_scene_ui_probe(g_probe.renderer, +[](const wchar_t* filename, const Theme& theme) noexcept {
                               return probe_capture_frame(filename, theme);
                           });
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 398;
    }
    wchar_t downloads_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_DOWNLOAD_PROBE", downloads_only, 2) == 1 &&
        downloads_only[0] == L'1') {
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 291;
    }
    wchar_t reply_draft_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_REPLY_DRAFT_PROBE", reply_draft_only, 2) == 1 &&
        reply_draft_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && reply_draft_ui_probe(
            g_probe.renderer, g_probe.text_measure_cache, g_probe.window, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
            valid ? "reply_draft_native_ui_passed" : "reply_draft_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 296;
    }
    wchar_t forum_media_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_FORUM_MEDIA_PROBE", forum_media_only, 2) == 1 &&
        forum_media_only[0] == L'1') {
        const bool valid =
            initialize_fixture(true) && forum_post_ui_probe(g_probe.renderer, g_probe.text_measure_cache,
                                                            g_probe.window, probe_capture_frame, true);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "forum_media_native_ui_passed" : "forum_media_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 301;
    }
    wchar_t thread_creation_only[2]{};
    wchar_t forum_post_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_FORUM_POST_PROBE", forum_post_only, 2) == 1 &&
        forum_post_only[0] == L'1') {
        const bool valid =
            initialize_fixture(true) && forum_post_ui_probe(g_probe.renderer, g_probe.text_measure_cache,
                                                            g_probe.window, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
            valid ? "forum_post_native_ui_passed" : "forum_post_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 299;
    }
    if (GetEnvironmentVariableW(L"MAICORD_UI_THREAD_CREATION_PROBE", thread_creation_only, 2) == 1 &&
        thread_creation_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && thread_creation_ui_probe(
            g_probe.renderer, g_probe.text_measure_cache, g_probe.window, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
            valid ? "thread_creation_native_ui_passed" : "thread_creation_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 298;
    }
    wchar_t thread_browser_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_THREAD_BROWSER_PROBE", thread_browser_only, 2) == 1 &&
        thread_browser_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && thread_browser_ui_probe(
            g_probe.renderer, g_probe.text_measure_cache, g_probe.window, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
            valid ? "thread_browser_native_ui_passed" : "thread_browser_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 297;
    }
    wchar_t thread_membership_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_THREAD_MEMBERSHIP_PROBE", thread_membership_only, 2) == 1 &&
        thread_membership_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && thread_membership_ui_probe(
            g_probe.renderer, g_probe.text_measure_cache, g_probe.window, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
            valid ? "thread_membership_native_ui_passed" : "thread_membership_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 295;
    }
    wchar_t reaction_intent_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_REACTION_INTENT_PROBE", reaction_intent_only, 2) == 1 &&
        reaction_intent_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && reaction_intent_ui_probe(
            g_probe.renderer,g_probe.text_measure_cache,g_probe.window,probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error,"probe",
            valid ? "reaction_intent_native_ui_passed" : "reaction_intent_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 294;
    }
    wchar_t rich_search_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_RICH_SEARCH_PROBE", rich_search_only, 2) == 1 &&
        rich_search_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && rich_search_ui_probe(
            g_probe.renderer,g_probe.text_measure_cache,probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error,"probe",
            valid ? "rich_search_native_ui_passed" : "rich_search_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 293;
    }
    wchar_t unread_boundary_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_UNREAD_BOUNDARY_PROBE", unread_boundary_only, 2) == 1 &&
        unread_boundary_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && unread_boundary_ui_probe(
            g_probe.renderer,g_probe.text_measure_cache,probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error,"probe",
            valid ? "unread_boundary_native_ui_passed" : "unread_boundary_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 292;
    }
    wchar_t message_forward_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_MESSAGE_FORWARD_PROBE", message_forward_only, 2) == 1 &&
        message_forward_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && message_forward_ui_probe(g_probe.renderer, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "message_forward_native_ui_passed" : "message_forward_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 290;
    }
    wchar_t quick_switcher_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_QUICK_SWITCHER_PROBE", quick_switcher_only, 2) == 1 &&
        quick_switcher_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && quick_switcher_ui_probe(g_probe.renderer, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "quick_switcher_native_ui_passed" : "quick_switcher_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 288;
    }
    wchar_t composer_workflow_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_COMPOSER_WORKFLOW_PROBE", composer_workflow_only, 2) == 1 &&
        composer_workflow_only[0] == L'1') {
        const auto publish = +[](const ComposerWorkflowProbeState& state) noexcept {
            auto& renderer = g_probe.renderer;
            const float width = renderer.width * 96.f / maximum(renderer.dpi, 1u);
            const float height = renderer.height * 96.f / maximum(renderer.dpi, 1u);
            auto metrics = kGuildChannelShellLayoutMetrics;
            metrics.composer_height = message_composer_scene_preferred_height(
                g_probe.message_composer, ui_scene_composer_width(metrics, width), state.reply.bytes != 0,
                state.status.bytes != 0);
            metrics.revision = 200'000u + ui_extent_units(metrics.composer_height);
            if (!ui_scene_layout_with_metrics(&g_probe.shell_scene, UiShellView::guild_channel, metrics,
                                              width, height))
                return false;
            const auto* composer = ui_scene_resolve(
                g_probe.shell_scene, ui_scene_find(g_probe.shell_scene, UiSemantic::composer));
            const auto* messages = ui_scene_resolve(
                g_probe.shell_scene, ui_scene_find(g_probe.shell_scene, UiSemantic::message_view));
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 287;
    }
    wchar_t pinned_messages_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_PINNED_MESSAGES_PROBE", pinned_messages_only, 2) == 1 &&
        pinned_messages_only[0] == L'1') {
        const bool valid = initialize_fixture(true) &&
                           pinned_messages_ui_probe(g_probe.renderer, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "pinned_messages_native_ui_passed" : "pinned_messages_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 286;
    }
    wchar_t search_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_SEARCH_UI_PROBE", search_only, 2) == 1 &&
        search_only[0] == L'1') {
        const bool valid =
            initialize_fixture(true) && search_ui_probe(g_probe.renderer, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "search_native_ui_passed" : "search_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 256;
    }
    wchar_t tooltip_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_TOOLTIP_UI_PROBE", tooltip_only, 2) == 1 &&
        tooltip_only[0] == L'1') {
        const bool valid =
            initialize_fixture(true) && tooltip_ui_probe(g_probe.renderer, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "tooltip_native_ui_passed" : "tooltip_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 254;
    }
    if(GetEnvironmentVariableW(L"MAICORD_CATALOG_APNG_UI_PROBE",nullptr,0)) {
        const bool valid=initialize_fixture(true) && probe_catalog_apng();
        log_write(valid?LogLevel::info:LogLevel::error,"probe",valid?"catalog_apng_native_ui_passed":"catalog_apng_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);DestroyWindow(g_probe.window);return valid?0:273;
    }
    wchar_t transparent_guild_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_LOTTIE_FIXTURE", nullptr, 0)) {
        const bool valid=initialize_fixture(true) && probe_lottie_gallery();
        log_write(valid ? LogLevel::info : LogLevel::error,"probe",
            valid ? "lottie_gallery_native_ui_passed" : "lottie_gallery_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);DestroyWindow(g_probe.window);
        return valid ? 0 : 272;
    }
    if (GetEnvironmentVariableW(L"MAICORD_TRANSPARENT_GUILD_UI_PROBE", transparent_guild_only, 2) == 1 &&
        transparent_guild_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && probe_transparent_guild_icon();
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
            valid ? "transparent_guild_native_ui_passed" : "transparent_guild_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 271;
    }
    wchar_t profile_editor_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_PROFILE_EDITOR_UI_PROBE", profile_editor_only, 2) == 1 &&
        profile_editor_only[0] == L'1') {
        const bool valid = initialize_fixture(true) &&
                           profile_editor_ui_probe(g_probe.renderer, &g_probe.text_measure_cache, &g_probe.chat_state,
                                                   &g_probe.users, probe_capture_frame);
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 272;
    }
    wchar_t braille_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_PROFILE_BRAILLE_UI_PROBE", braille_only, 2) == 1 &&
        braille_only[0] == L'1') {
        const bool valid = initialize_fixture(true) && probe_braille_profile();
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
            valid ? "braille_profile_native_ui_passed" : "braille_profile_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 270;
    }
    wchar_t animation_handoff_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_ANIMATION_HANDOFF_UI_PROBE", animation_handoff_only, 2) == 1 &&
        animation_handoff_only[0] == L'1') {
        bool valid = initialize_fixture(true);
        valid = valid && application_media_animation_ui_probe(g_probe.renderer, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "animation_handoff_native_ui_passed" : "animation_handoff_native_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 252;
    }
    wchar_t menu_rows_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_MENU_ROW_UI_PROBE", menu_rows_only, 2) == 1 &&
        menu_rows_only[0] == L'1') {
        bool valid = initialize_fixture(true);
        if (!valid) {
            log_u64(LogLevel::error, "probe", "menu_rows_fixture_failure_stage", g_probe.failure_stage);
        }
        valid = valid && menu_row_ui_probe(g_probe.renderer, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "shared_menu_rows_ui_passed" : "shared_menu_rows_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 251;
    }
    wchar_t video_controls_only[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_VIDEO_CONTROLS_UI_PROBE", video_controls_only, 2) == 1 &&
        video_controls_only[0] == L'1') {
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
        valid = valid && audio_scene_ui_probe(g_probe.renderer);
        valid = valid && poll_scene_ui_probe(g_probe.renderer);
        valid = valid && navigation_gaps_ui_probe(g_probe.renderer, &g_probe.navigation, g_probe.chat_state,
                                                  probe_sync_shell_and_text, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "video_controls_injected_ui_passed" : "video_controls_injected_ui_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 250;
    }
    if (plugin_catalog_requested) {
        const bool valid =
            initialize_fixture(true) && plugin_catalog_ui_probe(g_probe.renderer, probe_capture_frame);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "plugin_catalog_isolated_production_descriptors_passed"
                        : "plugin_catalog_isolated_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 247;
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 249;
    }
    const char password_mask[] = {static_cast<char>(0xE2u), static_cast<char>(0x80u),
                                  static_cast<char>(0xA2u), static_cast<char>(0xE2u),
                                  static_cast<char>(0x80u), static_cast<char>(0xA2u)};
    constexpr Utf8View qr_payload =
        utf8_literal("https://chat_state.com/ra/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    QrCode login_qr{};
    if (!qr_code_encode_v5_m(ConstSlice<u8>{reinterpret_cast<const u8*>(qr_payload.data), qr_payload.bytes},
                             &login_qr)) {
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 8;
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
    wchar_t tibetan_wrap_only[2]{};
    const bool tibetan_wrap_render =
        GetEnvironmentVariableW(L"MAICORD_TIBETAN_WRAP_RENDER_PROBE", tibetan_wrap_only, 2) == 1 &&
        tibetan_wrap_only[0] == L'1';
    wchar_t myanmar_wrap_only[2]{};
    const bool myanmar_wrap_render =
        GetEnvironmentVariableW(L"MAICORD_MYANMAR_WRAP_RENDER_PROBE", myanmar_wrap_only, 2) == 1 &&
        myanmar_wrap_only[0] == L'1';
    wchar_t khmer_wrap_only[2]{};
    const bool khmer_wrap_render =
        GetEnvironmentVariableW(L"MAICORD_KHMER_WRAP_RENDER_PROBE", khmer_wrap_only, 2) == 1 &&
        khmer_wrap_only[0] == L'1';
    wchar_t sinhala_wrap_only[2]{};
    const bool sinhala_wrap_render =
        GetEnvironmentVariableW(L"MAICORD_SINHALA_WRAP_RENDER_PROBE", sinhala_wrap_only, 2) == 1 &&
        sinhala_wrap_only[0] == L'1';
    wchar_t malayalam_wrap_only[2]{};
    const bool malayalam_wrap_render =
        GetEnvironmentVariableW(L"MAICORD_MALAYALAM_WRAP_RENDER_PROBE", malayalam_wrap_only, 2) == 1 &&
        malayalam_wrap_only[0] == L'1';
    wchar_t kannada_wrap_only[2]{};
    const bool kannada_wrap_render =
        GetEnvironmentVariableW(L"MAICORD_KANNADA_WRAP_RENDER_PROBE", kannada_wrap_only, 2) == 1 &&
        kannada_wrap_only[0] == L'1';
    wchar_t telugu_wrap_only[2]{};
    const bool telugu_wrap_render =
        GetEnvironmentVariableW(L"MAICORD_TELUGU_WRAP_RENDER_PROBE", telugu_wrap_only, 2) == 1 &&
        telugu_wrap_only[0] == L'1';
    wchar_t odia_wrap_only[2]{};
    const bool odia_wrap_render =
        GetEnvironmentVariableW(L"MAICORD_ODIA_WRAP_RENDER_PROBE", odia_wrap_only, 2) == 1 &&
        odia_wrap_only[0] == L'1';
    wchar_t gurmukhi_wrap_only[2]{};
    const bool gurmukhi_wrap_render =
        GetEnvironmentVariableW(L"MAICORD_GURMUKHI_WRAP_RENDER_PROBE", gurmukhi_wrap_only, 2) == 1 &&
        gurmukhi_wrap_only[0] == L'1';
    wchar_t gujarati_wrap_only[2]{};
    const bool gujarati_wrap_render =
        GetEnvironmentVariableW(L"MAICORD_GUJARATI_WRAP_RENDER_PROBE", gujarati_wrap_only, 2) == 1 &&
        gujarati_wrap_only[0] == L'1';
    wchar_t bengali_wrap_only[2]{};
    const bool bengali_wrap_render =
        GetEnvironmentVariableW(L"MAICORD_BENGALI_WRAP_RENDER_PROBE", bengali_wrap_only, 2) == 1 &&
        bengali_wrap_only[0] == L'1';
    wchar_t tamil_wrap_only[2]{};
    const bool tamil_wrap_render =
        GetEnvironmentVariableW(L"MAICORD_TAMIL_WRAP_RENDER_PROBE", tamil_wrap_only, 2) == 1 &&
        tamil_wrap_only[0] == L'1';
    wchar_t thai_wrap_only[2]{};
    if (tibetan_wrap_render || myanmar_wrap_render || khmer_wrap_render || sinhala_wrap_render ||
        malayalam_wrap_render || kannada_wrap_render || telugu_wrap_render || odia_wrap_render ||
        gurmukhi_wrap_render || gujarati_wrap_render || bengali_wrap_render || tamil_wrap_render ||
        (GetEnvironmentVariableW(L"MAICORD_THAI_WRAP_RENDER_PROBE", thai_wrap_only, 2) == 1 &&
         thai_wrap_only[0] == L'1')) {
        const bool valid = initialize_fixture(true) &&
                           probe_script_wrapped(tibetan_wrap_render     ? WrappedProbeScript::tibetan
                                                : myanmar_wrap_render   ? WrappedProbeScript::myanmar
                                                : khmer_wrap_render     ? WrappedProbeScript::khmer
                                                : sinhala_wrap_render   ? WrappedProbeScript::sinhala
                                                : malayalam_wrap_render ? WrappedProbeScript::malayalam
                                                : kannada_wrap_render   ? WrappedProbeScript::kannada
                                                : telugu_wrap_render    ? WrappedProbeScript::telugu
                                                : odia_wrap_render      ? WrappedProbeScript::odia
                                                : gurmukhi_wrap_render  ? WrappedProbeScript::gurmukhi
                                                : gujarati_wrap_render  ? WrappedProbeScript::gujarati
                                                : bengali_wrap_render   ? WrappedProbeScript::bengali
                                                : tamil_wrap_render     ? WrappedProbeScript::tamil
                                                                        : WrappedProbeScript::thai);
        log_write(
            valid ? LogLevel::info : LogLevel::error, "probe",
            tibetan_wrap_render
                ? (valid ? "tibetan_wrap_render_probe_passed" : "tibetan_wrap_render_probe_failed")
            : myanmar_wrap_render
                ? (valid ? "myanmar_wrap_render_probe_passed" : "myanmar_wrap_render_probe_failed")
            : khmer_wrap_render
                ? (valid ? "khmer_wrap_render_probe_passed" : "khmer_wrap_render_probe_failed")
            : sinhala_wrap_render
                ? (valid ? "sinhala_wrap_render_probe_passed" : "sinhala_wrap_render_probe_failed")
            : malayalam_wrap_render
                ? (valid ? "malayalam_wrap_render_probe_passed" : "malayalam_wrap_render_probe_failed")
            : kannada_wrap_render
                ? (valid ? "kannada_wrap_render_probe_passed" : "kannada_wrap_render_probe_failed")
            : telugu_wrap_render
                ? (valid ? "telugu_wrap_render_probe_passed" : "telugu_wrap_render_probe_failed")
            : odia_wrap_render ? (valid ? "odia_wrap_render_probe_passed" : "odia_wrap_render_probe_failed")
            : gurmukhi_wrap_render
                ? (valid ? "gurmukhi_wrap_render_probe_passed" : "gurmukhi_wrap_render_probe_failed")
            : gujarati_wrap_render
                ? (valid ? "gujarati_wrap_render_probe_passed" : "gujarati_wrap_render_probe_failed")
            : bengali_wrap_render
                ? (valid ? "bengali_wrap_render_probe_passed" : "bengali_wrap_render_probe_failed")
            : tamil_wrap_render
                ? (valid ? "tamil_wrap_render_probe_passed" : "tamil_wrap_render_probe_failed")
                : (valid ? "thai_wrap_render_probe_passed" : "thai_wrap_render_probe_failed"));
        renderer_shutdown(&g_probe.renderer);
        d3d11_draw_pipeline_release(&g_probe.renderer.pipeline);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 241;
    }
    wchar_t extended_only[2]{}, hangul_only[2]{}, standard_only[2]{}, deva_only[2]{},
        deva_tracking_only[2]{}, deva_direction_only[2]{}, direction_tracking_only[2]{};
    const bool direction_tracking_render =
        GetEnvironmentVariableW(L"MAICORD_DIRECTION_TRACKING_RENDER_PROBE", direction_tracking_only, 2) ==
            1 &&
        direction_tracking_only[0] == L'1';
    const bool deva_direction_render = GetEnvironmentVariableW(L"MAICORD_DEVANAGARI_DIRECTION_RENDER_PROBE",
                                                               deva_direction_only, 2) == 1 &&
                                       deva_direction_only[0] == L'1';
    const bool deva_tracking_render =
        GetEnvironmentVariableW(L"MAICORD_DEVANAGARI_TRACKING_RENDER_PROBE", deva_tracking_only, 2) == 1 &&
        deva_tracking_only[0] == L'1';
    const bool deva_render =
        direction_tracking_render || deva_direction_render || deva_tracking_render ||
        (GetEnvironmentVariableW(L"MAICORD_DEVANAGARI_RENDER_PROBE", deva_only, 2) == 1 &&
         deva_only[0] == L'1');
    const bool extended_render =
        GetEnvironmentVariableW(L"MAICORD_EXTENDED_STANDARD_RENDER_PROBE", extended_only, 2) == 1 &&
        extended_only[0] == L'1';
    const bool standard_render =
        extended_render ||
        (GetEnvironmentVariableW(L"MAICORD_STANDARD_RENDER_PROBE", standard_only, 2) == 1 &&
         standard_only[0] == L'1');
    if (deva_render || standard_render ||
        (GetEnvironmentVariableW(L"MAICORD_HANGUL_RENDER_PROBE", hangul_only, 2) == 1 &&
         hangul_only[0] == L'1')) {
        const Utf8View hangul_samples[]{utf8_literal("한국어 메시지 😀"), utf8_literal("한글 각 각"),
                                        utf8_literal("ᄓᅡᇫ ᅟᅠᆨ 가〮 각〯"),
                                        utf8_literal("〮 가〮〯 가〮")};
        const Utf8View extended_samples[]{utf8_literal("ሰላም ዓለም 😀"), utf8_literal("ሀ፝ ሁ፞ ሂ፟ ፩፪፫ ፲፻፼"),
                                          utf8_literal("ᎣᏏᏲ ᏣᎳᎩ ꭰꭱꭲ"), utf8_literal("Ꭰ́ Ꭱ̄ שלום ሰላም")};
        const Utf8View standard_samples[]{utf8_literal("Բարև աշխարհ 😀"), utf8_literal("և եւ ﬓ ﬔ ﬕ ﬖ ﬗ ա́"),
                                          utf8_literal("ქართული ᲥᲐᲠᲗᲣᲚᲘ"), utf8_literal("ႠႡႢ ⴀⴁⴂ ა́ ա̄")};
        const Utf8View deva_samples[]{utf8_literal("नमस्ते दुनिया 😀"), utf8_literal("र्कि किं कृ त्र ज्ञ"),
                                      utf8_literal("क्‍ष क्‌ष क़ि"),
                                      utf8_literal("ि ़ ॑ क॑ कः")};
        const Utf8View deva_direction_samples[]{
            utf8_literal("A \u202eर्कि हिन्दी 😀\u202c B"),
            utf8_literal("\u202eक्‍ष क्‌ष क्षेत्र\u202c"),
            utf8_literal("אב हिन्दी \u202eर्कि\u202c"), utf8_literal("\u2067हिन्दी \u202eर्कि\u202c\u2069")};
        const Utf8View direction_tracking_samples[]{
            utf8_literal("A \u202eर्कि हिन्दी 😀\u202c B"), utf8_literal("אָבּ גּד हिन्दी"),
            utf8_literal("سَلَامٌ \u202dب\u034fب بَ\u034fب\u202c"),
            utf8_literal("سَّلَام 👩‍👩‍👧‍👦 A")};
        const auto* samples = extended_render           ? extended_samples
                              : direction_tracking_render ? direction_tracking_samples
                              : deva_direction_render     ? deva_direction_samples
                              : deva_render               ? deva_samples
                              : standard_render           ? standard_samples
                                                          : hangul_samples;
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
                    if (deva_tracking_render) {
                        system.tracking = (i & 1) ? .13f : .03f;
                        system.independent_letters = i >= 2;
                    }
                    if (deva_direction_render) {
                        system.tracking = 0;
                        system.independent_letters = i >= 2;
                    }
                    if (direction_tracking_render || extended_render) {
                        system.tracking = (i & 1) ? .13f : .03f;
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
                    wchar_t hangul_capture[] = L"hangul-0-0-0.bmp",
                            standard_capture[] = L"standard-0-0-0.bmp";
                    wchar_t deva_capture[] = L"devanagari-0-0-0.bmp";
                    wchar_t deva_tracking_capture[] = L"devanagari-tracking-0-0-0.bmp";
                    wchar_t deva_direction_capture[] = L"devanagari-direction-0-0-0.bmp";
                    wchar_t direction_tracking_capture[] = L"direction-tracking-0-0-0.bmp",
                            extended_capture[] = L"extended-standard-0-0-0.bmp";
                    auto* capture = extended_render           ? extended_capture
                                    : direction_tracking_render ? direction_tracking_capture
                                    : deva_direction_render     ? deva_direction_capture
                                    : deva_tracking_render      ? deva_tracking_capture
                                    : deva_render               ? deva_capture
                                    : standard_render           ? standard_capture
                                                                : hangul_capture;
                    const u32 digit = extended_render           ? 18u
                                      : direction_tracking_render ? 19u
                                      : deva_direction_render     ? 21u
                                      : deva_tracking_render      ? 20u
                                      : deva_render               ? 11u
                                      : standard_render           ? 9u
                                                                  : 7u;
                    capture[digit] += static_cast<wchar_t>(i);
                    capture[digit + 2] += static_cast<wchar_t>(mode);
                    capture[digit + 4] += static_cast<wchar_t>(scale);
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
        log_write(
            valid ? LogLevel::info : LogLevel::error, "probe",
            extended_render ? (valid ? "extended_standard_render_probe_passed"
                                      : "extended_standard_render_probe_failed")
            : direction_tracking_render ? (valid ? "direction_tracking_render_probe_passed"
                                                 : "direction_tracking_render_probe_failed")
            : deva_direction_render     ? (valid ? "devanagari_direction_render_probe_passed"
                                                 : "devanagari_direction_render_probe_failed")
            : deva_tracking_render      ? (valid ? "devanagari_tracking_render_probe_passed"
                                                 : "devanagari_tracking_render_probe_failed")
            : deva_render ? (valid ? "devanagari_render_probe_passed" : "devanagari_render_probe_failed")
            : standard_render ? (valid ? "standard_render_probe_passed" : "standard_render_probe_failed")
                              : (valid ? "hangul_render_probe_passed" : "hangul_render_probe_failed"));
        renderer_shutdown(&g_probe.renderer);
        d3d11_draw_pipeline_release(&g_probe.renderer.pipeline);
        DestroyWindow(g_probe.window);
        return valid ? 0 : 240;
    }
    // Invalid and empty-owner contracts run once with the full integration probe.
    D3D11DeviceOwner invalid_owner{};
    if (d3d11_device_owner_resize(&invalid_owner, 1, 1).ok() ||
        d3d11_device_owner_present(&invalid_owner, 0, nullptr) != E_INVALIDARG ||
        d3d11_device_owner_wait(invalid_owner, 0) != WAIT_OBJECT_0) {
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
    D3D11GpuTimerOwner invalid_timer{};
    TextEditLayoutOwner invalid_composer{};
    MessageHitTestCacheOwner invalid_message_hit{};
    ContentTextRetentionOwner invalid_content_text{};
    ContentTextRetentionKey invalid_content_key{};
    if (d3d11_gpu_timer_initialize(nullptr, nullptr, true).error != D3D11GpuTimerError::invalid_input ||
        d3d11_gpu_timer_initialize(&invalid_timer, nullptr, true).error !=
            D3D11GpuTimerError::invalid_input ||
        !d3d11_gpu_timer_initialize(&invalid_timer, nullptr, false).ok()) {
        return 9;
    }
    d3d11_gpu_timer_release(&invalid_timer, nullptr);
    d3d11_gpu_timer_release(&invalid_timer, nullptr);
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
    if (!gpu_timer_empty(invalid_timer) || !composer_layout_empty(invalid_composer) ||
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 8;
    }
    RendererLoginView restoring_view = login_view;
    restoring_view.status = RendererLoginStatus::restoring_session;
    restoring_view.qr_code = nullptr;
    if (!set_probe_login_view(restoring_view) || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        !probe_login_pixels(true) || !set_probe_login_view(login_view) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) || !probe_login_pixels() ||
        !probe_capture_frame(L"login-text.bmp")) {
        log_write(LogLevel::error, "probe", "login_restore_text_pixels_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 8;
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
            renderer_shutdown(&g_probe.renderer);
            DestroyWindow(g_probe.window);
            return 8;
        }
        if (iteration == 0)
            warm_color_cache_builds = g_probe.renderer.glyph_text.cache_builds;
        else if (g_probe.renderer.glyph_text.cache_builds != warm_color_cache_builds) {
            log_write(LogLevel::error, "probe", "cached_colrv1_glyph_was_rasterized_again");
            renderer_shutdown(&g_probe.renderer);
            DestroyWindow(g_probe.window);
            return 8;
        }
    }
    const bool color_factory_ready = g_probe.renderer.glyph_text.color_glyphs;
    if (!probe_color_foreground_cache()) {
        log_write(LogLevel::error, "probe", "probe_color_foreground_cache()_failed");
        return 8;
    }
    // These two native-only oracle checks create extra atlas entries. Keep
    // them in ordinary integration runs, outside the paired scene-memory run.
    if (!memory_measurement.enabled() && !probe_empty_outline_cache()) {
        log_write(LogLevel::error, "probe", "probe_empty_outline_cache()_failed");
        return 8;
    }
    if (!memory_measurement.enabled() && !probe_color_foreground_cache(true)) {
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 8;
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 9;
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 9;
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
                renderer_shutdown(&g_probe.renderer);
                DestroyWindow(g_probe.window);
                return 9;
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 9;
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 9;
    }
    const u32 edit_paint_with_caret = g_probe.renderer.paint_stream.count;
    renderer_set_login_caret_visible(&g_probe.renderer, false);
    if (g_probe.renderer.login_caret_visible || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.paint_stream.count + 1 != edit_paint_with_caret) {
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 9;
    }
    renderer_set_login_caret_visible(&g_probe.renderer, true);
    if (!g_probe.renderer.login_caret_visible || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.paint_stream.count != edit_paint_with_caret) {
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 9;
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
                renderer_shutdown(&g_probe.renderer);
                DestroyWindow(g_probe.window);
                return 9;
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
            renderer_shutdown(&g_probe.renderer);
            DestroyWindow(g_probe.window);
            return 9;
        }
        log_write(LogLevel::info, "probe", "login_blocked_back_and_restored_projections_passed");
    }
    RendererLoginView mobile_view = login_view;
    mobile_view.remote_user = utf8_literal("doodles");
    mobile_view.status = RendererLoginStatus::remote_mobile_pending;
    mobile_view.focus = LoginControl::submit;
    if (!set_probe_login_view(mobile_view) || !probe_present(&g_probe.renderer, kDefaultTheme)) {
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 9;
    }
    RendererLoginView expired_view = mobile_view;
    expired_view.remote_user = {};
    expired_view.qr_code = nullptr;
    expired_view.status = RendererLoginStatus::remote_expired;
    expired_view.focus = LoginControl::qr;
    if (!set_probe_login_view(expired_view) || !probe_present(&g_probe.renderer, kDefaultTheme)) {
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 9;
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 9;
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 9;
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 9;
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 9;
    }
    // Recovery uses the standalone login renderer before account text is bound.
    if (!login_recovery_ui_probe(g_probe.renderer, probe_capture_frame, probe_login_recovery_text_pixels)) {
        log_write(LogLevel::error, "probe", "login_recovery_standalone_pixel_probe_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 9;
    }
    if (!login_reset_ui_probe(g_probe.renderer, probe_capture_frame, probe_login_recovery_text_pixels)) {
        log_write(LogLevel::error, "probe", "login_reset_standalone_pixel_probe_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 9;
    }
    if (!login_phone_ui_probe(g_probe.renderer, probe_capture_frame, probe_login_recovery_text_pixels)) {
        log_write(LogLevel::error, "probe", "login_phone_standalone_pixel_probe_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 9;
    }
    if (!login_browser_ui_probe(g_probe.renderer, probe_capture_frame, probe_login_recovery_text_pixels)) {
        log_write(LogLevel::error, "probe", "login_browser_standalone_pixel_probe_failed");
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 9;
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 4;
    }
    if (!probe_present(&g_probe.renderer, kDefaultTheme)) {
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 4;
    }
    if (!media_pins_match_visible_quads()) {
        log_u64(LogLevel::error, "probe", "offscreen_media_pinned_or_queued", 1);
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 4;
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 4;
    }
    MediaAtlasResult cached_visible_source{};
    MediaAssetKey cached_visible_key{};
    for (u32 index = 0; index < g_probe.renderer.media_quad_count; ++index) {
        MediaAtlasResult candidate{};
        const MediaAtlasHandle handle = g_probe.renderer.media_draw.handle_data[index];
        if (g_probe.renderer.media_draw.quad_data[index].u0 >= 0.0f &&
            media_atlas_validate(g_probe.media_atlas, handle, &candidate) &&
            candidate.width <= kRendererAnimationFrameCacheMaximumFrameExtent &&
            candidate.height <= kRendererAnimationFrameCacheMaximumFrameExtent) {
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 4;
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 4;
    }
    MediaAtlasResult live_visible_source{};
    MediaAssetKey live_visible_key{};
    for (u32 index = 0; index < g_probe.renderer.media_quad_count; ++index) {
        MediaAtlasResult candidate{};
        const MediaAtlasHandle handle = g_probe.renderer.media_draw.handle_data[index];
        if (g_probe.renderer.media_draw.quad_data[index].u0 >= 0.0f &&
            media_atlas_validate(g_probe.media_atlas, handle, &candidate) &&
            candidate.width <= kRendererAnimationFrameCacheMaximumFrameExtent &&
            candidate.height <= kRendererAnimationFrameCacheMaximumFrameExtent) {
            live_visible_source = candidate;
            live_visible_key = g_probe.media_atlas.entries[handle.slot].key;
            break;
        }
    }
    u8 live_pixels[kRendererAnimationFrameCacheMaximumFrameExtent *
                   kRendererAnimationFrameCacheMaximumFrameExtent * 4]{};
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
    const u32 live_gpu_uploads = g_probe.renderer.media_gpu.uploads;
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
        g_probe.renderer.media_gpu.uploads != live_gpu_uploads + 1u) {
        log_u64(LogLevel::error, "probe", "retained_presentation_stage", 4);
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 4;
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 4;
    }
    const u32 post_live_unchanged_skips = g_probe.renderer.unchanged_present_skips;
    const u32 post_live_media_draws_total = g_probe.renderer.media_draws_total;
    if (!probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.unchanged_present_skips != post_live_unchanged_skips + 1u ||
        g_probe.renderer.media_draws_total != post_live_media_draws_total ||
        g_probe.renderer.media_draws_last_frame != 0) {
        log_u64(LogLevel::error, "probe", "retained_presentation_stage", 6);
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 4;
    }
    if (g_probe.text.message_geometry_quads < 6) {
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 5;
    }
    // One live media upload and the reaction picker's server-icon fixture.
    if (g_probe.renderer.media_draws_last_frame != 0 || g_probe.renderer.media_gpu.uploads != 2 ||
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
        !g_probe.renderer.media_gpu.apng_backup_texture) {
        log_u64(LogLevel::error, "probe", "media_draws_last_frame",
                g_probe.renderer.media_draws_last_frame);
        log_u64(LogLevel::error, "probe", "media_uploads", g_probe.renderer.media_gpu.uploads);
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
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 6;
    }
    if (!video_retention_probe(g_probe.renderer, live_visible_key, live_visible_source.width,
                               live_visible_source.height)) {
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 10;
    }
    if (!animation_preview_probe(g_probe.renderer)) {
        renderer_shutdown(&g_probe.renderer);
        DestroyWindow(g_probe.window);
        return 11;
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
        if (!pane_resize_benchmark())
            return 15;
        bool scroll_measured = false;
        if (!scroll_benchmark(&scroll_measured))
            return 17;
        if (scroll_measured) {
            renderer_shutdown(&g_probe.renderer);
            DestroyWindow(g_probe.window);
            log_close();
            return 0;
        }
        if (!shaped_text_cache_equivalence())
            return 18;
        if (!jpeg_gpu_probe(g_probe.renderer))
            return 16;
        if (!webp_gpu_probe(g_probe.renderer))
            return 1;
        if (!video_frame_reference_probe(g_probe.renderer))
            return 1;
        if (!gif_scroll_probe(g_probe.renderer, true)) {
            renderer_shutdown(&g_probe.renderer);
            DestroyWindow(g_probe.window);
            return 12;
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
        const auto* home = ui_scene_resolve(g_probe.shell_scene,
                                            ui_scene_find(g_probe.shell_scene, UiSemantic::home_button));
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
            !ui_scene_resolve(g_probe.shell_scene, ui_scene_find(g_probe.shell_scene, UiSemantic::private_add)) ||
            !ui_scene_resolve(g_probe.shell_scene, ui_scene_find(g_probe.shell_scene, UiSemantic::private_call)) ||
            !ui_scene_resolve(g_probe.shell_scene, ui_scene_find(g_probe.shell_scene, UiSemantic::private_create)) ||
            !utf8_equal(members, utf8_literal("4 members")) ||
            ui_private_row_status(private_rows, 9998) != UiPrivateRowStatus::online)
            return 15;
        log_write(LogLevel::info, "probe", "group_conversation_icon_members_presence_and_actions_passed");
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
            bool listed = member_list_view_private(&group_members, g_probe.users, g_probe.chat_state, 9997).ok() &&
                          group_members.member_count >= 3 &&
                          member_list_scene_initialize(&group_scene, {group_row_widgets, 8}, {group_widgets, 40},
                                                       {group_nodes, 40}) &&
                          synchronize_probe_shell_layout({}, {}, 0, 240);
            const auto* pane = ui_scene_resolve(g_probe.shell_scene,
                                                ui_scene_find(g_probe.shell_scene, UiSemantic::member_sidebar));
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
        if (!probe_autocomplete_identity())
            return static_cast<int>(g_probe.failure_stage);
        if (!probe_glyph_atlas_gutters())
            return static_cast<int>(g_probe.failure_stage);
        if (!probe_dm_name_styles())
            return 237;
        if (!probe_profile_card())
            return static_cast<int>(g_probe.failure_stage);
        if (!probe_profile_activity_card())
            return static_cast<int>(g_probe.failure_stage);
        if (!probe_empty_decorations(g_probe.renderer))
            return 239;
        log_u64(LogLevel::info, "probe", "final_renderer_font_faces",
                g_probe.renderer.glyph_text.font_face_count);
        if (!probe_renderer_font_aliases(g_probe.renderer))
            return 238;
        // The renderer types hold no DirectWrite objects; this process loads
        // dwrite.dll only for the text form probe's test oracle.
        log_write(LogLevel::info, "probe", "native_application_text_objects_absent_passed");
        renderer_shutdown(&g_probe.renderer);
        d3d11_draw_pipeline_release(&g_probe.renderer.pipeline);
        const bool released = !g_probe.renderer.shell_text_storage.base &&
                              device_owner_empty(g_probe.renderer.graphics) &&
                              draw_pipeline_empty(g_probe.renderer.pipeline) &&
                              glyph_text_owner_empty(g_probe.renderer.glyph_text) &&
                              gpu_timer_empty(g_probe.renderer.gpu_timer) &&
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
                   gpu_timer_empty(g_probe.renderer.gpu_timer) &&
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
    ExitProcess(static_cast<UINT>(maicord::run_probe()));
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
bool application_quick_switcher_filter(const MSG&) noexcept { return false; }
void application_quick_switcher_edit_cancel_native_group(const TextInputTarget&) noexcept {}
bool application_message_forward_active() noexcept { return false; }
MessageComposer* application_message_forward_edit_model() noexcept { return nullptr; }
bool application_message_forward_edit_focused() noexcept { return false; }
bool application_message_forward_caption_focused() noexcept { return false; }
TextInputTarget application_message_forward_edit_target() noexcept { return {}; }
UiRect application_message_forward_edit_bounds() noexcept { return {}; }
bool application_message_forward_edit_changed(bool) noexcept { return true; }
bool application_message_forward_filter(const MSG&) noexcept { return false; }
void application_message_forward_edit_cancel_native_group(const TextInputTarget&) noexcept {}
} // namespace maicord::app_detail
