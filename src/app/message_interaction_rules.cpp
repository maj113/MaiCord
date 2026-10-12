#include "maicord/app/message_interaction_rules.hpp"

#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {

bool space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

Utf8View trim(Utf8View text) noexcept {
    u32 first = 0, last = text.bytes;
    while (first < last && space(text.data[first]))
        ++first;
    while (last > first && space(text.data[last - 1]))
        --last;
    return {text.data + first, last - first};
}

struct Writer {
    Slice<char> output;
    u32 used;
    bool ok;

    void append(Utf8View value) noexcept {
        if (!ok || value.bytes > output.count - used) {
            ok = false;
            return;
        }
        copy_bytes(output.data + used, value.data, value.bytes);
        used += value.bytes;
    }
};

// Optional argument after the command word; nullptr data means no match.
bool command_argument(Utf8View text, Utf8View command, Utf8View* argument) noexcept {
    if (!utf8_starts_with(text, command) || (text.bytes > command.bytes && !space(text.data[command.bytes])))
        return false;
    *argument = trim({text.data + command.bytes, text.bytes - command.bytes});
    return true;
}

float smooth(float t) noexcept {
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return t * t * (3 - 2 * t);
}

void append_two_digits(Writer* writer, u64 value) noexcept {
    const char digits[2] = {static_cast<char>('0' + value / 10 % 10), static_cast<char>('0' + value % 10)};
    writer->append({digits, 2});
}

void append_number(Writer* writer, u64 value) noexcept {
    char digits[20];
    writer->append({digits, decimal_write(digits, value)});
}

} // namespace

ComposerTextCommand composer_text_command_parse(Utf8View raw, Slice<char> output) noexcept {
    ComposerTextCommand command{};
    if (!raw.data || !raw.bytes)
        return command;
    const Utf8View text = trim(raw);
    struct Rewrite {
        Utf8View name, suffix;
    };
    // Suffixes follow the official client's built-in text commands.
    static constexpr Rewrite rewrites[] = {
        {utf8_literal("/shrug"), utf8_literal("\xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF")},
        {utf8_literal("/tableflip"),
         utf8_literal("(\xE2\x95\xAF\xC2\xB0\xE2\x96\xA1\xC2\xB0)\xE2\x95\xAF\xEF\xB8\xB5 \xE2\x94\xBB\xE2\x94\x81\xE2\x94\xBB")},
        {utf8_literal("/unflip"),
         utf8_literal("\xE2\x94\xAC\xE2\x94\x80\xE2\x94\xAC\xE3\x83\x8E( \xC2\xBA _ \xC2\xBA\xE3\x83\x8E)")},
    };
    Utf8View argument{};
    for (const auto& rewrite : rewrites) {
        if (!command_argument(text, rewrite.name, &argument))
            continue;
        Writer writer{output, 0, output.data != nullptr};
        if (argument.bytes) {
            writer.append(argument);
            writer.append(utf8_literal(" "));
        }
        writer.append(rewrite.suffix);
        if (writer.ok)
            command = {ComposerTextCommandKind::rewrite, writer.used};
        return command;
    }
    if (command_argument(text, utf8_literal("/me"), &argument)) {
        Writer writer{output, 0, output.data != nullptr};
        writer.append(utf8_literal("_"));
        writer.append(argument);
        writer.append(utf8_literal("_"));
        if (argument.bytes && writer.ok)
            command = {ComposerTextCommandKind::rewrite, writer.used};
        return command;
    }
    if (text.bytes >= 4 && text.data[0] == '+' && text.data[1] == ':' && text.data[text.bytes - 1] == ':') {
        const Utf8View name{text.data + 2, text.bytes - 3};
        if (name.bytes == 0 || name.bytes > 64)
            return command;
        for (u32 i = 0; i < name.bytes; ++i)
            if (name.data[i] == ':' || space(name.data[i]))
                return command;
        command.kind = ComposerTextCommandKind::react;
        command.emoji = name;
        return command;
    }
    if (text.bytes >= 4 && text.data[0] == 's' && text.data[1] == '/') {
        u32 cursor = 2;
        const u32 pattern_begin = cursor;
        while (cursor < text.bytes && text.data[cursor] != '/')
            ++cursor;
        if (cursor == pattern_begin || cursor == text.bytes)
            return command;
        const Utf8View pattern{text.data + pattern_begin, cursor - pattern_begin};
        const u32 replacement_begin = ++cursor;
        while (cursor < text.bytes && text.data[cursor] != '/')
            ++cursor;
        const Utf8View replacement{text.data + replacement_begin, cursor - replacement_begin};
        bool global = false;
        if (cursor < text.bytes) {
            const Utf8View tail{text.data + cursor, text.bytes - cursor};
            if (utf8_equal(tail, utf8_literal("/g")))
                global = true;
            else if (!utf8_equal(tail, utf8_literal("/")))
                return command;
        }
        command.kind = ComposerTextCommandKind::substitute;
        command.pattern = pattern;
        command.replacement = replacement;
        command.global = global;
    }
    return command;
}

