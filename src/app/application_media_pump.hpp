#pragma once

#include "application_media.hpp"
#include "maicord/ui/profile_media.hpp"
#include "maicord/ui/friends_media.hpp"
#include "application_state.hpp"

namespace maicord {
enum class CdnImageType : u8;
}

namespace maicord::app_detail {

struct ApplicationMediaPump {
    explicit ApplicationMediaPump(Application& source) noexcept : application(source) {
    }

    enum class MediaFrameUploadResult : u8 {
        failed,
        uploaded,
        deferred,
    };

    struct MessageMediaPumpContext {
        ApplicationMediaPump* owner;
        u64 now_ms;
        u64 now_seconds;
        u32* uploads;
        bool* repaint;
    };

    bool request_profile_media(const ProfileMedia& media, u64 now_ms, u64 now_seconds, u64 now_us,
                               u32* uploads, bool* repaint, bool* stop, bool play = true,
                               AccountMediaRequestResult* failure = nullptr) noexcept;
    // Go Live preview thumbnails shown by the stream card and call tiles.
    bool request_stream_previews(u64 now_ms, u64 now_seconds, u32* uploads, bool* repaint, bool* stop) noexcept;
    Application& application;
    ApplicationMediaPumpResult result{};
    bool prefetch_only = false;
    bool visibility_only = false;
    bool content_media_visible() const noexcept;
    // Chrome-only playback while the thread browser covers the chat column.
    bool pump_covered_animations(bool* repaint) noexcept;
    bool catalog_media_visible() const noexcept;
    bool search_media_visible() const noexcept;
    bool request_picker_emoji(u64 now_ms, u64 now_seconds, u64 now_us, u32* uploads,
                              bool* repaint) noexcept;
    bool request_inline_flags(u64 now_ms, u64 now_seconds, u32* uploads, bool* repaint) noexcept;
    bool request_catalog_previews(u64 now_ms, u64 now_seconds, u64 now_us, u32* uploads,
                                  bool* repaint) noexcept;
    bool media_key_is_current(const MediaAssetKey& key) noexcept;
    // An image of the open profile card that found the atlas full unpins the
    // visible member rows' nameplates and avatar decorations; returns how many
    // (zero for any other key). The caller retries its upload when nonzero.
    u32 yield_decorative_media(const MediaAssetKey& key) noexcept;
    // True when an animated key is resident and its timeline is observed (play) or merely present (held
    // frame). Otherwise force_request says whether the request must bypass its residency check.
    bool animation_resident(const MediaAssetKey& key, u64 now_us, bool* force_request, bool play = true) noexcept;
    bool avatar_key_is_current(const MediaAssetKey& key) noexcept;
    // Custom group DM icons on visible conversation rows or the DM header.
    bool channel_icon_key_is_current(const MediaAssetKey& key) noexcept;
    bool request_pinned_messages_avatars(u64 now_ms, u64 now_seconds, u64 now_us, u32* uploads,
                                         bool* repaint, bool* stop) noexcept;
    bool request_forum_avatars(u64 now_ms, u64 now_seconds, u64 now_us, u32* uploads, bool* repaint,
                               bool* stop) noexcept;
    bool request_forum_thumbnails(u64 now_ms, u64 now_seconds, u32* uploads, bool* repaint, bool* stop) noexcept;
    bool request_friends_avatars(u64 now_ms, u64 now_seconds, u64 now_us, u32* uploads, bool* repaint,
                                 bool* stop) noexcept;
    // Forward-destination tiles: server icons (the rail's key) and DM avatars.
    bool request_forward_identities(u64 now_ms, u64 now_seconds, u32* uploads, bool* repaint, bool* stop) noexcept;
    // Opaque Settings: avatar rows (My account card) share the user-avatar cache.
    bool settings_avatar_is_current(const MediaAssetKey& key) const noexcept;
    bool request_settings_avatars(u64 now_ms, u64 now_seconds, u32* uploads, bool* repaint) noexcept;

