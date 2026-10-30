#include "maicord/ui/badge.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/ui/profile_media.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/ui/profile_appearance.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/model/member_list.hpp"
#include "maicord/ui/member_list_scene.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/media/asset.hpp"
namespace maicord {
bool profile_media_use_compatibility_banner(ProfileScene* scene) noexcept {
    if (!scene || scene->banner_compatibility) return false;
    scene->banner_compatibility = true;
    return true;
}
namespace {
__forceinline u32 widget_bits(UiWidgetHandle handle) noexcept {
    static_assert(sizeof(handle) == sizeof(u32));
    u32 bits;
    CopyMemory(&bits, &handle, sizeof(bits));
    return bits;
}
bool append(ProfileString<384>* s, Utf8View text) noexcept {
    if (text.bytes > 384u - s->bytes)
        return false;
    copy_bytes(s->data + s->bytes, text.data, text.bytes);
    s->bytes += static_cast<u16>(text.bytes);
    return true;
}
bool id(ProfileString<384>* s, u64 value) noexcept {
    const u32 n = decimal_digits(value);
    if (n > 384u - s->bytes)
        return false;
    s->bytes += static_cast<u16>(decimal_write(s->data + s->bytes, value));
    return true;
}
bool asset(Utf8View value) noexcept {
    if (!value.bytes)
        return false;
    for (u32 i = 0; i < value.bytes; ++i) {
        const char c = value.data[i];
        if (!(ascii_is_alnum(c) || c == '_'))
            return false;
    }
    return true;
}
// "a_..." hashes are animated.
bool animated_hash(const Utf8View& hash) noexcept {
    return hash.bytes > 2 && hash.data[0] == 'a' && hash.data[1] == '_';
}
// The key of an image fetched from `url`.
MediaAssetKey profile_key(const ProfileString<384>& url, MediaAttachmentFormat format, bool animated) noexcept {
    auto key = media_asset_embed(hash64_bytes(kHash64Seed, url.data, url.bytes), 1, 0, MediaEmbedResource::image,
                                 format, animated, MediaResolutionTier::px512);
    key.kind = MediaAssetKind::profile_image;
    return key;
}
// A picked image placed so its crop mask covers target (profile editor
// preview): the whole turned image at the target's scale.
bool pending_media(const ProfilePendingImage& image, UiRect target, UiRect mask, float radius, UiRect clip,
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
bool guild_badge_media(bool enabled, u64 guild_id, Utf8View badge, UiRect rectangle, UiRect clip,
                       ProfileMedia* out) noexcept {
    if (!out || !enabled || !guild_id || !asset(badge))
        return false;
    ProfileMedia result{};
    append(&result.url, utf8_literal("https://cdn.discordapp.com/clan-badges/"));
    id(&result.url, guild_id);
    append(&result.url, utf8_literal("/"));
    append(&result.url, badge);
    append(&result.url, utf8_literal(".png?size=32"));
    result.key = profile_key(result.url, MediaAttachmentFormat::png, false);
    copy_value(&result.rectangle, rectangle);
    result.clip = clip;
    copy_value(out, result);
    return true;
}
} // namespace
bool user_guild_badge_media(const UserAppearance& appearance, UiRect rectangle, UiRect clip,
                            ProfileMedia* out) noexcept {
    return guild_badge_media(appearance.primary_enabled, appearance.primary_guild_id,
                             appearance.primary_badge.view(), rectangle, clip, out);
}
bool user_guild_badge_media(const UserAppearanceRecord& appearance, UiRect rectangle, UiRect clip,
                            ProfileMedia* out) noexcept {
    return guild_badge_media(appearance.primary_enabled, appearance.primary_guild_id,
                             appearance.primary_badge.view(), rectangle, clip, out);
}
bool profile_media_resolve(const ProfileScene& scene, u32 index, ProfileMedia* out) noexcept {
    if (!out || !scene.active || !scene.profile || index >= scene.part_count)
        return false;
    const auto& part = scene.parts[index];
    const auto& p = *scene.profile;
    const auto* node = ui_widget_manager_layout_node(scene.widgets, part.widget);
    if (!node)
        return false;
    ProfileMedia result{};
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
        copy_value(out, result);
        return true;
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
        copy_value(out, result);
        return media_asset_key_valid(result.key);
    }
    auto& url = result.url;
    append(&url, utf8_literal("https://cdn.discordapp.com/"));
    Utf8View hash{};
    bool animated = false;
    auto format = MediaAttachmentFormat::webp;
    const auto display = profile_display(p);
    const bool main_avatar = part.media == ProfileMediaKind::peer_avatar && part.source == 0xFFFF;
    if (part.media == ProfileMediaKind::avatar && !display.avatar.bytes) {
        // Users without an avatar show the CDN's default (embed) avatar.
        append(&url, utf8_literal("embed/avatars/"));
        id(&url, (p.user_id >> 22) % 6);
        append(&url, utf8_literal(".png"));
        result.circular = true;
        result.key = profile_key(url, MediaAttachmentFormat::png, false);
        copy_value(out, result);
        return media_asset_key_valid(result.key);
    }
    if (part.media == ProfileMediaKind::member_guild) {
        const auto* guild = scene.state ? chat_state_store_find_guild(*scene.state, p.guild_id) : nullptr;
        hash = guild ? chat_state_store_guild_icon_hash(*guild) : Utf8View{};
        if (!asset(hash))
            return false;
        append(&url, utf8_literal("icons/"));
        id(&url, p.guild_id);
        append(&url, utf8_literal("/"));
        animated = animated_hash(hash);
    } else if (main_avatar) {
        hash = p.avatar.view();
        if (!asset(hash))
            return false;
        append(&url, utf8_literal("avatars/"));
        id(&url, p.user_id);
        append(&url, utf8_literal("/"));
        result.circular = true;
        animated = animated_hash(hash);
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
        append(&url, user ? utf8_literal("avatars/") : utf8_literal("icons/"));
        id(&url, peer.id);
        append(&url, utf8_literal("/"));
        // Masked to their own circle: the full profile lists them outside the
        // profile card that unmasked media is rounded to.
        result.circular = true;
        result.mask = result.rectangle;
        result.mask_radius = ui_rect_width(result.rectangle) * .5f;
        animated = animated_hash(hash);
    } else if (part.media == ProfileMediaKind::avatar || part.media == ProfileMediaKind::banner) {
        const bool avatar = part.media == ProfileMediaKind::avatar;
        // Per-server avatar and banner fall back to the user's.
        const bool guild = avatar ? display.guild_avatar : display.guild_banner;
        hash = avatar ? display.avatar : display.banner;
        if (!asset(hash))
            return false;
        if (guild) {
            append(&url, utf8_literal("guilds/"));
            id(&url, p.guild_id);
            append(&url, utf8_literal("/users/"));
            id(&url, p.user_id);
            append(&url, avatar ? utf8_literal("/avatars/") : utf8_literal("/banners/"));
        } else {
            append(&url, avatar ? utf8_literal("avatars/") : utf8_literal("banners/"));
            id(&url, p.user_id);
            append(&url, utf8_literal("/"));
        }
        animated = animated_hash(hash);
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
        append(&url, utf8_literal("clan-badges/"));
        id(&url, a.primary_guild_id);
        append(&url, utf8_literal("/"));
        format = MediaAttachmentFormat::png;
    } else if (part.media == ProfileMediaKind::decoration) {
        hash = display.decoration;
        if (!asset(hash))
            return false;
        append(&url, utf8_literal("avatar-decoration-presets/"));
        format = MediaAttachmentFormat::png;
        animated = animated_hash(hash);
    } else if (part.media == ProfileMediaKind::badge && part.source < p.badge_count) {
        hash = p.badges[part.source].icon.view();
        if (!asset(hash))
            return false;
        append(&url, utf8_literal("badge-icons/"));
        format = MediaAttachmentFormat::png;
    } else
        return false;
    append(&url, hash);
    append(&url, format == MediaAttachmentFormat::png   ? utf8_literal(".png")
                 : format == MediaAttachmentFormat::gif ? utf8_literal(".gif")
                                                        : utf8_literal(".webp"));
    // Banners are fetched at 600 (the 300 DIP popout at 2x, the modal's 400),
    // animated WebP ones at the 512 its frames decode to; the modal's 120 DIP
    // avatar wants 256.
    const bool large = part.media == ProfileMediaKind::avatar && ui_rect_width(result.rectangle) > 96;
    const bool animated_webp = animated && format == MediaAttachmentFormat::webp;
    append(&url, part.media == ProfileMediaKind::banner ? animated_webp ? utf8_literal("?size=512")
                                                                         : utf8_literal("?size=600")
                 : large                                ? utf8_literal("?size=256")
                                                        : utf8_literal("?size=128"));
    if (animated && format == MediaAttachmentFormat::webp)
        append(&url, utf8_literal("&animated=true"));
    if (part.media == ProfileMediaKind::decoration)
        append(&url, animated ? utf8_literal("&passthrough=true") : utf8_literal("&passthrough=false"));
    result.key = profile_key(url, format, animated);
    // Clipped to the banner; the renderer rounds it with the card's mask
    // (profile_scene_card_mask), so its top corners follow the card's.
    if (part.media == ProfileMediaKind::banner)
        result.clip = ui_rect_intersection(result.clip, result.rectangle);
    copy_value(out, result);
    return media_asset_key_valid(result.key);
}
bool member_media_resolve(const MemberListScene& s, u32 index, MemberMedia layer, ProfileMedia* out,
                          bool visible_only) noexcept {
    if (!out || !s.active || !s.users || !s.source || index >= s.count)
        return false;
    const auto& row = s.rows[index];
    if (!member_list_scene_row_resident(s, index) || row.kind != MemberListRowKind::member)
        return false;
    const auto* r = member_list_scene_layout_node(s, row.widget);
    const auto* avatar = member_list_scene_layout_node(s, row.avatar);
    if (!r || !avatar || (visible_only && !ui_rect_intersects(r->rectangle, s.viewport)))
        return false;
    const auto* global = user_appearance_find(s.users->appearance, row.user_id);
    const auto* guild = user_appearance_find(s.users->appearance, row.user_id, s.source->guild_id);
    ProfileMedia result{};
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
        const auto label_rect = label->content_rectangle;
        result.rectangle = {label_rect.left, label_rect.top + 16, label_rect.left + 16,
                            label_rect.top + 32};
        result.key = media_asset_custom_emoji(a->status_emoji_id, a->emoji_animated);
        copy_value(out, result);
        return true;
    }
    auto& url = result.url;
    append(&url, utf8_literal("https://cdn.discordapp.com/"));
    auto format = MediaAttachmentFormat::webp;
    bool animated = false;
    Utf8View hash{};
    if (layer == MemberMedia::nameplate) {
        const auto* a = guild && (guild->fields & appearance_nameplate) ? guild : global;
        if (!a || !a->nameplate_sku)
            return false;
        append(&url, utf8_literal("media/v1/collectibles-shop/"));
        id(&url, a->nameplate_sku);
        // Compare against this row's widgets directly instead of searching all
        // materialized rows for the hovered widget.
        const u32 hovered = widget_bits(s.widgets.hovered);
        animated = (widget_bits(row.widget) == hovered || widget_bits(row.avatar) == hovered ||
                    widget_bits(row.label) == hovered || widget_bits(row.status) == hovered) &&
                   ui_widget_manager_resolve(s.widgets, s.widgets.hovered);
        if (animated) {
            BOOL motion = TRUE;
            SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &motion, 0);
            animated = motion != FALSE;
        }
        append(&url, animated ? utf8_literal("/animated") : utf8_literal("/static"));
        format = MediaAttachmentFormat::png;
        result.rectangle = {r->rectangle.left + 4, r->rectangle.top + 2, r->rectangle.right - 4,
                            r->rectangle.bottom - 2};
    } else if (layer == MemberMedia::guild_badge) {
        if (!global || !global->primary_enabled || !global->primary_guild_id ||
            !asset(global->primary_badge.view()))
            return false;
        append(&url, utf8_literal("clan-badges/"));
        id(&url, global->primary_guild_id);
        append(&url, utf8_literal("/"));
        append(&url, global->primary_badge.view());
        append(&url, utf8_literal(".png?size=32"));
        format = MediaAttachmentFormat::png;
        const auto badge = member_list_badge_resolve(s, index);
        result.rectangle = ui_badge_geometry(badge, s.widgets).icon;
    } else if (layer == MemberMedia::decoration) {
        const auto* a = guild && (guild->fields & appearance_decoration) ? guild : global;
        if (!a || !asset(a->decoration.view()))
            return false;
        append(&url, utf8_literal("avatar-decoration-presets/"));
        append(&url, a->decoration.view());
        append(&url, utf8_literal(".png?size=96&passthrough=true"));
        format = MediaAttachmentFormat::png;
        animated = animated_hash(a->decoration.view());
        result.rectangle = {avatar->rectangle.left - 4, avatar->rectangle.top - 4,
                            avatar->rectangle.right + 4, avatar->rectangle.bottom + 4};
    } else {
        // Only a per-guild avatar needs its own URL identity. Plain avatars use
        // the shared user-avatar key (member_plain_avatar_resolve), so the
        // timeline, DM rows and member list decode each avatar once.
        if (!guild || !guild->avatar.bytes || !s.source->guild_id)
            return false;
        hash = guild->avatar.view();
        if (!asset(hash))
            return false;
        append(&url, utf8_literal("guilds/"));
        id(&url, s.source->guild_id);
        append(&url, utf8_literal("/users/"));
        id(&url, row.user_id);
        append(&url, utf8_literal("/avatars/"));
        append(&url, hash);
        append(&url, utf8_literal(".webp?size=64"));
        animated = animated_hash(hash);
        result.circular = true;
    }
    if (animated && format == MediaAttachmentFormat::webp)
        append(&url, utf8_literal("&animated=true"));
    result.key = profile_key(url, format, animated);
    copy_value(out, result);
    return media_asset_key_valid(result.key);
}
bool member_plain_avatar_resolve(const MemberListScene& s, u32 index, MemberAvatarMedia* out,
                                 bool visible_only) noexcept {
    if (!out || !s.active || !s.users || !s.source || index >= s.count)
        return false;
    const auto& row = s.rows[index];
    if (!member_list_scene_row_resident(s, index) || row.kind != MemberListRowKind::member || !row.user_id)
        return false;
    const auto* avatar = member_list_scene_layout_node(s, row.avatar);
    if (!avatar || (visible_only && !ui_rect_intersects(avatar->rectangle, s.viewport)))
        return false;
    if (s.source->guild_id) {
        const auto* guild = user_appearance_find(s.users->appearance, row.user_id, s.source->guild_id);
        if (guild && guild->avatar.bytes)
            return false;
    }
    const auto* user = user_store_find(*s.users, row.user_id);
    if (!user)
        return false;
    const Utf8View hash = user_store_avatar_hash(*user);
    const u32 variant = user_avatar_variant(hash);
    if (!variant)
        return false;
    *out = {media_asset_user_avatar(row.user_id, variant, asset_hash_animated(hash)), hash, avatar->rectangle,
            s.viewport};
    return true;
}
bool member_plain_avatar_current(const MemberListScene& s, MediaAssetKey key) noexcept {
    if (key.kind != MediaAssetKind::user_avatar || !s.active)
        return false;
    for (u32 i = 0; i < s.count; ++i) {
        if (s.rows[i].user_id != key.id)
            continue;
        MemberAvatarMedia media{};
        if (member_plain_avatar_resolve(s, i, &media) && media_asset_key_equal(media.key, key))
            return true;
    }
    return false;
}
bool profile_effect_media_resolve(const ProfileScene& s, u32 index, u64 now, ProfileMedia* out) noexcept {
    if (!out || !s.active || !s.effect || !s.effect->sku)
        return false;
    ProfileMedia result{};
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
            append(&url, utf8_literal("https://cdn.discordapp.com/media/v1/collectibles-shop/"));
            id(&url, s.effect->sku);
            append(&url, utf8_literal("/"));
            append(&url, layer.id.view());
            append(&url, utf8_literal("/static"));
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
    result.key = profile_key(url, format, animated);
    copy_value(out, result);
    return true;
}
} // namespace maicord
