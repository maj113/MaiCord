#include "maicord/model/guild/scheduled_event_store.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/guild/invite_card_cache.hpp"
#include "maicord/ui/chat/messages/component_media.hpp"
#include "requests_internal.hpp"
#include "../chat/channel_authority.hpp"
#include "../account/profile.hpp"
#include "../guild/banner.hpp"
#include "../account/profile_editor.hpp"
#include "picker.hpp"
#include "../chat/search/search.hpp"
#include "image_viewer.hpp"
#include "../chat/threads/forum_preview.hpp"
#include "../chat/composer/preview.hpp"
#include "../chat/threads/forum_form.hpp"
#include "../chat/composer/commands.hpp"
#include "../chat/game_lookup.hpp"
#include "../chat/composer/game_search.hpp"
#include "maicord/ui/chat/game_media.hpp"

#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/media/core/animation_timeline.hpp"
#include "maicord/media/core/presentation_policy.hpp"
#include "maicord/ui/core/geometry.hpp"
#include "maicord/ui/account/profile_appearance.hpp"
#include "maicord/media/image/dominant_color.hpp"
#include "maicord/ui/chat/private_channel_rows.hpp"
#include "maicord/ui/shell/popup_presentation.hpp"
#include "maicord/ui/text/regional_flag.hpp"
#include "maicord/ui/chat/search/avatar_media.hpp"
#include "maicord/ui/voice/channel_rows.hpp"
#include "maicord/ui/shell/quick_switcher_scene.hpp"
#include "maicord/ui/chat/inbox_avatar_media.hpp"
#include "maicord/ui/chat/polls/voters_avatar_media.hpp"
#include "../../render/d3d11/renderer_interaction_state.hpp"

