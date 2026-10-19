#include "application_load_trace.hpp"
#include "application_media_pump.hpp"
#include "application_channel_authority.hpp"
#include "application_profile.hpp"
#include "application_profile_editor.hpp"
#include "application_guild_card.hpp"
#include "application_guild_banner.hpp"
#include "application_media_picker.hpp"
#include "application_composer_preview.hpp"
#include "application_image_viewer.hpp"
#include "catalog_preview_log.hpp"
#include "maicord/render/inline_media.hpp"
#include "maicord/platform/clipboard_image.hpp"

#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/media/animation_timeline.hpp"
#include "maicord/media/presentation_policy.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/private_channel_rows.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/ui/search_avatar_media.hpp"
#include "application_search.hpp"
#include "application_stream.hpp"
#include "maicord/ui/pinned_messages_avatar_media.hpp"
#include "maicord/ui/forum_channel_media.hpp"
#include "maicord/ui/voice_channel_rows.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/quick_switcher_scene.hpp"
#include "maicord/ui/message_forward_scene.hpp"
#include "maicord/ui/inbox_avatar_media.hpp"
#include "maicord/ui/poll_voters_avatar_media.hpp"
#include "../render/d3d11_renderer_interaction_state.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {

// Admission concerns preview decoding only. Blurred media still needs its
// preview; notices must not fetch, refresh URLs, or retain animated visibility.
bool preview_media_allowed(const Application& application, const MessageRecord& message,
                           const MessageRichStore& store, bool search, MessageMediaSource source, u32 index,
                           bool snapshot, bool spoiler, bool explicit_media) noexcept {
    const MessageScene* scene = &application.message_scene;
    if (search) {
        scene = application.renderer.search_scene
                    ? ui_search_scene_message_scene(*application.renderer.search_scene)
                    : nullptr;
        if (!scene)
            return false;
    }
    const auto owner = message_media_owner(message, message_rich_store_find(store, message.id));
    return !message_media_obscure_notice(message_scene_media_obscure(
        *scene, owner, message.id, message_media_reveal_key(message.id, source, index, snapshot), spoiler,
        explicit_media));
}

bool preview_decoder_animation(Application& application, const MediaAtlasResult& resident,
                               MediaAssetKey key) noexcept {
    const bool published = animation_timeline_find(&application.animations.timeline, key).ok();
    // A decoder-proven resident first frame remains animated when a stale
    // message owner retires its timeline and the real new owner must acquire it.
    return published || (resident.ok() && media_atlas_decoded_animation(
                                              *application.renderer.preview_media_atlas, resident.handle));
}

// Chat animation policy: paused media is only looked up, never marked
// visible, so its timeline holds the current frame until hover or focus.
bool observe_message_animation(Application& application, MediaAssetKey key, u64 message_id) noexcept {
    return message_media_controller_animation_plays(application.message_media, key.kind, message_id)
               ? animation_timeline_observe_visible(
                     &application.animations.timeline, key,
                     clock_microseconds(application.clock, application.process_start, clock_now()))
               : animation_timeline_find(&application.animations.timeline, key).ok();
}

// Forum post cards own their starter-image thumbnails while the browser is shown.
bool forum_thumbnail_key_current(const Application& application, MediaAssetKey key) noexcept {
    return key.kind == MediaAssetKind::attachment && !application.login.visible &&
           d3d11_renderer_thread_browser_visible(application.renderer) &&
           application.renderer.thread_browser_scene->forum &&
           forum_channel_thumbnail_current(*application.renderer.thread_browser_scene, application.renderer.dpi, key);
}

// A picker tile whose source the account refused or could not fetch or
// decode keeps its placeholder. The worker reports a failure once and then
// waits out its retry; a repeat for the same tile is not logged again.
void log_catalog_preview_failure(const AccountMediaCatalogPreviewRequest& request,
                                 const AccountMediaRequestResult& result) noexcept {
    static MediaAssetKey last{};
    if (media_asset_key_equal(last, request.key))
        return;
    last = request.key;
    CatalogPreviewLog("request")
        .field("item", request.key.id)
        .field("format", static_cast<u64>(request.format))
        .field("state", static_cast<u64>(result.state))
        .field("error", static_cast<u64>(result.error))
        .field("http_error", static_cast<u64>(result.http_error))
        .field("client_error", static_cast<u64>(result.client_error))
        .field("url_error", static_cast<u64>(result.media_url_error))
        .field("video_failure", static_cast<u64>(result.video_failure))
        .field("gif_error", static_cast<u64>(result.gif_error))
        .field("webp_error", static_cast<u64>(result.animated_webp_error))
        .source(request.url)
        .write();
}

} // namespace

