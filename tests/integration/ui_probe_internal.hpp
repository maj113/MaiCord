#pragma once

// State and entry points shared by the parts of the native UI probe (ui_probe.cpp,
// ui_probe_media.cpp, ui_probe_scenes.cpp and ui_probe_renderer.cpp).
#include "maicord/protocol/gateway/reaction_events.hpp"
#include "../../src/app/account/account_ui_runtime.hpp"
#include "../../src/app/shell/state.hpp"
#include "maicord/ui/chat/messages/presentation.hpp"
#include "maicord/text/fonts/run.hpp"
#include "pinned_messages_ui_probe.hpp"
#include "rich_search_ui_probe.hpp"
#include "forum_post_ui_probe.hpp"
#include "post_resize_pacing_ui_probe.hpp"

namespace maicord {
bool plugin_ui_probe(D3D11Renderer& renderer, PopupScene& popup,
                     bool (*capture)(const wchar_t*) noexcept) noexcept;
bool plugin_navigation_ui_probe(D3D11Renderer& renderer, PopupScene& popup,
                                bool (*capture)(const wchar_t*) noexcept) noexcept;
bool settings_choice_ui_probe(D3D11Renderer& renderer, PopupScene& popup,
                              bool (*capture)(const wchar_t*) noexcept) noexcept;

namespace ui_probe_detail {

// One popup bank for every probed menu, autocomplete and popover.
inline constexpr u32 kProbePopupItemCapacity = 72;

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
    // Root, top spacer, rows, the trailing "Add a server" button and the plugin tiles at both ends.
    UiWidget guild_rail_widgets[kProbeGuildCapacity + 3 + 2 * kGuildRailPluginTiles];
    UiLayoutNode guild_rail_layout_nodes[kProbeGuildCapacity + 3 + 2 * kGuildRailPluginTiles];
    ShellNavigationScene shell_navigation_scene;
    ShellNavigationRowWidget shell_navigation_rows[kProbeChannelCapacity];
    UiWidget shell_navigation_widgets[kProbeChannelCapacity * 4 + 2];
    UiLayoutNode shell_navigation_layout_nodes[kProbeChannelCapacity * 4 + 2];
    MessageComposer message_composer;
    MessageComposerScene message_composer_scene;
    char message_composer_text[128];
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

// Never activated, no taskbar button, opened behind every other window.
constexpr DWORD kProbeWindowExStyle = WS_EX_NOREDIRECTIONBITMAP | probe_window::kExtendedStyle;

extern ProbeState g_probe;

inline bool probe_option_enabled(const wchar_t* name) noexcept {
    wchar_t value[2]{};
    return GetEnvironmentVariableW(name, value, 2) == 1 && value[0] == L'1';
}

u64 probe_resize_geometry_signature() noexcept;
bool synchronize_probe_shell_layout(Utf8View typing_label = {},
                                    MessageComposerAttachmentInput attachments = {},
                                    float navigation_width = 0, float member_width = 0,
                                    bool members_visible = true) noexcept;
bool rebuild_probe_content_text() noexcept;
bool probe_sync_shell_and_text() noexcept;
bool set_probe_login_view(RendererLoginView view) noexcept;
bool add_message(u64 id, Utf8View content,
                 MessageContentAvailability availability = MessageContentAvailability::available) noexcept;
bool shaped_text_cache_equivalence() noexcept;
bool apng_atlas_pixels_exact(const MediaAtlasResult& allocation, u32 presented_frame, bool preview = false) noexcept;
bool gif_atlas_pixels_exact(const MediaAtlasResult& allocation, u32 presented_frame) noexcept;
bool animated_webp_atlas_pixels_exact(const MediaAtlasResult& allocation, u32 presented_frame,
                                      bool preview = false) noexcept;
bool mixed_animated_webp_atlas_pixels_exact(const MediaAtlasResult& allocation,
                                            u32 presented_frame) noexcept;
bool jpeg_atlas_pixels_exact(const MediaAtlasResult& allocation, u16 width, u16 height,
                             bool preview = false) noexcept;
bool jpeg_atlas_pixels_reference_tolerance(const MediaAtlasResult& allocation, u16 width, u16 height,
                                           ConstSlice<u8> reference_rgb,
                                           JpegOrientation orientation = JpegOrientation::top_left,
                                           bool swap_red_blue = false) noexcept;
void probe_icc_lab_to_srgb(float lightness_normalized, float a_normalized, float b_normalized,
                           u8 expected[3]) noexcept;
bool jpeg_icc_rgb_lut_pixels_tolerance(const MediaAtlasResult& allocation,
                                       ConstSlice<u8> reference_rgb) noexcept;
bool jpeg_atlas_uniform_rgb_tolerance(const MediaAtlasResult& allocation, u16 width, u16 height,
                                      const u8 expected[3], u8 tolerance) noexcept;
bool jpeg_atlas_differs_from_reference(const MediaAtlasResult& allocation, u16 width, u16 height,
                                       ConstSlice<u8> reference_rgb, u8 minimum_error) noexcept;
bool vp8_gpu_reconstruction_exact() noexcept;
bool vp8_real_fixture_gpu_exact() noexcept;
bool probe_login_edit_centered() noexcept;
bool probe_text_pixels(ConstSlice<UiRect> rectangles, u8 threshold = 160) noexcept;
bool probe_login_recovery_text_pixels(ConstSlice<UiRect> rectangles) noexcept;
bool probe_empty_outline_cache() noexcept;
bool probe_color_foreground_cache(bool native = false) noexcept;
bool probe_login_pixels(bool restoring = false) noexcept;
bool probe_prepare_capture() noexcept;
bool probe_capture_frame(const wchar_t* filename) noexcept;
bool probe_capture_frame(const wchar_t* filename, const Theme& theme) noexcept;
bool probe_ui_surface_motion() noexcept;
bool probe_media_underlay_and_quick_reactions() noexcept;
bool probe_media_gallery(bool favorites = false) noexcept;
bool probe_friends_scene() noexcept;
bool probe_friends_requests_scene() noexcept;
bool probe_account_settings_pages() noexcept;
bool probe_read_pixels(const Theme& theme, ConstSlice<UiPoint> points, u8 (*output)[4],
                       ID3D11Texture2D* texture = nullptr) noexcept;
bool probe_settings_backdrop() noexcept;
bool probe_settings_entrance_frames() noexcept;
bool probe_settings_scene() noexcept;
// Partial frames against full renders of the same state (frame_damage_probe.cpp).
bool frame_damage_ui_probe() noexcept;
bool probe_autocomplete_identity() noexcept;
bool probe_glyph_atlas_gutters() noexcept;
bool probe_message_display() noexcept;
bool probe_plugin_surfaces() noexcept;
bool probe_chat_name_styles() noexcept;
bool probe_dm_name_styles() noexcept;
bool probe_transparent_guild_icon() noexcept;
bool probe_braille_profile() noexcept;
bool probe_profile_member_modal() noexcept;
bool probe_profile_card() noexcept;
bool probe_profile_activity_card() noexcept;
bool probe_rich_composer() noexcept;
bool probe_attachment_save_affordances() noexcept;
bool probe_image_viewer() noexcept;
bool probe_voice_message_bar() noexcept;
bool probe_reaction_picker_server_icon() noexcept;
bool probe_popup_overlays() noexcept;
bool create_probe_window() noexcept;
bool once_requested() noexcept;
bool device_owner_empty(const D3D11DeviceOwner& owner) noexcept;
bool device_owner_ready(const D3D11Renderer& renderer) noexcept;
bool draw_pipeline_empty(const D3D11DrawPipelineOwner& owner) noexcept;
bool draw_pipeline_ready(const D3D11Renderer& renderer) noexcept;
bool glyph_text_owner_empty(const D3D11GlyphTextOwner& owner) noexcept;
bool glyph_text_owner_ready(const D3D11Renderer& renderer) noexcept;
bool message_hit_cache_empty(const MessageHitTestCacheOwner& owner) noexcept;
bool composer_layout_empty(const TextEditLayoutOwner& owner) noexcept;
bool content_text_retention_empty(const ContentTextRetentionOwner& owner) noexcept;
bool resize_stress_requested() noexcept;
bool run_resize_stress() noexcept;
bool initialize_probe_renderer() noexcept;
bool probe_bootstrap_title() noexcept;
bool probe_startup_glyph_atlas() noexcept;
bool probe_signout_glyph_atlas() noexcept;
bool probe_long_embed_text() noexcept;
bool probe_embed_code_block() noexcept;

} // namespace ui_probe_detail

using namespace ui_probe_detail;

} // namespace maicord
