#include "../support/ui_scene_fixtures.hpp"
#include "probe_present.hpp"
#include "settings/catalog.hpp"
#include "update/updates.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/render/d3d11/renderer_presentation.hpp"
#include "maicord/render/d3d11/layer_presentation.hpp"
#include "maicord/render/text/form_surface.hpp"
#include "maicord/ui/shell/popup_scene.hpp"
#include "maicord/ui/widgets/settings_choice.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/widgets/kit.hpp"
#include "maicord/ui/text/measure_cache.hpp"
#include "maicord/ui/core/scene.hpp"
#include "maicord/ui/text/form.hpp"
#include "maicord/core/log.hpp"
#include "maicord/app/settings/client_preferences.hpp"

namespace maicord {
namespace app_detail {
void application_settings_refresh_pages() noexcept {}
} // namespace app_detail
namespace {
// The real catalog pages and sidebar with fixture values, projected the way
// the settings host shows them before live values arrive, plus a gallery of
// control states. Only production scene, layout, paint and text paths run;
// no host action is dispatched and no account is involved.
constexpr u32 kRows = 32;
constexpr UiPageKey kGalleryPage{2, 1}, kShowcasePage{2, 2};
constexpr u32 kShowcaseIndex = kSettingsPanelCount + 1;
struct Fixture {
    UiPage pages[kSettingsPanelCount + 2];
    UiSettingsPageContent contents[kSettingsPanelCount + 2];
    UiSettingsControl controls[kSettingsPanelCount + 2][kRows];
    UiSettingsControlPresentation looks[kSettingsPanelCount + 2][kRows];
    u16 update_meters[kRows], showcase_meters[kRows];
    UiSettingsEditor editors[2];
    UiEditGeometry geometries[2];
    UiPageRegistry registry;
    UiSettingsScene scene;
    TextFormScene form;
    TextFormSurface surface;
};
Fixture fixture;
UiScene g_shell;

constexpr Utf8View kDevices[]{utf8_literal("Default"), utf8_literal("Headset (Synthetic Audio)")};
constexpr Utf8View kGames[]{utf8_literal("Example Game")};
constexpr Utf8View kThemes[]{utf8_literal("Light"), utf8_literal("Dark"), utf8_literal("AMOLED")};
// Appearance's "Your themes": two installed themes (the edited copy in use) and the Browse themes tile.
constexpr Utf8View kYourThemes[]{utf8_literal("Sample Neon Grid (edited)"), utf8_literal("Fjord"), utf8_literal("Browse")};
constexpr u32 kYourThemeStops[][2]{{0x140b24, 0xff3ea5}, {0x2e3440, 0x88c0d0}};
constexpr Utf8View kSpacing[]{utf8_literal("0px"), utf8_literal("4px"), utf8_literal("8px"), utf8_literal("16px"),
                              utf8_literal("24px")};
constexpr Utf8View kDisplay[]{utf8_literal("Default"), utf8_literal("Compact")};
constexpr Utf8View kSpoilers[]{utf8_literal("On click"), utf8_literal("Always"), utf8_literal("On servers I moderate")};
constexpr Utf8View kAccess[]{utf8_literal("Deny"), utf8_literal("Inherit"), utf8_literal("Allow")};
constexpr Utf8View kRoleColors[]{utf8_literal("Blue"), utf8_literal("Green"), utf8_literal("Gold"),
                                 utf8_literal("Red"), utf8_literal("Pink"), utf8_literal("Gray")};
constexpr u32 kRoleRgb[]{0x3498DB, 0x2ECC71, 0xF1C40F, 0xE74C3C, 0xE91E63, 0x95A5A6};
// Accessibility's preview card: Discord's example messages under a role-coloured name, cozy, with reactions.
constexpr Utf8View kPreviewTexts[]{utf8_literal("what happened to all the beans"),
                                   utf8_literal("here\xE2\x80\x99s a link "), utf8_literal("https://discord.com/accessibility")};
constexpr UiSettingsPreview kPreview{utf8_literal("Wumpus"), utf8_literal("Today at 4:20 PM"), 0xdd80f4, 0, false, true,
                                     true, 16};

// Theme swatches resolve as the settings host does, under default
// preferences (the dark theme the probe renders); the gallery's Theme row is
// the base theme palette and its colour grid shows role colours.
constexpr ClientPreferences kPreferences = client_preferences_defaults();
SettingBinding binding_of(u64 id) noexcept {
    UiSettingsSchemaLocation location{};
    const auto* node = settings_control_location(id, &location) ? settings_catalog_node(location) : nullptr;
    return node ? static_cast<SettingBinding>(node->binding()) : SettingBinding::none;
}
bool palette(void*, u64 id, u32 choice, BackgroundTheme* output) noexcept {
    if (id == 1000 + 17) {
        if (choice >= 6)
            return false;
        *output = background_theme_defaults();
        output->color_count = 2;
        output->colors[0] = output->colors[1] = kRoleRgb[choice];
        return true;
    }
    if (binding_of(id) == SettingBinding::installed_themes) {
        if (choice >= 2)
            return false;
        *output = background_theme_defaults();
        output->color_count = 2;
        output->colors[0] = kYourThemeStops[choice][0];
        output->colors[1] = kYourThemeStops[choice][1];
        output->colors[2] = 0;
        return true;
    }
    return settings_theme_swatch(kPreferences, id == 1000 + 15 ? SettingBinding::theme_base : binding_of(id), choice,
                                 output);
}

// One catalog panel with every searchable row at a representative value.
u32 project_panel(u32 panel, UiSettingsControl* out, UiSettingsControlPresentation* looks) noexcept {
    const auto& nodes = settings_catalog_panel(static_cast<SettingsPanel>(panel)).nodes;
    u32 count = 0;
    for (u32 i = 0; i < nodes.count && count < kRows; ++i) {
        const auto& n = nodes[i];
        if ((n.flags & ui_settings_schema_search_hidden) ||
            (n.kind == UiSettingsControlKind::category && (n.flags & ui_settings_schema_dynamic)))
            continue;
        auto& c = out[count];
        c = {settings_control_id(panel, i), n.label(), n.description(), n.kind, true, (i % 3) != 1, n.choices(),
             n.default_choice() < n.choices().count ? n.default_choice() : 0, n.action_label(), nullptr};
        if (n.kind == UiSettingsControlKind::choice && !c.choices.count)
            c.choices = n.binding() == static_cast<u32>(SettingBinding::add_game) ? ConstSlice<Utf8View>{kGames, 1}
                                                                                  : ConstSlice<Utf8View>{kDevices, 2};
        if (n.kind == UiSettingsControlKind::palette && !c.choices.count)
            c.choices = n.binding() == static_cast<u32>(SettingBinding::installed_themes) ? ConstSlice<Utf8View>{kYourThemes, 3}
                                                                                         : ConstSlice<Utf8View>{kThemes, 3};
        if (n.kind == UiSettingsControlKind::slider && c.choices.count > 120)
            c.selected_choice = c.choices.count * 2 / 3;
        if (n.kind == UiSettingsControlKind::category && n.binding())
            continue; // list headings are followed by host rows only
        if (n.kind == UiSettingsControlKind::preview)
            c.choices = {kPreviewTexts, 3}, c.action_label = utf8_literal("Example Button");
        looks[count] = {static_cast<UiSettingsVariant>(n.variant),
                        static_cast<u8>(n.flags & ui_settings_schema_marks ? ui_settings_presentation_marks : 0),
                        n.default_choice() < kUiSettingsNoDefault ? static_cast<u16>(n.default_choice())
                                                                  : kUiSettingsNoDefault};
        if (n.binding() == static_cast<u32>(SettingBinding::group_spacing))
            c.selected_choice = 2, looks[count].default_choice = 3;
        SettingValue value{};
        if (n.kind == UiSettingsControlKind::palette &&
            settings_store_read(kPreferences, static_cast<SettingBinding>(n.binding()), &value))
            c.selected_choice = value.choice;
        // The edited copy is in use: it is checked, and no built-in tile is. The arrow marks the Browse tile.
        if (n.binding() == static_cast<u32>(SettingBinding::installed_themes))
            c.selected_choice = 0, looks[count].icon_plus_one = static_cast<u8>(UiIcon::arrow_right) + 1;
        else if (n.binding() == static_cast<u32>(SettingBinding::theme_base) ||
                 n.binding() == static_cast<u32>(SettingBinding::theme_color))
            c.selected_choice = kUiSettingsNoChoice;
        ++count;
    }
    return count;
}

// The Updates page as application_updates projects it, one state at a time,
// through the real catalog nodes and the production projection functions.
enum class UpdatesState : u32 { idle, checking, ready, preview_downloading, error, long_build, count };
struct UpdatesFixture {
    app_detail::ApplicationUpdatesView view;
    app_detail::ApplicationUpdatesText text[8];
    app_detail::ApplicationUpdatesChangelogRows rows;
    UpdateChangelog log;
};
UpdatesFixture g_updates;
void add_entry(UpdateChangelog& log, u64 version, u64 date, const char* text) noexcept {
    auto& e = log.entries[log.count++];
    e.version = version;
    e.date_unix_seconds = date;
    e.text_offset = log.text_bytes;
    e.text_bytes = c_string_length(text);
    copy_bytes(log.text + log.text_bytes, text, e.text_bytes);
    log.text_bytes += e.text_bytes;
}
// Release notes of this probe's own words, with a heading, nested bullets, bold and code.
constexpr char kReleaseNotes[] =
    "This update installs from Settings > Updates; nothing runs until you restart.\n\n"
    "### Highlights\n\n"
    "- **Much lighter on the GPU.**\n"
    "  - Animated backgrounds cost far less GPU time, so an idle window stays quiet.\n"
    "  - The emoji picker follows a dragged scrollbar at full speed, and `Ctrl+F` finds settings faster.\n"
    "- A calmer Updates page with one status card.\n\n"
    "### Fixes\n\n"
    "- Fix a crash when the gateway drops during a voice call.\n"
    "- Keep the scroll position when a channel reloads.\n";
u32 project_updates(UpdatesState state, UiSettingsControl* out, UiSettingsControlPresentation* looks, u16* meters) noexcept {
    auto& u = g_updates;
    auto& v = u.view;
    const u64 now = clock_unix_milliseconds() / 1000;
    using S = app_detail::ApplicationUpdateState;
    v = {};
    v.interval = UpdateCheckInterval::hour;
    v.checked_unix = now - 120;
    v.changelog_revision = 1;
    u.log = {};
    if (state == UpdatesState::preview_downloading) {
        v.state = S::downloading;
        v.channel = UpdateChannel::preview;
        v.own_version = update_version_number(0, 3, 4, 12);
        copy_bytes(v.own_commit, "4d1c9a2", 7);
        v.offer_version = update_version_number(0, 3, 4, 17);
        copy_bytes(v.offer_commit, "9be03f1", 7);
        v.offer_bytes = 8598323;
        v.offer_date = now - 7200;
        v.total = v.offer_bytes;
        v.received = v.offer_bytes / 100 * 62;
        constexpr const char* subjects[]{"Show the release date and size of an available update",
                                         "Keep scroll position when a channel reloads",
                                         "Fix the emoji picker losing focus after a search",
                                         "Draw thread previews with their author colour",
                                         "Halve memory used by cached member lists",
                                         "Reconnect voice after the network changes"};
        u.log.channel = UpdateChannel::preview;
        u.log.version = v.offer_version;
        u.log.base = update_version_number(0, 3, 3, 0);
        for (u32 i = 0; i < 6; ++i)
            add_entry(u.log, v.offer_version - i, now - 7200 - i * 43200, subjects[i]);
    } else {
        v.channel = UpdateChannel::stable;
        v.own_version = update_version_number(0, 5, state == UpdatesState::ready ? 3 : 4, 0);
        if (state == UpdatesState::long_build) {
            // The owner's line: a dev build with its commit wraps in a narrow card.
            v.own_version = update_version_number(0, 5, 5, 9);
            copy_bytes(v.own_commit, "de4eb0a", 7);
        }
        u.log.channel = UpdateChannel::stable;
        u.log.version = update_version_number(0, 5, 4, 0);
        u.log.base = update_version_number(0, 4, 0, 0);
        for (u32 i = 0; i < 5; ++i)
            add_entry(u.log, update_version_number(0, 5, 4 - i, 0), now - 86400 * (2 + i), i ? "- Smaller fixes and polish." : kReleaseNotes);
        v.state = state == UpdatesState::checking ? S::checking : state == UpdatesState::ready ? S::ready
                  : state == UpdatesState::error ? S::error : S::up_to_date;
        if (state == UpdatesState::ready) {
            v.offer_version = v.ready_version = u.log.version;
            v.offer_bytes = 6710886;
            v.offer_date = now - 86400 * 2;
        }
        v.error = UpdateServiceError::transport;
    }
    clear_bytes(&u.rows, sizeof(u.rows));
    app_detail::application_updates_changelog_build(v, u.log, &u.rows);
    const u32 panel = static_cast<u32>(SettingsPanel::updates);
    const auto& nodes = settings_catalog_panel(SettingsPanel::updates).nodes;
    u32 count = 0;
    for (u32 i = 0; i < nodes.count && count < kRows; ++i) {
        const auto& n = nodes[i];
        auto& c = out[count];
        c = {};
        c.id = settings_control_id(panel, i);
        c.label = n.label(); c.description = n.description(); c.kind = n.kind; c.enabled = true;
        c.choices = n.choices(); c.action_label = n.action_label();
        looks[count] = {};
        looks[count].variant = static_cast<UiSettingsVariant>(n.variant);
        looks[count].default_choice = kUiSettingsNoDefault;
        meters[count] = kUiSettingsNoMeter;
        const auto binding = static_cast<SettingBinding>(n.binding());
        if (binding == SettingBinding::update_changelog) {
            if (!app_detail::application_updates_changelog_heading(u.rows, &c))
                continue;
            ++count;
            count += app_detail::application_updates_changelog_project(u.rows, {out + count, kRows - count},
                                                                       {looks + count, kRows - count});
            continue;
        }
        if (binding == SettingBinding::update_release_notes) {
            if (u.rows.count)
                ++count;
            continue;
        }
        if (binding == SettingBinding::none || app_detail::application_updates_project_view(v, now, binding, &u.text[i % 8], &c,
                                                                                            &looks[count], &meters[count]))
            ++count;
    }
    // Rows the changelog added have no meter.
    for (u32 i = 0; i < count; ++i)
        if (out[i].kind != UiSettingsControlKind::status)
            meters[i] = kUiSettingsNoMeter;
    return count;
}

// Control states: focus, disabled, error, value labels, marks and swatches.
u32 build_gallery(Fixture& f) noexcept {
    auto* c = f.controls[kSettingsPanelCount];
    auto* p = f.looks[kSettingsPanelCount];
    using K = UiSettingsControlKind;
    f.editors[0] = {{utf8_literal("A display name well past the limit"), 2, 9, true, true, true}, nullptr,
                    utf8_literal("Display Name")};
    f.editors[1] = {{{}, 0, 0, true, false, false}, nullptr, utf8_literal("Tell everyone about yourself")};
    u32 n = 0;
    const auto add = [&](UiSettingsControl control, UiSettingsControlPresentation look = {}) noexcept {
        control.id = 1000 + n;
        c[n] = control;
        p[n++] = look.default_choice || look.flags || look.variant != UiSettingsVariant::standard
                     ? look
                     : UiSettingsControlPresentation{UiSettingsVariant::standard, 0, kUiSettingsNoDefault};
    };
    add({0, utf8_literal("Switches"), {}, K::category, true, false});
    add({0, utf8_literal("Focused switch"), utf8_literal("Keyboard focus rings the track concentrically."), K::toggle,
         true, true});
    add({0, utf8_literal("Off"), {}, K::toggle, true, false});
    add({0, utf8_literal("Disabled"), utf8_literal("Disabled controls are drawn at half opacity."), K::toggle, false,
         true});
    add({0, utf8_literal("Sliders"), {}, K::category, true, false});
    add({0, utf8_literal("Speaker Volume"), {}, K::slider, true, false,
         settings_catalog_panel(SettingsPanel::voice).nodes[4].choices(), 140});
    add({0, utf8_literal("Space Between Message Groups"), utf8_literal("The default mark is highlighted."), K::slider,
         true, false, {kSpacing, 5}, 2},
        {UiSettingsVariant::standard, ui_settings_presentation_marks, 3});
    add({0, utf8_literal("Fields"), {}, K::category, true, false});
    add({0, utf8_literal("Display Name"), {}, K::edit, true, false, {}, 0, {}, &f.editors[0]});
    add({0, utf8_literal("Display names can be at most 32 characters."), {}, K::error, true, false});
    add({0, utf8_literal("Show spoiler content"), {}, K::choice, true, false, {kSpoilers, 3}, 0});
    add({0, utf8_literal("Bio"), {}, K::multiline, true, false, {}, 0, {}, &f.editors[1]});
    add({0, utf8_literal("Choices"), {}, K::category, true, false});
    add({0, utf8_literal("Chat Message Display"), {}, K::radio, true, false, {kDisplay, 2}, 0});
    add({0, utf8_literal("Send Messages"), {}, K::segments, true, false, {kAccess, 3}, 2},
        {UiSettingsVariant::standard, ui_settings_presentation_tristate, kUiSettingsNoDefault});
    add({0, utf8_literal("Theme"), {}, K::palette, true, false, {kThemes, 3}, 1});
    add({0, utf8_literal("Colors"), {}, K::category, true, false});
    add({0, utf8_literal("Role color"), {}, K::swatches, true, false, {kRoleColors, 6}, 2});
    add({0, utf8_literal("Couldn't save settings"), utf8_literal("Your latest changes are applied but not saved yet."),
         K::notice, true, false, {}, 0, utf8_literal("Try again")},
        {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    return n;
}

// The shell's new rows: trailing dropdown, slider and segmented controls, the status card with its meter and
// chips, a changelog-style disclosure list, markdown notes with their link, a footnote and an empty state.
constexpr Utf8View kIntervals[]{utf8_literal("Every hour"), utf8_literal("Every 4 hours"), utf8_literal("Every 12 hours"),
                                utf8_literal("Once a day")};
constexpr Utf8View kChannels[]{utf8_literal("Stable"), utf8_literal("Preview")};
constexpr Utf8View kChips[]{utf8_literal("Stable release"), utf8_literal("Signature verified")};
constexpr Utf8View kNotes = utf8_literal(
    "### What's new\n- Rebuilt the settings window with cards and a pinned footer\n- Added a **Browse themes** link to Appearance\n"
    "- Fixed `Ctrl F` focusing the wrong field\n  - nested notes keep their level\n- Search results group by page\n"
    "- Segmented controls replace radio groups\n- Sliders show their value\n- Nested lists hang under their item and use a dash\n"
    "  - a second level reads quieter than the first\n- Long notes fade out above their Show all link\n- Links ask before they open\n");
u32 build_showcase(Fixture& f) noexcept {
    auto* c = f.controls[kShowcaseIndex];
    auto* p = f.looks[kShowcaseIndex];
    auto* meters = f.showcase_meters;
    using K = UiSettingsControlKind;
    u32 n = 0;
    const auto add = [&](UiSettingsControl control, UiSettingsControlPresentation look = {}, u16 meter = kUiSettingsNoMeter) noexcept {
        control.id = 2000 + n;
        c[n] = control;
        meters[n] = meter;
        p[n++] = look.default_choice || look.flags || look.chip_tones || look.icon_plus_one || look.more || look.variant != UiSettingsVariant::standard
                     ? look
                     : UiSettingsControlPresentation{UiSettingsVariant::standard, 0, kUiSettingsNoDefault};
    };
    UiSettingsControl status{0, utf8_literal("You're up to date"), utf8_literal("MaiCord 0.5.4 \xC2\xB7 Stable channel"), K::status, true, false,
                             {kChips, 2}, 0, utf8_literal("Check now")};
    status.detail = utf8_literal("Last checked 4 minutes ago");
    UiSettingsControlPresentation status_look{UiSettingsVariant::standard, 0, kUiSettingsNoDefault};
    status_look.more = ui_settings_more_meta_parts;
    status_look.chip_tones = ui_settings_chip_positive | (ui_settings_chip_positive << 2);
    status_look.icon_plus_one = static_cast<u8>(UiIcon::circle_check) + 1;
    add(status, status_look, 1000);
    add({0, utf8_literal("Update settings"), {}, K::category, true, false});
    add({0, utf8_literal("Notify me about updates"), utf8_literal("Tell me when a new version is ready."), K::toggle, true, true});
    add({0, utf8_literal("Check for updates"), {}, K::choice, true, false, {kIntervals, 4}, 1});
    add({0, utf8_literal("Release channel"), utf8_literal("Preview builds come first and can be less stable."), K::segments, true, false,
         {kChannels, 2}, 0});
    add({0, utf8_literal("Speaker Volume"), {}, K::slider, true, false,
         settings_catalog_panel(SettingsPanel::voice).nodes[4].choices(), 140});
    add({0, utf8_literal("Release notes"), {}, K::category, true, false});
    UiSettingsControl latest{0, utf8_literal("MaiCord 0.5.4"), utf8_literal("Nov 1"), K::disclosure, true, true};
    latest.detail = utf8_literal("Current");
    UiSettingsControlPresentation latest_look{UiSettingsVariant::positive, 0, kUiSettingsNoDefault};
    add(latest, latest_look);
    UiSettingsControl notes{0, {}, kNotes, K::markdown, true, false};
    notes.detail = utf8_literal("Show all notes");
    add(notes);
    UiSettingsControl older{0, utf8_literal("MaiCord 0.5.3"), utf8_literal("Oct 30"), K::disclosure, true, false};
    older.detail = utf8_literal("dev.38");
    add(older);
    add({0, utf8_literal("Updates replace the app; your settings stay."), {}, K::footnote, true, false, {}, 0, utf8_literal("Releases")});
    return n;
}

bool show(Fixture& f, D3D11Renderer& renderer, UiPageKey page, UiRect viewport, const wchar_t* name,
          bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!ui_page_registry_select(&f.registry, page).ok() || !ui_settings_scene_update(&f.scene, &f.registry, viewport) ||
        !f.scene.content.controls.count)
        return false;
    // Every materialized control has visible geometry inside the panel.
    u32 placed = 0;
    for (u32 i = 0; i < f.scene.content.controls.count; ++i) {
        const UiSettingsTarget target{UiSettingsTargetKind::control, page, f.scene.content.controls[i].id};
        UiSettingsControlGeometry geometry{};
        if (ui_settings_scene_control_geometry(f.scene, target, &geometry))
            placed += ui_rect_has_area(ui_rect_intersection(geometry.clip, f.scene.layout.panel)) ? 1 : 0;
    }
    return placed && renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
           probe_present(&renderer, kDefaultTheme) && capture(name);
}

// The interval dropdown open: the client's themed context menu (a settings_choice popup) under the
// control, the current choice checked and the next row under the pointer.
struct MenuFixture {
    PopupScene popup;
    PopupItem choices[8], items[8];
    PopupRowWidget rows[8];
    UiWidget widgets[2 + 8 * kPopupWidgetsPerItem];
    UiLayoutNode nodes[2 + 8 * kPopupWidgetsPerItem];
};
MenuFixture g_menu;
bool show_interval_menu(Fixture& f, D3D11Renderer& renderer, UiRect viewport, UiPageKey page,
                        bool (*capture)(const wchar_t*) noexcept) noexcept {
    UiSettingsSchemaLocation location{};
    if (!settings_catalog_binding(SettingBinding::update_interval, &location) ||
        !ui_page_registry_select(&f.registry, page).ok() || !ui_settings_scene_update(&f.scene, &f.registry, viewport))
        return false;
    const u64 id = settings_control_id(location.panel, location.node);
    const UiSettingsTarget target{UiSettingsTargetKind::control, page, id};
    const auto* control = ui_settings_scene_control(f.scene, target);
    UiRect anchor{};
    auto& m = g_menu;
    const auto* old_popup = renderer.popup_scene;
    struct Restore {
        D3D11Renderer& renderer;
        MenuFixture& m;
        const PopupScene* old_popup;
        ~Restore() noexcept {
            renderer_bind_popup_scene(&renderer, old_popup);
            popup_scene_close(&m.popup);
            renderer.retained_presentation.valid = false;
            popup_scene_release(&m.popup);
        }
    } restore{renderer, m, old_popup};
    const u32 count = control ? ui_settings_choice_items(*control, 0, {m.choices, 8}) : 0;
    bool changed = false;
    PopupItemLayout next{};
    return control && count == control->choices.count && ui_settings_scene_control_anchor(f.scene, target, &anchor) &&
           popup_scene_initialize(&m.popup, {m.items, 8}, {m.rows, 8}, {m.widgets, 2 + 8 * kPopupWidgetsPerItem},
                                  {m.nodes, 2 + 8 * kPopupWidgetsPerItem}) &&
           popup_scene_open(&m.popup, PopupKind::settings_choice, id, {m.choices, count}, anchor, viewport, 240).ok() &&
           renderer_bind_popup_scene(&renderer, &m.popup) && popup_scene_resolve_item_layout(m.popup, 1, &next) && next.visible &&
           popup_scene_set_hover(&m.popup, (next.visible_rectangle.left + next.visible_rectangle.right) * .5f,
                                 (next.visible_rectangle.top + next.visible_rectangle.bottom) * .5f, &changed) &&
           (m.choices[0].flags & popup_item_checked) != 0 && renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
           probe_present(&renderer, kDefaultTheme) && renderer.interaction_text_popup_revision == m.popup.revision &&
           capture(L"updates-interval-menu.bmp");
}

// Typography commits back to back, as dragging Text size in chat (or changing the theme's fonts) makes them: after each
// one the settings page, whose sample line is chat text beside the interface's own text, rebuilds to the same text,
// nothing is rejected, and a second round of the same commits adds no atlas entry or font face.
bool commit_typography(D3D11Renderer& renderer, const UiThemeTypography& value) noexcept {
    RendererTypographyCandidate candidate{};
    const bool applied = renderer_prepare_typography(&renderer, value, &candidate) &&
                         renderer_commit_typography(&renderer, &candidate);
    renderer_discard_typography(&candidate);
    return applied;
}
UiSettingsPreview g_churn_preview;
bool typography_churn(Fixture& f, D3D11Renderer& renderer, UiTextMeasureCache& cache, UiRect viewport,
                      UiPageKey page) noexcept {
    const auto live = renderer_text_system(&renderer, UiTextFace::normal);
    const UiThemeTypography original = live.typography ? live.typography->value : UiThemeTypography{};
    const u32 panel = static_cast<u32>(SettingsPanel::accessibility);
    struct Restore {
        D3D11Renderer& renderer;
        Fixture& f;
        UiThemeTypography original;
        u32 panel;
        ~Restore() noexcept {
            commit_typography(renderer, original);
            f.contents[panel].preview = &kPreview;
            ++f.contents[panel].revision;
        }
    } restore{renderer, f, original, panel};
    g_churn_preview = kPreview;
    f.contents[panel].preview = &g_churn_preview;
    const auto settle = [&]() noexcept {
        ui_text_measure_cache_invalidate(&cache);
        ++f.contents[panel].revision;
        return ui_page_registry_select(&f.registry, page).ok() &&
               ui_settings_scene_update(&f.scene, &f.registry, viewport) &&
               renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme);
    };
    if (!settle() || !renderer.content_text)
        return false;
    const u32 baseline = renderer.content_text->quad_count, rejected = renderer.content_text->rejected;
    if (!baseline)
        return false;
    struct Step {
        float body, chat;
        UiTextFace face;
    };
    const Step steps[]{{0, 20, UiTextFace::normal},  {0, 12, UiTextFace::normal}, {0, 24, UiTextFace::normal},
                       {0, 0, UiTextFace::normal},   {0, 18, UiTextFace::normal}, {0, 14, UiTextFace::normal},
                       {18, 0, UiTextFace::normal},  {18, 20, UiTextFace::normal}, {0, 24, UiTextFace::serif},
                       {0, 0, UiTextFace::normal},   {0, 20, UiTextFace::normal}, {0, 20, UiTextFace::normal}};
    u32 entries_after_first = 0, faces_after_first = 0;
    for (u32 round = 0; round < 2; ++round) {
        for (const auto& step : steps) {
            auto value = original;
            if (step.body)
                value.body_size = step.body;
            value.chat_size = step.chat;
            value.body_face = step.face;
            g_churn_preview.text_size = value.chat_size ? value.chat_size : value.body_size;
            if (!commit_typography(renderer, value) || !settle())
                return false;
            const auto interface_system = renderer_text_system(&renderer, UiTextFace::normal);
            const auto chat_system = renderer_text_system(&renderer, ui_text_face_chat(UiTextFace::normal));
            const auto& storage = *renderer.content_text;
            // The page keeps its text (every line executed), nothing was rejected, and the two systems hold the
            // sizes asked for.
            if (storage.quad_count < baseline * 85 / 100 || storage.quad_count > baseline * 115 / 100 ||
                storage.rejected != rejected || storage.last_error != RendererTextBuildError::none ||
                interface_system.em_size != value.body_size || !interface_system.native_format ||
                chat_system.em_size != (value.chat_size ? value.chat_size : value.body_size) ||
                !chat_system.native_format)
                return false;
        }
        if (!round) {
            entries_after_first = renderer.glyph_text.allocator.entry_count;
            faces_after_first = renderer.glyph_text.font_face_count;
        }
    }
    log_u64(LogLevel::info, "probe", "typography_churn_atlas_entries", renderer.glyph_text.allocator.entry_count);
    return renderer.glyph_text.allocator.entry_count <= entries_after_first + 16 &&
           renderer.glyph_text.font_face_count <= faces_after_first + 1 && commit_typography(renderer, original) &&
           settle() && renderer.content_text->quad_count >= baseline * 95 / 100 &&
           renderer.content_text->quad_count <= baseline * 105 / 100;
}

// A rebuild that fails (here: a DPI the atlas key cannot hold, which every glyph run refuses) leaves the text layer
// empty; the next refresh must notice and rebuild within the same frame. Settings' own check ("is my revision the
// one the text was built for?") used to say yes, which left the page without text until a resize changed its
// revision. Then rebuilds at a high rate over text that keeps naming new glyphs, until the atlas evicts and resets:
// the text layer is never empty after a frame.
char g_pressure_text[256];
bool text_drop_recovery(Fixture& f, D3D11Renderer& renderer, UiTextMeasureCache& cache, UiRect viewport,
                        UiPageKey page) noexcept {
    const u32 panel = static_cast<u32>(SettingsPanel::accessibility);
    const Utf8View original_description = f.contents[panel].description;
    struct Restore {
        Fixture& f;
        u32 panel;
        Utf8View description;
        ~Restore() noexcept {
            f.contents[panel].description = description;
            ++f.contents[panel].revision;
        }
    } restore{f, panel, original_description};
    const auto settle = [&]() noexcept {
        ++f.contents[panel].revision;
        return ui_page_registry_select(&f.registry, page).ok() &&
               ui_settings_scene_update(&f.scene, &f.registry, viewport) &&
               renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme);
    };
    if (!settle() || !renderer.content_text || !renderer.content_text->quad_count ||
        renderer.interaction_text_settings_revision != f.scene.revision)
        return false;
    const u32 baseline = renderer.content_text->quad_count, rejected = renderer.content_text->rejected;
    // The failure: the build is refused, the layer is dropped and the committed state no longer matches the page.
    const u32 dpi = renderer.dpi;
    renderer.dpi = 0x4000;
    const bool refused = !renderer_rebuild_content_text(&renderer, kDefaultTheme);
    renderer.dpi = dpi;
    if (!refused || renderer.content_text->rejected == rejected || renderer.content_text->quad_count ||
        renderer.interaction_text_settings_revision == f.scene.revision)
        return false;
    // One refresh later the text is back.
    if (!renderer_refresh_interaction_text(&renderer, kDefaultTheme) || renderer.content_text->quad_count < baseline / 2 ||
        renderer.interaction_text_settings_revision != f.scene.revision)
        return false;
    // Pressure: 64 new CJK glyphs a frame, 300 frames, each a rebuild.
    const u32 resets = renderer.glyph_text.allocator.full_resets, evictions = renderer.glyph_text.allocator.evicted_entries;
    u32 recovered = 0;
    for (u32 frame = 0; frame < 300; ++frame) {
        u32 bytes = 0;
        for (u32 glyph = 0; glyph < 64; ++glyph) {
            const u32 code = 0x4E00u + (frame * 64u + glyph) % 0x4000u;
            g_pressure_text[bytes++] = static_cast<char>(0xE0u | (code >> 12));
            g_pressure_text[bytes++] = static_cast<char>(0x80u | ((code >> 6) & 0x3Fu));
            g_pressure_text[bytes++] = static_cast<char>(0x80u | (code & 0x3Fu));
        }
        f.contents[panel].description = {g_pressure_text, bytes};
        ui_text_measure_cache_invalidate(&cache);
        bool built = settle();
        // A frame whose rebuild failed recovers at once, as the sync after it does.
        if (!built || !renderer.content_text->quad_count) {
            ++recovered;
            built = renderer_refresh_interaction_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme);
        }
        if (!built || renderer.content_text->quad_count < baseline / 2)
            return false;
    }
    log_u64(LogLevel::info, "probe", "text_drop_pressure_recovered_frames", recovered);
    log_u64(LogLevel::info, "probe", "text_drop_pressure_atlas_resets", renderer.glyph_text.allocator.full_resets - resets);
    log_u64(LogLevel::info, "probe", "text_drop_pressure_atlas_evictions",
            renderer.glyph_text.allocator.evicted_entries - evictions);
    return true;
}
} // namespace

