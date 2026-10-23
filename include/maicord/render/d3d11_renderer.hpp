#pragma once
#include "maicord/ui/profile_scene.hpp"
#include "maicord/ui/profile_dialog_scene.hpp"
#include "maicord/render/text_effect.hpp"

#include "maicord/core/base.hpp"
#include "maicord/render/animation_frame_cache_gpu.hpp"
#include "maicord/render/content_text_retention.hpp"
#include "maicord/render/d3d11_background_fx.hpp"
#include "maicord/render/d3d11_device.hpp"
#include "maicord/render/d3d11_draw_pipeline.hpp"
#include "maicord/render/d3d11_glyph_text.hpp"
#include "maicord/render/d3d11_gpu_timer.hpp"
#include "maicord/render/icon_atlas.hpp"
#include "maicord/render/text_edit_layout.hpp"
#include "maicord/render/text_form_surface.hpp"
#include "maicord/render/forum_post_surface.hpp"
#include "maicord/render/message_hit_test.hpp"
#include "maicord/render/media_atlas_gpu.hpp"
#include "maicord/render/presentation_damage.hpp"
#include "maicord/render/renderer_media_draw_storage.hpp"
#include "maicord/ui/caption_bar.hpp"
#include "maicord/ui/login_scene.hpp"
#include "maicord/ui/login_view.hpp"
#include "maicord/ui/message_action_bar.hpp"
#include "maicord/ui/message_scene.hpp"
#include "maicord/ui/message_composer_scene.hpp"
#include "maicord/ui/member_list_scene.hpp"
#include "maicord/ui/message_text_presentation.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/ui/reaction_viewer.hpp"
#include "maicord/ui/image_viewer.hpp"
#include "maicord/ui/video_scene.hpp"
#include "maicord/ui/audio_controls.hpp"
#include "maicord/ui/poll_controls.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/theme_editor.hpp"
#include "maicord/ui/tooltip.hpp"
#include "maicord/ui/unread_banner_scene.hpp"
#include "maicord/ui/toast.hpp"
#include "maicord/ui/typing_indicator.hpp"
#include "maicord/ui/friends_scene.hpp"
#include "maicord/ui/search_scene.hpp"
#include "maicord/ui/pinned_messages_scene.hpp"
#include "maicord/ui/inbox_scene.hpp"
#include "maicord/ui/thread_browser_scene.hpp"
#include "maicord/ui/quick_switcher_scene.hpp"
#include "maicord/ui/message_forward_scene.hpp"
#include "maicord/ui/download_scene.hpp"
#include "maicord/ui/shortcuts_scene.hpp"
#include "maicord/ui/call_scene.hpp"
#include "maicord/ui/stream_card_scene.hpp"
#include "maicord/ui/guild_card_scene.hpp"
#include "maicord/ui/guild_banner.hpp"
#include "maicord/ui/media_picker.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/qr_code.hpp"
#include "maicord/ui/navigation_view.hpp"
#include "maicord/ui/guild_rail_scene.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/ui/virtual_list.hpp"
#include "maicord/ui/theme.hpp"
#include "maicord/ui/thread_header.hpp"

#include "maicord/core/win32_records.hpp"
#include "maicord/sys/d3d11.hpp"
#include <stddef.h>

struct ID3D11Buffer;
struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

