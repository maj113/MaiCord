#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/ui/text_style.hpp"

namespace maicord {

inline constexpr u32 kMessageMarkdownSemanticMaximum = 128;
inline constexpr u32 kMessageMarkdownTimestampMaximumBytes = 128;
// Coloured stretches of ```ansi blocks per parse; beyond it the remaining text
// keeps its SGR styles but not its colours (syntax_truncated).
inline constexpr u32 kMessageMarkdownAnsiRunMaximum = 1'024;

enum class MessageMarkdownEntityKind : u8 {
    user,
    channel,
    role,
};

enum class MessageMarkdownSemanticKind : u8 {
    none,
    link,
    message_link,
    spoiler,
    user_mention,
    channel_mention,
    role_mention,
    timestamp,
    custom_emoji,
    regional_flag,
    // </name sub:id>; target is the command id, source covers the tag.
    command_mention,
    // An icon cell inside a link chip: target is a MessageMarkdownChipIcon,
    // the cell is the measured space the renderer draws the icon over (an
    // em wide; the chevron half an em). reserved bit kMessageMarkdownChipHovered
    // marks a cell of the hovered chip. Never a hit target of its own.
    chip_icon,
    // The server icon cell of a link chip; target is the guild id. With
    // kMessageMarkdownChipInitials the server has no icon: the cell shows its
    // initials on a round plate instead.
    guild_icon,
};

inline constexpr u8 kMessageMarkdownChipHovered = 1u << 2;
// reserved bit of a link semantic: a CDN attachment URL shown as a chip with
// its file name (the source stays the full URL).
inline constexpr u8 kMessageMarkdownAttachmentChip = 1u << 3;
inline constexpr u8 kMessageMarkdownChipInitials = 1u << 4;

enum class MessageMarkdownChipIcon : u8 {
    text_channel,
    voice,
    stage,
    locked,
    thread,
    message,
    forum,
    media,
    announcement,
    attachment,
    chevron,
    // A quoted forward's leading icon (reply previews), in the text colour.
    forward,
    // A local command message's status: sending / thinking (text colour) and
    // failed or unanswered (critical colour).
    pending_dots,
    failed,
    count,
};

enum MessageMarkdownChannelFlag : u8 {
    message_markdown_channel_known = 1u << 0,
    // The account can view it (a DM always; a server channel with VIEW_CHANNEL).
    message_markdown_channel_viewable = 1u << 1,
    // Text-like, voice and stage channels; categories render as plain text.
    message_markdown_channel_mentionable = 1u << 2,
    message_markdown_channel_private = 1u << 3,
    // A thread of a forum or media channel.
    message_markdown_channel_forum_post = 1u << 4,
    // guild_name/guild_icon describe a server in the store.
    message_markdown_channel_guild_known = 1u << 5,
    // That server is the one of the message being rendered.
    message_markdown_channel_same_guild = 1u << 6,
    // The forum post's parent channel is in the store (parent_name/parent_icon).
    message_markdown_channel_parent_known = 1u << 7,
};

// What a channel reference (<#id> or a /channels/ URL) resolves to. The guild
// fields describe the channel's server, or for an unknown channel the URL's.
struct MessageMarkdownChannelTarget {
    Utf8View name;
    Utf8View guild_name;
    Utf8View parent_name;
    u64 guild_id;
    MessageMarkdownChipIcon icon;
    MessageMarkdownChipIcon parent_icon;
    u8 flags;
    // The server has a CDN icon (the chip draws it before the server name).
    bool guild_icon;
};

struct MessageLink {
    u64 guild_id;
    u64 channel_id;
    u64 message_id;
    u32 source_bytes;
    // A /threads/ URL: channel_id is the thread, this its parent channel.
    u64 parent_channel_id;
};

struct MessageMarkdownSemantic {
    u64 target;
    u32 byte_offset;
    u32 byte_count;
    u32 source_offset;
    u16 source_bytes;
    MessageMarkdownSemanticKind kind;
    u8 reserved;
};

struct TimestampFormatResult {
    u64 next_update_unix_seconds;
    u32 bytes;
    bool ok;
};

using MessageMarkdownResolveEntity = Utf8View (*)(void* context, MessageMarkdownEntityKind kind,
                                                  u64 id) noexcept;
using MessageMarkdownFormatTimestamp = TimestampFormatResult (*)(void* context, u64 unix_seconds,
                                                                        char style,
                                                                        Slice<char> output) noexcept;
using MessageMarkdownSpoilerRevealed = bool (*)(void* context, u32 source_offset) noexcept;
// Resolves a channel reference: guild_id is the URL's server (zero for @me and
// for <#id>). Returns false when nothing is known about the reference.
using MessageMarkdownResolveChannel = bool (*)(void* context, u64 guild_id, u64 channel_id,
                                               MessageMarkdownChannelTarget* target) noexcept;

struct MessageMarkdownOptions {
    void* context;
    MessageMarkdownResolveEntity resolve_entity;
    MessageMarkdownFormatTimestamp format_timestamp;
    MessageMarkdownSpoilerRevealed spoiler_revealed;
    u32 hovered_source_offset;
    MessageMarkdownSemanticKind hovered_kind;
    bool has_hover;
    // Editors resolve entities separately so their source/caret mapping stays authoritative.
    bool preserve_entities;
    bool profile_bio;
    u32 base_style;
    // Embed rule sets: titles and field names drop links and block
    // rules (headings, subtext, lists); field values drop headings only.
    bool no_links;
    bool no_headings;
    bool no_lists;
    // Rem sizes: headings 1.5/1.25/1rem, "-#" subtext .875rem, and
    // base_size (a TextSize) for the remaining text, which then gets size-only
    // spans so the whole display is covered.
    bool text_sizes;
    u8 base_size;
    // Inline reply preview rules: block
    // quotes stay literal, ```fences become inline code without the language
    // tag or edge newlines (```ansi drops its SGR escapes instead of colouring),
    // headings are inline bold and list markers inline text.
    bool inline_reply;
    // Set: channel references and /channels/ URLs become link chips with
    // channel, server and message icons. Unset (plain-text surfaces such as
    // notifications): a message URL shows "#name › 💬" from resolve_entity.
    MessageMarkdownResolveChannel resolve_channel;
};

// The TextSize the markdown gives a run with these style flags.
constexpr u8 message_markdown_text_size(u32 flags, u8 base_size = text_size_inherit) noexcept {
    return flags & text_style_heading_1   ? u8{text_size_xl}
           : flags & text_style_heading_2 ? u8{text_size_lg}
           : flags & text_style_heading_3 ? u8{text_size_md}
           : flags & text_style_subtext   ? u8{text_size_sm}
                                          : base_size;
}

enum class MessageMarkdownError : u8 {
    none,
    invalid_input,
    invalid_utf8,
    output_too_small,
    delimiter_capacity,
    span_capacity,
};

struct MessageMarkdownResult {
    Utf8View text;
    u32 span_count;
    u32 transformations;
    u32 syntax_tokens;
    u32 syntax_styled_bytes;
    u32 resolved_entities;
    u32 unresolved_entities;
    u32 timestamps;
    u32 semantic_count;
    u64 next_timestamp_update_unix_seconds;
    u16 list_items;
    u8 code_blocks;
    bool syntax_truncated;
    bool semantics_truncated;
    MessageMarkdownError error;