bool ApplicationMediaPump::content_media_visible() const noexcept {
    return !(application.renderer.forum_form && application.renderer.forum_form->scene &&
             application.renderer.forum_form->scene->active) &&
           !application.login.visible &&
           !(application.renderer.settings_scene && application.renderer.settings_scene->active) &&
           !(application.renderer.friends_scene && application.renderer.friends_scene->active) &&
           // The thread browser covers the chat column; chrome stays visible.
           !d3d11_renderer_thread_browser_visible(application.renderer);
}
bool ApplicationMediaPump::catalog_media_visible() const noexcept {
    const auto* scene = application.renderer.media_picker_scene;
    return content_media_visible() && scene && scene->active && ui_media_picker_scene_current(*scene) &&
           !(application.renderer.profile_scene && application.renderer.profile_scene->active) &&
           !application.image_viewer.scene.active && !application.reaction_viewer.active &&
           !(application.renderer.text_form && application.renderer.text_form->scene &&
             application.renderer.text_form->scene->active) &&
           !application.popup_scene.active;
}
bool ApplicationMediaPump::search_media_visible() const noexcept {
    const auto& renderer = application.renderer;
    return content_media_visible() && d3d11_renderer_search_visible(renderer) &&
           !application.image_viewer.scene.active && !application.reaction_viewer.active &&
           !application.popup_scene.active && !application.text_form &&
           !(renderer.profile_scene && renderer.profile_scene->active) &&
           !(renderer.quick_switcher_scene && renderer.quick_switcher_scene->active) &&
           !(renderer.message_forward_scene && renderer.message_forward_scene->active) &&
           !d3d11_renderer_pinned_messages_visible(renderer);
}
bool ApplicationMediaPump::request_picker_emoji(u64 now_ms, u64 now_seconds, u64 now_us, u32* uploads,
                                                bool* repaint) noexcept {
    if (!catalog_media_visible())
        return true;
    const auto& picker = *application.renderer.media_picker_scene;
    if (picker.kind == UiMediaPickerKind::gif)
        return true;
    // Sticker cells are catalog previews; only the server icons of their rail load here.
    const u32 count =
        picker.kind == UiMediaPickerKind::emoji ? minimum(picker.cell_count, kUiMediaPickerVisibleCells) : 0;
    for (u32 i = 0; i < count; ++i) {
        UiMediaPickerMediaSlot slot{};
        if (!ui_media_picker_scene_media(picker, i, &slot) || slot.kind != UiMediaPickerItemKind::emoji ||
            !ui_rect_has_area(slot.clip))
            continue;
        const bool animated = (slot.asset_flags & custom_emoji_animated) != 0;
        const auto key = slot.regional_flag
                             ? media_asset_regional_flag(static_cast<u16>(slot.regional_flag))
                             : media_asset_custom_emoji(slot.asset_id, animated);
        if (!media_asset_key_valid(key))
            continue;
        bool force = false;
        if (animated) {
            const bool observed =
                animation_timeline_observe_visible(&application.animations.timeline, key, now_us);
            const auto resident = media_atlas_find(application.renderer.media_atlas, key);
            if (resident.ok() && (observed || !media_atlas_decoded_animation(
                                                  *application.renderer.media_atlas, resident.handle)))
                continue;
            force = !observed;
        }
        if (*uploads >= kMessageMediaUploadBudget)
            continue;
        bool stop = false;
        if (!request_visible_static_media(key, now_ms, now_seconds, uploads, repaint, &stop, force, true))
            return false;
        if (stop)
            return true;
    }
    for (u32 i = 0; i < picker.rail_count && *uploads < kMessageMediaUploadBudget; ++i) {
        UiMediaPickerRailMediaSlot slot{};
        if (!ui_media_picker_scene_rail_media(picker, i, &slot) || !ui_rect_has_area(slot.clip) ||
            !application.account_ui)
            continue;
        const auto* guild =
            chat_state_store_find_guild(application.account_ui->chat_state, slot.guild_id);
        if (!guild || guild->icon_variant != slot.variant ||
            asset_hash_animated(chat_state_store_guild_icon_hash(*guild)) != slot.animated)
            continue;
        const auto key = media_asset_guild_icon(slot.guild_id, slot.variant, slot.animated);
        bool force = false;
        if (slot.animated && animation_resident(key, now_us, &force))
            continue;
        bool stop = false;
        if (!request_visible_guild_icon(*guild, now_ms, now_seconds, uploads, repaint, &stop, force))
            return false;
    }
    return true;
}
bool ApplicationMediaPump::request_inline_flags(u64 now_ms, u64 now_seconds, u32* uploads,
                                                bool* repaint) noexcept {
    if (application.login.visible || visibility_only || !application.renderer.inline_media.flags)
        return true;
    const auto& renderer = application.renderer;
    const u32 count = minimum(renderer.inline_media.count, kRendererInlineFlagCapacity);
    // Multiple foreground labels may borrow the same static country asset.
    // Identity is a bounded Unicode sequence, not a URL or widget hash.
    u8 seen[(680 + 7) / 8]{};
    for (u32 i = 0; i < count && *uploads < kMessageMediaUploadBudget; ++i) {
        const auto& flag = renderer.inline_media.flags[i];
        if (!renderer_inline_flag_current(renderer, flag) || flag.flag >= 680)
            continue;
        const u8 bit = static_cast<u8>(1u << (flag.flag & 7));
        if (seen[flag.flag >> 3] & bit)
            continue;
        seen[flag.flag >> 3] |= bit;
        bool stop = false;
        if (!request_visible_static_media(media_asset_regional_flag(flag.flag), now_ms, now_seconds,
                                          uploads, repaint, &stop, false, true))
            return false;
        if (stop)
            break;
    }
    return true;
}
bool ApplicationMediaPump::request_catalog_previews(u64 now_ms, u64 now_seconds, u64 now_us, u32* uploads,
                                                    bool* repaint) noexcept {
    if (!catalog_media_visible() || !application.account.api.catalog_preview_request)
        return true;
    const auto& scene = *application.renderer.media_picker_scene;
    // Cells, then rail entries (sticker pack covers).
    const u32 count = minimum<u32>(scene.cell_count, kUiMediaPickerVisibleCells) +
                      minimum<u32>(scene.rail_count, kUiMediaPickerVisibleSections);
    for (u32 index = 0; index < count; ++index) {
        AccountMediaCatalogPreviewRequest request{};
        if (!application_media_picker_preview_request(index, &request) ||
            !application_media_picker_preview_current(request.key, request.catalog_scope,
                                                      request.catalog_generation))
            continue;
        retire_stale_message_animation(request.key, request.catalog_scope, request.catalog_generation);
        const bool observed =
            animation_timeline_observe_visible(&application.animations.timeline, request.key, now_us);
        const auto resident = media_atlas_find(application.renderer.preview_media_atlas, request.key);
        if (observed && resident.ok())
            continue;
        // A resident decoder result settles the provisional WebP/GIF hint.
        // Only a decoded animation without its timeline needs reacquisition.
        if (resident.ok() && !preview_decoder_animation(application, resident, request.key))
            continue;
        if (visibility_only)
            continue;
        if (*uploads >= kMessageMediaUploadBudget ||
            !message_media_controller_preview_ready(&application.message_media, now_ms))
            return true;
        // A VIDEO favourite's tile decodes on the presenting device.
        request.video_device = application.renderer.graphics.device;
        const auto requested = application.account.api.catalog_preview_request(
            application.account.instance, &request, now_ms, now_seconds);
        if (requested.state == AccountMediaRequestState::queued)
            record_media_request_queued();
        if (requested.state == AccountMediaRequestState::failed ||
            requested.state == AccountMediaRequestState::unsupported ||
            requested.state == AccountMediaRequestState::invalid)
            log_catalog_preview_failure(request, requested);
        if (requested.state == AccountMediaRequestState::ready) {
            AccountMediaFrame frame{};
            if (!application.account.api.media_take(application.account.instance, &frame))
                return false;
            if (!media_asset_key_equal(frame.key, request.key) || !message_image_frame_is_current(frame))
                continue;
            const auto uploaded = upload_account_media_frame(frame);
            if (uploaded == MediaFrameUploadResult::failed)
                return false;
            if (uploaded == MediaFrameUploadResult::deferred)
                continue;
            ++*uploads;
            record_media_frame_uploaded();
            animation_timeline_observe_visible(&application.animations.timeline, request.key, now_us);
            if (repaint)
                *repaint = true;
        }
        message_media_controller_observe_retry(&application.message_media, requested.retry_at_ms);
        // Saturated lanes do not end this bounded surface's visibility scan.
    }
    return true;
}
namespace {
bool profile_scene_shows(const ProfileScene& scene, MediaAssetKey key) noexcept {
    for (u32 i = 0; i < scene.part_count; ++i) {
        ProfileMedia media{};
        if (profile_media_resolve(scene, i, &media) && media_asset_key_equal(key, media.key))
            return true;
    }
    for (u32 i = 0; scene.effect && i < maximum<u8>(1, scene.effect->count); ++i) {
        ProfileMedia media{};
        if (profile_effect_media_resolve(scene, i, GetTickCount64(), &media) &&
            media_asset_key_equal(key, media.key))
            return true;
    }
    return false;
}
// Edit Profile's preview card (current avatar, banner, decoration and effect
// with pending edits over them), its dialogs and its picked local images.
bool profile_editor_key_current(MediaAssetKey key) noexcept {
    if (key.kind == MediaAssetKind::local_draft_preview && application_profile_editor_frame_current(key))
        return true;
    if (const auto* preview = application_profile_editor_preview();
        preview && profile_scene_shows(*preview, key))
        return true;
    if (const auto* dialog = application_profile_editor_dialog()) {
        ProfileMedia media{};
        for (u32 i = 0; ui_profile_dialog_media(*dialog, i, &media); ++i, media = {})
            if (media_asset_key_equal(key, media.key))
                return true;
    }
    return false;
}
} // namespace
u32 ApplicationMediaPump::yield_decorative_media(MediaAssetKey key) noexcept {
    if (!application.profile || !application.profile->scene.active ||
        !profile_scene_shows(application.profile->scene, key))
        return 0;
    constexpr MemberMedia layers[] = {MemberMedia::nameplate, MemberMedia::decoration};
    MediaAssetKey decorative[128];
    u32 count = 0;
    const auto& members = application.member_list_scene;
    for (u32 i = 0; members.active && i < members.count && count + 2 <= 128; ++i)
        for (const MemberMedia layer : layers) {
            MemberMediaPlacement placed{};
            if (member_media_place(members, i, layer, &placed))
                decorative[count++] = placed.key;
        }
    return media_atlas_yield(application.renderer.media_atlas, {decorative, count});
}
bool ApplicationMediaPump::media_key_is_current(MediaAssetKey key) noexcept {
    if (application.login.visible)
        return false;
    if (forum_thumbnail_key_current(application, key))
        return true;
    // The profile editor is itself an opaque Settings modal: its preview card,
    // dialogs and picked images stay current while every other surface's
    // media is hidden beneath it.
    if (profile_editor_key_current(key))
        return true;
    if (const auto* panel = application.renderer.dm_profile_panel;
        panel && panel->active && content_media_visible() && profile_scene_shows(*panel, key))
        return true;
    if (key.kind == MediaAssetKind::local_draft_preview)
        return !(application.renderer.settings_scene && application.renderer.settings_scene->active) &&
               application_local_preview_key_current(key);
    if (media_asset_search(key)) {
        if (!media_asset_key_valid(key) ||
            (application.renderer.settings_scene && application.renderer.settings_scene->active))
            return false;
        return search_media_visible() ||
               (application.image_viewer.scene.active && application.image_viewer.search_result &&
                application_image_viewer_key_current(key));
    }
    if (key.kind == MediaAssetKind::regional_flag && application.renderer.inline_media.flags) {
        const auto& renderer = application.renderer;
        for (u32 i = 0; i < minimum(renderer.inline_media.count, kRendererInlineFlagCapacity); ++i) {
            const auto& flag = renderer.inline_media.flags[i];
            if (flag.flag == key.id && renderer_inline_flag_current(renderer, flag))
                return true;
        }
    }
    // Settings rows: avatars and icons, and Server Settings' emoji and
    // sticker previews.
    if ((key.kind == MediaAssetKind::user_avatar || key.kind == MediaAssetKind::guild_icon ||
         key.kind == MediaAssetKind::profile_image || key.kind == MediaAssetKind::custom_emoji ||
         key.kind == MediaAssetKind::sticker) &&
        settings_avatar_is_current(key))
        return true;
    if (key.kind == MediaAssetKind::embed && application_stream_preview_key_current(key))
        return true;
    if (key.kind == MediaAssetKind::profile_image &&
        (application_guild_card_key_current(key) || application_guild_banner_key_current(key)))
        return true;
    if (application.renderer.settings_scene && application.renderer.settings_scene->active)
        return false;
    if (key.kind == MediaAssetKind::catalog_preview)
        return catalog_media_visible() && application_media_picker_preview_visible(key);
    if (key.kind == MediaAssetKind::user_avatar)
        return avatar_key_is_current(key);
    if (key.kind == MediaAssetKind::channel_icon)
        return channel_icon_key_is_current(key);
    if (content_media_visible() || key.kind == MediaAssetKind::guild_icon)
        return true;
    if ((key.kind == MediaAssetKind::attachment || key.kind == MediaAssetKind::embed) &&
        application.image_viewer.scene.active && application_image_viewer_key_current(key))

        return true;
    if (application.profile && application.profile->scene.active &&
        profile_scene_shows(application.profile->scene, key))
        return true;
    if ((key.kind == MediaAssetKind::custom_emoji || key.kind == MediaAssetKind::regional_flag) &&
        application.popup_scene.active)
        for (u32 i = 0; i < application.popup_scene.item_count; ++i) {
            PopupMediaCommand command{};
            const auto& item = application.popup_scene.items[i];
            if (popup_media_presentation_resolve(application.popup_scene, i, &command) ==
                    PopupMediaPresentationStatus::ready &&
                !command.avatar && !command.guild_icon &&
                media_asset_key_equal(
                    key, command.regional_flag
                             ? media_asset_regional_flag(command.regional_flag)
                             : media_asset_custom_emoji(item.media_id,
                                                        (item.media_flags & custom_emoji_animated) != 0)))
                return true;
        }
    if (key.kind == MediaAssetKind::custom_emoji && application.reaction_viewer.active) {
        const auto& viewer = application.reaction_viewer;
        for (u32 i = viewer.first_aggregate; i < viewer.aggregate_count; ++i) {
            if (!ui_rect_has_area(reaction_viewer_aggregate_rectangle(viewer, i)))
                break;
            const auto& aggregate = viewer.aggregates[i];
            if (aggregate.emoji_id &&
                media_asset_key_equal(
                    key, media_asset_custom_emoji(aggregate.emoji_id, (aggregate.flags & 1u) != 0)))
                return true;
        }
    }
    return false;
}
bool ApplicationMediaPump::animation_resident(MediaAssetKey key, u64 now_us, bool* force_request,
                                              bool play) noexcept {
    auto& timeline = application.animations.timeline;
    const bool observed = play ? animation_timeline_observe_visible(&timeline, key, now_us)
                               : animation_timeline_find(&timeline, key).ok();
    *force_request = !observed;
    return observed && media_atlas_find(application.renderer.media_atlas, key).ok();
}
bool ApplicationMediaPump::settings_avatar_is_current(MediaAssetKey key) const noexcept {
    if (!renderer_settings_page_visible(application.renderer))
        return false;
    UiSettingsAvatarGeometry avatar{};
    for (u32 cursor = 0; ui_settings_scene_next_avatar(*application.renderer.settings_scene, &cursor, &avatar);)
        if (media_asset_key_equal(avatar.media->key, key))
            return true;
    return false;
}
bool ApplicationMediaPump::request_settings_avatars(u64 now_ms, u64 now_seconds, u32* uploads,
                                                    bool* repaint) noexcept {
    if (!renderer_settings_page_visible(application.renderer))
        return true;
    UiSettingsAvatarGeometry avatar{};
    for (u32 cursor = 0; ui_settings_scene_next_avatar(*application.renderer.settings_scene, &cursor, &avatar);) {
        if (message_media_controller_upload_budget_reached(&application.message_media, *uploads))
            return true;
        bool stop = false;
        // The request validates the hash and skips a resident key. Server
        // identity rows share the rail's guild icon key and CDN asset.
        const Utf8View hash{avatar.media->hash, avatar.media->hash_bytes};
        // Server Settings emoji and sticker previews, by their CDN identity.
        if (avatar.media->key.kind == MediaAssetKind::custom_emoji ||
            avatar.media->key.kind == MediaAssetKind::sticker) {
            if (!request_visible_static_media(avatar.media->key, now_ms, now_seconds, uploads, repaint, &stop, false,
                                              true))
                return false;
            if (stop)
                return true;
            continue;
        }
        if (avatar.media->key.kind == MediaAssetKind::profile_image) {
            // Default avatars: the hash carries the CDN path, fetched by URL
            // under the profile card's key.
            ProfileMedia media{};
            media.key = avatar.media->key;
            constexpr auto host = utf8_literal("https://cdn.discordapp.com/");
            copy_bytes(media.url.data, host.data, host.bytes);
            copy_bytes(media.url.data + host.bytes, hash.data, hash.bytes);
            media.url.bytes = static_cast<u16>(host.bytes + hash.bytes);
            if (!request_profile_media(media, now_ms, now_seconds, now_ms * 1000, uploads, repaint, &stop, false,
                                       nullptr))
                return false;
            if (stop)
                return true;
            continue;
        }
        if (!(avatar.media->key.kind == MediaAssetKind::guild_icon
                  ? request_visible_hashed_identity(avatar.media->key, hash, now_ms, now_seconds, uploads, repaint,
                                                    &stop, false)
                  : request_visible_user_avatar_value(avatar.media->key.id, hash, now_ms, now_seconds, uploads,
                                                      repaint, &stop, false)))
            return false;
        if (stop)
            return true;
    }
    return true;
}
bool ApplicationMediaPump::request_forward_identities(u64 now_ms, u64 now_seconds, u32* uploads, bool* repaint,
                                                      bool* stop) noexcept {
    if (!application.account_ui || !d3d11_renderer_message_forward_visible(application.renderer))
        return true;
    const auto& forward = *application.renderer.message_forward_scene;
    const auto& state = application.account_ui->chat_state;
    for (u32 slot = 0; slot < forward.count; ++slot) {
        UiMessageForwardMedia media{};
        if (!ui_message_forward_scene_media(forward, slot, &state, &media))
            continue;
        if (!visibility_only && *uploads >= kMessageMediaUploadBudget) {
            *stop = true;
            return true;
        }
        // Server tiles go through the rail's guild-icon request, so the key,
        // CDN size and residency are shared with the rail.
        const auto* guild =
            media.key.kind == MediaAssetKind::guild_icon ? chat_state_store_find_guild(state, media.key.id) : nullptr;
        if (media.key.kind == MediaAssetKind::guild_icon
                ? guild && !request_visible_guild_icon(*guild, now_ms, now_seconds, uploads, repaint, stop, false, false)
                : !request_visible_user_avatar_value(media.key.id, media.hash, now_ms, now_seconds, uploads, repaint,
                                                     stop, false))
            return false;
        if (*stop)
            return true;
    }
    return true;
}
bool ApplicationMediaPump::avatar_key_is_current(MediaAssetKey key) noexcept {
    if (!application.account_ui || key.kind != MediaAssetKind::user_avatar || !media_asset_key_valid(key))
        return false;
    const auto& users = application.account_ui->user_store;
    if (d3d11_renderer_search_visible(application.renderer) &&
        search_avatar_media_current(*application.renderer.search_scene, key))
        return true;
    if (d3d11_renderer_pinned_messages_visible(application.renderer) &&
        pinned_messages_avatar_media_current(*application.renderer.pinned_messages_scene, key))
        return true;
    if (d3d11_renderer_thread_browser_visible(application.renderer) &&
        forum_channel_avatar_media_current(*application.renderer.thread_browser_scene, key))
        return true;
    if (d3d11_renderer_quick_switcher_visible(application.renderer)) {
        const auto& switcher = *application.renderer.quick_switcher_scene;
        for (u32 slot = 0; slot < switcher.count; ++slot) {
            UiQuickSwitcherMedia media{};
            if (ui_quick_switcher_scene_media(switcher, slot, nullptr, &media) &&
                media_asset_key_equal(media.key, key))
                return true;
        }
    }
    if (d3d11_renderer_message_forward_visible(application.renderer)) {
        const auto& forward = *application.renderer.message_forward_scene;
        for (u32 slot = 0; slot < forward.count; ++slot) {
            UiMessageForwardMedia media{};
            if (ui_message_forward_scene_media(forward, slot, nullptr, &media) &&
                media_asset_key_equal(media.key, key))
                return true;
        }
    }
    if (d3d11_renderer_inbox_visible(application.renderer) &&
        inbox_avatar_media_current(*application.renderer.inbox_scene, key))
        return true;
    if (d3d11_renderer_poll_voters_visible(application.renderer) &&
        poll_voters_avatar_media_current(*application.renderer.poll_voters_scene, key))
        return true;
    const auto matches = [&](u64 id) noexcept {
        const auto* user = user_store_find(users, id);
        if (!user)
            return false;
        const auto hash = user_store_avatar_hash(*user);
        const u32 variant = user_avatar_variant(hash);
        return variant && media_asset_key_equal(
                              key, media_asset_user_avatar(id, variant, asset_hash_animated(hash)));
    };
    const auto& composer = application.message_composer_scene;
    if (content_media_visible() && ui_rect_has_area(message_composer_scene_command_avatar(composer)) &&
        matches(composer.command.bot_id))
        return true;
    // Member rows count only while chat content (and so the sidebar) is shown.
    if (content_media_visible() && application.member_list_scene.active &&
        member_plain_avatar_current(application.member_list_scene, key))
        return true;
    const auto* requests = application.renderer.friends_requests_scene;
    const bool requests_active = requests && requests->active;
    if (requests_active && friends_avatar_media_current(*requests, users, key))
        return true;
    if (!requests_active && application.renderer.friends_scene &&
        friends_avatar_media_current(*application.renderer.friends_scene, users, key))
        return true;
    // Friends keeps the sidebar/footer visible. Opaque Settings is rejected by
    // media_key_is_current before any borrowed surface is scanned.
    if (application.renderer.ui_scene) {
        const auto* footer =
            ui_scene_resolve(*application.renderer.ui_scene,
                             ui_scene_find(*application.renderer.ui_scene, UiSemantic::channel_footer));
        if (footer && ui_rect_has_area(footer->rectangle) &&
            matches(application.account_ui->chat_state.self_user_id))
            return true;
    }
    // Call view tiles keep their avatars while the picture is not resident.
    if (const auto* call = application.renderer.call_scene; call && call->active)
        for (u32 i = 0; i < call->tile_count; ++i)
            if (ui_call_scene_tile_visible(*call, i) && matches(call->tiles[i].user_id))
                return true;
    if (const auto* call = application.renderer.call_scene; call && call->active) {
        UiCallTileMedia stage_avatar{};
        for (u32 i = 0; ui_call_scene_stage_avatar(*call, i, &stage_avatar); ++i)
            if (matches(stage_avatar.user_id))
                return true;
    }
    const auto private_matches = [&](u64 channel_id) noexcept {
        u64 people[2]{};
        const u32 count = ui_private_row_group_members(application.account_ui->chat_state, channel_id, people);
        return (count > 0 && matches(people[0])) || (count > 1 && matches(people[1]));
    };
    if (application.shell_scene.view == UiShellView::private_channels &&
        private_matches(application.account_ui->chat_state.selected_channel_id))
        return true;
    const auto& navigation = application.shell_navigation_scene;
    if (navigation.active)
        for (u32 i = 0; i < navigation.count; ++i) {
            const auto& row = navigation.rows[i];
            const auto* icon = ui_widget_manager_layout_node(navigation.widgets, row.icon);
            if (channel_is_private(row.view.type) && icon &&
                ui_rect_has_area(ui_rect_intersection(icon->rectangle, navigation.viewport)) &&
                private_matches(row.view.id))
                return true;
            const auto* layout =
                row.view.voice_count ? shell_navigation_scene_layout_node(navigation, row) : nullptr;
            UiVoiceParticipantRow voice{};
            for (u32 j = 0; layout && ui_voice_participant_row(row.view, layout->rectangle,
                                                               application.account_ui->voice_states, j, &voice);
                 ++j)
                if (ui_rect_has_area(ui_rect_intersection(voice.avatar, navigation.viewport)) &&
                    matches(voice.state->user_id))
                    return true;
        }
    if (application.popup_scene.active)
        for (u32 i = 0; i < application.popup_scene.item_count; ++i) {
            PopupMediaCommand command{};
            if (popup_media_presentation_resolve(application.popup_scene, i, &command) ==
                    PopupMediaPresentationStatus::ready &&
                command.avatar && matches(command.media_id))
                return true;
        }
    if (application.reaction_viewer.active) {
        const auto& viewer = application.reaction_viewer;
        for (u32 i = viewer.first_user; i < viewer.user_count; ++i) {
            if (!ui_rect_has_area(reaction_viewer_user_rectangle(viewer, i)))
                break;
            const auto& user = viewer.users[i];
            const auto hash = reaction_viewer_user_avatar_hash(user);
            const u32 variant = user_avatar_variant(hash);
            if (variant &&
                media_asset_key_equal(
                    key, media_asset_user_avatar(user.id, variant, asset_hash_animated(hash))))
                return true;
        }
    }
    if (!content_media_visible())
        return false;
    const auto& scene = application.message_scene;
    if (scene.active)
        for (u32 i = 0; i < scene.count; ++i) {
            const auto& row = scene.rows[i];
            const auto* avatar = message_scene_layout_node(scene, row.avatar);
            const auto* message = message_store_resolve(application.message_view.store, row.message);
            if (!message)
                continue;
            if (avatar && message_scene_avatar_visible(scene, row) &&
                ui_rect_has_area(ui_rect_intersection(avatar->rectangle, scene.viewport)) &&
                matches(message->author_id))
                return true;
            const auto* reply = message_scene_layout_node(scene, row.reply);
            const auto* rich =
                message_rich_store_find(application.account_ui->message_rich_store, message->id);
            if (reply && rich && rich->message_revision == message->revision &&
                ui_rect_has_area(ui_rect_intersection(reply->rectangle, scene.viewport)) &&
                matches(message_rich_header_user(*rich)))
                return true;
        }
    return false;
}
bool ApplicationMediaPump::channel_icon_key_is_current(MediaAssetKey key) noexcept {
    if (!application.account_ui || key.kind != MediaAssetKind::channel_icon || !media_asset_key_valid(key))
        return false;
    const auto& state = application.account_ui->chat_state;
    const auto matches = [&](u64 channel_id) noexcept {
        const Utf8View custom = channel_id == key.id ? ui_private_row_custom_icon(state, channel_id) : Utf8View{};
        return custom.bytes &&
               media_asset_key_equal(key, media_asset_channel_icon(channel_id, user_avatar_variant(custom),
                                                                   asset_hash_animated(custom)));
    };
    if (application.shell_scene.view == UiShellView::private_channels && matches(state.selected_channel_id))
        return true;
    const auto& navigation = application.shell_navigation_scene;
    if (navigation.active)
        for (u32 i = 0; i < navigation.count; ++i) {
            const auto& row = navigation.rows[i];
            const auto* icon = ui_widget_manager_layout_node(navigation.widgets, row.icon);
            if (row.view.id == key.id && channel_is_private(row.view.type) && icon &&
                ui_rect_has_area(ui_rect_intersection(icon->rectangle, navigation.viewport)) && matches(row.view.id))
                return true;
        }
    return false;
}
bool ApplicationMediaPump::request_pinned_messages_avatars(u64 now_ms, u64 now_seconds, u64 now_us,
                                                           u32* uploads, bool* repaint,
                                                           bool* stop) noexcept {
    *stop = false;
    if (!d3d11_renderer_pinned_messages_visible(application.renderer))
        return true;
    const auto& scene = *application.renderer.pinned_messages_scene;
    for (u32 slot = 0; slot < scene.count && slot < kUiPinnedMessagesVisibleRows; ++slot) {
        PinnedMessagesAvatarMedia media{};
        if (!pinned_messages_avatar_media_resolve(scene, slot, &media))
            continue;
        bool force = false;
        if ((media.key.flags & media_asset_flag_animated) && animation_resident(media.key, now_us, &force))
            continue;
        if (!visibility_only && *uploads >= kMessageMediaUploadBudget) {
            *stop = true;
            return true;
        }
        if (!request_visible_user_avatar_value(media.key.id, media.hash, now_ms, now_seconds, uploads,
                                               repaint, stop, force))
            return false;
        if (*stop)
            return true;
    }
    return true;
}
// Forum post cards borrow author hashes from the visible page (starter author
// or user cache), like pins, including authors absent from the timeline.
bool ApplicationMediaPump::request_forum_avatars(u64 now_ms, u64 now_seconds, u64 now_us, u32* uploads,
                                                 bool* repaint, bool* stop) noexcept {
    *stop = false;
    if (!d3d11_renderer_thread_browser_visible(application.renderer) ||
        !application.renderer.thread_browser_scene->forum)
        return true;
    const auto& scene = *application.renderer.thread_browser_scene;
    for (u32 slot = 0; slot < scene.count && slot < kUiThreadBrowserPageCapacity; ++slot) {
        ForumChannelAvatarMedia media{};
        if (!forum_channel_avatar_media_resolve(scene, slot, &media))
            continue;
        bool force = false;
        if ((media.key.flags & media_asset_flag_animated) && animation_resident(media.key, now_us, &force))
            continue;
        if (!visibility_only && *uploads >= kMessageMediaUploadBudget) {
            *stop = true;
            return true;
        }
        if (!request_visible_user_avatar_value(media.key.id, media.hash, now_ms, now_seconds, uploads,
                                               repaint, stop, force))
            return false;
        if (*stop)
            return true;
    }
    return true;
}
// Starter-image thumbnails of visible forum post cards. The signed URL comes
// from the accepted post page; an expired one goes through the shared refresher.
bool ApplicationMediaPump::request_forum_thumbnails(u64 now_ms, u64 now_seconds, u32* uploads, bool* repaint,
                                                    bool* stop) noexcept {
    *stop = false;
    if (visibility_only || !d3d11_renderer_thread_browser_visible(application.renderer) ||
        !application.renderer.thread_browser_scene->forum || !application.renderer.preview_media_atlas ||
        !application.account.api.media_attachment_request || !application.account.api.media_take)
        return true;
    const auto& scene = *application.renderer.thread_browser_scene;
    for (u32 slot = 0; slot < scene.count && slot < kUiThreadBrowserPageCapacity; ++slot) {
        ForumChannelThumbnailMedia media{};
        if (!forum_channel_thumbnail_resolve(scene, slot, application.renderer.dpi, &media) ||
            media_atlas_find(application.renderer.preview_media_atlas, media.key).ok())
            continue;
        if (message_media_controller_upload_budget_reached(&application.message_media, *uploads) ||
            !message_media_controller_preview_ready(&application.message_media, now_ms)) {
            *stop = true;
            return true;
        }
        const Utf8View resolved = media_url_resolve(&application.media_urls, media.url, now_ms, now_seconds);
        if (!resolved.bytes)
            continue;
        const AccountMediaAttachmentRequest request{resolved, media.key, media.message_id, 1,
                                                    media.proxy.width, media.proxy.height,
                                                    cdn_image_type(media.format),
                                                    {0, 0, 0},
                                                    application.renderer.graphics.device};
        const AccountMediaRequestResult requested = application.account.api.media_attachment_request(
            application.account.instance, &request, now_ms, now_seconds);
        log_message_media_failure("forum_thumbnail_request_failed", requested, media.key);
        if (requested.state == AccountMediaRequestState::queued)
            record_media_request_queued();
        if (requested.state == AccountMediaRequestState::ready) {
            AccountMediaFrame frame{};
            if (!application.account.api.media_take(application.account.instance, &frame))
                return false;
            if (!media_asset_key_equal(frame.key, media.key) || !message_image_frame_is_current(frame))
                continue;
            const auto upload = upload_account_media_frame(frame);
            if (upload == MediaFrameUploadResult::failed)
                return false;
            if (upload == MediaFrameUploadResult::deferred) {
                *stop = true;
                return true;
            }
            ++*uploads;
            record_media_frame_uploaded();
            if (repaint)
                *repaint = true;
        } else if (requested.state == AccountMediaRequestState::busy) {
            *stop = true;
            return true;
        } else if (requested.state != AccountMediaRequestState::queued &&
                   requested.state != AccountMediaRequestState::pending) {
            message_media_controller_observe_retry(&application.message_media, requested.retry_at_ms);
        }
    }
    return true;
}
bool ApplicationMediaPump::request_friends_avatars(u64 now_ms, u64 now_seconds, u64 now_us, u32* uploads,
                                                   bool* repaint, bool* stop) noexcept {
    *stop = false;
    if (!application.account_ui)
        return true;
    const auto* requests = application.renderer.friends_requests_scene;
    const auto* scene = requests && requests->active ? requests : application.renderer.friends_scene;
    if (!scene)
        return true;
    struct Context {
        ApplicationMediaPump* owner;
        u64 now_ms, now_seconds, now_us;
        u32* uploads;
        bool* repaint;
    } context{this, now_ms, now_seconds, now_us, uploads, repaint};
    const FriendsAvatarMediaCallbacks callbacks{
        &context,
        +[](void* opaque, MediaAssetKey key) noexcept {
            return media_atlas_find(static_cast<Context*>(opaque)->owner->application.renderer.media_atlas,
                                    key)
                .ok();
        },
        +[](void* opaque, MediaAssetKey key) noexcept {
            auto& c = *static_cast<Context*>(opaque);
            return animation_timeline_observe_visible(&c.owner->application.animations.timeline, key,
                                                      c.now_us);
        },
        +[](void* opaque, const FriendsAvatarMedia& media, bool force, bool* stopped) noexcept {
            auto& c = *static_cast<Context*>(opaque);
            if (message_media_controller_upload_budget_reached(&c.owner->application.message_media,
                                                               *c.uploads)) {
                *stopped = true;
                return true;
            }
            return c.owner->request_visible_user_avatar_value(
                media.key.id, media.hash, c.now_ms, c.now_seconds, c.uploads, c.repaint, stopped, force);
        }};
    // The dimmed main list uses resident cache entries while this modal owns
    // avatar requests and animation visibility. No hidden rows are fetched.
    const auto visited =
        friends_avatar_media_visit(*scene, application.account_ui->user_store, callbacks);
    *stop = visited.stopped;
    return visited.ok;
}