namespace maicord {

struct D3D11Renderer;
struct RendererTypographyOwner;
struct RendererTypographyCandidate {
    RendererTypographyOwner* owner{};
    D3D11Renderer* renderer{};
    u64 source_identity{};
    u32 source_revision{};
    bool prepared{};
};

inline constexpr u32 kRendererContentGlyphCapacity = 8 * 1024;
inline constexpr u32 kRendererContentBatchCapacity = 128;
inline constexpr u32 kRendererContentTextUnitCapacity = 4 * 1024;
// One short inline-code range plus each separator can produce a separate
// shaped run. A legal embed description can exceed 128 runs while fitting
// comfortably in the existing text/glyph scratch. This storage shares the
// larger REST-response arena, so this bound does not enlarge that allocation.
inline constexpr u32 kRendererContentRunCapacity = 2'048;
inline constexpr u32 kRendererTextDecorationCapacity = 1'024;
inline constexpr u32 kRendererContentSemanticCapacity = 1024;
inline constexpr u32 kRendererContentHitRegionCapacity = 2048;
// One resident asset can be instanced many times (for example a repeated
// custom emoji), so draw capacity is independent of atlas identity capacity.
inline constexpr u32 kRendererMessageMediaCommandCapacity = 256;
inline constexpr u16 kRendererAnimationFrameCacheMaximumFrameExtent =
    kAnimationFrameCacheGpuMaximumFrameExtent;
inline constexpr u32 kRendererLoginSelectionCapacity = kTextEditSelectionCapacity;
inline constexpr u32 kRendererLoginCompositionCapacity = kTextCompositionSpanCapacity;

struct RendererLoginCompositionMetric {
    TextEditMetric metric;
    TextCompositionSpan span;
};

struct GlyphDrawBatch {
    UiRect clip;
    u32 first_quad;
    u32 quad_count;
};

struct RendererPresentationStamp {
    u64 primary;
    u64 secondary;
    // Every input except the message timeline's (its scene, text and
    // selection): unchanged, the paint built before the timeline is too.
    u64 shell;
    u32 count;
    u32 shell_count;
    bool valid;
    // Captured without caret visibility and hovered widgets: those inputs
    // may change paint quads but never draw structure.
    bool structural;
    bool timeline; // Pushes go to primary/secondary only.
};

static_assert(sizeof(RendererPresentationStamp) == 40);

using RendererLoginStatus = LoginStatus;
using RendererLoginView = LoginView;

enum class RendererTextBuildError : u8 {
    none,
    invalid_state,
    layout,
    shape,
    capacity,
    atlas,
    composer,
};

struct RendererTextStorage {
    GlyphQuad* quads;
    GlyphDrawBatch* batches;
    wchar_t* utf16;
    TextGlyph* glyphs;
    TextRun* runs;
    u16* clusters;
    TextHitSpan* hit_spans;
    TextHitRegion* hit_region_scratch;
    TextDecoration* decorations;
    MessageTextSemantic* content_semantics;
    TextHitRegion* content_hit_regions;
    u32 quad_capacity;
    u32 batch_capacity;
    u32 utf16_capacity;
    u32 glyph_capacity;
    u32 run_capacity;
    u32 cluster_capacity;
    u32 hit_span_capacity;
    u32 hit_region_scratch_capacity;
    u32 decoration_capacity;
    u32 content_semantic_capacity;
    u32 content_hit_region_capacity;
    u32 quad_count;
    u32 batch_count;
    u32 content_semantic_count;
    u32 content_hit_region_count;
    u32 rebuilds;
    u32 rejected;
    u32 semantic_truncations;
    u32 message_geometry_quads;
    HRESULT native_error;
    RendererTextBuildError last_error;
    TextShapeError last_shape_error;
    u16 substitutions;
    ShapedText scratch;
};

struct RendererLoginFormTextStorage;
struct RendererLoginFormTextMetrics {
    usize owner_bytes, reserved_bytes, committed_bytes;
    u32 commands, display_bytes;
};

enum class RendererInlineMediaLayer : u8 {
    base,
    profile,
    gallery,
    popup,
    settings_choice,
    settings_page,
    theme_editor,
    form,
    search,
    forum,
    count
};
inline constexpr u8 kRendererSearchSemanticFlag = 0x80u;
inline constexpr u32 kRendererInlineFlagCapacity = 512;
struct RendererInlineFlag {
    UiWidgetReference owner;
    UiRect rectangle, clip;
    u16 flag;
    RendererInlineMediaLayer layer;
};
struct RendererInlineMediaStorage {
    RendererInlineFlag* flags;
    u32 count, rejected;
};

// Cursors into existing storage; no second glyph, semantic or source bank.
// Shell commands lead the content stream, then messages, then overlays.
// A build whose shell projection and glyph inputs are unchanged keeps the
// shell's quads (shell_*) and rewrites only messages and overlays; one whose
// messages are also unchanged keeps both and rewrites the overlays.
struct RendererTextShellPrefix {
    RendererTextStorage* storage;
    u64 signature;
    u64 theme_signature;
    // The shell projection itself (emitter signature and count) and a stamp
    // of the scenes and stores it reads, for scroll_hint builds.
    u64 projection_signature;
    u64 scene_stamp;
    u32 projection_count;
    u32 quads, batches, semantics, hits, effects, inline_flags;
    u32 dpi, atlas_evicted_entries, atlas_full_resets;
    u32 build_serial, retention_generation, reuses, projections_skipped;
    bool animated;
    // Set by the host around a rebuild in which only the message viewport
    // moved: an unchanged scene stamp then skips projecting the shell at all.
    bool scroll_hint;
    // The member list rows end the shell. The part before them (projection
    // signature and count, its glyph key with the inputs the projection does
    // not hash, stream marks with the last batch's own quad count, and the
    // rebuild that rastered it) stays when only the members change.
    u64 static_signature, static_key;
    u32 static_count, static_quads, static_batches, static_semantics, static_hits, static_effects,
        static_inline_flags, static_last_batch_quads, static_serial;
    bool static_animated, static_valid;
};
static_assert(sizeof(RendererTextShellPrefix) == 160);
struct RendererTextPrefix {
    RendererTextStorage* storage;
    UiWidgetReference message_root;
    u32 quads, batches, semantics, hits, effects, inline_flags;
    u32 build_serial, base_builds, overlay_builds;
    bool animated;
    RendererTextShellPrefix shell;
};
static_assert(sizeof(RendererTextPrefix) == 224);

enum class RendererNavigationHitKind : u8 {
    none,
    guild,
    channel,
    home,
};

struct RendererNavigationHit {
    u64 id;
    RendererNavigationHitKind kind;
    bool selectable;
};

using RendererMessageTextPositionHit = MessagePositionHit;

// Draw-time factors only: retained geometry/text/media signatures exclude them.
struct RendererLayerOpacity {
    float gallery, popup, settings_choice, settings_page, theme_editor, profile;
    float forum_form;
};
static_assert(sizeof(RendererLayerOpacity) == 28);

// The previous built frame, kept to turn a rebuild whose draw structure is
// unchanged into damage covering only the quads that differ.
struct RendererPaintDiff {
    StableStorage paint_quads;
    StableStorage media_quads;
    StableStorage media_handles;
    StableStorage media_domains;
    StableStorage media_batches;
    PaintClip clips[kMaxPaintClips];
    PaintBatch batches[kMaxPaintBatches];
    RendererPresentationStamp structure;
    u64 text_signature;
    u64 layout_signature;
    u32 paint_count, clip_count, batch_count, media_count, media_batch_count;
    u32 partial_rebuilds;
    // Diagnostic: the first failed condition of the last comparison (0 = partial).
    u8 reject_reason;
    bool pending;
    bool valid;
};

// The paint and media build_layout emits before the message timeline (the
// chrome, server rail, channel and member lists and their avatars), kept
// with the key it was built under: the presentation stamp's shell hash and
// the composer's selection and caret. A rebuild driven by a stamp change in
// the timeline alone (a scroll, new history) replays it instead of building it
// again; an invalidated stamp (an unhashed input moved) builds it afresh.
struct RendererShellSegment {
    StableStorage paint_quads;
    StableStorage media_quads;
    StableStorage media_handles;
    StableStorage media_domains;
    StableStorage media_batches;
    PaintClip clips[kMaxPaintClips];
    PaintBatch batches[kMaxPaintBatches];
    UiRect message_view, member_sidebar;
    u64 key;
    u32 paint_count, clip_count, batch_count, media_count, media_batch_count;
    u32 builds, replays;
    bool member_sidebar_visible;
    bool valid;
};

struct PickerGlyphCache;
struct ShapedTextCache;
struct RendererCaption;

// The composer's animated typing dots in the built paint stream. A step
// (renderer_step_typing_dots) patches their quads in place, writes them past
// the stream's end in the instance buffer (no-overwrite, until it is full) and
// presents only their damage; the next full upload restores them in place.
struct RendererTypingDots {
    UiTypingDotsPaint paint;
    // Instance index the dots draw from, and the end of the step ring.
    u32 gpu_first, ring_end;
    u32 steps, uploads;
    bool upload_pending;
};

struct D3D11Renderer {
    // Frequent draw/layout accesses use short x64 displacements. Keep these
    // fields ahead of the large inline effect, atlas and selection arrays.
    D3D11DeviceOwner graphics;
    RendererTextStorage* content_text;
    u32 media_quad_count;
    u32 media_batch_count;
    // Content size: the target less the caption rows above it.
    u32 width;
    u32 height;
    u32 dpi;
    // Startup storage remains zero-filled; renderer_initialize binds identity.
    RendererLayerOpacity layer_opacity;
    float background_phase_seconds;
    u64 background_next_frame_ms;
    bool background_motion_running;
    TextEffectConstants text_effects;
    ID3D11Buffer* text_effect_buffer;
    u32 text_effect_count;
    bool text_effect_animated;
    bool reduced_motion;
    u8 text_effect_uploaded_count;
    u64 text_effect_next_ms;
    // Per material slot: DIP area an animated effect can touch, including its
    // vertex-shader displacement. Shader time is not part of the retained
    // stamp; animation ticks repaint only these areas.
    UiRect text_effect_bounds[kTextEffectCapacity];
    ProfileScene* profile_scene;
    // The DM profile panel in the shell's member slot (a ProfileScene in panel
    // presentation): base content, drawn with the shell's paint, media and text.
    ProfileScene* dm_profile_panel;
    RendererTypographyOwner* typography_owner;
    u32 typography_revision;
    u16 text_measure_revision;
    u32 native_text_faces[static_cast<u32>(UiTextFace::count)];
    D3D11DrawPipelineOwner pipeline;
    D3D11GlyphTextOwner glyph_text;
    IconAtlas icon_atlas;
    PickerGlyphCache* picker_glyph_cache;
    ShapedTextCache* shaped_text_cache;
    // Diagnostic A/B switch; shaping results are otherwise reused across rebuilds.
    bool shaped_text_cache_disabled;
    const LoginScene* login_scene;
    QrCode login_qr_code;
    MediaAtlas* media_atlas;
    MediaAtlas* preview_media_atlas;
    MediaAtlasGpu media_gpu;
    AnimationFrameCacheGpu animation_gpu;
    AnimationFrameCacheGpu preview_animation_gpu;
    RendererMediaDrawStorage media_draw;
    const UiScene* ui_scene;
    const UiThreadHeader* thread_header;
    const UiWidgetManager* focus_owner;
    const GuildRailScene* guild_rail_scene;
    const ShellNavigationScene* shell_navigation_scene;
    const MessageScene* message_scene;
    const MessageActionBar* message_action_bar;
    const MessageComposerScene* message_composer_scene;
    const MemberListScene* member_list_scene;
    const PopupScene* popup_scene;
    UiRect scrollbar_viewport;
    TextEditLayoutOwner profile_edit_layout;
    TextEditMetric profile_edit_selections[kTextEditSelectionCapacity];
    TextEditLayoutOwner popup_edit_layout;
    TextEditMetric popup_edit_selections[kTextEditSelectionCapacity];
    TextFormSurface* text_form;
    ForumPostSurface* forum_form;
    const ReactionViewer* reaction_viewer;
    const UiImageViewer* image_viewer;
    const UiVideoScene* video_scene;
    bool video_scene_search_result;
    bool video_scene_viewer; // The scene is hosted by the media viewer.

