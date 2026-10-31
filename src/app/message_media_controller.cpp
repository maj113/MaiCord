#include "maicord/app/message_media_controller.hpp"
#include "maicord/app/account_media.hpp"

#include "maicord/core/deadline.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/ui/regional_flag.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {

namespace {
// A decoded embed still sets the aspect of its frame. Declared dimensions that
// disagree with the picture by more than one percent keep their larger side
// and take the picture's aspect, so the frame fits the image instead of
// letterboxing it. Always succeeds: a still that cannot be matched is ignored.
bool fit_embed_still(MessageRichStore* store, const MessageRichRecord& rich, const AccountMediaFrame& frame,
                     MessageRichEmbedMediaSlot slot, u32 width, u32 height, bool* changed) noexcept {
    const u32 index = media_asset_embed_index(frame.key);
    const bool snapshot = media_asset_embed_snapshot(frame.key);
    const MessageRichRecord* owned = snapshot ? message_rich_store_snapshot(*store, frame.source_message_id) : &rich;
    if (!owned || owned->message_revision != frame.source_message_revision || index >= owned->embed_count)
        return true;
    const auto& embed = owned->embeds[index];
    const MessageRichEmbedMedia& media = slot == MessageRichEmbedMediaSlot::image ? embed.image : embed.thumbnail;
    if (!media.width || !media.height)
        return true;
    const u64 declared = u64{media.width} * height, actual = u64{width} * media.height;
    if ((declared > actual ? declared - actual : actual - declared) * 100u <= actual)
        return true;
    const u32 side = maximum(media.width, media.height);
    const u32 fitted_width = width >= height ? side : maximum<u32>(1u, static_cast<u32>((u64{side} * width + height / 2) / height));
    const u32 fitted_height = width >= height ? maximum<u32>(1u, static_cast<u32>((u64{side} * height + width / 2) / width)) : side;
    message_rich_store_set_embed_dimensions(store, frame.source_message_id, frame.source_message_revision, index, slot,
                                            fitted_width, fitted_height, changed, snapshot);
    return true;
}
} // namespace

bool message_media_apply_frame_dimensions(MessageRichStore* store, const AccountMediaFrame& frame,
                                          bool* changed) noexcept {
    if (!changed)
        return false;
    *changed = false;
    const bool video = frame.kind == AccountMediaFrameKind::video_nv12_gpu;
    if (frame.key.kind != MediaAssetKind::attachment && frame.key.kind != MediaAssetKind::embed)
        return true;
    // An embed still only refines its frame's aspect; it never fails a frame.
    const bool still = frame.key.kind == MediaAssetKind::embed && !video;
    if (!store)
        return still;
    const auto* rich = message_rich_store_find(*store, frame.source_message_id);
    if (!rich || rich->message_revision != frame.source_message_revision)
        return still;
    const u32 width = video ? frame.canvas_width : frame.width;
    const u32 height = video ? frame.canvas_height : frame.height;
    if (!width || !height)
        return still;
    if (frame.key.kind == MediaAssetKind::embed) {
        if (frame.key.id != frame.source_message_id || frame.key.variant != frame.source_message_revision ||
            (!still && media_asset_embed_format(frame.key) != MediaAttachmentFormat::mp4))
            return still;
        MessageRichEmbedMediaSlot slot{};
        switch (media_asset_embed_resource(frame.key)) {
        case MediaEmbedResource::image:
            slot = MessageRichEmbedMediaSlot::image;
            break;
        case MediaEmbedResource::thumbnail:
            slot = MessageRichEmbedMediaSlot::thumbnail;
            break;
        case MediaEmbedResource::video:
            if (still)
                return true;
            slot = MessageRichEmbedMediaSlot::video;
            break;
        default:
            return still;
        }
        if (still)
            return fit_embed_still(store, *rich, frame, slot, width, height, changed);
        return message_rich_store_set_embed_dimensions(
            store, frame.source_message_id, frame.source_message_revision,
            media_asset_embed_index(frame.key), slot, width, height, changed,
            media_asset_embed_snapshot(frame.key));
    }
    if (media_asset_search(frame.key)) {
        const bool snapshot = media_asset_search_snapshot(frame.key);
        const auto* owned = snapshot ? message_rich_store_snapshot(*store, frame.source_message_id) : rich;
        if (!owned || owned->message_revision != frame.source_message_revision ||
            media_asset_search_revision(frame.key) != frame.source_message_revision) return false;
        for (u32 i = 0; i < owned->attachment_count; ++i) {
            const auto& attachment = owned->attachments[i];
            if (attachment.id != frame.key.id) continue;
            if (!video && (attachment.state & 3u) == 3u && attachment.width && attachment.height) return true;
            return message_rich_store_set_attachment_dimensions(store, frame.source_message_id,
                frame.source_message_revision, frame.key.id, width, height, changed, snapshot);
        }
        return false;
    }
    if (!video)
        for (u32 i = 0; i < rich->attachment_count; ++i) {
            const auto& attachment = rich->attachments[i];
            if (attachment.id == frame.key.id && (attachment.state & 3u) == 3u && attachment.width &&
                attachment.height)
                return true;
        }
    const auto* child = message_rich_store_snapshot(*store, frame.source_message_id);
    bool snapshot = false;
    for (u32 i = 0; child && i < child->attachment_count; ++i) {
        const auto& attachment = child->attachments[i];
        if (attachment.id == frame.key.id) {
            if (!video && (attachment.state & 3u) == 3u && attachment.width && attachment.height)
                return true;
            snapshot = true;
        }
    }
    return message_rich_store_set_attachment_dimensions(store, frame.source_message_id,
                                                        frame.source_message_revision, frame.key.id, width,
                                                        height, changed, snapshot);
}

