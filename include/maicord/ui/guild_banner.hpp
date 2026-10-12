#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/media/asset.hpp"
#include "maicord/ui/geometry.hpp"

namespace maicord {
// Server banner CDN URLs and media identities (guild banner, invite splash,
// discovery splash), the channel-list header banner geometry that follows the
// list's scroll, and the rules that let an animated banner play.

// Media proxy sizes: the smallest listed size at or above pixels, else the
// largest (4096).
u32 ui_media_proxy_size(u32 pixels) noexcept;

inline constexpr u32 kUiGuildImageUrlBytes = 192;
// The longest side the CDN JPEG and WebP decoders accept (the media atlas's
// 512 px images). Larger renditions (e.g. a 1024x576 splash at 150%) cannot
// decode, so every request below asks for at most this many pixels and the
// renderer scales it up.
inline constexpr u32 kUiGuildImageMaximumPixels = 512;
struct UiGuildImageMedia {
    MediaAssetKey key; // profile_image identity of the exact URL
    u32 url_bytes;
    char url[kUiGuildImageUrlBytes];
    Utf8View url_view() const noexcept { return {url, url_bytes}; }
};
// Guild banner URL:
//   https://cdn.discordapp.com/banners/{guild}/{hash}.webp?size=ui_media_proxy_size(min(360*dpr, 512))
// An a_ hash adds &animated=true while animating; gif selects the GIF
// representation (a fallback for a failed animated WebP). A static banner shows
// the hash's first frame.
bool ui_guild_banner_media(u64 guild_id, Utf8View hash, u32 dpi, bool animate, bool gif,
                           UiGuildImageMedia* output) noexcept;
// Invite and discovery splash URLs: JPEG at ui_media_proxy_size(min(size_dips*dpr, 512)):
//   https://cdn.discordapp.com/splashes|discovery-splashes/{guild}/{hash}.jpg?size=N
// The CDN scales the splash to N px wide (a 16:9 splash: N x 9N/16).
bool ui_guild_splash_media(u64 guild_id, Utf8View hash, bool discovery, u32 size_dips, u32 dpi,
                           UiGuildImageMedia* output) noexcept;
// The server tag card's banner: a DISCOVERABLE profile's custom banner, from
// the discovery-splash path at 512 DIPs (size 512 * dpr). False (the card's
// default gradient) for any other profile or a malformed hash.
inline constexpr u32 kUiGuildCardBannerRequestDips = 512;
bool ui_guild_card_banner_media(u64 guild_id, Utf8View custom_banner, bool discoverable, u32 dpi,
                                UiGuildImageMedia* output) noexcept;

// Channel-list header banner height and scroll pinning: the banner fades and
// slides up over 88 DIP of scroll; the channel list gains an 84 DIP top inset.
inline constexpr float kUiGuildBannerHeight = 130;
inline constexpr float kUiGuildBannerListInset = 84;
inline constexpr float kUiGuildBannerScrollRange = 88;
inline constexpr float kUiGuildBannerSlide = 90;
// Header glass: backdrop-filter blur(30px).
inline constexpr float kUiGuildBannerGlassBlur = 30;
// An animated banner plays this long each time it comes into view (GIF autoplay).
inline constexpr u64 kUiGuildBannerAutoplayMs = 5'000;

struct UiGuildBannerGeometry {
    float value;     // controller value: 1 at rest, 0 once scrolled 88 DIP
    bool visible;    // banner shown (scroll < 88): white header text over the banner
    UiRect image;    // the cover-fit picture area, fixed at the sidebar top
    UiRect clip;     // its container, sliding up with the scroll
    UiRect header;   // the header bar the glass and scrim cover
    UiRect hover;    // hover layer: the banner strip under the header
    float opacity;   // container opacity (value)
    float glass;     // header glass opacity: (1 - value) * 6, 1 while the header is hovered
    float scrim;     // header scrim opacity (value)
};
// sidebar: the channel sidebar; header: its header bar; scroll: the channel
// list's scroll offset in DIPs.
UiGuildBannerGeometry ui_guild_banner_geometry(UiRect sidebar, UiRect header, float scroll,
                                               bool header_hovered) noexcept;

// The host's channel-header banner for the renderer: the picture to draw
// (the animated one while it plays) and the still drawn until it is resident.
struct UiGuildBannerView {
    u64 guild_id;
    MediaAssetKey key, fallback;
};

// Header banner animation: an a_ banner animates only on a guild
// with ANIMATED_BANNER. It plays for 5 s each time it becomes visible when
// GIFs autoplay, and while the strip below the header is hovered (entering
// it ends the timed play; leaving it stops). Reduced motion keeps it still.
struct UiGuildBannerAnimation {
    u64 guild_id;
    u64 autoplay_until_ms;
    bool was_visible;
    bool hovered;
};
bool ui_guild_banner_animated_hash(Utf8View hash) noexcept;
// Updates the state for this frame and returns whether the banner animates.
bool ui_guild_banner_animate(UiGuildBannerAnimation* state, u64 guild_id, bool eligible, bool visible, bool hovered,
                             bool gif_autoplay, u64 now_ms) noexcept;
// When the timed play ends (0 when nothing is pending).
u64 ui_guild_banner_next_wake(const UiGuildBannerAnimation& state, u64 now_ms) noexcept;
} // namespace maicord
