#pragma once

#include "maicord/render/d3d11/renderer.hpp"

#include "maicord/media/core/presentation_policy.hpp"
#include "maicord/render/d3d11/renderer_presentation.hpp"
#include "maicord/ui/account/login_presentation.hpp"
#include "maicord/ui/chat/messages/interaction.hpp"
#include "maicord/ui/chat/messages/presentation.hpp"
#include "maicord/ui/chat/composer/attachment_actions.hpp"
#include "maicord/ui/voice/audio_presentation.hpp"
#include "maicord/ui/shell/popup_presentation.hpp"
#include "maicord/ui/shell/presentation.hpp"
#include "maicord/render/media/inline_media.hpp"

#include "renderer_interaction_state.hpp"


namespace maicord::renderer_layout_detail {

// Per-frame arrays too large for the stack (each probe-heavy frame paid a
// __chkstk walk): one block per renderer, committed on first use. Pages become
// resident only where a frame writes, so a typical frame touches a few.
// build_timeline_media and build_search_media never overlap.
struct LayoutScratch {
    MessageMediaCommand media_commands[kRendererMessageMediaCommandCapacity];
    UiRect composer_selections[kMessageComposerSelectionRectangleCapacity];
    TextCompositionMark composer_marks[kTextCompositionSpanCapacity];
    MessageComposerChipGeometry composer_chips[kMessageComposerChipGeometryCapacity];
    UiRect login_selections[kTextEditSelectionCapacity];
    LoginCompositionGeometry login_composition[kRendererLoginCompositionCapacity];
};

// What a media quad adds to its rectangle and clip; plain callers share kPlainMediaQuad.
struct MediaQuadStyle {
    UiRect rounded_clip;
    float opacity, radius, clip_radius, blur;
    u8 domain;
};
inline constexpr MediaQuadStyle kPlainMediaQuad{{}, 1.f, 0.f, 0.f, 0.f, 0};

// A modal that owns its scroll viewport paints with the background scrollbar filter off.
struct UnfilteredPaint {
    PaintStream* stream;
    bool filter;
    explicit UnfilteredPaint(PaintStream* paint) noexcept : stream(paint), filter(paint->filter_scrollbars) {
        paint->filter_scrollbars = false;
    }
    ~UnfilteredPaint() noexcept {
        stream->filter_scrollbars = filter;
    }
};

// Defined in renderer_layout.cpp and renderer_layout_media.cpp.
bool layout_step_failed(u32 line) noexcept;
LayoutScratch* layout_scratch(D3D11Renderer* renderer) noexcept;
MediaAssetKey emoji_key(const MessageTextSemantic& semantic) noexcept;
UiRect emoji_cell(const TextHitRegion& region, bool flag) noexcept;
UiRect fit_in(const UiRect& tile, u32 width, u32 height, bool cover) noexcept;
// The quad's uv (v in the rows of the page it samples) and that page's binding (GpuPage::binding).
bool update_media_quad_source(D3D11Renderer* renderer, const MediaAtlasResult& allocation, u8 domain,
                              MediaQuad* quad, u8* binding, bool* changed) noexcept;
bool queue_media_quad(D3D11Renderer* renderer, const MediaAtlasResult& allocation, const UiRect& rectangle,
                      const UiRect& clip, bool rounded_mask = false, bool force_new_batch = false,
                      const MediaQuadStyle& style = kPlainMediaQuad) noexcept;
bool find_user_avatar(const D3D11Renderer& renderer, const UserRecord& user,
                      MediaAtlasResult* allocation) noexcept;
void reveal_media_underlay(D3D11Renderer& renderer, u32 media_before, const UiRect& rectangle,
                           const Color& fallback) noexcept;
bool queue_avatar(D3D11Renderer* renderer, const MediaAtlasResult& allocation, const UiRect& r, const UiRect& clip,
                  const Color& placeholder, bool first = false) noexcept;
bool queue_header_tag_icon(D3D11Renderer* renderer, const MessageScene& scene, const MessageTextCommand& command,
                           const UserAppearanceRecord& appearance, const UiBadgeWidget& tag) noexcept;
bool queue_chip_guild_icons(D3D11Renderer* renderer, bool search) noexcept;
bool paint_chip_initial_plates(D3D11Renderer* renderer, const Theme& theme, bool search) noexcept;
bool queue_profile_media(D3D11Renderer* renderer, const ProfileScene& profile, const Theme& theme,
                         u32* effect_first_batch) noexcept;
bool queue_settings_profile_overlays(D3D11Renderer* renderer, const Theme& theme, bool dialog) noexcept;
bool find_call_preview(const D3D11Renderer& renderer, const MediaAssetKey& key, MediaAtlasResult* allocation,
                       u8* domain) noexcept;
bool queue_call_media(D3D11Renderer* renderer, const Theme& theme) noexcept;
bool queue_toast(D3D11Renderer* renderer, const Theme& theme) noexcept;
bool queue_inline_flags(D3D11Renderer* renderer) noexcept;
// The magnifier: a layout step resolves its picture from the quad under the lens
// (the first that qualifies wins); its frame paints in the form layer, above
// the other paint, and `seal` opens that layer when nothing else has.
void resolve_zoom_lens(D3D11Renderer* renderer, const Theme& theme, const MediaAtlasResult& allocation,
                       const UiRect& picture, u8 domain) noexcept;
bool queue_zoom_lens_frame(D3D11Renderer* renderer, const Theme& theme, bool seal) noexcept;
bool split_paint_quad_boundary(PaintStream* paint, u32 quad, u32* batch_index) noexcept;
u64 shell_segment_key(const RendererPresentationStamp& stamp, const MessageComposerPresentationInput& composer,
                      const PaintStream& paint) noexcept;
void shell_segment_capture(D3D11Renderer* renderer, u64 key, const ShellPresentationRegions& regions) noexcept;
bool shell_segment_replay(D3D11Renderer* renderer, u64 key, float width_dip, float height_dip,
                          ShellPresentationRegions* regions) noexcept;
bool build_timeline_media(D3D11Renderer* renderer, const Theme& theme, const MessageScene* message_scene,
                          bool messages_visible, bool content_hidden) noexcept;
bool build_search_media(D3D11Renderer* renderer, const Theme& theme) noexcept;
bool build_settings_page(D3D11Renderer* renderer, const Theme& theme) noexcept;

} // namespace maicord::renderer_layout_detail
