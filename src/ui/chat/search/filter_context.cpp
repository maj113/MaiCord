#include "maicord/ui/chat/search/filter_context.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/protocol/messages/search.hpp"

#include "maicord/core/ascii.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
bool boundary(Utf8View text, u32 at) noexcept {
    return at <= text.bytes && (!at || at == text.bytes ||
                               (static_cast<u8>(text.data[at]) & 0xc0u) != 0x80u);
}
UiSearchFilterKind filter_kind(Utf8View key) noexcept {
    u32 index = 1;
    for (NameList list{"from\0in\0mentions\0has\0pinned\0authorType\0fileType\0linkFrom\0fileName\0"}; list;
         list.next(), ++index)
        if (ascii_equal_ignore_case(key, list.name()))
            return static_cast<UiSearchFilterKind>(index);
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
    u16 populated[10]{};
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
        copy_value(&result, candidate);
    }
    if (result.kind == UiSearchFilterKind::none) return {};
    const auto index = static_cast<u8>(result.kind);
    result.other_value_count = static_cast<u16>(populated[index] - (result.has_value ? 1u : 0u));
    return result;
}

// The fixed rows of the value filters, in order. Each kind lists its labels, its values (absent: the labels) and its
// details (absent: none), one name per row. A row's semantic is its index and its id 0; a file type's semantic is its
// icon (0 image, 1 video, 2 audio, 3 document, 4 archive) and its id its index + 1.
bool ui_search_filter_fixed_choice(UiSearchFilterKind kind, u32 index, UiSearchFilterChoiceView* row) noexcept {
    const char *labels, *values = nullptr, *details = nullptr;
    switch (kind) {
    case UiSearchFilterKind::has:
        labels = "Link\0Embed\0File\0Video\0Image\0Sound\0Sticker\0Poll\0Forward\0";
        values = kMessageSearchHasNames;
        break;
    case UiSearchFilterKind::pinned:
        labels = "True\0False\0";
        values = "true\0false\0";
        break;
    case UiSearchFilterKind::author_type:
        labels = "User\0Bot\0Webhook\0";
        values = kMessageSearchAuthorTypeNames;
        break;
    case UiSearchFilterKind::file_type:
        labels = "png\0jpg\0gif\0webp\0mp4\0mov\0mp3\0pdf\0txt\0zip\0";
        details = "Image\0Image\0Image\0Image\0Video\0Video\0Audio\0Document\0Text\0Archive\0";
        break;
    default: return false;
    }
    const Utf8View label = name_list_at(labels, index);
    if (!label.bytes)
        return false;
    static constexpr u8 file_icons[]{0, 0, 0, 0, 1, 1, 2, 3, 3, 4};
    const bool file = !values;
    *row = {file ? index + 1 : 0,
            file ? file_icons[index] : index,
            label,
            details ? name_list_at(details, index) : Utf8View{},
            name_list_at(values ? values : labels, index),
            {}};
    return true;
}
} // namespace maicord