bool ApplicationMediaPump::request_stream_previews(u64 now_ms, u64 now_seconds, u32* uploads, bool* repaint,
                                                   bool* stop) noexcept {
    *stop = false;
    StreamPreviewMedia previews[4];
    const u32 count = application_stream_preview_media(previews, 4);
    for (u32 i = 0; i < count && !*stop; ++i) {
        const auto key = previews[i].key;
        if (media_atlas_find(application.renderer.media_atlas, key).ok() ||
            (application.renderer.preview_media_atlas &&
             media_atlas_find(application.renderer.preview_media_atlas, key).ok()))
            continue;
        if (visibility_only)
            continue;
        const AccountMediaAttachmentRequest request{previews[i].url,
                                                    key,
                                                    0,
                                                    0,
                                                    0,
                                                    0,
                                                    cdn_image_type(media_asset_embed_format(key)),
                                                    {0, 0, 0},
                                                    application.renderer.graphics.device};
        const auto requested = application.account.api.media_attachment_request(application.account.instance,
                                                                                &request, now_ms, now_seconds);
        if (requested.state == AccountMediaRequestState::queued)
            record_media_request_queued();
        if (requested.state == AccountMediaRequestState::ready) {
            AccountMediaFrame frame{};
            if (!application.account.api.media_take(application.account.instance, &frame) ||
                !media_asset_key_equal(frame.key, key))
                return false;
            const auto upload = upload_account_media_frame(frame);
            if (upload == MediaFrameUploadResult::failed)
                return false;
            if (upload == MediaFrameUploadResult::deferred) {
                *stop = true;
                return true;
            }
            ++*uploads;
            record_media_frame_uploaded();
            if (repaint)
                *repaint = true;
            *stop = message_media_controller_upload_budget_reached(&application.message_media, *uploads);
        }
        message_media_controller_observe_retry(&application.message_media, requested.retry_at_ms);
    }
    // The server tag popout's icon, tag badge and banner, fetched by URL.
    GuildCardMedia cards[3];
    const u32 card_count = application_guild_card_media(cards, 3);
    for (u32 i = 0; i < card_count && !*stop; ++i) {
        ProfileMedia media{};
        media.key = cards[i].key;
        const u32 bytes = minimum<u32>(cards[i].url.bytes, sizeof(media.url.data));
        copy_bytes(media.url.data, cards[i].url.data, bytes);
        media.url.bytes = static_cast<u16>(bytes);
        AccountMediaRequestResult failure{};
        failure.state = AccountMediaRequestState::ready; // a resident image makes no request
        if (!request_profile_media(media, now_ms, now_seconds, now_ms * 1000, uploads, repaint, stop, false, &failure))
            return false;
        application_guild_card_media_failed(cards[i].key, failure);
    }
    return true;
}