namespace {
struct Emoticon {
    Utf8View text, emoji;
};
// The official client's common emoticon shortcuts. ":/" is excluded so URLs never match.
constexpr Emoticon kEmoticons[]{
    {utf8_literal(">:("), utf8_literal("😠")},  {utf8_literal(">:-("), utf8_literal("😠")},
    {utf8_literal(":')"), utf8_literal("🥲")},  {utf8_literal(":'("), utf8_literal("😢")},
    {utf8_literal(":)"), utf8_literal("🙂")},   {utf8_literal(":-)"), utf8_literal("🙂")},
    {utf8_literal("=)"), utf8_literal("🙂")},   {utf8_literal(":("), utf8_literal("🙁")},
    {utf8_literal(":-("), utf8_literal("🙁")},  {utf8_literal("=("), utf8_literal("🙁")},
    {utf8_literal(":D"), utf8_literal("😄")},   {utf8_literal(":-D"), utf8_literal("😄")},
    {utf8_literal("=D"), utf8_literal("😄")},   {utf8_literal(";)"), utf8_literal("😉")},
    {utf8_literal(";-)"), utf8_literal("😉")},  {utf8_literal(":P"), utf8_literal("😛")},
    {utf8_literal(":-P"), utf8_literal("😛")},  {utf8_literal("=P"), utf8_literal("😛")},
    {utf8_literal(":o"), utf8_literal("😮")},   {utf8_literal(":-o"), utf8_literal("😮")},
    {utf8_literal(":O"), utf8_literal("😮")},   {utf8_literal(":-O"), utf8_literal("😮")},
    {utf8_literal(":|"), utf8_literal("😐")},   {utf8_literal(":-|"), utf8_literal("😐")},
    {utf8_literal(":*"), utf8_literal("😗")},   {utf8_literal(":-*"), utf8_literal("😗")},
    {utf8_literal(":-/"), utf8_literal("😕")},  {utf8_literal(":@"), utf8_literal("😡")},
    {utf8_literal(";("), utf8_literal("😭")},   {utf8_literal("8-)"), utf8_literal("😎")},
    {utf8_literal("B-)"), utf8_literal("😎")},  {utf8_literal("</3"), utf8_literal("💔")},
    {utf8_literal("<3"), utf8_literal("❤️")},
};
} // namespace

bool composer_emoticons_convert(Utf8View text, Slice<char> output, u32* bytes) noexcept {
    if (!bytes || (!text.data && text.bytes) || (!output.data && output.count))
        return false;
    Writer writer{output, 0, true};
    bool converted = false, code_block = false, code_span = false;
    for (u32 at = 0; at < text.bytes;) {
        if (text.data[at] == '`') {
            const bool fence = text.bytes - at >= 3 && text.data[at + 1] == '`' && text.data[at + 2] == '`';
            if (fence)
                code_block = !code_block;
            else if (!code_block)
                code_span = !code_span;
            const u32 run = fence ? 3 : 1;
            writer.append({text.data + at, run});
            at += run;
            continue;
        }
        // Whole words only: start of text or after whitespace, then end or whitespace.
        const Emoticon* match = nullptr;
        if (!code_block && !code_span && (at == 0 || space(text.data[at - 1])))
            for (const auto& emoticon : kEmoticons) {
                const u32 end = at + emoticon.text.bytes;
                if (end <= text.bytes && (end == text.bytes || space(text.data[end])) &&
                    bytes_equal(text.data + at, emoticon.text.data, emoticon.text.bytes) &&
                    (!match || emoticon.text.bytes > match->text.bytes))
                    match = &emoticon;
            }
        if (match) {
            writer.append(match->emoji);
            at += match->text.bytes;
            converted = true;
        } else {
            writer.append({text.data + at, 1});
            ++at;
        }
    }
    *bytes = writer.used;
    return converted && writer.ok;
}

bool composer_text_substitute(Utf8View content, Utf8View pattern, Utf8View replacement, bool global,
                             Slice<char> output, u32* bytes) noexcept {
    if (!bytes || !pattern.bytes || !output.data)
        return false;
    Writer writer{output, 0, true};
    bool matched = false;
    u32 cursor = 0;
    while (cursor < content.bytes) {
        const bool match = (!matched || global) && content.bytes - cursor >= pattern.bytes &&
                           bytes_equal(content.data + cursor, pattern.data, pattern.bytes);
        if (match) {
            writer.append(replacement);
            cursor += pattern.bytes;
            matched = true;
        } else {
            writer.append({content.data + cursor, 1});
            ++cursor;
        }
    }
    if (!matched || !writer.ok || utf8_equal({output.data, writer.used}, content))
        return false;
    *bytes = writer.used;
    return true;
}