    __declspec(noinline) bool animated_webp_frame_kind(AccountMediaFrameKind kind) noexcept;
    __declspec(noinline) bool video_frame_kind(AccountMediaFrameKind kind) noexcept;
    __declspec(noinline) bool animation_frame_cache_descriptor(
        const AccountMediaFrame& frame, AnimationFrameCacheDescriptor* descriptor) noexcept;
    __declspec(noinline) void capture_animation_frame(const AccountMediaFrame& frame) noexcept;
    __declspec(noinline) bool animation_frame_kind_compatible(AccountMediaFrameKind left,
                                                              AccountMediaFrameKind right) noexcept;
    __declspec(noinline) MediaFrameUploadResult
    upload_account_media_frame(const AccountMediaFrame& frame) noexcept;
    __declspec(noinline) AnimationVisualState* find_animation_visual(const MediaAssetKey& key,
                                                                     u32 generation) noexcept;
    __declspec(noinline) void release_animation_visual(MediaAssetKey key, u32 generation) noexcept;
    __declspec(noinline) bool apply_animation_frame(const AnimationFrameAdvance& advance,
                                                    const AccountMediaFrame& frame, bool* repaint) noexcept;
    bool animation_frame_is_current(const AnimationFrameAdvance& advance, const AccountMediaFrame& frame,
                                    const AnimationVisualState& visual) noexcept;
    bool animation_advance_is_current(const AnimationFrameAdvance& advance,
                                      const AnimationVisualState& visual) noexcept;
    void retire_stale_message_animation(const MediaAssetKey& key, u64 source_message_id,
                                        u32 source_message_revision) noexcept;
    __declspec(noinline) bool restore_cached_animation_frame(const AnimationFrameAdvance& advance,
                                                             AnimationVisualState* visual,
                                                             bool* repaint) noexcept;
    static constexpr u32 pending_animation_capacity() noexcept {
        return kAnimationPendingAdvanceCapacity;
    }
    __declspec(noinline) u32 find_pending_animation(const AnimationFrameAdvance& advance) noexcept;
    __declspec(noinline) AnimationFrameAdvance remove_pending_animation(u32 index) noexcept;
    __declspec(noinline) bool consume_pending_animation_frame(const AccountMediaFrame& frame, bool* repaint,
                                                              bool* consumed) noexcept;
    __declspec(noinline) bool pump_due_animations(bool* repaint) noexcept;

