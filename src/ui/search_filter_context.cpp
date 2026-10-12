#include "maicord/ui/search_filter_context.hpp"

namespace maicord {
namespace {
bool boundary(Utf8View text, u32 at) noexcept {
    return at <= text.bytes && (!at || at == text.bytes ||
                               (static_cast<u8>(text.data[at]) & 0xc0u) != 0x80u);
}
char lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
}
bool key_equal(Utf8View key, Utf8View name) noexcept {
    if (key.bytes != name.bytes) return false;
    for (u32 i = 0; i < key.bytes; ++i)
        if (lower(key.data[i]) != lower(name.data[i])) return false;
    return true;
}
UiSearchFilterKind filter_kind(Utf8View key) noexcept {
    const Utf8View names[]{utf8_literal("from"),       utf8_literal("in"),       utf8_literal("mentions"),
                           utf8_literal("has"),        utf8_literal("pinned"),   utf8_literal("authorType"),
                           utf8_literal("fileType"),   utf8_literal("linkFrom"), utf8_literal("fileName")};
    for (u32 i = 0; i < 9; ++i)
        if (key_equal(key, names[i])) return static_cast<UiSearchFilterKind>(i + 1);
    return UiSearchFilterKind::none;
}
bool decode(Utf8View query, u32 caret, UiSearchFilterContext& out) noexcept {
    out.quoted = out.value_begin < out.value_end && query.data[out.value_begin] == '"';
    out.quote_complete = !out.quoted;
    u32 at = out.value_begin + (out.quoted ? 1u : 0u);
    while (at < out.value_end) {
        char c = query.data[at++];
        if (out.quoted && c == '"') {
            if (at != out.value_end) return false;
            out.quote_complete = true;
            break;
        }
        if (out.quoted && c == '\\' && at < out.value_end &&
            (query.data[at] == '"' || query.data[at] == '\\'))
            c = query.data[at++];
        out.decoded[out.value_bytes++] = c;
        // Do not include a character whose escape pair extends past the caret.
        if (at <= caret) out.prefix_bytes = out.value_bytes;
    }
    out.has_value = out.value_bytes != 0;
    return true;
}
constexpr UiSearchFilterChoiceView has_choices[]{
    {0, 0, utf8_literal("Link"), {}, utf8_literal("link")},
    {0, 1, utf8_literal("Embed"), {}, utf8_literal("embed")},
    {0, 2, utf8_literal("File"), {}, utf8_literal("file")},
    {0, 3, utf8_literal("Video"), {}, utf8_literal("video")},
    {0, 4, utf8_literal("Image"), {}, utf8_literal("image")},
    {0, 5, utf8_literal("Sound"), {}, utf8_literal("sound")},
    {0, 6, utf8_literal("Sticker"), {}, utf8_literal("sticker")},
    {0, 7, utf8_literal("Poll"), {}, utf8_literal("poll")},
    {0, 8, utf8_literal("Forward"), {}, utf8_literal("snapshot")},
};
constexpr UiSearchFilterChoiceView pinned_choices[]{
    {0, 0, utf8_literal("True"), {}, utf8_literal("true")},
    {0, 1, utf8_literal("False"), {}, utf8_literal("false")},
};
constexpr UiSearchFilterChoiceView author_type_choices[]{
    {0, 0, utf8_literal("User"), {}, utf8_literal("user")},
    {0, 1, utf8_literal("Bot"), {}, utf8_literal("bot")},
    {0, 2, utf8_literal("Webhook"), {}, utf8_literal("webhook")},
};
// Semantics pick the row icon: 0 image, 1 video, 2 audio, 3 document, 4 archive.
constexpr UiSearchFilterChoiceView file_type_choices[]{
    {1, 0, utf8_literal("png"), utf8_literal("Image"), utf8_literal("png")},
    {2, 0, utf8_literal("jpg"), utf8_literal("Image"), utf8_literal("jpg")},
    {3, 0, utf8_literal("gif"), utf8_literal("Image"), utf8_literal("gif")},
    {4, 0, utf8_literal("webp"), utf8_literal("Image"), utf8_literal("webp")},
    {5, 1, utf8_literal("mp4"), utf8_literal("Video"), utf8_literal("mp4")},
    {6, 1, utf8_literal("mov"), utf8_literal("Video"), utf8_literal("mov")},
    {7, 2, utf8_literal("mp3"), utf8_literal("Audio"), utf8_literal("mp3")},
    {8, 3, utf8_literal("pdf"), utf8_literal("Document"), utf8_literal("pdf")},
    {9, 3, utf8_literal("txt"), utf8_literal("Text"), utf8_literal("txt")},
    {10, 4, utf8_literal("zip"), utf8_literal("Archive"), utf8_literal("zip")},
};
} // namespace

