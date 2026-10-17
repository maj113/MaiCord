#include "maicord/ui/settings_schema.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/settings_scene.hpp"

namespace maicord {
namespace {
struct Word {
    const char* data;
    u32 bytes;
};
u8 fold(u8 byte) noexcept {
    return byte >= 'A' && byte <= 'Z' ? static_cast<u8>(byte + 32) : byte;
}
bool word_byte(u8 byte) noexcept {
    return byte >= 0x80 || (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
           (byte >= 'A' && byte <= 'Z');
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
bool prefixes_word(Word query, Utf8View text) noexcept {
    for (u32 i = 0; i < text.bytes;) {
        while (i < text.bytes && !word_byte(static_cast<u8>(text.data[i])))
            ++i;
        const u32 start = i;
        while (i < text.bytes && word_byte(static_cast<u8>(text.data[i])))
            ++i;
        if (i - start < query.bytes)
            continue;
        u32 j = 0;
        while (j < query.bytes && fold(static_cast<u8>(text.data[start + j])) == fold(static_cast<u8>(query.data[j])))
            ++j;
        if (j == query.bytes)
            return true;
    }
    return false;
}
bool all_words(const Word* words, u32 count, const Utf8View* fields, u32 field_count) noexcept {
    for (u32 w = 0; w < count; ++w) {
        bool found = false;
        for (u32 f = 0; f < field_count && !found; ++f)
            found = prefixes_word(words[w], fields[f]);
        if (!found)
            return false;
    }
    return true;
}
bool choice_kind(UiSettingsControlKind kind) noexcept {
    return kind == UiSettingsControlKind::choice || kind == UiSettingsControlKind::radio ||
           kind == UiSettingsControlKind::slider || kind == UiSettingsControlKind::palette;
}
bool result_kind(UiSettingsControlKind kind) noexcept {
    return kind != UiSettingsControlKind::category && kind != UiSettingsControlKind::notice;
}
bool valid_text(Utf8View text) noexcept {
    return (!text.bytes || text.data) && utf8_validate(text).valid;
}
} // namespace

bool ui_settings_schema_valid(const UiSettingsSchema& schema) noexcept {
    if ((schema.panels.count && !schema.panels.data) || (schema.sections.count && !schema.sections.data))
        return false;
    for (u32 p = 0; p < schema.panels.count; ++p) {
        const auto& panel = schema.panels[p];
        if (!panel.key.bytes || !valid_text(panel.key) || !panel.title.bytes || !valid_text(panel.title) ||
            !valid_text(panel.keywords) || panel.section >= schema.sections.count ||
            (panel.nodes.count && !panel.nodes.data))
            return false;
        for (u32 n = 0; n < panel.nodes.count; ++n) {
            const auto& node = panel.nodes[n];
            if (!node.key.bytes || !valid_text(node.key) || !valid_text(node.label) ||
                !valid_text(node.description) || !valid_text(node.keywords) || !valid_text(node.action_label) ||
                (!node.label.bytes && !(node.flags & ui_settings_schema_dynamic)))
                return false;
            if (choice_kind(node.kind) && !(node.flags & ui_settings_schema_dynamic) &&
                (!node.choices.count || !node.choices.data ||
                 (node.default_choice != kUiSettingsNoChoice && node.default_choice >= node.choices.count)))
                return false;
        }
    }
    // Keys are unique across panels and nodes so deep links never alias.
    for (u32 p = 0; p < schema.panels.count; ++p)
        for (u32 n = 0; n <= schema.panels[p].nodes.count; ++n) {
            const Utf8View key = n ? schema.panels[p].nodes[n - 1].key : schema.panels[p].key;
            UiSettingsSchemaLocation location{};
            if (!ui_settings_schema_find(schema, key, &location) || location.panel != p ||
                location.node != (n ? n - 1 : kUiSettingsNoChoice))
                return false;
        }
    return true;
}

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
            if (utf8_equal(panel.nodes[n].key, key)) {
                *location = {p, n};
                return true;
            }
    }
    return false;
}

u32 ui_settings_schema_category(const UiSettingsSchemaPanel& panel, u32 node) noexcept {
    for (u32 i = minimum(node, panel.nodes.count); i-- > 0;)
        if (panel.nodes[i].kind == UiSettingsControlKind::category)
            return i;
    return kUiSettingsNoChoice;
}

bool ui_settings_search_text_matches(Utf8View query, Utf8View text) noexcept {
    Word words[kUiSettingsSearchMaximumWords]{};
    const u32 count = split(query, words, kUiSettingsSearchMaximumWords);
    return count && count <= kUiSettingsSearchMaximumWords && all_words(words, count, &text, 1);
}

u32 ui_settings_schema_search(const UiSettingsSchema& schema, Utf8View query,
                              Slice<UiSettingsSearchHit> output) noexcept {
    Word words[kUiSettingsSearchMaximumWords]{};
    const u32 count = split(query, words, kUiSettingsSearchMaximumWords);
    if (!count || count > kUiSettingsSearchMaximumWords || !output.data || !output.count)
        return 0;
    u32 used = 0;
    for (u32 p = 0; p < schema.panels.count; ++p) {
        const auto& panel = schema.panels[p];
        for (u32 n = 0; n < panel.nodes.count; ++n) {
            const auto& node = panel.nodes[n];
            if (!result_kind(node.kind) || (node.flags & ui_settings_schema_search_hidden) || !node.label.bytes)
                continue;
            const u32 category = ui_settings_schema_category(panel, n);
            // Panel titles and keywords name the panel, not every row in it.
            const Utf8View context[]{node.label, node.keywords, node.description,
                                     category == kUiSettingsNoChoice ? Utf8View{} : panel.nodes[category].label};
            u32 score = 0;
            if (all_words(words, count, &node.label, 1))
                score = 4;
            else if (all_words(words, count, &node.keywords, 1))
                score = 3;
            else if (all_words(words, count, &node.description, 1))
                score = 2;
            else if (all_words(words, count, context, 4))
                score = 1;
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
