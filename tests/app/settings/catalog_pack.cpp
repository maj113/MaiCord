#include "settings/catalog.hpp"
#include "maicord/ui/settings/scene.hpp"

// The shipped settings catalog is a deflate stream that catalog.cpp inflates into its tables. This test compiles
// the data file the stream is packed from (src/app/settings/catalog_data.inc, as constant data) and checks every
// field of every section, panel and node of the decoded catalog against it.
namespace maicord {
namespace {
using K = UiSettingsControlKind;
constexpr u32 none = kUiSettingsNoChoice;
constexpr u16 dynamic = ui_settings_schema_dynamic, hidden = ui_settings_schema_search_hidden;
constexpr u32 bind(SettingBinding binding) noexcept {
    return static_cast<u32>(binding);
}
constexpr UiSettingsSchemaNode node(Utf8View key, K kind, Utf8View label, Utf8View description, Utf8View keywords,
                                    SettingBinding binding = SettingBinding::none, ConstSlice<Utf8View> choices = {},
                                    u32 default_choice = none, Utf8View action = {}, u16 flags = 0,
                                    u8 variant = 0) noexcept {
    return {key, label, description, keywords, kind, variant, flags, choices, default_choice, bind(binding), action};
}
// The numbered label sets are formatted at run time; the data file only names them and counts their entries.
struct Labels {
    Utf8View values[361];
};
Labels degree_labels, percent_labels, decibel_labels, millisecond_labels, zoom_labels, lens_labels;

#include "../../../src/app/settings/catalog_data.inc"

bool same_text(Utf8View a, Utf8View b) noexcept {
    return utf8_equal(a, b) && (a.data == nullptr) == (b.data == nullptr);
}
bool numbered_labels(const UiSettingsSchemaNode& reference) noexcept {
    const Utf8View* const sets[]{degree_labels.values, percent_labels.values, decibel_labels.values,
                                 millisecond_labels.values, zoom_labels.values, lens_labels.values};
    for (const Utf8View* set : sets)
        if (reference.choice_data == set)
            return true;
    return false;
}
bool same_node(const UiSettingsSchemaNode& decoded, const UiSettingsSchemaNode& reference) noexcept {
    if (!same_text(decoded.key(), reference.key()) || !same_text(decoded.label(), reference.label()) ||
        !same_text(decoded.description(), reference.description()) ||
        !same_text(decoded.keywords(), reference.keywords()) ||
        !same_text(decoded.action_label(), reference.action_label()) || decoded.kind != reference.kind ||
        decoded.variant != reference.variant || decoded.flags != reference.flags ||
        decoded.binding() != reference.binding() || decoded.default_choice() != reference.default_choice() ||
        decoded.choice_count != reference.choice_count)
        return false;
    if (numbered_labels(reference))
        // Formatted by catalog.cpp: the list has the declared length and its ends are text.
        return decoded.choice_count && decoded.choice_data && decoded.choices()[0].bytes &&
               decoded.choices()[decoded.choice_count - 1].bytes;
    for (u32 i = 0; i < reference.choice_count; ++i)
        if (!same_text(decoded.choices()[i], reference.choices()[i]))
            return false;
    return !!decoded.choice_data == !!reference.choice_data;
}
bool same_nodes(const UiSettingsSchemaPanel& decoded, const UiSettingsSchemaPanel& reference) noexcept {
    if (decoded.nodes.count != reference.nodes.count)
        return false;
    for (u32 i = 0; i < reference.nodes.count; ++i)
        if (!same_node(decoded.nodes[i], reference.nodes[i]))
            return false;
    return true;
}
bool same_panel(const UiSettingsSchemaPanel& decoded, const UiSettingsSchemaPanel& reference) noexcept {
    return same_text(decoded.key, reference.key) && same_text(decoded.title, reference.title) &&
           same_text(decoded.keywords, reference.keywords) && same_text(decoded.summary, reference.summary) &&
           decoded.icon == reference.icon && decoded.section == reference.section;
}
} // namespace

void test_settings_catalog_pack(void (*check)(bool, const char*) noexcept) noexcept {
    const auto& catalog = settings_catalog();
    constexpr u32 section_count = sizeof(sections) / sizeof(sections[0]);
    constexpr u32 panel_count = sizeof(panels) / sizeof(panels[0]);
    bool sections_match = catalog.sections.count == section_count;
    for (u32 i = 0; sections_match && i < section_count; ++i)
        sections_match = same_text(catalog.sections[i].header, sections[i].header);
    check(sections_match, "settings catalog pack: the section headers match the data file");

    bool panels_match = catalog.panels.count == panel_count && panel_count == kSettingsPanelCount, nodes_match = true;
    u32 nodes = 0;
    for (u32 i = 0; panels_match && i < panel_count; ++i) {
        panels_match = same_panel(catalog.panels[i], panels[i]);
        nodes_match = nodes_match && same_nodes(catalog.panels[i], panels[i]);
        nodes += panels[i].nodes.count;
    }
    check(panels_match, "settings catalog pack: panel keys, titles, keywords, summaries, icons and sections match the data file");
    check(nodes_match && nodes != 0,
          "settings catalog pack: every node's text, kind, variant, flags, binding, default and choices match the data file");

    // The sidebar's pages and headers come from the decoded panels and sections.
    const auto navigation = settings_catalog_navigation(true);
    const auto signed_out = settings_catalog_navigation_signed_out();
    bool nav = navigation.count == 25 && settings_catalog_navigation(false).count == 24 && signed_out.count == 10;
    for (u32 i = 0; nav && i < navigation.count; ++i) {
        const auto& entry = navigation[i];
        if (entry.kind == UiSettingsNavKind::page) {
            nav = entry.page.owner_id == kSettingsNativeOwner && entry.page.page_id - 1 < kSettingsPanelCount &&
                  same_text(entry.label, catalog.panels[entry.page.page_id - 1].title) &&
                  entry.icon == catalog.panels[entry.page.page_id - 1].icon && !entry.destructive;
        } else if (entry.kind == UiSettingsNavKind::header) {
            nav = i == 0 ? same_text(entry.label, sections[0].header) : entry.label.bytes != 0;
        } else {
            nav = i == 24 && entry.kind == UiSettingsNavKind::action && entry.destructive && entry.pinned &&
                  entry.action == kSettingsLogOutAction && utf8_equal(entry.label, utf8_literal("Log Out"));
        }
    }
    check(nav && signed_out[0].kind == UiSettingsNavKind::header && signed_out[9].pinned &&
              signed_out[9].page.page_id == static_cast<u64>(SettingsPanel::advanced) + 1,
          "settings catalog pack: the sidebar lists headers, panels and Log Out in order");
}

} // namespace maicord