namespace {

MessageMediaControllerResult failure(MessageMediaController* controller, MessageMediaControllerError error,
                                     u32 visited = 0, u32 candidates = 0) noexcept {
    if (controller) {
        saturating_increment(controller->rejected);
    }
    return MessageMediaControllerResult{visited, candidates, error, false};
}

bool controller_valid(const MessageMediaController* controller) noexcept {
    return controller && controller->initialized && controller->rows && controller->asset_marks &&
           controller->row_capacity != 0 && controller->asset_mark_capacity != 0 &&
           controller->row_count <= controller->row_capacity;
}

bool media_in_load_region(const UiLayoutNode* node, UiRect viewport, bool prefetch_only) noexcept {
    if (!node || !ui_rect_has_area(viewport))
        return false;
    const bool visible = ui_rect_has_area(ui_rect_intersection(node->rectangle, viewport));
    if (!prefetch_only)
        return visible;
    viewport.top -= kMessageMediaPreloadMarginDips;
    viewport.bottom += kMessageMediaPreloadMarginDips;
    return !visible && ui_rect_has_area(ui_rect_intersection(node->rectangle, viewport));
}

bool power_of_two(u32 value) noexcept {
    return value != 0 && (value & (value - 1u)) == 0;
}

bool first_asset_observation(MessageMediaController* controller, MediaAssetKey key) noexcept {
    if (controller->asset_marks_saturated) {
        return true;
    }
    const u32 mask = controller->asset_mark_capacity - 1u;
    u32 bucket = static_cast<u32>(media_asset_hash(key)) & mask;
    for (u32 probe = 0; probe < controller->asset_mark_capacity; ++probe) {
        MessageMediaAssetMark& mark = controller->asset_marks[bucket];
        if (mark.epoch != controller->asset_mark_epoch) {
            mark.key = key;
            mark.epoch = controller->asset_mark_epoch;
            ++controller->asset_mark_count;
            if (controller->asset_mark_count >= controller->asset_mark_capacity * 3u / 4u) {
                controller->asset_marks_saturated = true;
                saturating_accumulate(controller->dedupe_saturations, 1);
            }
            return true;
        }
        if (media_asset_key_equal(mark.key, key)) {
            saturating_accumulate(controller->duplicate_assets_skipped, 1);
            return false;
        }
        bucket = (bucket + 1u) & mask;
    }
    controller->asset_marks_saturated = true;
    saturating_accumulate(controller->dedupe_saturations, 1);
    return true;
}

bool asset_resident(MediaAtlas* atlas, MediaAssetKey key) noexcept {
    return atlas && media_atlas_find(atlas, key).ok();
}

// True when the asset still needs its callback. A paused animation (play false) keeps its current
// frame: the timeline entry is only looked up, so it is never marked visible and never advances.
// A paused or held asset is left unmarked so a hovered row or group sharing it can still play it.
bool needs_request(MessageMediaController* controller, MediaAssetKey key, bool animated,
                   AnimationTimeline* animations, MediaAtlas* atlas, u64 now_us, bool play,
                   bool* force_request) noexcept {
    if (animated && !play && animation_timeline_find(animations, key).ok() && asset_resident(atlas, key))
        return false;
    if (!first_asset_observation(controller, key))
        return false;
    *force_request = false;
    if (animated) {
        const bool observed = play ? animation_timeline_observe_visible(animations, key, now_us)
                                   : animation_timeline_find(animations, key).ok();
        if (observed && asset_resident(atlas, key))
            return false;
        *force_request = !observed;
        return true;
    }
    return !asset_resident(atlas, key);
}

bool request_asset(const MessageMediaControllerCallbacks& callbacks, MediaAssetKey key, bool animated,
                   MessageMediaController* controller, AnimationTimeline* animations, MediaAtlas* atlas,
                   u64 now_us, u32* candidates, bool* stopped, bool play = true) noexcept {
    bool force_request = false;
    if (!needs_request(controller, key, animated, animations, atlas, now_us, play, &force_request))
        return true;
    ++*candidates;
    bool stop_scan = false;
    if (!callbacks.asset(callbacks.context, key, force_request, &stop_scan)) {
        return false;
    }
    *stopped = stop_scan;
    return true;
}

// A held avatar (play false) keeps its current frame like a paused animation.
bool request_avatar(const MessageMediaControllerCallbacks& callbacks, const UserRecord& user,
                    MessageMediaController* controller, AnimationTimeline* animations, MediaAtlas* atlas,
                    u64 now_us, u32* candidates, bool* stopped, bool play) noexcept {
    const Utf8View avatar_hash = user_store_avatar_hash(user);
    const u32 variant = user_avatar_variant(avatar_hash);
    if (avatar_hash.bytes == 0 || variant == 0) {
        return true;
    }
    const bool animated = asset_hash_animated(avatar_hash);
    bool force_request = false;
    if (!needs_request(controller, media_asset_user_avatar(user.id, variant, animated), animated, animations,
                       atlas, now_us, play, &force_request))
        return true;
    ++*candidates;
    bool stop_scan = false;
    if (!callbacks.avatar(callbacks.context, user, force_request, &stop_scan)) {
        return false;
    }
    *stopped = stop_scan;
    return true;
}

constexpr u64 kEmojiMemoAnimated = 1ull << 63, kEmojiMemoFlag = 1ull << 62;

// The memo slot for this text: its items when *hit, else a slot to fill.
MessageMediaEmojiMemo* emoji_memo_slot(MessageMediaController* controller, u64 message_id, u32 revision,
                                       Utf8View content, u8 scope, bool* hit) noexcept {
    *hit = false;
    if (!controller || !controller->emoji_memo || !message_id)
        return nullptr;
    const u32 slot = static_cast<u32>(hash64_mix(hash64_mix(kHash64Seed, message_id), scope)) &
                     (kMessageMediaEmojiMemoSlots - 1);
    MessageMediaEmojiMemo& memo = controller->emoji_memo->slots[slot];
    *hit = memo.message_id == message_id && memo.revision == revision && memo.bytes == content.bytes &&
           memo.scope == scope && memo.count != 0xFF;
    return &memo;
}

bool visit_inline_emojis(const MessageMediaControllerCallbacks& callbacks, Utf8View content,
                         AnimationTimeline* animations, MediaAtlas* atlas,
                         MessageMediaController* controller, u64 now_us, u32* candidates,
                         bool* stopped, bool play = true, u64 message_id = 0, u32 revision = 0,
                         u8 scope = 0) noexcept {
    if (!content.data && content.bytes != 0) {
        return false;
    }
    // A revisited message replays its parsed items in the same order.
    bool hit = false;
    MessageMediaEmojiMemo* memo = emoji_memo_slot(controller, message_id, revision, content, scope, &hit);
    if (hit) {
        for (u32 i = 0; i < memo->count; ++i) {
            const u64 item = memo->items[i];
            const bool flag = (item & kEmojiMemoFlag) != 0, animated = (item & kEmojiMemoAnimated) != 0;
            const MediaAssetKey key =
                flag ? media_asset_regional_flag(static_cast<u16>(item))
                     : media_asset_custom_emoji(item & ~(kEmojiMemoAnimated | kEmojiMemoFlag), animated);
            if (!request_asset(callbacks, key, animated, controller, animations, atlas, now_us, candidates,
                               stopped, flag || play))
                return false;
            if (*stopped)
                return true;
        }
        return true;
    }
    // Record this parse unless a stop leaves it incomplete.
    u64 memo_items[kMessageMediaEmojiMemoItems]{};
    u32 recorded = 0;
    bool memo_overflow = false;
    const auto record = [&](u64 item) noexcept {
        if (recorded < kMessageMediaEmojiMemoItems)
            memo_items[recorded] = item;
        memo_overflow = memo_overflow || recorded >= kMessageMediaEmojiMemoItems;
        ++recorded;
    };
    if (memo)
        saturating_increment(controller->emoji_memo->parses);
    u32 flag_cursor = 0;
    RegionalFlagRange flag{};
    while (regional_flag_next(content, &flag_cursor, &flag)) {
        record(kEmojiMemoFlag | flag.sequence);
        if (!request_asset(callbacks, media_asset_regional_flag(flag.sequence), false, controller,
                           animations, atlas, now_us, candidates, stopped))
            return false;
        if (*stopped)
            return true;
    }
    for (u32 begin = 0; begin < content.bytes; ++begin) {
        if (content.data[begin] != '<') {
            continue;
        }
        u32 cursor = begin + 1u;
        bool animated = false;
        if (cursor < content.bytes && content.data[cursor] == 'a') {
            animated = true;
            ++cursor;
        }
        if (cursor >= content.bytes || content.data[cursor++] != ':') {
            continue;
        }
        const u32 name_begin = cursor;
        while (cursor < content.bytes && content.data[cursor] != ':' && content.data[cursor] != '\r' &&
               content.data[cursor] != '\n' && content.data[cursor] != '>') {
            ++cursor;
        }
        if (cursor == name_begin || cursor >= content.bytes || content.data[cursor++] != ':') {
            continue;
        }
        u64 id = 0;
        u32 digits = 0;
        bool overflow = false;
        while (cursor < content.bytes && content.data[cursor] >= '0' && content.data[cursor] <= '9') {
            const u32 digit = static_cast<u32>(content.data[cursor] - '0');
            if (id > (~0ull - digit) / 10ull) {
                overflow = true;
            } else {
                id = id * 10ull + digit;
            }
            ++digits;
            ++cursor;
        }
        if (overflow || digits == 0 || id == 0 || cursor >= content.bytes || content.data[cursor] != '>') {
            continue;
        }
        record(id | (animated ? kEmojiMemoAnimated : 0));
        if (!request_asset(callbacks, media_asset_custom_emoji(id, animated), animated, controller,
                           animations, atlas, now_us, candidates, stopped, play)) {
            return false;
        }
        if (*stopped) {
            return true;
        }
        begin = cursor;
    }
    if (memo) {
        *memo = MessageMediaEmojiMemo{message_id, revision, content.bytes, scope,
                                      static_cast<u8>(memo_overflow ? 0xFF : recorded), {}};
        copy_bytes(memo->items, memo_items, sizeof(memo_items));
    }
    return true;
}

} // namespace