bool ApplicationMediaPump::request_profile_media(const ProfileMedia& media, u64 now_ms, u64 now_seconds,
                                                 u64 now_us, u32* uploads, bool* repaint, bool* stop,
                                                 bool play, AccountMediaRequestResult* failure) noexcept {
    *stop = false;
    const auto key = media.key;
    const bool resident = media_atlas_find(application.renderer.media_atlas, key).ok();
    if (media.playback_epoch && resident)
        animation_timeline_restart(&application.animations.timeline, key, media.playback_epoch, now_us);
    if (!play && resident)
        return true;
    if (play && (key.flags & media_asset_flag_animated)) {
        if (animation_timeline_observe_visible(&application.animations.timeline, key, now_us) && resident)
            return true;
    } else if (resident)
        return true;
    if (visibility_only)
        return true;
    const AccountMediaAttachmentRequest request{media.url.view(),
                                                key,
                                                0,
                                                0,
                                                0,
                                                0,
                                                cdn_image_type(media_asset_embed_format(key)),
                                                {0, 0, 0},
                                                application.renderer.graphics.device};
    const auto requested =
        key.kind == MediaAssetKind::custom_emoji
            ? application.account.api.media_request(application.account.instance, key, now_ms, now_seconds)
            : application.account.api.media_attachment_request(application.account.instance, &request,
                                                               now_ms, now_seconds);
    if (failure)
        *failure = requested;
    if (requested.state == AccountMediaRequestState::queued)
        record_media_request_queued();
    if (requested.state == AccountMediaRequestState::busy) {
        // Admission depends on media type: still-image lanes can be full while
        // the animation reserve is idle. Keep scanning this bounded surface so
        // one waiting image does not prevent independent work from being queued.
        return true;
    }
    if (requested.state == AccountMediaRequestState::ready) {
        AccountMediaFrame frame{};
        if (!application.account.api.media_take(application.account.instance, &frame) ||
            !media_asset_key_equal(frame.key, key))
            return false;
        const auto upload = upload_account_media_frame(frame);
        if (upload == MediaFrameUploadResult::failed)
            return false;
        if (upload == MediaFrameUploadResult::deferred) {
            *stop = true;
            return true;
        }
        ++*uploads;
        record_media_frame_uploaded();
        if (repaint)
            *repaint = true;
        if (play)
            animation_timeline_observe_visible(&application.animations.timeline, key, now_us);
        *stop = message_media_controller_upload_budget_reached(&application.message_media, *uploads);
    }
    message_media_controller_observe_retry(&application.message_media, requested.retry_at_ms);
    return true;
}

__declspec(noinline) bool ApplicationMediaPump::request_visible_static_media(
    MediaAssetKey key, u64 now_ms, u64 now_seconds, u32* uploads, bool* repaint, bool* stop_scan,
    bool force_request, bool continue_on_busy) noexcept {
    *stop_scan = false;
    if (visibility_only)
        return true;
    if (!force_request && media_atlas_find(application.renderer.media_atlas, key).ok()) {
        return true;
    }
    return settle_media_request(
        application.account.api.media_request(application.account.instance, key, now_ms, now_seconds), key,
        uploads, repaint, stop_scan, !continue_on_busy);
}

