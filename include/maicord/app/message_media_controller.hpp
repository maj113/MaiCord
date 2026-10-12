#pragma once

#include "maicord/app/message_view_runtime.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/media/animation_timeline.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/render/media_atlas.hpp"
#include "maicord/ui/message_scene.hpp"

namespace maicord {

struct AccountMediaFrame;

// Apply an admitted frame's layout dimensions to its exact current source.
// Static CDN transforms cannot replace known source geometry. Native video
// canvases describe display geometry independently of their GPU pixel size.
bool message_media_apply_frame_dimensions(MessageRichStore* store, const AccountMediaFrame& frame,
                                          bool* changed) noexcept;

inline constexpr u32 kMessageMediaUploadBudget = 4;
inline constexpr u32 kMessageMediaCompletionDrainBudget = 8;
inline constexpr u32 kMessageMediaAssetMarkCapacity = 8'192;
inline constexpr float kMessageMediaPreloadMarginDips = 300.0f;

enum class MessageMediaControllerError : u8 {
    none,
    invalid_input,
    capacity,
    callback,
};

struct MessageMediaVisibleRow {
    const MessageRecord* message;
    const MessageRichRecord* rich;
    const MessageSceneRowWidget* scene;
    MessageHandle handle;
    u32 source_index;
};

static_assert(sizeof(MessageMediaVisibleRow) == 40);

struct MessageMediaAssetMark {
    MediaAssetKey key;
    u32 epoch;
    u32 reserved;
};

static_assert(sizeof(MessageMediaAssetMark) == 24);

struct MessageMediaControllerResult {
    u32 visited;
    u32 candidates;
    MessageMediaControllerError error;
    bool stopped;

