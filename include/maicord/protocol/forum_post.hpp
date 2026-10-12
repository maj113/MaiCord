#pragma once

#include "maicord/protocol/thread_creation.hpp"
#include "maicord/protocol/attachment_upload.hpp"
#include "maicord/protocol/gateway_message_events.hpp"

namespace maicord {
inline constexpr u32 kForumPostTitleMaximumScalars = 100;
inline constexpr u32 kForumPostTitleMaximumBytes = 400;
inline constexpr u32 kForumPostContentMaximumScalars = 2000;
inline constexpr u32 kForumPostContentMaximumBytes = 8000;
inline constexpr u32 kForumPostAvailableTagMaximum = 20;
inline constexpr u32 kForumPostAppliedTagMaximum = 5;
inline constexpr u32 kForumPostTagNameMaximumScalars = 20;
inline constexpr u32 kForumPostTagNameMaximumBytes = 80;
inline constexpr u32 kForumPostMaximumTargetBytes = 96;
inline constexpr u32 kForumPostMaximumBodyBytes = 65536;
inline constexpr u32 kForumPostMaximumResponseBytes = 262144;
inline constexpr u32 kForumPostAttachmentTextBytes = 65536;
inline constexpr u16 kForumPostDefaultArchiveDuration = 4320;
inline constexpr u32 kForumRequireTagFlag = 1u << 4;
inline constexpr u64 kForumPostRouteId = 0x464f52554d504f53ull;

enum class ForumPostOperation : u8 {
    parent_metadata,
    create,
    attachment_url
};
struct ForumPostRequest {
    u64 parent_id, guild_id;
    Utf8View title, content;
    u64 applied_tag_ids[kForumPostAppliedTagMaximum];
    u16 auto_archive_duration;
    u8 tag_count;
    ForumPostOperation operation;
    u8 reserved[4];
    ConstSlice<UploadedAttachment> attachments;
    Utf8View upload_filename;
    u64 upload_bytes;
    u32 upload_id, upload_reserved;
};
enum class ForumPostError : u8 {
    none,
    input,
    capacity,
    json,
    duplicate,
    identity,
    field
};
struct ForumPostResult {
    u32 bytes{};
    ForumPostError error{};
    constexpr bool ok() const noexcept {
        return error == ForumPostError::none;
    }
};
struct ForumTagSnapshot {
    u64 id;
    u32 name_bytes;
    bool moderated;
    u8 reserved[3];
    char name[kForumPostTagNameMaximumBytes];
};
enum ForumParentField : u8 {
    forum_parent_flags = 1,
    forum_parent_tags = 2,
    forum_parent_default_archive_duration = 4,
    // Post list defaults: sort 0 latest activity / 1 creation date; layout
    // 0 not set / 1 list / 2 gallery; tag setting 0 match_some / 1 match_all.
    forum_parent_default_sort_order = 8,
    forum_parent_default_forum_layout = 16,
    forum_parent_default_tag_setting = 32,
};
// Absent flags/tag inventory stay unknown and cannot authorize creation.
// An absent default auto-archive duration falls back to 4320 minutes.
struct ForumParentSnapshot {
    u64 id, guild_id;
    u32 flags, name_bytes;
    u16 default_auto_archive_duration;
    u8 tag_count, fields, type, default_sort_order, default_forum_layout, default_tag_setting;
    char name[kForumPostTitleMaximumBytes];
    ForumTagSnapshot tags[kForumPostAvailableTagMaximum];
};
// The channel and initial message form one validated response. No view borrows
// the response buffer. The optional self member is only ever positive evidence of membership.
struct ForumPostSnapshot {
    ThreadCreationSnapshot thread;
    u64 owner_id, message_id, message_channel_id, message_author_id, message_timestamp_ms;
    u32 message_flags, content_bytes;
    u8 applied_tag_count, reserved[7];
    u64 applied_tag_ids[kForumPostAppliedTagMaximum];
    char content[kForumPostContentMaximumBytes];
};
struct ForumPostTextRange {
    u32 offset, bytes;
};
struct ForumPostAttachmentSnapshot {
    u64 id, size;
    ForumPostTextRange filename, title, description, content_type, url, proxy_url;
    u32 width, height, flags;
    bool has_width, has_height, ephemeral;
    u8 reserved;
};
// Separate storage, allocated on demand, so the plain snapshot stays small.
// All strings use offsets into this bank, so copying the snapshot keeps them valid.
struct ForumPostMediaSnapshot {
    ForumPostSnapshot post;
    ForumPostAttachmentSnapshot attachments[kAttachmentUploadMaximumCount];
    char attachment_text[kForumPostAttachmentTextBytes];
    u32 attachment_text_bytes;
    u8 attachment_count, reserved[3];
};
bool forum_post_request_valid(const ForumPostRequest&) noexcept;
// Encoded body size. A capacity failure also reports the required byte count.
ForumPostResult forum_post_body_bytes(const ForumPostRequest&) noexcept;
ForumPostResult forum_post_write_target(const ForumPostRequest&, Slice<u8>) noexcept;
ForumPostResult forum_post_write_body(const ForumPostRequest&, Slice<u8>) noexcept;
// Needs the complete document. Output is written only on success; structural scratch
// may change on failure. Response, scratch and output must not overlap.
ForumPostResult forum_parent_decode(Utf8View, const ForumPostRequest&, Slice<u32> structural_scratch,
                                    ForumParentSnapshot*) noexcept;
ForumPostResult forum_post_decode(Utf8View, const ForumPostRequest&, u64 self_user_id,
                                  Slice<u32> structural_scratch, ForumPostSnapshot*) noexcept;
// Scratch and output must be separate caller-provided banks, disjoint from input
// and structural scratch. Output changes only after the whole response validates.
ForumPostResult forum_post_media_decode(Utf8View, const ForumPostRequest&, u64 self_user_id,
                                        Slice<u32> structural_scratch,
                                        ForumPostMediaSnapshot* transaction_scratch,
                                        ForumPostMediaSnapshot* output) noexcept;
GatewayMessageAttachment forum_post_attachment_view(const ForumPostMediaSnapshot&, u32 index) noexcept;
// Checks the selection against the tag inventory, REQUIRE_TAG and moderated tags. The host
// passes a freshly resolved MANAGE_THREADS permission.
bool forum_post_selection_valid(const ForumParentSnapshot&, ConstSlice<u64> selected,
                                bool manage_threads) noexcept;
Utf8View forum_parent_name_view(const ForumParentSnapshot&) noexcept;
Utf8View forum_tag_name_view(const ForumTagSnapshot&) noexcept;
Utf8View forum_post_content_view(const ForumPostSnapshot&) noexcept;
static_assert(sizeof(ForumPostRequest) == 144);
static_assert(sizeof(ForumTagSnapshot) == 96);
static_assert(sizeof(ForumParentSnapshot) == 2352);
static_assert(sizeof(ForumPostSnapshot) == 8560);
static_assert(sizeof(ForumPostAttachmentSnapshot) == 80);
static_assert(sizeof(ForumPostMediaSnapshot) == 74904);
} // namespace maicord