namespace maicord::app_detail {
namespace {
// The official client's shared avatar rule: an animated avatar
// plays while it is hovered, or, when GIFs autoplay, while its row (a message
// group, a member or DM row, the account panel) is hovered. Elsewhere it
// holds its current frame; nothing plays while the window is unfocused.
bool avatar_row_animates(bool row_hovered) noexcept {
    return row_hovered && message_rich_display().gifs == MessageMediaAnimation::always;
}

// The first message of the hovered message's group (whose avatar shows).
u64 animated_avatar_message(const Application& application) noexcept {
    const MessageScene& scene = application.message_scene;
    const u64 hovered = application.message_action_bar.message_id;
    if (!scene.active || !scene.timeline || !scene.timeline->store || !avatar_row_animates(hovered != 0))
        return application.message_media.avatar_hover_message_id;
    for (u32 i = 0; i < scene.count; ++i) {
        const MessageRecord* record = message_store_resolve(*scene.timeline->store, scene.rows[i].message);
        if (!record || record->id != hovered)
            continue;
        u32 head = i;
        while (head > 0 && scene.rows[head].grouped)
            --head;
        const MessageRecord* first = message_store_resolve(*scene.timeline->store, scene.rows[head].message);
        return first ? first->id : 0;
    }
    return 0;
}
} // namespace

// The saved avatar's colours a card needs, read once a still picture of the avatar is resident (the official client
// reads them from the static avatar URL): the banner's fill when the profile names no colour and shows no banner
// picture (the avatar as drawn), and an avatar-themed card's two theme colours (the avatar at 80 DIPs; the card is
// laid out again with them). The pictures are fetched until then, and not drawn. The banner never reads a picked,
// unsaved avatar; the preview's theme does (below).
bool ApplicationMediaPump::read_avatar_colors(ProfileScene& scene, bool* stop) noexcept {
    if (!scene.active || !scene.profile)
        return true;
    u32 colors[kImagePaletteColors];
    ProfileMedia still;
    const float drawn = ui_rect_width(scene.avatar);
    if (!scene.has_banner_color && !scene.pending_banner.active && !profile_display(*scene.profile).banner.bytes &&
        drawn > 0 && profile_avatar_still_media(scene, drawn, &still)) {
        if (!media_asset_key_equal(still.key, scene.avatar_color_key)) {
            scene.avatar_color_key = still.key;
            scene.has_avatar_color = false;
        }
        if (!scene.has_avatar_color) {
            if (renderer_media_palette(&application.renderer, still.key, colors)) {
                scene.avatar_color = colors[0];
                scene.has_avatar_color = true;
                if (repaint)
                    *repaint = true;
            } else if (!request_profile_media(still, stop))
                return false;
        }
    }
    // The editor's preview themes from a picked, unsaved avatar as it is cropped (the official client reads the
    // pending avatar), drawn at the size the saved one is read at.
    const bool picked = scene.avatar_themed && scene.presentation == ProfilePresentation::preview &&
                        scene.pending_avatar.active;
    RendererPaletteCrop crop{};
    u64 signature = 0;
    if (picked) {
        const auto out = image_crop_output(scene.pending_avatar.crop);
        const u32 dpi = scene.media_dpi ? scene.media_dpi : 96u;
        crop = {out.x, out.y, out.width, out.height, scene.pending_avatar.crop.image_width,
                (static_cast<u32>(kProfileAvatarThemeDips) * dpi + 95) / 96, out.rotation};
        const u32 fields[]{crop.x, crop.y, crop.width, crop.height, crop.source_width, crop.size, crop.rotation};
        signature = hash64_bytes(kHash64Seed, fields, sizeof(fields)) | 1;
        still.key = scene.pending_avatar.key;
    }
    if (scene.avatar_themed && (picked || profile_avatar_still_media(scene, kProfileAvatarThemeDips, &still))) {
        if (!media_asset_key_equal(still.key, scene.avatar_theme_key) || signature != scene.avatar_theme_crop) {
            scene.avatar_theme_key = still.key;
            scene.avatar_theme_crop = signature;
            scene.has_avatar_theme = false;
        }
        if (!scene.has_avatar_theme) {
            // A picked avatar is already resident in the preview atlas (the card draws it).
            if (const u32 count = picked ? renderer_media_crop_palette(&application.renderer, still.key, crop, colors)
                                         : renderer_media_palette(&application.renderer, still.key, colors)) {
                // A one-colour palette pairs its colour with a companion.
                scene.avatar_theme[0] = colors[0];
                scene.avatar_theme[1] = count > 1 ? colors[1] : image_palette_companion(colors[0]);
                scene.has_avatar_theme = true;
                if (!profile_scene_layout(&scene, scene.viewport, true))
                    return false;
                result.text_changed = true;
                if (repaint)
                    *repaint = true;
            } else if (!picked && !request_profile_media(still, stop))
                return false;
        }
    }
    return true;
}

__declspec(noinline) bool ApplicationMediaPump::pump_visible_static_media(bool* repaint) noexcept {
    if (repaint) {
        *repaint = false;
    }
    if (!application.account.instance || !application.account_ui || !application.renderer.media_atlas ||
        !application.renderer.preview_media_atlas) {
        return true;
    }
#if !defined(_WIN32)
    // Lossy WebP decodes on the CPU when this GPU has no VP8 path (renderer_vp8_gpu), before any request below.
    account_module_media_vp8_cpu(application.account.instance, !renderer_vp8_gpu(application.renderer));
#endif
    const u64 now_ms = clock_monotonic_milliseconds(), now_seconds = clock_unix_milliseconds() / 1'000;
    pass(now_ms, now_seconds, 0, repaint);
    // Interaction-only chat animations play for the hovered, else focused, message.
    message_media_controller_set_interaction(&application.message_media,
                                             application.message_action_bar.message_id
                                                 ? application.message_action_bar.message_id
                                                 : application.message_scene.focused_message_id);
    message_media_controller_set_avatar_interaction(&application.message_media, animated_avatar_message(application));
    // Editor preview exposes the main canvas without cancelling Settings' scope
    // or read-ack barrier. Media uses the same rendering predicate as paint:
    // a modal or dialog keeps the shell beneath it visible and animating.
    const bool settings_visible = renderer_settings_page_visible(application.renderer);
    const bool shell_hidden = renderer_settings_hides_shell(application.renderer);
    message_media_controller_begin_pump(&application.message_media);
    uploads = 0;
    const u64 visibility_now_us =
        clock_microseconds(application.clock, application.process_start, clock_now());
    animation_timeline_begin_visibility(&application.animations.timeline);
    // Gallery item lifetimes never return after their accepted source bank is
    // replaced. Cancel those catalog entries once per visibility epoch,
    // while keeping current-bank offscreen entries for ordinary pause/resume.
    // Only occupied entries can match; stop once every live entry was visited.
    for (u32 slot = 0, live = application.animations.timeline.entry_count;
         live && slot < application.animations.timeline.capacity; ++slot) {
        const auto& entry = application.animations.timeline.entries[slot];
        if (!(entry.flags & animation_timeline_entry_occupied))
            continue;
        --live;
        if (entry.key.kind != MediaAssetKind::catalog_preview && !media_asset_search(entry.key))
            continue;
        const auto* visual = find_animation_visual(entry.key, entry.generation);
        if (!visual || (media_asset_search(entry.key)
                            ? !message_image_source_is_current(entry.key, visual->source_scope,
                                                               visual->source_revision)
                            : !application_media_picker_preview_owner_current(
                                  entry.key, visual->source_scope, visual->source_revision)))
            release_animation_visual(entry.key, entry.generation);
    }
    if (!shell_hidden && !application.login.visible) {
        visibility_only = true;
        const bool observed = scan_visible_media(visibility_now_us);
        visibility_only = false;
        if (!observed)
            return false;
        message_media_controller_begin_asset_scan(&application.message_media);
    }
    // Opaque login/settings commits an empty visible set. Reduced motion keeps
    // the visible set but the existing suspended timeline accepts no work.
    animation_timeline_end_visibility(&application.animations.timeline, visibility_now_us);
    // Queue cold picker sources before frame advances can occupy the download
    // workers. This limits first-paint latency while existing animations play.
    if (!shell_hidden && !application.login.visible &&
        (!application_forum_preview_request(now_ms, now_seconds) ||
         !application_composer_preview_request(now_ms, now_seconds)))
        return false;
    if (!shell_hidden && !application.login.visible &&
        (!request_catalog_previews() ||
         !request_picker_emoji()))
        return false;
    if (!pump_due_animations(repaint)) {
        return false;
    }
    // Collect every finished worker result in one UI pump. Upload stays
    // limited, and animation completions are routed back to their timeline
    // advance rather than being mistaken for an unrelated static image.
    for (u32 completions = 0;
         completions < kMessageMediaCompletionDrainBudget && uploads < kMessageMediaUploadBudget;
         ++completions) {
        AccountMediaFrame pending{};
        if (!account_module_media_take(application.account.instance, &pending)) {
            break;
        }
        if (pending.source_kind == AccountMediaSourceKind::local_draft_preview) {
            if (!application_local_preview_frame_current(pending))
                continue;
            const auto upload = upload_account_media_frame(pending);
            if (upload == MediaFrameUploadResult::deferred)
                continue;
            application_local_preview_frame_uploaded(pending);
            ++uploads;
            trace_first_media_upload();
            if (repaint)
                *repaint = true;
            continue;
        }
        bool animation_consumed = false;
        if (!consume_pending_animation_frame(pending, repaint, &animation_consumed)) {
            return false;
        }
        if (animation_consumed) {
            continue;
        }
        // A cancelled cold advance may still finish in its borrowed codec worker.
        // Only a pending request that is still current can accept later animation frames.
        if (pending.frame_index &&
            (pending.kind == AccountMediaFrameKind::lottie_paint ||
             pending.kind == AccountMediaFrameKind::apng_scanlines ||
             pending.kind == AccountMediaFrameKind::gif_indices || animated_webp_frame_kind(pending.kind) ||
             video_frame_kind(pending.kind)))
            continue;
        if (!message_image_frame_is_current(pending)) {
            continue;
        }
        // The worker retains the encoded source. This completion only wakes
        // the selected player; audio has no pixels or display dimensions.
        if (pending.kind == AccountMediaFrameKind::audio_source) {
            if (repaint)
                *repaint = true;
            continue;
        }
        const MediaFrameUploadResult upload = upload_account_media_frame(pending);
        if (upload == MediaFrameUploadResult::deferred) {
            continue;
        }
        bool geometry_changed = false;
        if (pending.key.kind != MediaAssetKind::catalog_preview &&
            !update_media_display_dimensions(pending, &geometry_changed)) {
            return false;
        }
        ++uploads;
        trace_first_media_upload();
        if (geometry_changed) {
            message_media_controller_defer_scan(&application.message_media);
        }
        if (repaint) {
            *repaint = true;
        }
    }
    if (settings_visible && !application.login.visible &&
        (!request_inline_flags() ||
         !request_settings_avatars()))
        return false;
    if (application.login.visible || shell_hidden)
        return true;
    return scan_visible_media(clock_microseconds(application.clock, application.process_start, clock_now()));
}

__declspec(noinline) bool ApplicationMediaPump::scan_visible_media(u64 us) noexcept {
    now_us = us;
    if (!visibility_only && uploads >= kMessageMediaUploadBudget)
        return true;
    const bool chat_visible =
        content_media_visible() && application.account_ui &&
        application_channel_readable(application, application.account_ui->active_channel_id);
    const MessageTimeline& timeline = application.message_view.timeline;
    const UiVariableRange range =
        chat_visible ? ui_variable_list_range(application.message_view.list, timeline.scroll_units,
                                              timeline.viewport_units, 1)
                     : UiVariableRange{};
    if (!request_catalog_previews())
        return false;
    if (!request_picker_emoji())
        return false;
    if (!request_inline_flags())
        return false;
    if (!visibility_only && uploads >= kMessageMediaUploadBudget)
        return true;
    // `stop` is the requests' stop-scan flag and stays false while the scan goes on. When proceed()
    // returns false the scan ends with `outcome` (false: a request failed).
    bool stop = false, outcome = true;
    const auto proceed = [&](bool ok) noexcept {
        outcome = ok;
        return ok && !stop;
    };
    // Requests a card's plain media parts (no banner fallback handling).
    const auto request_parts = [&](const ProfileScene& scene) noexcept {
        for (u32 i = 0; i < scene.part_count; ++i) {
            ProfileMedia media;
            if (!profile_media_resolve(scene, i, &media) || media.domain || !media.url.bytes)
                continue;
            if (!proceed(request_profile_media(media, &stop)))
                return false;
        }
        return true;
    };
    // Requests a card's effect layers; playback starts once every layer is resident.
    const auto request_effects = [&](ProfileScene& scene) noexcept {
        bool ready = true;
        for (u32 i = 0; scene.effect && i < maximum<u8>(1, scene.effect->count); ++i) {
            ProfileMedia media;
            if (!profile_effect_media_resolve(scene, i, now_ms, &media))
                continue;
            const bool requested = request_profile_media(media, &stop,
                                                         scene.effect_started_ms != 0);
            if (requested)
                ready = ready && media_atlas_find(application.renderer.media_atlas, media.key).ok();
            if (!proceed(requested))
                return false;
        }
        if (!visibility_only && scene.effect && scene.effect->count && !scene.effect_started_ms && ready) {
            scene.effect_started_ms = now_ms + 500;
            // Nothing was uploaded for an effect whose layers were already resident: the frame
            // loop no longer runs for an unstarted effect, so ask for the one that starts it.
            if (repaint)
                *repaint = true;
        }
        return true;
    };
    // Cards that take their avatar's colours, read (and their still avatars requested) before any request can end
    // the scan.
    if (!visibility_only) {
        ProfileScene* const cards[]{application.profile ? &application.profile->scene : nullptr,
                                    content_media_visible() ? application.renderer.dm_profile_panel : nullptr,
                                    application_profile_editor_preview()};
        for (ProfileScene* card : cards)
            if (card && !proceed(read_avatar_colors(*card, &stop)))
                return outcome;
    }
    if (application.profile && application.profile->scene.active) {
        auto& profile = application.profile->scene;
        // Submit the banner and avatar before decorative effect layers can fill the worker queue.
        for (u32 i = 0; i < profile.part_count; ++i) {
            ProfileMedia media;
            if (!profile_media_resolve(profile, i, &media) || media.domain)
                continue;
            AccountMediaRequestResult failed{};
            if (!request_profile_media(media, &stop, true,
                                       &failed))
                return false;
            if (profile.parts[i].media == ProfileMediaKind::banner && !profile.banner_compatibility &&
                (media.key.flags & media_asset_flag_animated) &&
                (failed.state == AccountMediaRequestState::failed ||
                 failed.state == AccountMediaRequestState::unsupported)) {
                // Retry a failed WebP representation once using the CDN's GIF representation.
                log_message_media_failure("profile_banner_webp_failed", failed, media.key);
                // Only the media URL changed. Advancing the text/layout revision here
                // suppresses all retained profile text until an unrelated UI rebuild.
                profile_media_use_compatibility_banner(&profile);
                if (repaint)
                    *repaint = true;
            }
            if (stop)
                return true;
        }
        if (!request_effects(profile))
            return outcome;
    }
    // The selected server's banner: its animation first while it plays, then
    // the still it falls back to. A stopped animation gives back its playback.
    for (MediaAssetKey retired{}; application_guild_banner_take_retired(&retired);) {
        const auto found = animation_timeline_find(&application.animations.timeline, retired);
        if (found.ok() && found.slot < application.animations.timeline.capacity)
            release_animation_visual(retired, application.animations.timeline.entries[found.slot].generation);
    }
    {
        GuildBannerMedia banner[2]{};
        const u32 banner_count = application_guild_banner_media(banner, 2);
        for (u32 i = 0; i < banner_count; ++i) {
            ProfileMedia media{};
            if (banner[i].url.bytes > sizeof(media.url.data))
                continue;
            copy_bytes(media.url.data, banner[i].url.data, banner[i].url.bytes);
            media.url.bytes = static_cast<u16>(banner[i].url.bytes);
            media.key = banner[i].key;
            AccountMediaRequestResult failed{};
            if (!request_profile_media(media, &stop, banner[i].play,
                                       &failed))
                return false;
            if (banner[i].play && (failed.state == AccountMediaRequestState::failed ||
                                   failed.state == AccountMediaRequestState::unsupported))
                application_guild_banner_media_failed(banner[i].key);
            if (stop)
                return true;
        }
    }
    // The DM profile panel beside a one-to-one DM (no profile effect).
    if (const auto* panel = application.renderer.dm_profile_panel; panel && panel->active && content_media_visible())
        if (!request_parts(*panel))
            return outcome;
    // The profile editor's preview card and its picker tiles (CDN images;
    // picked images are local previews and never fetched).
    if (auto* preview = application_profile_editor_preview())
        if (!request_parts(*preview) || !request_effects(*preview))
            return outcome;
    if (const auto* dialog = application_profile_editor_dialog()) {
        ProfileMedia media;
        for (u32 i = 0; ui_profile_dialog_media(*dialog, i, &media); ++i) {
            if (media.domain || !media.url.bytes)
                continue;
            if (!proceed(request_profile_media(media, &stop)))
                return outcome;
        }
    }
    // An expanded attachment is the foreground media consumer. Reuse the
    // normal request/decode/atlas pipeline, including its signed-URL refresh path.
    auto& expanded = application.image_viewer;
    if (expanded.scene.active) {
        ApplicationSearchSourceView source{};
        if (application_image_viewer_source(&source) && source.message && source.rich &&
            source.rich_store) {
            const auto* message = source.message;
            const auto* rich = source.rich;
            const auto& rich_store = *source.rich_store;
            const auto* search_store = expanded.search_result ? source.rich_store : nullptr;
            // Current item first, then bounded gallery neighbours for instant browsing.
            MediaAssetKey requested[3]{};
            const u32 requested_count = application_image_viewer_request_keys(requested, 3);
            for (u32 request = 0; request < requested_count; ++request) {
            const auto viewer_key = requested[request];
            if (viewer_key.kind == MediaAssetKind::embed) {
                const u32 index = media_asset_embed_index(viewer_key);
                if (index < rich->embed_count) {
                    MessageRichEmbedMediaSelection selected{};
                    if (message_rich_embed_resource_select(rich_store, rich->embeds[index],
                                                           media_asset_embed_resource(viewer_key),
                                                           &selected)) {
                        const float extent = static_cast<float>(media_preview_extent_cap(selected.format, selected.animated)) *
                                             96.0f / application.renderer.dpi;
                        if (!proceed(request_visible_embed_media(*message, *rich, index, extent, extent, &stop,
                                                                 selected.resource == MediaEmbedResource::thumbnail,
                                                                 search_store)))
                            return outcome;
                    }
                }
            } else
                for (u32 i = 0; i < rich->attachment_count; ++i)
                    if (rich->attachments[i].id == viewer_key.id) {
                        const float extent =
                            static_cast<float>(media_preview_extent_cap(
                                message_rich_attachment_format(rich_store, rich->attachments[i]))) *
                            96.0f / application.renderer.dpi;
                        if (!proceed(request_visible_attachment(*message, rich->attachments[i], extent, extent, &stop,
                                                                search_store, source.snapshot)))
                            return outcome;
                        break;
                    }
            }
        }
    }
    // A shown media page asks for its picture through its own {url, key}.
    if (!proceed(application_media_page_pump(*this, &stop)))
        return outcome;

    // A command app's picture (the commands module's fallback chain): its bot's
    // avatar through the user store, or a picture fetched by the URL the
    // commands module (or the game search's rows) keeps under the key.
    const auto request_app_picture = [&](const MediaAssetKey& key) noexcept {
        if (key.kind == MediaAssetKind::profile_image) {
            ProfileMedia media{};
            media.key = key;
            media.url.bytes = static_cast<u16>(
                application_commands_icon_url(key.id, {media.url.data, sizeof(media.url.data)}));
            if (!media.url.bytes)
                media.url.bytes = static_cast<u16>(
                    application_game_search_icon_url(key.id, {media.url.data, sizeof(media.url.data)}));
            return !media.url.bytes ||
                   request_profile_media(media, &stop);
        }
        const auto* user =
            key.kind == MediaAssetKind::user_avatar ? user_store_find(application.account_ui->user_store, key.id)
                                                    : nullptr;
        bool force = false;
        const bool resident = (key.flags & media_asset_flag_animated) && animation_resident(key, &force);
        return !user || resident ||
               request_visible_user_avatar(*user, &stop, force);
    };
    // The open picker is an active interaction surface, so its visible custom
    // emoji cells outrank decorative identity media. The CDN serves all
    // in-client custom emoji as WebP; animated cells join the normal bounded
    // animation timeline rather than starting a second playback path.
    if (chat_visible && application.message_composer_scene.active) {
        const auto& composer = application.message_composer_scene;
        if (composer.command.avatar.kind != MediaAssetKind::none &&
            !proceed(request_app_picture(composer.command.avatar)))
            return outcome;
        for (u32 i = 0; i < composer.projection_span_count; ++i) {
            UiRect rectangle{}, clip{};
            const auto& span = composer.projection_spans[i];
            // A game mention in the draft asks for its game, then its icon.
            if (span.kind == MessageComposerProjectionKind::game)
                game_lookup_want(span.target);
            if (!text_composer_emoji_rectangle(application.renderer.composer_layout, composer, i,
                                                 &rectangle, &clip))
                continue;
            if (span.kind == MessageComposerProjectionKind::game) {
                GameImageMedia icon;
                if (!game_chip_icon_media(application_games(), span.target, &icon))
                    continue;
                ProfileMedia media{};
                media.key = icon.key;
                media.url.bytes = static_cast<u16>(icon.url_bytes);
                copy_bytes(media.url.data, icon.url, icon.url_bytes);
                if (!proceed(request_profile_media(media, &stop)))
                    return outcome;
                continue;
            }
            const auto key = span.kind == MessageComposerProjectionKind::regional_flag
                                 ? media_asset_regional_flag(static_cast<u16>(span.target))
                                 : media_asset_custom_emoji(span.target, span.animated);
            bool force = false;
            if (span.animated && animation_resident(key, &force))
                continue;
            if (!proceed(request_visible_static_media(key, &stop, force)))
                return outcome;
        }
    }
    if (application.popup_scene.active) {
        for (u32 index = 0; index < application.popup_scene.item_count; ++index) {
            const PopupItem& item = application.popup_scene.items[index];
            PopupMediaCommand command{};
            if (popup_media_presentation_resolve(application.popup_scene, index, &command) !=
                PopupMediaPresentationStatus::ready) {
                continue;
            }
            const bool animated = (item.media_flags & custom_emoji_animated) != 0;
            const MediaAssetKey key = popup_media_asset_key(command);
            if (command.avatar && (item.media_flags & kPopupItemMediaUrl)) {
                if (!proceed(request_app_picture(key)))
                    return outcome;
                continue;
            }
            bool force_request = false;
            if (animated && animation_resident(key, &force_request))
                continue;
            const auto* user =
                command.avatar
                    ? user_store_find(application.account_ui->user_store, command.media_id)
                    : nullptr;
            const auto* guild = command.guild_icon
                                    ? chat_state_store_find_guild(application.account_ui->chat_state,
                                                                     command.media_id)
                                    : nullptr;
            if (!proceed(command.guild_icon
                             ? (!guild || request_visible_guild_icon(*guild, &stop, force_request))
                         : command.avatar
                             ? (!user || request_visible_user_avatar(*user,
                                                                     &stop, force_request))
                             : request_visible_static_media(key,
                                                            &stop, force_request)))
                return outcome;
        }
    }

    // The reactions modal is virtualized independently from the message
    // scene. Only its visible custom emoji and avatars enter the media queue,
    // so the per-frame request cost matches the visible rows regardless of page size.
    if (application.reaction_viewer.active) {
        const ReactionViewer& viewer = application.reaction_viewer;
        for (u32 index = viewer.first_aggregate; index < viewer.aggregate_count; ++index) {
            if (!ui_rect_has_area(reaction_viewer_aggregate_rectangle(viewer, index))) {
                break;
            }
            const ReactionViewerAggregate& aggregate = viewer.aggregates[index];
            const u16 flag = aggregate.emoji_id == 0
                                 ? regional_flag_sequence(reaction_viewer_aggregate_name(viewer, index))
                                 : 0;
            if (aggregate.emoji_id == 0 && !flag) {
                continue;
            }
            const bool animated = (aggregate.flags & 1u) != 0;
            const MediaAssetKey key = flag ? media_asset_regional_flag(flag)
                                           : media_asset_custom_emoji(aggregate.emoji_id, animated);
            bool force_request = false;
            if (animated && animation_resident(key, &force_request))
                continue;
            if (!proceed(request_visible_static_media(key, &stop,
                                                      force_request)))
                return outcome;
        }
        for (u32 index = viewer.first_user; index < viewer.user_count; ++index) {
            if (!ui_rect_has_area(reaction_viewer_user_rectangle(viewer, index))) {
                break;
            }
            const ReactionViewerUser& user = viewer.users[index];
            const Utf8View hash = reaction_viewer_user_avatar_hash(user);
            const bool animated = asset_hash_animated(hash);
            const MediaAssetKey key =
                media_asset_user_avatar(user.id, user_avatar_variant(hash), animated);
            bool force_request = false;
            if (animated && media_asset_key_valid(key) && animation_resident(key, &force_request))
                continue;
            if (!proceed(request_visible_user_avatar_value(user.id, hash,
                                                           &stop, force_request)))
                return outcome;
        }
    }

    // Message media is the primary content of the selected channel. Service
    // it before identity decoration so a long run of guild/member/avatar
    // misses cannot hold an attachment placeholder on screen indefinitely.
    // The account media worker is single-flight, therefore
    // ordering here is also the request priority.
    const MessageMediaControllerCallbacks message_media_callbacks{
        this, request_controller_attachment, request_controller_embed, request_controller_avatar,
        request_controller_asset};
    if (!proceed(request_friends_avatars(&stop)))
        return outcome;
    // Foreground results take priority for the primary preview before the underlying
    // conversation can occupy the single-flight media worker with a cold miss.
    if (!proceed(request_search_previews(&stop)))
        return outcome;
    // Foreground panels (pins, search) borrow tiny author avatars. Service them
    // before the conversation underneath, whose cold media or playing animations
    // can keep the single-flight worker busy and stop this scan every frame.
    // Pins borrow author hashes from the accepted visible page through
    // the shared avatar cache, including authors absent from the live timeline.
    if (!proceed(request_pinned_messages_avatars(&stop)))
        return outcome;
    if (!proceed(request_forum_avatars(&stop)))
        return outcome;

    // Search uses author hashes from its accepted page, including authors that
    // have never appeared in the selected conversation's user cache, then the
    // people suggestions of its filter chooser.
    if (surfaces() & d3d11_surface_search) {
        const auto& search = *application.renderer.search_scene;
        for (u32 slot = 0; slot < kSearchAvatarMediaSlots; ++slot) {
            AvatarMedia media;
            if (!search_avatar_media_resolve(search, slot, &media))
                continue;
            const ScanStep step = request_scene_avatar(*this, media.key, media.hash, &stop);
            if (step != ScanStep::next)
                return step == ScanStep::stopped;
        }
    }

    // Quick switcher and people picker rows: user avatars and server icons.
    if (surfaces() & d3d11_surface_quick_switcher) {
        const auto& switcher = *application.renderer.quick_switcher_scene;
        const auto& state = application.account_ui->chat_state;
        for (u32 slot = 0; slot < switcher.count; ++slot) {
            UiQuickSwitcherMedia media{};
            if (!ui_quick_switcher_scene_media(switcher, slot, &state, &media))
                continue;
            if (!visibility_only && uploads >= kMessageMediaUploadBudget)
                return true;
            const auto* guild = media.key.kind == MediaAssetKind::guild_icon
                                    ? chat_state_store_find_guild(state, media.key.id)
                                    : nullptr;
            if (!proceed(media.key.kind == MediaAssetKind::guild_icon
                             ? !guild || request_visible_guild_icon(*guild,
                                                                    &stop, false, false)
                             : request_visible_avatar_media(media.key, media.hash, &stop, false)))
                return outcome;
        }
    }

    if (!proceed(request_forward_identities(&stop)))
        return outcome;

    // Inbox mention cards and poll voter rows borrow their page's author hashes.
    if (surfaces() & d3d11_surface_inbox) {
        const auto& inbox = *application.renderer.inbox_scene;
        for (u32 slot = 0; slot < inbox.count && slot < kUiInboxVisibleRows; ++slot) {
            AvatarMedia media;
            if (!inbox_avatar_media_resolve(inbox, slot, &media))
                continue;
            if (!visibility_only && uploads >= kMessageMediaUploadBudget)
                return true;
            if (!proceed(request_visible_avatar_media(media.key, media.hash, &stop, false)))
                return outcome;
        }
    }
    if (surfaces() & d3d11_surface_poll_voters) {
        const auto& voters = *application.renderer.poll_voters_scene;
        for (u32 index = voters.first; index < voters.first + voters.visible; ++index) {
            AvatarMedia media;
            if (!poll_voters_avatar_media_resolve(voters, index, &media))
                continue;
            if (!visibility_only && uploads >= kMessageMediaUploadBudget)
                return true;
            if (!proceed(request_visible_avatar_media(media.key, media.hash, &stop, false)))
                return outcome;
        }
    }

    if (chat_visible) {
        const MessageMediaControllerResult collected = message_media_controller_collect(
            &application.message_media, application.message_view.store,
            application.account_ui->message_rich_store, application.message_scene, range);
        if (!collected.ok()) {
            log_u64(LogLevel::error, "media", "visible_collect_error", static_cast<u64>(collected.error));
            return false;
        }
        const MessageMediaControllerResult primary = message_media_controller_visit_primary(
            &application.message_media, application.account_ui->message_rich_store,
            application.message_scene, message_media_callbacks);
        if (!primary.ok()) {
            log_u64(LogLevel::error, "media", "primary_visit_error", static_cast<u64>(primary.error));
            return false;
        }
        if (primary.stopped)
            return true;
    }

    if (application.guild_rail_scene.active) {
        // Collapsed folder tiles also preview up to four member icons.
        const auto& rail_view = application.account_ui->navigation_view;
        for (u32 visible = 0, preview = 0; visible < application.guild_rail_scene.count; ++visible) {
            const GuildRailRowWidget& row = application.guild_rail_scene.rows[visible];
            u64 guild_id = row.view.id;
            if (row.view.folder_role == GuildFolderRole::header) {
                const u32 source = row.source_index + 1 + preview;
                const bool more = !row.view.folder_expanded && preview < 4 && preview < row.view.folder_size &&
                                  source < rail_view.guild_count;
                guild_id = more ? rail_view.guild_rows[source].id : 0;
                if (more) {
                    ++preview;
                    --visible;
                } else
                    preview = 0;
            }
            const GuildRecord* guild =
                chat_state_store_find_guild(application.account_ui->chat_state, guild_id);
            if (!guild || guild->icon_hash_bytes == 0 || guild->icon_variant == 0) {
                continue;
            }
            // The official client's rail shows the still rendition (a non-animated
            // icon URL: .webp?size=N, the CDN's representative frame) and
            // swaps in the animation only while the server's own row is
            // hovered; folder previews and other rows keep the still.
            if (!proceed(request_visible_guild_icon(*guild, &stop, false,
                                                    false)))
                return outcome;
            const bool hovered = guild_id == row.view.id &&
                                 guild_rail_scene_resolve(application.guild_rail_scene,
                                                          application.guild_rail_scene.widgets.hovered) == &row;
            if (!hovered || !asset_hash_animated(chat_state_store_guild_icon_hash(*guild)))
                continue;
            const MediaAssetKey key = media_asset_guild_icon(guild->id, guild->icon_variant, true);
            bool force = false;
            if (animation_resident(key, &force))
                continue;
            if (!proceed(request_visible_guild_icon(*guild, &stop, force,
                                                    true)))
                return outcome;
        }
    }
    // A held avatar (play false) keeps its current frame.
    const auto request_avatar = [&](const UserRecord* user, bool play) noexcept {
        if (!user)
            return true;
        const Utf8View hash = user_store_avatar_hash(*user);
        const u32 variant = user_avatar_variant(hash);
        if (hash.bytes && !variant)
            return true;
        bool force_request = false;
        if (asset_hash_animated(hash) &&
            animation_resident(media_asset_user_avatar(user->id, variant, true), &force_request,
                               play))
            return true;
        return request_visible_user_avatar(*user, &stop,
                                           force_request);
    };
    // A fullscreen call covers the account panel: its avatar neither loads
    // nor keeps an animated avatar advancing frames underneath.
    const auto* covering_call = application.renderer.call_scene;
    const bool footer_covered = covering_call && covering_call->active && covering_call->fullscreen;
    // The official client's account panel requests a still (non-animated) avatar.
    if (!proceed(footer_covered || request_avatar(user_store_find(application.account_ui->user_store,
                                                                  application.account_ui->chat_state.self_user_id),
                                                  false)))
        return outcome;
    // Call view tiles show avatars until a camera picture is resident. A tile
    // drawing its picture (or a stream tile) shows no avatar, so an animated
    // avatar there must not advance frames nobody sees.
    if (const auto* call = application.renderer.call_scene; call && call->active)
        for (u32 i = 0; i < call->tile_count; ++i) {
            if (!ui_call_scene_tile_visible(*call, i))
                continue;
            UiCallTileMedia tile_media{};
            if (!ui_call_scene_tile_media(*call, i, &tile_media) || !ui_rect_has_area(tile_media.avatar))
                continue;
            if (tile_media.video_ssrc && ui_rect_has_area(tile_media.video) && application.renderer.preview_media_atlas &&
                media_atlas_peek(application.renderer.preview_media_atlas,
                                 media_asset_call_video(tile_media.user_id, tile_media.video_ssrc))
                    .ok())
                continue;
            if (!proceed(request_avatar(user_store_find(application.account_ui->user_store, call->tiles[i].user_id), true)))
                return outcome;
        }
    // Stage audience and hand-raise rows.
    if (const auto* call = application.renderer.call_scene; call && call->active) {
        UiCallTileMedia stage_avatar{};
        for (u32 i = 0; ui_call_scene_stage_avatar(*call, i, &stage_avatar); ++i)
            if (!proceed(request_avatar(user_store_find(application.account_ui->user_store, stage_avatar.user_id), true)))
                return outcome;
    }
    // Go Live previews: the hover card and visible stream tiles.
    if (!proceed(request_stream_previews(&stop)))
        return outcome;
    // Custom group DM icons replace the generated member stack.
    const auto request_group_icon = [&](u64 channel_id, bool* handled, bool play) noexcept {
        const Utf8View custom = ui_private_row_custom_icon(application.account_ui->chat_state, channel_id);
        *handled = custom.bytes != 0;
        if (!*handled)
            return true;
        const auto key = media_asset_channel_icon(channel_id, user_avatar_variant(custom), asset_hash_animated(custom));
        bool force_request = false;
        if ((key.flags & media_asset_flag_animated) &&
            animation_resident(key, &force_request, play))
            return true;
        return request_visible_hashed_identity(key, custom, &stop,
                                               force_request);
    };
    // A conversation's custom icon, else the avatars of its (up to two) members.
    const auto request_conversation = [&](u64 channel_id, bool play) noexcept {
        bool custom_icon = false;
        if (!request_group_icon(channel_id, &custom_icon, play))
            return false;
        if (stop || custom_icon)
            return true;
        u64 people[2]{};
        const u32 count = ui_private_row_group_members(application.account_ui->chat_state, channel_id, people);
        for (u32 j = 0; j < count; ++j) {
            if (!request_avatar(user_store_find(application.account_ui->user_store, people[j]), play))
                return false;
            if (stop)
                return true;
        }
        return true;
    };
    // Conversations pinned in the server rail show still avatars.
    for (u32 i = 0; application.guild_rail_scene.active && i < application.guild_rail_scene.count; ++i) {
        const auto& row = application.guild_rail_scene.rows[i];
        if (row.view.folder_role != GuildFolderRole::conversation)
            continue;
        if (!proceed(request_conversation(row.view.id, false)))
            return outcome;
    }
    if (application.shell_navigation_scene.active) {
        const auto& scene = application.shell_navigation_scene;
        const auto* hovered_row = shell_navigation_scene_resolve(scene, scene.widgets.hovered);
        for (u32 i = 0; i < scene.count; ++i) {
            const auto& row = scene.rows[i];
            if (!channel_is_private(row.view.type))
                continue;
            const auto* icon = ui_widget_manager_layout_node(scene.widgets, row.icon);
            if (!icon || !ui_rect_has_area(ui_rect_intersection(icon->rectangle, scene.viewport)))
                continue;
            // As in the official client's DM list: the open (selected) or hovered conversation.
            const bool row_plays = avatar_row_animates(hovered_row == &row || row.view.selected);
            if (!proceed(request_conversation(row.view.id, row_plays)))
                return outcome;
            // A one-to-one row's nameplate (hovered or selected; the still under its animation) and server tag badge,
            // as the member list requests them.
            const auto* rows_source = application.account_ui->navigation_view.private_rows;
            const UiPrivateRowMedia layers[]{UiPrivateRowMedia::nameplate_still, UiPrivateRowMedia::nameplate,
                                             UiPrivateRowMedia::tag};
            for (u32 layer = 0; rows_source && layer < 3; ++layer) {
                const u64 selected = application.account_ui->navigation_view.friends_mode
                                         ? 0
                                         : application.account_ui->chat_state.selected_channel_id;
                ProfileMedia media;
                if (!ui_private_row_media(scene, i, *rows_source, selected, !application.renderer.system_animation_off,
                                          layers[layer], &media))
                    continue;
                const bool resident = media_atlas_find(application.renderer.media_atlas, media.key).ok();
                if (media.key.flags & media_asset_flag_animated) {
                    if (animation_timeline_observe_visible(&application.animations.timeline, media.key, now_us) &&
                        resident)
                        continue;
                } else if (resident) {
                    continue;
                }
                if (visibility_only)
                    continue;
                if (!proceed(request_profile_media(media, &stop)))
                    return outcome;
            }
        }
        // The conversation header repeats the selected row's avatars, still.
        if (application.shell_scene.view == UiShellView::private_channels) {
            if (!proceed(request_conversation(application.account_ui->chat_state.selected_channel_id, false)))
                return outcome;
        }
        for (u32 i = 0; i < scene.count; ++i) {
            const auto& row = scene.rows[i];
            const auto* layout = row.view.voice_count ? shell_navigation_scene_layout_node(scene, row) : nullptr;
            UiVoiceParticipantRow voice{};
            for (u32 j = 0; layout && ui_voice_participant_row(row.view, layout->rectangle,
                                                               application.account_ui->voice_states, j, &voice);
                 ++j) {
                if (!ui_rect_has_area(ui_rect_intersection(voice.avatar, scene.viewport)))
                    continue;
                // Voice participant rows show still avatars in the official client.
                if (!proceed(request_avatar(user_store_find(application.account_ui->user_store, voice.state->user_id),
                                            false)))
                    return outcome;
            }
        }
    }
    if (chat_visible && application.message_scene.active) {
        // Component thumbnails/gallery images and invite icons are fetched by
        // URL (profile-image keys), only while their rectangles are visible.
        const auto& scene = application.message_scene;
        const auto& store = application.account_ui->message_rich_store;
        for (u32 i = 0; i < scene.count; ++i) {
            const auto& row = scene.rows[i];
            // Every loop below is bounded by one of these counts.
            if (!row.component_item_count && !row.embed_item_count && !row.invite_item_count)
                continue;
            const auto* record = message_store_resolve(application.message_view.store, row.message);
            const auto* rich = record ? message_rich_store_find(store, record->id) : nullptr;
            for (u32 c = 0; rich && rich->message_revision == record->revision && c < rich->component_count &&
                            c < row.component_item_count;
                 ++c) {
                MessageComponentMedia resolved{};
                const auto* node = message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::component, c);
                if (!node || !ui_rect_has_area(ui_rect_intersection(node->rectangle, scene.viewport)) ||
                    !message_component_media_resolve(store, rich->components[c], &resolved) ||
                    resolved.url.bytes > sizeof(ProfileMedia{}.url.data))
                    continue;
                ProfileMedia media{};
                media.key = resolved.key;
                copy_bytes(media.url.data, resolved.url.data, resolved.url.bytes);
                media.url.bytes = static_cast<u16>(resolved.url.bytes);
                media.rectangle = node->rectangle;
                media.clip = scene.viewport;
                if (!proceed(request_profile_media(media, &stop)))
                    return outcome;
            }
            // Embed author/footer icons: proxied URLs, only while visible.
            for (u32 e = 0; rich && rich->message_revision == record->revision && message_rich_display().embeds &&
                            e < rich->embed_count && e < row.embed_item_count;
                 ++e) {
                const auto* node = message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::embed, e);
                if (!node)
                    continue;
                const MessageRichEmbed& embed = rich->embeds[e];
                UiRect icons[2]{};
                const Utf8View urls[2]{message_rich_embed_author_icon(store, embed),
                                       message_rich_embed_footer_icon(store, embed)};
                message_rich_embed_author_icon_rect(store, embed, row.rich_item_layout, e, node->content_rectangle,
                                                    &icons[0]);
                message_rich_embed_footer_icon_rect(store, embed, row.rich_item_layout, e, node->content_rectangle,
                                                    &icons[1]);
                for (u32 icon = 0; icon < 2; ++icon) {
                    if (!urls[icon].bytes || urls[icon].bytes > sizeof(ProfileMedia{}.url.data) ||
                        !ui_rect_has_area(ui_rect_intersection(icons[icon], scene.viewport)))
                        continue;
                    ProfileMedia media{};
                    media.key = message_component_media_key(hash64_bytes(kHash64Seed, urls[icon].data, urls[icon].bytes),
                                                            message_rich_embed_icon_format(urls[icon]));
                    copy_bytes(media.url.data, urls[icon].data, urls[icon].bytes);
                    media.url.bytes = static_cast<u16>(urls[icon].bytes);
                    media.rectangle = icons[icon];
                    media.clip = scene.viewport;
                    if (!proceed(request_profile_media(media, &stop)))
                        return outcome;
                }
            }
            InviteLink links[kMessageInviteCardMaximum];
            const u32 invites = record && scene.invite_cards && row.invite_item_count
                                    ? message_scene_invite_codes(scene, *record, {links, kMessageInviteCardMaximum})
                                    : 0u;
            for (u32 l = 0; l < invites && l < row.invite_item_count; ++l) {
                const bool event_link = invite_link_event(links[l]);
                const auto* entry = event_link ? nullptr
                                               : invite_card_cache_find(*scene.invite_cards,
                                                                        {links[l].code, links[l].code_bytes});
                const auto* event = event_link && scene.scheduled_events
                                        ? scheduled_event_store_find(*scene.scheduled_events, links[l].event_id)
                                        : nullptr;
                const auto* node = message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::invite, l);
                if ((!entry && !event) || !node ||
                    !ui_rect_has_area(ui_rect_intersection(node->rectangle, scene.viewport)))
                    continue;
                ProfileMedia media{};
                const u32 bytes = event ? message_event_cover_url(*event, {media.url.data, sizeof(media.url.data)})
                                        : message_invite_icon_url(entry->invite, {media.url.data, sizeof(media.url.data)});
                if (!bytes)
                    continue;
                media.url.bytes = static_cast<u16>(bytes);
                media.key = message_component_media_key(hash64_bytes(kHash64Seed, media.url.data, bytes),
                                                        MediaAttachmentFormat::png);
                media.rectangle = message_invite_card_geometry(node->rectangle).icon;
                media.clip = scene.viewport;
                if (!proceed(request_profile_media(media, &stop)))
                    return outcome;
            }
        }
        for (u32 i = 0; i < application.message_scene.count; ++i) {
            const auto& row = application.message_scene.rows[i];
            if (row.grouped)
                continue;
            const auto* layout = message_scene_layout_node(application.message_scene, row.author);
            if (!layout || !ui_rect_has_area(
                               ui_rect_intersection(layout->rectangle, application.message_scene.viewport)))
                continue;
            const auto* record = message_store_resolve(application.message_view.store, row.message);
            const auto* appearance =
                record ? user_appearance_find(application.account_ui->user_store.appearance,
                                                 record->author_id)
                       : nullptr;
            ProfileMedia media;
            if (!appearance || appearance->bot ||
                !user_guild_badge_media(*appearance, layout->rectangle, application.message_scene.viewport,
                                        &media))
                continue;
            if (!proceed(request_profile_media(media, &stop)))
                return outcome;
        }
        // An interaction header ("<user> used <command>") shows the command
        // user's server tag chip, from the same badge asset.
        for (u32 i = 0; i < application.message_scene.count; ++i) {
            const auto& row = application.message_scene.rows[i];
            const auto* layout = message_scene_layout_node(application.message_scene, row.reply);
            if (!layout || !ui_rect_has_area(
                               ui_rect_intersection(layout->rectangle, application.message_scene.viewport)))
                continue;
            const auto* record = message_store_resolve(application.message_view.store, row.message);
            const auto* rich = record && application.message_scene.timeline->rich_store
                                   ? message_rich_store_find(*application.message_scene.timeline->rich_store, record->id)
                                   : nullptr;
            if (!rich || rich->message_revision != record->revision || !message_rich_has_interaction(*rich) ||
                rich->reference.state != GatewayMessageReferenceState::none)
                continue;
            const auto* appearance =
                user_appearance_find(application.account_ui->user_store.appearance, rich->interaction.user_id);
            ProfileMedia media;
            if (!appearance || appearance->bot ||
                !user_guild_badge_media(*appearance, layout->rectangle, application.message_scene.viewport, &media))
                continue;
            if (!proceed(request_profile_media(media, &stop)))
                return outcome;
        }
    }
    if (chat_visible && application.member_list_scene.active && application.member_list_scene.users) {
        // Plain avatars first, through the shared user-avatar identity (one
        // decode for timeline and sidebar). Only rows inside the viewport
        // resolve, so a large list never queues or pins off-screen members.
        // Rows resolve once per user (member_media_place); a resident item
        // needs no hash or URL, so only a request resolves its row again.
        const auto& members = application.member_list_scene;
        MediaAtlas* const atlas = application.renderer.media_atlas;
        const auto* hovered_member = member_list_scene_resolve(members, members.widgets.hovered);
        for (u32 visible = 0; visible < members.count; ++visible) {
            MemberMediaPlacement placed{};
            if (!member_plain_avatar_place(members, visible, &placed))
                continue;
            bool force = false;
            if ((placed.key.flags & media_asset_flag_animated) &&
                animation_resident(placed.key, &force,
                                   avatar_row_animates(hovered_member == &members.rows[visible])))
                continue;
            if (!visibility_only && uploads >= kMessageMediaUploadBudget)
                return true;
            // request_visible_hashed_identity's own early returns.
            if (visibility_only || (!force && media_atlas_find(atlas, placed.key).ok()))
                continue;
            AvatarMedia media;
            if (!member_plain_avatar_resolve(members, visible, &media))
                continue;
            if (!proceed(request_visible_avatar_media(media.key, media.hash, &stop, force)))
                return outcome;
        }
        for (u32 visible = 0; visible < members.count; ++visible) {
            const MemberListRowWidget& row = members.rows[visible];
            if (row.kind != MemberListRowKind::member || row.user_id == 0) {
                continue;
            }
            for (u32 layer = 0; layer < kMemberMediaLayerCount; ++layer) {
                MemberMediaPlacement placed{};
                if (!member_media_place(members, visible, static_cast<MemberMedia>(layer), &placed))
                    continue;
                // request_profile_media's resident and playback checks.
                const bool resident = media_atlas_find(atlas, placed.key).ok();
                if (placed.key.flags & media_asset_flag_animated) {
                    if (animation_timeline_observe_visible(&application.animations.timeline, placed.key, now_us) &&
                        resident)
                        continue;
                } else if (resident) {
                    continue;
                }
                ProfileMedia media;
                if (visibility_only ||
                    !member_media_resolve(members, visible, static_cast<MemberMedia>(layer), &media))
                    continue;
                if (!proceed(request_profile_media(media, &stop)))
                    return outcome;
            }
        }
    }
    if (chat_visible) {
        const MessageMediaControllerResult decorations = message_media_controller_visit_decorations(
            &application.message_media, application.message_view.store,
            application.account_ui->user_store, &application.animations.timeline,
            application.renderer.media_atlas, now_us, message_media_callbacks,
            application.message_scene.timeline->rich_store);
        if (!decorations.ok()) {
            log_u64(LogLevel::error, "media", "decoration_visit_error",
                    static_cast<u64>(decorations.error));
            return false;
        }
        if (decorations.stopped)
            return true;
    }
    if ((chat_visible && !proceed(request_chip_guild_icons(false, &stop))) ||
        !proceed(request_chip_guild_icons(true, &stop)))
        return outcome;
    if (!proceed(request_search_decorations(&stop)) || !chat_visible)
        return outcome;
    if (visibility_only || uploads >= kMessageMediaUploadBudget)
        return true;
    // Load a bounded neighborhood only after all visible surfaces. This pass
    // omits decorations and never marks playback visibility.
    const UiVariableRange nearby = ui_variable_list_buffered_range(
        application.message_view.list, timeline.scroll_units, timeline.viewport_units,
        ui_extent_units(kMessageMediaPreloadMarginDips));
    const auto prefetched_rows = message_media_controller_collect(
        &application.message_media, application.message_view.store,
        application.account_ui->message_rich_store, application.message_scene, nearby);
    if (!prefetched_rows.ok())
        return false;
    prefetch_only = true;
    const auto prefetched = message_media_controller_visit_primary(
        &application.message_media, application.account_ui->message_rich_store, application.message_scene,
        message_media_callbacks, true);
    prefetch_only = false;
    if (!prefetched.ok())
        return false;
    return true;
}

