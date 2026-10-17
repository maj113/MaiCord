// Settings > Plugins with the catalog: control counts per tab and filter,
// chip text, paging over 130 entries, the detail view and the dialog texts.
// A synthetic snapshot; nothing touches the disk or the network.
#include "../src/app/application_plugin_browser_projection.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/ui/theme.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
using namespace app_detail;
u32 g_failures;
void check(bool passed, const char* name) noexcept { test_check(&g_failures, passed, name); }

constexpr u32 kEntries = 130, kOfficial = 100;
constexpr u64 kCustomId = 0x1234;
struct World {
    PluginCatalogSnapshot snapshot;
    PluginCatalogEntryView entries[kEntries];
    PluginCatalogVersionView versions[kEntries];
    char documents[64 * 1024];
    PluginBrowserBank bank;
    NativePluginCatalogEntry inventory_entries[1];
    NativePluginCatalog inventory;
};
World w;

PluginIndexText text(u32 provider, const char* value) noexcept {
    auto& p = w.snapshot.providers[provider];
    const u32 offset = p.document_bytes;
    u32 n = 0;
    while (value[n]) { w.documents[p.document_offset + offset + n] = value[n]; ++n; }
    p.document_bytes += n;
    w.snapshot.documents_bytes = maximum(w.snapshot.documents_bytes, p.document_offset + p.document_bytes);
    return {offset, n};
}
void set_text(char* out, u32* bytes, const char* value) noexcept {
    u32 n = 0;
    while (value[n]) { out[n] = value[n]; ++n; }
    *bytes = n;
}
void build() noexcept {
    clear_bytes(&w, sizeof(w));
    auto& s = w.snapshot;
    s.entries = w.entries;
    s.versions = w.versions;
    s.documents = w.documents;
    s.configured = true;
    s.provider_count = 2;
    auto& official = s.providers[0];
    official.id = kPluginProviderOfficial;
    official.kind = PluginProviderKind::official;
    official.status = PluginCatalogStatus::ok;
    official.installable = official.enabled = official.check_daily = true;
    set_text(official.host, &official.host_bytes, "example.github.io");
    set_text(official.name, &official.name_bytes, "MaiCord plugins");
    auto& custom = s.providers[1];
    custom.id = kCustomId;
    custom.kind = PluginProviderKind::custom_signed;
    custom.status = PluginCatalogStatus::ok;
    custom.installable = custom.enabled = custom.check_daily = true;
    custom.document_offset = 32 * 1024;
    set_text(custom.host, &custom.host_bytes, "example.com");
    set_text(custom.path, &custom.path_bytes, "/plugins/");
    set_text(custom.name, &custom.name_bytes, "Example plugins");
    custom.fingerprint[0] = 0xAB; custom.fingerprint[1] = 0x12; custom.fingerprint[2] = 0xCD; custom.fingerprint[3] = 0x34;
    for (u32 i = 0; i < kEntries; ++i) {
        const u32 provider = i < kOfficial ? 0 : 1;
        char name[16] = "Plugin ";
        u32 at = 7;
        at += decimal_write(name + at, i);
        name[at] = 0;
        auto& e = w.entries[i];
        e.owner = 1000 + i;
        e.provider = provider;
        e.name = text(provider, name);
        e.summary = text(provider, "Does useful things.");
        e.author = text(provider, "Author");
        e.license = text(provider, "MPL-2.0");
        e.author_count = 1;
        e.categories = 1u << (i % 10);
        e.first_version = i;
        e.version_count = 1;
        e.installed_index = ~0u;
        auto& v = w.versions[i];
        v.code = 1000000999;
        v.released = 1760200000;
        v.bytes = 48640;
        v.permissions = i == 0 ? 0b111 : 0b1;
        v.compatibility = PluginCompatibility::compatible;
    }
    w.entries[0].disclosures = 0b11;
    w.versions[0].changelog = text(0, "First release.");
    // Needs a newer MaiCord: nothing installable.
    w.versions[5].compatibility = PluginCompatibility::needs_newer_host;
    w.entries[5].newest = kPluginIndexNoVersion;
    s.entry_count = s.version_count = kEntries;
    // Two installed: an official one with an update, a custom one whose update needs review.
    s.installed_count = 2;
    auto& a = s.installed[0];
    a.owner = w.entries[1].owner; a.provider = kPluginProviderOfficial; a.has_state = true; a.trusted = true;
    a.installed_code = 1000000999; a.approved_permissions = 1; a.entry = 1;
    set_text(a.name, &a.name_bytes, "Plugin 1");
    set_text(a.version, &a.version_bytes, "1.0.0");
    w.versions[1].code = 1001000999;
    w.entries[1].installed_index = 0;
    w.entries[1].bound = true;
    w.entries[1].update.outcome = PluginUpdateOutcome::available;
    auto& b = s.installed[1];
    b.owner = w.entries[101].owner; b.provider = kCustomId; b.has_state = true; b.trusted = true;
    b.installed_code = 1000000999; b.entry = 101;
    set_text(b.name, &b.name_bytes, "Plugin 101");
    set_text(b.version, &b.version_bytes, "0.9.0");
    w.entries[101].installed_index = 1;
    w.entries[101].bound = true;
    w.entries[101].update.outcome = PluginUpdateOutcome::review;
    w.entries[101].update.permissions = {0b10, 0, 0b1};
}
PluginBrowserFacts facts() noexcept {
    PluginBrowserFacts f{};
    f.snapshot = &w.snapshot;
    f.configured = true;
    f.now_unix = 1760400000;
    f.checked_unix = 1760400000 - 300;
    return f;
}
const UiSettingsControl* find(u32 count, u64 id) noexcept {
    for (u32 i = 0; i < count; ++i) if (w.bank.controls[i].id == id) return &w.bank.controls[i];
    return nullptr;
}
bool contains(Utf8View haystack, const char* needle) noexcept {
    const u32 n = test_text_length(needle);
    for (u32 i = 0; i + n <= haystack.bytes; ++i)
        if (bytes_equal(haystack.data + i, needle, n)) return true;
    return false;
}
bool is(Utf8View a, const char* b) noexcept { return a.bytes == test_text_length(b) && bytes_equal(a.data, b, a.bytes); }