__declspec(noinline) bool ApplicationMediaPump::settle_media_request(
    const AccountMediaRequestResult& requested, MediaAssetKey key, u32* uploads, bool* repaint,
    bool* stop_scan, bool stop_on_busy) noexcept {
    if (requested.state == AccountMediaRequestState::queued) {
        record_media_request_queued();
    }
    if (requested.state == AccountMediaRequestState::ready) {
        AccountMediaFrame frame{};
        if (!application.account.api.media_take(application.account.instance, &frame) ||
            !media_asset_key_equal(frame.key, key)) {
            return false;
        }
        const MediaFrameUploadResult upload = upload_account_media_frame(frame);
        if (upload == MediaFrameUploadResult::failed) {
            return false;
        }
        if (upload == MediaFrameUploadResult::deferred) {
            *stop_scan = true;
            return true;
        }
        ++*uploads;
        record_media_frame_uploaded();
        if (repaint) {
            *repaint = true;
        }
        if (message_media_controller_upload_budget_reached(&application.message_media, *uploads)) {
            *stop_scan = true;
        }
        return true;
    }
    if (requested.state == AccountMediaRequestState::queued ||
        requested.state == AccountMediaRequestState::pending) {
        return true;
    }
    if (requested.state == AccountMediaRequestState::busy) {
        *stop_scan = stop_on_busy;
        return true;
    }
    message_media_controller_observe_retry(&application.message_media, requested.retry_at_ms);
    return true;
}

__declspec(noinline) bool ApplicationMediaPump::request_visible_guild_icon(const GuildRecord& guild,
                                                                           u64 now_ms, u64 now_seconds,
                                                                           u32* uploads, bool* repaint,
                                                                           bool* stop_scan,
                                                                           bool force_request,
                                                                           bool animate) noexcept {
    if (!application.account_ui || !application.account.api.media_guild_icon_request) {
        return false;
    }
    *stop_scan = false;
    if (visibility_only)
        return true;
    const Utf8View icon_hash = chat_state_store_guild_icon_hash(guild);
    if (icon_hash.bytes == 0 || guild.icon_variant == 0) {
        return true;
    }
    const bool animated = animate && asset_hash_animated(icon_hash);
    const MediaAssetKey key = media_asset_guild_icon(guild.id, guild.icon_variant, animated);
    const auto resident = media_atlas_find(application.renderer.media_atlas, key);
    if (resident.ok() && (!force_request || !media_atlas_decoded_animation(
                                                *application.renderer.media_atlas, resident.handle))) {
        return true;
    }
    const AccountMediaGuildIconRequest request{icon_hash, key, {0, 0}};
    return settle_media_request(application.account.api.media_guild_icon_request(
                                    application.account.instance, &request, now_ms, now_seconds),
                                key, uploads, repaint, stop_scan, true);
}

__declspec(noinline) bool ApplicationMediaPump::request_visible_user_avatar_value(
    u64 user_id, Utf8View avatar_hash, u64 now_ms, u64 now_seconds, u32* uploads, bool* repaint,
    bool* stop_scan, bool force_request) noexcept {
    *stop_scan = false;
    const u32 variant = user_avatar_variant(avatar_hash);
    if (user_id == 0 || avatar_hash.bytes == 0 || variant == 0)
        return application.account_ui && application.account.api.media_guild_icon_request;
    return request_visible_hashed_identity(
        media_asset_user_avatar(user_id, variant, asset_hash_animated(avatar_hash)), avatar_hash, now_ms,
        now_seconds, uploads, repaint, stop_scan, force_request);
}

u32 ApplicationMediaPump::identity_cdn_size() const noexcept {
    return cdn_image_size(kCdnIdentityDisplayDips, application.renderer.dpi);
}

__declspec(noinline) bool ApplicationMediaPump::request_visible_hashed_identity(
    MediaAssetKey key, Utf8View hash, u64 now_ms, u64 now_seconds, u32* uploads, bool* repaint,
    bool* stop_scan, bool force_request) noexcept {
    if (!application.account_ui || !application.account.api.media_guild_icon_request) {
        return false;
    }
    *stop_scan = false;
    if (visibility_only || !media_asset_key_valid(key) || !hash.bytes)
        return true;
    // A grouped or clipped author can be collected without a visible avatar.
    // Reject that owner before queueing the decode that the take path discards.
    if (!media_key_is_current(key))
        return true;
    const auto resident = media_atlas_find(application.renderer.media_atlas, key);
    if (resident.ok() && (!force_request || !media_atlas_decoded_animation(
                                                *application.renderer.media_atlas, resident.handle))) {
        return true;
    }
    // The account ABI keeps this historical name, but the request is the
    // generic hash-addressed CDN asset path. The asset key selects
    // /avatars or /channel-icons rather than /icons in the account module;
    // reserved[0] asks for the CDN size covering the displayed extent at this DPI.
    const AccountMediaGuildIconRequest request{hash, key, {identity_cdn_size(), 0}};
    return settle_media_request(application.account.api.media_guild_icon_request(
                                    application.account.instance, &request, now_ms, now_seconds),
                                key, uploads, repaint, stop_scan, true);
}

__declspec(noinline) bool ApplicationMediaPump::request_visible_user_avatar(const UserRecord& user,
                                                                            u64 now_ms, u64 now_seconds,
                                                                            u32* uploads, bool* repaint,
                                                                            bool* stop_scan,
                                                                            bool force_request) noexcept {
    return request_visible_user_avatar_value(user.id, user_store_avatar_hash(user), now_ms,
                                             now_seconds, uploads, repaint, stop_scan, force_request);
}

__declspec(noinline) void ApplicationMediaPump::log_message_media_failure(
    const char* item, const AccountMediaRequestResult& failure, MediaAssetKey key) noexcept {
    if (!item || (failure.state != AccountMediaRequestState::failed &&
                  failure.state != AccountMediaRequestState::invalid &&
                  failure.state != AccountMediaRequestState::unsupported)) {
        return;
    }
    log_write(LogLevel::warning, "media", item);
    log_u64(LogLevel::warning, "media", "asset_kind", static_cast<u64>(key.kind));
    log_u64(LogLevel::warning, "media", "asset_id", key.id);
    log_u64(LogLevel::warning, "media", "request_state", static_cast<u64>(failure.state));
    log_u64(LogLevel::warning, "media", "request_error", static_cast<u64>(failure.error));
    log_u64(LogLevel::warning, "media", "client_error", static_cast<u64>(failure.client_error));
    log_u64(LogLevel::warning, "media", "http_error", static_cast<u64>(failure.http_error));
    log_u64(LogLevel::warning, "media", "attachment_url_error",
            static_cast<u64>(failure.attachment_url_error));
    if (failure.attachment_query_error != AttachmentQueryError::none)
        log_u64(LogLevel::warning, "media", "attachment_query_error",
                static_cast<u64>(failure.attachment_query_error));
    if (failure.video_failure != AccountMediaVideoFailure::none)
        log_u64(LogLevel::warning, "media", "video_decode_failure",
                static_cast<u64>(failure.video_failure));
    log_u64(LogLevel::warning, "media", "media_url_error", static_cast<u64>(failure.media_url_error));
    log_u64(LogLevel::warning, "media", "target_error", static_cast<u64>(failure.target_error));
    log_u64(LogLevel::warning, "media", "png_error", static_cast<u64>(failure.png_error));
    log_u64(LogLevel::warning, "media", "webp_error", static_cast<u64>(failure.webp_error));
    log_u64(LogLevel::warning, "media", "jpeg_error", static_cast<u64>(failure.jpeg_error));
    if (failure.gif_error != GifError::none)
        log_u64(LogLevel::warning, "media", "gif_error", static_cast<u64>(failure.gif_error));
    if (failure.animated_webp_error != AnimatedWebpError::none)
        log_u64(LogLevel::warning, "media", "animated_webp_error",
                static_cast<u64>(failure.animated_webp_error));
    if (failure.lossy_webp_error != LossyWebpError::none)
        log_u64(LogLevel::warning, "media", "lossy_webp_error", static_cast<u64>(failure.lossy_webp_error));
}

__declspec(noinline) void ApplicationMediaPump::record_media_request_queued() noexcept {
    message_media_controller_record_request(&application.message_media);
}

__declspec(noinline) void ApplicationMediaPump::record_media_animation_advance(MediaAssetKind kind) noexcept {
    message_media_controller_record_animation_advance(&application.message_media, kind);
}

__declspec(noinline) void ApplicationMediaPump::record_media_frame_uploaded() noexcept {
    message_media_controller_record_upload(&application.message_media);
    if (application.load_trace.channel_started && !application.load_trace.media_uploaded) {
        application.load_trace.media_uploaded = true;
        application_trace_load_stage("first_media_uploaded");
    }
}

__declspec(noinline) void ApplicationMediaPump::flush_media_success_diagnostics(u64 now_ms) noexcept {
    const MessageMediaControllerDiagnostics diagnostics =
        message_media_controller_take_diagnostics(&application.message_media, now_ms);
    if (!diagnostics.ready) {
        return;
    }
    if (diagnostics.requests_queued != 0) {
        log_u64(LogLevel::info, "media", "requests_queued_5s", diagnostics.requests_queued);
    }
    if (diagnostics.frames_uploaded != 0) {
        log_u64(LogLevel::info, "media", "frames_uploaded_5s", diagnostics.frames_uploaded);
    }
    // Playback of visible animations, apart from fetches: the advance count
    // and the media kind that advanced most (MediaAssetKind), never a URL.
    if (diagnostics.animation_advances != 0) {
        log_u64(LogLevel::info, "media", "animation_advances_5s", diagnostics.animation_advances);
        log_u64(LogLevel::info, "media", "animation_advance_kind", static_cast<u64>(diagnostics.animation_kind));
        log_u64(LogLevel::info, "media", "animation_advance_kind_5s", diagnostics.animation_kind_advances);
    }
    // Opt-in interval diagnostics share the existing five-second media gate.
    // No extra wake, readback, heap allocation, or per-frame log traffic.
    if (application.renderer.gpu_timer.enabled) {
        const D3D11Renderer& r = application.renderer;
        static u64 previous_ms = 0;
        log_u64(LogLevel::info, "render", "interval_ms", previous_ms ? now_ms - previous_ms : 0);
        previous_ms = now_ms;
#define LOG_RENDER_DELTA(name, value)                                                                      \
    do {                                                                                                   \
        static u64 previous = 0;                                                                           \
        const u64 current = (value);                                                                       \
        log_u64(LogLevel::info, "render", name, current >= previous ? current - previous : current);       \
        previous = current;                                                                                \
    } while (false)
        LOG_RENDER_DELTA("interval_full_draws", r.full_present_draws);
        LOG_RENDER_DELTA("interval_partial_draws", r.partial_present_draws);
        LOG_RENDER_DELTA("interval_idle_presents", r.idle_begin_frame_presents);
        LOG_RENDER_DELTA("interval_text_builds", r.content_text_retention.builds);
        LOG_RENDER_DELTA("interval_layout_builds", r.paint_stream_builds);
        LOG_RENDER_DELTA("interval_video_frames", r.media_gpu.video.frames);
        LOG_RENDER_DELTA("interval_glyph_uploads", r.text_instance_uploads);
        LOG_RENDER_DELTA("interval_partial_pixels", r.partial_present_pixels);
        LOG_RENDER_DELTA("interval_draw_gpu_us", r.gpu_timer.total_microseconds);
        LOG_RENDER_DELTA("interval_draw_gpu_samples", r.gpu_timer.samples);
#undef LOG_RENDER_DELTA
    }
}

__declspec(noinline) CdnImageType ApplicationMediaPump::cdn_image_type(MediaAttachmentFormat format) noexcept {
    return format == MediaAttachmentFormat::webp   ? CdnImageType::webp
           : format == MediaAttachmentFormat::jpeg ? CdnImageType::jpeg
           : format == MediaAttachmentFormat::mp4  ? CdnImageType::mp4
           : format == MediaAttachmentFormat::gif  ? CdnImageType::gif
                                                   : CdnImageType::png;
}