    const UiAudioScene* audio_scene;
    const UiPollScene* poll_scene;
    const UiPollVotersScene* poll_voters_scene;
    const UiSettingsScene* settings_scene;
    const UiThemeEditorScene* theme_editor_scene;
    const UiTooltipScene* tooltip_scene;
    const UiUnreadBannerScene* unread_banner_scene;
    // Drawn last, above every layer, at toast_opacity from its first batches.
    const UiToastScene* toast_scene;
    u32 toast_paint_first_batch, toast_text_first_batch;
    float toast_opacity;
    const UiFriendsScene* friends_scene;
    const UiFriendsScene* friends_requests_scene;
    const UiSearchScene* search_scene;
    const MessageTextPresentation* search_message_text;
    UiTextMeasureCache* search_message_measure_cache;
    const UserStore* search_message_users;
    TextEditLayoutOwner search_layout;
    TextEditMetric search_edit_selections[kTextEditSelectionCapacity];
    UiEditGeometry search_edit_geometry;
    u32 interaction_text_search_revision;
    const UiPinnedMessagesScene* pinned_messages_scene;
    u32 interaction_text_pinned_messages_revision;
    // Inbox popout, painted in the popup layer.
    const UiInboxScene* inbox_scene;
    u32 interaction_text_inbox_revision;
    UiThreadBrowserScene* thread_browser_scene;
    u64 interaction_text_thread_browser_publication;
    u32 interaction_text_thread_browser_revision;
    const UiQuickSwitcherScene* quick_switcher_scene;
    u32 interaction_text_quick_switcher_revision;
    const UiMessageForwardScene* message_forward_scene;
    u32 interaction_text_message_forward_revision;
    const UiDownloadScene* download_scene;
    u32 interaction_text_download_revision;
    // Keyboard shortcuts overlay (Ctrl+/), drawn above the message modals.
    const UiShortcutsScene* shortcuts_scene;
    u32 interaction_text_shortcuts_revision;
    // Call view over the content column (or the whole window in fullscreen),
    // drawn in the side-panel layer between the base content and popups.
    const UiCallScene* call_scene;
    u32 interaction_text_call_revision;
    // Go Live preview card beside a voice channel's live user, popup layer.
    const UiStreamCardScene* stream_card_scene;
    u32 interaction_text_stream_card_revision;
    // Server tag popout (popup layer), opened from a server tag chip.
    const UiGuildCardScene* guild_card_scene;
    u32 interaction_text_guild_card_revision;
    // The selected server's banner over the channel list (shell layer).
    const UiGuildBannerView* guild_banner;
    const UiMediaPickerScene* media_picker_scene;
    TextEditLayoutOwner media_picker_layout;
    TextEditMetric media_picker_edit_selections[kTextEditSelectionCapacity];
    UiEditGeometry media_picker_edit_geometry;
    u32 interaction_text_media_picker_revision;
    MediaAssetKey image_viewer_key, image_viewer_fallback;
    // The hovered chat GIF's media widget and its favourite state.
    UiWidgetHandle gif_favorite_owner;
    bool gif_favorite, gif_favorite_pending;
    u32 interaction_text_profile_revision;
    u32 interaction_text_image_viewer_revision;
    u64 interaction_text_video_revision;
    u64 interaction_text_audio_revision;
    u64 interaction_text_poll_revision;
    u64 interaction_text_poll_voters_revision;
    u32 interaction_text_settings_revision;
    u32 interaction_text_theme_editor_revision;
    u32 interaction_text_tooltip_revision;
    u32 interaction_text_unread_banner_revision;
    u32 interaction_text_toast_revision;
    u32 interaction_text_friends_revision;
    u32 interaction_text_friends_requests_revision;
    const MessageTextPresentation* message_text;
    // Borrowed from the same account/probe owner as message_text. Author
    // measurement must remain available when the member pane is absent.
    UiTextMeasureCache* message_measure_cache;
    const ChatStateStore* chat_state;
    const UserStore* user_store;
    const NavigationView* navigation_view;
    RendererLoginFormTextStorage* login_form_text;
    RendererInlineMediaStorage inline_media;
    u32 inline_media_first_batch, inline_media_last_batch;
    u32 inline_media_layer_first[static_cast<u32>(RendererInlineMediaLayer::count)];
    u32 inline_media_layer_end[static_cast<u32>(RendererInlineMediaLayer::count)];
    ContentTextRetentionOwner content_text_retention;
    RendererTextPrefix content_text_prefix;
    Arena shell_text_storage;
    D3D11GpuTimerOwner gpu_timer;
    // Background effect pass (Stars/Comets or the selected plugin effect).
    D3D11BackgroundFxOwner background_fx;
    TextEditMetric edit_selections[kTextEditSelectionCapacity];
    TextEditLayoutOwner* edit_selection_owner;
    TextEditLayoutOwner composer_layout;
    // Friends borrows composer_layout while the chat composer is hidden.
    UiEditGeometry friends_search_geometry;
    TextEditLayoutOwner login_edit;
    MessageHitTestCacheOwner message_hit_test;
    RendererLoginCompositionMetric login_composition_metrics[kRendererLoginCompositionCapacity];
    PaintStream paint_stream;
    RendererPresentationStamp retained_presentation;
    PresentationDamageTracker presentation_damage;
    RendererPaintDiff paint_diff;
    RendererShellSegment shell_segment;
    RendererTypingDots typing_dots;
    Win32Rect present_dirty_rect;
    // Flip-sequential rotates between two independently retained back buffers.
    // `previous_buffer_damage` is the logical damage submitted on the prior
    // Present; it must be repainted into the next buffer before that buffer is
    // displayed, even when the current BeginFrame contains no new mutation.
    Win32Rect previous_buffer_damage_rect;
    Win32Rect present_logical_damage_rect;
    i64 performance_frequency;
    u64 paint_cpu_us_total;
    u64 paint_cpu_us_max;
    u64 first_frame_cpu_us;
    // renderer_initialize adopted the prestarted device (bit 0), draw
    // pipeline (bit 1) and glyph atlas (bit 2) instead of creating them on the
    // main thread.
    u8 startup_prestart;
    u32 painted_frames;
    u32 draw_calls_last_frame;
    u32 draw_calls_max;
    u32 resize_clipped_updates;
    u32 media_draws_last_frame;
    u32 media_draws_total;
    u64 preview_media_uploaded_pixels;
    u32 preview_media_uploads;
    u32 media_rejected;
    u32 media_atlas_resizes; // Atlas growth steps (texture recreated, rows copied).
    u32 media_atlas_trims;   // Idle trims that shrank, compacted or released media surfaces.
    u32 media_instance_uploads;
    u32 text_instance_uploads;
    u32 paint_instance_uploads;
    // Instance bytes written to the GPU (paint, glyph and media streams).
    u64 paint_upload_bytes, text_upload_bytes, media_upload_bytes;
    u32 paint_stream_builds;
    u32 paint_stream_reuses;
    u32 media_source_refreshes;
    u32 media_source_upload_skips;
    u32 full_present_draws;
    u32 partial_present_draws;
    u32 unchanged_present_skips;
    u32 idle_begin_frame_presents;
    u64 partial_present_pixels;
    u32 media_overlay_paint_first_batch;
    u32 profile_foreground_first_batch, profile_controls_first_batch;
    u32 profile_paint_first_batch, profile_paint_end_batch;
    u32 profile_text_first_batch, profile_text_end_batch;
    u32 profile_media_first_batch, profile_effect_media_first_batch, profile_media_end_batch;
    u32 popup_media_first_batch;
    u32 menu_paint_first_batch;
    u32 menu_media_first_batch;
    u32 menu_text_first_batch;
    u32 overlay_paint_first_batch;
    u32 edit_overlay_paint_first_batch;
    u32 overlay_text_first_batch;
    u32 friends_requests_foreground_first_batch, friends_requests_paint_end_batch;
    u32 friends_requests_media_first_batch, friends_requests_media_end_batch;
    u32 friends_requests_text_end_batch;
    u32 media_picker_paint_first_batch, media_picker_paint_end_batch;
    u32 media_picker_foreground_first_batch;
    u32 media_picker_media_first_batch, media_picker_media_end_batch;
    u32 media_picker_text_first_batch, media_picker_text_end_batch;
    // Half-open ranges isolate only PopupScene, excluding reaction/image viewers.
    u32 popup_paint_first_batch, popup_paint_end_batch;
    u32 popup_scene_media_first_batch, popup_scene_media_end_batch;
    u32 popup_text_first_batch, popup_text_end_batch;
    // Server tag popout with a banner picture: its card and banner paint, the
    // banner media, then its foreground (icon ring, chips) under the icon.
    u32 guild_card_foreground_first_batch, guild_card_banner_media_end_batch;
    // Settings page stays outside its optional choice popup's ranges.
    u32 settings_choice_paint_first_batch, settings_choice_paint_end_batch;
    u32 settings_choice_media_first_batch, settings_choice_media_end_batch;
    u32 settings_choice_text_first_batch, settings_choice_text_end_batch;
    u32 theme_editor_paint_first_batch, theme_editor_paint_end_batch;
    // Quick switcher status dots: drawn over its avatar media, faded and
    // scaled with the popup layer.
    u32 quick_switcher_status_first_batch, quick_switcher_status_end_batch;
    u32 theme_editor_text_first_batch, theme_editor_text_end_batch;
    u32 settings_page_paint_first_batch, settings_page_paint_end_batch;
    // Settings avatar rows (My account card): media over the page's initials.
    u32 settings_page_media_first_batch, settings_page_media_end_batch;
    u32 settings_page_text_first_batch, settings_page_text_end_batch;
    // Profile editor overlays on a settings page: the live preview card (a
    // ProfileScene in preview presentation) and its modal dialog (image crop,
    // collectible picker). Each draws paint, media, foreground, then text.
    const ProfileScene* settings_profile_preview;
    const UiProfileDialogScene* settings_profile_dialog;
    u32 settings_preview_paint_first_batch, settings_preview_foreground_first_batch,
        settings_preview_paint_end_batch;
    u32 settings_preview_media_first_batch, settings_preview_effect_media_first_batch,
        settings_preview_media_end_batch;
    u32 settings_preview_controls_first_batch;
    u32 settings_preview_text_first_batch, settings_preview_text_end_batch;
    u32 settings_dialog_paint_first_batch, settings_dialog_foreground_first_batch,
        settings_dialog_paint_end_batch;
    u32 settings_dialog_media_first_batch, settings_dialog_media_end_batch;
    u32 settings_dialog_text_first_batch, settings_dialog_text_end_batch;
    // Modal settings: dialog batches drawn with the entrance transform over the
    // scrim, above an offscreen render of the live shell. The shell is drawn
    // again only when its stamp (content, theme, size) changes.
    u32 settings_modal_paint_first_batch, settings_modal_paint_end_batch;
    float settings_modal_progress;
    ID3D11Texture2D* settings_backdrop;
    ID3D11ShaderResourceView* settings_backdrop_view;
    ID3D11RenderTargetView* settings_backdrop_target;
    ID3D11Buffer* settings_backdrop_quad;
    u32 settings_backdrop_width, settings_backdrop_height;
    // The shell stamp the backdrop was last drawn (or rejected) for, and the
    // media it drew: a new frame of any of them (GIF, animated emoji or
    // avatar) redraws the backdrop at that animation's own cadence.
    RendererPresentationStamp settings_backdrop_stamp;
    u32 settings_backdrop_renders, settings_backdrop_media_count;
    MediaAtlasHandle settings_backdrop_media[kRendererMediaQuadCapacity];
    u8 settings_backdrop_media_domain[kRendererMediaQuadCapacity];
    bool settings_backdrop_valid, settings_backdrop_failed;
    // Layer transform currently bound while drawing; scissors follow it.
    float draw_layer_scale, draw_layer_origin_x, draw_layer_origin_y;
    // Popup entrance: uniform scale about a DIP origin, 1 when settled.
    float popup_layer_scale, popup_layer_origin_x, popup_layer_origin_y;
    u32 form_text_first_batch, form_paint_first_batch;
    u32 forum_media_first_batch, forum_media_end_batch;
    u32 forum_paint_end_batch, forum_text_end_batch;
    u32 interaction_text_action_bar_revision;
    u32 interaction_text_popup_revision;
    u32 interaction_text_reaction_viewer_revision;
    u32 interaction_text_form_revision;
    u32 interaction_text_forum_form_revision;
    u32 stale_interaction_text_suppressions;
    u32 login_composition_metric_count;
    u64 message_text_selection_message_id;
    u64 message_text_selection_command_identity;
    u32 message_text_selection_revision;
    u32 message_text_selection_command_index;
    u32 message_text_selection_anchor;
    u32 message_text_selection_caret;
    u32 uploaded_text_rebuild;
    u32 uploaded_text_quad_count;
    const RendererTextStorage* uploaded_text_storage;
    // Content glyphs [0, text_stable_quads) are unchanged since the text
    // rebuild numbered text_stable_serial: the shell (or, after an
    // overlay-only rebuild, the shell and messages). An upload leaves them
    // where the glyph ring already holds them and writes only the rest.
    u32 text_stable_quads, text_stable_serial, text_shell_serial, text_message_serial;
    // Scroll positions of the shell's lists when the shell was last rastered.
    u64 text_shell_scroll_key;
    // The channel list's part of it: unchanged while only the members moved.
    u64 text_shell_navigation_scroll_key;
    // The uploaded stream in the glyph ring: [0, uploaded_text_prefix) from
    // instance prefix_base (written by text rebuild prefix_serial in ring
    // epoch prefix_epoch), the remainder from instance suffix_base.
    u32 uploaded_text_prefix, uploaded_text_prefix_base, uploaded_text_suffix_base;
    u32 uploaded_text_prefix_serial, uploaded_text_prefix_epoch, text_ring_epoch;
    // Window caption drawn above the content (renderer_set_caption_height).
    RendererCaption* caption;
    u32 caption_height;
    // Rows above the content in the bound target: caption_height for the swap
    // chain, zero for offscreen targets that hold only content.
    u32 target_origin_y;
    bool has_presented;
    bool present_required;
    bool present_full;
    bool previous_buffer_damage_valid;
    bool previous_buffer_damage_full;
    bool present_logical_damage_valid;
    bool present_logical_damage_full;
    bool live_resize;
    bool paint_overflow_reported;
    bool login_visible;
    bool login_qr_ready;
    bool login_caret_visible;
    bool begin_frame_permit;
    u32 interaction_text_active_mask;
    RendererLoginStatus login_status;
};
static_assert(offsetof(D3D11Renderer, dpi) < 128);
static_assert(sizeof(D3D11Renderer) == 120712);

// Starts hardware device and draw-pipeline creation on a worker at process
// entry; the next renderer_initialize adopts them. Main thread only.
void renderer_prestart() noexcept;
// Joins the worker and releases anything renderer_initialize did not adopt.
void renderer_prestart_cancel() noexcept;
// content_text: content text (kRendererContentGlyphCapacity glyph quads)
// binds before the first frame; its glyph ring is created up front.
bool renderer_initialize(D3D11Renderer* renderer, HWND window, bool content_text = false) noexcept;
bool renderer_resize(D3D11Renderer* renderer, u32 width, u32 height) noexcept;
// The top `height` pixels of the swap chain hold the window caption; width,
// height and every content coordinate exclude them. Before renderer_initialize
// the initial client splits the same way; afterwards follow a change with
// renderer_resize to the new content size. Zero (the default) removes it.
void renderer_set_caption_height(D3D11Renderer* renderer, u32 height) noexcept;
// Caption state and title for the next frame. The caption is redrawn, and
// presented as its own dirty rectangle, only when its appearance changes.
bool renderer_set_caption(D3D11Renderer* renderer, const UiCaptionBarState& state, Utf8View title) noexcept;
// The same with the title's glyph or server icon (ui_caption_title).
bool renderer_set_caption(D3D11Renderer* renderer, const UiCaptionBarState& state,
                          const UiCaptionTitle& title) noexcept;
// The app icon for the caption: `extent` x `extent` premultiplied BGRA, kept
// as one texture until another extent replaces it. Needs renderer_set_caption.
bool renderer_set_caption_icon(D3D11Renderer* renderer, const u32* pixels, u32 extent) noexcept;
// Viewport origin for the bound target; an offscreen content-only replay
// binds zero and restores caption_height.
void renderer_bind_target_origin(D3D11Renderer* renderer, u32 origin_y) noexcept;
// Only sampled presentation state and full-surface damage change. Scene,
// widget, retained paint/text and media generations remain untouched.
// Returns success, including an unchanged valid phase; invalid input fails.
bool renderer_set_background_phase(D3D11Renderer*, float seconds) noexcept;
// The host's selected plugin background effect (validated ps_5_0 bytecode),
// applied each frame; repaints when the shader or parameters change. Returns
// the device's CreatePixelShader failure.
HRESULT renderer_select_plugin_effect(D3D11Renderer*, u64 owner_id, u64 effect_id, u64 revision,
                                      ConstSlice<u8> bytecode, const float* parameters) noexcept;
// Back to the theme's built-in effect.
void renderer_clear_plugin_effect(D3D11Renderer*) noexcept;
// Resize keeps an acquired swap-chain slot. Only a successful Present consumes it.
bool renderer_finish_resize(D3D11Renderer* renderer, u32 width, u32 height) noexcept;
void renderer_set_live_resize(D3D11Renderer* renderer, bool live_resize) noexcept;
void renderer_accept_begin_frame(D3D11Renderer* renderer) noexcept;
bool renderer_present(D3D11Renderer* renderer, const Theme& theme) noexcept;
bool renderer_bind_ui_scene(D3D11Renderer* renderer, const UiScene* scene) noexcept;
bool renderer_bind_thread_header(D3D11Renderer* renderer, const UiThreadHeader* header) noexcept;
bool renderer_bind_thread_browser_scene(D3D11Renderer* renderer, UiThreadBrowserScene* scene) noexcept;
bool renderer_bind_audio_scene(D3D11Renderer* renderer, const UiAudioScene* scene) noexcept;
bool renderer_bind_poll_scene(D3D11Renderer*, const UiPollScene*) noexcept;
bool renderer_bind_poll_voters_scene(D3D11Renderer*, const UiPollVotersScene*) noexcept;
// Caller owns the manager until unbound. Generational focus and layout changes
// participate in retained presentation invalidation.
void renderer_bind_focus_owner(D3D11Renderer* renderer, const UiWidgetManager* owner) noexcept;
bool renderer_bind_guild_rail_scene(D3D11Renderer* renderer, const GuildRailScene* scene) noexcept;
bool renderer_bind_shell_navigation_scene(D3D11Renderer* renderer,
                                          const ShellNavigationScene* scene) noexcept;
bool renderer_bind_message_scene(D3D11Renderer* renderer, const MessageScene* scene) noexcept;
bool renderer_bind_message_action_bar(D3D11Renderer* renderer, const MessageActionBar* bar) noexcept;
bool renderer_bind_message_composer_scene(D3D11Renderer* renderer,
                                          const MessageComposerScene* scene) noexcept;
bool renderer_bind_member_list_scene(D3D11Renderer* renderer, const MemberListScene* scene) noexcept;
bool renderer_bind_popup_scene(D3D11Renderer* renderer, const PopupScene* scene) noexcept;
void renderer_bind_forum_form(D3D11Renderer* renderer, ForumPostSurface* surface) noexcept;
void renderer_bind_text_form(D3D11Renderer* renderer, TextFormSurface* surface) noexcept;
// Model-free login confirmations prepare this lazy text bank before measuring,
// then refresh the current login view before rasterization. Detach the form
// before releasing; ordinary login never allocates this storage.
bool renderer_prepare_login_form_text(D3D11Renderer* renderer) noexcept;
void renderer_release_login_form_text(D3D11Renderer* renderer) noexcept;
RendererLoginFormTextMetrics renderer_login_form_text_metrics(const D3D11Renderer& renderer) noexcept;
bool renderer_bind_reaction_viewer(D3D11Renderer* renderer, const ReactionViewer* viewer) noexcept;
bool renderer_measure_message_composer(D3D11Renderer* renderer, float* preferred_height) noexcept;
bool renderer_bind_message_text_presentation(D3D11Renderer* renderer,
                                             const MessageTextPresentation* presentation) noexcept;
void renderer_unbind_message_text_presentation(D3D11Renderer* renderer) noexcept;
bool renderer_bind_search_message_text_presentation(D3D11Renderer*, const MessageTextPresentation*,
                                                    const UserStore*, UiTextMeasureCache*) noexcept;
void renderer_unbind_search_message_text_presentation(D3D11Renderer*) noexcept;
bool renderer_set_login_view(D3D11Renderer* renderer, const LoginScene* scene,
                             const RendererLoginView& view, const Theme& theme) noexcept;
bool renderer_login_edit_hit_test(const D3D11Renderer& renderer, const UiRect& text_rectangle,
                                  Utf8View visible_text, i32 client_x_pixels, i32 client_y_pixels,
                                  u32* codepoint) noexcept;
bool renderer_message_composer_edit_hit_test(const D3D11Renderer& renderer, i32 client_x_pixels,
                                             i32 client_y_pixels, u32* model_byte_offset) noexcept;
bool renderer_message_composer_edit_move_vertical(const D3D11Renderer& renderer, bool down,
                                                  u32* model_byte_offset) noexcept;
void renderer_set_login_caret_visible(D3D11Renderer* renderer, bool visible) noexcept;
bool renderer_message_view_hit_test(const D3D11Renderer& renderer, i32 client_x_pixels,
                                    i32 client_y_pixels) noexcept;
bool renderer_guild_rail_hit_test(const D3D11Renderer& renderer, i32 client_x_pixels,
                                  i32 client_y_pixels) noexcept;
RendererNavigationHit renderer_navigation_hit_test(const D3D11Renderer& renderer, i32 client_x_pixels,
                                                   i32 client_y_pixels) noexcept;
void renderer_bind_navigation_view(D3D11Renderer* renderer, const ChatStateStore* state,
                                const UserStore* users, const NavigationView* view) noexcept;
bool renderer_bind_media_atlas(D3D11Renderer* renderer, MediaAtlas* atlas) noexcept;
// UI thread, every loop turn (cheap when nothing is due): records time for
// the atlases' recency windows and runs their idle trims (shrink/compact the
// media atlases, release unused disposal backups and idle frame caches).
// True when a surface changed; retained quads refresh their UVs at the next paint.
bool renderer_media_maintain(D3D11Renderer* renderer, u64 now_ms) noexcept;
// Earliest pending idle trim (0: none), for the scheduler's wait timeout.
u64 renderer_media_next_trim_ms(const D3D11Renderer& renderer) noexcept;
// Paint programs compiled by the account module's media worker.
MediaAtlasGpuResult renderer_upload_regional_flag(D3D11Renderer* renderer, MediaAssetKey key,
                                                  const ColrPaintProgram& program) noexcept;
MediaAtlasGpuResult renderer_upload_lottie_program(D3D11Renderer* renderer, MediaAssetKey key,
                                                   const ColrPaintProgram& program) noexcept;
// maximum_extent applies to call video keys only (0: native picture).
MediaAtlasGpuResult renderer_present_video_frame(D3D11Renderer* renderer, MediaAssetKey key,
                                                 const GpuVideoFrame& frame, u32 maximum_extent = 0) noexcept;
bool renderer_bind_preview_media_atlas(D3D11Renderer* renderer, MediaAtlas* atlas) noexcept;
// Copies one resident media allocation to straight RGBA8 rows (Copy image).
bool renderer_read_media_rgba(D3D11Renderer* renderer, MediaAssetKey key, Slice<u8> rgba, u32* width,
                              u32* height) noexcept;
void renderer_unbind_media_atlas(D3D11Renderer* renderer) noexcept;
MediaAtlasGpuResult renderer_upload_media_rgba8(D3D11Renderer* renderer, MediaAssetKey key, u16 width,
                                                u16 height, u32 row_pitch,
                                                ConstSlice<u8> premultiplied_rgba8) noexcept;
MediaAtlasGpuResult renderer_upload_media_straight_rgba8(D3D11Renderer* renderer, MediaAssetKey key,
                                                         u16 width, u16 height, u32 row_pitch,
                                                         ConstSlice<u8> straight_rgba8) noexcept;
bool renderer_capture_animation_frame(D3D11Renderer* renderer,
                                      const AnimationFrameCacheDescriptor& descriptor) noexcept;
bool renderer_restore_animation_frame(D3D11Renderer* renderer, const AnimationFrameCacheKey& key,
                                      AnimationFrameCacheResult* result) noexcept;
bool renderer_materialize_animation_frame(D3D11Renderer* renderer, MediaAssetKey key) noexcept;
MediaAtlasGpuResult renderer_upload_png_scanlines(D3D11Renderer* renderer, MediaAssetKey key,
                                                  const PngImage& image,
                                                  ConstSlice<u8> reconstructed_scanlines) noexcept;
MediaAtlasGpuResult renderer_upload_jpeg(D3D11Renderer* renderer, MediaAssetKey key, const JpegImage& image,
                                         const JpegCoefficientWorkspace& coefficients) noexcept;
MediaAtlasGpuResult renderer_begin_apng(D3D11Renderer* renderer, MediaAssetKey key, u16 canvas_width,
                                        u16 canvas_height, const ApngFrame& frame,
                                        const PngImage& frame_image,
                                        ConstSlice<u8> reconstructed_scanlines) noexcept;
MediaAtlasGpuResult renderer_advance_apng(D3D11Renderer* renderer, MediaAssetKey key,
                                          const ApngFrame& previous_frame, const ApngFrame& frame,
                                          const PngImage& frame_image,
                                          ConstSlice<u8> reconstructed_scanlines) noexcept;
MediaAtlasGpuResult renderer_begin_gif(D3D11Renderer* renderer, MediaAssetKey key, u16 canvas_width,
                                       u16 canvas_height, const GifFrame& frame, const GifPalette& palette,
                                       ConstSlice<u8> indices) noexcept;
MediaAtlasGpuResult renderer_advance_gif(D3D11Renderer* renderer, MediaAssetKey key,
                                         const GifFrame& previous_frame, const GifFrame& frame,
                                         const GifPalette& palette, ConstSlice<u8> indices) noexcept;
MediaAtlasGpuResult renderer_begin_animated_webp(D3D11Renderer* renderer, MediaAssetKey key,
                                                 u16 canvas_width, u16 canvas_height, u32 background_rgba,
                                                 const AnimatedWebpFrame& frame,
                                                 ConstSlice<u8> straight_rgba8) noexcept;
MediaAtlasGpuResult renderer_advance_animated_webp(D3D11Renderer* renderer, MediaAssetKey key,
                                                   u32 background_rgba,
                                                   const AnimatedWebpFrame& previous_frame,
                                                   const AnimatedWebpFrame& frame,
                                                   ConstSlice<u8> straight_rgba8) noexcept;
MediaAtlasGpuResult renderer_begin_animated_webp_vp8(
    D3D11Renderer* renderer, MediaAssetKey key, u16 canvas_width, u16 canvas_height, u32 background_rgba,
    const AnimatedWebpFrame& animation_frame, const Vp8Keyframe& vp8_frame, const Vp8ControlHeader& header,
    const Vp8ModeWorkspace& modes, const Vp8CoefficientWorkspace& coefficients,
    ConstSlice<u8> alpha) noexcept;
MediaAtlasGpuResult renderer_advance_animated_webp_vp8(
    D3D11Renderer* renderer, MediaAssetKey key, u32 background_rgba,
    const AnimatedWebpFrame& previous_frame, const AnimatedWebpFrame& animation_frame,
    const Vp8Keyframe& vp8_frame, const Vp8ControlHeader& header, const Vp8ModeWorkspace& modes,
    const Vp8CoefficientWorkspace& coefficients, ConstSlice<u8> alpha) noexcept;
MediaAtlasGpuResult renderer_reconstruct_vp8_keyframe(D3D11Renderer* renderer, const Vp8Keyframe& frame,
                                                      const Vp8ModeWorkspace& modes,
                                                      const Vp8CoefficientWorkspace& coefficients) noexcept;
MediaAtlasGpuResult renderer_filter_vp8_keyframe(D3D11Renderer* renderer, const Vp8Keyframe& frame,
                                                 const Vp8ControlHeader& header,
                                                 const Vp8ModeWorkspace& modes,
                                                 const Vp8CoefficientWorkspace& coefficients) noexcept;
MediaAtlasGpuResult renderer_upload_vp8_keyframe(D3D11Renderer* renderer, MediaAssetKey key,
                                                 const Vp8Keyframe& frame, const Vp8ControlHeader& header,
                                                 const Vp8ModeWorkspace& modes,
                                                 const Vp8CoefficientWorkspace& coefficients,
                                                 ConstSlice<u8> alpha) noexcept;
void renderer_unbind_navigation_view(D3D11Renderer* renderer) noexcept;
bool renderer_bind_text_storage(D3D11Renderer* renderer, RendererTextStorage* storage) noexcept;
void renderer_unbind_text_storage(D3D11Renderer* renderer) noexcept;
TextSystem renderer_text_system(D3D11Renderer*, UiTextFace) noexcept;
// Cold UI-thread transaction. A failed prepare/commit preserves active resources.
// Prepared candidates own resources until commit or explicit discard; no copying.
bool renderer_prepare_typography(D3D11Renderer*, const UiThemeTypography&,
                                 RendererTypographyCandidate*) noexcept;
bool renderer_commit_typography(D3D11Renderer*, RendererTypographyCandidate*) noexcept;
void renderer_discard_typography(RendererTypographyCandidate*) noexcept;
// Shutdown-only: called after the glyph TextSystem has released its policy borrow.
void renderer_release_typography(D3D11Renderer*) noexcept;
u32 renderer_typography_revision(const D3D11Renderer&) noexcept;
inline constexpr u16 renderer_text_measure_revision(const D3D11Renderer& renderer) noexcept {
    return renderer.text_measure_revision ? renderer.text_measure_revision : 1;
}
bool renderer_text_measure_backend(void* context, const UiTextMeasureRequest& request,
                                   UiTextMeasureValue* value) noexcept;
bool renderer_rebuild_content_text(D3D11Renderer* renderer, const Theme& theme) noexcept;
// Synchronize retained interaction glyphs after the host's final surface
// publication. Unchanged account frames and standalone login text stay cheap.
bool renderer_refresh_interaction_text(D3D11Renderer* renderer, const Theme& theme) noexcept;
// Replays the shell and overlay text of the bound account surfaces (not the
// standalone login card or message bodies) through consume, without drawing.
// Accessibility names text by its owner widget.
bool renderer_project_accessible_text(D3D11Renderer* renderer, const Theme& theme, UiTextConsumeFunction consume,
                                      void* context) noexcept;
bool renderer_rebuild_shell_text(D3D11Renderer* renderer, const Theme& theme) noexcept;
MessageTextHit renderer_content_hit_test(const D3D11Renderer& renderer, i32 client_x_pixels,
                                         i32 client_y_pixels) noexcept;
RendererMessageTextPositionHit renderer_message_text_position_hit_test(D3D11Renderer& renderer,
                                                                       i32 client_x_pixels,
                                                                       i32 client_y_pixels) noexcept;
void renderer_set_message_text_selection(D3D11Renderer* renderer,
                                         const RendererMessageTextPositionHit& command, u32 anchor,
                                         u32 caret) noexcept;
void renderer_clear_message_text_selection(D3D11Renderer* renderer) noexcept;
void renderer_set_dpi(D3D11Renderer* renderer, u32 dpi) noexcept;
void renderer_shutdown(D3D11Renderer* renderer) noexcept;
void renderer_bind_theme_editor_scene(D3D11Renderer*, const UiThemeEditorScene*) noexcept;
void renderer_bind_settings_scene(D3D11Renderer* renderer, const UiSettingsScene* scene) noexcept;
void renderer_release_settings_backdrop(D3D11Renderer* renderer) noexcept;
// True while a modal or dialog needs the shell beneath it redrawn: it was
// never drawn, or the shell, theme or size changed since.
bool renderer_settings_backdrop_stale(D3D11Renderer* renderer, const Theme& theme) noexcept;
// Draws the shell offscreen as it would appear with no modal, then restores
// the modal's text. A shell that cannot draw yet leaves the previous image
// (or a flat base) and returns true; false only when the modal text fails.
bool renderer_refresh_settings_backdrop(D3D11Renderer* renderer, const Theme& theme) noexcept;
// Keeps the media the backdrop was drawn from resident while it is in use.
// Call after each visibility epoch begins on a frame that draws the modal
// stream: those frames pin only the modal's own media.
void renderer_pin_settings_backdrop_media(D3D11Renderer* renderer) noexcept;
void renderer_bind_friends_scene(D3D11Renderer* renderer, const UiFriendsScene* scene) noexcept;
void renderer_bind_friends_requests_scene(D3D11Renderer* renderer, const UiFriendsScene* scene) noexcept;

// Rendering visibility only: Settings authority/read barriers remain active.
__declspec(noinline) inline bool renderer_theme_editor_visible(const D3D11Renderer& renderer) noexcept {
    return !renderer.login_visible && renderer.theme_editor_scene &&
           ui_theme_editor_current(*renderer.theme_editor_scene);
}
__declspec(noinline) inline bool renderer_settings_page_visible(const D3D11Renderer& renderer) noexcept {
    return renderer.settings_scene && renderer.settings_scene->active &&
           !renderer_theme_editor_visible(renderer);
}
// Only a full-window settings layer hides the shell. Beneath a modal or a
// dialog the shell stays live: its scenes, media and animations keep their
// own cadence and the backdrop redraws when they change.
inline bool renderer_settings_hides_shell(const D3D11Renderer& renderer) noexcept {
    return renderer_settings_page_visible(renderer) &&
           renderer.settings_scene->presentation == UiSettingsPresentation::layer;
}

} // namespace maicord
