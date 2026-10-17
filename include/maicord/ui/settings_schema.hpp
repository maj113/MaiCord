#pragma once

#include "maicord/ui/icon.hpp"

namespace maicord {
enum class UiSettingsControlKind : u8;

// Declarative settings vocabulary: sections -> panels -> nodes. Keys are stable
// dotted identifiers ("appearance.theme") shared by search, deep links and
// storage bindings. Catalog data is constexpr; hosts project live values and
// omit nodes without backing at runtime.
enum UiSettingsSchemaFlag : u16 {
    ui_settings_schema_none = 0,
    ui_settings_schema_search_hidden = 1u << 0,
    ui_settings_schema_marks = 1u << 1,
    ui_settings_schema_dynamic = 1u << 2,
};

struct UiSettingsSchemaNode {
    Utf8View key, label, description, keywords;
    UiSettingsControlKind kind;
    u8 variant;
    u16 flags;
    ConstSlice<Utf8View> choices;
    u32 default_choice;
    u32 binding;
    Utf8View action_label;
};

struct UiSettingsSchemaPanel {
    Utf8View key, title, keywords;
    UiIcon icon;
    u8 section;
    ConstSlice<UiSettingsSchemaNode> nodes;
};

struct UiSettingsSchemaSection {
    Utf8View header;
};

struct UiSettingsSchema {
    ConstSlice<UiSettingsSchemaSection> sections;
    ConstSlice<UiSettingsSchemaPanel> panels;
};

struct UiSettingsSchemaLocation {
    u32 panel, node;
};

// Unique non-empty keys, valid UTF-8, panel sections in range, choices present
// for choice-like kinds and default choices in range.
bool ui_settings_schema_valid(const UiSettingsSchema&) noexcept;
// A panel key resolves with node kUiSettingsNoChoice.
bool ui_settings_schema_find(const UiSettingsSchema&, Utf8View key, UiSettingsSchemaLocation*) noexcept;
// Index of the category node that precedes node, or kUiSettingsNoChoice.
u32 ui_settings_schema_category(const UiSettingsSchemaPanel&, u32 node) noexcept;

// Word-prefix search: every query word must prefix a word of the node label,
// keywords, description or category label (ASCII case-insensitive).
// Categories and notices are never results. Hits are ordered by match
// strength (label, keywords, description, context), then catalog order.
struct UiSettingsSearchHit {
    u32 panel, node, score;
};
inline constexpr u32 kUiSettingsSearchMaximumWords = 8;
bool ui_settings_search_text_matches(Utf8View query, Utf8View text) noexcept;
u32 ui_settings_schema_search(const UiSettingsSchema&, Utf8View query, Slice<UiSettingsSearchHit> output) noexcept;

} // namespace maicord