__declspec(noinline) bool ApplicationMediaPump::message_image_frame_is_current(
    const AccountMediaFrame& frame) noexcept {
    if (frame.key.kind == MediaAssetKind::catalog_preview)
        return frame.source_kind == AccountMediaSourceKind::catalog_preview && !frame.source_message_id &&
               !frame.source_message_revision && frame.source_catalog_scope &&
               frame.source_catalog_generation &&
               message_image_source_is_current(frame.key, frame.source_catalog_scope,
                                               frame.source_catalog_generation);
    if (frame.source_kind == AccountMediaSourceKind::catalog_preview || frame.source_catalog_scope ||
        frame.source_catalog_generation)
        return false;
    return message_image_source_is_current(frame.key, frame.source_message_id,
                                           frame.source_message_revision);
}

bool ApplicationMediaPump::message_image_source_is_current(MediaAssetKey key, u64 source_message_id,
                                                           u32 source_message_revision) noexcept {
    if (!media_key_is_current(key))
        return false;
    // Forum thumbnails come from a post page, not the message store.
    if (forum_thumbnail_key_current(application, key))
        return true;
    if (key.kind == MediaAssetKind::catalog_preview)
        return application_media_picker_preview_current(key, source_message_id, source_message_revision);
    if (media_asset_search(key)) {
        if (media_asset_search_revision(key) != source_message_revision)
            return false;
        for (u32 scope = 0; scope < 2; ++scope) {
            if (media_asset_search_snapshot(key) != (scope != 0))
                continue;
            ApplicationSearchSourceView source{};
            if (!application_search_source_resolve(source_message_id, source_message_revision, scope != 0,
                                                   &source) ||
                !source.message || !source.rich_store || !source.rich ||
                source.message->revision != source_message_revision ||
                source.rich->message_revision != source_message_revision)
                continue;
            if (key.kind == MediaAssetKind::attachment) {
                for (u32 i = 0; i < source.rich->attachment_count; ++i) {
                    const auto& item = source.rich->attachments[i];
                    const auto audio_format =
                        message_rich_attachment_audio_format(*source.rich_store, item);
                    const auto format =
                        media_attachment_format_audio_supported(media_asset_attachment_format(key))
                            ? audio_format
                            : message_media_supported_attachment_format(*source.rich_store, item);
                    if (item.id == key.id && format != MediaAttachmentFormat::unknown &&
                        media_asset_key_equal(key, media_asset_search_attachment(
                                                       item.id, format, media_asset_resolution_tier(key),
                                                       source_message_revision, scope != 0)))
                        return true;
                }
            } else {
                const u32 index = media_asset_embed_index(key);
                MessageRichEmbedMediaSelection selected{};
                if (key.id == source_message_id && index < source.rich->embed_count &&
                    message_rich_embed_resource_select(*source.rich_store, source.rich->embeds[index],
                                                       media_asset_embed_resource(key), &selected) &&
                    media_asset_key_equal(
                        key, media_asset_search_embed(source_message_id, source_message_revision,
                                                      static_cast<u8>(index), selected.resource,
                                                      selected.format, selected.animated,
                                                      media_asset_resolution_tier(key), scope != 0)))
                    return true;
            }
        }
        return false;
    }
    if ((key.kind != MediaAssetKind::attachment && key.kind != MediaAssetKind::embed) ||
        !application.account_ui || source_message_id == 0 || source_message_revision == 0) {
        return key.kind != MediaAssetKind::attachment && key.kind != MediaAssetKind::embed;
    }
    const MessageStoreResult found = message_store_find(application.message_view.store, source_message_id);
    const MessageRecord* message =
        found.ok() ? message_store_resolve(application.message_view.store, found.handle) : nullptr;
    if (!message || !application_channel_readable(application, message->channel_id) ||
        message->revision != source_message_revision ||
        message->channel_id != application.account_ui->active_channel_id) {
        return false;
    }
    const MessageRichRecord* rich =
        media_asset_embed_snapshot(key)
            ? message_rich_store_snapshot(application.account_ui->message_rich_store, message->id)
            : message_rich_store_find(application.account_ui->message_rich_store, message->id);
    if (!rich || rich->message_revision != message->revision) {
        return false;
    }
    if (key.kind == MediaAssetKind::embed) {
        const u8 embed_index = media_asset_embed_index(key);
        if (key.id != message->id || key.variant != message->revision || embed_index >= rich->embed_count) {
            return false;
        }
        MessageRichEmbedMediaSelection selected{};
        if (!message_rich_embed_resource_select(application.account_ui->message_rich_store,
                                                rich->embeds[embed_index], media_asset_embed_resource(key),
                                                &selected)) {
            return false;
        }
        const MediaAssetKey expected = media_asset_embed(
            message->id, message->revision, embed_index, selected.resource, selected.format,
            selected.animated, media_asset_resolution_tier(key), rich->forwarded_snapshot_child);
        return media_asset_key_equal(key, expected);
    }
    for (u32 content_scope = 0; content_scope < 2; ++content_scope) {
        if (content_scope)
            rich = message_rich_store_snapshot(application.account_ui->message_rich_store, message->id);
        for (u32 index = 0; rich && index < rich->attachment_count; ++index) {
            const MessageRichAttachment& attachment = rich->attachments[index];
            const auto audio_format = message_rich_attachment_audio_format(
                application.account_ui->message_rich_store, attachment);
            const MediaAttachmentFormat format =
                media_attachment_format_audio_supported(media_asset_attachment_format(key))
                    ? audio_format
                    : message_media_supported_attachment_format(application.account_ui->message_rich_store,
                                                                attachment);
            if (attachment.id == key.id && media_asset_attachment_format(key) == format &&
                format != MediaAttachmentFormat::unknown) {
                return true;
            }
        }
    }
    return false;
}

__declspec(noinline) bool ApplicationMediaPump::embed_media_dimensions_supported(
    const MessageRichEmbedMediaSelection& selected) noexcept {
    if (!selected.media) {
        return false;
    }
    return selected.format != MediaAttachmentFormat::unknown && selected.media->width != 0 &&
           selected.media->height != 0;
}

__declspec(noinline) bool ApplicationMediaPump::update_media_display_dimensions(
    const AccountMediaFrame& frame, bool* changed) noexcept {
    if (media_asset_search(frame.key)) {
        if (!changed)
            return false;
        *changed = false;
        if (!message_image_frame_is_current(frame))
            return true;
        ApplicationSearchSourceView source{};
        if (!application_search_source_resolve(frame.source_message_id, frame.source_message_revision,
                                               media_asset_search_snapshot(frame.key), &source))
            return true;
        const bool applied = message_media_apply_frame_dimensions(source.rich_store, frame, changed);
        result.text_changed = result.text_changed || *changed;
        if (applied && *changed)
            application_search_media_dimensions_changed();
        return applied;
    }
    if (!message_media_apply_frame_dimensions(
            application.account_ui ? &application.account_ui->message_rich_store : nullptr, frame, changed))
        return false;
    result.text_changed = result.text_changed || *changed;
    return true;
}
__declspec(noinline) bool ApplicationMediaPump::request_visible_attachment(
    const MessageRecord& message, const MessageRichAttachment& attachment, float preview_width,
    float preview_height, u64 now_ms, u64 now_seconds, u32* uploads, bool* repaint, bool* stop_scan,
    const MessageRichStore* search_store, bool search_snapshot) noexcept {
    if (!application.account_ui || !application.account.api.media_attachment_request) {
        return false;
    }
    *stop_scan = false;
    const auto& store = search_store ? *search_store : application.account_ui->message_rich_store;
    if (!application_channel_readable(application, message.channel_id))
        return true;
    bool snapshot = search_snapshot;
    if (!search_store)
        if (const auto* child = message_rich_store_snapshot(store, message.id))
            for (u32 index = 0; index < child->attachment_count; ++index)
                if (&child->attachments[index] == &attachment)
                    snapshot = true;
    const auto* rich = snapshot ? message_rich_store_snapshot(store, message.id)
                                : message_rich_store_find(store, message.id);
    u32 attachment_index = 0;
    if (rich)
        for (u32 index = 0; index < rich->attachment_count; ++index)
            if (rich->attachments[index].id == attachment.id) {
                attachment_index = index;
                break;
            }
    if (!preview_media_allowed(application, message, store, search_store != nullptr,
                               MessageMediaSource::attachment, attachment_index, snapshot,
                               message_rich_attachment_spoiler(store, attachment),
                               message_rich_attachment_explicit(attachment)))
        return true;
    const MediaAttachmentFormat format = message_media_supported_attachment_format(store, attachment);
    if (format == MediaAttachmentFormat::unknown) {
        return true;
    }
    const MediaPresentationRequest proxy =
        media_presentation_request(attachment.width, attachment.height, preview_width, preview_height,
                                   application.renderer.dpi, media_preview_extent_cap(format));
    const MediaAssetKey key = search_store
                                  ? media_asset_search_attachment(attachment.id, format, proxy.resolution,
                                                                  message.revision, search_snapshot)
                                  : media_asset_attachment(attachment.id, format, proxy.resolution);
    const auto resident_media = prefetch_only
                                    ? media_atlas_peek(application.renderer.preview_media_atlas, key)
                                    : media_atlas_find(application.renderer.preview_media_atlas, key);
    const bool resident = resident_media.ok();
    if (prefetch_only && resident)
        return true;
    if (format == MediaAttachmentFormat::mp4 && resident)
        return true;
    // The decoder may discover APNG/animated WebP without an animation hint
    // in attachment metadata. Its published timeline still owns visibility.
    if (!prefetch_only && ((key.flags & media_asset_flag_animated) != 0 ||
                           preview_decoder_animation(application, resident_media, key))) {
        retire_stale_message_animation(key, message.id, message.revision);
        const bool observed = observe_message_animation(application, key, message.id);
        if (observed && resident)
            return true;
    } else if (resident) {
        return true;
    }
    if (visibility_only)
        return true;
    if (!message_media_controller_preview_ready(&application.message_media, now_ms))
        return true;
    const Utf8View signed_url = message_rich_store_string(store, attachment.url);
    if (signed_url.bytes == 0) {
        return true;
    }
    const Utf8View resolved = media_url_resolve(&application.media_urls, signed_url, now_ms, now_seconds);
    if (resolved.bytes == 0) {
        result.history_requested =
            result.history_requested || media_url_next(&application.media_urls, now_ms) != nullptr;
        return true;
    }
    const AccountMediaAttachmentRequest request{resolved,
                                                key,
                                                message.id,
                                                message.revision,
                                                proxy.width,
                                                proxy.height,
                                                cdn_image_type(format),
                                                {0, 0, 0},
                                                application.renderer.graphics.device};
    const AccountMediaRequestResult requested = application.account.api.media_attachment_request(
        application.account.instance, &request, now_ms, now_seconds);
    log_message_media_failure("attachment_request_failed", requested, key);
    if (requested.state == AccountMediaRequestState::refresh_required) {
        if (search_store) {
            message_media_controller_observe_retry(&application.message_media, requested.retry_at_ms);
            return true;
        }
        result.history_requested =
            message_history_attachment_queue(&application.attachment_refresh, message.channel_id,
                                             message.id, message.revision, attachment.id) ||
            result.history_requested;
        *stop_scan = true;
        return true;
    }
    if (requested.state == AccountMediaRequestState::failed &&
        requested.error == AccountMediaError::target) {
        if (search_store) {
            message_media_controller_observe_retry(&application.message_media, requested.retry_at_ms);
            return true;
        }
        log_u64(LogLevel::warning, "media", "attachment_url_refresh_error",
                static_cast<u64>(requested.attachment_url_error));
        result.history_requested =
            message_history_attachment_queue(&application.attachment_refresh, message.channel_id,
                                             message.id, message.revision, attachment.id) ||
            result.history_requested;
        *stop_scan = true;
        return true;
    }
    return settle_message_media(requested, key, message.id, uploads, repaint, stop_scan);
}