MediaAttachmentFormat message_media_supported_attachment_format(
    const MessageRichStore& store, const MessageRichAttachment& attachment) noexcept {
    return message_rich_attachment_format(store, attachment);
}

bool message_media_controller_initialize(MessageMediaController* controller,
                                         Slice<MessageMediaVisibleRow> rows,
                                         Slice<MessageMediaAssetMark> asset_marks) noexcept {
    if (!controller || !rows.data || rows.count == 0 || !asset_marks.data ||
        !power_of_two(asset_marks.count) || asset_marks.count < 4) {
        return false;
    }
    reset_value<MessageMediaController>(controller);
    controller->rows = rows.data;
    controller->asset_marks = asset_marks.data;
    controller->row_capacity = rows.count;
    controller->asset_mark_capacity = asset_marks.count;
    controller->asset_mark_epoch = 1;
    for (u32 index = 0; index < asset_marks.count; ++index) {
        reset_value<MessageMediaAssetMark>(&asset_marks[index]);
    }
    controller->initialized = true;
    return true;
}

bool message_media_controller_initialize_owned(MessageMediaController* controller, u32 row_capacity,
                                               u32 asset_mark_capacity) noexcept {
    if (!controller || controller->owned_rows.arena.base || controller->owned_asset_marks.arena.base ||
        row_capacity == 0 || !power_of_two(asset_mark_capacity) || asset_mark_capacity < 4) {
        return false;
    }
    reset_value<MessageMediaController>(controller);
    if (!stable_storage_initialize(&controller->owned_rows, row_capacity, row_capacity,
                                   sizeof(MessageMediaVisibleRow), alignof(MessageMediaVisibleRow), true) ||
        !stable_storage_initialize(&controller->owned_asset_marks, asset_mark_capacity, asset_mark_capacity,
                                   sizeof(MessageMediaAssetMark), alignof(MessageMediaAssetMark), true)) {
        message_media_controller_release(controller);
        return false;
    }
    controller->rows = stable_storage_data<MessageMediaVisibleRow>(controller->owned_rows);
    controller->asset_marks = stable_storage_data<MessageMediaAssetMark>(controller->owned_asset_marks);
    // Zero-filled on demand; a failed reservation only parses every visit.
    controller->emoji_memo = static_cast<MessageMediaEmojiMemoBank*>(
        VirtualAlloc(nullptr, sizeof(MessageMediaEmojiMemoBank), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    controller->row_capacity = row_capacity;
    controller->asset_mark_capacity = asset_mark_capacity;
    controller->asset_mark_epoch = 1;
    controller->initialized = true;
    controller->owns_storage = true;
    return true;
}

void message_media_controller_release(MessageMediaController* controller) noexcept {
    if (!controller) {
        return;
    }
    if (controller->owns_storage) {
        stable_storage_release(&controller->owned_asset_marks);
        stable_storage_release(&controller->owned_rows);
        if (controller->emoji_memo)
            VirtualFree(controller->emoji_memo, 0, MEM_RELEASE);
    }
    reset_value<MessageMediaController>(controller);
}

void message_media_controller_begin_pump(MessageMediaController* controller, u64 view_signature) noexcept {
    if (!controller_valid(controller)) {
        return;
    }
    controller->scan_pending = false;
    if (controller->preview_view_signature != view_signature)
        controller->preview_retry_at_ms = 0;
    controller->preview_view_signature = view_signature;
    controller->retry_at_ms = 0;
    controller->row_count = 0;
    message_media_controller_begin_asset_scan(controller);
}

bool message_media_controller_animation_plays(const MessageMediaController& controller, MediaAssetKind kind,
                                              u64 message_id) noexcept {
    const MessageRichDisplay& display = message_rich_display();
    if (kind != MediaAssetKind::custom_emoji && kind != MediaAssetKind::sticker &&
        kind != MediaAssetKind::attachment && kind != MediaAssetKind::embed)
        return true;
    const MessageMediaAnimation mode = kind == MediaAssetKind::custom_emoji ? display.emoji
                                       : kind == MediaAssetKind::sticker    ? display.stickers
                                                                            : display.gifs;
    return message_media_animates(mode, message_id && message_id == controller.interacting_message_id);
}

bool message_media_controller_set_interaction(MessageMediaController* controller, u64 message_id) noexcept {
    if (!controller || controller->interacting_message_id == message_id)
        return false;
    controller->interacting_message_id = message_id;
    controller->scan_pending = true;
    return true;
}

bool message_media_controller_set_avatar_interaction(MessageMediaController* controller, u64 message_id) noexcept {
    if (!controller || controller->avatar_message_id == message_id)
        return false;
    controller->avatar_message_id = message_id;
    controller->scan_pending = true;
    return true;
}

bool message_media_controller_set_avatar_hover(MessageMediaController* controller, u64 message_id) noexcept {
    if (!controller || controller->avatar_hover_message_id == message_id)
        return false;
    controller->avatar_hover_message_id = message_id;
    controller->scan_pending = true;
    return true;
}

void message_media_controller_begin_asset_scan(MessageMediaController* controller) noexcept {
    if (!controller_valid(controller))
        return;
    controller->asset_mark_count = 0;
    controller->asset_marks_saturated = false;
    ++controller->asset_mark_epoch;
    if (controller->asset_mark_epoch == 0) {
        for (u32 index = 0; index < controller->asset_mark_capacity; ++index) {
            controller->asset_marks[index].epoch = 0;
        }
        controller->asset_mark_epoch = 1;
    }
}

bool message_media_controller_preview_ready(MessageMediaController* controller, u64 now_ms) noexcept {
    if (!controller_valid(controller))
        return false;
    if (now_ms >= controller->preview_retry_at_ms)
        return true;
    message_media_controller_observe_retry(controller, controller->preview_retry_at_ms);
    return false;
}

void message_media_controller_defer_preview(MessageMediaController* controller, u64 now_ms) noexcept {
    if (!controller_valid(controller))
        return;
    controller->preview_retry_at_ms = deadline_after(now_ms, 1000);
    message_media_controller_observe_retry(controller, controller->preview_retry_at_ms);
}

bool message_media_primary_visible(const MessageScene& scene, u64 message_id, MediaAssetKey key) noexcept {
    if (!scene.active || !scene.timeline || !scene.timeline->store || !scene.timeline->rich_store)
        return false;
    const auto found = message_store_find(*scene.timeline->store, message_id);
    const auto* row = found.ok() ? message_scene_find_row(scene, found.order_index) : nullptr;
    if (!row || row->message_id != message_id)
        return false;
    const bool snapshot = media_asset_embed_snapshot(key);
    if (snapshot) row = message_scene_snapshot_row(scene, *row);
    const auto* rich = snapshot ? message_rich_store_snapshot(*scene.timeline->rich_store, message_id)
                               : message_rich_store_find(*scene.timeline->rich_store, message_id);
    if (!row) return false;
    if (!rich || rich->message_revision != row->message_revision)
        return false;
    if (key.kind == MediaAssetKind::embed) {
        const u32 index = media_asset_embed_index(key);
        return index < rich->embed_count &&
               media_in_load_region(
                   message_scene_rich_item_layout_node(scene, *row, MessageSceneRichItemKind::embed, index),
                   scene.viewport, false);
    }
    for (u32 i = 0; key.kind == MediaAssetKind::attachment && i < rich->attachment_count; ++i) {
        if (rich->attachments[i].id == key.id)
            return media_in_load_region(
                message_scene_rich_item_layout_node(scene, *row, MessageSceneRichItemKind::attachment, i),
                scene.viewport, false);
    }
    if (key.kind == MediaAssetKind::attachment) {
        const auto* child = message_scene_snapshot_row(scene, *row);
        const auto* child_rich = message_rich_store_snapshot(*scene.timeline->rich_store, message_id);
        for (u32 i = 0; child && child_rich && i < child_rich->attachment_count; ++i)
            if (child_rich->attachments[i].id == key.id)
                return media_in_load_region(message_scene_rich_item_layout_node(scene, *child,
                    MessageSceneRichItemKind::attachment, i), scene.viewport, false);
    }
    return false;
}

void message_media_controller_defer_scan(MessageMediaController* controller) noexcept {
    if (controller_valid(controller)) {
        controller->scan_pending = true;
    }
}

void message_media_controller_observe_retry(MessageMediaController* controller, u64 retry_at_ms) noexcept {
    if (!controller_valid(controller) || retry_at_ms == 0) {
        return;
    }
    if (controller->retry_at_ms == 0 || retry_at_ms < controller->retry_at_ms) {
        controller->retry_at_ms = retry_at_ms;
    }
}

void message_media_controller_record_request(MessageMediaController* controller) noexcept {
    if (controller_valid(controller)) {
        saturating_increment(controller->requests_queued);
    }
}

void message_media_controller_record_upload(MessageMediaController* controller) noexcept {
    if (controller_valid(controller)) {
        saturating_increment(controller->frames_uploaded);
    }
}

void message_media_controller_record_animation_advance(MessageMediaController* controller,
                                                       MediaAssetKind kind) noexcept {
    if (!controller_valid(controller))
        return;
    saturating_increment(controller->animation_advances);
    const u32 index = static_cast<u32>(kind);
    if (index < kMessageMediaAdvanceKinds && controller->animation_kind_advances[index] != 0xFFFFu)
        ++controller->animation_kind_advances[index];
}

bool message_media_controller_upload_budget_reached(MessageMediaController* controller,
                                                    u32 uploads) noexcept {
    if (!controller_valid(controller) || uploads < kMessageMediaUploadBudget) {
        return false;
    }
    controller->scan_pending = true;
    return true;
}

bool message_media_controller_should_log_capacity(MessageMediaController* controller, u64 now_ms,
                                                  u32 interval_ms) noexcept {
    if (!controller_valid(controller) || interval_ms == 0 ||
        (controller->capacity_log_at_ms != 0 && now_ms < controller->capacity_log_at_ms)) {
        return false;
    }
    controller->capacity_log_at_ms = deadline_after(now_ms, interval_ms);
    return true;
}

MessageMediaControllerDiagnostics message_media_controller_take_diagnostics(
    MessageMediaController* controller, u64 now_ms, u32 interval_ms) noexcept {
    MessageMediaControllerDiagnostics diagnostics{};
    if (!controller_valid(controller) || interval_ms == 0) {
        return diagnostics;
    }
    if (controller->summary_at_ms == 0) {
        controller->summary_at_ms = deadline_after(now_ms, interval_ms);
        return diagnostics;
    }
    if (now_ms < controller->summary_at_ms) {
        return diagnostics;
    }
    diagnostics.requests_queued = controller->requests_queued;
    diagnostics.frames_uploaded = controller->frames_uploaded;
    diagnostics.animation_advances = controller->animation_advances;
    for (u32 index = 0; index < kMessageMediaAdvanceKinds; ++index) {
        if (controller->animation_kind_advances[index] > diagnostics.animation_kind_advances) {
            diagnostics.animation_kind_advances = controller->animation_kind_advances[index];
            diagnostics.animation_kind = static_cast<MediaAssetKind>(index);
        }
        controller->animation_kind_advances[index] = 0;
    }
    diagnostics.ready = true;
    controller->requests_queued = 0;
    controller->frames_uploaded = 0;
    controller->animation_advances = 0;
    controller->summary_at_ms = deadline_after(now_ms, interval_ms);
    return diagnostics;
}

MessageMediaControllerResult message_media_controller_collect(MessageMediaController* controller,
                                                              const MessageStore& messages,
                                                              const MessageRichStore& rich_store,
                                                              const MessageScene& scene,
                                                              UiVariableRange range) noexcept {
    if (!controller_valid(controller) || !messages.records || !messages.order || !rich_store.records ||
        !scene.rows.data || range.first > messages.count || range.count > messages.count - range.first) {
        return failure(controller, MessageMediaControllerError::invalid_input);
    }
    controller->row_count = 0;
    if (range.count > controller->row_capacity) {
        return failure(controller, MessageMediaControllerError::capacity, range.count);
    }
    for (u32 visible = 0; visible < range.count; ++visible) {
        const u32 source_index = range.first + visible;
        const MessageHandle handle = message_store_ordered_handle(messages, source_index);
        const MessageRecord* message = message_store_resolve(messages, handle);
        if (!message) {
            continue;
        }
        const MessageRichRecord* rich = message_rich_store_find(rich_store, message->id);
        if (rich && rich->message_revision != message->revision) {
            rich = nullptr;
        }
        const MessageSceneRowWidget* scene_row = message_scene_find_row(scene, source_index);
        if (scene_row && scene_row->message_id != message->id) {
            scene_row = nullptr;
        }
        controller->rows[controller->row_count++] =
            MessageMediaVisibleRow{message, rich, scene_row, handle, source_index};
    }
    saturating_increment(controller->collections);
    saturating_accumulate(controller->source_rows_visited, range.count);
    saturating_accumulate(controller->rows_collected, controller->row_count);
    return MessageMediaControllerResult{range.count, controller->row_count,
                                        MessageMediaControllerError::none, false};
}

MessageMediaControllerResult message_media_controller_visit_primary(
    MessageMediaController* controller, const MessageRichStore& rich_store, const MessageScene& scene,
    const MessageMediaControllerCallbacks& callbacks, bool prefetch_only) noexcept {
    if (!controller_valid(controller) || !rich_store.records || !callbacks.attachment || !callbacks.embed) {
        return failure(controller, MessageMediaControllerError::invalid_input);
    }
    u32 candidates = 0;
    for (u32 visible = 0; visible < controller->row_count; ++visible) {
        for (u32 content_scope = 0; content_scope < 2; ++content_scope) {
        MessageMediaVisibleRow row = controller->rows[visible];
        if (content_scope && row.message && row.scene) {
            row.rich = message_rich_store_snapshot(rich_store, row.message->id);
            row.scene = message_scene_snapshot_row(scene, *row.scene);
        }
        if (!row.message || !row.rich || !row.scene) {
            continue;
        }
        for (u32 attachment_index = 0; attachment_index < row.rich->attachment_count; ++attachment_index) {
            const MessageRichAttachment& attachment = row.rich->attachments[attachment_index];
            if (message_media_supported_attachment_format(rich_store, attachment) ==
                MediaAttachmentFormat::unknown) {
                continue;
            }
            const UiLayoutNode* item_layout = message_scene_rich_item_layout_node(
                scene, *row.scene, MessageSceneRichItemKind::attachment, attachment_index);
            float preview_width = 0.0f;
            float preview_height = 0.0f;
            if (!media_in_load_region(item_layout, scene.viewport, prefetch_only) ||
                !message_rich_attachment_preview_extent(rich_store, attachment,
                                                        ui_rect_width(item_layout->content_rectangle),
                                                        &preview_width, &preview_height)) {
                continue;
            }
            ++candidates;
            bool stop_scan = false;
            if (!callbacks.attachment(callbacks.context, *row.message, attachment, preview_width,
                                      preview_height, &stop_scan)) {
                return failure(controller, MessageMediaControllerError::callback, visible + 1u, candidates);
            }
            if (stop_scan) {
                saturating_accumulate(controller->primary_candidates, candidates);
                return MessageMediaControllerResult{visible + 1u, candidates,
                                                    MessageMediaControllerError::none, true};
            }
        }
        for (u32 embed_index = 0; message_rich_display().embeds && embed_index < row.rich->embed_count;
             ++embed_index) {
            // Gallery images (a merged embed's included) load at their tile size.
            MessageRichEmbedGalleryPlace place{};
            const bool tile = message_rich_embed_gallery_place(rich_store, *row.rich, embed_index, &place);
            const u32 host = tile ? place.host : message_rich_embed_merge_host(rich_store, *row.rich, embed_index);
            if (host != embed_index && !tile)
                continue;
            const UiLayoutNode* item_layout = message_scene_rich_item_layout_node(
                scene, *row.scene, MessageSceneRichItemKind::embed, host);
            float preview_width = row.scene->rich_item_layout.embed_media_widths[host];
            float preview_height = row.scene->rich_item_layout.embed_media_heights[host];
            if (tile) {
                const UiRect cell = message_rich_embed_gallery_tile({0, 0, preview_width, preview_height},
                                                                    place.count, place.ordinal);
                preview_width = ui_rect_width(cell);
                preview_height = ui_rect_height(cell);
            }
            float thumb_width = 0, thumb_height = 0;
            const bool thumbnail = host == embed_index && message_rich_embed_thumbnail_extent(
                rich_store, row.rich->embeds[embed_index], 80, &thumb_width, &thumb_height);
            if (!media_in_load_region(item_layout, scene.viewport, prefetch_only) ||
                (!thumbnail && (!(preview_width > 0.0f) || !(preview_height > 0.0f)))) {
                continue;
            }
            ++candidates;
            bool stop_scan = false;
            if (!callbacks.embed(callbacks.context, *row.message, *row.rich, embed_index, preview_width,
                                 preview_height, &stop_scan)) {
                return failure(controller, MessageMediaControllerError::callback, visible + 1u, candidates);
            }
            if (stop_scan) {
                saturating_accumulate(controller->primary_candidates, candidates);
                return MessageMediaControllerResult{visible + 1u, candidates,
                                                    MessageMediaControllerError::none, true};
            }
        }
    }
        }
    saturating_accumulate(controller->primary_candidates, candidates);
    return MessageMediaControllerResult{controller->row_count, candidates,
                                        MessageMediaControllerError::none, false};
}

MessageMediaControllerResult message_media_controller_visit_decorations(
    MessageMediaController* controller, const MessageStore& messages, const UserStore& users,
    AnimationTimeline* animations, MediaAtlas* media_atlas, u64 now_us,
    const MessageMediaControllerCallbacks& callbacks, const MessageRichStore* rich_store) noexcept {
    if (!controller_valid(controller) || !messages.records || !messages.text || !users.records ||
        !animations || !media_atlas || !callbacks.avatar || !callbacks.asset) {
        return failure(controller, MessageMediaControllerError::invalid_input);
    }
    u32 candidates = 0;
    const auto stopped_at = [&](u32 visible) noexcept {
        saturating_accumulate(controller->decoration_candidates, candidates);
        return MessageMediaControllerResult{visible + 1u, candidates, MessageMediaControllerError::none, true};
    };
    for (u32 visible = 0; visible < controller->row_count; ++visible) {
        const MessageMediaVisibleRow& row = controller->rows[visible];
        if (!row.message) {
            continue;
        }
        const UserRecord* author = user_store_find(users, row.message->author_id);
        bool stopped = false;
        if (author && !request_avatar(callbacks, *author, controller, animations, media_atlas, now_us,
                                      &candidates, &stopped,
                                      row.message->id == controller->avatar_message_id)) {
            return failure(controller, MessageMediaControllerError::callback, visible + 1u, candidates);
        }
        if (stopped)
            return stopped_at(visible);
        const bool emoji_plays =
            message_media_controller_animation_plays(*controller, MediaAssetKind::custom_emoji, row.message->id);
        const bool stickers_play =
            message_media_controller_animation_plays(*controller, MediaAssetKind::sticker, row.message->id);
        if (!visit_inline_emojis(callbacks, message_store_content(messages, row.handle), animations,
                                 media_atlas, controller, now_us, &candidates, &stopped, emoji_plays,
                                 row.message->id, row.message->revision, 0)) {
            return failure(controller, MessageMediaControllerError::callback, visible + 1u, candidates);
        }
        if (stopped)
            return stopped_at(visible);
        for (u32 content_scope = 0; content_scope < 2; ++content_scope) {
        if (content_scope && !rich_store) continue;
        const auto* content_rich = content_scope && rich_store
            ? message_rich_store_snapshot(*rich_store, row.message->id) : row.rich;
        if (content_scope && content_rich && rich_store &&
            !visit_inline_emojis(callbacks, message_rich_store_string(*rich_store, content_rich->snapshot.content),
                                 animations, media_atlas, controller, now_us, &candidates, &stopped, emoji_plays,
                                 row.message->id, content_rich->message_revision, 1))
            return failure(controller, MessageMediaControllerError::callback, visible + 1u, candidates);
        if (stopped) return {visible + 1u, candidates, MessageMediaControllerError::none, true};
        if (!content_rich) {
            continue;
        }
        if (!content_scope && message_rich_has_header(*content_rich) && message_rich_header_user(*content_rich) != 0) {
            const UserRecord* reference_author =
                user_store_find(users, message_rich_header_user(*content_rich));
            // Reply previews show the referenced author's avatar still.
            if (reference_author && !request_avatar(callbacks, *reference_author, controller, animations,
                                                    media_atlas, now_us, &candidates, &stopped, false)) {
                return failure(controller, MessageMediaControllerError::callback, visible + 1u, candidates);
            }
            if (stopped)
                return stopped_at(visible);
        }
        for (u32 sticker = 0; sticker < content_rich->sticker_count; ++sticker) {
            const MessageRichSticker& value = content_rich->stickers[sticker];
            const MediaAssetKey key = media_asset_sticker(value.id, value.format_type);
            const bool animated = value.format_type == 2 || value.format_type == 4;
            if (value.format_type != 1 && value.format_type != 3 && !animated) {
                continue;
            }
            if (!request_asset(callbacks, key, animated, controller, animations, media_atlas, now_us,
                               &candidates, &stopped, stickers_play)) {
                return failure(controller, MessageMediaControllerError::callback, visible + 1u, candidates);
            }
            if (stopped)
                return stopped_at(visible);
        }
        // Button emoji: the custom emoji drawn ahead of a component's label.
        for (u32 index = 0; index < content_rich->component_count; ++index) {
            const MessageRichComponent& value = content_rich->components[index];
            if (value.type != gateway_component_button || value.emoji_id == 0)
                continue;
            const bool animated = (value.flags & gateway_message_component_emoji_animated) != 0;
            if (!request_asset(callbacks, media_asset_custom_emoji(value.emoji_id, animated), animated, controller,
                               animations, media_atlas, now_us, &candidates, &stopped, emoji_plays))
                return failure(controller, MessageMediaControllerError::callback, visible + 1u, candidates);
            if (stopped)
                return stopped_at(visible);
        }
        for (u32 reaction = 0; !content_scope && message_rich_display().reactions &&
                                reaction < content_rich->reaction_count;
             ++reaction) {
            const MessageRichReaction& value = content_rich->reactions[reaction];
            const u16 flag =
                value.emoji_id == 0 && rich_store
                    ? regional_flag_sequence(message_rich_store_string(*rich_store, value.name))
                    : 0;
            if (value.emoji_id == 0 && !flag) {
                continue;
            }
            const bool animated = (value.flags & 4u) != 0;
            if (!request_asset(callbacks,
                               flag ? media_asset_regional_flag(flag)
                                    : media_asset_custom_emoji(value.emoji_id, animated),
                               animated, controller, animations, media_atlas, now_us, &candidates,
                               &stopped, emoji_plays)) {
                return failure(controller, MessageMediaControllerError::callback, visible + 1u, candidates);
            }
            if (stopped)
                return stopped_at(visible);
        }
    }
        }
    saturating_accumulate(controller->decoration_candidates, candidates);
    return MessageMediaControllerResult{controller->row_count, candidates,
                                        MessageMediaControllerError::none, false};
}

MessageMediaControllerResult message_media_controller_visit_asset(
    MessageMediaController* controller, MediaAssetKey key, bool animated,
    AnimationTimeline* animations, MediaAtlas* media_atlas, u64 now_us,
    const MessageMediaControllerCallbacks& callbacks) noexcept {
    if (!controller_valid(controller) || !media_asset_key_valid(key) || !animations || !media_atlas ||
        !callbacks.asset)
        return failure(controller, MessageMediaControllerError::invalid_input);
    u32 candidates = 0;
    bool stopped = false;
    if (!request_asset(callbacks, key, animated, controller, animations, media_atlas, now_us,
                       &candidates, &stopped))
        return failure(controller, MessageMediaControllerError::callback, 0, candidates);
    saturating_accumulate(controller->decoration_candidates, candidates);
    return {0, candidates, MessageMediaControllerError::none, stopped};
}

} // namespace maicord
