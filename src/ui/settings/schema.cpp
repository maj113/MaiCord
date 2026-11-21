#include "maicord/ui/settings/schema.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/ui/settings/scene.hpp"

namespace maicord {
namespace {
struct Word {
    const char* data;
    u32 bytes;
};
bool word_byte(u8 byte) noexcept {
    return byte >= 0x80 || ascii_is_alnum(byte);
}
u32 split(Utf8View text, Word* words, u32 capacity) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < text.bytes;) {
        while (i < text.bytes && !word_byte(static_cast<u8>(text.data[i])))
            ++i;
        const u32 start = i;
        while (i < text.bytes && word_byte(static_cast<u8>(text.data[i])))
            ++i;
        if (i > start) {
            if (count == capacity)
                return capacity + 1;
            words[count++] = {text.data + start, i - start};
        }
    }
    return count;
}
// Bit w is set when query word w prefixes a word of text (ASCII case-insensitive). The text is split once for all
// the query words.
u32 word_mask(const Word* words, u32 count, Utf8View text) noexcept {
    const u32 all = (1u << count) - 1;
    u32 mask = 0;
    for (u32 i = 0; i < text.bytes && mask != all;) {
        while (i < text.bytes && !word_byte(static_cast<u8>(text.data[i])))
            ++i;
        const u32 start = i;
        while (i < text.bytes && word_byte(static_cast<u8>(text.data[i])))
            ++i;
        for (u32 w = 0; w < count; ++w) {
            const Word query = words[w];
            if (i - start < query.bytes)
                continue;
            u32 j = 0;
            while (j < query.bytes && ascii_lower(text.data[start + j]) == ascii_lower(query.data[j]))
                ++j;
            if (j == query.bytes)
                mask |= 1u << w;
        }
    }
    return mask;
}
bool result_kind(UiSettingsControlKind kind) noexcept {
    return kind != UiSettingsControlKind::category && kind != UiSettingsControlKind::notice;
}
} // namespace

bool ui_settings_schema_find(const UiSettingsSchema& schema, Utf8View key,
                             UiSettingsSchemaLocation* location) noexcept {
    if (!key.bytes || !location)
        return false;
    for (u32 p = 0; p < schema.panels.count; ++p) {
        const auto& panel = schema.panels[p];
        if (utf8_equal(panel.key, key)) {
            *location = {p, kUiSettingsNoChoice};
            return true;
        }
        for (u32 n = 0; n < panel.nodes.count; ++n)
            if (utf8_equal(panel.nodes[n].key(), key)) {
                *location = {p, n};
                return true;
            }
    }
    return false;
}

bool ui_settings_search_text_matches(Utf8View query, Utf8View text) noexcept {
    Word words[kUiSettingsSearchMaximumWords]{};
    const u32 count = split(query, words, kUiSettingsSearchMaximumWords);
    return count && count <= kUiSettingsSearchMaximumWords && word_mask(words, count, text) == (1u << count) - 1;
}

u32 ui_settings_schema_search(const UiSettingsSchema& schema, Utf8View query,
                              Slice<UiSettingsSearchHit> output) noexcept {
    Word words[kUiSettingsSearchMaximumWords]{};
    const u32 count = split(query, words, kUiSettingsSearchMaximumWords);
    if (!count || count > kUiSettingsSearchMaximumWords || !output.data || !output.count)
        return 0;
    const u32 all = (1u << count) - 1;
    u32 used = 0;
    for (u32 p = 0; p < schema.panels.count; ++p) {
        const auto& panel = schema.panels[p];
        // The query words in the label of the category over the node. Panel titles and keywords name the panel, not
        // every row in it.
        u32 category = 0;
        for (u32 n = 0; n < panel.nodes.count; ++n) {
            const auto& node = panel.nodes[n];
            if (node.kind == UiSettingsControlKind::category)
                category = word_mask(words, count, node.label());
            if (!result_kind(node.kind) || (node.flags & ui_settings_schema_search_hidden) || !node.label().bytes)
                continue;
            // Every word in the label, else in the keywords, else in the description, else spread over all of them.
            const u32 label = word_mask(words, count, node.label());
            u32 score = 4;
            if (label != all) {
                const u32 keywords = word_mask(words, count, node.keywords());
                score = 3;
                if (keywords != all) {
                    const u32 description = word_mask(words, count, node.description());
                    score = description == all ? 2 : (label | keywords | description | category) == all ? 1 : 0;
                }
            }
            if (!score)
                continue;
            // Stable insertion by descending score keeps catalog order for ties.
            u32 at = used;
            while (at && output[at - 1].score < score)
                --at;
            if (at == output.count)
                continue;
            for (u32 i = minimum(used, output.count - 1); i > at; --i)
                output[i] = output[i - 1];
            output[at] = {p, n, score};
            used = minimum(used + 1, output.count);
        }
    }
    return used;
}

} // namespace maicord
