#pragma once

#include "ui_surface_motion_measurement.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/icon.hpp"

namespace maicord {
namespace guild_roles_probe_detail {

// Pixels near `color` inside a rectangle (DIPs).
inline u32 ink_count(const surface_motion_probe_detail::Readback& rb, u32 image, UiRect r, float scale,
                     Color color) noexcept {
    const auto clip = raster_clip_rectangle(r, scale, rb.width, rb.height);
    const u8 expected[]{static_cast<u8>(color.blue * 255.f + .5f), static_cast<u8>(color.green * 255.f + .5f),
                        static_cast<u8>(color.red * 255.f + .5f)};
    u32 pixels = 0;
    for (i32 y = maximum(clip.top, 0); y < clip.bottom; ++y)
        for (i32 x = maximum(clip.left, 0); x < clip.right; ++x)
            pixels += surface_motion_probe_detail::distance(rb.images + image * rb.bytes + (y * rb.width + x) * 4,
                                                            expected) <= 24;
    return pixels;
}

// Original role colours of the fixture, by row.
constexpr u32 kRoleColors[]{0xE67E22, 0x1ABC9C, 0x9B59B6, 0x3498DB, 0xE91E63};
inline bool resolve(void*, u64 id, u32 choice, BackgroundTheme* theme) noexcept {
    if (choice != kUiSettingsAccentChoice || id < 100 || id >= 100 + sizeof(kRoleColors) / sizeof(u32))
        return false;
    *theme = {};
    theme->colors[0] = kRoleColors[id - 100];
    theme->color_count = 1;
    return true;
}

} // namespace guild_roles_probe_detail

// Server Settings › Roles, the role editor's Permissions tab and a channel's
// overrides with tri-state switches, on the production settings scene and
// renderer. Original offline models only; captures when
// MAICORD_UI_PROBE_CAPTURE_DIR is set.
inline bool guild_roles_ui_probe(D3D11Renderer& renderer) noexcept {
    using namespace guild_roles_probe_detail;
    using surface_motion_probe_detail::Readback;
    u32 stage = 1;
    const auto fail = [&]() noexcept {
        log_u64(LogLevel::error, "probe", "guild_roles_failure_stage", stage);
        return false;
    };
    if (!renderer.content_text || !renderer.glyph_text.ready)
        return fail();
    static UiSettingsScene scene{};
    static UiPageRegistry registry{};
    static UiPage pages[3]{};
    const auto* old_settings = renderer.settings_scene;
    const auto* old_focus = renderer.focus_owner;
    struct Restore {
        D3D11Renderer& renderer;
        const UiSettingsScene* settings;
        const UiWidgetManager* focus;
        ~Restore() noexcept {
            renderer_bind_settings_scene(&renderer, settings);
            renderer_bind_focus_owner(&renderer, focus);
            renderer.retained_presentation.valid = false;
            ui_settings_scene_release(&scene);
            scene = {};
            registry = {};
        }
    } restore{renderer, old_settings, old_focus};
    const float scale = static_cast<float>(renderer.dpi) / 96.f;
    const UiRect viewport{0, 0, renderer.width / scale, renderer.height / scale};
    const auto tokens = ui_settings_tokens(kDefaultTheme);
    using K = UiSettingsControlKind;
    constexpr UiIcon none = UiIcon::count;

    // Roles list.
    UiSettingsEditor search{{{}, 0, 0, true, false, false}, nullptr, utf8_literal("Search roles")};
    static const Utf8View members[]{utf8_literal("MEMBERS")};
    static const Utf8View counts[][1]{{utf8_literal("1")}, {utf8_literal("4")}, {utf8_literal("12")},
                                      {utf8_literal("57")}, {utf8_literal("0")}};
    const Utf8View names[]{utf8_literal("Gardeners"), utf8_literal("Moderators"), utf8_literal("Sketch club"),
                           utf8_literal("Visitors"), utf8_literal("Weather bot")};
    UiSettingsControl list[9]{};
    UiSettingsControlPresentation list_looks[9]{};
    UiSettingsListItem list_items[9]{};
    list[0] = {1, utf8_literal("Default permissions"), utf8_literal("@everyone · applies to all server members"),
               K::action, true, false, {}, 0, {}, nullptr};
    list_items[0] = {UiSettingsListStyle::card, UiIcon::members, none, {none, none}, false};
    list[1] = {2, {}, {}, K::edit, true, false, {}, 0, utf8_literal("Create Role"), &search};
    list[2] = {3, utf8_literal("ROLES — 5"),
               utf8_literal("Members use the colour of the highest role they have on this list. Drag roles to "
                            "reorder them."),
               K::category, true, false, {members, 1}, 0, {}, nullptr};
    list_items[2] = {UiSettingsListStyle::header, none, none, {none, none}, false};
    for (u32 i = 0; i < 5; ++i) {
        const bool locked = i == 0 || i == 4;
        list[3 + i] = {100 + i, names[i], {}, K::action, true, false, {counts[i], 1}, 0, {}, nullptr};
        list_looks[3 + i] = {UiSettingsVariant::standard,
                             static_cast<u8>(ui_settings_presentation_accent |
                                             (locked ? 0 : ui_settings_presentation_grip)),
                             kUiSettingsNoDefault};
        list_items[3 + i] = {UiSettingsListStyle::row, UiIcon::shield, UiIcon::user,
                             {locked ? UiIcon::eye : UiIcon::edit, UiIcon::ellipsis}, locked};
    }
    for (u32 i = 0; i < 3; ++i)
        list_looks[i] = {UiSettingsVariant::standard, 0, kUiSettingsNoDefault};
    UiSettingsPageContent roles{utf8_literal("Roles"),
                                utf8_literal("Use roles to group your server members and assign permissions."),
                                {list, 8},
                                1,
                                kUiSettingsPageFormat,
                                &kUiDefaultSettingsStyle,
                                nullptr};
    roles.presentation = list_looks;
    roles.list_items = list_items;

    // Role editor, Permissions tab, with an unsaved change.
    static const Utf8View tabs[]{utf8_literal("Display"), utf8_literal("Permissions")};
    UiSettingsEditor permission_search{{{}, 0, 0, true, false, false}, nullptr, utf8_literal("Search permissions")};
    UiSettingsControl editor[]{
        {1, utf8_literal("Sketch club"), utf8_literal("Editing role"), K::action, true, false, {}, 0,
         utf8_literal("Back to roles"), nullptr},
        {2, utf8_literal("Role settings"), {}, K::tabs, true, false, {tabs, 2}, 1, {}, nullptr},
        {3, {}, {}, K::edit, true, false, {}, 0, {}, &permission_search},
        {4, utf8_literal("Clear permissions"), utf8_literal("Turns off every permission of this role."), K::action,
         true, false, {}, 0, utf8_literal("Clear"), nullptr},
        {5, utf8_literal("General server permissions"), {}, K::category, true, false, {}, 0, {}, nullptr},
        {6, utf8_literal("View Channels"), utf8_literal("Lets members see channels by default."), K::toggle, true,
         true, {}, 0, {}, nullptr},
        {7, utf8_literal("Manage Roles"),
         utf8_literal("Create roles and edit the ones below their highest role. You can't change this: none of your "
                      "roles have it."),
         K::toggle, false, false, {}, 0, {}, nullptr},
        {8, utf8_literal("Text channel permissions"), {}, K::category, true, false, {}, 0, {}, nullptr},
        {9, utf8_literal("Send Messages"), utf8_literal("Post messages in text channels."), K::toggle, true, true, {},
         0, {}, nullptr},
        {10, utf8_literal("Attach Files"), utf8_literal("Upload files and media."), K::toggle, true, false, {}, 0, {},
         nullptr}};
    UiSettingsControlPresentation editor_looks[10]{};
    for (auto& look : editor_looks)
        look = {UiSettingsVariant::standard, 0, kUiSettingsNoDefault};
    editor_looks[0].flags = ui_settings_presentation_accent;
    UiSettingsFooter footer{utf8_literal("Careful — you have unsaved changes!"), utf8_literal("Save changes"),
                            utf8_literal("Reset"), true, true, false, false};
    UiSettingsPageContent permissions{utf8_literal("Edit role — Sketch club"), {}, {editor, 10}, 1,
                                      kUiSettingsPageFormat, &kUiDefaultSettingsStyle, &footer};
    permissions.presentation = editor_looks;

    // Channel overrides: deny, neutral and allow.
    static const Utf8View states[]{utf8_literal("Deny"), utf8_literal("Neutral"), utf8_literal("Allow")};
    UiSettingsControl channel[]{
        {1, utf8_literal("General channel permissions"), utf8_literal("Deny, keep neutral to inherit, or allow."),
         K::category, true, false, {}, 0, {}, nullptr},
        {2, utf8_literal("View Channel"), utf8_literal("Lets members see this channel."), K::segments, true, false,
         {states, 3}, 2, {}, nullptr},
        {3, utf8_literal("Manage Channel"), utf8_literal("Edit the name, topic and settings."), K::segments, true,
         false, {states, 3}, 1, {}, nullptr},
        {4, utf8_literal("Send Messages"), utf8_literal("Post in this channel."), K::segments, true, false,
         {states, 3}, 0, {}, nullptr},
        {5, utf8_literal("Attach Files"), utf8_literal("Upload files and media here."), K::segments, false, false,
         {states, 3}, 1, {}, nullptr}};
    UiSettingsControlPresentation channel_looks[5]{};
    for (auto& look : channel_looks)
        look = {UiSettingsVariant::standard, ui_settings_presentation_tristate, kUiSettingsNoDefault};
    channel_looks[0].flags = 0;
    UiSettingsPageContent overrides{utf8_literal("Permissions"), {}, {channel, 5}, 1, kUiSettingsPageFormat,
                                    &kUiDefaultSettingsStyle, nullptr};
    overrides.presentation = channel_looks;

    const UiPageKey roles_key{0x701e, 1}, editor_key{0x701e, 2}, channel_key{0x701e, 3};
    if (!ui_page_registry_initialize(&registry, {pages, 3}) ||
        !ui_page_registry_register(&registry, {roles_key, utf8_literal("Roles"), &roles}).ok() ||
        !ui_page_registry_register(&registry, {editor_key, utf8_literal("Edit role"), &permissions}).ok() ||
        !ui_page_registry_register(&registry, {channel_key, utf8_literal("Channel permissions"), &overrides}).ok() ||
        !ui_settings_scene_update(&scene, &registry, viewport) ||
        !ui_settings_scene_bind_palette(&scene, {&scene, resolve, 1}))
        return fail();
    renderer_bind_settings_scene(&renderer, &scene);
    renderer_bind_focus_owner(&renderer, &scene.widgets);
    Readback readback;
    if (!readback.initialize(renderer))
        return fail();
    const auto render = [&](u32 image, const wchar_t* filename) noexcept {
        if (!ui_settings_scene_update(&scene, &registry, viewport) ||
            !renderer_rebuild_content_text(&renderer, kDefaultTheme) || !probe_present(&renderer, kDefaultTheme) ||
            !probe_present(&renderer, kDefaultTheme) || !surface_motion_probe_detail::draw_fixed(renderer) ||
            !readback.read(renderer, image))
            return false;
        return readback.capture(image, filename);
    };

    // Stage 2: the list, with the keyboard on Moderators showing its buttons.
    stage = 2;
    const UiSettingsTarget moderators{UiSettingsTargetKind::control, roles_key, 101};
    ui_settings_scene_set_focus_visible(&scene, true);
    if (!ui_settings_scene_select(&scene, roles_key) || !ui_settings_scene_update(&scene, &registry, viewport) ||
        !ui_settings_scene_focus(&scene, moderators) || !render(0, L"guild-roles-list.bmp"))
        return fail();
    UiRect row{}, column{}, edit{}, more{};
    UiSettingsActivation part{};
    if (!ui_settings_scene_choice_rectangle(scene, moderators, ui_settings_list_row, &row) ||
        !ui_settings_scene_choice_rectangle(scene, moderators, ui_settings_list_count, &column) ||
        !ui_settings_scene_choice_rectangle(scene, moderators, ui_settings_list_action, &edit) ||
        !ui_settings_scene_choice_rectangle(scene, moderators, ui_settings_list_action + 1, &more) ||
        !(column.right < edit.left && edit.right < more.left && more.right <= row.right) ||
        !ui_settings_scene_option_pointer(scene, moderators, (more.left + more.right) * .5f,
                                          (more.top + more.bottom) * .5f, &part) ||
        part.selected_choice != ui_settings_list_action + 1 ||
        !ui_settings_scene_option_pointer(scene, moderators, row.left + ui_rect_width(row) * .3f,
                                          (row.top + row.bottom) * .5f, &part) ||
        part.selected_choice != ui_settings_list_row)
        return fail();
    // The lead shield wears the role colour; the icon buttons show on focus.
    const UiRect lead{row.left, row.top, row.left + ui_rect_width(row) * .15f, row.bottom};
    const Color teal{0x1A / 255.f, 0xBC / 255.f, 0x9C / 255.f, 1};
    if (ink_count(readback, 0, lead, scale, teal) < 8 ||
        ink_count(readback, 0, more, scale, tokens.color.text_default) < 4)
        return fail();

    // Stage 3: the Permissions tab with a disabled, explained switch.
    stage = 3;
    if (!ui_settings_scene_select(&scene, editor_key) || !render(1, L"guild-role-permissions.bmp"))
        return fail();
    UiSettingsControlGeometry tab_geometry{};
    if (!ui_settings_scene_control_geometry(scene, {UiSettingsTargetKind::control, editor_key, 2}, &tab_geometry))
        return fail();

    // Stage 4: overrides; each tri-state segment shows its state.
    stage = 4;
    if (!ui_settings_scene_select(&scene, channel_key) || !render(2, L"channel-override-tristate.bmp"))
        return fail();
    const UiSettingsTarget allow_row{UiSettingsTargetKind::control, channel_key, 2};
    const UiSettingsTarget deny_row{UiSettingsTargetKind::control, channel_key, 4};
    UiRect allow{}, deny{}, neutral{};
    if (!ui_settings_scene_choice_rectangle(scene, allow_row, 2, &allow) ||
        !ui_settings_scene_choice_rectangle(scene, deny_row, 0, &deny) ||
        !ui_settings_scene_choice_rectangle(scene, deny_row, 1, &neutral) || ui_rect_width(allow) != 32 ||
        neutral.left != deny.right)
        return fail();
    const Color positive = ui_settings_color_mix(tokens.color.positive, {0, 0, 0, 1}, tokens.light ? 0 : .35f);
    if (ink_count(readback, 2, allow, scale, positive) < 40 ||
        ink_count(readback, 2, deny, scale, tokens.color.critical) < 40 ||
        ink_count(readback, 2, neutral, scale, tokens.color.critical) > 4)
        return fail();
    log_write(LogLevel::info, "probe", "guild_roles_native_ui_passed");
    return true;
}

} // namespace maicord
