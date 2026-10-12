#include "probe_present.hpp"
#include "maicord/ui/inbox_scene.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/core/log.hpp"

namespace maicord {
namespace {
// Synthetic rows only; no account, network or application owner is involved.
// Popup rows and dialog descriptors are copied from
// application_notification_controls.cpp and application_custom_status.cpp.
struct InboxProbe {
    UiInboxScene scene;
    UiInboxSource source;
    UiInboxContent content;
    UiInboxTab tab;
    PopupItem items[8];
    UiSettingsScene dialog;
    UiPageRegistry registry;
    UiPage page;
    UiSettingsPageContent page_content;
    UiSettingsControl controls[3];
    UiSettingsEditor editors[2];
    UiSettingsFooter footer;
    // Isolated runs have no shared popup bank; this probe owns one.
    PopupScene menu;
    PopupItem menu_items[8];
    PopupRowWidget menu_rows[8];
    UiWidget menu_widgets[2 + 8 * kPopupWidgetsPerItem];
    UiLayoutNode menu_nodes[2 + 8 * kPopupWidgetsPerItem];
};
InboxProbe probe;

constexpr Utf8View kAuthors[]{utf8_literal("Ana Kovač"), utf8_literal("Bo"), utf8_literal("Cleo"),
                              utf8_literal("Dino"), utf8_literal("Eva")};
constexpr Utf8View kBodies[]{
    utf8_literal("@Rob the release build passed on every runner, want to tag it tonight?"),
    utf8_literal("@everyone standup moves to 10:30 tomorrow."),
    utf8_literal("@Design can you look at the new inbox spacing?"),
    utf8_literal("@Rob thanks!"),
    utf8_literal("@Rob I left notes on the custom status dialog copy and the clear-after options.")};
bool resolve(void* context, u32 index, UiInboxRow* row) noexcept {
    const auto& p = *static_cast<InboxProbe*>(context);
    if (!row)
        return false;
    if (p.tab == UiInboxTab::mentions) {
        *row = {UiInboxRowKind::mention, 900 + index, 40 + index % 2, 7, kAuthors[index % 5],
                index % 2 ? utf8_literal("#announcements · Native Lab") : utf8_literal("#general · Native Lab"),
                kBodies[index % 5], index ? utf8_literal("2 hours ago") : utf8_literal("5 minutes ago")};
        return true;
    }
    constexpr Utf8View guilds[]{utf8_literal("Direct messages"), utf8_literal("Native Lab")};
    constexpr Utf8View channels[]{utf8_literal("Ana Kovač"), utf8_literal("general"), utf8_literal("design"),
                                  utf8_literal("release-notes")};
    if (index == 0 || index == 2)
        *row = {UiInboxRowKind::group, index ? 7u : ~u64{0}, 0, index ? 7u : 0u, guilds[index / 2]};
    else {
        const u32 channel = index == 1 ? 0 : index - 2;
        *row = {UiInboxRowKind::channel, 50 + index, 50 + index, index == 1 ? 0u : 7u, channels[channel], {}, {}, {},
                index == 1 ? 2u : index == 3 ? 1u : 0u, index == 5};
    }
    return true;
}
bool current(void*) noexcept {
    return true;
}
bool show_inbox(D3D11Renderer& renderer, UiRect viewport, UiInboxTab tab, const wchar_t* name,
                bool (*capture)(const wchar_t*) noexcept, bool hover_channel = false) noexcept {
    auto& p = probe;
    p.tab = tab;
    p.content.tab = tab;
    ++p.content.revision;
    p.source.count = tab == UiInboxTab::mentions ? 5 : 6;
    ++p.source.revision;
    if (!ui_inbox_scene_update(&p.scene, &p.source, &p.content, viewport))
        return false;
    if (tab == UiInboxTab::mentions) {
        bool changed = false;
        const auto card = p.scene.row_rectangles[0];
        ui_inbox_scene_set_hover(&p.scene, (card.left + card.right) * .5f, card.top + 20, &changed);
    } else if (hover_channel) {
        // Hovering a channel row reveals its check button beside the badge.
        bool changed = false;
        const auto row = p.scene.row_rectangles[1];
        if (!ui_inbox_scene_set_hover(&p.scene, row.left + 60, (row.top + row.bottom) * .5f, &changed) ||
            !ui_inbox_scene_channel_action_visible(p.scene, 1) || ui_inbox_scene_channel_action_visible(p.scene, 3))
            return false;
    }
    renderer.inbox_scene = &p.scene;
    return renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) &&
           renderer.interaction_text_inbox_revision == p.scene.revision && capture(name);
}
bool show_menu(D3D11Renderer& renderer, PopupScene& popup, UiRect viewport, bool levels, const wchar_t* name,
               bool (*capture)(const wchar_t*) noexcept) noexcept {
    auto& p = probe;
    u32 count = 0;
    const auto row = [&](Utf8View label, u16 flags) noexcept {
        p.items[count] = {};
        p.items[count].label = label;
        p.items[count].action = 0x4e430000 + count + 1;
        p.items[count].flags = static_cast<u16>(popup_item_enabled | flags);
        ++count;
    };
    if (levels) {
        row(utf8_literal("All messages"), 0);
        row(utf8_literal("Only @mentions"), popup_item_checked);
        row(utf8_literal("Nothing"), 0);
        row(utf8_literal("Suppress @everyone and @here"), popup_item_separator_before | popup_item_checked);
        row(utf8_literal("Suppress all role @mentions"), 0);
    } else {
        row(utf8_literal("For 15 minutes"), 0);
        row(utf8_literal("For 1 hour"), 0);
        row(utf8_literal("For 3 hours"), 0);
        row(utf8_literal("For 8 hours"), 0);
        row(utf8_literal("For 24 hours"), 0);
        row(utf8_literal("Until I turn it back on"), 0);
    }
    popup_scene_close(&popup);
    const UiRect anchor{90, 120, 91, 121};
    return popup_scene_open(&popup, PopupKind::context_menu, 7, {p.items, count}, anchor, viewport,
                            levels ? 300.f : 220.f)
               .ok() &&
           renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) &&
           capture(name);
}
constexpr Utf8View kClearLabels[]{utf8_literal("Today"), utf8_literal("4 hours"), utf8_literal("1 hour"),
                                  utf8_literal("30 minutes"), utf8_literal("Don't clear")};
