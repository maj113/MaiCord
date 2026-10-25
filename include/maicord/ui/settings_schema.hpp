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

// Text pointers first, then the small fields, so the node packs to one cache
// line. Text over 255 bytes, over 65535 choices or a binding or default
// choice over 65534 cannot be encoded; constructing such a node in a constant
// expression fails to compile.
namespace ui_settings_schema_detail {
void field_out_of_range() noexcept; // Deliberately undefined: only a failed range check calls it.
constexpr u8 narrow_text(u32 bytes) noexcept {
    if (bytes > 0xff)
        field_out_of_range();
    return static_cast<u8>(bytes);
}
constexpr u16 narrow_field(u32 value) noexcept {
    if (value > 0xffff)
        field_out_of_range();
    return static_cast<u16>(value);
}
} // namespace ui_settings_schema_detail
struct UiSettingsSchemaNode {
    const char *key_data, *label_data, *description_data, *keywords_data, *action_data;
    const Utf8View* choice_data;
    u16 choice_count, default_choice_slot, binding_slot, flags;
    u8 key_bytes, label_bytes, description_bytes, keywords_bytes, action_bytes;
    UiSettingsControlKind kind;
    u8 variant;

    // default_option ~0u means no default.
    constexpr UiSettingsSchemaNode(Utf8View key, Utf8View label, Utf8View description,
                                   Utf8View keywords, UiSettingsControlKind control_kind,
                                   u8 control_variant, u16 control_flags, ConstSlice<Utf8View> options,
                                   u32 default_option, u32 control_binding, Utf8View action) noexcept
        : key_data(key.data), label_data(label.data), description_data(description.data),
          keywords_data(keywords.data), action_data(action.data), choice_data(options.data),
          choice_count(ui_settings_schema_detail::narrow_field(options.count)),
          default_choice_slot(default_option == ~0u ? u16{0xffff}
                                                    : ui_settings_schema_detail::narrow_field(default_option)),
          binding_slot(ui_settings_schema_detail::narrow_field(control_binding)), flags(control_flags),
          key_bytes(ui_settings_schema_detail::narrow_text(key.bytes)),
          label_bytes(ui_settings_schema_detail::narrow_text(label.bytes)),
          description_bytes(ui_settings_schema_detail::narrow_text(description.bytes)),
          keywords_bytes(ui_settings_schema_detail::narrow_text(keywords.bytes)),
          action_bytes(ui_settings_schema_detail::narrow_text(action.bytes)), kind(control_kind),
          variant(control_variant) {}

    constexpr Utf8View key() const noexcept { return {key_data, key_bytes}; }
    constexpr Utf8View label() const noexcept { return {label_data, label_bytes}; }
    constexpr Utf8View description() const noexcept { return {description_data, description_bytes}; }
    constexpr Utf8View keywords() const noexcept { return {keywords_data, keywords_bytes}; }
    constexpr Utf8View action_label() const noexcept { return {action_data, action_bytes}; }
    constexpr ConstSlice<Utf8View> choices() const noexcept { return {choice_data, choice_count}; }
    // ~0u (kUiSettingsNoChoice) when the node has no default.
    constexpr u32 default_choice() const noexcept {
        return default_choice_slot == 0xffff ? ~0u : default_choice_slot;
    }
    constexpr u32 binding() const noexcept { return binding_slot; }
};
static_assert(sizeof(UiSettingsSchemaNode) == 64);

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
