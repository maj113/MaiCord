#pragma once
#include "maicord/media/asset_key.hpp"
#include "maicord/ui/hover_card.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/text_presentation.hpp"

namespace maicord {
struct UiTextMeasureCache;

// Server tag popout, opened from a server tag chip: a card with the server's
// banner (custom picture, else a gradient of its brand colour), icon, name, tag,
// online/member counts, founding month, description, traits and one action. Pure
// geometry and paint: the host fetches GET /guilds/{id}/profile, draws the
// banner, icon and badge into ui_guild_card_media and runs the action.
inline constexpr float kUiGuildCardWidth = 300, kUiGuildCardPadding = 16, kUiGuildCardGap = 8;
inline constexpr float kUiGuildCardBanner = 120, kUiGuildCardIcon = 64;
inline constexpr u32 kUiGuildCardNameBytes = 256, kUiGuildCardDescriptionBytes = 1024, kUiGuildCardTagBytes = 32;
inline constexpr u32 kUiGuildCardTraits = 5, kUiGuildCardTraitBytes = 96;
inline constexpr u32 kUiGuildCardEntranceMs = 220;

enum class UiGuildCardPhase : u8 {
    loading,         // GET /guilds/{id}/profile in flight: spinner.
    ready,           // The profile.
    private_profile, // 50001: "Private Server".
    failed,          // Anything else: "Unable to load server profile" (click retries).
};
// The card's single full-width button.
enum class UiGuildCardAction : u8 {
    none,
    go_to_server, // Already a member.
    join,         // Discoverable server, or a known invite.
    joining,      // Join sent; disabled until the server arrives.
};
enum class UiGuildCardControl : u8 { none, action, retry };
struct UiGuildCardContent {
    u64 guild_id; // Its snowflake dates "Est. Mon YYYY".
    UiGuildCardPhase phase;
    Utf8View name, description, tag; // Copied.
    Utf8View traits[kUiGuildCardTraits];
    u32 trait_count;
    u32 online_count, member_count;
    u32 brand_color; // 0xRRGGBB when has_brand_color
    bool has_brand_color;
    MediaAssetKey icon, badge; // Host keys (the badge leads the tag chip).
    MediaAssetKey banner;      // Custom banner picture, cover-fit over the colour.
    UiGuildCardAction action;
};
struct UiGuildCardScene : UiHoverCard {
    // Panel-relative geometry, offset by the entrance in paint and text.
    UiRect banner, icon_ring, icon, name, tag_chip, tag_badge, tag_text, online_dot, online, members_dot, members,
        established, description, traits[kUiGuildCardTraits], button, status_icon, status_title, status_text;
    char name_text[kUiGuildCardNameBytes], description_text[kUiGuildCardDescriptionBytes],
        tag[kUiGuildCardTagBytes], trait_text[kUiGuildCardTraits][kUiGuildCardTraitBytes];
    char online_text[32], members_text[32], established_text[32];
    u32 name_bytes, description_bytes, tag_bytes, trait_bytes[kUiGuildCardTraits], trait_count;
    u32 online_bytes, members_bytes, established_bytes;
    u32 brand_color;
    MediaAssetKey icon_key, badge_key, banner_key;
    UiGuildCardPhase phase;
    UiGuildCardAction action;
    bool has_brand_color;
};

// Shows (or re-projects) the card for identity (the guild) beside anchor
// inside viewport; measure sizes the description and traits when given.
bool ui_guild_card_show(UiGuildCardScene*, u64 identity, const UiGuildCardContent&, UiRect anchor, UiRect viewport,
                        u64 now_ms, bool reduced_motion, UiTextMeasureCache* measure) noexcept;
void ui_guild_card_close(UiGuildCardScene*) noexcept;
bool ui_guild_card_tick(UiGuildCardScene*, u64 now_ms, bool reduced_motion) noexcept;
u64 ui_guild_card_next_wake(const UiGuildCardScene&, u64 now_ms, bool reduced_motion) noexcept;
bool ui_guild_card_contains(const UiGuildCardScene&, float x, float y) noexcept;
UiGuildCardControl ui_guild_card_hit(const UiGuildCardScene&, float x, float y) noexcept;
bool ui_guild_card_set_hover(UiGuildCardScene*, float x, float y) noexcept;
bool ui_guild_card_set_pressed(UiGuildCardScene*, bool pressed) noexcept;
bool ui_guild_card_spinner_visible(const UiGuildCardScene&) noexcept;
// "1,234" with thousands separators; returns the bytes written.
u32 ui_guild_card_format_count(u32 value, char* output, u32 capacity) noexcept;
// Media the host draws: 0 the icon, 1 the tag badge, 2 the banner picture.
// The banner covers its rectangle (cover = true, centred), masked to the
// card's rounded top corners (mask, mask_radius).
inline constexpr u32 kUiGuildCardBannerMedia = 2;
struct UiGuildCardMedia {
    MediaAssetKey key;
    UiRect rectangle, clip;
    float radius, opacity;
    UiRect mask;
    float mask_radius;
    bool cover;
};
bool ui_guild_card_media(const UiGuildCardScene&, u32 index, UiGuildCardMedia* output) noexcept;
// The card and its banner colour; the banner picture (media 2) draws over
// them, then the foreground (icon ring and plate, chips, button) and the
// icon and badge media.
bool ui_guild_card_paint(const UiGuildCardScene&, const Theme&, PaintStream*) noexcept;
bool ui_guild_card_paint_foreground(const UiGuildCardScene&, const Theme&, PaintStream*) noexcept;
// Fallback banner: approximates radial-gradient(at 50% 127%, brighten(c, 1.75)
// 20.65%, c 85.16%) as a vertical ramp from c to the brightened colour.
Color ui_guild_card_banner_highlight(Color base) noexcept;
bool ui_guild_card_text_output(const UiGuildCardScene&, const Theme&, UiTextOutput*) noexcept;
} // namespace maicord