    constexpr bool ok() const noexcept {
        return error == MessageMarkdownError::none;
    }
};

// Produces display UTF-8 with recognized control delimiters removed and a
// sorted, nonoverlapping style partition over the transformed bytes. The base
// transform does not expand; resolved semantic labels may use additional
// caller capacity and degrade to their bounded fallback if it is insufficient.
// All scratch/output belongs to the caller.
MessageMarkdownResult message_markdown_parse(Utf8View input, Slice<char> text_output,
                                             Slice<u32> byte_style_scratch,
                                             Slice<TextStyleSpan> span_output) noexcept;
MessageMarkdownResult message_markdown_parse(Utf8View input, Slice<char> text_output,
                                             Slice<u32> byte_style_scratch,
                                             Slice<TextStyleSpan> span_output,
                                             const MessageMarkdownOptions& options,
                                             Slice<MessageMarkdownSemantic> semantic_output) noexcept;
// Uses the same parser/highlighter but retains editable source delimiters;
// bounded recognition replay emits source spans without source-sized scratch.
u32 message_markdown_source_styles(Utf8View input, Slice<TextStyleSpan> output) noexcept;

bool message_markdown_url_valid(Utf8View value) noexcept;
// A discord.com/discordapp.com (canary./ptb.) /channels/<guild|@me>/<channel>/<message>
// URL, optionally with a ?query, or /channels/<guild>/<parent>/threads/<thread>/<message>
// (channel_id is then the thread); the whole value must be the URL.
bool message_markdown_message_link(Utf8View value, MessageLink* link) noexcept;
// A CDN attachment URL the chat shows as an attachment chip; the whole value
// must match. name receives the chip's file name (a view into value).
bool message_markdown_attachment_link(Utf8View value, Utf8View* name) noexcept;

inline constexpr u32 kInviteCodeMaximumBytes = 32;
inline constexpr u32 kInviteLinksPerMessage = 10;
struct InviteLink {
    u32 source_offset, source_bytes;
    u8 code_bytes;
    char code[kInviteCodeMaximumBytes];
    // discord.com/events/<guild>/<event>[/<recurrence>] is an event card:
    // both IDs are set and code holds the event ID digits (for de-duplication).
    u64 event_guild_id, event_id;
};
constexpr bool invite_link_event(const InviteLink& link) noexcept {
    return link.event_id != 0;
}
// discord.gg/<code>, discord.com/invite/<code> and discordapp.com/invite/<code>
// (www., canary. and ptb. hosts too) with an http(s) scheme, and event links
// on the full hosts. Query and fragment are ignored. The whole value must be
// the URL.
bool message_markdown_invite_link(Utf8View value, InviteLink* link) noexcept;
// Distinct invite codes in message content, in order, at most output.count
// (and never more than kInviteLinksPerMessage). URLs with a backslash are skipped.
u32 message_markdown_find_invites(Utf8View content, Slice<InviteLink> output) noexcept;

static_assert(sizeof(MessageMarkdownSemantic) == 24);

} // namespace maicord
