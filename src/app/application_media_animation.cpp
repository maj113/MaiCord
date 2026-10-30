#include "application_media_pump.hpp"
#include "maicord/core/memory.hpp"
#include "application_media_picker.hpp"
#include "application_composer_preview.hpp"
#include "application_video.hpp"
#include "catalog_preview_log.hpp"

#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/media/animation_timeline.hpp"
#include "maicord/media/jpeg.hpp"
#include "maicord/render/colr_program.hpp"
#include "maicord/render/renderer_settings_visibility.hpp"

namespace maicord::app_detail {

void application_media_poster_uploaded(Application& application, const AccountMediaFrame& frame) noexcept {
    if (frame.key.kind != MediaAssetKind::local_draft_preview ||
        frame.source_kind != AccountMediaSourceKind::local_draft_preview ||
        frame.kind != AccountMediaFrameKind::video_nv12_gpu || !frame.gpu_video || !frame.lease_serial ||
        !application.account.instance || !application.account.status.running ||
        !application.account.api.media_local_preview || !application_local_preview_frame_current(frame))
        return;
    const auto instance = application.account.instance;
    const u64 profile = application.account.status.profile_id;
    const u64 account_scope = application.plugin_account_scope_serial;
    AccountMediaLocalPreviewRequest request{};
    request.key = frame.key;
    request.owner_scope = frame.source_catalog_scope;
    request.owner_generation = frame.source_catalog_generation;
    request.lease_serial = frame.lease_serial;
    const auto result = application.account.api.media_local_preview(
        instance, AccountLocalPreviewOperation::uploaded, &request, 0, 0);
    if (result.ok() && result.state == AccountMediaRequestState::pending && application.account.instance == instance &&
        application.account.status.running && application.account.status.profile_id == profile &&
        application.plugin_account_scope_serial == account_scope)
        application.local_poster_presentation = {instance, profile, account_scope, true};
}

void application_media_posters_presented(Application& application, bool succeeded, u64 before, u64 after) noexcept {
    auto& pending = application.local_poster_presentation;
    if (!pending.pending)
        return;
    if (application.account.instance != pending.instance || !application.account.status.running ||
        application.account.status.profile_id != pending.profile ||
        application.plugin_account_scope_serial != pending.account_scope ||
        !application.account.api.media_local_preview) {
        reset_value(&pending);
        return;
    }
    if (!succeeded || before == after)
        return;
    const auto result = application.account.api.media_local_preview(
        pending.instance, AccountLocalPreviewOperation::presented, nullptr, 0, 0);
    if (!result.ok() || result.state != AccountMediaRequestState::pending ||
        application.account.instance != pending.instance || !application.account.status.running ||
        application.account.status.profile_id != pending.profile ||
        application.plugin_account_scope_serial != pending.account_scope)
        reset_value(&pending);
}

__declspec(noinline) bool ApplicationMediaPump::animated_webp_frame_kind(
    AccountMediaFrameKind kind) noexcept {
    return kind == AccountMediaFrameKind::animated_webp_rgba8 ||
           kind == AccountMediaFrameKind::animated_webp_vp8;
}

__declspec(noinline) bool ApplicationMediaPump::video_frame_kind(AccountMediaFrameKind kind) noexcept {
    return kind == AccountMediaFrameKind::video_nv12_gpu;
}

__declspec(noinline) bool ApplicationMediaPump::animation_frame_cache_descriptor(
    const AccountMediaFrame& frame, AnimationFrameCacheDescriptor* descriptor) noexcept {
    if (!descriptor || frame.cache_generation == 0 || frame.duration_us == 0 || frame.canvas_width == 0 ||
        frame.canvas_height == 0 || frame.frame_index >= frame.frame_count) {
        return false;
    }
    reset_value<AnimationFrameCacheDescriptor>(descriptor);
    descriptor->key = AnimationFrameCacheKey{frame.key, frame.frame_index, frame.cache_generation, 0};
    descriptor->key.content_identity = frame.content_identity;
    descriptor->duration_us = frame.duration_us;
    descriptor->background_rgba = frame.background_rgba;
    descriptor->width = frame.canvas_width;
    descriptor->height = frame.canvas_height;
    if (frame.kind == AccountMediaFrameKind::apng_scanlines) {
        descriptor->control.apng = frame.apng_frame;
        descriptor->kind = AnimationFrameCacheKind::apng;
    } else if (frame.kind == AccountMediaFrameKind::gif_indices) {
        descriptor->control.gif = frame.gif_frame;
        descriptor->kind = AnimationFrameCacheKind::gif;
    } else if (animated_webp_frame_kind(frame.kind)) {
        descriptor->control.animated_webp = frame.animated_webp_frame;
        descriptor->kind = AnimationFrameCacheKind::animated_webp;
    } else if (frame.kind == AccountMediaFrameKind::lottie_paint) {
        descriptor->kind = AnimationFrameCacheKind::lottie;
    } else {
        return false;
    }
    return true;
}

__declspec(noinline) void ApplicationMediaPump::capture_animation_frame(
    const AccountMediaFrame& frame) noexcept {
    AnimationFrameCacheDescriptor descriptor{};
    if (animation_frame_cache_descriptor(frame, &descriptor)) {
        // Failure is an explicit bounded-cache fallback: playback continues
        // through the codec worker and the renderer counters retain evidence.
        renderer_capture_animation_frame(&application.renderer, descriptor);
    }
}

__declspec(noinline) bool ApplicationMediaPump::animation_frame_kind_compatible(
    AccountMediaFrameKind left, AccountMediaFrameKind right) noexcept {
    return left == right || (animated_webp_frame_kind(left) && animated_webp_frame_kind(right));
}
__declspec(noinline) ApplicationMediaPump::MediaFrameUploadResult ApplicationMediaPump::
    upload_account_media_frame(const AccountMediaFrame& frame) noexcept {
    if (!media_key_is_current(frame.key))
        return MediaFrameUploadResult::deferred;
    const auto reject_source = [&]() noexcept {
        if (frame.key.kind == MediaAssetKind::local_draft_preview) {
            application_local_preview_rejected(frame.key);
            return MediaFrameUploadResult::deferred;
        }
        if (frame.key.kind == MediaAssetKind::catalog_preview) {
            if (application_media_picker_preview_rejected(frame.key, frame.source_catalog_scope,
                                                          frame.source_catalog_generation))
                CatalogPreviewLog("upload")
                    .field("item", frame.key.id)
                    .field("format", static_cast<u64>(media_asset_catalog_format(frame.key)))
                    .field("frame_kind", static_cast<u64>(frame.kind))
                    .field("width", frame.width)
                    .field("height", frame.height)
                    .write();
            return MediaFrameUploadResult::deferred;
        }
        return MediaFrameUploadResult::failed;
    };
    const bool vp8 = frame.kind == AccountMediaFrameKind::vp8_keyframe ||
                     frame.kind == AccountMediaFrameKind::animated_webp_vp8;
    const bool jpeg = frame.kind == AccountMediaFrameKind::jpeg_coefficients;
    const bool gpu_video = frame.kind == AccountMediaFrameKind::video_nv12_gpu && frame.gpu_video;
    if (!media_asset_key_valid(frame.key) || (!frame.bytes.data && !jpeg && !gpu_video) ||
        (frame.bytes.count == 0 && !vp8 && !jpeg && !gpu_video) || frame.width == 0 || frame.height == 0 ||
        (frame.row_pitch == 0 && !gpu_video) || frame.kind == AccountMediaFrameKind::none) {
        return reject_source();
    }
    MediaAtlasGpuResult uploaded{};
    const auto upload = [&]() noexcept {
        if (frame.kind == AccountMediaFrameKind::regional_flag_paint &&
            frame.key.kind == MediaAssetKind::regional_flag && frame.bytes.count == sizeof(ColrPaintProgram)) {
            uploaded = renderer_upload_regional_flag(
                &application.renderer, frame.key, *reinterpret_cast<const ColrPaintProgram*>(frame.bytes.data));
        } else if (frame.kind == AccountMediaFrameKind::lottie_paint &&
                   frame.bytes.count == sizeof(ColrPaintProgram)) {
            uploaded = renderer_upload_lottie_program(
                &application.renderer, frame.key, *reinterpret_cast<const ColrPaintProgram*>(frame.bytes.data));
        } else if (frame.kind == AccountMediaFrameKind::png_scanlines && frame.image) {
            uploaded =
                renderer_upload_png_scanlines(&application.renderer, frame.key, *frame.image, frame.bytes);
        } else if (jpeg && frame.jpeg_image && frame.jpeg_coefficients &&
                   frame.width == jpeg_display_width(*frame.jpeg_image) &&
                   frame.height == jpeg_display_height(*frame.jpeg_image)) {
            uploaded = renderer_upload_jpeg(&application.renderer, frame.key, *frame.jpeg_image,
                                            *frame.jpeg_coefficients);
        } else if (frame.kind == AccountMediaFrameKind::straight_rgba8 && frame.width <= 0xFFFFu &&
                   frame.height <= 0xFFFFu) {
            uploaded = renderer_upload_media_straight_rgba8(
                &application.renderer, frame.key, static_cast<u16>(frame.width), static_cast<u16>(frame.height),
                frame.row_pitch, frame.bytes);
        } else if (video_frame_kind(frame.kind) && frame.width <= 0xFFFFu && frame.height <= 0xFFFFu &&
                   frame.duration_us != 0 && frame.frame_count != 0 && frame.cache_generation != 0) {
            uploaded = gpu_video
                           ? renderer_present_video_frame(&application.renderer, frame.key, *frame.gpu_video)
                           : renderer_upload_media_straight_rgba8(
                                 &application.renderer, frame.key, static_cast<u16>(frame.width),
                                 static_cast<u16>(frame.height), frame.row_pitch, frame.bytes);
        } else if (frame.kind == AccountMediaFrameKind::vp8_keyframe && frame.vp8_frame && frame.vp8_header &&
                   frame.vp8_modes && frame.vp8_coefficients && frame.vp8_frame->width == frame.width &&
                   frame.vp8_frame->height == frame.height) {
            uploaded = renderer_upload_vp8_keyframe(&application.renderer, frame.key, *frame.vp8_frame,
                                                    *frame.vp8_header, *frame.vp8_modes,
                                                    *frame.vp8_coefficients, frame.bytes);
        } else if (frame.kind == AccountMediaFrameKind::apng_scanlines && frame.image &&
                   frame.frame_index == 0 && frame.frame_count != 0 &&
                   frame.frame_count <= kApngFrameCapacity && frame.duration_us != 0 &&
                   frame.canvas_width != 0 && frame.canvas_height != 0) {
            uploaded = renderer_begin_apng(&application.renderer, frame.key, frame.canvas_width,
                                           frame.canvas_height, frame.apng_frame, *frame.image, frame.bytes);
        } else if (frame.kind == AccountMediaFrameKind::gif_indices && frame.gif_palette &&
                   frame.frame_index == 0 && frame.frame_count != 0 && frame.frame_count <= kGifFrameCapacity &&
                   frame.duration_us != 0 && frame.canvas_width != 0 && frame.canvas_height != 0) {
            uploaded =
                renderer_begin_gif(&application.renderer, frame.key, frame.canvas_width, frame.canvas_height,
                                   frame.gif_frame, *frame.gif_palette, frame.bytes);
        } else if (frame.kind == AccountMediaFrameKind::animated_webp_rgba8 && frame.frame_index == 0 &&
                   frame.frame_count != 0 && frame.frame_count <= kAnimatedWebpFrameCapacity &&
                   frame.duration_us != 0 && frame.canvas_width != 0 && frame.canvas_height != 0) {
            uploaded = renderer_begin_animated_webp(&application.renderer, frame.key, frame.canvas_width,
                                                    frame.canvas_height, frame.background_rgba,
                                                    frame.animated_webp_frame, frame.bytes);
        } else if (frame.kind == AccountMediaFrameKind::animated_webp_vp8 && frame.vp8_frame &&
                   frame.vp8_header && frame.vp8_modes && frame.vp8_coefficients && frame.frame_index == 0 &&
                   frame.frame_count != 0 && frame.frame_count <= kAnimatedWebpFrameCapacity &&
                   frame.duration_us != 0 && frame.canvas_width != 0 && frame.canvas_height != 0 &&
                   frame.vp8_frame->width == frame.width && frame.vp8_frame->height == frame.height) {
            uploaded = renderer_begin_animated_webp_vp8(
                &application.renderer, frame.key, frame.canvas_width, frame.canvas_height,
                frame.background_rgba, frame.animated_webp_frame, *frame.vp8_frame, *frame.vp8_header,
                *frame.vp8_modes, *frame.vp8_coefficients, frame.bytes);
        } else {
            return false;
        }
        return true;
    };
    const auto capacity_miss = [&]() noexcept {
        return uploaded.error == MediaAtlasGpuError::atlas_failure &&
               uploaded.allocation.error == MediaAtlasError::no_capacity;
    };
    if (!upload())
        return reject_source();
    // The open profile's images take shelves from visible member-list ornaments.
    if (capacity_miss() && yield_decorative_media(frame.key) && !upload())
        return reject_source();
    if (!uploaded.ok()) {
        if (capacity_miss()) {
            const u64 now_ms = clock_monotonic_milliseconds();
            const u64 retry_at_ms = now_ms + 1'000u;
            message_media_controller_observe_retry(&application.message_media, retry_at_ms);
            // Composer thumbnails back off with the rest instead of re-decoding every poll.
            if (frame.key.kind == MediaAssetKind::local_draft_preview)
                application_local_preview_deferred(frame.key, retry_at_ms);
            if (media_asset_large_preview(frame.key))
                message_media_controller_defer_preview(&application.message_media, now_ms);
            if (message_media_controller_should_log_capacity(&application.message_media, now_ms)) {
                log_write(LogLevel::warning, "media", "visible_media_working_set_full_deferred");
                log_u64(LogLevel::warning, "media", "asset_kind", static_cast<u64>(frame.key.kind));
                log_u64(LogLevel::warning, "media", "asset_id", frame.key.id);
                log_u64(LogLevel::warning, "media", "frame_kind", static_cast<u64>(frame.kind));
                log_u64(LogLevel::warning, "media", "frame_width", frame.width);
                log_u64(LogLevel::warning, "media", "frame_height", frame.height);
                const auto* atlas = media_asset_large_preview(frame.key)
                                        ? application.renderer.preview_media_atlas
                                        : application.renderer.media_atlas;
                if (atlas) {
                    log_u64(LogLevel::warning, "media", "atlas_entries", atlas->entry_count);
                    log_u64(LogLevel::warning, "media", "atlas_evictions",
                            atlas->allocations.evicted_entries);
                    log_u64(LogLevel::warning, "media", "atlas_full_resets",
                            atlas->allocations.full_resets);
                }
            }
            return MediaFrameUploadResult::deferred;
        }
        log_u64(LogLevel::error, "media", "asset_upload_error", static_cast<u64>(uploaded.error));
        log_u64(LogLevel::error, "media", "asset_upload_native_error",
                static_cast<u64>(uploaded.native_error));
        log_u64(LogLevel::error, "media", "asset_kind", static_cast<u64>(frame.key.kind));
        log_u64(LogLevel::error, "media", "frame_kind", static_cast<u64>(frame.kind));
        log_u64(LogLevel::error, "media", "frame_width", frame.width);
        log_u64(LogLevel::error, "media", "frame_height", frame.height);
        if (uploaded.error == MediaAtlasGpuError::invalid_input)
            return reject_source();
        return MediaFrameUploadResult::failed;
    }
    // Transfer the exact lease before any later media request can reclaim its lane.
    application_media_poster_uploaded(application, frame);
    // Upload admission and cache lookup do not imply visibility. Pending jobs
    // may finish after scrolling away, and prefetch must remain reclaimable.
    const bool source_visible =
        frame.key.kind == MediaAssetKind::local_draft_preview
            ? application_local_preview_key_current(frame.key)
        : frame.key.kind == MediaAssetKind::catalog_preview
            ? application_media_picker_preview_visible(frame.key)
            : message_media_primary_visible(application.message_scene, frame.source_message_id, frame.key);
    if (media_asset_large_preview(frame.key) && !source_visible) {
        media_atlas_unpin(application.renderer.preview_media_atlas, frame.key);
    }
    const bool animation_frame =
        frame.key.kind != MediaAssetKind::local_draft_preview &&
        ((frame.kind == AccountMediaFrameKind::lottie_paint && frame.frame_count > 1 &&
          frame.cache_generation) ||
         frame.kind == AccountMediaFrameKind::apng_scanlines ||
         frame.kind == AccountMediaFrameKind::gif_indices || animated_webp_frame_kind(frame.kind) ||
         (video_frame_kind(frame.kind) && !application_video_ordinary(frame.key)));
    if (animation_frame) {
        u32 generation = frame.cache_generation;
        if (generation == 0) {
            generation = ++application.animations.next_generation;
            if (generation == 0) {
                generation = ++application.animations.next_generation;
            }
        }
        const AnimationTimelineResult published = animation_visual_runtime_publish(
            &application.animations,
            AnimationTimelineDescriptor{frame.key, frame.duration_us, generation,
                                        frame.cache_generation == 0 ? 1u : frame.frame_count,
                                        frame.cache_generation == 0 ? 1u : frame.play_count,
                                        !media_asset_large_preview(frame.key) &&
                                            frame.key.kind != MediaAssetKind::profile_image &&
                                            message_media_controller_animation_plays(
                                                application.message_media, frame.key.kind,
                                                frame.source_message_id)},
            clock_microseconds(application.clock, application.process_start, clock_now()));
        if (!published.ok() || published.slot >= application.animations.timeline.capacity) {
            return MediaFrameUploadResult::failed;
        }
        AnimationVisualState& visual = application.animations.visuals[published.slot];
        reset_value<AnimationVisualState>(&visual);
        visual.key = frame.key;
        if (frame.kind == AccountMediaFrameKind::apng_scanlines) {
            visual.frame.apng = frame.apng_frame;
        } else if (frame.kind == AccountMediaFrameKind::gif_indices) {
            visual.frame.gif = frame.gif_frame;
        } else if (animated_webp_frame_kind(frame.kind)) {
            visual.frame.animated_webp.frame = frame.animated_webp_frame;
            visual.frame.animated_webp.background_rgba = frame.background_rgba;
        }
        visual.generation = generation;
        visual.cache_generation = frame.cache_generation;
        visual.content_identity = frame.content_identity;
        visual.source_scope = frame.key.kind == MediaAssetKind::catalog_preview ? frame.source_catalog_scope
                                                                                : frame.source_message_id;
        visual.source_revision = frame.key.kind == MediaAssetKind::catalog_preview
                                     ? frame.source_catalog_generation
                                     : frame.source_message_revision;
        visual.kind = frame.kind;
        visual.occupied = true;
        capture_animation_frame(frame);
    } else {
        // A successfully accepted still-image replacement must not keep a
        // former decoder's timeline or classification for the same residency.
        const auto prior = animation_timeline_find(&application.animations.timeline, frame.key);
        if (prior.ok())
            release_animation_visual(frame.key,
                                     application.animations.timeline.entries[prior.slot].generation);
    }
    const bool preview = media_asset_large_preview(frame.key) &&
                         media_asset_resolution_tier(frame.key) != MediaResolutionTier::original &&
                         application.renderer.preview_media_atlas;
    auto* atlas = preview ? application.renderer.preview_media_atlas : application.renderer.media_atlas;
    if (!media_atlas_set_decoded_animation(atlas, uploaded.allocation.handle,
                                           animation_frame && frame.frame_count > 1))
        return MediaFrameUploadResult::failed;
    return MediaFrameUploadResult::uploaded;
}

__declspec(noinline) AnimationVisualState* ApplicationMediaPump::find_animation_visual(
    const MediaAssetKey& key, u32 generation) noexcept {
    const AnimationTimelineResult found = animation_timeline_find(&application.animations.timeline, key);
    if (!found.ok() || found.slot >= application.animations.timeline.capacity ||
        !application.animations.visuals) {
        return nullptr;
    }
    AnimationVisualState& visual = application.animations.visuals[found.slot];
    return visual.occupied && visual.generation == generation && media_asset_key_equal(visual.key, key)
               ? &visual
               : nullptr;
}

__declspec(noinline) void ApplicationMediaPump::release_animation_visual(MediaAssetKey key,
                                                                         u32 generation) noexcept {
    AnimationVisualState* visual = find_animation_visual(key, generation);
    if (visual) {
        reset_value<AnimationVisualState>(visual);
    }
    animation_timeline_release(&application.animations.timeline, key, generation);
    for (u32 index = 0; index < application.animations.pending_advance_count;) {
        const auto& pending = application.animations.pending_advances[index];
        if (pending.generation == generation && media_asset_key_equal(pending.key, key))
            remove_pending_animation(index);
        else
            ++index;
    }
}

void ApplicationMediaPump::retire_stale_message_animation(const MediaAssetKey& key, u64 source_message_id,
                                                          u32 source_message_revision) noexcept {
    const auto found = animation_timeline_find(&application.animations.timeline, key);
    if (!found.ok() || found.slot >= application.animations.timeline.capacity)
        return;
    const u32 generation = application.animations.timeline.entries[found.slot].generation;
    const auto* visual = find_animation_visual(key, generation);
    if (!visual || visual->source_scope != source_message_id ||
        visual->source_revision != source_message_revision ||
        !message_image_source_is_current(key, visual->source_scope, visual->source_revision))
        release_animation_visual(key, generation);
}

bool ApplicationMediaPump::animation_advance_is_current(const AnimationFrameAdvance& advance,
                                                        const AnimationVisualState& visual) noexcept {
    const auto found = animation_timeline_find(&application.animations.timeline, advance.key);
    if (!found.ok() || found.slot >= application.animations.timeline.capacity)
        return false;
    const auto& entry = application.animations.timeline.entries[found.slot];
    if (!(entry.flags & animation_timeline_entry_pending) || entry.generation != advance.generation ||
        entry.due_at_us != advance.scheduled_at_us || entry.pending_frame_index != advance.frame_index ||
        entry.pending_play_index != advance.play_index || !visual.occupied ||
        visual.generation != advance.generation || !media_asset_key_equal(visual.key, advance.key) ||
        !visual.cache_generation)
        return false;
    return message_image_source_is_current(advance.key, visual.source_scope, visual.source_revision);
}

bool ApplicationMediaPump::animation_frame_is_current(const AnimationFrameAdvance& advance,
                                                      const AccountMediaFrame& frame,
                                                      const AnimationVisualState& visual) noexcept {
    if (!animation_advance_is_current(advance, visual) ||
        !animation_frame_kind_compatible(frame.kind, visual.kind) ||
        !media_asset_key_equal(frame.key, advance.key) ||
        frame.cache_generation != visual.cache_generation || frame.frame_index != advance.frame_index ||
        frame.content_identity != visual.content_identity)
        return false;
    // Zero codec authority is valid only through the exact pending token and
    // its retained initial owner. Partially specified or different owners fail.
    if (frame.key.kind == MediaAssetKind::catalog_preview) {
        if (frame.source_kind == AccountMediaSourceKind::message || frame.source_message_id ||
            frame.source_message_revision)
            return false;
        return !(frame.source_catalog_scope || frame.source_catalog_generation) ||
               (frame.source_kind == AccountMediaSourceKind::catalog_preview &&
                frame.source_catalog_scope == visual.source_scope &&
                frame.source_catalog_generation == visual.source_revision);
    }
    if (frame.source_kind == AccountMediaSourceKind::catalog_preview || frame.source_catalog_scope ||
        frame.source_catalog_generation)
        return false;
    if ((frame.source_message_id || frame.source_message_revision) &&
        (frame.source_message_id != visual.source_scope ||
         frame.source_message_revision != visual.source_revision))
        return false;
    return true;
}

__declspec(noinline) bool ApplicationMediaPump::apply_animation_frame(const AnimationFrameAdvance& advance,
                                                                      const AccountMediaFrame& frame,
                                                                      bool* repaint) noexcept {
    if (application.animations.timeline.suspended) {
        animation_timeline_defer_advance(
            &application.animations.timeline, advance,
            clock_microseconds(application.clock, application.process_start, clock_now()), 1'000);
        return true;
    }
    AnimationVisualState* visual = find_animation_visual(advance.key, advance.generation);
    if (!visual || !animation_frame_is_current(advance, frame, *visual)) {
        release_animation_visual(advance.key, advance.generation);
        return true;
    }

    bool composed = false;
    if (frame.kind == AccountMediaFrameKind::lottie_paint &&
        frame.bytes.count == sizeof(ColrPaintProgram)) {
        composed =
            renderer_upload_lottie_program(&application.renderer, frame.key,
                                           *reinterpret_cast<const ColrPaintProgram*>(frame.bytes.data))
                .ok();
    } else if (frame.kind == AccountMediaFrameKind::apng_scanlines && frame.image) {
        composed =
            frame.frame_index == 0
                ? renderer_begin_apng(&application.renderer, frame.key, frame.canvas_width,
                                      frame.canvas_height, frame.apng_frame, *frame.image, frame.bytes)
                      .ok()
                : renderer_advance_apng(&application.renderer, frame.key, visual->frame.apng,
                                        frame.apng_frame, *frame.image, frame.bytes)
                      .ok();
    } else if (frame.kind == AccountMediaFrameKind::gif_indices && frame.gif_palette) {
        composed =
            frame.frame_index == 0
                ? renderer_begin_gif(&application.renderer, frame.key, frame.canvas_width,
                                     frame.canvas_height, frame.gif_frame, *frame.gif_palette, frame.bytes)
                      .ok()
                : renderer_advance_gif(&application.renderer, frame.key, visual->frame.gif, frame.gif_frame,
                                       *frame.gif_palette, frame.bytes)
                      .ok();
    } else if (frame.kind == AccountMediaFrameKind::animated_webp_rgba8) {
        composed = frame.frame_index == 0
                       ? renderer_begin_animated_webp(&application.renderer, frame.key, frame.canvas_width,
                                                      frame.canvas_height, frame.background_rgba,
                                                      frame.animated_webp_frame, frame.bytes)
                             .ok()
                       : renderer_advance_animated_webp(
                             &application.renderer, frame.key, visual->frame.animated_webp.background_rgba,
                             visual->frame.animated_webp.frame, frame.animated_webp_frame, frame.bytes)
                             .ok();
    } else if (frame.kind == AccountMediaFrameKind::animated_webp_vp8 && frame.vp8_frame &&
               frame.vp8_header && frame.vp8_modes && frame.vp8_coefficients) {
        composed = frame.frame_index == 0
                       ? renderer_begin_animated_webp_vp8(
                             &application.renderer, frame.key, frame.canvas_width, frame.canvas_height,
                             frame.background_rgba, frame.animated_webp_frame, *frame.vp8_frame,
                             *frame.vp8_header, *frame.vp8_modes, *frame.vp8_coefficients, frame.bytes)
                             .ok()
                       : renderer_advance_animated_webp_vp8(
                             &application.renderer, frame.key, visual->frame.animated_webp.background_rgba,
                             visual->frame.animated_webp.frame, frame.animated_webp_frame, *frame.vp8_frame,
                             *frame.vp8_header, *frame.vp8_modes, *frame.vp8_coefficients, frame.bytes)
                             .ok();
    } else if (video_frame_kind(frame.kind) && frame.width <= 0xFFFFu && frame.height <= 0xFFFFu) {
        composed =
            frame.kind == AccountMediaFrameKind::video_nv12_gpu && frame.gpu_video
                ? renderer_present_video_frame(&application.renderer, frame.key, *frame.gpu_video).ok()
                : renderer_upload_media_straight_rgba8(
                      &application.renderer, frame.key, static_cast<u16>(frame.width),
                      static_cast<u16>(frame.height), frame.row_pitch, frame.bytes)
                      .ok();
    }
    if (!composed ||
        !animation_timeline_commit_advance(&application.animations.timeline, advance, frame.duration_us)
             .ok()) {
        release_animation_visual(advance.key, advance.generation);
        return true;
    }
    capture_animation_frame(frame);
    if (frame.kind == AccountMediaFrameKind::apng_scanlines) {
        visual->frame.apng = frame.apng_frame;
    } else if (frame.kind == AccountMediaFrameKind::gif_indices) {
        visual->frame.gif = frame.gif_frame;
    } else if (animated_webp_frame_kind(frame.kind)) {
        visual->frame.animated_webp.frame = frame.animated_webp_frame;
        visual->frame.animated_webp.background_rgba = frame.background_rgba;
    }
    visual->kind = frame.kind;
    if (repaint) {
        *repaint = true;
    }
    return true;
}

__declspec(noinline) bool ApplicationMediaPump::restore_cached_animation_frame(
    const AnimationFrameAdvance& advance, AnimationVisualState* visual, bool* repaint) noexcept {
    if (!visual || !animation_advance_is_current(advance, *visual)) {
        release_animation_visual(advance.key, advance.generation);
        return true;
    }
    if (visual->cache_generation == 0) {
        return false;
    }
    AnimationFrameCacheResult cached{};
    if (!renderer_restore_animation_frame(&application.renderer,
                                          AnimationFrameCacheKey{advance.key, advance.frame_index,
                                                                 visual->cache_generation, 0,
                                                                 visual->content_identity},
                                          &cached)) {
        return false;
    }
    if (cached.kind == AnimationFrameCacheKind::apng) {
        visual->frame.apng = cached.control.apng;
    } else if (cached.kind == AnimationFrameCacheKind::gif) {
        visual->frame.gif = cached.control.gif;
    } else if (cached.kind == AnimationFrameCacheKind::animated_webp) {
        visual->frame.animated_webp.frame = cached.control.animated_webp;
        visual->frame.animated_webp.background_rgba = cached.background_rgba;
    } else if (cached.kind != AnimationFrameCacheKind::lottie) {
        return false;
    }
    if (!animation_timeline_commit_advance(&application.animations.timeline, advance, cached.duration_us)
             .ok()) {
        release_animation_visual(advance.key, advance.generation);
        return true;
    }
    if (repaint) {
        *repaint = true;
    }
    return true;
}

__declspec(noinline) u32
ApplicationMediaPump::find_pending_animation(const AnimationFrameAdvance& advance) noexcept {
    for (u32 index = 0; index < application.animations.pending_advance_count; ++index) {
        const AnimationFrameAdvance& pending = application.animations.pending_advances[index];
        if (media_asset_key_equal(pending.key, advance.key) && pending.generation == advance.generation &&
            pending.frame_index == advance.frame_index) {
            return index;
        }
    }
    return 0xFFFFFFFFu;
}

__declspec(noinline) AnimationFrameAdvance
ApplicationMediaPump::remove_pending_animation(u32 index) noexcept {
    if (index >= application.animations.pending_advance_count) {
        return {};
    }
    const AnimationFrameAdvance removed = application.animations.pending_advances[index];
    --application.animations.pending_advance_count;
    if (index != application.animations.pending_advance_count) {
        application.animations.pending_advances[index] =
            application.animations.pending_advances[application.animations.pending_advance_count];
    }
    reset_value<AnimationFrameAdvance>(&application.animations.pending_advances[application.animations.pending_advance_count]);
    return removed;
}

__declspec(noinline) bool ApplicationMediaPump::consume_pending_animation_frame(
    const AccountMediaFrame& frame, bool* repaint, bool* consumed) noexcept {
    *consumed = false;
    for (u32 index = 0; index < application.animations.pending_advance_count; ++index) {
        const AnimationFrameAdvance& pending = application.animations.pending_advances[index];
        const AnimationVisualState* visual = find_animation_visual(pending.key, pending.generation);
        if (!media_asset_key_equal(pending.key, frame.key) || pending.frame_index != frame.frame_index ||
            !visual || visual->cache_generation != frame.cache_generation) {
            continue;
        }
        const AnimationFrameAdvance advance = remove_pending_animation(index);
        *consumed = true;
        return apply_animation_frame(advance, frame, repaint);
    }
    return true;
}

__declspec(noinline) bool ApplicationMediaPump::pump_due_animations(bool* repaint) noexcept {
    const u64 now_us = clock_microseconds(application.clock, application.process_start, clock_now());
    const u64 now_seconds = clock_unix_milliseconds() / 1'000;

    // Poll all cold frames already assigned to the bounded codec lanes. The
    // matching request marks its lane preferred, so media_take returns the
    // correct borrowed workspace even when several decodes finish together.
    for (u32 pending_index = 0; pending_index < application.animations.pending_advance_count;) {
        const AnimationFrameAdvance advance = application.animations.pending_advances[pending_index];
        if (!media_key_is_current(advance.key)) {
            remove_pending_animation(pending_index);
            release_animation_visual(advance.key, advance.generation);
            continue;
        }
        if (application.animations.timeline.suspended) {
            remove_pending_animation(pending_index);
            animation_timeline_defer_advance(&application.animations.timeline, advance, now_us, 1'000);
            continue;
        }
        AnimationVisualState* visual = find_animation_visual(advance.key, advance.generation);
        if (!visual || visual->cache_generation == 0) {
            remove_pending_animation(pending_index);
            release_animation_visual(advance.key, advance.generation);
            continue;
        }
        const AccountMediaRequestResult requested = application.account.api.media_advance(
            application.account.instance, advance.key, visual->cache_generation, advance.frame_index,
            now_seconds);
        if (requested.state == AccountMediaRequestState::queued ||
            requested.state == AccountMediaRequestState::pending ||
            requested.state == AccountMediaRequestState::busy) {
            ++pending_index;
            continue;
        }
        remove_pending_animation(pending_index);
        if (requested.state != AccountMediaRequestState::ready) {
            release_animation_visual(advance.key, advance.generation);
            continue;
        }
        AccountMediaFrame frame{};
        if (!application.account.api.media_take(application.account.instance, &frame) ||
            !media_asset_key_equal(frame.key, advance.key) || frame.frame_index != advance.frame_index) {
            release_animation_visual(advance.key, advance.generation);
            continue;
        }
        if (!apply_animation_frame(advance, frame, repaint)) {
            return false;
        }
    }

    // Cached animations are presentation metadata updates, so drain a wide
    // bounded batch per pump. Up to four cache misses can occupy independent
    // codec workspaces; later misses defer without blocking cached peers.
    if (!application.animations.timeline.entry_count || application.animations.timeline.suspended ||
        application.login.visible || renderer_settings_hides_shell(application.renderer))
        return true;
    constexpr u32 kCachedAnimationAdvanceBatch = 512;
    // Main-thread scratch: collect_due writes every entry it reports, so the batch needs neither a
    // stack frame (__chkstk) nor zeroing on each pump.
    static AnimationFrameAdvance advances[kCachedAnimationAdvanceBatch];
    const AnimationTimelineCollectResult collected = animation_timeline_collect_due(
        &application.animations.timeline, now_us,
        Slice<AnimationFrameAdvance>{advances, kCachedAnimationAdvanceBatch});
    if (!collected.ok()) {
        return false;
    }
    if (collected.advances == 0) {
        return true;
    }

    for (u32 index = 0; index < collected.advances; ++index) {
        const AnimationFrameAdvance advance = advances[index];
        if (!media_key_is_current(advance.key)) {
            release_animation_visual(advance.key, advance.generation);
            continue;
        }
        AnimationVisualState* visual = find_animation_visual(advance.key, advance.generation);
        if (!visual || visual->cache_generation == 0) {
            release_animation_visual(advance.key, advance.generation);
            continue;
        }
        if (restore_cached_animation_frame(advance, visual, repaint)) {
            continue;
        }
        if (application.animations.pending_advance_count >= pending_animation_capacity()) {
            if (!animation_timeline_defer_advance(&application.animations.timeline, advance, now_us, 1'000)
                     .ok()) {
                return false;
            }
            continue;
        }
        if (!video_frame_kind(visual->kind) &&
            !renderer_materialize_animation_frame(&application.renderer, advance.key)) {
            release_animation_visual(advance.key, advance.generation);
            continue;
        }
        const AccountMediaRequestResult requested = application.account.api.media_advance(
            application.account.instance, advance.key, visual->cache_generation, advance.frame_index,
            now_seconds);
        if (requested.state == AccountMediaRequestState::busy) {
            if (!animation_timeline_defer_advance(&application.animations.timeline, advance, now_us, 1'000)
                     .ok()) {
                return false;
            }
            continue;
        }
        if (requested.state == AccountMediaRequestState::queued ||
            requested.state == AccountMediaRequestState::pending) {
            if (requested.state == AccountMediaRequestState::queued) {
                record_media_animation_advance(advance.key.kind);
            }
            if (find_pending_animation(advance) == 0xFFFFFFFFu) {
                application.animations.pending_advances[application.animations.pending_advance_count++] =
                    advance;
            }
            continue;
        }
        if (requested.state != AccountMediaRequestState::ready) {
            release_animation_visual(advance.key, advance.generation);
            continue;
        }
        AccountMediaFrame frame{};
        if (!application.account.api.media_take(application.account.instance, &frame)) {
            release_animation_visual(advance.key, advance.generation);
            continue;
        }
        if (!apply_animation_frame(advance, frame, repaint)) {
            return false;
        }
    }
    return true;
}

} // namespace maicord::app_detail
