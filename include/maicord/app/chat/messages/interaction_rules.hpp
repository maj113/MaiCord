#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

// Composer text commands, applied just before a send.
enum class ComposerTextCommandKind : u8 {
    none,
    rewrite,    // output holds the replacement content (/shrug, /tableflip, /unflip, /me)
    react,      // +:name: reacts to the latest message
    substitute, // s/old/new[/g] edits your latest message
};

struct ComposerTextCommand {
    ComposerTextCommandKind kind;
    u32 bytes;     // rewrite: bytes written to the output
    Utf8View emoji;
    Utf8View pattern;
    Utf8View replacement;
    bool global;
};

ComposerTextCommand composer_text_command_parse(Utf8View text, Slice<char> output) noexcept;
// Replaces the first (or every) occurrence. False when nothing matches or the
// result would not fit.
bool composer_text_substitute(Utf8View content, Utf8View pattern, Utf8View replacement, bool global,
                             Slice<char> output, u32* bytes) noexcept;

// Chat "convert emoticons": whole-word :) <3 ;) ... become emoji outside code.
// False when nothing converts or the result would not fit.
bool composer_emoticons_convert(Utf8View text, Slice<char> output, u32* bytes) noexcept;

// "Author: content", cut on a scalar boundary with an ellipsis.
inline constexpr u32 kMessageDeletePreviewContentBytes = 280;
u32 message_delete_preview_format(Utf8View author, Utf8View content, bool has_attachments,
                                  Slice<char> output) noexcept;

// Jump highlight: fade in, hold, fade out.
inline constexpr u64 kMessageJumpHighlightMs = 2000;
inline constexpr u64 kMessageJumpHighlightFadeMs = 200;
inline constexpr u64 kMessageJumpHighlightFrameMs = 16;
// 0..1; zero once finished.
float message_jump_highlight_level(u64 elapsed_ms) noexcept;
// Next time the level changes; zero when finished. Holds wake only at their end.
u64 message_jump_highlight_next_wake(u64 started_ms, u64 now_ms) noexcept;

// Gateway connection notice with a debounce so short blips never show.
enum class ConnectionNoticeState : u8 { connected, reconnecting, offline };
inline constexpr u64 kConnectionNoticeDelayMs = 1000;
struct ConnectionNotice {
    u64 since_ms;
    ConnectionNoticeState raw;
    ConnectionNoticeState shown;
};
// Both return true when the shown state changed.
bool connection_notice_observe(ConnectionNotice* notice, ConnectionNoticeState state, u64 now_ms) noexcept;
bool connection_notice_tick(ConnectionNotice* notice, u64 now_ms) noexcept;
u64 connection_notice_next_wake(const ConnectionNotice& notice) noexcept;
Utf8View connection_notice_text(ConnectionNoticeState state) noexcept;

// "Slowmode: m:ss" for the remaining cooldown, rounded up to whole seconds.
inline constexpr u32 kSlowmodeLabelBytes = 32;
u32 slowmode_label_format(u64 remaining_ms, Slice<char> output) noexcept;

} // namespace maicord
