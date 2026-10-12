#pragma once

#include "maicord/model/chat_state.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/ui/message_markdown.hpp"
#include "maicord/ui/navigation_view.hpp"
#include "maicord/ui/message_scene.hpp"
#include "maicord/ui/text_flow_layout.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/theme.hpp"

namespace maicord {

inline constexpr u32 kMaximumMessageTextCommands = 1'024;
inline constexpr u32 kMaximumMessageTextUtf8Bytes = 32 * 1'024;
inline constexpr u32 kMaximumMessageTextStyleSpans = 1'024;
inline constexpr u32 kMaximumMessageTextSemantics = 256;
inline constexpr u32 kMaximumMessageTextMeasurements = 192;
inline constexpr u32 kMaximumMessageTextMeasureCacheRecords = 256;
inline constexpr u32 kMaximumMessageTextSpoilerReveals = 64;
inline constexpr u32 kMessageTextByteStyleScratchCapacity = 16 * 1'024;
inline constexpr u16 kInvalidMessageTextMeasurement = 0xFFFFu;

enum class MessageTextMeasurementKind : u8 {
    none,
    reply,
    content,
    attachment,
    embed,
    sticker,
    component,
    reaction,
    notice,
};

struct MessageTextCommand {
    Utf8View text;
    ConstSlice<TextStyleSpan> styles;
    ConstSlice<MessageMarkdownSemantic> semantics;
    UiWidgetReference owner;
    UiRect rectangle;
    UiRect clip;
    Color color;
    Color inline_role_color;
    u64 message_id;
    u64 measurement_identity;
    u32 message_revision;
    float minimum_height;
    float gap_after;
    float measured_height;
    float measured_width;
    u16 measurement_index;
    MessageTextMeasurementKind measurement_kind;
    u8 measurement_slot;
    bool themed_styles;
    UiTextAlignment horizontal_alignment;
    UiTextAlignment vertical_alignment;
    UiTextWrapping wrapping;
    UiTextOverflow overflow;
    // Logical selector of the owning field; stable across rich-store compaction
    // (never a storage offset).
    u16 source_id;
};

struct MessageTextMeasurement {
    u64 message_id;
    u32 message_revision;
    u32 source_index;
    float reply_height;
    float content_height;
    MessageSceneRichLayout rich_layout;
    MessageSceneRichItemLayout rich_item_layout;
    float reaction_widths[kGatewayMessageReactionMaximum];
    bool content_present;
    bool forwarded;
};

struct MessageTextSemantic {
    u64 message_id;
    u64 target;
    u32 message_revision;
    u32 source_offset;
    u16 source_bytes;
    MessageMarkdownSemanticKind kind;
    u8 reserved;
};

struct MessageTextHit {
    u64 message_id;
    u64 target;
    u32 message_revision;
    u32 source_offset;
    u16 source_bytes;
    MessageMarkdownSemanticKind kind;
    bool valid;
};

struct MessageTextCopyProjectionResult {
    u32 bytes;
    bool transformed;
    bool ok;
};

struct MessageTextSpoilerReveal {
    u64 message_id;
    u32 message_revision;
    u32 source_offset;
};

struct MessageTextRichMetrics {
    u32 replies;
    u32 reactions;
    u32 attachments;
    u32 embeds;
    u32 stickers;
    u32 components;
    u32 truncated_records;
    u32 flow_items;
    u32 flow_overflow_regions;
    u32 skipped_components;
};

enum class MessageTextPresentationError : u8 {
    none,
    invalid_input,
    model,
    layout,
    capacity,
    measurement,
};

enum class MessageTextPresentationStage : u8 {
    none,
    row,
    reply,
    author,
    content,
    attachment,
    embed,
    sticker,
    component,
    reaction,
    notice,
};

enum class MessageTextPresentationOperation : u8 {
    none,
    resolve_layout,
    initialize_flow,
    begin_flow_item,
    append_command,
    commit_flow_item,
    advance_flow,
    ensure_extent,
    finish_flow,
    build_reactions,
};

struct MessageTextPresentationResult {
    u32 command_count;
    u32 measurement_count;
    u32 transformed_bytes;
    MessageTextPresentationError error;

