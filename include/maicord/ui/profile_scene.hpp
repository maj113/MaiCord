#include "maicord/model/user_activity.hpp"
#include "maicord/model/image_crop.hpp"
#include "maicord/media/asset.hpp"
#include "maicord/ui/scroll_host.hpp"
#include "maicord/ui/message_markdown.hpp"
#pragma once
#include "maicord/ui/surface_geometry.hpp"
#include "maicord/ui/edit_field.hpp"
#include "maicord/ui/badge.hpp"
#include "maicord/model/user_profile.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/ui/button_keyboard.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/ui/presence.hpp"
namespace maicord {
// The official client's user profile surfaces: the click popout (compact
// card), the full profile modal (profile card and tab panel) and the profile
// editor's preview. Geometry follows the web client in CSS px (DIPs).
enum class ProfileAction : u16 {
    none,
    close,
    // Opens the full profile (avatar, name, username, View Full Profile).
    expand,
    // The footer message field (popout) or the Message button (modal).
    message,
    // Edit Profile: the main editor, or the per-server menu in a server.
    edit,
    retry,
    // Modal tabs and the popout's mutuals sections.
    activity,
    friends,
    servers,
    open_friend,
    open_server,
    remove_role,
    // The "+" role button: the host opens the role picker.
    choose_roles,
    // A plugin profile-section node; the part's target is its node index.
    plugin_slot,
    // The modal's note field: starts editing it.
    note,
    // The popout's hover note button: the full profile at its Note field.
    open_note,
    add_friend,
    // Friend / pending header button: the host opens Friend Actions.
    relationship,
    // "More": the host opens the user menu.
    more,
    // "View Full Bio": the full profile.
    view_bio,
    // "+N" expands the roles; the chevron collapses them.
    roles_toggle,
    // The popout's activity card: the full profile on Activity.
    card,
    // Accept on the incoming friend request card.
    accept_friend,
    // The server tag chip: the host opens the server tag popout (target: guild).
    guild_tag
};
enum class ProfilePhase : u8 {
    loading,
    ready,
    failed
};
// Where the card is presented: the click popout, the full profile modal, or
// the profile editor's live preview (a non-interactive popout-sized card at
// the anchor's top-left, without footer or close; it reports its height).
enum class ProfilePresentation : u8 {
    popout,
    modal,
    preview
};
// Which side of its anchor a popout opens on.
enum class ProfilePlacement : u8 {
    right,
    left,
    above
};
// A picked image shown before upload (profile editor preview): the local
// preview-atlas key and the crop the user chose (image_crop.hpp).
struct ProfilePendingImage {
    MediaAssetKey key;
    ImageCropState crop;
    bool active;
};
enum class ProfileMediaKind : u8 {
    none,
    banner,
    avatar,
    decoration,
    badge,
    guild_icon,
    peer_avatar,
    peer_guild,
    status_emoji,
    // Not media: a plugin profile-section chip; source holds its icon, style its tone.
    plugin_slot,
    // Rich presence card (source: activity index): asset images and, not
    // media, the Spotify progress track.
    activity_large,
    activity_small,
    activity_progress,
    // Not media: secondary card text, and the right-aligned end clock.
    activity_text,
    activity_clock,
    // The current server's icon beside its Member Since date.
    member_guild
};
// How a part paints (its widget kind stays text, button or container).
enum class ProfileLook : u8 {
    plain,
    // 32 DIP circle over the banner (header buttons, the modal's close).
    header_button,
    // A bare icon control (note button, role remove, connection mark).
    icon,
    // Primary and secondary buttons; icon_only secondary buttons are square.
    primary,
    secondary,
    // Activity and notice overlays; peek is the stacked card behind them.
    card,
    peek,
    // Mutual friend/server rows: hover background.
    row,
    // Role pill (color: role colour) and the +N / add-role pills.
    role,
    pill,
    tab,
    // 4 DIP separator dot.
    dot,
    // The popout footer's message field background.
    input,
    // Custom status thought bubble.
    bubble,
    // Connection platform box.
    box,
    // The circle behind a profile badge, and a 1 DIP divider.
    badge,
    rule
};
// Text colour roles (resolved per profile theme in paint/text).
enum class ProfileTone : u8 {
    normal,
    strong,
    subtle,
    muted,
    positive,
    link,
    on_accent,
    custom
};
struct ProfilePart {
    UiBadgeWidget badge;
    u64 target;
    UiWidgetHandle widget;
    Utf8View text;
    Color color;
    ProfileAction action;
    ProfileMediaKind media;
    ProfileLook look;
    ProfileTone tone;
    u16 source;
    // A UiIcon for icon looks and leading button icons; kProfileNoIcon none.
    u16 icon;
    u32 style;
    bool wrap, centered;
};
inline constexpr u16 kProfileNoIcon = 0xFFFF;
struct ProfileTextHit {
    UiWidgetHandle owner;
    UiRect layout, ink;
};
struct ProfileScene {
    // Only plain inline action labels use ink hits; cards retain their full targets.
    mutable ProfileTextHit text_hits[16];
    mutable u32 text_hit_count;
    UiWidgetManager widgets;
    Arena storage;
    ProfilePart* parts;
    u32 part_count;
    UiWidgetHandle root, panel, body, message_field, bio_widget, name_widget;
    ProfileString<kProfileBioMaximumUtf8Bytes> bio_display;
    // Every bio byte carries the text-sm size (gaps between styles included).
    TextStyleSpan bio_styles[260];
    MessageMarkdownSemantic bio_semantics[64];
    u32 bio_style_count, bio_semantic_count, bio_revealed[64], bio_revealed_count, bio_pressed;
    u64 bio_signature;
    UiEditView edit;
    UiRect edit_rectangle, edit_content;
    ProfileString<192> message_placeholder;
    bool message_failed;
    UiButtonKeyboard keyboard;
    UiTextMeasureCache* measure;
    const ProfileEffect* effect;
    u64 effect_started_ms, name_started_ms;
    bool reduced_motion, banner_compatibility;
    UiPresence presence;
    // Online on mobile only: the avatar shows the phone status.
    bool presence_mobile;
    bool focus_visible;
    const UserProfile* profile;
    const ChatStateStore* state;
    const UserStore* users;
    UiRect viewport, anchor, rectangle, scroll_viewport;
    // Laid-out header geometry (absolute DIPs): banner, avatar, its
    // decoration and status dot, the card holding them (the modal's profile
    // card) and the profile effect's area.
    UiRect banner, avatar, decoration, status, card, effect_area, secondary_viewport, media_viewport;
    float avatar_stroke;
    float secondary_scroll, secondary_height;
    u64 self_id;
    u64 opens;
    u32 revision, users_revision, permission_revision;
    float scroll, content_height;
    ProfilePhase phase;
    u16 http_status;
    u8 tab;
    // Host facts: the relationship (0 none, 1 friend, 2 incoming, 3 outgoing,
    // 4 blocked) and where the popout opens.
    u8 relation;
    ProfilePlacement placement;
    bool active, roles_busy, roles_expanded, can_manage_roles;
    // Pointer over the card: the note button shows and the effect dims.
    bool hovered;
    u64 hover_changed_ms;
    // Popout entrance: translated 10 DIPs toward the anchor, settling over
    // 200 ms (opened_ms is the open time; the host re-lays out meanwhile).
    u64 opened_ms;
    float intro_offset;
    // Theme resolved at layout (Nitro theme colours, its dark/light choice).
    bool themed, theme_dark;
    u32 theme_primary, theme_secondary, banner_color;
    bool has_banner_color;
    // Set before profile_scene_open. Preview cards draw pending images in
    // place of the saved avatar/banner and publish their laid-out height.
    ProfilePresentation presentation;
    ProfilePendingImage pending_avatar, pending_banner;
    float preview_height;
    // Activity cards with timestamps re-layout once a second (elapsed time,
    // Spotify progress); activity_clock_s is the Unix second laid out.
    bool activity_timed;
    u64 activity_clock_s;
    // Note about another user. The host owns the text (borrowed until the next
    // layout); while note_editing the edit view belongs to the note field.
    Utf8View note_text;
    bool note_visible, note_loading, note_editing;
    UiWidgetHandle note_field;
    // The full profile opened at its Note: the next layout scrolls the card
    // body to note_offset (the note section's top in body content).
    bool scroll_to_note;
    float note_offset;
    // The modal's corners are rounded only when it does not fill the window.
    bool modal_rounded;
};
bool profile_scene_open(ProfileScene* scene, const UserProfile* profile, const ChatStateStore* state,
                        const UserStore* users, u64 self, UiRect anchor, UiRect viewport) noexcept;
bool profile_scene_layout(ProfileScene* scene, UiRect viewport, bool force = false) noexcept;
UiScrollBinding profile_scene_scroll_binding(ProfileScene*, bool secondary = false) noexcept;
// Absolute-position adapter; wheel and key input must use the shared scroll host.
bool profile_scene_scroll(ProfileScene* scene, float delta, bool secondary = false) noexcept;
void profile_scene_release(ProfileScene* scene) noexcept;
bool profile_scene_paint(const ProfileScene& scene, const Theme& theme, PaintStream* paint) noexcept;
// The rounded region the card's media (banner, profile effect) must stay
// inside, in the radius the card itself is painted with: the popout (inside a
// Nitro theme's 2 DIP border) or the modal's profile card, whose top corners
// alone round because it runs on past the modal's bottom edge.
struct ProfileCardMask {
    UiRect rectangle;
    float radius;
};
ProfileCardMask profile_scene_card_mask(const ProfileScene& scene, const Theme& theme) noexcept;
bool profile_scene_foreground(const ProfileScene& scene, const Theme& theme, PaintStream* paint,
                              const UiEditGeometry* edit = nullptr, u32 dpi = 96) noexcept;
bool profile_scene_text(const ProfileScene& scene, const Theme& theme, UiTextCommandStream* text,
                        const UiEditGeometry* edit = nullptr) noexcept;
ProfileAction profile_scene_action(const ProfileScene& scene, UiWidgetHandle widget) noexcept;
// The part behind a widget, or null.
const ProfilePart* profile_scene_part(const ProfileScene& scene, UiWidgetHandle widget) noexcept;
UiWidgetHandle profile_scene_hit_test(const ProfileScene&, float x, float y) noexcept;
void profile_scene_publish_text_hit(const ProfileScene&, UiWidgetReference, UiRect layout,
                                     UiRect ink, UiRect clip) noexcept;
// Popout entrance offset at now (0 when settled or under reduced motion).
float profile_scene_intro_offset(const ProfileScene& scene, u64 now_ms) noexcept;
// Effect opacity at now: 1, fading to .35 over 600 ms while hovered.
float profile_scene_effect_opacity(const ProfileScene& scene, u64 now_ms) noexcept;
inline bool profile_scene_animating(const ProfileScene* scene, u64 now) noexcept {
    if (!scene || !scene->active || !scene->effect || !scene->effect->sku)
        return false;
    if (!scene->reduced_motion && scene->hover_changed_ms && now < scene->hover_changed_ms + 600)
        return true;
    if (scene->reduced_motion)
        return false;
    for (u32 i = 0; i < scene->effect->count; ++i) {
        const auto& layer = scene->effect->layers[i];
        if (layer.loop || now < scene->effect_started_ms + layer.start_ms + layer.duration_ms)
            return true;
    }
    return false;
}
} // namespace maicord

namespace maicord {
bool profile_scene_text_output(const ProfileScene& s, const Theme& theme, UiTextOutput* text,
                        const UiEditGeometry* edit) noexcept;
} // namespace maicord