ApplicationMediaPumpResult application_pump_visible_media(Application* application) noexcept {
    ApplicationMediaPump pump{*application};
    pump.result.ok = pump.pump_visible_static_media(&pump.result.repaint);
    return pump.result;
}

__declspec(noinline) bool ApplicationMediaPump::pump_covered_animations(bool* repaint) noexcept {
    if (application.login.visible || renderer_settings_hides_shell(application.renderer) ||
        !application.account_ui || !application.account.instance)
        return true;
    const u64 now_ms = clock_monotonic_milliseconds(), now_seconds = clock_unix_milliseconds() / 1'000;
    const u64 now_us = clock_microseconds(application.clock, application.process_start, clock_now());
    pass(now_ms, now_seconds, now_us, repaint).uploads = 0;
    // content_media_visible() is false under the browser, so chat entries are
    // not observed and end_visibility parks them with their remaining frame
    // time. Uncovered chrome stays visible and keeps its cadence.
    animation_timeline_begin_visibility(&application.animations.timeline);
    visibility_only = true;
    const bool observed = scan_visible_media(now_us);
    visibility_only = false;
    if (!observed)
        return false;
    animation_timeline_end_visibility(&application.animations.timeline, now_us);
    if (!pump_due_animations(repaint))
        return false;
    for (u32 completions = 0; completions < kMessageMediaCompletionDrainBudget; ++completions) {
        AccountMediaFrame pending{};
        if (!account_module_media_take(application.account.instance, &pending))
            break;
        if (pending.source_kind == AccountMediaSourceKind::local_draft_preview)
            continue;
        bool consumed = false;
        if (!consume_pending_animation_frame(pending, repaint, &consumed))
            return false;
        // Forum post cards are the browser's own content: their author avatars
        // upload here. Other completions are leased without being shown, exactly
        // like the browser-covered forum preview drain; normal reacquisition reloads them.
        if (!consumed && (pending.key.kind == MediaAssetKind::user_avatar || is_default_avatar_key(pending.key) ||
                          pending.key.kind == MediaAssetKind::attachment) &&
            (surfaces() & d3d11_surface_thread_browser) &&
            application.renderer.thread_browser_scene->forum && message_image_frame_is_current(pending)) {
            const auto upload = upload_account_media_frame(pending);
            if (upload == MediaFrameUploadResult::uploaded) {
                ++uploads;
                trace_first_media_upload();
                if (repaint)
                    *repaint = true;
            }
        }
    }
    bool forum_stop = false;
    if (!request_forum_avatars(&forum_stop))
        return false;
    return forum_stop || request_forum_thumbnails(&forum_stop);
}