__declspec(noinline) bool ApplicationMediaPump::settle_message_media(
    const AccountMediaRequestResult& requested, MediaAssetKey key, u64 message_id, u32* uploads,
    bool* repaint, bool* stop_scan) noexcept {
    if (application.image_viewer.scene.active &&
        (requested.state == AccountMediaRequestState::unsupported ||
         (requested.state == AccountMediaRequestState::failed && requested.error != AccountMediaError::target)))
        application_image_viewer_request_failed(key);
    if (requested.state == AccountMediaRequestState::queued) {
        record_media_request_queued();
        // At most one speculative miss per pump; never fill every free lane
        // ahead of content that becomes visible on the next scroll update.
        if (prefetch_only)
            *stop_scan = true;
    }
    if (requested.state == AccountMediaRequestState::ready) {
        AccountMediaFrame frame{};
        bool geometry_changed = false;
        if (!application.account.api.media_take(application.account.instance, &frame) ||
            !media_asset_key_equal(frame.key, key) || !message_image_frame_is_current(frame)) {
            return false;
        }
        const MediaFrameUploadResult upload = upload_account_media_frame(frame);
        if (upload == MediaFrameUploadResult::failed) {
            return false;
        }
        if (upload == MediaFrameUploadResult::deferred) {
            *stop_scan = true;
            return true;
        }
        if (!update_media_display_dimensions(frame, &geometry_changed)) {
            return false;
        }
        if (!prefetch_only)
            observe_message_animation(application, key, message_id);
        ++*uploads;
        record_media_frame_uploaded();
        if (repaint) {
            *repaint = true;
        }
        if (message_media_controller_upload_budget_reached(&application.message_media, *uploads)) {
            *stop_scan = true;
        }
        if (geometry_changed) {
            message_media_controller_defer_scan(&application.message_media);
            *stop_scan = true;
        }
        return true;
    }
    if (requested.state == AccountMediaRequestState::queued ||
        requested.state == AccountMediaRequestState::pending) {
        return true;
    }
    if (requested.state == AccountMediaRequestState::busy) {
        *stop_scan = true;
        return true;
    }
    message_media_controller_observe_retry(&application.message_media, requested.retry_at_ms);
    return true;
}

__declspec(noinline) bool ApplicationMediaPump::request_visible_embed_media(
    const MessageRecord& message, const MessageRichRecord& rich, u32 embed_index, float preview_width,
    float preview_height, u64 now_ms, u64 now_seconds, u32* uploads, bool* repaint, bool* stop_scan,
    bool thumbnail, const MessageRichStore* search_store) noexcept {
    if (!application.account_ui || !application.account.api.media_attachment_request ||
        embed_index >= rich.embed_count) {
        return false;
    }
    *stop_scan = false;
    MessageRichEmbedMediaSelection selected{};
    if (!application_channel_readable(application, message.channel_id))
        return true;
    const auto& store = search_store ? *search_store : application.account_ui->message_rich_store;
    if (!preview_media_allowed(application, message, store, search_store != nullptr,
                               MessageMediaSource::embed, embed_index, rich.forwarded_snapshot_child, false,
                               message_rich_embed_explicit(rich.embeds[embed_index])))
        return true;
    MessageRichEmbedGalleryPlace place{};
    const bool tile = !thumbnail && message_rich_embed_gallery_place(store, rich, embed_index, &place);
    if (!(thumbnail ? message_rich_embed_resource_select(store, rich.embeds[embed_index],
                                                         MediaEmbedResource::thumbnail, &selected)
          : tile    ? message_rich_embed_gallery_select(store, rich.embeds[embed_index], &selected)
                    : message_rich_embed_media_select(store, rich.embeds[embed_index], &selected)) ||
        !embed_media_dimensions_supported(selected)) {
        return true;
    }
    const MediaPresentationRequest proxy = media_presentation_request(
        selected.media->width, selected.media->height, preview_width, preview_height,
        application.renderer.dpi, media_preview_extent_cap(selected.format));
    const MediaAssetKey key =
        search_store ? media_asset_search_embed(message.id, message.revision, static_cast<u8>(embed_index),
                                                selected.resource, selected.format, selected.animated,
                                                proxy.resolution, rich.forwarded_snapshot_child)
                     : media_asset_embed(message.id, message.revision, static_cast<u8>(embed_index),
                                         selected.resource, selected.format, selected.animated,
                                         proxy.resolution, rich.forwarded_snapshot_child);
    const auto resident_media = prefetch_only
                                    ? media_atlas_peek(application.renderer.preview_media_atlas, key)
                                    : media_atlas_find(application.renderer.preview_media_atlas, key);
    const bool resident = resident_media.ok();
    if (prefetch_only && resident)
        return true;
    if (selected.format == MediaAttachmentFormat::mp4 &&
        rich.embeds[embed_index].type != GatewayMessageEmbedType::gifv && resident)
        return true;
    // Preserve a decoder-discovered animation even when the rich descriptor
    // did not carry an animation hint; the stable media key stays unchanged.
    if (!prefetch_only &&
        (selected.animated || preview_decoder_animation(application, resident_media, key))) {
        retire_stale_message_animation(key, message.id, message.revision);
        // A resident first frame is not sufficient for animated media. Mark
        // the timeline entry during every visibility epoch so end_visibility
        // does not pause the retained decoder after its initial upload.
        const bool observed = observe_message_animation(application, key, message.id);
        if (observed && resident) {
            return true;
        }
    } else if (resident) {
        return true;
    }
    if (visibility_only)
        return true;
    if (!message_media_controller_preview_ready(&application.message_media, now_ms))
        return true;
    Utf8View request_source = selected.source;
    u8 validated_target[kCdnMaximumTargetBytes]{};
    if (!parse_media_url(request_source, Slice<u8>{validated_target, sizeof(validated_target)})
             .ok() &&
        selected.fallback_source.bytes != 0 &&
        parse_media_url(selected.fallback_source,
                        Slice<u8>{validated_target, sizeof(validated_target)})
            .ok()) {
        request_source = selected.fallback_source;
    }
    const Utf8View resolved =
        media_url_resolve(&application.media_urls, request_source, now_ms, now_seconds);
    if (resolved.bytes == 0) {
        result.history_requested =
            result.history_requested || media_url_next(&application.media_urls, now_ms) != nullptr;
        return true;
    }
    const AccountMediaAttachmentRequest request{resolved,
                                                key,
                                                message.id,
                                                message.revision,
                                                proxy.width,
                                                proxy.height,
                                                cdn_image_type(selected.format),
                                                {0, 0, 0},
                                                application.renderer.graphics.device};
    const AccountMediaRequestResult requested = application.account.api.media_attachment_request(
        application.account.instance, &request, now_ms, now_seconds);
    log_message_media_failure("embed_media_request_failed", requested, key);
    return settle_message_media(requested, key, message.id, uploads, repaint, stop_scan);
}

bool ApplicationMediaPump::request_search_previews(u64 now_ms, u64 now_seconds, u32* uploads, bool* repaint,
                                                   bool* stop_scan) noexcept {
    *stop_scan = false;
    if (!search_media_visible() || prefetch_only)
        return true;
    const auto& search = *application.renderer.search_scene;
    // Media grid tiles are sized independently of the hidden message projection.
    for (u32 slot = 0, tiles = ui_search_scene_visible_tile_count(search); slot < tiles; ++slot) {
        UiSearchTarget target{};
        ApplicationSearchSourceTicket ticket{};
        ApplicationSearchSourceView source{};
        UiRect rectangle{}, clip{};
        if (!ui_search_scene_visible_tile(search, slot, &target) ||
            !application_search_source_ticket(target.source_index, &ticket) ||
            !application_search_source_resolve(ticket, target.snapshot, &source) || !source.message ||
            !source.rich || !source.rich_store ||
            !ui_search_scene_rich_preview_geometry(search, target, &rectangle, &clip))
            continue;
        bool stop = false;
        if (target.rich_kind == MessageSceneRichItemKind::attachment) {
            if (target.rich_index < source.rich->attachment_count &&
                !request_visible_attachment(*source.message, source.rich->attachments[target.rich_index],
                                            ui_rect_width(rectangle), ui_rect_height(rectangle), now_ms,
                                            now_seconds, uploads, repaint, &stop, source.rich_store,
                                            target.snapshot))
                return false;
        } else if (!request_visible_embed_media(*source.message, *source.rich, target.rich_index,
                                                ui_rect_width(rectangle), ui_rect_height(rectangle), now_ms,
                                                now_seconds, uploads, repaint, &stop,
                                                target.resource == MediaEmbedResource::thumbnail,
                                                source.rich_store))
            return false;
        if (stop) {
            *stop_scan = true;
            return true;
        }
    }
    const auto* scene = ui_search_scene_message_scene(search);
    if (!scene)
        return true;
    for (u32 visible = 0; visible < scene->count && visible < kUiSearchVisibleRows; ++visible) {
        const auto& outer = scene->rows[visible];
        ApplicationSearchSourceTicket ticket{};
        if (!application_search_source_ticket(outer.source_index, &ticket))
            continue;
        for (u32 scope = 0; scope < 2; ++scope) {
            ApplicationSearchSourceView source{};
            if (!application_search_source_resolve(ticket, scope != 0, &source) || !source.message ||
                !source.rich || !source.rich_store || source.message->id != outer.message_id ||
                source.message->revision != outer.message_revision)
                continue;
            const auto* row = scope ? message_scene_snapshot_row(*scene, outer) : &outer;
            if (!row)
                continue;
            for (u32 kind = 0; kind < 2; ++kind) {
                const u32 count = kind ? source.rich->embed_count : source.rich->attachment_count;
                for (u32 index = 0; index < count; ++index) {
                    for (u32 resource_index = 0; resource_index < (kind ? 2u : 1u); ++resource_index) {
                        UiSearchTarget target{};
                        target.action = UiSearchAction::rich_item;
                        target.channel_id = source.message->channel_id;
                        target.message_id = source.message->id;
                        target.source_revision = search.source_revision;
                        target.source_index = ticket.index;
                        target.rich_kind =
                            kind ? MessageSceneRichItemKind::embed : MessageSceneRichItemKind::attachment;
                        target.rich_index = static_cast<u16>(index);
                        target.snapshot = scope != 0;
                        if (kind) {
                            MessageRichEmbedMediaSelection selected{};
                            if (!(resource_index
                                      ? message_rich_embed_resource_select(
                                            *source.rich_store, source.rich->embeds[index],
                                            MediaEmbedResource::thumbnail, &selected)
                                      : message_rich_embed_media_select(
                                            *source.rich_store, source.rich->embeds[index], &selected)))
                                continue;
                            // A large thumbnail (video poster, article image) is the
                            // primary media; only a corner thumbnail is requested twice.
                            if (resource_index && !message_rich_embed_plan(*source.rich_store,
                                                                           source.rich->embeds[index])
                                                       .corner_thumbnail)
                                continue;
                            target.resource = selected.resource;
                        }
                        UiRect rectangle{}, clip{};
                        if (!ui_search_scene_rich_preview_geometry(search, target, &rectangle, &clip) ||
                            !ui_rect_has_area(clip))
                            continue;
                        bool stop = false;
                        if (!kind) {
                            if (!request_visible_attachment(
                                    *source.message, source.rich->attachments[index],
                                    ui_rect_width(rectangle), ui_rect_height(rectangle), now_ms,
                                    now_seconds, uploads, repaint, &stop, source.rich_store, scope != 0))
                                return false;
                        } else if (!request_visible_embed_media(
                                       *source.message, *source.rich, index, ui_rect_width(rectangle),
                                       ui_rect_height(rectangle), now_ms, now_seconds, uploads, repaint,
                                       &stop, resource_index != 0, source.rich_store))
                            return false;
                        if (stop) {
                            *stop_scan = true;
                            return true;
                        }
                    }
                }
            }
        }
    }
    return true;
}