    constexpr bool ok() const noexcept {
        return error == MessageTextPresentationError::none;
    }
};

// Retained Markdown results so scroll and relayout skip reparsing. Keys cover
// the source bytes and every parse option; entries that depended on entities,
// spoiler reveals or timestamp formatting also record that state and expire with
// it. Payloads live in a ring: the oldest are overwritten first, and one still
// in use is copied forward before the ring reaches it.
inline constexpr u32 kMessageMarkdownCacheSlots = 512;
inline constexpr u32 kMessageMarkdownCacheArenaBytes = 64 * 1'024;

struct MessageMarkdownCacheEntry {
    u64 key;
    u64 position; // ring position + 1 of the payload; zero is empty
    u64 expires_unix_seconds;
    u64 entity_epoch;
    u64 spoiler_signature;
    u32 required_capacity;
    u32 text_bytes;
    u32 transformations;
    u32 syntax_tokens;
    u32 syntax_styled_bytes;
    u16 span_count;
    u16 list_items;
    u16 resolved_entities;
    u16 unresolved_entities;
    u16 timestamps;
    u8 semantic_count;
    u8 flags;
};

struct MessageMarkdownCache {
    MessageMarkdownCacheEntry* entries;
    u8* arena;
    u32 slot_count;
    u32 arena_bytes;
    u64 head;
    u32 hits;
    u32 misses;
    u32 inserts;
    u32 refreshes;
    u32 uncacheable;
};

// entries start zeroed; their count is a power of two and arena.count a
// multiple of eight.
bool message_markdown_cache_initialize(MessageMarkdownCache* cache, Slice<MessageMarkdownCacheEntry> entries,
                                       Slice<u8> arena) noexcept;
void message_markdown_cache_clear(MessageMarkdownCache* cache) noexcept;

struct MessageTextPresentation {
    const GuildMemberStore* guild_members;
    MessageMarkdownCache* markdown_cache;
    u64 markdown_entity_epoch;
    Slice<MessageTextCommand> commands;
    Slice<char> utf8;
    Slice<TextStyleSpan> styles;
    Slice<MessageMarkdownSemantic> semantics;
    Slice<MessageTextMeasurement> measurements;
    Slice<MessageTextSpoilerReveal> spoiler_reveals;
    Slice<u32> byte_style_scratch;
    u32 command_count;
    u32 utf8_count;
    u32 style_count;
    u32 semantic_count;
    u32 measurement_count;
    u32 measurement_history_count;
    u32 measurement_history_offset;
    u32 spoiler_reveal_count;
    u32 spoiler_reveal_cursor;
    u32 builds;
    u32 rejected;
    u32 markdown_parses; // parser runs; cache hits count in markdown_cache_hits
    u32 markdown_cache_hits;
    u32 markdown_transformations;
    u32 markdown_spans_emitted;
    u32 markdown_rejected;
    u32 markdown_syntax_tokens;
    u32 markdown_syntax_styled_bytes;
    u32 markdown_list_items;
    u32 markdown_syntax_truncated;
    u32 markdown_resolved_entities;
    u32 markdown_unresolved_entities;
    u32 markdown_timestamps;
    u32 markdown_semantics_truncated;
    MessageTextRichMetrics rich;
    u64 next_timestamp_update_unix_seconds;
    u64 projection_signature;
    u64 hovered_message_id;
    u32 hovered_source_offset;
    // Where the next measurement-history lookup starts (see append_command).
    u32 measurement_history_cursor;
    MessageMarkdownSemanticKind hovered_kind;
    bool command_measurements_changed;
    bool has_hover;
    bool active;
    bool reveal_spoilers; // Chat setting: spoiler text renders revealed
    MessageMarkdownError last_markdown_error;
    MessageTextPresentationError last_error;
    u64 failure_message_id;
    u32 failure_source_index;
    u16 failure_item_index;
    MessageTextPresentationStage failure_stage;
    MessageTextPresentationOperation failure_operation;
    UiTextFlowError failure_flow_error;
};

bool message_text_presentation_initialize(MessageTextPresentation* presentation,
                                          Slice<MessageTextCommand> commands, Slice<char> utf8,
                                          Slice<TextStyleSpan> styles,
                                          Slice<MessageMarkdownSemantic> semantics,
                                          Slice<MessageTextMeasurement> measurements,
                                          Slice<MessageTextSpoilerReveal> spoiler_reveals,
                                          Slice<u32> byte_style_scratch) noexcept;
bool message_text_presentation_bind_guild_members(
    MessageTextPresentation* presentation,
    const GuildMemberStore* members) noexcept;
// Optional; without a cache every build parses every source.
bool message_text_presentation_bind_markdown_cache(MessageTextPresentation* presentation,
                                                   MessageMarkdownCache* cache) noexcept;
u64 message_text_presentation_projection_signature(
    const MessageTextPresentation& presentation) noexcept;

MessageTextPresentationResult message_text_presentation_build(
    MessageTextPresentation* presentation, const MessageScene& scene, const ChatStateStore& state,
    const UserStore* users, const NavigationView& navigation, const Theme& theme,
    u64 now_unix_seconds) noexcept;

bool message_text_presentation_apply_measurements(MessageScene* scene,
                                                  const MessageTextPresentation& presentation,
                                                  bool* changed) noexcept;
bool message_text_presentation_measure(MessageTextPresentation* presentation, UiTextMeasureCache* cache,
                                       u16 backend_revision, bool* changed) noexcept;
bool message_text_presentation_set_hover(MessageTextPresentation* presentation, const MessageTextHit& hit,
                                         bool* changed) noexcept;
bool message_text_presentation_toggle_spoiler(MessageTextPresentation* presentation,
                                              const MessageStore& store, const MessageTextHit& hit,
                                              bool* changed) noexcept;
Utf8View message_text_presentation_hit_source(const MessageStore& store,
                                              const MessageTextHit& hit) noexcept;
Utf8View message_text_presentation_hit_source(const MessageStore& store,
                                              const MessageRichStore* rich_store,
                                              const MessageTextHit& hit) noexcept;
Utf8View message_text_presentation_command_source(const MessageStore& store,
                                                  const MessageRichStore* rich_store,
                                                  const MessageTextCommand& command) noexcept;
bool message_text_presentation_toggle_spoiler(MessageTextPresentation* presentation,
                                              const MessageStore& store,
                                              const MessageRichStore* rich_store,
                                              const MessageTextHit& hit,
                                              bool* changed) noexcept;
MessageTextCopyProjectionResult message_text_project_selection_for_copy(
    const MessageTextCommand& command, Utf8View source, u32 first, u32 last,
    Slice<char> output) noexcept;

static_assert(sizeof(MessageTextSemantic) == 32);
static_assert(sizeof(MessageTextHit) == 32);
static_assert(sizeof(MessageTextSpoilerReveal) == 16);
static_assert(sizeof(MessageTextCommand) == 176);
static_assert(sizeof(MessageTextMeasurement) == 816);
static_assert(sizeof(MessageTextRichMetrics) == 40);
static_assert(sizeof(MessageMarkdownCacheEntry) == 72);

} // namespace maicord