UiSearchFilterContext ui_search_filter_context(Utf8View query, u32 anchor, u32 caret) noexcept {
    if (query.bytes > kUiSearchFilterQueryBytes || (!query.data && query.bytes) ||
        !utf8_validate(query).valid || !boundary(query, anchor) || !boundary(query, caret)) return {};
    for (u32 at = 0; at < query.bytes;) {
        const auto scalar = utf8_decode_one(query.data + at, query.bytes - at);
        if (scalar.codepoint < 32 || scalar.codepoint == 127 || scalar.codepoint == 0x2028 ||
            scalar.codepoint == 0x2029) return {};
        at += scalar.width;
    }
    UiSearchFilterContext result{};
    u16 occurrences[10]{}, populated[10]{};
    for (u32 at = 0; at < query.bytes;) {
        while (at < query.bytes && query.data[at] == ' ') ++at;
        if (at == query.bytes) break;
        const u32 first = at;
        bool quoted = false, escaped = false;
        while (at < query.bytes) {
            const char c = query.data[at];
            if (c == '"' && !escaped) quoted = !quoted;
            if (c == ' ' && !quoted) break;
            escaped = c == '\\' ? !escaped : false;
            ++at;
        }
        u32 colon = first;
        while (colon < at && query.data[colon] != ':') ++colon;
        if (colon == at) continue;
        const auto kind = filter_kind({query.data + first, colon - first});
        if (kind == UiSearchFilterKind::none) continue;
        const auto index = static_cast<u8>(kind);
        ++occurrences[index];
        const u32 value_begin = colon + 1;
        // An empty quoted value is not an already populated identity slot.
        const bool has_value = at > value_begin &&
            !(at == value_begin + 1 && query.data[value_begin] == '"') &&
            !(at == value_begin + 2 && query.data[value_begin] == '"' && query.data[value_begin + 1] == '"');
        if (has_value) ++populated[index];
        if (anchor < value_begin || anchor > at || caret < value_begin || caret > at) continue;
        UiSearchFilterContext candidate{};
        candidate.kind = kind;
        candidate.token_begin = first;
        candidate.token_end = at;
        candidate.value_begin = value_begin;
        candidate.value_end = at;
        if (!decode(query, caret, candidate)) return {};
        result = candidate;
    }
    if (result.kind == UiSearchFilterKind::none) return {};
    const auto index = static_cast<u8>(result.kind);
    result.same_kind_occurrences = occurrences[index];
    result.other_value_count = static_cast<u16>(populated[index] - (result.has_value ? 1u : 0u));
    return result;
}

ConstSlice<UiSearchFilterChoiceView> ui_search_filter_fixed_choices(UiSearchFilterKind kind) noexcept {
    if (kind == UiSearchFilterKind::has) return {has_choices, 9};
    if (kind == UiSearchFilterKind::pinned) return {pinned_choices, 2};
    if (kind == UiSearchFilterKind::author_type) return {author_type_choices, 3};
    if (kind == UiSearchFilterKind::file_type) return {file_type_choices, 10};
    return {};
}
} // namespace maicord
