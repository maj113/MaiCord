#include "requests_internal.hpp"
#include "maicord/os/text.hpp"
#include "../shell/load_trace.hpp"
#include "maicord/platform/desktop/clipboard_image.hpp"
#include "maicord/core/win32_kernel.hpp"
#if !defined(_WIN32)
#include "maicord/sys/linux/path.hpp"
#endif
#include "maicord/core/data_path.hpp"
#include "maicord/os/file.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/ui/media/avatar_media.hpp"
#include "../chat/channel_authority.hpp"
#include "../chat/game_lookup.hpp"
#include "maicord/ui/chat/game_media.hpp"

namespace maicord::app_detail {

bool ApplicationMediaPump::request_profile_media(const ProfileMedia& media, bool* stop,
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
    if (visibility_only || asset_backed_off(key))
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
            ? account_module_media_request(application.account.instance, key, now_ms, now_seconds)
            : account_module_media_attachment_request(application.account.instance, &request,
                                                               now_ms, now_seconds);
    if (failure)
        *failure = requested;
    // A profile image (banner, effect layer, decoration, avatar) the worker could not fetch or decode is logged
    // once, at its failure record's first entry, and then backs off like a refused upload. The drawn size tells
    // the part apart; effect_slot names the pool an effect layer targets.
    if ((requested.state == AccountMediaRequestState::failed || requested.state == AccountMediaRequestState::invalid ||
         requested.state == AccountMediaRequestState::unsupported) &&
        key.kind == MediaAssetKind::profile_image &&
        media_asset_failure_record(&application.media_failures, key, clock_monotonic_milliseconds())) {
        const u64 values[]{static_cast<u64>(requested.state),        static_cast<u64>(requested.error),
                           static_cast<u64>(requested.http_error),   static_cast<u64>(ui_rect_width(media.rectangle)),
                           static_cast<u64>(ui_rect_height(media.rectangle)),
                           static_cast<u64>(media_asset_embed_format(key)), media_asset_profile_effect_layer(key)};
        LogEvent("profile_media_request_failed")
            .fields("state\0error\0http_error\0drawn_dips\0drawn_height_dips\0format\0effect_slot", values)
            .write(LogLevel::warning, "media");
    }
    if (requested.state == AccountMediaRequestState::busy) {
        // Admission depends on media type: still-image lanes can be full while
        // the animation reserve is idle. Keep scanning this bounded surface so
        // one waiting image does not prevent independent work from being queued.
        return true;
    }
    if (requested.state == AccountMediaRequestState::ready) {
        const FrameTake taken = take_ready_frame(*this, key, false, true);
        if (taken == FrameTake::failed)
            return false;
        // A frame the renderer did not take (no longer current, or rejected): the rest of the scan,
        // other sizes included, still gets its turn.
        if (taken == FrameTake::deferred)
            return true;
        note_frame_uploaded(*this);
        if (play)
            animation_timeline_observe_visible(&application.animations.timeline, key, now_us);
        *stop = message_media_controller_upload_budget_reached(&application.message_media, uploads);
    }
    message_media_controller_observe_retry(&application.message_media, requested.retry_at_ms);
    return true;
}

__declspec(noinline) bool ApplicationMediaPump::request_visible_static_media(
    const MediaAssetKey& key, bool* stop_scan,
    bool force_request, bool continue_on_busy) noexcept {
    *stop_scan = false;
    if (visibility_only)
        return true;
    if ((!force_request && media_atlas_find(application.renderer.media_atlas, key).ok()) || asset_backed_off(key))
        return true;
    return settle_media_request(
        account_module_media_request(application.account.instance, key, now_ms, now_seconds), key, stop_scan, !continue_on_busy);
}

__declspec(noinline) bool ApplicationMediaPump::settle_media_request(
    const AccountMediaRequestResult& requested, const MediaAssetKey& key,
    bool* stop_scan, bool stop_on_busy) noexcept {
    if (requested.state == AccountMediaRequestState::ready) {
        const FrameTake taken = take_ready_frame(*this, key, false, true);
        if (taken == FrameTake::failed)
            return false;
        if (taken == FrameTake::deferred)
            return true;
        note_frame_uploaded(*this);
        if (message_media_controller_upload_budget_reached(&application.message_media, uploads)) {
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
                                                                           bool* stop_scan,
                                                                           bool force_request,
                                                                           bool animate) noexcept {
    if (!application.account_ui) {
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
    if ((resident.ok() && (!force_request || !media_atlas_decoded_animation(
                                                 *application.renderer.media_atlas, resident.handle))) ||
        asset_backed_off(key))
        return true;
    const AccountMediaGuildIconRequest request{icon_hash, key, {0, 0}};
    return settle_media_request(account_module_media_guild_icon_request(
                                    application.account.instance, &request, now_ms, now_seconds),
                                key, stop_scan, true);
}

__declspec(noinline) bool ApplicationMediaPump::request_visible_user_avatar_value(
    u64 user_id, const Utf8View& avatar_hash,
    bool* stop_scan, bool force_request) noexcept {
    *stop_scan = false;
    // No hash: the CDN's default avatar, the one the user's id picks.
    if (user_id != 0 && avatar_hash.bytes == 0)
        return request_visible_default_avatar(default_avatar_key(default_avatar_index(user_id)), stop_scan);
    const u32 variant = user_avatar_variant(avatar_hash);
    if (user_id == 0 || avatar_hash.bytes == 0 || variant == 0)
        return application.account_ui != nullptr;
    return request_visible_hashed_identity(
        media_asset_user_avatar(user_id, variant, asset_hash_animated(avatar_hash)), avatar_hash, stop_scan, force_request);
}

__declspec(noinline) bool ApplicationMediaPump::request_visible_default_avatar(const MediaAssetKey& key,
                                                                                bool* stop_scan) noexcept {
    *stop_scan = false;
    if (!application.account_ui)
        return false;
    // A default avatar is one of six shared pictures: skip it unless a visible surface shows it.
    if (visibility_only || !is_default_avatar_key(key) || !media_key_is_current(key))
        return true;
    ProfileMedia media{};
    media.key = key;
    constexpr auto host = utf8_literal("https://cdn.discordapp.com/");
    char path[kDefaultAvatarPathBytes];
    default_avatar_path(static_cast<u32>(key.id - kDefaultAvatarKeyBase), path);
    copy_bytes(media.url.data, host.data, host.bytes);
    copy_bytes(media.url.data + host.bytes, path, kDefaultAvatarPathBytes);
    media.url.bytes = static_cast<u16>(host.bytes + kDefaultAvatarPathBytes);
    return request_profile_media(media, stop_scan, false, nullptr);
}

__declspec(noinline) bool ApplicationMediaPump::request_visible_avatar_media(const MediaAssetKey& key,
                                                                              const Utf8View& hash,
                                                                              bool* stop_scan,
                                                                              bool force_request) noexcept {
    return is_default_avatar_key(key) ? request_visible_default_avatar(key, stop_scan)
                                      : request_visible_user_avatar_value(key.id, hash, stop_scan, force_request);
}

u32 ApplicationMediaPump::identity_cdn_size() const noexcept {
    return cdn_image_size(kCdnIdentityDisplayDips, application.renderer.dpi);
}

__declspec(noinline) bool ApplicationMediaPump::request_visible_hashed_identity(
    const MediaAssetKey& key, const Utf8View& hash,
    bool* stop_scan, bool force_request) noexcept {
    if (!application.account_ui) {
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
    if ((resident.ok() && (!force_request || !media_atlas_decoded_animation(
                                                 *application.renderer.media_atlas, resident.handle))) ||
        asset_backed_off(key))
        return true;
    // The account ABI keeps this historical name, but the request is the
    // generic hash-addressed CDN asset path. The asset key selects
    // /avatars or /channel-icons rather than /icons in the account module;
    // reserved[0] asks for the CDN size covering the displayed extent at this DPI.
    const AccountMediaGuildIconRequest request{hash, key, {identity_cdn_size(), 0}};
    return settle_media_request(account_module_media_guild_icon_request(
                                    application.account.instance, &request, now_ms, now_seconds),
                                key, stop_scan, true);
}

__declspec(noinline) bool ApplicationMediaPump::request_visible_user_avatar(const UserRecord& user,
                                                                            bool* stop_scan,
                                                                            bool force_request) noexcept {
    return request_visible_user_avatar_value(user.id, user_store_avatar_hash(user), stop_scan, force_request);
}

__declspec(noinline) void ApplicationMediaPump::log_message_media_failure(
    const char* item, const AccountMediaRequestResult& failure, const MediaAssetKey& key) noexcept {
    if (!item || (failure.state != AccountMediaRequestState::failed &&
                  failure.state != AccountMediaRequestState::invalid &&
                  failure.state != AccountMediaRequestState::unsupported)) {
        return;
    }
    const u64 request[] = {static_cast<u64>(key.kind),
                           key.id,
                           static_cast<u64>(failure.state),
                           static_cast<u64>(failure.error),
                           static_cast<u64>(failure.client_error),
                           static_cast<u64>(failure.http_error),
                           static_cast<u64>(failure.attachment_url_error),
                           static_cast<u64>(failure.media_url_error),
                           static_cast<u64>(failure.target_error),
                           static_cast<u64>(failure.png_error),
                           static_cast<u64>(failure.webp_error),
                           static_cast<u64>(failure.jpeg_error)};
    LogEvent line(item);
    line.fields("asset_kind\0asset_id\0request_state\0request_error\0client_error\0http_error\0attachment_url_error\0"
                "media_url_error\0target_error\0png_error\0webp_error\0jpeg_error",
                request);
    // The rarer decoders' errors only when they failed.
    const u64 details[] = {static_cast<u64>(failure.attachment_query_error), static_cast<u64>(failure.video_failure),
                           static_cast<u64>(failure.gif_error), static_cast<u64>(failure.animated_webp_error),
                           static_cast<u64>(failure.lossy_webp_error)};
    const char* keys = "attachment_query_error\0video_decode_failure\0gif_error\0animated_webp_error\0lossy_webp_error";
    for (const u64 value : details) {
        if (value)
            line.field(keys, value);
        while (*keys++) {
        }
    }
    line.write(LogLevel::warning, "media");
}

__declspec(noinline) void ApplicationMediaPump::trace_first_media_upload() noexcept {
    if (application.load_trace.channel_started && !application.load_trace.media_uploaded) {
        application.load_trace.media_uploaded = true;
        application_trace_load_stage("first_media_uploaded");
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

bool ApplicationMediaPump::message_image_source_is_current(const MediaAssetKey& key, u64 source_message_id,
                                                           u32 source_message_revision) noexcept {
    if (!media_key_is_current(key))
        return false;
    // Forum thumbnails come from a post page, not the message store.
    if (forum_thumbnail_key_current(*this, key))
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
                    const auto format = attachment_format_for_key(key, *source.rich_store, item);
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
            const MediaAttachmentFormat format =
                attachment_format_for_key(key, application.account_ui->message_rich_store, attachment);
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
    float preview_height, bool* stop_scan,
    const MessageRichStore* search_store, bool search_snapshot) noexcept {
    if (!application.account_ui) {
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
    const MediaAttachmentFormat format = message_rich_attachment_format(store, attachment);
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
    if (message_media_resident(*this, message, key, (key.flags & media_asset_flag_animated) != 0,
                               format == MediaAttachmentFormat::mp4))
        return true;
    if (visibility_only || asset_backed_off(key))
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
    const AccountMediaRequestResult requested = account_module_media_attachment_request(
        application.account.instance, &request, now_ms, now_seconds);
    log_message_media_failure("attachment_request_failed", requested, key);
    const bool target_failed =
        requested.state == AccountMediaRequestState::failed && requested.error == AccountMediaError::target;
    if (requested.state == AccountMediaRequestState::refresh_required || target_failed) {
        if (search_store) {
            message_media_controller_observe_retry(&application.message_media, requested.retry_at_ms);
            return true;
        }
        if (target_failed)
            log_u64(LogLevel::warning, "media", "attachment_url_refresh_error",
                    static_cast<u64>(requested.attachment_url_error));
        result.history_requested =
            message_history_attachment_queue(&application.attachment_refresh, message.channel_id,
                                             message.id, message.revision, attachment.id) ||
            result.history_requested;
        *stop_scan = true;
        return true;
    }
    return settle_message_media(requested, key, message.id, stop_scan);
}

__declspec(noinline) bool ApplicationMediaPump::settle_message_media(
    const AccountMediaRequestResult& requested, const MediaAssetKey& key, u64 message_id, bool* stop_scan) noexcept {
    if (application.image_viewer.scene.active &&
        (requested.state == AccountMediaRequestState::unsupported ||
         (requested.state == AccountMediaRequestState::failed && requested.error != AccountMediaError::target)))
        application_image_viewer_request_failed(key);
    if (requested.state == AccountMediaRequestState::queued) {
        // At most one speculative miss per pump; never fill every free lane
        // ahead of content that becomes visible on the next scroll update.
        if (prefetch_only)
            *stop_scan = true;
    }
    if (requested.state == AccountMediaRequestState::ready) {
        AccountMediaFrame frame;
        bool geometry_changed = false;
        const FrameTake taken = take_ready_frame(*this, key, true, true, &frame);
        if (taken == FrameTake::failed) {
            return false;
        }
        if (taken == FrameTake::deferred) {
            *stop_scan = true;
            return true;
        }
        if (!update_media_display_dimensions(frame, &geometry_changed)) {
            return false;
        }
        if (!prefetch_only)
            observe_message_animation(application, key, message_id);
        note_frame_uploaded(*this);
        if (message_media_controller_upload_budget_reached(&application.message_media, uploads)) {
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
    float preview_height, bool* stop_scan,
    bool thumbnail, const MessageRichStore* search_store) noexcept {
    if (!application.account_ui || embed_index >= rich.embed_count) {
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
        application.renderer.dpi, media_preview_extent_cap(selected.format, selected.animated));
    const MediaAssetKey key =
        search_store ? media_asset_search_embed(message.id, message.revision, static_cast<u8>(embed_index),
                                                selected.resource, selected.format, selected.animated,
                                                proxy.resolution, rich.forwarded_snapshot_child)
                     : media_asset_embed(message.id, message.revision, static_cast<u8>(embed_index),
                                         selected.resource, selected.format, selected.animated,
                                         proxy.resolution, rich.forwarded_snapshot_child);
    if (message_media_resident(*this, message, key, selected.animated,
                               selected.format == MediaAttachmentFormat::mp4 &&
                                   rich.embeds[embed_index].type != GatewayMessageEmbedType::gifv))
        return true;
    if (visibility_only)
        return true;
    Utf8View request_source = selected.source;
    u8 validated_target[kCdnMaximumTargetBytes];
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
    if (asset_backed_off(key))
        return true;
    const AccountMediaAttachmentRequest request{resolved,
                                                key,
                                                message.id,
                                                message.revision,
                                                proxy.width,
                                                proxy.height,
                                                cdn_image_type(selected.format),
                                                {static_cast<u8>(selected.format == MediaAttachmentFormat::mp4 &&
                                                     rich.embeds[embed_index].type != GatewayMessageEmbedType::gifv),
                                                 {0, 0}},
                                                application.renderer.graphics.device};
    const AccountMediaRequestResult requested = account_module_media_attachment_request(
        application.account.instance, &request, now_ms, now_seconds);
    log_message_media_failure("embed_media_request_failed", requested, key);
    return settle_message_media(requested, key, message.id, stop_scan);
}

bool ApplicationMediaPump::request_search_previews(bool* stop_scan) noexcept {
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
                                            ui_rect_width(rectangle), ui_rect_height(rectangle), &stop, source.rich_store,
                                            target.snapshot))
                return false;
        } else if (!request_visible_embed_media(*source.message, *source.rich, target.rich_index,
                                                ui_rect_width(rectangle), ui_rect_height(rectangle), &stop,
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
                        auto target = application_search_rich_target(
                            search, source.message->channel_id, source.message->id, ticket.index,
                            kind ? MessageSceneRichItemKind::embed : MessageSceneRichItemKind::attachment,
                            index, scope != 0);
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
                                    ui_rect_width(rectangle), ui_rect_height(rectangle), &stop, source.rich_store, scope != 0))
                                return false;
                        } else if (!request_visible_embed_media(
                                       *source.message, *source.rich, index, ui_rect_width(rectangle),
                                       ui_rect_height(rectangle),
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

bool ApplicationMediaPump::request_search_decorations(bool* stop_scan) noexcept {
    *stop_scan = false;
    if (!search_media_visible() || prefetch_only)
        return true;
    const auto& search = *application.renderer.search_scene;
    MessageMediaControllerCallbacks callbacks{};
    callbacks.context = this;
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
                const auto target = application_search_rich_target(
                    search, source.message->channel_id, source.message->id, ticket.index,
                    MessageSceneRichItemKind::sticker, index, scope != 0);
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

bool ApplicationMediaPump::request_chip_guild_icons(bool search, bool* stop_scan) noexcept {
    *stop_scan = false;
    const auto* text = application.renderer.content_text;
    const auto* state = application.account_ui ? &application.account_ui->chat_state : nullptr;
    if (!text || !state || !text->content_semantics || !text->content_hit_regions || prefetch_only ||
        (search && !search_media_visible()) || (!search && !application.message_scene.active))
        return true;
    for (u32 i = 0; i < text->content_hit_region_count; ++i) {
        const auto& region = text->content_hit_regions[i];
        if (region.semantic_index >= text->content_semantic_count)
            continue;
        const auto& semantic = text->content_semantics[region.semantic_index];
        const UiRect cell{region.left, region.top, region.right, region.bottom};
        const bool game = semantic.kind == MessageMarkdownSemanticKind::game_icon;
        const bool author = semantic.kind == MessageMarkdownSemanticKind::author_avatar;
        if ((!game && !author && semantic.kind != MessageMarkdownSemanticKind::guild_icon) || (semantic.reserved & 2u) ||
            ((semantic.reserved & kRendererSearchSemanticFlag) != 0) != search ||
            !ui_rect_has_area(search ? ui_search_scene_content_clip(*application.renderer.search_scene, cell)
                                     : ui_rect_intersection(application.message_scene.viewport, cell)))
            continue;
        // A shown game mention asks for its game, then for its icon (32 px).
        if (game) {
            game_lookup_want(semantic.target);
            GameImageMedia icon;
            if (!game_chip_icon_media(application_games(), semantic.target, &icon))
                continue;
            ProfileMedia media{};
            media.key = icon.key;
            media.url.bytes = static_cast<u16>(icon.url_bytes);
            copy_bytes(media.url.data, icon.url, icon.url_bytes);
            if (!request_profile_media(media, stop_scan))
                return false;
            if (*stop_scan)
                return true;
            continue;
        }
        // A compact row's author avatar, as the row's own avatar would ask for it.
        if (author) {
            const UserRecord* user = application.account_ui
                                         ? user_store_find(application.account_ui->user_store, semantic.target)
                                         : nullptr;
            if (user && !request_visible_user_avatar(*user, stop_scan, false))
                return false;
            if (*stop_scan)
                return true;
            continue;
        }
        const GuildRecord* guild = chat_state_store_find_guild(*state, semantic.target);
        if (!guild || !request_visible_guild_icon(*guild, stop_scan, false,
                                                  false))
            continue;
        if (*stop_scan)
            return true;
    }
    return true;
}

bool ApplicationMediaPump::request_controller_attachment(void* context, const MessageRecord& message,
                                                         const MessageRichAttachment& attachment,
                                                         float preview_width, float preview_height,
                                                         bool* stop_scan) noexcept {
    auto* pump = static_cast<ApplicationMediaPump*>(context);
    return pump &&
           pump->request_visible_attachment(message, attachment, preview_width, preview_height, stop_scan);
}

bool ApplicationMediaPump::request_controller_embed(void* context, const MessageRecord& message,
                                                    const MessageRichRecord& rich, u32 embed_index,
                                                    float preview_width, float preview_height,
                                                    bool* stop_scan) noexcept {
    auto* pump = static_cast<ApplicationMediaPump*>(context);
    if (!pump ||
        !pump->request_visible_embed_media(message, rich, embed_index, preview_width, preview_height, stop_scan))
        return false;
    if (*stop_scan)
        return true;
    float width = 0, height = 0;
    const auto& store = pump->application.account_ui->message_rich_store;
    // A merged gallery image's own corner thumbnail is never drawn.
    return message_rich_embed_merge_host(store, rich, embed_index) != embed_index ||
           !message_rich_embed_thumbnail_extent(store, rich.embeds[embed_index], 80, &width, &height) ||
           pump->request_visible_embed_media(message, rich, embed_index, width, height,
                                                    stop_scan, true);
}

bool ApplicationMediaPump::request_controller_avatar(void* context, const UserRecord& user,
                                                     bool force_request, bool* stop_scan) noexcept {
    auto* pump = static_cast<ApplicationMediaPump*>(context);
    return pump &&
           pump->request_visible_user_avatar(user, stop_scan, force_request);
}

bool ApplicationMediaPump::request_controller_asset(void* context, MediaAssetKey key, bool force_request,
                                                    bool* stop_scan) noexcept {
    auto* pump = static_cast<ApplicationMediaPump*>(context);
    return pump &&
           pump->request_visible_static_media(key, stop_scan, force_request);
}

namespace {

// A path being built in place; every piece stops at the end of the buffer.
struct PathBuilder {
    wchar_t* path;
    u32 used;
    __declspec(noinline) void append(const wchar_t* text) noexcept {
        while (*text && used + 1 < MAX_PATH)
            path[used++] = *text++;
        path[used] = 0;
    }
    __declspec(noinline) void append_hex(u64 value, u32 digits) noexcept {
        for (u32 i = digits; i-- && used + 1 < MAX_PATH;)
            path[used++] = L"0123456789abcdef"[(value >> (i * 4)) & 15];
        path[used] = 0;
    }
};

} // namespace

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
#if defined(_WIN32)
    wchar_t path[MAX_PATH]{};
    const u32 base = local_data_root(path, MAX_PATH);
    if (!base || base >= MAX_PATH - 96)
        return {};
    PathBuilder builder{path, base};
    if (path[builder.used - 1] != L'\\')
        builder.append(L"\\");
    builder.append(L"MaiCord");
    dir_create(path);
    builder.append(L"\\notification-avatars");
    dir_create(path);
#else
    // The files can be made again: they live in the cache directory ($XDG_CACHE_HOME/maicord/notification-avatars).
    wchar_t path[MAX_PATH]{};
    const DataPathResult directory = cache_path(path, MAX_PATH - 32, L"notification-avatars", true);
    if (!directory.ok())
        return {};
    PathBuilder builder{path, directory.length};
#endif
    builder.append(L"\\");
    builder.append_hex(user_id, 16);
    builder.append(L"-");
    builder.append_hex(variant, 8);
    builder.append(L".png");
    // One file per user and avatar hash: an existing file already holds these pixels.
    FileStat cached{};
    if (!file_path_stat(path, &cached)) {
        const u32 pixels_bytes = resident.width * resident.height * 4u;
        const u64 png_bytes = clipboard_image_png_bytes(resident.width, resident.height);
        if (!png_bytes || png_bytes > 1u << 20)
            return {};
        auto* memory = static_cast<u8*>(commit_pages(pixels_bytes + static_cast<SIZE_T>(png_bytes)));
        if (!memory)
            return {};
        u32 width = 0, height = 0;
        bool written = renderer_read_media_rgba(&g_app.renderer, key, {memory, pixels_bytes}, &width, &height) &&
                       clipboard_image_encode_png(memory, width, height, width * 4,
                                                  {memory + pixels_bytes, static_cast<u32>(png_bytes)}) ==
                           clipboard_image_png_bytes(width, height);
        if (written) {
            const HANDLE file = file_open(path, {FileAccess::write, FileCreate::create_new, 0, 0});
            u32 count = 0;
            const u32 bytes = static_cast<u32>(clipboard_image_png_bytes(width, height));
            written = file != INVALID_HANDLE_VALUE &&
                      file_write(file, memory + pixels_bytes, bytes, &count) && count == bytes;
            if (file != INVALID_HANDLE_VALUE)
                file_close(file);
            if (!written)
                file_remove(path);
        }
        pages_release(memory);
        if (!written)
            return {};
    }
#if defined(_WIN32)
    // file:///C:/... with forward slashes; toasts reject '?', '#' and backslashes.
    static constexpr char prefix[] = "file:///";
    copy_bytes(storage.data, prefix, sizeof(prefix) - 1);
    const int converted = utf16_to_utf8(path, static_cast<int>(builder.used), storage.data + sizeof(prefix) - 1, static_cast<int>(storage.count - (sizeof(prefix) - 1)), false);
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
#else
    // file:// and the absolute path, every byte a path name does not carry as it is written %XX.
    char utf8[MAX_PATH * 3];
    if (!path_to_utf8(path, utf8, sizeof(utf8)) || utf8[0] != '/')
        return {};
    static constexpr char prefix[] = "file://";
    copy_bytes(storage.data, prefix, sizeof(prefix) - 1);
    u32 bytes = sizeof(prefix) - 1;
    for (const char* c = utf8; *c; ++c) {
        const char value = *c;
        const bool plain = (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
                           (value >= 'a' && value <= 'z') || value == '/' || value == '-' || value == '.' ||
                           value == '_' || value == '~';
        if (bytes + (plain ? 1 : 3) >= storage.count)
            return {};
        if (plain) {
            storage.data[bytes++] = value;
        } else {
            storage.data[bytes++] = '%';
            storage.data[bytes++] = "0123456789ABCDEF"[static_cast<u8>(value) >> 4];
            storage.data[bytes++] = "0123456789ABCDEF"[static_cast<u8>(value) & 15];
        }
    }
    return {storage.data, bytes};
#endif
}

} // namespace maicord::app_detail
