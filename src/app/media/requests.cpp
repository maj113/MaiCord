#include "requests_internal.hpp"
#include "../account/profile.hpp"
#include "maicord/core/time.hpp"
#include "../account/profile_editor.hpp"
#include "../guild/card.hpp"
#include "expression_card.hpp"
#include "../guild/banner.hpp"
#include "../guild/settings/settings.hpp"
#include "catalog_preview_log.hpp"
#include "maicord/render/media/inline_media.hpp"
#include "maicord/ui/chat/private_channel_rows.hpp"
#include "maicord/ui/shell/popup_presentation.hpp"
#include "maicord/ui/chat/search/avatar_media.hpp"
#include "../voice/stream.hpp"
#include "maicord/ui/chat/pinned_messages_avatar_media.hpp"
#include "maicord/ui/chat/threads/forum_channel_media.hpp"
#include "maicord/ui/voice/channel_rows.hpp"
#include "maicord/ui/chat/inbox_avatar_media.hpp"
#include "../../render/d3d11/renderer_interaction_state.hpp"
#include "../account/account_ui_runtime.hpp"
#include "maicord/ui/account/friends_media.hpp"
#include "maicord/ui/chat/polls/voters_avatar_media.hpp"

namespace maicord::app_detail {
namespace requests_detail {

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

} // namespace requests_detail
namespace {

bool preview_decoder_animation(Application& application, const MediaAtlasResult& resident,
                               const MediaAssetKey& key) noexcept {
    const bool published = animation_timeline_find(&application.animations.timeline, key).ok();
    // A decoder-proven resident first frame remains animated when a stale
    // message owner retires its timeline and the real new owner must acquire it.
    return published || (resident.ok() && media_atlas_decoded_animation(
                                              *application.renderer.preview_media_atlas, resident.handle));
}

} // namespace
namespace requests_detail {

// Chat animation policy: paused media is only looked up, never marked
// visible, so its timeline holds the current frame until hover or focus.
bool observe_message_animation(Application& application, const MediaAssetKey& key, u64 message_id) noexcept {
    return message_media_controller_animation_plays(application.message_media, key.kind, message_id)
               ? animation_timeline_observe_visible(
                     &application.animations.timeline, key,
                     clock_microseconds(application.clock, application.process_start, clock_now()))
               : animation_timeline_find(&application.animations.timeline, key).ok();
}

// Forum post cards own their starter-image thumbnails while the browser is shown.
bool forum_thumbnail_key_current(const ApplicationMediaPump& pump, const MediaAssetKey& key) noexcept {
    const Application& application = pump.application;
    return key.kind == MediaAssetKind::attachment && !application.login.visible &&
           (pump.surfaces() & d3d11_surface_thread_browser) &&
           application.renderer.thread_browser_scene->forum &&
           forum_channel_thumbnail_current(*application.renderer.thread_browser_scene, application.renderer.dpi, key);
}

} // namespace requests_detail
namespace {

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
namespace requests_detail {

// The format a key's attachment resolves to: audio keys use the audio format table, others the
// image one.
MediaAttachmentFormat attachment_format_for_key(const MediaAssetKey& key, const MessageRichStore& store,
                                                const MessageRichAttachment& item) noexcept {
    return media_attachment_format_audio_supported(media_asset_attachment_format(key))
               ? message_rich_attachment_audio_format(store, item)
               : message_rich_attachment_format(store, item);
}

// Takes the frame the worker published for `key` and uploads it. A frame of another key, or with
// message_owned one that no shown message owns, is stale (a failure when stale_fails).
__declspec(noinline) FrameTake take_ready_frame(ApplicationMediaPump& pump, const MediaAssetKey& key,
                                                bool message_owned, bool stale_fails,
                                                AccountMediaFrame* frame) noexcept {
    AccountMediaFrame local;
    if (!frame)
        frame = &local;
    *frame = {};
    const auto& application = pump.application;
    if (!account_module_media_take(application.account.instance, frame))
        return FrameTake::failed;
    if (!media_asset_key_equal(frame->key, key) ||
        (message_owned && !pump.message_image_frame_is_current(*frame)))
        return stale_fails ? FrameTake::failed : FrameTake::stale;
    return pump.upload_account_media_frame(*frame) == ApplicationMediaPump::MediaFrameUploadResult::uploaded
               ? FrameTake::uploaded
               : FrameTake::deferred;
}

__declspec(noinline) void note_frame_uploaded(ApplicationMediaPump& pump) noexcept {
    ++pump.uploads;
    pump.trace_first_media_upload();
    if (pump.repaint)
        *pump.repaint = true;
}

// True when a resident preview needs no request. A resident static image (or any resident image while
// prefetching) settles at once; a hinted or decoder-proven animation must also be observed visible.
__declspec(noinline) bool message_media_resident(ApplicationMediaPump& pump, const MessageRecord& message,
                                                 const MediaAssetKey& key, bool animated_hint,
                                                 bool static_resident) noexcept {
    auto& application = pump.application;
    const auto resident_media = pump.prefetch_only
                                    ? media_atlas_peek(application.renderer.preview_media_atlas, key)
                                    : media_atlas_find(application.renderer.preview_media_atlas, key);
    const bool resident = resident_media.ok();
    if (resident && (pump.prefetch_only || static_resident))
        return true;
    // The decoder may discover APNG/animated WebP without an animation hint in the
    // descriptor. Its published timeline still owns visibility, and a resident first
    // frame is not sufficient: the timeline entry is marked during every visibility
    // epoch so end_visibility does not pause the retained decoder.
    if (!pump.prefetch_only && (animated_hint || preview_decoder_animation(application, resident_media, key))) {
        pump.retire_stale_message_animation(key, message.id, message.revision);
        return observe_message_animation(application, key, message.id) && resident;
    }
    return resident;
}

// One avatar of a scene's visible list: skips a resident animation, stops at the upload budget and
// otherwise requests it.
__declspec(noinline) ScanStep request_scene_avatar(ApplicationMediaPump& pump, const MediaAssetKey& key,
                                                   const Utf8View& hash, bool* stop) noexcept {
    bool force = false;
    if ((key.flags & media_asset_flag_animated) && pump.animation_resident(key, &force))
        return ScanStep::next;
    if (!pump.visibility_only && pump.uploads >= kMessageMediaUploadBudget) {
        *stop = true;
        return ScanStep::stopped;
    }
    if (!pump.request_visible_avatar_media(key, hash, stop, force))
        return ScanStep::failed;
    return *stop ? ScanStep::stopped : ScanStep::next;
}

} // namespace requests_detail

bool ApplicationMediaPump::content_media_visible() const noexcept {
    return !(application.renderer.forum_form && application.renderer.forum_form->scene &&
             application.renderer.forum_form->scene->active) &&
           !application.login.visible &&
           !(application.renderer.settings_scene && application.renderer.settings_scene->active) &&
           !(application.renderer.friends_scene && application.renderer.friends_scene->active) &&
           // The thread browser covers the chat column; chrome stays visible.
           !(surfaces() & d3d11_surface_thread_browser);
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
    return content_media_visible() && (surfaces() & d3d11_surface_search) &&
           !application.image_viewer.scene.active && !application.reaction_viewer.active &&
           !application.popup_scene.active && !application.text_form &&
           !(renderer.profile_scene && renderer.profile_scene->active) &&
           !(renderer.quick_switcher_scene && renderer.quick_switcher_scene->active) &&
           !(renderer.message_forward_scene && renderer.message_forward_scene->active) &&
           !(surfaces() & d3d11_surface_pinned_messages);
}
bool ApplicationMediaPump::request_picker_emoji() noexcept {
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
        if (uploads >= kMessageMediaUploadBudget)
            continue;
        bool stop = false;
        if (!request_visible_static_media(key, &stop, force, true))
            return false;
        if (stop)
            return true;
    }
    for (u32 i = 0; i < picker.rail_count && uploads < kMessageMediaUploadBudget; ++i) {
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
        if (slot.animated && animation_resident(key, &force))
            continue;
        bool stop = false;
        if (!request_visible_guild_icon(*guild, &stop, force))
            return false;
    }
    return true;
}
bool ApplicationMediaPump::request_inline_flags() noexcept {
    if (application.login.visible || visibility_only || !application.renderer.inline_media.flags)
        return true;
    const auto& renderer = application.renderer;
    const u32 count = minimum(renderer.inline_media.count, kRendererInlineFlagCapacity);
    // Multiple foreground labels may borrow the same static country asset.
    // Identity is a bounded Unicode sequence, not a URL or widget hash.
    u8 seen[(680 + 7) / 8]{};
    for (u32 i = 0; i < count && uploads < kMessageMediaUploadBudget; ++i) {
        const auto& flag = renderer.inline_media.flags[i];
        if (!renderer_inline_flag_current(renderer, flag) || flag.flag >= 680)
            continue;
        const u8 bit = static_cast<u8>(1u << (flag.flag & 7));
        if (seen[flag.flag >> 3] & bit)
            continue;
        seen[flag.flag >> 3] |= bit;
        bool stop = false;
        if (!request_visible_static_media(media_asset_regional_flag(flag.flag), &stop, false, true))
            return false;
        if (stop)
            break;
    }
    return true;
}
bool ApplicationMediaPump::request_catalog_previews() noexcept {
    if (!catalog_media_visible())
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
        if (visibility_only || asset_backed_off(request.key))
            continue;
        if (uploads >= kMessageMediaUploadBudget)
            return true;
        // A VIDEO favourite's tile decodes on the presenting device.
        request.video_device = application.renderer.graphics.device;
        const auto requested = account_module_catalog_preview_request(
            application.account.instance, &request, now_ms, now_seconds);
        if (requested.state == AccountMediaRequestState::failed ||
            requested.state == AccountMediaRequestState::unsupported ||
            requested.state == AccountMediaRequestState::invalid)
            log_catalog_preview_failure(request, requested);
        if (requested.state == AccountMediaRequestState::ready) {
            const FrameTake taken = take_ready_frame(*this, request.key, true, false);
            if (taken == FrameTake::failed)
                return false;
            if (taken != FrameTake::uploaded)
                continue;
            note_frame_uploaded(*this);
            animation_timeline_observe_visible(&application.animations.timeline, request.key, now_us);
        }
        message_media_controller_observe_retry(&application.message_media, requested.retry_at_ms);
        // Saturated lanes do not end this bounded surface's visibility scan.
    }
    return true;
}