bool ApplicationMediaPump::request_search_decorations(u64 now_ms, u64 now_seconds, u64 now_us, u32* uploads,
                                                      bool* repaint, bool* stop_scan) noexcept {
    *stop_scan = false;
    if (!search_media_visible() || prefetch_only)
        return true;
    const auto& search = *application.renderer.search_scene;
    MessageMediaPumpContext context{this, now_ms, now_seconds, uploads, repaint};
    MessageMediaControllerCallbacks callbacks{};
    callbacks.context = &context;
    callbacks.asset = request_controller_asset;
    const auto visit = [&](MediaAssetKey key, bool animated) noexcept {
        const auto asset_result = message_media_controller_visit_asset(
            &application.message_media, key, animated, &application.animations.timeline,
            application.renderer.media_atlas, now_us, callbacks);
        *stop_scan = asset_result.stopped;
        return asset_result.ok();
    };
    const auto* text = application.renderer.content_text;
    if (text && text->content_semantics && text->content_hit_regions) {
        // The native parser owns spoiler visibility and source ranges. Never
        // rescan raw accepted text, which would disclose concealed artwork.
        for (u32 i = 0; i < text->content_hit_region_count; ++i) {
            const auto& region = text->content_hit_regions[i];
            if (region.semantic_index >= text->content_semantic_count)
                continue;
            const auto& semantic = text->content_semantics[region.semantic_index];
            if (!(semantic.reserved & kRendererSearchSemanticFlag) || (semantic.reserved & 2u) ||
                (semantic.kind != MessageMarkdownSemanticKind::custom_emoji &&
                 semantic.kind != MessageMarkdownSemanticKind::regional_flag) ||
                !ui_rect_has_area(ui_search_scene_content_clip(
                    search, {region.left, region.top, region.right, region.bottom})))
                continue;
            ApplicationSearchSourceView source{};
            const bool snapshot = ((semantic.source_offset >> 16u) & 0x4000u) != 0;
            if (!application_search_source_resolve(semantic.message_id, semantic.message_revision, snapshot,
                                                   &source))
                continue;
            const bool animated =
                semantic.kind == MessageMarkdownSemanticKind::custom_emoji && (semantic.reserved & 1u);
            const auto key = semantic.kind == MessageMarkdownSemanticKind::regional_flag
                                 ? media_asset_regional_flag(static_cast<u16>(semantic.target))
                                 : media_asset_custom_emoji(semantic.target, animated);
            if (semantic.kind == MessageMarkdownSemanticKind::regional_flag && semantic.target > 679u)
                continue;
            if (!media_asset_key_valid(key))
                continue;
            if (!visit(key, animated))
                return false;
            if (*stop_scan)
                return true;
        }
    }
    const auto* scene = ui_search_scene_message_scene(search);
    if (!scene)
        return true;
    for (u32 visible = 0; visible < scene->count && visible < kUiSearchVisibleRows; ++visible) {
        const auto& outer = scene->rows[visible];
        ApplicationSearchSourceTicket ticket{};
        if (!application_search_source_ticket(outer.source_index, &ticket))
            continue;
        for (u32 scope = 0; scope < 2; ++scope) {
            ApplicationSearchSourceView source{};
            if (!application_search_source_resolve(ticket, scope != 0, &source) || !source.message ||
                !source.rich || source.message->id != outer.message_id ||
                source.message->revision != outer.message_revision)
                continue;
            for (u32 index = 0; index < source.rich->sticker_count; ++index) {
                const auto& sticker = source.rich->stickers[index];
                const auto key = media_asset_sticker(sticker.id, sticker.format_type);
                if (!media_asset_key_valid(key))
                    continue;
                UiSearchTarget target{};
                target.action = UiSearchAction::rich_item;
                target.channel_id = source.message->channel_id;
                target.message_id = source.message->id;
                target.source_revision = search.source_revision;
                target.source_index = ticket.index;
                target.rich_kind = MessageSceneRichItemKind::sticker;
                target.rich_index = static_cast<u16>(index);
                target.snapshot = scope != 0;
                UiRect rectangle{}, clip{};
                if (!ui_search_scene_rich_preview_geometry(search, target, &rectangle, &clip) ||
                    !ui_rect_has_area(clip))
                    continue;
                if (!visit(key, sticker.format_type == 2 || sticker.format_type == 4))
                    return false;
                if (*stop_scan)
                    return true;
            }
        }
    }
    return true;
}

bool ApplicationMediaPump::request_controller_attachment(void* context, const MessageRecord& message,
                                                         const MessageRichAttachment& attachment,
                                                         float preview_width, float preview_height,
                                                         bool* stop_scan) noexcept {
    auto* pump = static_cast<MessageMediaPumpContext*>(context);
    return pump && pump->owner &&
           pump->owner->request_visible_attachment(message, attachment, preview_width, preview_height,
                                                   pump->now_ms, pump->now_seconds, pump->uploads,
                                                   pump->repaint, stop_scan);
}

bool ApplicationMediaPump::request_controller_embed(void* context, const MessageRecord& message,
                                                    const MessageRichRecord& rich, u32 embed_index,
                                                    float preview_width, float preview_height,
                                                    bool* stop_scan) noexcept {
    auto* pump = static_cast<MessageMediaPumpContext*>(context);
    if (!pump || !pump->owner ||
        !pump->owner->request_visible_embed_media(message, rich, embed_index, preview_width, preview_height,
                                                  pump->now_ms, pump->now_seconds, pump->uploads,
                                                  pump->repaint, stop_scan))
        return false;
    if (*stop_scan)
        return true;
    float width = 0, height = 0;
    const auto& store = pump->owner->application.account_ui->message_rich_store;
    // A merged gallery image's own corner thumbnail is never drawn.
    return message_rich_embed_merge_host(store, rich, embed_index) != embed_index ||
           !message_rich_embed_thumbnail_extent(store, rich.embeds[embed_index], 80, &width, &height) ||
           pump->owner->request_visible_embed_media(message, rich, embed_index, width, height, pump->now_ms,
                                                    pump->now_seconds, pump->uploads, pump->repaint,
                                                    stop_scan, true);
}

bool ApplicationMediaPump::request_controller_avatar(void* context, const UserRecord& user,
                                                     bool force_request, bool* stop_scan) noexcept {
    auto* pump = static_cast<MessageMediaPumpContext*>(context);
    return pump && pump->owner &&
           pump->owner->request_visible_user_avatar(user, pump->now_ms, pump->now_seconds, pump->uploads,
                                                    pump->repaint, stop_scan, force_request);
}

bool ApplicationMediaPump::request_controller_asset(void* context, MediaAssetKey key, bool force_request,
                                                    bool* stop_scan) noexcept {
    auto* pump = static_cast<MessageMediaPumpContext*>(context);
    return pump && pump->owner &&
           pump->owner->request_visible_static_media(key, pump->now_ms, pump->now_seconds, pump->uploads,
                                                     pump->repaint, stop_scan, force_request);
}

Utf8View application_notification_avatar_uri(u64 user_id, Utf8View avatar_hash, Slice<char> storage) noexcept {
    const u32 variant = user_avatar_variant(avatar_hash);
    if (!user_id || !variant || !storage.data || storage.count < 64 || !g_app.renderer.media_atlas)
        return {};
    const MediaAssetKey key = media_asset_user_avatar(user_id, variant, asset_hash_animated(avatar_hash));
    const MediaAtlasResult resident = media_atlas_peek(g_app.renderer.media_atlas, key);
    // Identity requests are capped at 128px, so a resident avatar fits 256px.
    constexpr u32 kMaximumExtent = 256;
    if (!resident.ok() || resident.width > kMaximumExtent || resident.height > kMaximumExtent)
        return {};
    wchar_t path[MAX_PATH]{};
    const DWORD base = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (!base || base >= MAX_PATH - 96)
        return {};
    u32 used = base;
    const auto append = [&](const wchar_t* text) noexcept {
        while (*text && used + 1 < MAX_PATH)
            path[used++] = *text++;
        path[used] = 0;
    };
    const auto append_hex = [&](u64 value, u32 digits) noexcept {
        for (u32 i = digits; i-- && used + 1 < MAX_PATH;)
            path[used++] = L"0123456789abcdef"[(value >> (i * 4)) & 15];
        path[used] = 0;
    };
    if (path[used - 1] != L'\\')
        append(L"\\");
    append(L"MaiCord");
    CreateDirectoryW(path, nullptr);
    append(L"\\notification-avatars");
    CreateDirectoryW(path, nullptr);
    append(L"\\");
    append_hex(user_id, 16);
    append(L"-");
    append_hex(variant, 8);
    append(L".png");
    // One file per user and avatar hash: an existing file already holds these pixels.
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        const u32 pixels_bytes = resident.width * resident.height * 4u;
        const u64 png_bytes = clipboard_image_png_bytes(resident.width, resident.height);
        if (!png_bytes || png_bytes > 1u << 20)
            return {};
        auto* memory = static_cast<u8*>(VirtualAlloc(nullptr, pixels_bytes + static_cast<SIZE_T>(png_bytes),
                                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!memory)
            return {};
        u32 width = 0, height = 0;
        bool written = renderer_read_media_rgba(&g_app.renderer, key, {memory, pixels_bytes}, &width, &height) &&
                       clipboard_image_encode_png(memory, width, height, width * 4,
                                                  {memory + pixels_bytes, static_cast<u32>(png_bytes)}) ==
                           clipboard_image_png_bytes(width, height);
        if (written) {
            const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                            nullptr);
            DWORD count = 0;
            const DWORD bytes = static_cast<DWORD>(clipboard_image_png_bytes(width, height));
            written = file != INVALID_HANDLE_VALUE &&
                      WriteFile(file, memory + pixels_bytes, bytes, &count, nullptr) && count == bytes;
            if (file != INVALID_HANDLE_VALUE)
                CloseHandle(file);
            if (!written)
                DeleteFileW(path);
        }
        VirtualFree(memory, 0, MEM_RELEASE);
        if (!written)
            return {};
    }
    // file:///C:/... with forward slashes; toasts reject '?', '#' and backslashes.
    constexpr char prefix[] = "file:///";
    copy_bytes(storage.data, prefix, sizeof(prefix) - 1);
    const int converted = WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(used),
                                              storage.data + sizeof(prefix) - 1,
                                              static_cast<int>(storage.count - (sizeof(prefix) - 1)), nullptr, nullptr);
    if (converted <= 0)
        return {};
    const u32 bytes = static_cast<u32>(converted) + sizeof(prefix) - 1;
    for (u32 i = sizeof(prefix) - 1; i < bytes; ++i) {
        if (storage.data[i] == '\\')
            storage.data[i] = '/';
        else if (storage.data[i] == '?' || storage.data[i] == '#')
            return {};
    }
    return {storage.data, bytes};
}

} // namespace maicord::app_detail
