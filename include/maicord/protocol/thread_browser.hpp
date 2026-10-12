#pragma once

#include "maicord/core/json_reader.hpp"

namespace maicord {

inline constexpr u32 kThreadBrowserArchivedLimit = 25;
inline constexpr u32 kThreadBrowserSearchMaximumRows = 100;
inline constexpr u32 kThreadBrowserNameMaximumBytes = 400;
inline constexpr u32 kThreadBrowserNameMaximumScalars = 100;
inline constexpr u32 kThreadBrowserMaximumTargetBytes = 1344;
inline constexpr u32 kThreadBrowserMaximumBodyBytes = 512 * 1024;
inline constexpr u32 kThreadBrowserMaximumOffset = 1'000'000;
inline constexpr u64 kThreadBrowserRouteId = 0x5448524453454152ull;

// active: GET /channels/{id}/threads/search?archived=false (the archived
// listing's counterpart), used when the Gateway active inventory for the parent
// is not synced. Pages of 25; has_more may be omitted (no further page).
enum class ThreadBrowserMode : u8 { archived, search, active };
struct ThreadBrowserRequest {
    u64 parent_id, guild_id;
    Utf8View name;
    u32 offset;
    ThreadBrowserMode mode;
    u8 reserved[3];
};
enum class ThreadBrowserError : u8 { none, input, capacity, json, duplicate, identity, field };
struct ThreadBrowserResult {
    u32 bytes{};
    ThreadBrowserError error{};
    constexpr bool ok() const noexcept { return error == ThreadBrowserError::none; }
};
enum ThreadBrowserField : u8 {
    thread_browser_last_message_known = 1,
    thread_browser_archived_known = 2,
    thread_browser_archived = 4,
    thread_browser_locked_known = 8,
    thread_browser_locked = 16,
    thread_browser_self_joined = 32,
};
struct ThreadBrowserRow {
    u64 id, parent_id, guild_id, last_message_id, self_join_timestamp_ms;
    u32 name_offset, name_bytes, self_flags;
    u8 type, fields, reserved[2];
};
// All storage belongs to the caller. No response strings or objects are borrowed.
// Absence of self_joined is UNKNOWN, never an authoritative membership removal.
struct ThreadBrowserPage {
    Slice<ThreadBrowserRow> rows;
    Slice<char> names;
    u32 row_count, name_bytes, next_offset;
    bool has_more;
    u8 reserved[3];
};
// Forum/media post metadata retained beside a page (one post per row, same
// order). Only decoded when the caller supplies post storage; otherwise the
// extra thread fields and first_messages previews are ignored.
inline constexpr u32 kThreadBrowserPostTagMaximum = 5;
inline constexpr u32 kThreadBrowserPostPreviewMaximumBytes = 600;
inline constexpr u32 kThreadBrowserPostContentMaximumBytes = 16'384;
inline constexpr u32 kThreadBrowserPostAuthorNameMaximumBytes = 128;
inline constexpr u32 kThreadBrowserPostAvatarHashMaximumBytes = 64;
inline constexpr u32 kThreadBrowserPostMediaUrlMaximumBytes = 2048;
inline constexpr u32 kThreadBrowserPostTextMaximumBytes = kThreadBrowserPostPreviewMaximumBytes +
    kThreadBrowserPostAuthorNameMaximumBytes + kThreadBrowserPostAvatarHashMaximumBytes +
    kThreadBrowserPostMediaUrlMaximumBytes;
// Channel flag PINNED (forum posts pinned to the top of the list).
inline constexpr u32 kThreadBrowserPinnedFlag = 1u << 1;
enum class ThreadBrowserMediaType : u8 { none, png, jpeg, webp, gif };
enum ThreadBrowserPostField : u16 {
    thread_browser_post_tags = 1,
    thread_browser_post_message_count = 2,
    thread_browser_post_total_sent = 4,
    thread_browser_post_owner = 8,
    thread_browser_post_flags = 16,
    thread_browser_post_created = 32,
    thread_browser_post_starter = 64,
    thread_browser_post_author = 128,
    thread_browser_post_media = 256,
};
// Text is owned by the caller's bank: [preview][author name][avatar hash][media URL]
// starting at text_offset. The preview is the starter content with whitespace
// runs collapsed, cut at a scalar boundary (never mid-codepoint).
struct ThreadBrowserPost {
    u64 thread_id, owner_id, create_timestamp_ms, starter_message_id, author_id, media_id;
    u64 tags[kThreadBrowserPostTagMaximum];
    u32 message_count, total_message_sent, flags, text_offset;
    u16 preview_bytes, author_name_bytes, avatar_hash_bytes, media_url_bytes;
    u32 media_width, media_height;
    u16 fields;
    u8 tag_count;
    ThreadBrowserMediaType media_type;
    u8 reserved[4];
};
struct ThreadBrowserPostPage {
    Slice<ThreadBrowserPost> posts;
    Slice<char> text;
    u32 post_count, text_bytes;
};
bool thread_browser_request_valid(const ThreadBrowserRequest&) noexcept;
// GET only, with no request body. Writers preserve output on every failure.
ThreadBrowserResult thread_browser_write_target(
    const ThreadBrowserRequest&, Slice<u8>) noexcept;
// Candidate scratch may be changed on failure; the output descriptor is only
// published after the entire page validates. Use an unpublished spare bank.
// Scratch and output must be disjoint from the immutable input and each other.
// Archived pages require has_more and <=25 rows. Search has no pagination and
// rejects >100 rows, or any insufficient caller storage, without truncation.
ThreadBrowserResult thread_browser_decode(
    Utf8View, const ThreadBrowserRequest&, u64 self_user_id,
    Slice<u32> structural_scratch, Slice<ThreadBrowserRow> row_scratch,
    Slice<char> name_scratch, ThreadBrowserPage* output) noexcept;
// Same contract as thread_browser_decode, plus strict post metadata:
// applied_tags (<=5 unique snowflakes), message_count, total_message_sent,
// owner_id, flags, thread_metadata.create_timestamp and first_messages
// (one starter per page thread: content, author, first image attachment).
// Both descriptors are published together, only after everything validates.
ThreadBrowserResult thread_browser_decode_posts(
    Utf8View, const ThreadBrowserRequest&, u64 self_user_id,
    Slice<u32> structural_scratch, Slice<ThreadBrowserRow> row_scratch,
    Slice<char> name_scratch, Slice<ThreadBrowserPost> post_scratch, Slice<char> text_scratch,
    ThreadBrowserPage* output, ThreadBrowserPostPage* post_output) noexcept;
Utf8View thread_browser_row_name(const ThreadBrowserPage&, u32 index) noexcept;
Utf8View thread_browser_post_preview(const ThreadBrowserPostPage&, u32 index) noexcept;
Utf8View thread_browser_post_author_name(const ThreadBrowserPostPage&, u32 index) noexcept;
Utf8View thread_browser_post_avatar_hash(const ThreadBrowserPostPage&, u32 index) noexcept;
Utf8View thread_browser_post_media_url(const ThreadBrowserPostPage&, u32 index) noexcept;
static_assert(sizeof(ThreadBrowserRequest) == 40);
static_assert(sizeof(ThreadBrowserRow) == 56);
static_assert(sizeof(ThreadBrowserPage) == 48);
static_assert(sizeof(ThreadBrowserPost) == 128);

} // namespace maicord
