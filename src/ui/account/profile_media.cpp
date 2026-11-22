#include "maicord/ui/widgets/badge.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/ui/account/profile_media.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/account/profile_appearance.hpp"
#include "maicord/model/user/store.hpp"
#include "maicord/model/chat/chat_state.hpp"
#include "maicord/model/guild/member_store.hpp"
#include "maicord/model/guild/member_list.hpp"
#include "maicord/ui/guild/member_list_scene.hpp"
#include "maicord/ui/widgets/activity_status.hpp"
#include "maicord/ui/guild/banner.hpp"
#include "maicord/ui/media/avatar_media.hpp"
#include "maicord/ui/chat/game_media.hpp"
#include "maicord/model/user/profile_connection.hpp"
#include "maicord/model/chat/game_store.hpp"

#include "maicord/media/core/asset.hpp"
namespace maicord {
bool profile_media_use_compatibility_banner(ProfileScene* scene) noexcept {
    if (scene->banner_compatibility) return false;
    scene->banner_compatibility = true;
    return true;
}
namespace {
// Sets every field of a result but the unused bytes of its URL, which a reader never reads past its length: about
// 90 bytes written, not 480. One body for the six resolvers.
__declspec(noinline) ProfileMedia& start(ProfileMedia* out) noexcept {
    clear_bytes(out, __builtin_offsetof(ProfileMedia, url));
    out->url.bytes = 0;
    clear_bytes(&out->rectangle, sizeof(ProfileMedia) - __builtin_offsetof(ProfileMedia, rectangle));
    return *out;
}
bool append(ProfileString<384>* s, const char* text, u32 bytes) noexcept {
    if (bytes > 384u - s->bytes)
        return false;
    copy_bytes(s->data + s->bytes, text, bytes);
    s->bytes += static_cast<u16>(bytes);
    return true;
}
bool append(ProfileString<384>* s, const Utf8View& text) noexcept {
    return append(s, text.data, text.bytes);
}
// A literal goes in as pointer and length: no Utf8View temporary on the caller's stack.
template <u32 N> bool append(ProfileString<384>* s, const char (&text)[N]) noexcept {
    return append(s, text, N - 1);
}
bool id(ProfileString<384>* s, u64 value) noexcept {
    const u32 n = decimal_digits(value);
    if (n > 384u - s->bytes)
        return false;
    s->bytes += static_cast<u16>(decimal_write(s->data + s->bytes, value));
    return true;
}
// A path segment named by an id: "<prefix><id>/".
void segment(ProfileString<384>* url, const char* prefix, u32 prefix_bytes, u64 value) noexcept {
    append(url, prefix, prefix_bytes);
    id(url, value);
    append(url, "/");
}
void segment(ProfileString<384>* url, const Utf8View& prefix, u64 value) noexcept {
    segment(url, prefix.data, prefix.bytes, value);
}
template <u32 N> void segment(ProfileString<384>* url, const char (&prefix)[N], u64 value) noexcept {
    segment(url, prefix, N - 1, value);
}
// The CDN's default (embed) avatar after the CDN's root: "embed/avatars/<index>.png".
void default_avatar_path(ProfileString<384>* url, u32 index) noexcept {
    append(url, "embed/avatars/");
    id(url, index);
    append(url, ".png");
}
// A member's per-server image directory after the CDN's root: "guilds/<guild>/users/<user>/".
void member_path(ProfileString<384>* url, u64 guild, u64 user) noexcept {
    segment(url, "guilds/", guild);
    segment(url, "users/", user);
}
// The CDN's root; every URL below continues from it.
void cdn_root(ProfileString<384>* url) noexcept {
    append(url, "https://cdn.discordapp.com/");
}
// "media/v1/collectibles-shop/<sku>/animated" (or "/static") after the CDN's root; false when it does not fit.
bool nameplate_path(ProfileString<384>* url, u64 sku, bool animated) noexcept {
    return append(url, "media/v1/collectibles-shop/") && id(url, sku) &&
           (animated ? append(url, "/animated") : append(url, "/static"));
}
// "clan-badges/<guild>/<badge>.png?size=32" after the CDN's root.
void badge_path(ProfileString<384>* url, u64 guild, const Utf8View& badge) noexcept {
    segment(url, "clan-badges/", guild);
    append(url, badge);
    append(url, ".png?size=32");
}
void passthrough(ProfileString<384>* url, bool on) noexcept {
    if (on)
        append(url, "&passthrough=true");
    else
        append(url, "&passthrough=false");
}
bool asset(const Utf8View& value) noexcept {
    if (!value.bytes)
        return false;
    for (u32 i = 0; i < value.bytes; ++i) {
        const char c = value.data[i];
        if (!(ascii_is_alnum(c) || c == '_'))
            return false;
    }
    return true;
}
// The smallest CDN `size` covering `dips` at `dpi` (ui_media_proxy_size), so an image takes the pixels it is
// drawn at. `limit`, itself a proxy size, caps it at the largest the image's decoder and atlas page take.
u32 cdn_display_size(float dips, u32 dpi, u32 limit = 4096) noexcept {
    const u32 pixels = static_cast<u32>(dips * static_cast<float>(dpi ? dpi : 96u) / 96.f + .999f);
    return ui_media_proxy_size(minimum(pixels, limit));
}
// Animated WebP frames decode up to 512 px (kWebpDimensionMaximum). An identity atlas entry keeps 1 px of padding
// on each side, so a page takes 2046 columns and 1022 rows: a square image fits up to 640 and a 5:2 banner up to
// 1536 (2048 would be 2 px too wide).
inline constexpr u32 kCdnAnimatedWebpSizeLimit = 512, kCdnSquareSizeLimit = 640, kCdnBannerSizeLimit = 1536;
// The key of an image fetched from `url`; an effect layer's is marked so its decode fits the effect slot.
MediaAssetKey profile_key(const ProfileString<384>& url, MediaAttachmentFormat format, bool animated,
                          bool effect_layer = false) noexcept {
    auto key = media_asset_embed(hash64_words(kHash64Seed, url.data, url.bytes), 1,
                                 effect_layer ? kMediaProfileEffectLayerIndex : 0, MediaEmbedResource::image, format,
                                 animated, MediaResolutionTier::px512);
    key.kind = MediaAssetKind::profile_image;
    return key;
}
// A picked image placed so its crop mask covers target (profile editor
// preview): the whole turned image at the target's scale.
bool pending_media(const ProfilePendingImage& image, const UiRect& target, const UiRect& mask, float radius, const UiRect& clip,
                   ProfileMedia* out) noexcept {
    const auto crop = image_crop_mask_rect(image.crop);
    const float mask_width = crop.right - crop.left;
    if (!image.active || !media_asset_key_valid(image.key) || !(mask_width > 0))
        return false;
    const auto whole = image_crop_image_rect(image.crop);
    const float k = ui_rect_width(target) / mask_width;
    ProfileMedia result{};
    result.domain = 1;
    result.key = image.key;
    result.rotation = image.crop.rotation;
    result.rectangle = {target.left + (whole.left - crop.left) * k, target.top + (whole.top - crop.top) * k,
                        target.left + (whole.right - crop.left) * k, target.top + (whole.bottom - crop.top) * k};
    result.mask = mask;
    result.mask_radius = radius;
    result.clip = clip;
    copy_value(out, result);
    return true;
}
} // namespace
namespace {
bool guild_badge_media(bool enabled, u64 guild_id, const Utf8View& badge, const UiRect& rectangle, const UiRect& clip,
                       ProfileMedia* out) noexcept {
    if (!enabled || !guild_id || !asset(badge))
        return false;
    ProfileMedia& result = start(out);
    cdn_root(&result.url);
    badge_path(&result.url, guild_id, badge);
    result.key = profile_key(result.url, MediaAttachmentFormat::png, false);
    copy_value(&result.rectangle, rectangle);
    result.clip = clip;
    return true;
}
} // namespace
bool user_guild_badge_media(const UserAppearance& appearance, const UiRect& rectangle, const UiRect& clip,
                            ProfileMedia* out) noexcept {
    return guild_badge_media(appearance.primary_enabled, appearance.primary_guild_id,
                             appearance.primary_badge.view(), rectangle, clip, out);
}
bool user_guild_badge_media(const UserAppearanceRecord& appearance, const UiRect& rectangle, const UiRect& clip,
                            ProfileMedia* out) noexcept {
    return guild_badge_media(appearance.primary_enabled, appearance.primary_guild_id,
                             appearance.primary_badge.view(), rectangle, clip, out);
}
bool user_nameplate_media(const UserAppearanceRecord& appearance, bool animated, const UiRect& rectangle,
                          const UiRect& clip, ProfileMedia* out) noexcept {
    if (!appearance.nameplate_sku)
        return false;
    ProfileMedia& result = start(out);
    cdn_root(&result.url);
    if (!nameplate_path(&result.url, appearance.nameplate_sku, animated))
        return false;
    result.key = profile_key(result.url, MediaAttachmentFormat::png, animated);
    result.rectangle = rectangle;
    result.clip = clip;
    return media_asset_key_valid(result.key);
}
bool profile_media_resolve(const ProfileScene& scene, u32 index, ProfileMedia* out) noexcept {
    // Most parts (text, buttons) have no media: the renderer and the media scans ask for every part each frame.
    if (!scene.active || !scene.profile || index >= scene.part_count ||
        scene.parts[index].media == ProfileMediaKind::none)
        return false;
    const auto& part = scene.parts[index];
    const auto& p = *scene.profile;
    const auto* node = ui_widget_manager_layout_node(scene.widgets, part.widget);
    if (!node)
        return false;
    // The media is built in *out itself: a caller reads it only on success (profile_media.hpp).
    ProfileMedia& result = start(out);
    result.rectangle = node->rectangle;
    result.clip = node->clip_rectangle;
    if (!ui_rect_intersects(result.rectangle, result.clip))
        return false;
    if (part.media == ProfileMediaKind::status_emoji) {
        const auto* a =
            scene.users ? user_appearance_find(scene.users->appearance, p.user_id, p.guild_id) : nullptr;
        if (!a || !(a->fields & appearance_activity))
            a = scene.users ? user_appearance_find(scene.users->appearance, p.user_id) : nullptr;
        if (!a || !a->status_emoji_id)
            return false;
        result.key = media_asset_custom_emoji(a->status_emoji_id, a->emoji_animated);
        return true;
    }
    if (part.media == ProfileMediaKind::nameplate) {
        // The editor preview's drafted nameplate (the server's, else the user's), its still as a member row at rest
        // draws it, under the same URL.
        const auto& server = p.guild_appearance;
        const auto& a = p.guild_id && (server.fields & appearance_nameplate) ? server : p.user_appearance;
        cdn_root(&result.url);
        if (!(a.fields & appearance_nameplate) || !a.nameplate_sku || !nameplate_path(&result.url, a.nameplate_sku, false))
            return false;
        result.mask = result.rectangle;
        result.mask_radius = 4;
        result.key = profile_key(result.url, MediaAttachmentFormat::png, false);
        return media_asset_key_valid(result.key);
    }
    if (part.media == ProfileMediaKind::avatar && scene.pending_avatar.active)
        return pending_media(scene.pending_avatar, result.rectangle, result.rectangle,
                             ui_rect_width(result.rectangle) * .5f, result.clip, out);
    if (part.media == ProfileMediaKind::banner && scene.pending_banner.active) {
        // Clipped to the banner; the renderer rounds it with the card's mask.
        const auto r = result.rectangle;
        return pending_media(scene.pending_banner, r, {}, 0, ui_rect_intersection(result.clip, r), out);
    }
    if (part.media == ProfileMediaKind::activity_large || part.media == ProfileMediaKind::activity_small) {
        // The laid-out copy, which the card's text came from.
        if (!scene.activity_set || part.source >= scene.activity_set->count)
            return false;
        const auto& activity = scene.activity_set->items[part.source];
        const bool corner = part.media == ProfileMediaKind::activity_small;
        const auto art = corner ? UserActivityArt::small_image : static_cast<UserActivityArt>(part.target);
        char url[384];
        auto format = MediaAttachmentFormat::png;
        u32 bytes = 0;
        if (art == UserActivityArt::application) {
            for (u32 i = 0; i < scene.applications.count; ++i)
                if (scene.applications[i].id == activity.application_id)
                    bytes = user_activity_application_icon_url(activity.application_id,
                                                               scene.applications[i].icon.view(), {url, sizeof(url)});
        } else if (art == UserActivityArt::large || art == UserActivityArt::small_image)
            bytes = user_activity_image_url(activity, art == UserActivityArt::large, {url, sizeof(url)}, &format);
        if (!bytes || !append(&result.url, {url, bytes}))
            return false;
        // Masked to their own shape: the full profile's cards lie outside the
        // profile card that unmasked media is rounded to. The artwork stays
        // concentric with its card (profile_scene_paint sets the radius).
        result.circular = corner;
        result.mask = result.rectangle;
        result.mask_radius = corner ? ui_rect_width(result.rectangle) * .5f : scene.activity_art_radius;
        result.key = profile_key(result.url, format, false);
        return media_asset_key_valid(result.key);
    }
    if (part.media == ProfileMediaKind::widget_game || part.media == ProfileMediaKind::collection_game) {
        // A game's cover (its icon without one), rounded like activity artwork; or its icon on the 24 DIP
        // collection plate, at half opacity under the row's "+N".
        const bool cover = part.media == ProfileMediaKind::widget_game;
        const auto* record = scene.games ? game_store_find(*scene.games, part.target) : nullptr;
        GameImageMedia image;
        const u32 size = static_cast<u32>(ui_rect_width(result.rectangle) + .5f);
        const u32 dpi = scene.media_dpi ? scene.media_dpi : 96;
        if (!record || record->state != GameState::known ||
            !((cover && game_cover_media(record->game, size, dpi, &image)) ||
              game_icon_media(record->game, size, dpi, &image)) ||
            !append(&result.url, image.url_view()))
            return false;
        result.key = image.key;
        result.mask = result.rectangle;
        result.mask_radius = cover ? scene.activity_art_radius : scene.game_icon_radius;
        if (!cover)
            result.opacity = part.style ? .5f : 0.f;
        return media_asset_key_valid(result.key);
    }
    if (part.media == ProfileMediaKind::connection_logo || part.media == ProfileMediaKind::connection_chip) {
        // The service's logo or a game's picture, from the web client's asset host, drawn inside its tile; no
        // file (an unlisted type) draws the bare tile.
        const bool logo = part.media == ProfileMediaKind::connection_logo;
        Utf8View file{};
        if (!logo)
            file = profile_connection_chip_icon(static_cast<ProfileConnectionChipIcon>(part.source));
        else if (part.source < p.connection_count)
            file = profile_connection_icon(profile_connection_kind(p.connections[part.source].type.view()));
        if (!file.bytes || !append(&result.url, "https://discord.com/assets/") ||
            !append(&result.url, file) || !append(&result.url, ".png"))
            return false;
        if (logo)
            result.rectangle = ui_rect_inset(result.rectangle, {4, 4, 4, 4});
        result.key = profile_key(result.url, MediaAttachmentFormat::png, false);
        return media_asset_key_valid(result.key);
    }
    auto& url = result.url;
    cdn_root(&url);
    Utf8View hash{};
    bool animated = false;
    auto format = MediaAttachmentFormat::webp;
    const auto display = profile_display(p);
    const bool main_avatar = part.media == ProfileMediaKind::peer_avatar && part.source == 0xFFFF;
    if (part.media == ProfileMediaKind::avatar && !display.avatar.bytes) {
        // Users without an avatar show the CDN's default (embed) avatar under the key every list shares.
        const u32 index = default_avatar_index(p.user_id);
        default_avatar_path(&url, index);
        result.circular = true;
        result.key = default_avatar_key(index);
        return media_asset_key_valid(result.key);
    }
    // A voice card member: their avatar, or the default one, masked to its circle.
    const auto* voice_user = part.media == ProfileMediaKind::voice_avatar && scene.users
                                 ? user_store_find(*scene.users, part.target)
                                 : nullptr;
    if (part.media == ProfileMediaKind::voice_avatar) {
        hash = voice_user ? user_store_avatar_hash(*voice_user) : Utf8View{};
        result.circular = true;
        result.mask = result.rectangle;
        result.mask_radius = ui_rect_width(result.rectangle) * .5f;
        if (!asset(hash)) {
            const u32 index = default_avatar_index(part.target);
            default_avatar_path(&url, index);
            result.key = default_avatar_key(index);
            return media_asset_key_valid(result.key);
        }
    }
    if (part.media == ProfileMediaKind::member_guild) {
        const auto* guild = scene.state ? chat_state_store_find_guild(*scene.state, p.guild_id) : nullptr;
        hash = guild ? chat_state_store_guild_icon_hash(*guild) : Utf8View{};
        if (!asset(hash))
            return false;
        segment(&url, "icons/", p.guild_id);
        animated = ui_guild_banner_animated_hash(hash);
    } else if (main_avatar) {
        hash = p.avatar.view();
        if (!asset(hash))
            return false;
        segment(&url, "avatars/", p.user_id);
        result.circular = true;
        animated = ui_guild_banner_animated_hash(hash);
    } else if (part.media == ProfileMediaKind::voice_avatar) {
        segment(&url, "avatars/", part.target);
        animated = ui_guild_banner_animated_hash(hash);
    } else if (part.media == ProfileMediaKind::peer_avatar || part.media == ProfileMediaKind::peer_guild) {
        const bool user = part.media == ProfileMediaKind::peer_avatar;
        if (part.source >= (user ? p.mutual_friends_count : p.mutual_guild_count))
            return false;
        const auto& peer = user ? p.mutual_friends[part.source] : p.mutual_guilds[part.source];
        hash = peer.icon.view();
        if (!user && scene.state) {
            const auto* guild = chat_state_store_find_guild(*scene.state, peer.id);
            if (guild)
                hash = chat_state_store_guild_icon_hash(*guild);
        }
        if (!asset(hash))
            return false;
        segment(&url, user ? utf8_literal("avatars/") : utf8_literal("icons/"), peer.id);
        // Masked to their own circle: the full profile lists them outside the
        // profile card that unmasked media is rounded to.
        result.circular = true;
        result.mask = result.rectangle;
        result.mask_radius = ui_rect_width(result.rectangle) * .5f;
        animated = ui_guild_banner_animated_hash(hash);
    } else if (part.media == ProfileMediaKind::avatar || part.media == ProfileMediaKind::banner) {
        const bool avatar = part.media == ProfileMediaKind::avatar;
        // Per-server avatar and banner fall back to the user's.
        const bool guild = avatar ? display.guild_avatar : display.guild_banner;
        hash = avatar ? display.avatar : display.banner;
        if (!asset(hash))
            return false;
        if (guild) {
            member_path(&url, p.guild_id, p.user_id);
            append(&url, avatar ? utf8_literal("avatars/") : utf8_literal("banners/"));
        } else
            segment(&url, avatar ? utf8_literal("avatars/") : utf8_literal("banners/"), p.user_id);
        animated = ui_guild_banner_animated_hash(hash);
        result.circular = avatar;
        if (!avatar)
            format = animated ? (scene.banner_compatibility ? MediaAttachmentFormat::gif
                                                            : MediaAttachmentFormat::webp)
                              : MediaAttachmentFormat::png;
    } else if (part.media == ProfileMediaKind::guild_icon) {
        const auto& a = p.user_appearance;
        if (!a.primary_enabled || !a.primary_guild_id || !asset(a.primary_badge.view()))
            return false;
        hash = a.primary_badge.view();
        segment(&url, "clan-badges/", a.primary_guild_id);
        format = MediaAttachmentFormat::png;
    } else if (part.media == ProfileMediaKind::decoration) {
        hash = display.decoration;
        if (!asset(hash))
            return false;
        append(&url, "avatar-decoration-presets/");
        format = MediaAttachmentFormat::png;
        animated = ui_guild_banner_animated_hash(hash);
    } else if (part.media == ProfileMediaKind::badge && part.source < p.badge_count) {
        hash = p.badges[part.source].icon.view();
        if (!asset(hash))
            return false;
        append(&url, "badge-icons/");
        format = MediaAttachmentFormat::png;
    } else
        return false;
    append(&url, hash);
    append(&url, format == MediaAttachmentFormat::png   ? ".png"
                 : format == MediaAttachmentFormat::gif ? ".gif"
                                                        : ".webp",
           format == MediaAttachmentFormat::png || format == MediaAttachmentFormat::gif ? 4u : 5u);
    // Every part is fetched at the smallest CDN size covering the width it is drawn at on this display
    // (a DPI change is a new key), within what its decoder and atlas page take.
    const bool animated_webp = animated && format == MediaAttachmentFormat::webp;
    const u32 limit = animated_webp                              ? kCdnAnimatedWebpSizeLimit
                      : part.media == ProfileMediaKind::banner ? kCdnBannerSizeLimit
                                                                 : kCdnSquareSizeLimit;
    append(&url, "?size=");
    id(&url, cdn_display_size(ui_rect_width(result.rectangle), scene.media_dpi, limit));
    if (animated_webp)
        append(&url, "&animated=true");
    if (part.media == ProfileMediaKind::decoration)
        passthrough(&url, animated);
    result.key = profile_key(url, format, animated);
    // Clipped to the banner; the renderer rounds it with the card's mask
    // (profile_scene_card_mask), so its top corners follow the card's.
    if (part.media == ProfileMediaKind::banner)
        result.clip = ui_rect_intersection(result.clip, result.rectangle);
    return media_asset_key_valid(result.key);
}
bool profile_avatar_still_media(const ProfileScene& scene, float dips, ProfileMedia* out) noexcept {
    if (!scene.active || !scene.profile)
        return false;
    const auto& p = *scene.profile;
    const auto display = profile_display(p);
    ProfileMedia& result = start(out);
    auto& url = result.url;
    cdn_root(&url);
    if (!display.avatar.bytes) {
        // The CDN's default (embed) avatar, under the key every list shares.
        const u32 index = default_avatar_index(p.user_id);
        default_avatar_path(&url, index);
        result.key = default_avatar_key(index);
    } else {
        if (!asset(display.avatar))
            return false;
        if (display.guild_avatar) {
            member_path(&url, p.guild_id, p.user_id);
            append(&url, "avatars/");
        } else
            segment(&url, "avatars/", p.user_id);
        append(&url, display.avatar);
        // A static WebP: the first frame of an animated avatar. The same URL and key as a still avatar drawn
        // at this size, so a still one is not fetched twice.
        append(&url, ".webp?size=");
        id(&url, cdn_display_size(dips, scene.media_dpi, kCdnSquareSizeLimit));
        result.key = profile_key(url, MediaAttachmentFormat::webp, false);
    }
    return media_asset_key_valid(result.key);
}
namespace {
// What each layer of a member row reads: the row, its node, its avatar's node and the user's global and server
// appearance records. member_media_cache_fill looks them up once for all the layers of the row.
struct MemberRowMedia {
    const MemberListRowWidget* row;
    const UiLayoutNode *node, *avatar;
    const UserAppearanceRecord *global, *guild;
};
bool member_row_media(const MemberListScene& s, u32 index, bool visible_only, MemberRowMedia* out) noexcept {
    if (!s.active || !s.users || !s.source || index >= s.count)
        return false;
    const auto& row = s.rows[index];
    if (!member_list_scene_row_resident(s, index) || row.kind != MemberListRowKind::member)
        return false;
    const auto* r = member_list_scene_layout_node(s, row.widget);
    const auto* avatar = member_list_scene_layout_node(s, row.avatar);
    if (!r || !avatar || (visible_only && !ui_rect_intersects(r->rectangle, s.viewport)))
        return false;
    *out = {&row, r, avatar, user_appearance_find(s.users->appearance, row.user_id),
            user_appearance_find(s.users->appearance, row.user_id, s.source->guild_id)};
    return true;
}
// One layer of member_media_resolve. `badge` is the row's member_list_badge_resolve when the caller holds it.
bool member_layer_media(const MemberListScene& s, u32 index, const MemberRowMedia& m, MemberMedia layer,
                        const UiBadgeWidget* badge, ProfileMedia* out) noexcept {
    const auto& row = *m.row;
    const auto *r = m.node, *avatar = m.avatar;
    const auto *global = m.global, *guild = m.guild;
    ProfileMedia& result = start(out);
    result.clip = s.viewport;
    result.rectangle = avatar->rectangle;
    if (layer == MemberMedia::status_emoji) {
        const auto* a = guild && (guild->fields & appearance_activity) ? guild : global;
        if (!a || !a->status_emoji_id || !a->activity.bytes ||
            row.source_row.status == GuildMemberStatus::offline)
            return false;
        const auto* label = member_list_scene_layout_node(s, row.label);
        if (!label)
            return false;
        // After the activity glyph that leads the status line, as the text does.
        UiActivityStatus status{};
        member_list_row_status(s, index, {}, &status);
        const auto line = ui_activity_glyph_text(member_list_row_status_line(label->content_rectangle), status.glyph);
        result.rectangle = {line.left, line.top, line.left + 16, line.top + 16};
        result.key = media_asset_custom_emoji(a->status_emoji_id, a->emoji_animated);
        return true;
    }
    auto& url = result.url;
    cdn_root(&url);
    auto format = MediaAttachmentFormat::webp;
    bool animated = false;
    Utf8View hash{};
    // Compare against this row's widgets directly instead of searching all
    // materialized rows for the hovered widget.
    const u32 hovered = ui_widget_handle_bits(s.widgets.hovered);
    const bool row_hovered =
        (ui_widget_handle_bits(row.widget) == hovered || ui_widget_handle_bits(row.avatar) == hovered ||
         ui_widget_handle_bits(row.label) == hovered || ui_widget_handle_bits(row.status) == hovered) &&
        ui_widget_manager_resolve(s.widgets, s.widgets.hovered) && !s.system_animation_off;
    if (layer == MemberMedia::nameplate || layer == MemberMedia::nameplate_still) {
        const auto* a = guild && (guild->fields & appearance_nameplate) ? guild : global;
        if (!a || !a->nameplate_sku)
            return false;
        animated = row_hovered;
        // The still behind a hovered row's animation: drawn while the animated variant is not
        // resident (so the nameplate never blinks out on hover) and kept pinned with it.
        if (layer == MemberMedia::nameplate_still) {
            if (!animated)
                return false;
            animated = false;
        }
        nameplate_path(&url, a->nameplate_sku, animated);
        format = MediaAttachmentFormat::png;
        result.rectangle = {r->rectangle.left + 4, r->rectangle.top + 2, r->rectangle.right - 4,
                            r->rectangle.bottom - 2};
    } else if (layer == MemberMedia::guild_badge) {
        if (!global || !global->primary_enabled || !global->primary_guild_id ||
            !asset(global->primary_badge.view()))
            return false;
        badge_path(&url, global->primary_guild_id, global->primary_badge.view());
        format = MediaAttachmentFormat::png;
        UiBadgeWidget resolved;
        if (!badge) {
            resolved = member_list_badge_resolve(s, index);
            badge = &resolved;
        }
        result.rectangle = ui_badge_geometry(*badge, s.widgets).icon;
    } else if (layer == MemberMedia::decoration || layer == MemberMedia::decoration_still) {
        const auto* a = guild && (guild->fields & appearance_decoration) ? guild : global;
        if (!a || !asset(a->decoration.view()))
            return false;
        // Like the nameplate: a still at the drawn size at rest, the animation only under the pointer
        // (the CDN returns an animated preset at its native 288 px whatever `size` says).
        animated = ui_guild_banner_animated_hash(a->decoration.view()) && row_hovered;
        if (layer == MemberMedia::decoration_still) {
            if (!animated)
                return false;
            animated = false;
        }
        result.rectangle = {avatar->rectangle.left - 4, avatar->rectangle.top - 4,
                            avatar->rectangle.right + 4, avatar->rectangle.bottom + 4};
        append(&url, "avatar-decoration-presets/");
        append(&url, a->decoration.view());
        append(&url, ".png?size=");
        id(&url, cdn_display_size(ui_rect_width(result.rectangle), s.media_dpi));
        passthrough(&url, animated);
        format = MediaAttachmentFormat::png;
    } else {
        // Only a per-guild avatar needs its own URL identity. Plain avatars use
        // the shared user-avatar key (member_plain_avatar_resolve), so the
        // timeline, DM rows and member list decode each avatar once.
        if (!guild || !guild->avatar.bytes || !s.source->guild_id)
            return false;
        hash = guild->avatar.view();
        if (!asset(hash))
            return false;
        member_path(&url, s.source->guild_id, row.user_id);
        append(&url, "avatars/");
        append(&url, hash);
        append(&url, ".webp?size=64");
        animated = ui_guild_banner_animated_hash(hash);
        result.circular = true;
    }
    if (animated && format == MediaAttachmentFormat::webp)
        append(&url, "&animated=true");
    result.key = profile_key(url, format, animated);
    return media_asset_key_valid(result.key);
}
// member_plain_avatar_resolve past its row checks. `guild` is the user's server appearance record (null outside a
// server).
bool member_plain_avatar(const MemberListScene& s, const MemberListRowWidget& row, const UiLayoutNode& avatar,
                         const UserAppearanceRecord* guild, AvatarMedia* out) noexcept {
    if (guild && guild->avatar.bytes)
        return false;
    const auto* user = user_store_find(*s.users, row.user_id);
    if (!user)
        return false;
    const Utf8View hash = user_store_avatar_hash(*user);
    MediaAssetKey key;
    if (!avatar_media_key(row.user_id, hash, &key))
        return false;
    *out = {key, hash, avatar.rectangle, s.viewport};
    return true;
}
} // namespace
bool member_media_resolve(const MemberListScene& s, u32 index, MemberMedia layer, ProfileMedia* out,
                          bool visible_only) noexcept {
    MemberRowMedia m;
    return member_row_media(s, index, visible_only, &m) && member_layer_media(s, index, m, layer, nullptr, out);
}
void member_media_cache_fill(const MemberListScene& s, u32 index, const UiBadgeWidget& badge, float dx, float dy,
                             MemberMediaCacheEntry* entry) noexcept {
    MemberRowMedia m;
    if (!member_row_media(s, index, false, &m))
        return;
    ProfileMedia media; // member_layer_media sets it
    for (u32 layer = 0; layer < kMemberMediaLayerCount; ++layer) {
        if (!member_layer_media(s, index, m, static_cast<MemberMedia>(layer), &badge, &media))
            continue;
        entry->layer_mask = static_cast<u8>(entry->layer_mask | (1u << layer));
        entry->circular_mask = static_cast<u8>(entry->circular_mask | (media.circular ? 1u << layer : 0u));
        entry->keys[layer] = media.key;
        entry->rectangles[layer] = member_media_offset(media.rectangle, dx, dy);
    }
    constexpr u32 avatar = static_cast<u32>(MemberMedia::avatar);
    AvatarMedia plain;
    if (!(entry->layer_mask & (1u << avatar)) &&
        member_plain_avatar(s, *m.row, *m.avatar, s.source->guild_id ? m.guild : nullptr, &plain)) {
        entry->plain_avatar = true;
        entry->layer_mask = static_cast<u8>(entry->layer_mask | (1u << avatar));
        entry->circular_mask = static_cast<u8>(entry->circular_mask | (1u << avatar));
        entry->keys[avatar] = plain.key;
        entry->rectangles[avatar] = member_media_offset(plain.rectangle, dx, dy);
    }
}
bool member_plain_avatar_resolve(const MemberListScene& s, u32 index, AvatarMedia* out) noexcept {
    if (!s.active || !s.users || !s.source || index >= s.count)
        return false;
    const auto& row = s.rows[index];
    if (!member_list_scene_row_resident(s, index) || row.kind != MemberListRowKind::member || !row.user_id)
        return false;
    const auto* avatar = member_list_scene_layout_node(s, row.avatar);
    if (!avatar || !ui_rect_intersects(avatar->rectangle, s.viewport))
        return false;
    return member_plain_avatar(
        s, row, *avatar,
        s.source->guild_id ? user_appearance_find(s.users->appearance, row.user_id, s.source->guild_id) : nullptr,
        out);
}
bool member_plain_avatar_current(const MemberListScene& s, MediaAssetKey key) noexcept {
    const bool shared = is_default_avatar_key(key);
    if ((key.kind != MediaAssetKind::user_avatar && !shared) || !s.active)
        return false;
    return avatar_media_current(0, s.count, key, [&](u32 i, AvatarMedia* media) noexcept {
        return (shared || s.rows[i].user_id == key.id) && member_plain_avatar_resolve(s, i, media);
    });
}
bool profile_effect_media_resolve(const ProfileScene& s, u32 index, u64 now, ProfileMedia* out) noexcept {
    if (!s.active || !s.effect || !s.effect->sku)
        return false;
    ProfileMedia& result = start(out);
    auto& url = result.url;
    bool animated = !s.reduced_motion;
    // The effect spans the card (the popout inside its theme border, the
    // modal's profile card); layer positions are authored for 420 wide.
    const UiRect area = ui_rect_has_area(s.effect_area) ? s.effect_area : s.rectangle;
    const float width = ui_rect_width(area), scale = width / 420.f;
    result.rectangle = area;
    // An editor preview scrolled partly out of its page is cut at the page's edge.
    result.clip = ui_rect_intersection(area, s.viewport);
    result.opacity = profile_scene_effect_opacity(s, now);
    if (s.reduced_motion) {
        if (index || !s.effect->reduced_source.bytes)
            return false;
        append(&url, s.effect->reduced_source.view());
    } else {
        if (index >= s.effect->count)
            return false;
        const auto& layer = s.effect->layers[index];
        const u64 elapsed = now > s.effect_started_ms ? now - s.effect_started_ms : 0;
        if (s.effect_started_ms && (now < s.effect_started_ms || elapsed < layer.start_ms))
            return false;
        const u64 local = elapsed - layer.start_ms;
        const u64 period = static_cast<u64>(layer.duration_ms) + layer.loop_delay_ms;
        const u64 cycle = layer.loop && layer.loop_delay_ms && period ? local / period : 0;
        result.playback_epoch = s.effect_started_ms ? (s.effect_started_ms * 65537ull + cycle + 1) : 0;
        if (s.effect_started_ms && !layer.loop && local >= layer.duration_ms)
            return false;
        if (s.effect_started_ms && layer.loop && layer.duration_ms + layer.loop_delay_ms &&
            local % (layer.duration_ms + layer.loop_delay_ms) >= layer.duration_ms)
            return false;
        if (layer.id.bytes) {
            if (!asset(layer.id.view()))
                return false;
            cdn_root(&url);
            append(&url, "media/v1/collectibles-shop/");
            id(&url, s.effect->sku);
            append(&url, "/");
            append(&url, layer.id.view());
            append(&url, "/static");
        } else
            append(&url, layer.source.view());
        result.rectangle.left += layer.x * scale;
        result.rectangle.right += layer.x * scale;
        result.rectangle.top += layer.y * scale;
        result.rectangle.bottom += layer.y * scale;
    }
    if (!url.bytes)
        return false;
    auto format = MediaAttachmentFormat::png;
    for (u32 i = 0; i + 5 <= url.bytes; ++i) {
        if (bytes_equal(url.data + i, ".webp", 5))
            format = MediaAttachmentFormat::webp;
        if (bytes_equal(url.data + i, ".gif", 4))
            format = MediaAttachmentFormat::gif;
    }
    // The scan stops before the last four bytes: a ".gif" there is the last match.
    if (url.bytes >= 4 && bytes_equal(url.data + url.bytes - 4, ".gif", 4))
        format = MediaAttachmentFormat::gif;
    result.key = profile_key(url, format, animated, true);
    return true;
}
} // namespace maicord