u32 message_delete_preview_format(Utf8View author, Utf8View content, bool has_attachments,
                                  Slice<char> output) noexcept {
    Writer writer{output, 0, output.data != nullptr};
    if (author.bytes) {
        writer.append(author);
        writer.append(utf8_literal(": "));
    }
    content = trim(content);
    if (!content.bytes) {
        writer.append(has_attachments ? utf8_literal("Attachment") : utf8_literal("Empty message"));
        return writer.ok ? writer.used : 0;
    }
    u32 cursor = 0;
    while (writer.ok && cursor < content.bytes) {
        if (cursor >= kMessageDeletePreviewContentBytes) {
            writer.append(utf8_literal("\xE2\x80\xA6"));
            break;
        }
        const Utf8DecodeResult scalar = utf8_decode_one(content.data + cursor, content.bytes - cursor);
        if (!scalar.valid || !scalar.width)
            return 0;
        // A single line keeps the dialog compact.
        writer.append(content.data[cursor] == '\n' || content.data[cursor] == '\r'
                          ? utf8_literal(" ")
                          : Utf8View{content.data + cursor, scalar.width});
        cursor += scalar.width;
    }
    return writer.ok ? writer.used : 0;
}

float message_jump_highlight_level(u64 elapsed_ms) noexcept {
    if (elapsed_ms < kMessageJumpHighlightFadeMs)
        return smooth(static_cast<float>(elapsed_ms) / static_cast<float>(kMessageJumpHighlightFadeMs));
    if (elapsed_ms < kMessageJumpHighlightMs)
        return 1;
    const u64 out = elapsed_ms - kMessageJumpHighlightMs;
    return out < kMessageJumpHighlightFadeMs
               ? smooth(1 - static_cast<float>(out) / static_cast<float>(kMessageJumpHighlightFadeMs))
               : 0;
}

u64 message_jump_highlight_next_wake(u64 started_ms, u64 now_ms) noexcept {
    if (!started_ms || now_ms < started_ms)
        return started_ms;
    const u64 elapsed = now_ms - started_ms;
    const u64 end = started_ms + kMessageJumpHighlightMs + kMessageJumpHighlightFadeMs;
    if (elapsed < kMessageJumpHighlightFadeMs)
        return minimum(now_ms + kMessageJumpHighlightFrameMs, started_ms + kMessageJumpHighlightFadeMs);
    if (elapsed < kMessageJumpHighlightMs)
        return started_ms + kMessageJumpHighlightMs;
    return now_ms < end ? minimum(now_ms + kMessageJumpHighlightFrameMs, end) : 0;
}

bool connection_notice_observe(ConnectionNotice* notice, ConnectionNoticeState state, u64 now_ms) noexcept {
    if (!notice)
        return false;
    if (notice->raw != state) {
        notice->raw = state;
        notice->since_ms = now_ms;
    }
    return connection_notice_tick(notice, now_ms);
}

bool connection_notice_tick(ConnectionNotice* notice, u64 now_ms) noexcept {
    if (!notice)
        return false;
    ConnectionNoticeState next = notice->shown;
    if (notice->raw == ConnectionNoticeState::connected)
        next = ConnectionNoticeState::connected;
    else if (notice->shown != ConnectionNoticeState::connected ||
             now_ms - notice->since_ms >= kConnectionNoticeDelayMs)
        next = notice->raw;
    if (next == notice->shown)
        return false;
    notice->shown = next;
    return true;
}

u64 connection_notice_next_wake(const ConnectionNotice& notice) noexcept {
    return notice.raw != ConnectionNoticeState::connected && notice.shown == ConnectionNoticeState::connected
               ? notice.since_ms + kConnectionNoticeDelayMs
               : 0;
}

Utf8View connection_notice_text(ConnectionNoticeState state) noexcept {
    switch (state) {
    case ConnectionNoticeState::reconnecting:
        return utf8_literal("Reconnecting\xE2\x80\xA6");
    case ConnectionNoticeState::offline:
        return utf8_literal("You\xE2\x80\x99re offline. Check your connection.");
    case ConnectionNoticeState::connected:
        break;
    }
    return {};
}

u32 slowmode_label_format(u64 remaining_ms, Slice<char> output) noexcept {
    if (!remaining_ms)
        return 0;
    const u64 seconds = (remaining_ms + 999) / 1000;
    Writer writer{output, 0, output.data != nullptr};
    writer.append(utf8_literal("Slowmode: "));
    if (seconds >= 3600) {
        append_number(&writer, seconds / 3600);
        writer.append(utf8_literal(":"));
        append_two_digits(&writer, seconds / 60 % 60);
    } else {
        append_number(&writer, seconds / 60);
    }
    writer.append(utf8_literal(":"));
    append_two_digits(&writer, seconds % 60);
    return writer.ok ? writer.used : 0;
}

} // namespace maicord
