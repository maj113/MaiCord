#include "settings/catalog.hpp"
#include "maicord/core/ascii.hpp"
#include "../../support/test_output.hpp"

// The settings catalog on Linux says "your desktop" or "the system" where the Windows data names Windows
// (catalog_data.inc keeps the Windows text; catalog.cpp replaces it once on Linux), and leaves out the rows this
// system has nothing behind: the Windows notification settings page and the voice processing switch.

namespace maicord {
namespace {

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

// Whether `text` holds `word`, with the ASCII letters of both folded.
bool holds(Utf8View text, const char* word) noexcept {
    const u32 length = c_string_length(word);
    for (u32 start = 0; start + length <= text.bytes; ++start) {
        u32 at = 0;
        while (at < length && ascii_lower(text.data[start + at]) == ascii_lower(word[at]))
            ++at;
        if (at == length)
            return true;
    }
    return false;
}

constexpr const char* kWindowsWords[] = {"windows", ".exe", "registry"};

bool names_windows(Utf8View text) noexcept {
    for (const char* word : kWindowsWords)
        if (holds(text, word))
            return true;
    return false;
}

bool equals(Utf8View text, const char* expected) noexcept {
    return text.bytes == c_string_length(expected) && bytes_equal(text.data, expected, text.bytes);
}

void report(const char* what, Utf8View key) noexcept {
    test_write_text(what);
    test_write_bytes(key.data, key.bytes);
    test_write_text("\n");
}

void test_texts() noexcept {
    const UiSettingsSchema& catalog = settings_catalog();
    u32 nodes = 0, offenders = 0;
    for (u32 p = 0; p < catalog.panels.count; ++p) {
        const UiSettingsSchemaPanel& panel = catalog.panels[p];
        if (names_windows(panel.title) || names_windows(panel.keywords) || names_windows(panel.summary)) {
            ++offenders;
            report("panel names Windows: ", panel.key);
        }
        for (u32 n = 0; n < panel.nodes.count; ++n) {
            const UiSettingsSchemaNode& node = panel.nodes[n];
            ++nodes;
            const auto binding = static_cast<SettingBinding>(node.binding_slot);
            if (settings_binding_unavailable(binding))
                continue;  // not shown, not searched
            bool names = names_windows(node.label()) || names_windows(node.description()) ||
                         names_windows(node.keywords()) || names_windows(node.action_label());
            for (u32 i = 0; i < node.choices().count; ++i)
                names = names || names_windows(node.choices()[i]);
            if (names) {
                ++offenders;
                report("node names Windows: ", node.key());
                report("  label: ", node.label());
                report("  description: ", node.description());
                report("  keywords: ", node.keywords());
            }
        }
    }
    check(catalog.panels.count > 10 && nodes > 100, "the catalog holds its panels and nodes");
    check(offenders == 0, "no shown catalog text names Windows");
}

void test_replacements() noexcept {
    UiSettingsSchemaLocation where{};
    check(ui_settings_schema_find(settings_catalog(), utf8_literal("language.time_format"), &where) &&
              equals(settings_catalog().panels[where.panel].nodes[where.node].description(),
                     "Auto follows your system locale."),
          "the time format row follows the system locale");
    check(ui_settings_schema_find(settings_catalog(), utf8_literal("notifications.taskbar_flash"), &where) &&
              equals(settings_catalog().panels[where.panel].nodes[where.node].label(), "Enable App Icon Flashing"),
          "the taskbar flashing row names the app icon");
    check(ui_settings_schema_find(settings_catalog(), utf8_literal("updates.assets"), &where) &&
              equals(settings_catalog().panels[where.panel].nodes[where.node].label(),
                     "Updates replace MaiCord, its fonts and its licenses."),
          "the update footnote names the program, its fonts and its licenses, not an .exe");
    check(equals(settings_catalog_panel(SettingsPanel::updates).summary,
                 "Updates are signed. Nothing installs until you restart."),
          "the updates page's summary does not promise that every install updates itself");
    check(ui_settings_schema_find(settings_catalog(), utf8_literal("voice.push_to_talk_key"), &where) &&
              holds(settings_catalog().panels[where.panel].nodes[where.node].description(), "allows global shortcuts") &&
              ui_settings_schema_find(settings_catalog(), utf8_literal("keybinds.custom"), &where) &&
              holds(settings_catalog().panels[where.panel].nodes[where.node].description(), "allows global shortcuts"),
          "the push-to-talk wording depends on the desktop's global shortcuts");
    const UiSettingsSchemaPanel& system = settings_catalog_panel(SettingsPanel::system);
    check(equals(system.summary, "Startup, the tray and how links open on your desktop."),
          "the system page's summary names the desktop");
    check(settings_binding_unavailable(SettingBinding::windows_notification_settings) &&
              settings_binding_unavailable(SettingBinding::voice_processing),
          "the Windows notification page and the voice processing switch are unavailable");
    check(!settings_binding_unavailable(SettingBinding::taskbar_flash) &&
              !settings_binding_unavailable(SettingBinding::open_on_startup) &&
              !settings_binding_unavailable(SettingBinding::protocol_handler) &&
              !settings_binding_unavailable(SettingBinding::voice_input_device),
          "the rows with a Linux backing stay");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_linux_catalog_text_entry) {
    maicord::test_texts();
    maicord::test_replacements();
    maicord::test_exit(maicord::g_failures, "linux_catalog_text");
}
