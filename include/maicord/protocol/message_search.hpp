#pragma once

#include "maicord/protocol/gateway_message_events.hpp"

namespace maicord {

inline constexpr u32 kMessageSearchContentBytes = 256;
inline constexpr u32 kMessageSearchFilterIds = 4;
inline constexpr u32 kMessageSearchMaximumGroups = 25;
inline constexpr u32 kMessageSearchMaximumContext = 5;
inline constexpr u32 kMessageSearchMaximumMessages = 125;
inline constexpr u32 kMessageSearchMaximumOffset = 5000;
inline constexpr u32 kMessageSearchMaximumTargetBytes = 1344;
inline constexpr u32 kMessageSearchMaximumBodyBytes = 16 * 1024;
inline constexpr u32 kMessageSearchHostnameBytes = 64;
inline constexpr u32 kMessageSearchExtensionBytes = 16;
inline constexpr u32 kMessageSearchFilenameBytes = 64;
inline constexpr u32 kMessageSearchTabCount = 5;

// private_channels searches every DM through /users/@me; only the tabs endpoint serves it.
enum class MessageSearchScope : u8 {
    guild,
    channel,
    private_channels
};
// Tab presets: media has image/video, files has file, links has link, pins is
// pinned. A preset yields to the user's own has/pinned filter.
enum class MessageSearchTab : u8 {
    messages,
    media,
    files,
    links,
    pins
};
enum class MessageSearchEndpoint : u8 {
    legacy, // GET .../messages/search
    tabs    // POST .../messages/search/tabs
};
enum MessageSearchAuthorType : u8 {
    search_author_user = 1u << 0,
    search_author_bot = 1u << 1,
    search_author_webhook = 1u << 2,
};
enum class MessageSearchSort : u8 {
    newest,
    oldest,
    relevance
};
enum class MessageSearchPinned : u8 {
    any,
    pinned,
    unpinned
};
enum MessageSearchHas : u16 {
    search_has_link = 1u << 0,
    search_has_embed = 1u << 1,
    search_has_file = 1u << 2,
    search_has_video = 1u << 3,
    search_has_image = 1u << 4,
    search_has_sound = 1u << 5,
    search_has_sticker = 1u << 6,
    search_has_poll = 1u << 7,
    search_has_snapshot = 1u << 8,
};

// The request owns its query text, so nothing borrowed crosses an async boundary.
// The limits are MaiCord's own, not the server's limits.
struct MessageSearchQuery {
    u64 scope_id;
    u64 min_id;
    u64 max_id;
    u64 author_ids[kMessageSearchFilterIds];
    u64 mention_ids[kMessageSearchFilterIds];
    u64 channel_ids[kMessageSearchFilterIds];
    char content[kMessageSearchContentBytes];
    u32 offset;
    u16 content_bytes;
    u16 has;
    u8 author_count;
    u8 mention_count;
    u8 channel_count;
    u8 limit;
    MessageSearchScope scope;
    MessageSearchSort sort;
    MessageSearchPinned pinned;
    bool include_nsfw;
    // Excluded has values are spelled "-link"; a value is never both included and excluded.
    u16 has_excluded;
    u8 author_types, author_types_excluded;
    u8 hostname_bytes, extension_bytes, filename_bytes;
    char link_hostname[kMessageSearchHostnameBytes];
    char attachment_extension[kMessageSearchExtensionBytes];
    char attachment_filename[kMessageSearchFilenameBytes];
    MessageSearchTab tab;
    MessageSearchEndpoint endpoint;
    // First tabs page also asks the other tabs for one result, so their totals are known.
    bool tab_counts;
};

enum class MessageSearchWriteError : u8 {
    none,
    invalid_input,
    output_capacity
};
struct MessageSearchWriteResult {
    u32 bytes;
    MessageSearchWriteError error;
    constexpr bool ok() const noexcept {
        return error == MessageSearchWriteError::none;
    }
};

struct MessageSearchMessage {
    GatewayMessageEvent message;
    u64 guild_id;
    bool hit;
    bool guild_known;
    // The overload without a sink keeps a short plain-text excerpt of the forwarded message.
    // The rich sink receives the complete outer and child media.
    Utf8View card_excerpt{};
    u16 snapshot_attachments{}, snapshot_embeds{}, snapshot_stickers{}, snapshot_components{};
    bool forwarded{}, excerpt_truncated{};
};
struct MessageSearchGroup {
    u32 first_message;
    u16 message_count;
    u16 hit_count;
};
struct MessageSearchPage {
    u64 total_results;
    u64 documents_indexed;
    u32 group_count;
    u32 message_count;
    u32 text_bytes;
    bool historical_indexing;
    u8 tab_known; // bit per MessageSearchTab with a decoded total
    u64 tab_totals[kMessageSearchTabCount];
};
enum class MessageSearchDecodeError : u8 {
    none,
    invalid_input,
    scan_failed,
    invalid_json,
    duplicate_field,
    missing_field,
    invalid_field,
    group_capacity,
    message_capacity,
    message_decode,
    invalid_group,
    channel_mismatch,
    guild_mismatch,
    sink_rejected,
};
struct MessageSearchDecodeResult {
    MessageSearchPage page;
    u32 error_offset;
    MessageSearchDecodeError error;
    GatewayEventDecodeError message_error;
    JsonScanError scan_error;
    JsonReadError read_error;
    constexpr bool ok() const noexcept {
        return error == MessageSearchDecodeError::none;
    }
};

// Validates a query independently of either endpoint's encoding capacity.
bool message_search_query_valid(const MessageSearchQuery& query) noexcept;
MessageSearchWriteResult write_message_search_target(const MessageSearchQuery& query,
                                                     Slice<u8> output) noexcept;
struct MessageSearchTabsWriteResult {
    u32 target_bytes, body_bytes;
    MessageSearchWriteError error;
    constexpr bool ok() const noexcept {
        return error == MessageSearchWriteError::none;
    }
};
// POST target and JSON body for the tabs endpoint. Filters go into the active
// tab; in: channels go into the root channel_ids.
MessageSearchTabsWriteResult write_message_search_tabs_request(const MessageSearchQuery& query,
                                                               Slice<u8> target, Slice<u8> body) noexcept;

// Both decoders accept the legacy root (messages/total_results) or the tabs root
// ({"tabs":{"<tab>":{messages,total_results}}}); the query's tab must be present.
// On failure ignore all output slices. On success every text view points into
// text_output, never the HTTP body. Search results are not conversation events
// and never reach notification handling. Guild IDs present in the response are
// checked against the query; when guild_known is false the host must verify
// channel membership against server-confirmed guild state, since a missing guild
// ID does not show the channel belongs to the guild.
MessageSearchDecodeResult decode_message_search_page(Utf8View body, const MessageSearchQuery& query,
                                                     Slice<u32> page_structurals, u32 maximum_depth,
                                                     Slice<char> text_output,
                                                     Slice<MessageSearchGroup> groups,
                                                     Slice<MessageSearchMessage> messages) noexcept;

// Scratch reused for each message of a result, rather than one rich descriptor set
// per message. Every pool must hold the gateway's per-message maximum.
// Snapshot pools are independent of the outer message's capacities.
// rich.poll is a reusable bank for ten answers plus counts; polls need its capacity.
// The sink must copy poll data before the scratch is reused and commit it only after the page succeeds.
struct MessageSearchRichScratch {
    Slice<u64> mention_roles;
    Slice<u64> member_roles;
    Slice<GatewayMessageNamedUser> mention_users;
    GatewayMessageRichScratch rich;
};
struct MessageSearchRichSink {
    void* context;
    bool (*accept)(void* context, u32 index, const MessageSearchMessage& message) noexcept;
};

// accept must copy every string/descriptor it needs before returning: message text
// and rich scratch are reused for the next result. Discard anything collected
// through the sink if the page fails for any reason (a later group without a hit,
// a sink rejection); a sink call does not mean the page was accepted.
// On success messages keep routing/scalar summaries only: every borrowed
// text/descriptor view is cleared. page.text_bytes counts all decoded text, not
// the occupancy of per_message_text.
MessageSearchDecodeResult decode_message_search_page(
    Utf8View body, const MessageSearchQuery& query, Slice<u32> page_structurals, u32 maximum_depth,
    Slice<char> per_message_text, Slice<MessageSearchGroup> groups, Slice<MessageSearchMessage> messages,
    const MessageSearchRichScratch& rich_scratch, MessageSearchRichSink sink) noexcept;

} // namespace maicord