u32 dialog_failure;
bool dialog_stage(u32 stage) noexcept {
    dialog_failure = stage;
    return false;
}
bool show_dialog(D3D11Renderer& renderer, UiRect viewport, bool (*capture)(const wchar_t*) noexcept) noexcept {
    auto& p = probe;
    // The status emoji the user typed: U+1F354 HAMBURGER, caret after it.
    const auto emoji = utf8_literal("\xf0\x9f\x8d\x94");
    const auto status = utf8_literal("Out to lunch");
    p.editors[0] = {{emoji, emoji.bytes, emoji.bytes, true, false, false}, nullptr, utf8_literal("😀 or <:name:id>")};
    p.editors[1] = {{status, status.bytes, status.bytes, true, true, true}, nullptr,
                    utf8_literal("What's on your mind?")};
    p.controls[0] = {1, utf8_literal("Emoji"), {}, UiSettingsControlKind::edit, true, false, {}, 0, {}, &p.editors[0]};
    p.controls[1] = {2, utf8_literal("Status"), {}, UiSettingsControlKind::edit, true, false, {}, 0, {}, &p.editors[1]};
    p.controls[2] = {3, utf8_literal("Clear after"), {}, UiSettingsControlKind::choice, true, false, {kClearLabels, 5},
                     0, {}, nullptr};
    p.footer = {utf8_literal("Everyone who can see you sees your status."), utf8_literal("Save"),
                utf8_literal("Clear status"), true, true, false, false};
    p.page_content = {utf8_literal("Set a custom status"), {}, {p.controls, 3}, 1, kUiSettingsPageFormat,
                      &kUiDefaultSettingsStyle, &p.footer};
    if (!ui_page_registry_initialize(&p.registry, {&p.page, 1}) ||
        !ui_page_registry_register(&p.registry, {{0x4d41494353544154ull, 1}, p.page_content.title, &p.page_content}).ok())
        return dialog_stage(11);
    ui_settings_scene_set_presentation(&p.dialog, UiSettingsPresentation::dialog);
    if (!ui_settings_scene_update(&p.dialog, &p.registry, viewport))
        return dialog_stage(12);
    // The custom status form is a compact dialog: no sidebar or search, a
    // fitted height and its actions in a band along the bottom edge.
    {
        const auto& l = p.dialog.layout;
        const auto* status_node = ui_widget_manager_layout_node(p.dialog.widgets, p.dialog.footer_status);
        const auto* band = status_node ? ui_widget_manager_layout_node(
                                             p.dialog.widgets, p.dialog.widgets.storage[p.dialog.footer_status.slot].parent)
                                       : nullptr;
        if (p.dialog.search_field.generation || ui_rect_width(l.sidebar) > 0 ||
            ui_rect_width(l.modal) > kUiSettingsDialogWidth + .01f ||
            ui_rect_height(l.modal) >= ui_rect_height(viewport) * .75f || !band ||
            band->rectangle.bottom > l.modal.bottom + .01f || band->rectangle.bottom < l.modal.bottom - .01f ||
            band->rectangle.left > l.modal.left + .01f)
            return dialog_stage(17);
    }
    // Stages 16/13/14/15 separate the scene's own text output from the
    // renderer rebuild, present and capture when this isolated fixture fails.
    {
        static UiTextCommand commands[128];
        static char owned[8192];
        UiTextCommandStream stream{};
        UiTextOutput output{&stream};
        if (!ui_text_command_stream_initialize(&stream, {commands, 128}, {owned, sizeof(owned)}) ||
            !ui_settings_scene_text_output(p.dialog, kDefaultTheme, &output))
            return dialog_stage(16);
    }
    renderer_bind_settings_scene(&renderer, &p.dialog);
    if (!renderer_rebuild_content_text(&renderer, kDefaultTheme)) {
        log_u64(LogLevel::error, "probe", "inbox_dialog_text_error",
                static_cast<u32>(renderer.content_text->last_error));
        log_u64(LogLevel::error, "probe", "inbox_dialog_shape_error",
                static_cast<u32>(renderer.content_text->last_shape_error));
        return dialog_stage(13);
    }
    if (!probe_present(&renderer, kDefaultTheme))
        return dialog_stage(14);
    return capture(L"custom-status-dialog.bmp") || dialog_stage(15);
}
} // namespace