namespace {

bool profile_scene_shows(const ProfileScene& scene, const MediaAssetKey& key) noexcept {
    for (u32 i = 0; i < scene.part_count; ++i) {
        ProfileMedia media;
        if (profile_media_resolve(scene, i, &media) && media_asset_key_equal(key, media.key))
            return true;
    }
    for (u32 i = 0; scene.effect && i < maximum<u8>(1, scene.effect->count); ++i) {
        ProfileMedia media;
        if (profile_effect_media_resolve(scene, i, clock_monotonic_milliseconds(), &media) &&
            media_asset_key_equal(key, media.key))
            return true;
    }
    return false;
}
// Edit Profile's preview card (current avatar, banner, decoration and effect
// with pending edits over them), its dialogs and its picked local images.
bool profile_editor_key_current(const MediaAssetKey& key) noexcept {
    if (key.kind == MediaAssetKind::local_draft_preview && application_profile_editor_frame_current(key))
        return true;
    if (const auto* preview = application_profile_editor_preview();
        preview && profile_scene_shows(*preview, key))
        return true;
    if (const auto* dialog = application_profile_editor_dialog()) {
        ProfileMedia media;
        for (u32 i = 0; ui_profile_dialog_media(*dialog, i, &media); ++i)
            if (media_asset_key_equal(key, media.key))
                return true;
    }
    return false;
}

} // namespace

