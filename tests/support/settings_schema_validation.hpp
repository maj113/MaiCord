#pragma once

// Data lint for settings schemas: unique non-empty keys, valid UTF-8, panel
// sections in range, choices present for choice-like kinds and default choices
// in range. The client builds its schema from static data and never checks it.
#include "maicord/core/utf.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/settings/schema.hpp"

namespace maicord {

inline bool ui_settings_schema_valid(const UiSettingsSchema& schema) noexcept {
    const auto valid_text = [](Utf8View text) noexcept { return (!text.bytes || text.data) && utf8_validate(text).valid; };
    const auto choice_kind = [](UiSettingsControlKind kind) noexcept {
        return kind == UiSettingsControlKind::choice || kind == UiSettingsControlKind::radio ||
               kind == UiSettingsControlKind::slider || kind == UiSettingsControlKind::palette;
    };
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
            if (!node.key().bytes || !valid_text(node.key()) || !valid_text(node.label()) ||
                !valid_text(node.description()) || !valid_text(node.keywords()) || !valid_text(node.action_label()) ||
                (!node.label().bytes && !(node.flags & ui_settings_schema_dynamic)))
                return false;
            if (choice_kind(node.kind) && !(node.flags & ui_settings_schema_dynamic) &&
                (!node.choices().count || !node.choices().data ||
                 (node.default_choice() != kUiSettingsNoChoice && node.default_choice() >= node.choices().count)))
                return false;
        }
    }
    // Keys are unique across panels and nodes so deep links never alias.
    for (u32 p = 0; p < schema.panels.count; ++p)
        for (u32 n = 0; n <= schema.panels[p].nodes.count; ++n) {
            const Utf8View key = n ? schema.panels[p].nodes[n - 1].key() : schema.panels[p].key;
            UiSettingsSchemaLocation location{};
            if (!ui_settings_schema_find(schema, key, &location) || location.panel != p ||
                location.node != (n ? n - 1 : kUiSettingsNoChoice))
                return false;
        }
    return true;
}

} // namespace maicord