    constexpr bool ok() const noexcept {
        return error == MessageMediaControllerError::none;
    }
};

struct MessageMediaControllerDiagnostics {
    u32 requests_queued;  // Fetches handed to the media workers (not animation frames).
    u32 frames_uploaded;
    // Animation frame advances (decoded GIF/WebP/APNG/video frames of visible
    // media) are playback, not fetches; counted apart with the busiest kind.
    u32 animation_advances;
    u32 animation_kind_advances;
    MediaAssetKind animation_kind;
    bool ready;
};
inline constexpr u32 kMessageMediaAdvanceKinds = 13;

using MessageMediaAttachmentCallback = bool (*)(void* context, const MessageRecord& message,
                                                const MessageRichAttachment& attachment,
                                                float preview_width, float preview_height,
                                                bool* stop_scan) noexcept;
using MessageMediaEmbedCallback = bool (*)(void* context, const MessageRecord& message,
                                           const MessageRichRecord& rich, u32 embed_index,
                                           float preview_width, float preview_height,
                                           bool* stop_scan) noexcept;
using MessageMediaAvatarCallback = bool (*)(void* context, const UserRecord& user,
                                            bool force_request, bool* stop_scan) noexcept;
using MessageMediaAssetCallback = bool (*)(void* context, MediaAssetKey key, bool force_request,
                                           bool* stop_scan) noexcept;

struct MessageMediaControllerCallbacks {
    void* context;
    MessageMediaAttachmentCallback attachment;
    MessageMediaEmbedCallback embed;
    MessageMediaAvatarCallback avatar;
    MessageMediaAssetCallback asset;
};

// The inline custom emoji and regional flags of one message text, in request
// order, so revisiting a row does not parse its text again. Items hold an
// emoji id (bit 63: animated) or a flag sequence (bit 62).
inline constexpr u32 kMessageMediaEmojiMemoItems = 7;
inline constexpr u32 kMessageMediaEmojiMemoSlots = 128;
struct MessageMediaEmojiMemo {
    u64 message_id;
    u32 revision;
    u32 bytes;
    u8 scope;
    u8 count; // 0xFF: more items than fit; the text is parsed each visit.
    u64 items[kMessageMediaEmojiMemoItems];
};
struct MessageMediaEmojiMemoBank {
    MessageMediaEmojiMemo slots[kMessageMediaEmojiMemoSlots];
    u32 parses; // Texts parsed (memo misses).
};

struct MessageMediaController {
    MessageMediaVisibleRow* rows;
    MessageMediaAssetMark* asset_marks;
    MessageMediaEmojiMemoBank* emoji_memo; // Owned storage only; null parses every visit.
    StableStorage owned_rows;
    StableStorage owned_asset_marks;
    u32 row_capacity;
    u32 row_count;
    u32 asset_mark_capacity;
    u32 asset_mark_count;
    u32 asset_mark_epoch;
    bool scan_pending;
    bool asset_marks_saturated;
    bool initialized;
    u64 retry_at_ms;
    u64 preview_retry_at_ms;
    u64 preview_view_signature;
    u64 interacting_message_id; // hovered or focused message for interaction-only animation
    u64 avatar_message_id;      // group head whose author avatar plays (on group hover)
    u64 avatar_hover_message_id; // message whose avatar is under the pointer
    u64 capacity_log_at_ms;
    u64 summary_at_ms;
    u64 source_rows_visited;
    u64 rows_collected;
    u64 primary_candidates;
    u64 decoration_candidates;
    u64 duplicate_assets_skipped;
    u64 dedupe_saturations;
    u32 collections;
    u32 requests_queued;
    u32 frames_uploaded;
    u32 animation_advances;
    u16 animation_kind_advances[kMessageMediaAdvanceKinds];
    u32 rejected;
    bool owns_storage;
};

static_assert(sizeof(MessageMediaController) < 384);

bool message_media_controller_initialize(MessageMediaController* controller,
                                         Slice<MessageMediaVisibleRow> rows,
                                         Slice<MessageMediaAssetMark> asset_marks) noexcept;
bool message_media_controller_initialize_owned(MessageMediaController* controller, u32 row_capacity,
                                               u32 asset_mark_capacity) noexcept;
void message_media_controller_release(MessageMediaController* controller) noexcept;
void message_media_controller_begin_pump(MessageMediaController* controller,
                                         u64 view_signature = 0) noexcept;
// A complete visibility pass and the later bounded admission pass have
// independent duplicate suppression, while sharing the existing mark bank.
void message_media_controller_begin_asset_scan(MessageMediaController* controller) noexcept;
// Chat animation policy (message_rich_display) for a message's media of this
// kind: emoji, stickers, attachments and embeds as GIFs; other kinds play.
bool message_media_controller_animation_plays(const MessageMediaController&, MediaAssetKind,
                                              u64 message_id) noexcept;
// Returns true and schedules a scan when the interacting message changed.
bool message_media_controller_set_interaction(MessageMediaController*, u64 message_id) noexcept;
// The official client animates an author avatar only while its message group (or the
// avatar itself) is hovered: message_id is that group's first message, zero
// when no avatar plays. Other visible avatars hold their current frame.
bool message_media_controller_set_avatar_interaction(MessageMediaController*, u64 message_id) noexcept;
// The message whose author avatar is under the pointer; a hovered avatar
// plays even when GIFs do not autoplay. Schedules a scan when it changed.
bool message_media_controller_set_avatar_hover(MessageMediaController*, u64 message_id) noexcept;
bool message_media_controller_preview_ready(MessageMediaController* controller, u64 now_ms) noexcept;
void message_media_controller_defer_preview(MessageMediaController* controller, u64 now_ms) noexcept;
bool message_media_primary_visible(const MessageScene& scene, u64 message_id, MediaAssetKey key) noexcept;
void message_media_controller_defer_scan(MessageMediaController* controller) noexcept;
void message_media_controller_observe_retry(MessageMediaController* controller, u64 retry_at_ms) noexcept;
void message_media_controller_record_request(MessageMediaController* controller) noexcept;
void message_media_controller_record_upload(MessageMediaController* controller) noexcept;
void message_media_controller_record_animation_advance(MessageMediaController* controller,
                                                       MediaAssetKind kind) noexcept;
bool message_media_controller_upload_budget_reached(MessageMediaController* controller,
                                                    u32 uploads) noexcept;
bool message_media_controller_should_log_capacity(MessageMediaController* controller, u64 now_ms,
                                                  u32 interval_ms = 5'000) noexcept;
MessageMediaControllerDiagnostics message_media_controller_take_diagnostics(
    MessageMediaController* controller, u64 now_ms, u32 interval_ms = 5'000) noexcept;
MediaAttachmentFormat message_media_supported_attachment_format(
    const MessageRichStore& store, const MessageRichAttachment& attachment) noexcept;

MessageMediaControllerResult message_media_controller_collect(MessageMediaController* controller,
                                                              const MessageStore& messages,
                                                              const MessageRichStore& rich_store,
                                                              const MessageScene& scene,
                                                              UiVariableRange range) noexcept;
MessageMediaControllerResult message_media_controller_visit_primary(
    MessageMediaController* controller, const MessageRichStore& rich_store, const MessageScene& scene,
    const MessageMediaControllerCallbacks& callbacks, bool prefetch_only = false) noexcept;
MessageMediaControllerResult message_media_controller_visit_decorations(
    MessageMediaController* controller, const MessageStore& messages, const UserStore& users,
    AnimationTimeline* animations, MediaAtlas* media_atlas, u64 now_us,
    const MessageMediaControllerCallbacks& callbacks,
    const MessageRichStore* rich_store = nullptr) noexcept;
// Parsed foreground surfaces share the current bounded asset marks and
// animation visibility epoch with timeline decorations.
MessageMediaControllerResult message_media_controller_visit_asset(
    MessageMediaController* controller, MediaAssetKey key, bool animated,
    AnimationTimeline* animations, MediaAtlas* media_atlas, u64 now_us,
    const MessageMediaControllerCallbacks& callbacks) noexcept;

} // namespace maicord
