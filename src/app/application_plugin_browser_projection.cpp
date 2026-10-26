#include "application_plugin_browser_projection.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/decimal.hpp"

namespace maicord::app_detail {
namespace {
// Appends whole UTF-8 sequences while they fit in `capacity`.
void append_utf8(char* buffer, u32& used, u32 capacity, const char* data, u32 bytes) noexcept {
    for (u32 i = 0; i < bytes;) {
        const auto decoded = utf8_decode_one(data + i, bytes - i);
        if (!decoded.valid || !decoded.width || decoded.width > capacity - used) break;
        for (u32 j = 0; j < decoded.width; ++j) buffer[used++] = data[i++];
    }
}
void put(PluginBrowserBank& bank, const char* data, u32 bytes) noexcept {
    append_utf8(bank.text, bank.used, kPluginBrowserTextBytes, data, bytes);
}
Utf8View add(PluginBrowserBank& bank, const Utf8View& text) noexcept {
    const u32 start = bank.used;
    put(bank, text.data, text.bytes);
    return {bank.text + start, bank.used - start};
}
// A literal passes its address and length in registers; an unused result costs nothing.
template <usize Count> __forceinline Utf8View add(PluginBrowserBank& bank, const char (&text)[Count]) noexcept {
    const u32 start = bank.used;
    put(bank, text, Count - 1);
    return {bank.text + start, bank.used - start};
}
void number(PluginBrowserBank& bank, u64 value) noexcept {
    char digits[20];
    put(bank, digits, decimal_write(digits, value));
}
void version(PluginBrowserBank& bank, u64 code) noexcept {
    char text[32];
    put(bank, text, update_version_text(code, {text, sizeof(text)}));
}
// Decoded index text (the index caps every field at 512 bytes).
void index_text(PluginBrowserBank& bank, const Utf8View& document, PluginIndexText text) noexcept {
    if (!text.bytes) return;
    char decoded[520];
    const auto result = plugin_index_text_decode(document, text, {decoded, sizeof(decoded)});
    if (result.ok()) add(bank, {decoded, result.bytes});
}
Utf8View since(const PluginBrowserBank& bank, u32 start) noexcept { return {bank.text + start, bank.used - start}; }
const PluginCatalogProviderView& provider_of(const PluginCatalogSnapshot& s, const PluginCatalogEntryView& e) noexcept {
    return s.providers[e.provider];
}
bool official(const PluginCatalogSnapshot& s, const PluginCatalogEntryView& e) noexcept {
    return provider_of(s, e).kind == PluginProviderKind::official;
}
// The provider chip: drawn from the provider kind and host alone.
Utf8View provider_chip(PluginBrowserBank& bank, PluginProviderKind kind, const Utf8View& host) noexcept {
    if (kind == PluginProviderKind::official) return add(bank, "Official");
    const u32 start = bank.used;
    add(bank, "Unofficial \xC2\xB7 ");
    add(bank, host);
    return since(bank, start);
}
bool matches(const PluginCatalogSnapshot& s, const PluginCatalogEntryView& e, const PluginBrowserUiState& ui) noexcept {
    if (ui.source == PluginBrowserSource::official && !official(s, e)) return false;
    if (ui.source == PluginBrowserSource::community && official(s, e)) return false;
    if (ui.category && !(e.categories & (1u << (ui.category - 1)))) return false;
    if (!ui.query_bytes) return true;
    const Utf8View document = plugin_catalog_document(s, e.provider);
    const Utf8View query{ui.query, minimum(ui.query_bytes, kPluginBrowserQueryBytes)};
    char text[520];
    const PluginIndexText fields[]{e.name, e.summary, e.author, e.slug};
    for (const auto& field : fields) {
        const auto decoded = plugin_index_text_decode(document, field, {text, sizeof(text)});
        if (decoded.ok() && ascii_contains_ignore_case({text, decoded.bytes}, query)) return true;
    }
    return false;
}
bool loaded(const PluginBrowserFacts& facts, u64 owner) noexcept {
    for (u32 i = 0; i < facts.loaded.count; ++i) if (facts.loaded[i] == owner) return true;
    return false;
}
const PluginCatalogInstalledView* installed(const PluginCatalogSnapshot& s, const PluginCatalogEntryView& e) noexcept {
    return e.installed_index < s.installed_count ? &s.installed[e.installed_index] : nullptr;
}
const PluginCatalogInstalledView* installed_owner(const PluginCatalogSnapshot* s, u64 owner) noexcept {
    for (u32 i = 0; s && i < s->installed_count; ++i) if (s->installed[i].owner == owner) return &s->installed[i];
    return nullptr;
}
bool update_offered(const PluginCatalogEntryView& e) noexcept {
    return e.bound && (e.update.outcome == PluginUpdateOutcome::available || e.update.outcome == PluginUpdateOutcome::review);
}
u32 update_count(const PluginCatalogSnapshot* s) noexcept {
    u32 n = 0;
    for (u32 i = 0; s && i < s->entry_count; ++i) n += update_offered(s->entries[i]) ? 1u : 0u;
    return n;
}
bool needs_newer_host(const PluginCatalogSnapshot& s, const PluginCatalogEntryView& e) noexcept {
    for (u32 v = 0; v < e.version_count; ++v)
        if (s.versions[e.first_version + v].compatibility == PluginCompatibility::needs_newer_host) return true;
    return false;
}
// "Needs a newer MaiCord" when a newer host would run some version, otherwise "Not compatible".
Utf8View compatibility_text(const PluginCatalogSnapshot& s, const PluginCatalogEntryView& e) noexcept {
    return needs_newer_host(s, e) ? utf8_literal("Needs a newer MaiCord") : utf8_literal("Not compatible");
}
void relative_time(PluginBrowserBank& bank, u64 now, u64 then) noexcept {
    if (!then) { add(bank, "never"); return; }
    const u64 age = now > then ? now - then : 0;
    if (age < 60) { add(bank, "just now"); return; }
    const u64 minutes = age / 60, hours = age / 3600, days = age / 86400;
    number(bank, days ? days : hours ? hours : minutes);
    add(bank, days ? (days == 1 ? utf8_literal(" day ago") : utf8_literal(" days ago"))
        : hours ? (hours == 1 ? utf8_literal(" hour ago") : utf8_literal(" hours ago"))
        : minutes == 1 ? utf8_literal(" minute ago") : utf8_literal(" minutes ago"));
}
// YYYY-MM-DD (UTC) of a unix time.
void date(PluginBrowserBank& bank, u64 unix) noexcept {
    i64 days = static_cast<i64>(unix / 86400) + 719468;
    const i64 era = days / 146097, doe = days - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    const u64 day = static_cast<u64>(doy - (153 * mp + 2) / 5 + 1), month = static_cast<u64>(mp < 10 ? mp + 3 : mp - 9);
    const u64 year = static_cast<u64>(yoe + era * 400 + (month <= 2 ? 1 : 0));
    number(bank, year);
    add(bank, month < 10 ? utf8_literal("-0") : utf8_literal("-")); number(bank, month);
    add(bank, day < 10 ? utf8_literal("-0") : utf8_literal("-")); number(bank, day);
}
void bytes_text(PluginBrowserBank& bank, u64 bytes) noexcept {
    if (bytes < 1024) { number(bank, bytes); add(bank, " bytes"); return; }
    const u64 tenths = bytes < 1024 * 1024 ? bytes * 10 / 1024 : bytes * 10 / (1024 * 1024);
    number(bank, tenths / 10); add(bank, "."); number(bank, tenths % 10);
    add(bank, bytes < 1024 * 1024 ? utf8_literal(" KB") : utf8_literal(" MB"));
}
struct Emitter {
    PluginBrowserBank& bank;
    u32 count;
    // Out of line: inlined at each row, the braced control copy repeats.
    __declspec(noinline) UiSettingsControl& emit(u64 id, const Utf8View& label, const Utf8View& description,
        UiSettingsControlKind kind, bool enabled = true, const Utf8View& action = {},
        UiSettingsVariant variant = UiSettingsVariant::standard,
        u8 flags = 0) noexcept {
        static UiSettingsControl overflow{};
        if (count >= kPluginBrowserControls) {
            reset_value(&overflow);
            return overflow;
        }
        bank.controls[count] = {id, label, description, kind, enabled, false, {}, 0, action, nullptr};
        bank.presentation[count] = {variant, flags, kUiSettingsNoDefault};
        return bank.controls[count++];
    }
    // The common plain row: enabled, no action button, standard look.
    __declspec(noinline) UiSettingsControl& row(u64 id, const Utf8View& label, const Utf8View& description,
        UiSettingsControlKind kind) noexcept {
        return emit(id, label, description, kind);
    }
};
} // namespace

// ---- list -------------------------------------------------------------------------------------
u32 plugin_browser_filtered(const PluginCatalogSnapshot* snapshot, const PluginBrowserUiState& ui) noexcept {
    u32 n = 0;
    for (u32 i = 0; snapshot && i < snapshot->entry_count; ++i) n += matches(*snapshot, snapshot->entries[i], ui) ? 1u : 0u;
    return n;
}
u32 plugin_browser_find(const PluginCatalogSnapshot* snapshot, u64 provider, u64 owner) noexcept {
    for (u32 i = 0; snapshot && i < snapshot->entry_count; ++i) {
        const auto& e = snapshot->entries[i];
        if (e.owner == owner && snapshot->providers[e.provider].id == provider) return i;
    }
    return ~0u;
}

namespace {
void tabs(Emitter& out, const PluginBrowserUiState& ui, const PluginBrowserFacts& facts) noexcept {
    auto& bank = out.bank;
    bank.tabs[0] = add(bank, "Browse");
    bank.tabs[1] = add(bank, "Installed");
    const u32 updates = facts.snapshot ? update_count(facts.snapshot) : facts.offers;
    const u32 start = bank.used;
    add(bank, "Updates");
    if (updates) { add(bank, " ("); number(bank, updates); add(bank, ")"); }
    bank.tabs[2] = since(bank, start);
    bank.tabs[3] = add(bank, "Providers");
    auto& control = out.row(kPluginBrowserTabsId, utf8_literal("Plugins"), {}, UiSettingsControlKind::tabs);
    control.choices = {bank.tabs, 4};
    control.selected_choice = static_cast<u32>(ui.tab);
}
// Status line, then the harmful-plugin notice; both on every tab.
void notices(Emitter& out, const PluginBrowserUiState& ui, const PluginBrowserFacts& facts) noexcept {
    auto& bank = out.bank;
    const PluginCatalogSnapshot* s = facts.snapshot;
    if (facts.status.bytes) {
        out.emit(kPluginBrowserStatusId, facts.status, {}, UiSettingsControlKind::notice, true, {},
            facts.status_critical ? UiSettingsVariant::critical : UiSettingsVariant::standard);
    } else if (ui.tab == PluginBrowserTab::browse && !facts.configured) {
        out.row(kPluginBrowserStatusId, utf8_literal("The official plugin catalog isn't configured for this build"),
            utf8_literal("Plugins from providers you add still appear here."), UiSettingsControlKind::notice);
    } else if (ui.tab == PluginBrowserTab::browse && s && s->provider_count && s->providers[0].kind == PluginProviderKind::official) {
        const auto& p = s->providers[0];
        const bool stale = p.status == PluginCatalogStatus::expired || p.status == PluginCatalogStatus::unadmitted;
        if (stale || p.status == PluginCatalogStatus::failed) {
            const u32 start = bank.used;
            if (p.status == PluginCatalogStatus::failed) add(bank, "Couldn't read the saved catalog");
            else { add(bank, "Catalog information from "); date(bank, p.issued); add(bank, " is out of date"); }
            out.emit(kPluginBrowserStatusId, since(bank, start), utf8_literal("Refresh to install or update plugins."),
                UiSettingsControlKind::notice, !facts.busy, utf8_literal("Refresh"), UiSettingsVariant::critical);
        } else if (p.status == PluginCatalogStatus::none) {
            out.emit(kPluginBrowserStatusId, facts.checking ? utf8_literal("Checking the catalog\xE2\x80\xA6") : utf8_literal("No catalog yet"),
                {}, UiSettingsControlKind::notice, !facts.busy, facts.checking ? Utf8View{} : utf8_literal("Refresh"));
        }
    }
    if (facts.revoked && facts.revoked->owner) {
        const u32 start = bank.used;
        add(bank, "MaiCord turned off ");
        add(bank, facts.revoked->name_bytes ? Utf8View{facts.revoked->name, facts.revoked->name_bytes} : utf8_literal("a plugin"));
        add(bank, " because its provider found it harmful.");
        const bool present = installed_owner(s, facts.revoked->owner) != nullptr;
        out.emit(kPluginBrowserRevokedId, since(bank, start), utf8_literal("It stays off. Remove it from your computer."),
            UiSettingsControlKind::notice, present && !facts.busy, present ? utf8_literal("Remove") : Utf8View{}, UiSettingsVariant::critical);
    }
}
// Chips of a list row, provider first.
ConstSlice<Utf8View> entry_chips(PluginBrowserBank& bank, u32 slot, const PluginCatalogSnapshot& s, const PluginCatalogEntryView& e) noexcept {
    Utf8View* chips = bank.chips[slot];
    u32 n = 0;
    const auto& provider = provider_of(s, e);
    chips[n++] = provider_chip(bank, provider.kind, {provider.host, provider.host_bytes});
    const auto* local = installed(s, e);
    if (local && (local->state_flags & kPluginStateRevokedMalicious)) chips[n++] = add(bank, "Revoked");
    else if (local) chips[n++] = add(bank, "Installed");
    if (update_offered(e) && n < kPluginBrowserChips) {
        const u32 start = bank.used;
        add(bank, "Update ");
        version(bank, s.versions[e.first_version + e.update.version].code);
        chips[n++] = since(bank, start);
    }
    if (e.newest == kPluginIndexNoVersion && n < kPluginBrowserChips) chips[n++] = compatibility_text(s, e);
    if (local && (local->state_flags & kPluginStateRevokedBroken) && n < kPluginBrowserChips) chips[n++] = add(bank, "Known problem");
    return {chips, n};
}
void author_line(PluginBrowserBank& bank, const Utf8View& document, const PluginCatalogEntryView& e) noexcept {
    if (e.author.bytes) {
        add(bank, "by ");
        index_text(bank, document, e.author);
        if (e.author_count > 1) add(bank, " and others");
        add(bank, " \xC2\xB7 ");
    }
}
void browse(Emitter& out, PluginBrowserUiState ui, const PluginBrowserFacts& facts) noexcept {
    auto& bank = out.bank;
    const PluginCatalogSnapshot* s = facts.snapshot;
    bank.search[0] = add(bank, "Search\xE2\x80\xA6");
    bank.search[1] = add(bank, "Clear");
    const Utf8View query{ui.query, minimum(ui.query_bytes, kPluginBrowserQueryBytes)};
    u32 start = bank.used;
    if (query.bytes) { add(bank, "Showing results for \xE2\x80\x9C"); add(bank, query); add(bank, "\xE2\x80\x9D"); }
    auto& search = out.emit(kPluginBrowserSearchId, utf8_literal("Search"), since(bank, start), UiSettingsControlKind::buttons, !!s);
    search.choices = {bank.search, query.bytes ? 2u : 1u};
    bank.sources[0] = utf8_literal("All"); bank.sources[1] = utf8_literal("Official"); bank.sources[2] = utf8_literal("Community");
    auto& source = out.emit(kPluginBrowserSourceId, utf8_literal("Source"), {}, UiSettingsControlKind::segments, !!s);
    source.choices = {bank.sources, 3};
    source.selected_choice = static_cast<u32>(ui.source);
    bank.categories[0] = utf8_literal("All");
    for (u32 i = 0; i < kPluginIndexCategoryCount; ++i) {
        const u32 at = bank.used;
        add(bank, plugin_index_category_name(i));
        if (bank.used > at && bank.text[at] >= 'a' && bank.text[at] <= 'z') bank.text[at] = static_cast<char>(bank.text[at] - 32);
        bank.categories[i + 1] = since(bank, at);
    }
    auto& category = out.emit(kPluginBrowserCategoryId, utf8_literal("Category"), {}, UiSettingsControlKind::choice, !!s);
    category.choices = {bank.categories, kPluginIndexCategoryCount + 1};
    category.selected_choice = minimum(ui.category, kPluginIndexCategoryCount);
    const u32 total = plugin_browser_filtered(s, ui);
    start = bank.used;
    number(bank, total);
    add(bank, total == 1 ? utf8_literal(" plugin") : utf8_literal(" plugins"));
    out.row(kPluginBrowserCountId, since(bank, start), {}, UiSettingsControlKind::category);
    const u32 first = total && ui.first < total ? ui.first / kPluginBrowserRows * kPluginBrowserRows : 0;
    bank.map.row_count = 0;
    for (u32 index = 0, nth = 0; s && index < s->entry_count && bank.map.row_count < kPluginBrowserRows; ++index) {
        if (!matches(*s, s->entries[index], ui) || nth++ < first) continue;
        const u32 row = bank.map.row_count;
        const auto& e = s->entries[index];
        const Utf8View document = plugin_catalog_document(*s, e.provider);
        start = bank.used;
        index_text(bank, document, e.name);
        const Utf8View name = since(bank, start);
        start = bank.used;
        author_line(bank, document, e);
        index_text(bank, document, e.summary);
        const Utf8View description = since(bank, start);
        auto& control = out.emit(kPluginBrowserRowFirst + row, name, description, UiSettingsControlKind::action, true,
            utf8_literal("View"), UiSettingsVariant::standard,
            static_cast<u8>(ui_settings_presentation_avatar | ui_settings_presentation_compact));
        control.choices = entry_chips(bank, row, *s, e);
        bank.map.rows[bank.map.row_count++] = index;
    }
    if (total > kPluginBrowserRows) {
        out.emit(kPluginBrowserPreviousId, utf8_literal("Previous plugins"), {}, UiSettingsControlKind::action, first != 0,
            utf8_literal("Previous"));
        out.emit(kPluginBrowserNextId, utf8_literal("More plugins"), {}, UiSettingsControlKind::action, total - first > kPluginBrowserRows,
            utf8_literal("Next"));
    }
}
} // namespace

// ---- detail -----------------------------------------------------------------------------------
namespace {
Utf8View disclosure_label(u32 bit) noexcept {
    static constexpr Utf8View labels[]{utf8_literal("the internet"), utf8_literal("your files"), utf8_literal("other programs"),
        utf8_literal("your keyboard and mouse")};
    return bit < 4 ? labels[bit] : Utf8View{};
}
void disclosures_text(PluginBrowserBank& bank, u32 disclosures) noexcept {
    add(bank, "Also uses (outside MaiCord's permissions): ");
    bool first = true;
    for (u32 bit = 0; bit < kPluginIndexDisclosureCount; ++bit)
        if (disclosures & (1u << bit)) {
            if (!first) add(bank, ", ");
            first = false;
            add(bank, disclosure_label(bit));
        }
}
bool has_settings(const PluginBrowserFacts& facts, u64 owner) noexcept {
    for (u32 i = 0; i < facts.settings_owners.count; ++i) if (facts.settings_owners[i] == owner) return true;
    return false;
}
void detail(Emitter& out, const PluginBrowserFacts& facts, u32 index) noexcept {
    auto& bank = out.bank;
    const PluginCatalogSnapshot& s = *facts.snapshot;
    const PluginCatalogEntryView& e = s.entries[index];
    const PluginCatalogProviderView& provider = provider_of(s, e);
    const Utf8View document = plugin_catalog_document(s, e.provider);
    const Utf8View host{provider.host, provider.host_bytes};
    const bool custom = provider.kind != PluginProviderKind::official;
    const auto* local = installed(s, e);
    const bool is_loaded = local && loaded(facts, e.owner);
    const bool revoked = local && (local->state_flags & kPluginStateRevokedMalicious);
    const u32 newest = e.newest;
    const PluginCatalogVersionView* shown = &s.versions[e.first_version + (newest != kPluginIndexNoVersion ? newest : 0)];
    bank.map.detail_entry = index;
    out.emit(kPluginBrowserBackId, utf8_literal("\xE2\x86\x90 All plugins"), {}, UiSettingsControlKind::action, true, utf8_literal("Back"));
    u32 start = bank.used;
    index_text(bank, document, e.name);
    const Utf8View name = since(bank, start);
    start = bank.used;
    author_line(bank, document, e);
    add(bank, provider.name_bytes ? Utf8View{provider.name, provider.name_bytes} : host);
    out.row(kPluginBrowserHeaderId, name, since(bank, start), UiSettingsControlKind::category);
    u64 notice = kPluginBrowserNoticeFirst;
    const auto critical = [&](const Utf8View& text) noexcept {
        if (notice < kPluginBrowserNoticeFirst + 6)
            out.emit(notice++, text, {}, UiSettingsControlKind::notice, true, {}, UiSettingsVariant::critical);
    };
    if (custom) {
        start = bank.used;
        add(bank, "From "); add(bank, host);
        add(bank, ", an unofficial provider. MaiCord doesn't review these plugins. Plugins run with full access to your computer.");
        critical(since(bank, start));
    }
    if (provider.kind == PluginProviderKind::custom_unsigned)
        critical(utf8_literal("This provider doesn't sign its catalog. Anyone who controls its website can change what you install."));
    const bool broken = !revoked && local && (local->state_flags & kPluginStateRevokedBroken);
    if (revoked || broken) {
        start = bank.used;
        add(bank, revoked ? utf8_literal("Revoked by its provider") : utf8_literal("Known problem"));
        if (e.revocation_reason.bytes) {
            add(bank, ": ");
            index_text(bank, plugin_catalog_document(s, e.revocation_provider), e.revocation_reason);
        }
        critical(since(bank, start));
    } else if (local && (local->state_flags & kPluginStateWithdrawn)) {
        critical(utf8_literal("No longer maintained by its provider."));
    }
    if (newest == kPluginIndexNoVersion) {
        critical(needs_newer_host(s, e) ? utf8_literal("This plugin needs a newer version of MaiCord.")
                                        : utf8_literal("No version of this plugin works with this version of MaiCord."));
    } else if (local && (local->state_flags & kPluginStateIncompatibleHost)) {
        critical(utf8_literal("The installed version doesn't work with this version of MaiCord. Waiting for a compatible update."));
    }
    if (e.reserved) critical(utf8_literal("An official plugin uses this ID. MaiCord won't install it from this provider."));
    if (e.official_name) critical(utf8_literal("An official plugin has the same name. Make sure you trust this provider."));
    if (local && !e.bound) critical(utf8_literal("Installed from another source. Updates come from there."));
    // Actions by state.
    auto& map = bank.map;
    map.action_count = 0;
    const auto action = [&](PluginBrowserDetailAction kind, const Utf8View& label) noexcept {
        if (map.action_count < 6) { bank.detail[map.action_count] = label; map.actions[map.action_count++] = kind; }
    };
    const bool installable = provider.installable && newest != kPluginIndexNoVersion && !e.reserved;
    if (!local) {
        if (installable) action(PluginBrowserDetailAction::install, utf8_literal("Install"));
    } else if (e.bound) {
        if (e.update.outcome == PluginUpdateOutcome::available) {
            start = bank.used;
            add(bank, "Update to ");
            version(bank, s.versions[e.first_version + e.update.version].code);
            action(PluginBrowserDetailAction::update, since(bank, start));
        } else if (e.update.outcome == PluginUpdateOutcome::review) {
            action(PluginBrowserDetailAction::review, utf8_literal("Review update"));
        }
        if (!revoked) action(is_loaded ? PluginBrowserDetailAction::disable : PluginBrowserDetailAction::enable,
            is_loaded ? utf8_literal("Disable") : utf8_literal("Enable"));
        if (local->previous_code && !revoked) {
            start = bank.used;
            add(bank, "Roll back to ");
            version(bank, local->previous_code);
            action(PluginBrowserDetailAction::rollback, since(bank, start));
        }
        if (is_loaded && has_settings(facts, e.owner)) action(PluginBrowserDetailAction::settings, utf8_literal("Plugin settings"));
        action(PluginBrowserDetailAction::uninstall, utf8_literal("Uninstall"));
    }
    if (map.action_count) {
        auto& buttons = out.emit(kPluginBrowserActionsId, local ? utf8_literal("Installed") : utf8_literal("Get this plugin"), {},
            UiSettingsControlKind::buttons, !facts.busy);
        buttons.choices = {bank.detail, map.action_count};
    }
    start = bank.used;
    index_text(bank, document, e.summary);
    out.row(kPluginBrowserDescriptionId, since(bank, start), {}, UiSettingsControlKind::notice);
    out.row(kPluginBrowserPermissionsId, utf8_literal("Permissions"), {}, UiSettingsControlKind::category);
    u64 permission = kPluginBrowserPermissionFirst;
    for (u32 bit = 0; bit < kNativePluginPermissionCount; ++bit)
        if (shown->permissions & (1ull << bit))
            out.emit(permission++, native_plugin_permission_label(bit), {}, UiSettingsControlKind::notice, true, {}, UiSettingsVariant::standard,
                ui_settings_presentation_compact);
    if (!shown->permissions)
        out.row(permission, utf8_literal("Nothing beyond running its own code."), {}, UiSettingsControlKind::notice);
    if (e.disclosures) {
        start = bank.used;
        disclosures_text(bank, e.disclosures);
        out.emit(kPluginBrowserDisclosuresId, since(bank, start), {}, UiSettingsControlKind::notice, true, {}, UiSettingsVariant::critical);
    }
    if (shown->changelog.bytes) {
        start = bank.used;
        add(bank, "What's new in ");
        version(bank, shown->code);
        out.row(kPluginBrowserChangesId, since(bank, start), {}, UiSettingsControlKind::category);
        start = bank.used;
        index_text(bank, document, shown->changelog);
        out.row(kPluginBrowserChangelogId, since(bank, start), {}, UiSettingsControlKind::notice);
    }
    out.row(kPluginBrowserDetailsId, utf8_literal("Details"), {}, UiSettingsControlKind::category);
    u64 row = kPluginBrowserDetailFirst;
    const auto detail_row = [&](const Utf8View& label, u32 value_start) noexcept {
        out.emit(row++, label, since(bank, value_start), UiSettingsControlKind::notice, true, {}, UiSettingsVariant::standard,
            ui_settings_presentation_compact);
    };
    start = bank.used; version(bank, shown->code);
    if (local) { add(bank, " (installed: "); add(bank, {local->version, local->version_bytes}); add(bank, ")"); }
    detail_row(utf8_literal("Version"), start);
    start = bank.used; date(bank, shown->released); detail_row(utf8_literal("Released"), start);
    start = bank.used; bytes_text(bank, shown->bytes); detail_row(utf8_literal("Size"), start);
    start = bank.used; index_text(bank, document, e.license); detail_row(utf8_literal("License"), start);
    start = bank.used;
    for (u32 i = 0; i < 8; ++i) { const char pair[2]{ascii_hex_digit(shown->sha256[i] >> 4), ascii_hex_digit(shown->sha256[i])}; add(bank, {pair, 2}); }
    detail_row(utf8_literal("SHA-256"), start);
    if (e.homepage.bytes) {
        start = bank.used;
        index_text(bank, document, e.homepage);
        out.emit(kPluginBrowserHomepageId, utf8_literal("Homepage"), since(bank, start), UiSettingsControlKind::action, true,
            utf8_literal("Open"), UiSettingsVariant::standard, ui_settings_presentation_compact);
    }
    // Compatible older versions, behind a downgrade warning.
    map.version_count = 0;
    for (u32 v = 0; v < e.version_count && map.version_count < 4; ++v) {
        const auto& listed = s.versions[e.first_version + v];
        if (v == newest || listed.compatibility != PluginCompatibility::compatible || listed.revoked || listed.blocked) continue;
        if (local && local->installed_code == listed.code) continue;
        if (!map.version_count) out.row(kPluginBrowserVersionsId, utf8_literal("Other versions"), {}, UiSettingsControlKind::category);
        start = bank.used;
        version(bank, listed.code);
        const Utf8View label = since(bank, start);
        start = bank.used;
        add(bank, "Released "); date(bank, listed.released);
        out.emit(kPluginBrowserVersionFirst + map.version_count, label, since(bank, start), UiSettingsControlKind::action,
            installable && (!local || e.bound) && !facts.busy, utf8_literal("Install"), UiSettingsVariant::standard, ui_settings_presentation_compact);
        map.versions[map.version_count++] = v;
    }
    if (local && e.bound && !custom) {
        out.row(kPluginBrowserOptionsId, utf8_literal("Options"), {}, UiSettingsControlKind::category);
        auto& toggle = out.emit(kPluginBrowserAutoUpdateId, utf8_literal("Update automatically"),
            utf8_literal("Download new versions now and apply them the next time MaiCord starts."), UiSettingsControlKind::toggle, !facts.busy);
        toggle.checked = !(local->state_flags & kPluginStateAutoUpdateOff);
    }
}
} // namespace

// ---- installed, updates, providers ------------------------------------------------------------
namespace {
void installed_tab(Emitter& out, const PluginBrowserFacts& facts) noexcept {
    auto& bank = out.bank;
    const PluginCatalogSnapshot* s = facts.snapshot;
    for (u32 i = 0; i < facts.local.count; ++i) {
        const UiSettingsControl& source = facts.local[i];
        auto& control = out.emit(source.id, source.label, source.description, source.kind, source.enabled, source.action_label);
        control.checked = source.checked;
        control.choices = source.choices;
        control.selected_choice = source.selected_choice;
        const u64 id = source.id;
        if (id < kApplicationPluginCatalogActionFirst || id >= kApplicationPluginCatalogActionFirst + kNativePluginCatalogMaximumEntries ||
            !facts.inventory || id - kApplicationPluginCatalogActionFirst >= facts.inventory->count)
            continue;
        const u32 row = static_cast<u32>(id - kApplicationPluginCatalogActionFirst);
        const auto& entry = facts.inventory->entries[row];
        if (entry.status != NativePluginCatalogError::none) continue;
        const u64 owner = entry.manifest.owner_id;
        const u8 gate = facts.gate ? plugin_startup_gate_reason(*facts.gate, owner) : 0;
        const PluginCatalogInstalledView* local = installed_owner(s, owner);
        const bool revoked = (gate & kPluginGateRevoked) || (local && (local->state_flags & kPluginStateRevokedMalicious));
        if (revoked) {
            control.enabled = false;
            control.action_label = utf8_literal("Turned off");
            bank.presentation[out.count - 1].variant = UiSettingsVariant::critical;
        }
        // The visible window is eight consecutive inventory rows: one chip slot each.
        Utf8View* chips = bank.chips[row % kPluginBrowserRows];
        u32 n = 0;
        if (local && local->has_state && local->provider != kPluginProviderFile) {
            const PluginCatalogProviderView* provider = nullptr;
            for (u32 p = 0; s && p < s->provider_count; ++p) if (s->providers[p].id == local->provider) provider = &s->providers[p];
            if (provider) chips[n++] = provider_chip(bank, provider->kind, {provider->host, provider->host_bytes});
            else chips[n++] = local->provider == kPluginProviderOfficial ? add(bank, "Official")
                                                                         : add(bank, "Unofficial");
        } else if (s) {
            chips[n++] = add(bank, "Installed from file");
        }
        if (revoked) chips[n++] = add(bank, "Revoked");
        else if (gate & kPluginGateIncompatible || (local && (local->state_flags & kPluginStateIncompatibleHost)))
            chips[n++] = add(bank, "Incompatible");
        if (local && local->entry < (s ? s->entry_count : 0) && update_offered(s->entries[local->entry])) {
            const auto& e = s->entries[local->entry];
            const u32 start = bank.used;
            add(bank, "Update ");
            version(bank, s->versions[e.first_version + e.update.version].code);
            chips[n++] = since(bank, start);
        }
        if (n) control.choices = {chips, n};
    }
}
void updates_tab(Emitter& out, const PluginBrowserUiState& ui, const PluginBrowserFacts& facts) noexcept {
    auto& bank = out.bank;
    const PluginCatalogSnapshot* s = facts.snapshot;
    auto& map = bank.map;
    map.update_count = 0;
    map.update_all = 0;
    u32 offered = 0, ready = 0;
    for (u32 i = 0; s && i < s->entry_count; ++i)
        if (update_offered(s->entries[i])) { ++offered; ready += s->entries[i].update.outcome == PluginUpdateOutcome::available ? 1u : 0u; }
    map.update_all = ready;
    u32 start = bank.used;
    add(bank, "Update all");
    if (ready) { add(bank, " ("); number(bank, ready); add(bank, ")"); }
    bank.updates[0] = since(bank, start);
    bank.updates[1] = add(bank, facts.checking ? utf8_literal("Checking\xE2\x80\xA6") : utf8_literal("Check now"));
    start = bank.used;
    add(bank, "Last checked ");
    relative_time(bank, facts.now_unix, facts.checked_unix);
    add(bank, ".");
    auto& header = out.emit(kPluginBrowserUpdatesHeaderId, offered ? utf8_literal("Updates available") : utf8_literal("Your plugins are up to date"),
        since(bank, start), UiSettingsControlKind::buttons, !facts.busy);
    if (ready) header.choices = {bank.updates, 2};
    else header.choices = {bank.updates + 1, 1};
    auto& toggle = out.emit(kPluginBrowserUpdatesToggleId, utf8_literal("Update official plugins automatically"),
        utf8_literal("New versions download now and apply the next time MaiCord starts. New permissions always ask first."),
        UiSettingsControlKind::toggle, facts.configured && !facts.busy && !!s);
    toggle.checked = !s || !(s->official_flags & kPluginOfficialAutoUpdateOff);
    const u32 first = ui.updates_first < offered ? ui.updates_first / kPluginBrowserRows * kPluginBrowserRows : 0;
    u32 nth = 0;
    for (u32 i = 0; s && i < s->entry_count && map.update_count < kPluginBrowserRows; ++i) {
        const auto& e = s->entries[i];
        if (!update_offered(e) || nth++ < first) continue;
        const Utf8View document = plugin_catalog_document(*s, e.provider);
        start = bank.used;
        index_text(bank, document, e.name);
        const Utf8View name = since(bank, start);
        start = bank.used;
        const auto* local = installed(*s, e);
        if (local) add(bank, {local->version, local->version_bytes});
        add(bank, " \xE2\x86\x92 ");
        version(bank, s->versions[e.first_version + e.update.version].code);
        const bool review = e.update.outcome == PluginUpdateOutcome::review;
        const u32 slot = map.update_count;
        auto& control = out.emit(kPluginBrowserUpdateFirst + slot, name, since(bank, start), UiSettingsControlKind::action, !facts.busy,
            review ? utf8_literal("Review") : utf8_literal("Update"), review ? UiSettingsVariant::standard : UiSettingsVariant::primary,
            static_cast<u8>(ui_settings_presentation_avatar | ui_settings_presentation_compact));
        Utf8View* chips = bank.chips[slot];
        u32 n = 0;
        const auto& provider = provider_of(*s, e);
        chips[n++] = provider_chip(bank, provider.kind, {provider.host, provider.host_bytes});
        if (review) chips[n++] = add(bank, "New permissions");
        control.choices = {chips, n};
        map.updates[map.update_count++] = i;
    }
    if (offered > kPluginBrowserRows) {
        out.emit(kPluginBrowserUpdatesPreviousId, utf8_literal("Previous updates"), {}, UiSettingsControlKind::action, first != 0,
            utf8_literal("Previous"));
        out.emit(kPluginBrowserUpdatesNextId, utf8_literal("More updates"), {}, UiSettingsControlKind::action, offered - first > kPluginBrowserRows,
            utf8_literal("Next"));
    }
}
void fingerprint_short(PluginBrowserBank& bank, const u8* fingerprint) noexcept {
    char text[kPluginProviderFingerprintTextBytes];
    const u32 bytes = plugin_provider_fingerprint_text(fingerprint, {text, sizeof(text)});
    add(bank, {text, bytes >= 9 ? 9u : bytes});
    add(bank, " \xE2\x80\xA6");
}
void providers_tab(Emitter& out, const PluginBrowserFacts& facts) noexcept {
    auto& bank = out.bank;
    const PluginCatalogSnapshot* s = facts.snapshot;
    auto& map = bank.map;
    map.provider_count = 0;
    const PluginCatalogProviderView* official = s && s->provider_count && s->providers[0].kind == PluginProviderKind::official ? &s->providers[0] : nullptr;
    u32 start = bank.used;
    if (!facts.configured) add(bank, "Not configured for this build.");
    else {
        add(bank, official ? Utf8View{official->host, official->host_bytes} : utf8_literal("GitHub Pages"));
        add(bank, " \xC2\xB7 signed \xC2\xB7 checked ");
        relative_time(bank, facts.now_unix, facts.checked_unix);
    }
    auto& row = out.emit(kPluginBrowserOfficialId, utf8_literal("MaiCord plugins (official)"), since(bank, start), UiSettingsControlKind::toggle,
        facts.configured && !facts.busy && !!s);
    row.checked = facts.configured && (!s || !(s->official_flags & kPluginOfficialChecksOff));
    for (u32 p = 0; s && p < s->provider_count && map.provider_count < kPluginProviderMaximum; ++p) {
        const auto& provider = s->providers[p];
        if (provider.kind == PluginProviderKind::official) continue;
        const u32 slot = map.provider_count;
        const bool changed = provider.status == PluginCatalogStatus::key_changed;
        start = bank.used;
        add(bank, {provider.host, provider.host_bytes});
        if (provider.path_bytes > 1) add(bank, {provider.path, provider.path_bytes});
        if (changed) add(bank, " \xC2\xB7 This provider now uses a different signing key.");
        else if (provider.kind == PluginProviderKind::custom_signed) { add(bank, " \xC2\xB7 Signed, key "); fingerprint_short(bank, provider.fingerprint); }
        else add(bank, " \xC2\xB7 Not signed");
        add(bank, " \xC2\xB7 Unofficial");
        const Utf8View description = since(bank, start);
        PluginBrowserProviderAction* actions = map.provider_actions[slot];
        Utf8View* labels = bank.providers[slot];
        u32 n = 0;
        const auto button = [&](PluginBrowserProviderAction kind, const Utf8View& label) noexcept { actions[n] = kind; labels[n++] = label; };
        if (changed) {
            button(PluginBrowserProviderAction::remove, utf8_literal("Remove"));
            button(PluginBrowserProviderAction::trust_key, utf8_literal("Trust new key"));
        } else {
            button(PluginBrowserProviderAction::refresh, utf8_literal("Refresh"));
            if (provider.kind == PluginProviderKind::custom_signed)
                button(PluginBrowserProviderAction::checks, provider.check_daily ? utf8_literal("Turn off checks") : utf8_literal("Turn on checks"));
            button(PluginBrowserProviderAction::remove, utf8_literal("Remove"));
        }
        auto& control = out.emit(kPluginBrowserProviderFirst + slot,
            provider.name_bytes ? Utf8View{provider.name, provider.name_bytes} : Utf8View{provider.host, provider.host_bytes}, description,
            UiSettingsControlKind::buttons, !facts.busy, {}, changed ? UiSettingsVariant::critical : UiSettingsVariant::standard);
        control.choices = {labels, n};
        map.providers[slot] = p;
        ++map.provider_count;
    }
    out.emit(kPluginBrowserAddProviderId, utf8_literal("Add provider"),
        utf8_literal("Plugins from other providers aren't reviewed by MaiCord. Only add providers you trust."), UiSettingsControlKind::action,
        !facts.busy && map.provider_count < kPluginProviderMaximum, utf8_literal("Add provider"));
}
} // namespace

u32 plugin_browser_project(const PluginBrowserUiState& ui, const PluginBrowserFacts& facts, PluginBrowserBank* bank) noexcept {
    if (!bank) return 0;
    bank->used = 0;
    reset_value(&bank->map);
    bank->map.detail_entry = ~0u;
    Emitter out{*bank, 0};
    tabs(out, ui, facts);
    notices(out, ui, facts);
    switch (ui.tab) {
    case PluginBrowserTab::browse: {
        const u32 entry = ui.detail ? plugin_browser_find(facts.snapshot, ui.detail_provider, ui.detail_owner) : ~0u;
        if (entry != ~0u) detail(out, facts, entry);
        else browse(out, ui, facts);
        break;
    }
    case PluginBrowserTab::installed: installed_tab(out, facts); break;
    case PluginBrowserTab::updates: updates_tab(out, ui, facts); break;
    case PluginBrowserTab::providers: providers_tab(out, facts); break;
    }
    return out.count;
}

// ---- dialog texts -----------------------------------------------------------------------------
namespace {
struct DialogText {
    PluginBrowserDialog& d;
    void put(const char* data, u32 bytes) noexcept { append_utf8(d.text, d.used, kPluginBrowserDialogBytes, data, bytes); }
    Utf8View add(const Utf8View& text) noexcept {
        const u32 start = d.used;
        put(text.data, text.bytes);
        return {d.text + start, d.used - start};
    }
    template <usize Count> __forceinline Utf8View add(const char (&text)[Count]) noexcept {
        const u32 start = d.used;
        put(text, Count - 1);
        return {d.text + start, d.used - start};
    }
    void version(u64 code) noexcept { char t[32]; put(t, update_version_text(code, {t, sizeof(t)})); }
    void number(u64 value) noexcept { char t[20]; put(t, decimal_write(t, value)); }
    Utf8View since(u32 start) const noexcept { return {d.text + start, d.used - start}; }
    void fingerprint(const u8* fp) noexcept {
        char t[kPluginProviderFingerprintTextBytes];
        put(t, plugin_provider_fingerprint_text(fp, {t, sizeof(t)}));
    }
    void permissions(u64 mask, const Utf8View& prefix) noexcept {
        for (u32 bit = 0; bit < kNativePluginPermissionCount; ++bit)
            if (mask & (1ull << bit)) { add("\n"); add(prefix); add(native_plugin_permission_label(bit)); }
    }
};
inline constexpr Utf8View kBullet = utf8_literal("\xE2\x80\xA2 ");
} // namespace

// "Install <name>?": provider line, publisher, permissions, disclosures, the
// full-access warning; unofficial plugins use the danger fill.
void plugin_browser_install_dialog(const PluginBrowserInstallFacts& f, PluginBrowserDialog* d) noexcept {
    reset_value(d);
    DialogText t{*d};
    const bool custom = f.kind != PluginProviderKind::official;
    u32 start = d->used;
    if (f.update_from) {
        t.add("Update "); t.add(f.name); t.add(" to "); t.version(f.version_code); t.add("?");
    } else {
        t.add(f.downgrade_from ? utf8_literal("Install an older version of ") : utf8_literal("Install "));
        t.add(f.name);
        t.add("?");
    }
    d->title = t.since(start);
    start = d->used;
    if (custom) { t.add("From "); t.add(f.host); t.add(" (unofficial)."); }
    else { t.add("From "); t.add(f.provider_name.bytes ? f.provider_name : utf8_literal("MaiCord plugins")); t.add(" (official)."); }
    t.add(" Version "); t.version(f.version_code); t.add(".");
    if (f.publisher.bytes) { t.add("\nPublished by "); t.add(f.publisher); t.add("."); }
    if (f.downgrade_from) {
        t.add("\nThis replaces version "); t.version(f.downgrade_from);
        t.add(". Older versions can lack fixes, and newer versions won't install automatically until you update again.");
    }
    t.add("\nThis plugin will be able to:");
    t.permissions(f.permissions, kBullet);
    if (!f.permissions) t.add("\nNothing beyond running its own code.");
    if (f.disclosures) {
        t.add("\nAlso uses (outside MaiCord's permissions):");
        for (u32 bit = 0; bit < kPluginIndexDisclosureCount; ++bit)
            if (f.disclosures & (1u << bit)) { t.add("\n"); t.add(kBullet); t.add(disclosure_label(bit)); }
    }
    if (custom) t.add("\nMaiCord doesn't review plugins from unofficial providers.");
    if (f.kind == PluginProviderKind::custom_unsigned)
        t.add("\nThis provider doesn't sign its catalog. Anyone who controls it can change what you install.");
    t.add("\nPlugins run with full access to your computer. Only install plugins you trust.");
    d->context = t.since(start);
    if (f.update_from || f.downgrade_from) {
        d->accept = custom ? utf8_literal("Update unofficial plugin") : utf8_literal("Install this version");
    } else {
        d->accept = custom ? utf8_literal("Install unofficial plugin") : utf8_literal("Install and enable");
        d->option = utf8_literal("Enable after installing");
        d->option_checked = true;
    }
    d->danger = custom || f.downgrade_from;
}
void plugin_browser_review_dialog(Utf8View name, u64 version_code, const PluginPermissionDiff& diff, PluginBrowserDialog* d) noexcept {
    reset_value(d);
    DialogText t{*d};
    u32 start = d->used;
    t.add("Update "); t.add(name); t.add(" to "); t.version(version_code); t.add("?");
    d->title = t.since(start);
    start = d->used;
    t.add("This version asks for more permissions.");
    t.permissions(diff.added, utf8_literal("New: "));
    if (diff.kept) { t.add("\nStill allowed:"); t.permissions(diff.kept, kBullet); }
    if (diff.removed) { t.add("\nNo longer needed:"); t.permissions(diff.removed, kBullet); }
    t.add("\nPlugins run with full access to your computer.");
    d->context = t.since(start);
    d->accept = utf8_literal("Allow and update");
    d->cancel = utf8_literal("Not now");
    d->option = utf8_literal("Skip this version instead");
}
void plugin_browser_provider_dialog(const PluginBrowserProviderFacts& f, PluginBrowserDialog* d) noexcept {
    reset_value(d);
    DialogText t{*d};
    const bool key_change = f.old_fingerprint != nullptr;
    u32 start = d->used;
    if (key_change) { t.add("Trust the new key of "); t.add(f.host); t.add("?"); }
    else t.add("Add an unofficial plugin provider?");
    d->title = t.since(start);
    start = d->used;
    if (key_change) {
        t.add("This provider now signs its catalog with a different key. If you didn't expect this, someone else may control it.");
    } else {
        t.add("MaiCord doesn't review plugins from "); t.add(f.host);
        t.add(". Plugins are programs that run with full access to your computer, your files and your Discord account in MaiCord. Only add providers you trust.");
    }
    t.add("\nProvider: ");
    t.add(f.name.bytes ? f.name : f.host);
    t.add(" \xC2\xB7 "); t.number(f.plugin_count); t.add(f.plugin_count == 1 ? utf8_literal(" plugin") : utf8_literal(" plugins"));
    t.add("\nAddress: https://"); t.add(f.host); t.add(f.path);
    if (key_change && f.old_fingerprint) { t.add("\nOld key: "); t.fingerprint(f.old_fingerprint); }
    if (f.signed_provider && f.fingerprint) {
        t.add(key_change ? utf8_literal("\nNew key: ") : utf8_literal("\nSigning key: "));
        t.fingerprint(f.fingerprint);
        if (f.pin_matched) t.add(" (the key matches the link you were given)");
    } else {
        t.add("\nThis provider doesn't sign its catalog. Anyone who controls ");
        t.add(f.host);
        t.add(" can change what you install.");
    }
    d->context = t.since(start);
    d->accept = key_change ? utf8_literal("Trust new key") : utf8_literal("Add provider");
    d->option = utf8_literal("I trust this provider and understand MaiCord doesn't review its plugins.");
    d->option_required = true;
    d->danger = true;
}
void plugin_browser_remove_provider_dialog(Utf8View name, u32 installed, PluginBrowserDialog* d) noexcept {
    reset_value(d);
    DialogText t{*d};
    u32 start = d->used;
    t.add("Remove "); t.add(name); t.add("?");
    d->title = t.since(start);
    d->context = t.add("Plugins you installed from it stay installed but won't get updates.");
    if (installed) {
        start = d->used;
        t.add("Also uninstall its "); t.number(installed); t.add(installed == 1 ? utf8_literal(" plugin") : utf8_literal(" plugins"));
        d->option = t.since(start);
    }
    d->accept = utf8_literal("Remove");
    d->danger = true;
}
void plugin_browser_uninstall_dialog(Utf8View name, PluginBrowserDialog* d) noexcept {
    reset_value(d);
    DialogText t{*d};
    const u32 start = d->used;
    t.add("Uninstall "); t.add(name); t.add("?");
    d->title = t.since(start);
    d->context = t.add("MaiCord turns the plugin off and deletes its files. Its saved data stays unless you delete it too.");
    d->option = utf8_literal("Also delete its saved data");
    d->accept = utf8_literal("Uninstall");
    d->danger = true;
}
void plugin_browser_rollback_dialog(Utf8View name, u64 to_code, u64 from_code, PluginBrowserDialog* d) noexcept {
    reset_value(d);
    DialogText t{*d};
    u32 start = d->used;
    t.add("Roll back "); t.add(name); t.add(" to "); t.version(to_code); t.add("?");
    d->title = t.since(start);
    start = d->used;
    t.add("Version "); t.version(from_code);
    t.add(" won't be offered again until a newer version comes out. Older versions can lack fixes.");
    d->context = t.since(start);
    d->accept = utf8_literal("Roll back");
}

const PluginCatalogInstalledView* plugin_browser_installed(const PluginCatalogSnapshot* snapshot, u64 owner) noexcept {
    return installed_owner(snapshot, owner);
}
void plugin_browser_update_all_dialog(const PluginCatalogSnapshot& s, ConstSlice<u32> entries, PluginBrowserDialog* d) noexcept {
    reset_value(d);
    DialogText t{*d};
    u32 start = d->used;
    t.add("Update "); t.number(entries.count); t.add(entries.count == 1 ? utf8_literal(" plugin?") : utf8_literal(" plugins?"));
    d->title = t.since(start);
    start = d->used;
    t.add("These come from unofficial providers that MaiCord doesn't review:");
    for (u32 i = 0; i < entries.count; ++i) {
        if (entries[i] >= s.entry_count) continue;
        const auto& e = s.entries[entries[i]];
        const auto& provider = s.providers[e.provider];
        if (provider.kind == PluginProviderKind::official) continue;
        char name[kNativePluginNameCapacity];
        const auto decoded = plugin_index_text_decode(plugin_catalog_document(s, e.provider), e.name, {name, sizeof(name)});
        t.add("\n\xE2\x80\xA2 "); t.add({name, decoded.ok() ? decoded.bytes : 0});
        t.add(" ("); t.add({provider.host, provider.host_bytes}); t.add(")");
    }
    d->context = t.since(start);
    d->accept = utf8_literal("Update all");
    d->danger = true;
}

} // namespace maicord::app_detail