u32 inbox_ui_probe_stage;
bool inbox_ui_probe(D3D11Renderer& renderer, PopupScene&, bool (*capture)(const wchar_t*) noexcept) noexcept {
    auto& popup = probe.menu;
    if (!popup.items.data && !popup_scene_initialize(&popup, {probe.menu_items, 8}, {probe.menu_rows, 8},
                                                     {probe.menu_widgets, 2 + 8 * kPopupWidgetsPerItem},
                                                     {probe.menu_nodes, 2 + 8 * kPopupWidgetsPerItem})) {
        inbox_ui_probe_stage = 8;
        return false;
    }
    if (!capture || renderer.login_visible || !renderer.dpi || popup.active) {
        inbox_ui_probe_stage = 9;
        log_u64(LogLevel::error, "probe", "inbox_guard",
                (capture ? 0u : 1u) | (renderer.login_visible ? 2u : 0u) | (renderer.dpi ? 0u : 4u) |
                    (popup.active ? 8u : 0u));
        return false;
    }
    const PopupScene* old_popup = renderer.popup_scene;
    if (old_popup != &popup && !renderer_bind_popup_scene(&renderer, &popup)) {
        inbox_ui_probe_stage = 7;
        return false;
    }
    auto& p = probe;
    p.source = {&p, 5, 1, resolve, current};
    p.content = {};
    p.content.revision = 1;
    p.content.phase = UiInboxPhase::ready;
    p.content.this_server_available = true;
    p.content.has_more = true;
    const u32 width = renderer.width, height = renderer.height;
    const auto* old_settings = renderer.settings_scene;
    struct Restore {
        D3D11Renderer& renderer;
        PopupScene& popup;
        const PopupScene* old_popup;
        const UiSettingsScene* settings;
        u32 width, height;
        ~Restore() noexcept {
            popup_scene_close(&popup);
            renderer_bind_popup_scene(&renderer, old_popup);
            renderer.inbox_scene = nullptr;
            renderer_bind_settings_scene(&renderer, settings);
            ui_inbox_scene_release(&probe.scene);
            ui_settings_scene_release(&probe.dialog);
            renderer_resize(&renderer, width, height);
        }
    } restore{renderer, popup, old_popup, old_settings, width, height};
    const float scale = static_cast<float>(renderer.dpi) / 96.f;
    u32 stage = 0;
    bool valid = renderer_resize(&renderer, 1200, 970);
    const UiRect viewport{0, 0, 1200 / scale, 970 / scale};
    const UiRect below_header{312, 48, viewport.right, viewport.bottom};
    stage = 1;
    valid = valid && show_inbox(renderer, below_header, UiInboxTab::mentions, L"inbox-mentions.bmp", capture);
    stage = 2;
    valid = valid && show_inbox(renderer, below_header, UiInboxTab::unreads, L"inbox-unreads.bmp", capture);
    valid = valid && show_inbox(renderer, below_header, UiInboxTab::unreads, L"inbox-unreads-hover.bmp", capture, true);
    renderer.inbox_scene = nullptr;
    stage = 3;
    valid = valid && show_menu(renderer, popup, viewport, false, L"mute-durations.bmp", capture);
    stage = 4;
    valid = valid && show_menu(renderer, popup, viewport, true, L"notification-levels.bmp", capture);
    popup_scene_close(&popup);
    stage = 5;
    valid = valid && show_dialog(renderer, viewport, capture);
    inbox_ui_probe_stage = valid ? 0 : stage == 5 && dialog_failure ? dialog_failure : stage;
    if (!valid) {
        log_u64(LogLevel::error, "probe", "inbox_failure_line", stage);
        log_u64(LogLevel::error, "probe", "inbox_dialog_failure", dialog_failure);
    }
    log_write(valid ? LogLevel::info : LogLevel::error, "probe",
              valid ? "inbox_notification_controls_fixture_passed" : "inbox_notification_controls_fixture_failed");
    return valid;
}
} // namespace maicord