MediaAtlas* ApplicationMediaPump::upload_atlas(const MediaAssetKey& key) noexcept {
    const bool preview = media_asset_large_preview(key) &&
                         media_asset_resolution_tier(key) != MediaResolutionTier::original &&
                         application.renderer.preview_media_atlas;
    return preview ? application.renderer.preview_media_atlas : application.renderer.media_atlas;
}

bool ApplicationMediaPump::media_key_is_current(const MediaAssetKey& key) noexcept {
    if (application.login.visible)
        return false;
    if (forum_thumbnail_key_current(*this, key))
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
        (application_guild_card_key_current(key) || application_guild_banner_key_current(key) ||
         application_guild_settings_preview_key_current(key)))
        return true;
    // Active Now's activity art (the Friends page, under no requests dialog).
    if (key.kind == MediaAssetKind::profile_image && application.renderer.friends_scene &&
        application.renderer.friends_scene->active &&
        !(application.renderer.friends_requests_scene && application.renderer.friends_requests_scene->active) &&
        friends_activity_media_current(*application.renderer.friends_scene, key))
        return true;
    if (application.renderer.settings_scene && application.renderer.settings_scene->active)
        return false;
    if (key.kind == MediaAssetKind::catalog_preview)
        return catalog_media_visible() && application_media_picker_preview_visible(key);
    if (key.kind == MediaAssetKind::user_avatar)
        return avatar_key_is_current(key);
    if (is_default_avatar_key(key) && avatar_key_is_current(key))
        return true;
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
bool ApplicationMediaPump::animation_resident(const MediaAssetKey& key, bool* force_request,
                                              bool play) noexcept {
    auto& timeline = application.animations.timeline;
    const bool observed = play ? animation_timeline_observe_visible(&timeline, key, now_us)
                               : animation_timeline_find(&timeline, key).ok();
    *force_request = !observed;
    return observed && media_atlas_find(application.renderer.media_atlas, key).ok();
}
bool ApplicationMediaPump::settings_avatar_is_current(const MediaAssetKey& key) const noexcept {
    if (!(surfaces() & d3d11_surface_settings_page))
        return false;
    UiSettingsAvatarGeometry avatar{};
    for (u32 cursor = 0; ui_settings_scene_next_avatar(*application.renderer.settings_scene, &cursor, &avatar);)
        if (media_asset_key_equal(avatar.media->key, key))
            return true;
    return false;
}
bool ApplicationMediaPump::request_settings_avatars() noexcept {
    if (!(surfaces() & d3d11_surface_settings_page))
        return true;
    UiSettingsAvatarGeometry avatar{};
    for (u32 cursor = 0; ui_settings_scene_next_avatar(*application.renderer.settings_scene, &cursor, &avatar);) {
        if (message_media_controller_upload_budget_reached(&application.message_media, uploads))
            return true;
        bool stop = false;
        // The request validates the hash and skips a resident key. Server
        // identity rows share the rail's guild icon key and CDN asset.
        const Utf8View hash{avatar.media->hash, avatar.media->hash_bytes};
        // Server Settings emoji and sticker previews, by their CDN identity.
        if (avatar.media->key.kind == MediaAssetKind::custom_emoji ||
            avatar.media->key.kind == MediaAssetKind::sticker) {
            if (!request_visible_static_media(avatar.media->key, &stop, false,
                                              true))
                return false;
            if (stop)
                return true;
            continue;
        }
        if (avatar.media->key.kind == MediaAssetKind::profile_image) {
            // Default avatars: the hash carries the CDN path, fetched by URL
            // under the profile card's key; a game cover carries its full URL.
            ProfileMedia media{};
            media.key = avatar.media->key;
            const Utf8View url = avatar.media->url;
            if (url.bytes) {
                if (url.bytes > sizeof(media.url.data))
                    continue;
                copy_bytes(media.url.data, url.data, url.bytes);
                media.url.bytes = static_cast<u16>(url.bytes);
            } else {
                constexpr auto host = utf8_literal("https://cdn.discordapp.com/");
                copy_bytes(media.url.data, host.data, host.bytes);
                copy_bytes(media.url.data + host.bytes, hash.data, hash.bytes);
                media.url.bytes = static_cast<u16>(host.bytes + hash.bytes);
            }
            if (!request_profile_media(media, &stop, false,
                                       nullptr))
                return false;
            if (stop)
                return true;
            continue;
        }
        if (!(avatar.media->key.kind == MediaAssetKind::guild_icon
                  ? request_visible_hashed_identity(avatar.media->key, hash,
                                                    &stop, false)
                  : request_visible_user_avatar_value(avatar.media->key.id, hash, &stop, false)))
            return false;
        if (stop)
            return true;
    }
    // The Server Profile preview card's game icons and custom banner by URL, its traits' server emoji by key.
    MediaAssetKey key{};
    Utf8View url{};
    for (u32 index = 0; application_guild_settings_preview_media(index, &key, &url); ++index) {
        if (message_media_controller_upload_budget_reached(&application.message_media, uploads))
            return true;
        if (!url.bytes) {
            bool stop = false;
            if (!request_visible_static_media(key, &stop, false, true))
                return false;
            if (stop)
                return true;
            continue;
        }
        if (url.bytes > sizeof(ProfileMedia{}.url.data))
            continue;
        ProfileMedia media{};
        media.key = key;
        copy_bytes(media.url.data, url.data, url.bytes);
        media.url.bytes = static_cast<u16>(url.bytes);
        bool stop = false;
        if (!request_profile_media(media, &stop, false, nullptr))
            return false;
        if (stop)
            return true;
    }
    return true;
}
bool ApplicationMediaPump::request_forward_identities(bool* stop) noexcept {
    if (!application.account_ui || !(surfaces() & d3d11_surface_message_forward))
        return true;
    const auto& forward = *application.renderer.message_forward_scene;
    const auto& state = application.account_ui->chat_state;
    for (u32 slot = 0; slot < forward.count; ++slot) {
        UiMessageForwardMedia media{};
        if (!ui_message_forward_scene_media(forward, slot, &state, &media))
            continue;
        if (!visibility_only && uploads >= kMessageMediaUploadBudget) {
            *stop = true;
            return true;
        }
        // Server tiles go through the rail's guild-icon request, so the key,
        // CDN size and residency are shared with the rail.
        const auto* guild =
            media.key.kind == MediaAssetKind::guild_icon ? chat_state_store_find_guild(state, media.key.id) : nullptr;
        if (media.key.kind == MediaAssetKind::guild_icon
                ? guild && !request_visible_guild_icon(*guild, stop, false, false)
                : !request_visible_avatar_media(media.key, media.hash, stop, false))
            return false;
        if (*stop)
            return true;
    }
    return true;
}
bool ApplicationMediaPump::avatar_key_is_current(const MediaAssetKey& key) noexcept {
    if (!application.account_ui || (key.kind != MediaAssetKind::user_avatar && !is_default_avatar_key(key)) ||
        !media_asset_key_valid(key))
        return false;
    const auto& users = application.account_ui->user_store;
    const u32 shown = surfaces();
    if ((shown & d3d11_surface_search) &&
        search_avatar_media_current(*application.renderer.search_scene, key))
        return true;
    if ((shown & d3d11_surface_pinned_messages) &&
        pinned_messages_avatar_media_current(*application.renderer.pinned_messages_scene, key))
        return true;
    if ((shown & d3d11_surface_thread_browser) &&
        forum_channel_avatar_media_current(*application.renderer.thread_browser_scene, key))
        return true;
    if (shown & d3d11_surface_quick_switcher) {
        const auto& switcher = *application.renderer.quick_switcher_scene;
        for (u32 slot = 0; slot < switcher.count; ++slot) {
            UiQuickSwitcherMedia media{};
            if (ui_quick_switcher_scene_media(switcher, slot, nullptr, &media) &&
                media_asset_key_equal(media.key, key))
                return true;
        }
    }
    if (shown & d3d11_surface_message_forward) {
        const auto& forward = *application.renderer.message_forward_scene;
        for (u32 slot = 0; slot < forward.count; ++slot) {
            UiMessageForwardMedia media{};
            if (ui_message_forward_scene_media(forward, slot, nullptr, &media) &&
                media_asset_key_equal(media.key, key))
                return true;
        }
    }
    if ((shown & d3d11_surface_inbox) &&
        inbox_avatar_media_current(*application.renderer.inbox_scene, key))
        return true;
    if ((shown & d3d11_surface_poll_voters) &&
        poll_voters_avatar_media_current(*application.renderer.poll_voters_scene, key))
        return true;
    const auto matches = [&](u64 id) noexcept {
        const auto* user = user_store_find(users, id);
        if (!user)
            return false;
        return media_asset_key_equal(key, user_avatar_key(id, user_store_avatar_hash(*user)));
    };
    const auto& composer = application.message_composer_scene;
    if (content_media_visible() && ui_rect_has_area(message_composer_scene_command_avatar(composer)) &&
        media_asset_key_equal(key, composer.command.avatar))
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
            ui_scene_node(*application.renderer.ui_scene, UiSemantic::channel_footer);
        if (footer && ui_rect_has_area(footer->rectangle) &&
            matches(application.account_ui->chat_state.self_user_id))
            return true;
    }
    // Call view tiles keep their avatars while the picture is not resident.
    if (const auto* call = application.renderer.call_scene; call && call->active) {
        for (u32 i = 0; i < call->tile_count; ++i)
            if (ui_call_scene_tile_visible(*call, i) && matches(call->tiles[i].user_id))
                return true;
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
            if (media_asset_key_equal(key, user_avatar_key(user.id, reaction_viewer_user_avatar_hash(user))))
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
bool ApplicationMediaPump::channel_icon_key_is_current(const MediaAssetKey& key) noexcept {
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
bool ApplicationMediaPump::request_pinned_messages_avatars(bool* stop) noexcept {
    *stop = false;
    if (!(surfaces() & d3d11_surface_pinned_messages))
        return true;
    const auto& scene = *application.renderer.pinned_messages_scene;
    for (u32 slot = 0; slot < scene.count && slot < kUiPinnedMessagesVisibleRows; ++slot) {
        AvatarMedia media;
        if (!pinned_messages_avatar_media_resolve(scene, slot, &media))
            continue;
        const ScanStep step = request_scene_avatar(*this, media.key, media.hash, stop);
        if (step != ScanStep::next)
            return step == ScanStep::stopped;
    }
    return true;
}
// Forum post cards borrow author hashes from the visible page (starter author
// or user cache), like pins, including authors absent from the timeline.
bool ApplicationMediaPump::request_forum_avatars(bool* stop) noexcept {
    *stop = false;
    if (!(surfaces() & d3d11_surface_thread_browser) ||
        !application.renderer.thread_browser_scene->forum)
        return true;
    const auto& scene = *application.renderer.thread_browser_scene;
    for (u32 slot = 0; slot < scene.count && slot < kUiThreadBrowserPageCapacity; ++slot) {
        AvatarMedia media;
        if (!forum_channel_avatar_media_resolve(scene, slot, &media))
            continue;
        const ScanStep step = request_scene_avatar(*this, media.key, media.hash, stop);
        if (step != ScanStep::next)
            return step == ScanStep::stopped;
    }
    return true;
}
// Starter-image thumbnails of visible forum post cards. The signed URL comes
// from the accepted post page; an expired one goes through the shared refresher.
bool ApplicationMediaPump::request_forum_thumbnails(bool* stop) noexcept {
    *stop = false;
    if (visibility_only || !(surfaces() & d3d11_surface_thread_browser) ||
        !application.renderer.thread_browser_scene->forum || !application.renderer.preview_media_atlas)
        return true;
    const auto& scene = *application.renderer.thread_browser_scene;
    for (u32 slot = 0; slot < scene.count && slot < kUiThreadBrowserPageCapacity; ++slot) {
        ForumChannelThumbnailMedia media{};
        if (!forum_channel_thumbnail_resolve(scene, slot, application.renderer.dpi, &media) ||
            media_atlas_find(application.renderer.preview_media_atlas, media.key).ok() || asset_backed_off(media.key))
            continue;
        if (message_media_controller_upload_budget_reached(&application.message_media, uploads)) {
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
        const AccountMediaRequestResult requested = account_module_media_attachment_request(
            application.account.instance, &request, now_ms, now_seconds);
        log_message_media_failure("forum_thumbnail_request_failed", requested, media.key);
        if (requested.state == AccountMediaRequestState::ready) {
            const FrameTake taken = take_ready_frame(*this, media.key, true, false);
            if (taken == FrameTake::failed)
                return false;
            if (taken == FrameTake::deferred) {
                *stop = true;
                return true;
            }
            if (taken == FrameTake::uploaded)
                note_frame_uploaded(*this);
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
bool ApplicationMediaPump::request_friends_avatars(bool* stop) noexcept {
    *stop = false;
    if (!application.account_ui)
        return true;
    const auto* requests = application.renderer.friends_requests_scene;
    const auto* scene = requests && requests->active ? requests : application.renderer.friends_scene;
    if (!scene)
        return true;
    const FriendsAvatarMediaCallbacks callbacks{
        this,
        +[](void* opaque, MediaAssetKey key) noexcept {
            return media_atlas_find(static_cast<ApplicationMediaPump*>(opaque)->application.renderer.media_atlas,
                                    key)
                .ok();
        },
        +[](void* opaque, MediaAssetKey key) noexcept {
            auto& pump = *static_cast<ApplicationMediaPump*>(opaque);
            return animation_timeline_observe_visible(&pump.application.animations.timeline, key, pump.now_us);
        },
        +[](void* opaque, const AvatarMedia& media, bool force, bool* stopped) noexcept {
            auto& pump = *static_cast<ApplicationMediaPump*>(opaque);
            if (message_media_controller_upload_budget_reached(&pump.application.message_media, pump.uploads)) {
                *stopped = true;
                return true;
            }
            return pump.request_visible_avatar_media(media.key, media.hash, stopped, force);
        }};
    // The dimmed main list uses resident cache entries while this modal owns
    // avatar requests and animation visibility. No hidden rows are fetched.
    const auto visited =
        friends_avatar_media_visit(*scene, application.account_ui->user_store, callbacks);
    *stop = visited.stopped;
    if (!visited.ok || *stop || scene != application.renderer.friends_scene)
        return visited.ok;
    // Active Now's activity art and badges, by URL like the profile's.
    ProfileMedia media;
    for (u32 i = 0; friends_activity_media_resolve(*scene, i, &media) && !*stop; ++i) {
        if (!ui_rect_intersects(media.rectangle, media.clip))
            continue;
        if (message_media_controller_upload_budget_reached(&application.message_media, uploads)) {
            *stop = true;
            break;
        }
        if (!request_profile_media(media, stop, false, nullptr))
            return false;
    }
    return true;
}

bool ApplicationMediaPump::request_stream_previews(bool* stop) noexcept {
    *stop = false;
    StreamPreviewMedia previews[4];
    const u32 count = application_stream_preview_media(previews, 4);
    for (u32 i = 0; i < count && !*stop; ++i) {
        const auto key = previews[i].key;
        if (media_atlas_find(application.renderer.media_atlas, key).ok() ||
            (application.renderer.preview_media_atlas &&
             media_atlas_find(application.renderer.preview_media_atlas, key).ok()))
            continue;
        if (visibility_only || asset_backed_off(key))
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
        const auto requested = account_module_media_attachment_request(application.account.instance,
                                                                                &request, now_ms, now_seconds);
        if (requested.state == AccountMediaRequestState::ready) {
            const FrameTake taken = take_ready_frame(*this, key, false, true);
            if (taken == FrameTake::failed)
                return false;
            if (taken == FrameTake::deferred) {
                *stop = true;
                return true;
            }
            note_frame_uploaded(*this);
            *stop = message_media_controller_upload_budget_reached(&application.message_media, uploads);
        }
        message_media_controller_observe_retry(&application.message_media, requested.retry_at_ms);
    }
    // The server tag popout's icon, tag badge, banner and game pictures, fetched by URL, then its traits' custom
    // emoji by key.
    GuildCardMedia cards[kGuildCardMediaCapacity];
    const u32 card_count = application_guild_card_media(cards, kGuildCardMediaCapacity);
    for (u32 i = 0; i < card_count && !*stop; ++i) {
        ProfileMedia media{};
        media.key = cards[i].key;
        const u32 bytes = minimum<u32>(cards[i].url.bytes, sizeof(media.url.data));
        copy_bytes(media.url.data, cards[i].url.data, bytes);
        media.url.bytes = static_cast<u16>(bytes);
        AccountMediaRequestResult failure{};
        failure.state = AccountMediaRequestState::ready; // a resident image makes no request
        if (!request_profile_media(media, stop, false, &failure))
            return false;
        application_guild_card_media_failed(cards[i].key, failure);
    }
    // An animated trait emoji joins the shared animation timeline while shown, as the composer's do.
    MediaAssetKey trait_emoji[kUiGuildCardTraits];
    const u32 trait_emoji_count = application_guild_card_emoji_media(trait_emoji, kUiGuildCardTraits);
    for (u32 i = 0; i < trait_emoji_count && !*stop; ++i) {
        bool force = false;
        if ((trait_emoji[i].flags & media_asset_flag_animated) && animation_resident(trait_emoji[i], &force))
            continue;
        if (!request_visible_static_media(trait_emoji[i], stop, force, true))
            return false;
    }
    // The emoji/sticker popout's picture and pack stickers by key, and its server's icon.
    MediaAssetKey expressions[1 + kUiExpressionCardPreviews];
    const u32 expression_count = application_expression_card_media(expressions, 1 + kUiExpressionCardPreviews);
    for (u32 i = 0; i < expression_count && !*stop; ++i)
        if (!request_visible_static_media(expressions[i], stop, false, true))
            return false;
    if (const auto* guild = application_expression_card_guild(); guild && !*stop)
        return request_visible_guild_icon(*guild, stop, false);
    return true;
}

} // namespace maicord::app_detail