void test_browse() noexcept {
    build();
    PluginBrowserUiState ui{};
    u32 count = plugin_browser_project(ui, facts(), &w.bank);
    const auto* tabs = find(count, kPluginBrowserTabsId);
    check(count == 15 && tabs && tabs->kind == UiSettingsControlKind::tabs && tabs->choices.count == 4 && is(tabs->choices[2], "Updates (2)") &&
        w.bank.map.row_count == 8 && w.bank.map.rows[7] == 7 && is(find(count, kPluginBrowserCountId)->label, "130 plugins") &&
        !find(count, kPluginBrowserPreviousId)->enabled && find(count, kPluginBrowserNextId)->enabled,
        "plugin browser: Browse shows tabs, search, filters, a count, 8 rows and paging (15 controls)");
    const auto* first = find(count, kPluginBrowserRowFirst);
    check(first && is(first->label, "Plugin 0") && is(first->description, "by Author \xC2\xB7 Does useful things.") &&
        first->choices.count == 1 && is(first->choices[0], "Official") && is(first->action_label, "View") &&
        (w.bank.presentation[5].flags & ui_settings_presentation_avatar),
        "plugin browser: a row shows name, author and summary, the host-drawn Official chip and View");
    const auto* updated = find(count, kPluginBrowserRowFirst + 1);
    check(updated && updated->choices.count == 3 && is(updated->choices[1], "Installed") && is(updated->choices[2], "Update 1.1.0"),
        "plugin browser: an installed plugin with an update shows Installed and Update 1.1.0");
    const auto* newer = find(count, kPluginBrowserRowFirst + 5);
    check(newer && newer->choices.count == 2 && is(newer->choices[1], "Needs a newer MaiCord"),
        "plugin browser: a plugin no version of which runs here says Needs a newer MaiCord");
    ui.first = 128;
    count = plugin_browser_project(ui, facts(), &w.bank);
    check(count == 9 && w.bank.map.row_count == 2 && w.bank.map.rows[1] == 129 && find(count, kPluginBrowserPreviousId)->enabled &&
        !find(count, kPluginBrowserNextId)->enabled, "plugin browser: the last of 17 pages holds 2 rows with Next off");
    ui.first = 0;
    ui.source = PluginBrowserSource::community;
    count = plugin_browser_project(ui, facts(), &w.bank);
    first = find(count, kPluginBrowserRowFirst);
    check(is(find(count, kPluginBrowserCountId)->label, "30 plugins") && first && is(first->choices[0], "Unofficial \xC2\xB7 example.com") &&
        w.bank.map.rows[0] == kOfficial, "plugin browser: Community lists 30 with the host-drawn Unofficial \xC2\xB7 example.com chip");
    ui.source = PluginBrowserSource::official;
    count = plugin_browser_project(ui, facts(), &w.bank);
    check(is(find(count, kPluginBrowserCountId)->label, "100 plugins"), "plugin browser: Official lists 100");
    ui.source = PluginBrowserSource::all;
    ui.category = 3;
    count = plugin_browser_project(ui, facts(), &w.bank);
    check(is(find(count, kPluginBrowserCountId)->label, "13 plugins") && find(count, kPluginBrowserCategoryId)->selected_choice == 3 &&
        is(find(count, kPluginBrowserCategoryId)->choices[3], "Notifications") && count == 15,
        "plugin browser: a category filter keeps 13 and the choice names it");
    ui.category = 0;
    constexpr char query[] = "plugin 12";
    copy_bytes(ui.query, query, sizeof(query) - 1);
    ui.query_bytes = sizeof(query) - 1;
    count = plugin_browser_project(ui, facts(), &w.bank);
    const auto* search = find(count, kPluginBrowserSearchId);
    check(is(find(count, kPluginBrowserCountId)->label, "11 plugins") && search && search->choices.count == 2 &&
        contains(search->description, "plugin 12"), "plugin browser: a search matches 11 names and offers Clear");
    PluginBrowserFacts unconfigured = facts();
    unconfigured.configured = false;
    ui.query_bytes = 0;
    count = plugin_browser_project(ui, unconfigured, &w.bank);
    check(count == 16 && contains(find(count, kPluginBrowserStatusId)->label, "isn't configured"),
        "plugin browser: an unconfigured build says so above the list");
    w.snapshot.providers[0].status = PluginCatalogStatus::expired;
    w.snapshot.providers[0].issued = 1760226600;
    count = plugin_browser_project(ui, facts(), &w.bank);
    const auto* stale = find(count, kPluginBrowserStatusId);
    check(stale && contains(stale->label, "2025-10-11") && is(stale->action_label, "Refresh") &&
        w.bank.presentation[1].variant == UiSettingsVariant::critical, "plugin browser: an expired catalog shows its date and Refresh");
}
void test_detail() noexcept {
    build();
    PluginBrowserUiState ui{};
    ui.detail = true;
    ui.detail_provider = kPluginProviderOfficial;
    ui.detail_owner = w.entries[0].owner;
    u32 count = plugin_browser_project(ui, facts(), &w.bank);
    const auto* actions = find(count, kPluginBrowserActionsId);
    check(w.bank.map.detail_entry == 0 && actions && actions->choices.count == 1 && is(actions->choices[0], "Install") &&
        w.bank.map.actions[0] == PluginBrowserDetailAction::install, "plugin detail: a plugin not installed offers Install");
    check(find(count, kPluginBrowserPermissionFirst) && find(count, kPluginBrowserPermissionFirst + 2) &&
        !find(count, kPluginBrowserPermissionFirst + 3) && find(count, kPluginBrowserDisclosuresId) &&
        contains(find(count, kPluginBrowserDisclosuresId)->label, "the internet, your files") &&
        is(find(count, kPluginBrowserChangelogId)->label, "First release.") && !find(count, kPluginBrowserNoticeFirst),
        "plugin detail: one permission row per permission (3), disclosures and the changelog; no warning for an official plugin");
    ui.detail_owner = w.entries[5].owner;
    count = plugin_browser_project(ui, facts(), &w.bank);
    check(!find(count, kPluginBrowserActionsId) && w.bank.map.action_count == 0 && find(count, kPluginBrowserNoticeFirst) &&
        contains(find(count, kPluginBrowserNoticeFirst)->label, "needs a newer version"),
        "plugin detail: Install is withheld and the notice says it needs a newer MaiCord");
    ui.detail_provider = kCustomId;
    ui.detail_owner = w.entries[100].owner;
    count = plugin_browser_project(ui, facts(), &w.bank);
    const auto* warning = find(count, kPluginBrowserNoticeFirst);
    check(warning && contains(warning->label, "From example.com, an unofficial provider") &&
        w.bank.presentation[warning - w.bank.controls].variant == UiSettingsVariant::critical,
        "plugin detail: an unofficial plugin carries the unofficial-provider warning");
    ui.detail_provider = kPluginProviderOfficial;
    ui.detail_owner = w.entries[1].owner;
    count = plugin_browser_project(ui, facts(), &w.bank);
    actions = find(count, kPluginBrowserActionsId);
    check(actions && actions->choices.count == 3 && is(actions->choices[0], "Update to 1.1.0") && is(actions->choices[1], "Enable") &&
        is(actions->choices[2], "Uninstall") && find(count, kPluginBrowserAutoUpdateId) && find(count, kPluginBrowserAutoUpdateId)->checked,
        "plugin detail: an installed official plugin offers Update, Enable and Uninstall and its automatic update toggle");
    u64 loaded[]{w.entries[1].owner};
    PluginBrowserFacts running = facts();
    running.loaded = {loaded, 1};
    running.settings_owners = {loaded, 1};
    count = plugin_browser_project(ui, running, &w.bank);
    actions = find(count, kPluginBrowserActionsId);
    check(actions && actions->choices.count == 4 && is(actions->choices[1], "Disable") && is(actions->choices[2], "Plugin settings"),
        "plugin detail: a running plugin offers Disable and its settings page");
    ui.detail_owner = 1;
    count = plugin_browser_project(ui, facts(), &w.bank);
    check(count == 15 && w.bank.map.detail_entry == ~0u, "plugin detail: a vanished entry falls back to the list");
}
void test_tabs() noexcept {
    build();
    PluginBrowserUiState ui{};
    ui.tab = PluginBrowserTab::updates;
    u32 count = plugin_browser_project(ui, facts(), &w.bank);
    const auto* header = find(count, kPluginBrowserUpdatesHeaderId);
    const auto* official = find(count, kPluginBrowserUpdateFirst);
    const auto* custom = find(count, kPluginBrowserUpdateFirst + 1);
    check(count == 5 && header && header->choices.count == 2 && is(header->choices[0], "Update all (1)") && w.bank.map.update_all == 1 &&
        contains(header->description, "Last checked 5 minutes ago") && official && is(official->action_label, "Update") &&
        is(official->description, "1.0.0 \xE2\x86\x92 1.1.0") && custom && is(custom->action_label, "Review") &&
        custom->choices.count == 2 && is(custom->choices[1], "New permissions") && w.bank.map.update_count == 2,
        "plugin updates: Update all counts only rows without Review; Review rows say New permissions");
    ui.tab = PluginBrowserTab::providers;
    count = plugin_browser_project(ui, facts(), &w.bank);
    const auto* row = find(count, kPluginBrowserProviderFirst);
    check(count == 4 && find(count, kPluginBrowserOfficialId)->checked && row && row->choices.count == 3 && is(row->choices[0], "Refresh") &&
        is(row->choices[1], "Turn off checks") && contains(row->description, "example.com/plugins/ \xC2\xB7 Signed, key AB12 CD34 \xE2\x80\xA6") &&
        contains(row->description, "Unofficial") && find(count, kPluginBrowserAddProviderId)->enabled,
        "plugin providers: the official row, a signed custom row with its key and Add provider");
    w.snapshot.providers[1].status = PluginCatalogStatus::key_changed;
    count = plugin_browser_project(ui, facts(), &w.bank);
    row = find(count, kPluginBrowserProviderFirst);
    check(row && row->choices.count == 2 && is(row->choices[1], "Trust new key") &&
        w.bank.map.provider_actions[0][1] == PluginBrowserProviderAction::trust_key && contains(row->description, "different signing key"),
        "plugin providers: a changed key offers Remove or Trust new key");
    // Installed: the local rows with catalog chips.
    build();
    w.inventory_entries[0].manifest.owner_id = w.entries[1].owner;
    w.inventory.entries = {w.inventory_entries, 1};
    w.inventory.count = 1;
    const UiSettingsControl local[]{{1, utf8_literal("Load plugin"), {}, UiSettingsControlKind::action, true, false, {}, 0, utf8_literal("Choose DLL")},
        {kApplicationPluginInstallAction, utf8_literal("Install or update plugin"), {}, UiSettingsControlKind::action, true, false, {}, 0, {}},
        {kApplicationPluginCatalogActionFirst, utf8_literal("Plugin 1"), {}, UiSettingsControlKind::action, true, false, {}, 0, utf8_literal("Enable")}};
    PluginBrowserFacts f = facts();
    f.local = {local, 3};
    f.inventory = &w.inventory;
    ui.tab = PluginBrowserTab::installed;
    count = plugin_browser_project(ui, f, &w.bank);
    row = find(count, kApplicationPluginCatalogActionFirst);
    check(count == 4 && row && row->enabled && row->choices.count == 2 && is(row->choices[0], "Official") && is(row->choices[1], "Update 1.1.0"),
        "plugin installed: local rows keep their ids and gain the provider chip and update");
    PluginStartupGate gate{};
    gate.count = 1;
    gate.owners[0] = w.entries[1].owner;
    gate.reasons[0] = kPluginGateRevoked;
    PluginRevokedPlugin revoked{};
    revoked.owner = w.entries[1].owner;
    set_text(revoked.name, &revoked.name_bytes, "Plugin 1");
    f.gate = &gate;
    f.revoked = &revoked;
    count = plugin_browser_project(ui, f, &w.bank);
    row = find(count, kApplicationPluginCatalogActionFirst);
    const auto* notice = find(count, kPluginBrowserRevokedId);
    check(count == 5 && row && !row->enabled && is(row->action_label, "Turned off") && is(row->choices[1], "Revoked") && notice &&
        contains(notice->label, "MaiCord turned off Plugin 1 because its provider found it harmful.") && is(notice->action_label, "Remove"),
        "plugin installed: a revoked plugin is turned off, its Enable hidden, and the notice offers Remove");
    check(plugin_browser_control(kPluginBrowserTabsId) && plugin_browser_control(kPluginBrowserAddProviderId) &&
        !plugin_browser_control(kApplicationPluginCatalogActionFirst + 31) && !plugin_browser_control(410),
        "plugin browser: its ids stay clear of the local rows");
}
// Every view lays out and draws its text through the real settings scene.
struct SceneFixture {
    UiPage page;
    UiPageRegistry registry;
    UiSettingsScene scene;
    UiSettingsPageContent content;
    UiTextCommand commands[512];
};
SceneFixture g_scene;
u32 g_scene_line, g_scene_commands, g_scene_count;
bool lays_out(u32 count, UiRect viewport) noexcept {
    auto& f = g_scene;
    ui_settings_scene_release(&f.scene);
    f.content = {utf8_literal("Plugins"), utf8_literal("Add features to MaiCord."), {w.bank.controls, count}, 1};
    f.content.presentation = w.bank.presentation;
    UiTextCommandStream stream{};
    UiTextOutput stream_output{&stream, nullptr};
    if (!ui_page_registry_initialize(&f.registry, {&f.page, 1}) ||
        !ui_page_registry_register(&f.registry, {{1, 4}, utf8_literal("Plugins"), &f.content}).ok()) { g_scene_line = __LINE__; return false; }
    if (!ui_settings_scene_update(&f.scene, &f.registry, viewport)) { g_scene_line = __LINE__; return false; }
    if (!ui_text_command_stream_initialize(&stream, {f.commands, 512}) || !ui_settings_scene_text_output(f.scene, kDefaultTheme, &stream_output)) {
        g_scene_line = __LINE__;
        return false;
    }
    g_scene_commands = maximum(g_scene_commands, stream.count);
    g_scene_count = stream.count;
    return true;
}
// The probe's readability rule: the exact text with a visible, unclipped area.
bool drawn(Utf8View text) noexcept {
    for (u32 i = 0; i < g_scene_count; ++i) {
        const auto& command = g_scene.commands[i];
        if (utf8_equal(command.text, text) && ui_rect_has_area(ui_rect_intersection(command.rectangle, command.clip))) return true;
    }
    return false;
}
void test_scene() noexcept {
    build();
    const UiRect wide{0, 0, 2560, 1440}, narrow{0, 0, 640, 480};
    PluginBrowserUiState ui{};
    u32 failed = 0, first_failed = ~0u;
    const auto view = [&](u32 index) noexcept {
        const u32 count = plugin_browser_project(ui, facts(), &w.bank);
        if (!lays_out(count, wide) || !lays_out(plugin_browser_project(ui, facts(), &w.bank), narrow)) {
            ++failed;
            if (first_failed == ~0u) first_failed = index * 1000 + g_scene_line;
        }
    };
    view(1);
    ui.source = PluginBrowserSource::community;
    view(2);
    ui = {};
    ui.detail = true; ui.detail_provider = kPluginProviderOfficial; ui.detail_owner = w.entries[0].owner;
    view(3);
    ui.detail_owner = w.entries[1].owner;
    view(4);
    ui.detail_owner = w.entries[5].owner;
    view(5);
    ui.detail_provider = kCustomId; ui.detail_owner = w.entries[101].owner;
    view(6);
    ui = {};
    ui.tab = PluginBrowserTab::updates;
    view(7);
    ui.tab = PluginBrowserTab::providers;
    view(8);
    w.snapshot.providers[1].status = PluginCatalogStatus::key_changed;
    view(9);
    if (failed) { test_write_text("first failing view*1000+line: "); test_write_u64(first_failed); test_write_text("\n"); }
    test_write_text("MEASURE plugin_browser_text_commands="); test_write_u64(g_scene_commands); test_write_text("\n");
    check(!failed, "plugin browser: every tab and detail view lays out and draws its text in the settings scene");
    ui = {};
    ui.source = PluginBrowserSource::community;
    const bool community = lays_out(plugin_browser_project(ui, facts(), &w.bank), wide) && drawn(utf8_literal("Unofficial \xC2\xB7 example.com"));
    ui = {};
    ui.tab = PluginBrowserTab::updates;
    const bool updates = lays_out(plugin_browser_project(ui, facts(), &w.bank), wide) && drawn(utf8_literal("New permissions")) &&
        drawn(utf8_literal("Update all (1)"));
    ui.tab = PluginBrowserTab::providers;
    const bool providers = lays_out(plugin_browser_project(ui, facts(), &w.bank), wide) && drawn(utf8_literal("Trust new key"));
    check(community && updates && providers, "plugin browser: chips and buttons are drawn as their own visible text");
    ui_settings_scene_release(&g_scene.scene);
}
void test_dialogs() noexcept {
    PluginBrowserDialog d{};
    PluginBrowserInstallFacts f{};
    f.name = utf8_literal("Showcase");
    f.publisher = utf8_literal("MaiCord");
    f.provider_name = utf8_literal("MaiCord plugins");
    f.kind = PluginProviderKind::official;
    f.version_code = 1000000999;
    f.permissions = 0b11;
    f.disclosures = 0b1;
    plugin_browser_install_dialog(f, &d);
    check(is(d.title, "Install Showcase?") && contains(d.context, "From MaiCord plugins (official). Version 1.0.0.") &&
        contains(d.context, "Published by MaiCord.") && contains(d.context, native_plugin_permission_label(0).data) &&
        contains(d.context, "\xE2\x80\xA2 the internet") && is(d.accept, "Install and enable") && is(d.option, "Enable after installing") &&
        d.option_checked && !d.danger, "plugin dialog: install lists provider, publisher, permissions and disclosures");
    f.kind = PluginProviderKind::custom_unsigned;
    f.host = utf8_literal("example.com");
    plugin_browser_install_dialog(f, &d);
    check(contains(d.context, "From example.com (unofficial).") && contains(d.context, "doesn't sign its catalog") &&
        is(d.accept, "Install unofficial plugin") && d.danger, "plugin dialog: an unofficial install uses the danger fill and the unsigned sentence");
    f.kind = PluginProviderKind::official;
    f.downgrade_from = 1002000999;
    plugin_browser_install_dialog(f, &d);
    check(is(d.title, "Install an older version of Showcase?") && contains(d.context, "This replaces version 1.2.0.") && d.danger && !d.option.bytes,
        "plugin dialog: a downgrade warns about the version it replaces");
    plugin_browser_review_dialog(utf8_literal("Showcase"), 1003000999, {0b100, 0b10, 0b1}, &d);
    check(is(d.title, "Update Showcase to 1.3.0?") && contains(d.context, "New: ") && contains(d.context, "Still allowed:") &&
        contains(d.context, "No longer needed:") && is(d.accept, "Allow and update") && is(d.cancel, "Not now") &&
        is(d.option, "Skip this version instead"), "plugin dialog: the permission review lists new, kept and dropped permissions");
    u8 fingerprint[32]{0xAB, 0x12, 0xCD, 0x34, 0xEF, 0x56, 0x78, 0x90, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    PluginBrowserProviderFacts p{};
    p.host = utf8_literal("example.com");
    p.path = utf8_literal("/plugins/");
    p.name = utf8_literal("Example plugins");
    p.plugin_count = 3;
    p.signed_provider = p.pin_matched = true;
    p.fingerprint = fingerprint;
    plugin_browser_provider_dialog(p, &d);
    check(is(d.title, "Add an unofficial plugin provider?") && contains(d.context, "Provider: Example plugins \xC2\xB7 3 plugins") &&
        contains(d.context, "Address: https://example.com/plugins/") &&
        contains(d.context, "Signing key: AB12 CD34 EF56 7890 1122 3344 5566 7788 (the key matches the link you were given)") &&
        d.danger && d.option_required && !plugin_browser_dialog_accepts(d, false) && plugin_browser_dialog_accepts(d, true),
        "plugin dialog: adding a provider shows the fingerprint groups and refuses Accept until I trust this provider is checked");
    p.signed_provider = false;
    plugin_browser_provider_dialog(p, &d);
    check(contains(d.context, "This provider doesn't sign its catalog. Anyone who controls example.com can change what you install."),
        "plugin dialog: an unsigned provider says anyone controlling its host can change installs");
    u8 old[32]{0x01, 0x02};
    p.signed_provider = true;
    p.pin_matched = false;
    p.old_fingerprint = old;
    plugin_browser_provider_dialog(p, &d);
    check(is(d.title, "Trust the new key of example.com?") && contains(d.context, "Old key: 0102 0000") &&
        contains(d.context, "New key: AB12 CD34") && is(d.accept, "Trust new key") && d.option_required,
        "plugin dialog: a key change shows the old and new fingerprints");
    plugin_browser_remove_provider_dialog(utf8_literal("Example plugins"), 2, &d);
    check(is(d.title, "Remove Example plugins?") && contains(d.context, "stay installed but won't get updates") &&
        is(d.option, "Also uninstall its 2 plugins") && d.danger, "plugin dialog: removing a provider offers to uninstall its plugins");
    plugin_browser_uninstall_dialog(utf8_literal("Showcase"), &d);
    check(is(d.title, "Uninstall Showcase?") && is(d.option, "Also delete its saved data") && !d.option_checked && d.danger &&
        plugin_browser_dialog_accepts(d, false), "plugin dialog: uninstall keeps saved data unless asked");
    plugin_browser_rollback_dialog(utf8_literal("Showcase"), 1001000999, 1002000999, &d);
    check(is(d.title, "Roll back Showcase to 1.1.0?") && contains(d.context, "Version 1.2.0 won't be offered again"),
        "plugin dialog: a rollback says the version left is held");
}
} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_plugin_browser_tests_entry() noexcept {
    using namespace maicord;
    test_browse();
    test_detail();
    test_tabs();
    test_scene();
    test_dialogs();
    ExitProcess(g_failures ? 1 : 0);
}