    __declspec(noinline) bool request_visible_static_media(const MediaAssetKey& key, u64 now_ms, u64 now_seconds,
                                                           u32* uploads, bool* repaint, bool* stop_scan,
                                                           bool force_request = false,
                                                           bool continue_on_busy = false) noexcept;
    // animate false requests the still rendition even for an a_ hash (a
    // non-animated icon URL); true the animation an a_ hash has.
    __declspec(noinline) bool request_visible_guild_icon(const GuildRecord& guild, u64 now_ms,
                                                         u64 now_seconds, u32* uploads, bool* repaint,
                                                         bool* stop_scan,
                                                         bool force_request = false,
                                                         bool animate = true) noexcept;
    __declspec(noinline) bool request_visible_user_avatar_value(u64 user_id, const Utf8View& avatar_hash,
                                                                u64 now_ms, u64 now_seconds, u32* uploads,
                                                                bool* repaint, bool* stop_scan,
                                                                bool force_request = false) noexcept;
    // Shared request path for hash-addressed user avatars and group DM icons.
    __declspec(noinline) bool request_visible_hashed_identity(const MediaAssetKey& key, const Utf8View& hash,
                                                              u64 now_ms, u64 now_seconds, u32* uploads,
                                                              bool* repaint, bool* stop_scan,
                                                              bool force_request = false) noexcept;
    // CDN ?size= for shared avatar/icon identities at the renderer DPI.
    u32 identity_cdn_size() const noexcept;
    __declspec(noinline) bool request_visible_user_avatar(const UserRecord& user, u64 now_ms,
                                                          u64 now_seconds, u32* uploads, bool* repaint,
                                                          bool* stop_scan,
                                                          bool force_request = false) noexcept;
    // Finishes a request for one image: uploads a ready frame (the scan stops on a deferred upload or
    // the upload budget) and records the retry time of anything else. A busy lane stops the scan if
    // stop_on_busy.
    __declspec(noinline) bool settle_media_request(const AccountMediaRequestResult& requested,
                                                   const MediaAssetKey& key, u32* uploads, bool* repaint,
                                                   bool* stop_scan, bool stop_on_busy) noexcept;
    // The same for message attachments and embeds, whose frames must belong to the shown message.
    __declspec(noinline) bool settle_message_media(const AccountMediaRequestResult& requested,
                                                   const MediaAssetKey& key, u64 message_id, u32* uploads,
                                                   bool* repaint, bool* stop_scan) noexcept;
    __declspec(noinline) void log_message_media_failure(const char* item,
                                                        const AccountMediaRequestResult& failure,
                                                        const MediaAssetKey& key) noexcept;
    __declspec(noinline) void record_media_request_queued() noexcept;
    __declspec(noinline) void record_media_animation_advance(MediaAssetKind kind) noexcept;
    __declspec(noinline) void record_media_frame_uploaded() noexcept;
    __declspec(noinline) void flush_media_success_diagnostics(u64 now_ms) noexcept;
    __declspec(noinline) CdnImageType cdn_image_type(MediaAttachmentFormat format) noexcept;
    __declspec(noinline) bool message_image_frame_is_current(const AccountMediaFrame& frame) noexcept;
    bool message_image_source_is_current(const MediaAssetKey& key, u64 source_message_id,
                                         u32 source_message_revision) noexcept;
    __declspec(noinline) bool embed_media_dimensions_supported(
        const MessageRichEmbedMediaSelection& selected) noexcept;
    __declspec(noinline) bool update_media_display_dimensions(const AccountMediaFrame& frame,
                                                              bool* changed) noexcept;
    __declspec(noinline) bool request_visible_attachment(const MessageRecord& message,
                                                         const MessageRichAttachment& attachment,
                                                         float preview_width, float preview_height,
                                                         u64 now_ms, u64 now_seconds, u32* uploads,
                                                         bool* repaint, bool* stop_scan,
                                                         const MessageRichStore* search_store = nullptr,
                                                         bool search_snapshot = false) noexcept;
    __declspec(noinline) bool request_visible_embed_media(const MessageRecord& message,
                                                          const MessageRichRecord& rich, u32 embed_index,
                                                          float preview_width, float preview_height,
                                                          u64 now_ms, u64 now_seconds, u32* uploads,
                                                          bool* repaint, bool* stop_scan,
                                                          bool thumbnail = false,
                                                          const MessageRichStore* search_store = nullptr) noexcept;
    bool request_search_previews(u64 now_ms, u64 now_seconds, u32* uploads,
                                  bool* repaint, bool* stop_scan) noexcept;
    bool request_search_decorations(u64 now_ms, u64 now_seconds, u64 now_us, u32* uploads,
                                    bool* repaint, bool* stop_scan) noexcept;
    // Server icons of link chips in rendered timeline or Search text.
    bool request_chip_guild_icons(bool search, u64 now_ms, u64 now_seconds, u32* uploads, bool* repaint,
                                  bool* stop_scan) noexcept;

    static bool request_controller_attachment(void* context, const MessageRecord& message,
                                              const MessageRichAttachment& attachment, float preview_width,
                                              float preview_height, bool* stop_scan) noexcept;
    static bool request_controller_embed(void* context, const MessageRecord& message,
                                         const MessageRichRecord& rich, u32 embed_index,
                                         float preview_width, float preview_height,
                                         bool* stop_scan) noexcept;
    static bool request_controller_avatar(void* context, const UserRecord& user, bool force_request,
                                          bool* stop_scan) noexcept;
    static bool request_controller_asset(void* context, MediaAssetKey key, bool force_request,
                                         bool* stop_scan) noexcept;

    // Visibility traverses every existing scene without fetch/upload admission.
    // The normal request pass may then stop at a budget without pausing peers.
    __declspec(noinline) bool scan_visible_media(u64 now_ms, u64 now_seconds, u64 now_us, u32& uploads,
                                                 bool* repaint) noexcept;
    __declspec(noinline) bool pump_visible_static_media(bool* repaint) noexcept;
};

} // namespace maicord::app_detail