bool user_settings_ui_probe(D3D11Renderer& renderer, UiTextMeasureCache& cache,
                            bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!capture || renderer.login_visible || !renderer.dpi)
        return false;
    auto& f = fixture;
    const UiSettingsScene* old_scene = renderer.settings_scene;
    const UiScene* old_shell = renderer.ui_scene;
    const u32 width = renderer.width, height = renderer.height;
    struct Restore {
        D3D11Renderer& renderer;
        Fixture& f;
        const UiSettingsScene* old_scene;
        const UiScene* old_shell;
        u32 width, height;
        ~Restore() noexcept {
            renderer_bind_text_form(&renderer, nullptr);
            text_form_surface_release(&f.surface);
            text_form_release(&f.form);
            renderer_bind_settings_scene(&renderer, old_scene);
            renderer_bind_ui_scene(&renderer, old_shell);
            ui_scene_release(&g_shell);
            ui_settings_scene_release(&f.scene);
            ui_page_registry_clear(&f.registry);
            renderer_resize(&renderer, width, height);
        }
    } restore{renderer, f, old_scene, old_shell, width, height};
    u32 stage = __LINE__;
    bool valid = ui_page_registry_initialize(&f.registry, {f.pages, kSettingsPanelCount + 2});
    for (u32 panel = 0; valid && panel < kSettingsPanelCount; ++panel) {
        const auto& source = settings_catalog_panel(static_cast<SettingsPanel>(panel));
        const u32 count = project_panel(panel, f.controls[panel], f.looks[panel]);
        f.contents[panel] = {source.title, source.summary, {f.controls[panel], count}, 1};
        f.contents[panel].presentation = f.looks[panel];
        f.contents[panel].preview = &kPreview;
        if (panel == static_cast<u32>(SettingsPanel::updates)) {
            f.contents[panel].controls = {f.controls[panel],
                                          project_updates(UpdatesState::idle, f.controls[panel], f.looks[panel], f.update_meters)};
            f.contents[panel].list_items = nullptr;
            f.contents[panel].meters = f.update_meters;
        }
        valid = ui_page_registry_register(&f.registry,
                                          {{kSettingsNativeOwner, panel + 1u}, source.title, &f.contents[panel]})
                    .ok();
    }
    const u32 gallery = build_gallery(f);
    f.contents[kSettingsPanelCount] = {utf8_literal("Control states"),
                                       utf8_literal("Synthetic rows for every settings control state."),
                                       {f.controls[kSettingsPanelCount], gallery}, 1};
    f.contents[kSettingsPanelCount].presentation = f.looks[kSettingsPanelCount];
    if (valid)
        stage = __LINE__;
    valid = valid &&
            ui_page_registry_register(&f.registry,
                                      {kGalleryPage, utf8_literal("Control states"), &f.contents[kSettingsPanelCount]})
                .ok();
    const u32 showcase = build_showcase(f);
    f.contents[kShowcaseIndex] = {utf8_literal("Showcase"), utf8_literal("The shell's cards, trailing controls and status rows."),
                                  {f.controls[kShowcaseIndex], showcase}, 1};
    f.contents[kShowcaseIndex].presentation = f.looks[kShowcaseIndex];
    f.contents[kShowcaseIndex].meters = f.showcase_meters;
    valid = valid &&
            ui_page_registry_register(&f.registry, {kShowcasePage, utf8_literal("Showcase"), &f.contents[kShowcaseIndex]}).ok();
    const auto navigation = settings_catalog_navigation(true);
    if (valid)
        stage = __LINE__;
    valid = valid && renderer_resize(&renderer, 1440, 900);
    const float scale = static_cast<float>(renderer.dpi) / 96.f;
    const UiRect viewport{0, 0, 1440 / scale, 900 / scale};
    valid = valid && ui_settings_scene_set_presentation(&f.scene, UiSettingsPresentation::modal) &&
            ui_settings_scene_bind_navigation(&f.scene, navigation, 1) &&
            ui_settings_scene_bind_palette(&f.scene, {&f, palette, 1});
    valid = valid && (ui_settings_scene_bind_measure(&f.scene, &cache), true);
    // The probe's shell scene keeps the size it was laid out at and clips every label to it: lay a fresh one out at
    // the capture size so wide pages are not cut.
    valid = valid && ui_scene_initialize_bootstrap(&g_shell) &&
            ui_scene_layout_with_metrics(&g_shell, UiShellView::guild_channel, kGuildChannelShellLayoutMetrics, 1440 / scale,
                                         900 / scale) &&
            renderer_bind_ui_scene(&renderer, &g_shell);
    if (valid)
        renderer_bind_settings_scene(&renderer, &f.scene);
    const auto page = [](SettingsPanel panel) noexcept {
        return UiPageKey{kSettingsNativeOwner, static_cast<u64>(panel) + 1};
    };
    if (valid)
        stage = __LINE__;
    valid = valid && show(f, renderer, page(SettingsPanel::content_social), viewport, L"user-settings-messaging.bmp", capture);
    if (valid)
        stage = __LINE__;
    valid = valid && show(f, renderer, page(SettingsPanel::appearance), viewport, L"user-settings-appearance.bmp", capture);
    // Mid-entrance, every quad (plates, rings, icons), glyph and picture of the modal draws under the one
    // entrance transform; only the scrim before it draws apart.
    if (valid) {
        stage = __LINE__;
        renderer_set_settings_modal_progress(&renderer, .5f);
        valid = probe_present(&renderer, kDefaultTheme) && renderer.content_text &&
                renderer.settings_modal_paint_first_batch < renderer.paint_stream.batch_count;
        if (valid) {
            const u32 scrim = renderer.paint_stream.batches[renderer.settings_modal_paint_first_batch].first_quad;
            const u32 glyphs = renderer.content_text->quad_count;
            log_u64(LogLevel::info, "probe", "settings_mid_open_modal_instances",
                    renderer.modal_layer_instances_last_frame);
            log_u64(LogLevel::info, "probe", "settings_mid_open_other_instances",
                    renderer.other_layer_instances_last_frame);
            valid = capture(L"settings-mid-open.bmp") && scrim && glyphs &&
                    renderer.other_layer_instances_last_frame == scrim &&
                    renderer.modal_layer_instances_last_frame >= renderer.paint_stream.count - scrim + glyphs;
        }
        renderer_set_settings_modal_progress(&renderer, 1);
    }
    if (valid)
        stage = __LINE__;
    valid = valid &&
            show(f, renderer, page(SettingsPanel::accessibility), viewport, L"user-settings-accessibility.bmp", capture);
    if (valid)
        stage = __LINE__;
    valid = valid && typography_churn(f, renderer, cache, viewport, page(SettingsPanel::accessibility));
    if (valid)
        stage = __LINE__;
    valid = valid && text_drop_recovery(f, renderer, cache, viewport, page(SettingsPanel::accessibility));
    if (valid)
        stage = __LINE__;
    valid = valid &&
            show(f, renderer, page(SettingsPanel::activity_privacy), viewport, L"user-settings-activity.bmp", capture);
    if (valid)
        stage = __LINE__;
    valid = valid && show(f, renderer, page(SettingsPanel::voice), viewport, L"user-settings-voice.bmp", capture);
    if (valid)
        stage = __LINE__;
    // Every page state of Updates: up to date, checking (with the retained frame's spinner), ready,
    // a preview download and an error; then the interval dropdown's themed menu.
    const auto updates_state_in = [&](UpdatesState state, const wchar_t* name, const UiRect& where) noexcept {
        const u32 panel_index = static_cast<u32>(SettingsPanel::updates);
        const u32 count = project_updates(state, f.controls[panel_index], f.looks[panel_index], f.update_meters);
        f.contents[panel_index].controls = {f.controls[panel_index], count};
        ++f.contents[panel_index].revision;
        return count && show(f, renderer, page(SettingsPanel::updates), where, name, capture);
    };
    const auto updates_state = [&](UpdatesState state, const wchar_t* name) noexcept {
        return updates_state_in(state, name, viewport);
    };
    if (valid)
        stage = __LINE__;
    valid = valid && updates_state(UpdatesState::idle, L"updates-idle.bmp");
    if (valid)
        stage = __LINE__;
    valid = valid && show_interval_menu(f, renderer, viewport, page(SettingsPanel::updates), capture);
    if (valid)
        stage = __LINE__;
    valid = valid && updates_state(UpdatesState::checking, L"updates-checking.bmp");
    // The busy status card's spinner: a phase step alone must invalidate the retained frame.
    if (valid) {
        stage = __LINE__;
        const auto before = renderer_capture_presentation_stamp(renderer, kDefaultTheme);
        ++f.scene.spinner_phase;
        const auto after = renderer_capture_presentation_stamp(renderer, kDefaultTheme);
        valid = ui_settings_scene_spinner_visible(f.scene) && !renderer_presentation_stamp_equal(before, after);
    }
    if (valid)
        stage = __LINE__;
    valid = valid && updates_state(UpdatesState::ready, L"updates-ready.bmp");
    // The sidebar's attention dot on Updates while an update waits for Restart to update, as the host binds it,
    // with the sidebar scrolled to the entry.
    static UiSettingsNavEntry dotted[kUiSettingsNavigationMaximum];
    if (valid) {
        stage = __LINE__;
        const auto marked = settings_catalog_navigation_dotted(navigation, dotted);
        valid = marked.count == navigation.count && ui_settings_scene_bind_navigation(&f.scene, marked, 2) &&
                ui_page_registry_select(&f.registry, page(SettingsPanel::updates)).ok() &&
                ui_settings_scene_update(&f.scene, &f.registry, viewport) &&
                ui_settings_scene_reveal(&f.scene, {UiSettingsTargetKind::page, page(SettingsPanel::updates), 0}) &&
                renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) &&
                capture(L"updates-sidebar-dot.bmp") && ui_settings_scene_bind_navigation(&f.scene, navigation, 3);
        ui_settings_scene_scroll(&f.scene, true, -f.scene.sidebar_offset);
    }
    valid = valid && updates_state(UpdatesState::preview_downloading, L"updates-preview-downloading.bmp");
    valid = valid && updates_state(UpdatesState::error, L"updates-error.bmp");
    // The long identity line at the full width and wrapped in two narrower cards: continuation lines start at the
    // left edge and only whole " · " parts move down.
    if (valid)
        stage = __LINE__;
    valid = valid && updates_state(UpdatesState::long_build, L"updates-long-wide.bmp") &&
            updates_state_in(UpdatesState::long_build, L"updates-long-narrow.bmp", {0, 0, 840, 720}) &&
            updates_state_in(UpdatesState::long_build, L"updates-long-narrower.bmp", {0, 0, 720, 720});
    // The shell's new rows at rest: status card, trailing controls, disclosures, markdown, footnote and empty state.
    if (valid)
        stage = __LINE__;
    valid = valid && show(f, renderer, kShowcasePage, viewport, L"settings-showcase.bmp", capture);
    // The notes row clipped under its Show all link, scrolled into view.
    if (valid) {
        stage = __LINE__;
        valid = ui_settings_scene_reveal(&f.scene, {UiSettingsTargetKind::control, kShowcasePage, 2008}) &&
                renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) &&
                capture(L"settings-showcase-notes.bmp");
        ui_settings_scene_scroll(&f.scene, false, -f.scene.content_offset);
    }
    // The gallery with keyboard focus on its first switch, then scrolled to
    // the swatches with focus on them.
    if (valid)
        stage = __LINE__;
    const UiSettingsTarget field_target{UiSettingsTargetKind::control, kGalleryPage, 1008};
    valid = valid && ui_page_registry_select(&f.registry, kGalleryPage).ok() &&
            ui_settings_scene_update(&f.scene, &f.registry, viewport) &&
            ui_settings_scene_focus(&f.scene, {UiSettingsTargetKind::control, kGalleryPage, 1001});
    ui_settings_scene_set_focus_visible(&f.scene, true);
    UiSettingsControlGeometry field{};
    if (valid)
        stage = __LINE__;
    valid = valid && (ui_settings_scene_reveal(&f.scene, field_target), true) &&
            ui_settings_scene_control_geometry(f.scene, field_target, &field);
    if (valid) {
        // A measured line with a selection over "display" and the caret after it.
        auto& g = f.geometries[0];
        const float middle = (field.text.top + field.text.bottom) * .5f;
        g = {};
        g.text = {field.text.left, middle - 10, field.text.left + 240, middle + 10};
        g.selections[0] = {field.text.left + 14, middle - 10, field.text.left + 64, middle + 10};
        g.selection_count = 1;
        g.caret = {field.text.left + 64, middle - 10, field.text.left + 65, middle + 10};
        f.editors[0].geometry = &g;
        ++f.contents[kSettingsPanelCount].revision;
    }
    if (valid)
        stage = __LINE__;
    valid = valid && show(f, renderer, kGalleryPage, viewport, L"user-settings-controls.bmp", capture);
    const UiSettingsTarget swatch_target{UiSettingsTargetKind::control, kGalleryPage, 1017};
    if (valid)
        stage = __LINE__;
    valid = valid && ui_settings_scene_focus(&f.scene, swatch_target) &&
            (ui_settings_scene_reveal(&f.scene, {UiSettingsTargetKind::control, kGalleryPage, 1018}), true) &&
            show(f, renderer, kGalleryPage, viewport, L"user-settings-controls-swatch-focus.bmp", capture);
    // A hex colour form after Apply with invalid text: reverted to the last
    // accepted colour, previewed in its swatch, with the error outline.
    TextFormContent content{utf8_literal("Pick a Color"),
                            utf8_literal("Enter a hex colour such as #5865F2."),
                            utf8_literal("Primary"),
                            utf8_literal("That isn't a hex colour. Use #RGB or #RRGGBB."),
                            utf8_literal("Cancel"),
                            utf8_literal("Apply"),
                            utf8_literal("#58F"),
                            4,
                            4,
                            true,
                            true};
    content.single_line = true;
    content.placeholder = utf8_literal("#RRGGBB");
    content.error = true;
    content.swatch = true;
    content.swatch_rgba = 0x5588FFFFu;
    if (valid)
        stage = __LINE__;
    valid = valid && text_form_update(&f.form, content, viewport, &cache) &&
            ui_rect_has_area(text_form_swatch_box(f.form));
    if (valid) {
        f.surface.scene = &f.form;
        renderer_bind_text_form(&renderer, &f.surface);
        stage = __LINE__;
        valid = renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) &&
                capture(L"user-settings-hex-error.bmp");
    }
    if (!valid)
        log_u64(LogLevel::error, "probe", "user_settings_failure_line", stage);
    log_write(valid ? LogLevel::info : LogLevel::error, "probe",
              valid ? "user_settings_catalog_pages_and_control_states_passed" : "user_settings_ui_failed");
    return valid;
}
} // namespace maicord