ApplicationMediaPumpResult application_pump_covered_media(Application* application) noexcept {
    ApplicationMediaPumpResult result{false, false, false, true};
    ApplicationMediaPump pump{*application};
    result.ok = pump.pump_covered_animations(&result.repaint);
    return result;
}

ApplicationMediaPumpResult application_pump_forum_preview_media(Application* application) noexcept {
    ApplicationMediaPumpResult result{false, false, false, true};
    const auto* scene = application_forum_form_scene();
    const auto post = application_forum_post_view();
    if (!scene || !scene->active || scene->content.owner_serial != post.ticket.serial ||
        !post.available || !application_forum_post_current(post.ticket) || application->login.visible ||
        renderer_settings_page_visible(application->renderer) || !application->account.instance ||
        !application->account_ui || !application->renderer.media_atlas ||
        !application->renderer.preview_media_atlas)
        return result;
    const u64 now = clock_monotonic_milliseconds();
    result.ok = application_forum_preview_request(now, clock_unix_milliseconds() / 1'000);
    if (!result.ok)
        return result;
    ApplicationMediaPump pump{*application};
    u32 uploads{};
    // This shares the mailbox with already queued ordinary work. Remove or defer
    // animation completions for this asset set and show only current
    // local previews. Start no hidden assets, geometry, history or read work.
    for (u32 completed = 0;
         completed < kMessageMediaCompletionDrainBudget && uploads < kMessageMediaUploadBudget;
         ++completed) {
        AccountMediaFrame frame{};
        if (!account_module_media_take(application->account.instance, &frame))
            break;
        const bool local = frame.source_kind == AccountMediaSourceKind::local_draft_preview;
        if (local) {
            if (!application_local_preview_frame_current(frame))
                continue;
        } else {
            for (u32 index = 0; index < application->animations.pending_advance_count; ++index) {
                const auto& pending = application->animations.pending_advances[index];
                const auto* visual = pump.find_animation_visual(pending.key, pending.generation);
                if (!media_asset_key_equal(pending.key, frame.key) ||
                    pending.frame_index != frame.frame_index || !visual ||
                    visual->cache_generation != frame.cache_generation)
                    continue;
                const auto advance = pump.remove_pending_animation(index);
                const auto now_us =
                    clock_microseconds(application->clock, application->process_start, clock_now());
                if (!animation_timeline_defer_advance(&application->animations.timeline, advance, now_us,
                                                      1'000)
                         .ok())
                    pump.release_animation_visual(advance.key, advance.generation);
                break;
            }
            // Cold hidden completions are leased without being shown. The
            // worker reclaims leases on the next keyed request, and the normal
            // still-image atlas-miss path reloads from its retained encoded
            // cache. Other formats follow their normal reacquisition path.
            continue;
        }
        const auto uploaded = pump.upload_account_media_frame(frame);
        if (uploaded == ApplicationMediaPump::MediaFrameUploadResult::deferred)
            continue;
        if (local)
            application_local_preview_frame_uploaded(frame);
        ++uploads;
        pump.trace_first_media_upload();
        result.repaint = true;
    }
    return result;
}

} // namespace maicord::app_detail
