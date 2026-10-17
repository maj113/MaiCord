#include "application_downloads_inactive.hpp"
#include "application_account_menu.hpp"
#include "application_account_logout.hpp"
#include "application_state.hpp"
#include "application_profile.hpp"
#include "application_composer.hpp"
#include "application_self_status.hpp"
#include "application_account_switch.hpp"
#include "maicord/ui/account_menu.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"

namespace maicord::account_menu_runtime_test {
using namespace app_detail;
u32 failures, refreshes, views, edits, copies;
GatewayClientSelfStatusSnapshot self_status;
u32 status_requests;
GatewayUserStatus requested_status;
bool settings;
bool logout_available, logout_blocked;
u32 logout_opens;
bool switch_available;
u32 switch_panels, switch_actions, switch_action;
UiAccountSwitcherPanel switch_panel;
u64 viewed_user, edited_user;
char copied[20];
u32 copied_bytes;
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}
struct KeyboardFixture {
    BYTE saved[256]{};
    bool captured;
    KeyboardFixture() noexcept : captured(GetKeyboardState(saved) != FALSE) {
        check(captured, "account menu runtime: synthetic keyboard fixture saves thread state");
    }
    bool apply(bool control = false) noexcept {
        BYTE keys[256]{};
        BYTE observed[256]{};
        if (control)
            keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
        return captured && SetKeyboardState(keys) && GetKeyboardState(observed) &&
               bytes_equal(keys, observed, sizeof(keys));
    }
    ~KeyboardFixture() noexcept {
        BYTE observed[256]{};
        check(!captured || (SetKeyboardState(saved) && GetKeyboardState(observed) &&
                            bytes_equal(saved, observed, sizeof(saved))),
              "account menu runtime: synthetic keyboard fixture restores exact thread state");
    }
};
void metric(const char* label, u64 value) noexcept {
    test_write_metric(label, value);
}
void run() noexcept {
    KeyboardFixture keyboard;
    check(keyboard.apply(), "account menu runtime: synthetic keyboard state has controlled modifiers");
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(g_app.account_ui != nullptr, "account menu runtime: isolated account owner allocates");
    if (!g_app.account_ui)
        return;
    UserRecord users[2]{};
    u32 order[2]{};
    u8 names[512]{};
    PopupItem items[9]{};
    PopupRowWidget rows[9]{};
    UiWidget widgets[20]{};
    UiLayoutNode nodes[20]{};
    g_app.plugin_account_scope_serial = 7;
    g_app.account.status.profile_id = 4;
    g_app.account_ui->chat_state.self_user_id = ~u64{0};
    check(user_store_initialize(&g_app.account_ui->user_store, {users, 2}, {order, 2},
                                        {names, 512}) &&
              user_store_upsert(&g_app.account_ui->user_store, ~u64{0},
                                        utf8_literal("Native Account"), utf8_literal("native.user"))
                  .ok() &&
              ui_scene_initialize_bootstrap(&g_app.shell_scene) &&
              ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::guild_channel,
                                           kGuildChannelShellLayoutMetrics, 1000, 700) &&
              popup_scene_initialize(&g_app.popup_scene, {items, 9}, {rows, 9}, {widgets, 20}, {nodes, 20}),
          "account menu runtime: borrowed source/user/popup banks initialize");
    const auto footer =
        ui_widget_manager_find(g_app.shell_scene.widgets, static_cast<u32>(UiSemantic::channel_footer) + 1);
    const auto* node = ui_widget_manager_layout_node(g_app.shell_scene.widgets, footer);
    if (!node) {
        check(false, "account menu runtime: actual footer exists");
        return;
    }
    const auto anchor = ui_rect_intersection(node->rectangle, node->clip_rectangle);
    g_app.context_focus = &g_app.shell_scene.widgets;
    g_app.keyboard_focus_visible = true;
    ui_widget_manager_focus(&g_app.shell_scene.widgets, footer);
    check(keyboard.apply(true) && (GetKeyState(VK_CONTROL) & 0x8000) &&
              !application_account_menu_key(VK_RETURN) && !g_app.popup_scene.active,
          "account menu runtime: explicit Ctrl guard never opens a footer menu");
    check(keyboard.apply() && application_account_menu_key(VK_RETURN) && g_app.popup_scene.active &&
              g_app.composer_menu_held_key == VK_RETURN,
          "account menu runtime: current keyboard footer opens menu once and retains opening key tail");
    check(ui_input_consume_key_tail(&g_app.composer_menu_held_key,
                                    {UiInputKind::key_down, VK_RETURN, true}) &&
              ui_input_consume_key_tail(&g_app.composer_menu_held_key, {UiInputKind::character, '\r'}) &&
              ui_input_consume_key_tail(&g_app.composer_menu_held_key, {UiInputKind::key_up, VK_RETURN}) &&
              !g_app.composer_menu_held_key && g_app.popup_scene.active && !views && !edits,
          "account menu runtime: held Return and translated character cannot activate newly opened profile "
          "row");
    application_account_menu_clear();
    check(g_app.context_focus == &g_app.shell_scene.widgets &&
              g_app.shell_scene.widgets.focused.slot == footer.slot &&
              g_app.shell_scene.widgets.focused.generation == footer.generation,
          "account menu runtime: close restores exact invoking footer keyboard generation");
    check(application_account_menu_open(footer, anchor) &&
              g_app.popup_owner_kind == PopupOwnerKind::account_menu &&
              utf8_equal(g_app.popup_scene.items[0].label, utf8_literal("Native Account")) &&
              g_app.popup_scene.items[0].status == 0,
          "account menu runtime: actual footer opens copied native identity without inventing desired "
          "status");
    MEMORY_BASIC_INFORMATION region{};
    check(g_app.account_menu &&
              VirtualQuery(g_app.account_menu, &region, sizeof(region)) == sizeof(region) &&
              region.RegionSize <= 4096 && application_account_menu_owner_bytes() <= 4096,
          "account menu runtime: lazy private menu owner commits at most one page");
    metric("account_menu_model_bytes=", sizeof(UiAccountMenuModel));
    metric("account_menu_owner_bytes=", application_account_menu_owner_bytes());
    metric("account_menu_owner_committed_bytes=", region.RegionSize);
    metric("account_menu_application_bytes=", sizeof(Application));
    bool changed = false;
    popup_scene_select_index(&g_app.popup_scene, 1, &changed);
    const u32 prior_opens = g_app.popup_scene.opens;
    check(user_store_upsert(&g_app.account_ui->user_store, ~u64{0},
                                    utf8_literal("Updated Account"), utf8_literal("updated.user"))
                  .ok() &&
              application_account_menu_synchronize() && g_app.popup_scene.opens == prior_opens + 1 &&
              utf8_equal(g_app.popup_scene.items[0].label, utf8_literal("Updated Account")) &&
              popup_scene_selected_action(g_app.popup_scene) ==
                  static_cast<u32>(UiAccountMenuAction::view_profile) &&
              g_app.popup_scene.selected_index == 1,
          "account menu runtime: identity refresh preserves exact View profile row rather than duplicate "
          "clickable header");
    check(popup_scene_select_index(&g_app.popup_scene, 2, &changed),
          "account menu runtime: real Edit profile row remains selectable after refresh");
    check(application_account_menu_action(static_cast<u32>(UiAccountMenuAction::edit_profile)) &&
              edits == 1 && edited_user == ~u64{0} && !g_app.popup_scene.active,
          "account menu runtime: Edit profile dispatches existing self-profile edit adapter after closing "
          "menu");
    check(application_account_menu_open(footer, anchor) &&
              application_account_menu_action(static_cast<u32>(UiAccountMenuAction::view_profile)) &&
              views == 1 && viewed_user == ~u64{0},
          "account menu runtime: View profile dispatches existing self profile flow");
    check(application_account_menu_open(footer, anchor) &&
              application_account_menu_action(static_cast<u32>(UiAccountMenuAction::view_profile_header)) &&
              views == 2 && viewed_user == ~u64{0},
          "account menu runtime: distinct clickable identity row delegates the same real profile flow");
    g_app.preferences.developer_mode = false;
    check(application_account_menu_open(footer, anchor) &&
              application_account_menu_action(static_cast<u32>(UiAccountMenuAction::copy_user_id)) &&
              copies == 0 && !g_app.popup_scene.active,
          "account menu runtime: Copy user ID is offered only with Developer Mode on");
    g_app.preferences.developer_mode = true;
    check(application_account_menu_open(footer, anchor) &&
              application_account_menu_action(static_cast<u32>(UiAccountMenuAction::copy_user_id)) &&
              copies == 1 && copied_bytes == 20 &&
              utf8_equal({copied, copied_bytes}, utf8_literal("18446744073709551615")),
          "account menu runtime: Copy user ID uses bounded exact20-digit formatting and typed clipboard "
          "output");
    self_status.profile_id = 4;
    self_status.self_user_id = ~u64{0};
    self_status.revision = 1;
    self_status.available = self_status.authoritative = true;
    self_status.status = GatewayUserStatus::invisible;
    check(application_account_menu_open(footer, anchor) && g_app.popup_scene.item_count == 9 &&
              g_app.popup_scene.items[0].status == 4 &&
              (g_app.popup_scene.items[7].flags & popup_item_checked),
          "account menu runtime: authoritative self Invisible produces native rows and exact checked "
          "desired status");
    self_status.status = GatewayUserStatus::dnd;
    ++self_status.revision;
    check(application_account_menu_synchronize() &&
              (g_app.popup_scene.items[6].flags & popup_item_checked) &&
              g_app.popup_scene.items[0].status == 3,
          "account menu runtime: remote status revision synchronizes shared menu/avatar status glyphs");
    check(application_account_menu_action(static_cast<u32>(UiAccountMenuAction::status_idle)) &&
              status_requests == 1 && requested_status == GatewayUserStatus::idle &&
              !g_app.popup_scene.active,
          "account menu runtime: native Idle selection enters actual status broker contract after closing "
          "popup");
    check(application_account_menu_open(footer, anchor),
          "account menu runtime: stale status fixture opens");
    self_status.available = false;
    ++self_status.revision;
    check(application_account_menu_action(static_cast<u32>(UiAccountMenuAction::status_online)) &&
              status_requests == 1 && !g_app.popup_scene.active,
          "account menu runtime: lost availability rechecks backend before any status request");
    self_status = {};
    logout_available = true;
    check(application_account_menu_open(footer, anchor) && g_app.popup_scene.item_count == 6 &&
              g_app.popup_scene.items[5].action == static_cast<u32>(UiAccountMenuAction::log_out) &&
              application_account_menu_action(static_cast<u32>(UiAccountMenuAction::log_out)) &&
              logout_opens == 1 && !g_app.popup_scene.active,
          "account menu runtime: available logout row closes its popup before opening confirmation");
    switch_available = true;
    check(application_account_menu_open(footer, anchor) && g_app.popup_scene.item_count == 7 &&
              g_app.popup_scene.items[5].action == static_cast<u32>(UiAccountMenuAction::switch_accounts) &&
              g_app.popup_scene.items[6].action == static_cast<u32>(UiAccountMenuAction::log_out),
          "account menu runtime: Switch accounts sits above Log out");
    const u32 add = static_cast<u32>(UiAccountSwitcherAction::add_account);
    const u32 manage = static_cast<u32>(UiAccountSwitcherAction::manage_accounts);
    const u32 back = static_cast<u32>(UiAccountSwitcherAction::back);
    const u32 first_account = static_cast<u32>(UiAccountSwitcherAction::account_first);
    check(application_account_menu_action(static_cast<u32>(UiAccountMenuAction::switch_accounts)) &&
              g_app.popup_scene.active && g_app.popup_scene.item_count == 4 &&
              g_app.popup_scene.items[0].media_id == ~u64{0} &&
              (g_app.popup_scene.items[0].flags & (popup_item_checked | popup_item_avatar)) ==
                  (popup_item_checked | popup_item_avatar) &&
              !(g_app.popup_scene.items[1].flags & popup_item_checked) &&
              g_app.popup_scene.items[2].action == add && g_app.popup_scene.items[3].action == manage &&
              switch_actions == 0,
          "account menu runtime: Switch accounts opens the saved account panel in place");
    check(application_account_menu_action(manage) && g_app.popup_scene.active &&
              utf8_equal(g_app.popup_scene.items[1].label, utf8_literal("Log out of Second")) &&
              (g_app.popup_scene.items[1].flags & popup_item_danger) && g_app.popup_scene.items[3].action == back,
          "account menu runtime: Manage accounts lists a log out row per account");
    check(application_account_menu_action(back) && g_app.popup_scene.active &&
              g_app.popup_scene.items[3].action == manage,
          "account menu runtime: Back returns to the account list");
    check(application_account_menu_action(first_account + 1) && switch_actions == 1 &&
              switch_action == first_account + 1 && switch_panel == UiAccountSwitcherPanel::switch_accounts &&
              !g_app.popup_scene.active,
          "account menu runtime: choosing an account closes the menu before switching");
    check(application_account_menu_open(footer, anchor) && g_app.popup_scene.item_count == 7,
          "account menu runtime: reopening starts from the menu, not the last panel");
    application_account_menu_clear();
    switch_available = false;
    logout_blocked = true;
    check(!application_account_menu_open(footer, anchor) && logout_opens == 1,
          "account menu runtime: committed logout blocks a second account action");
    logout_blocked = false;
    logout_available = false;
    check(application_account_menu_open(footer, anchor),
          "account menu runtime: scope invalidation fixture opens");
    ++g_app.plugin_account_scope_serial;
    check(application_account_menu_action(static_cast<u32>(UiAccountMenuAction::copy_user_id)) &&
              copies == 1 && !g_app.popup_scene.active,
          "account menu runtime: expired account scope cannot copy prior user identity");
    check(g_app.context_focus == nullptr,
          "account menu runtime: expired source scope cannot regain recycled footer focus");
    check(application_account_menu_open(footer, anchor),
          "account menu runtime: replacement popup fixture opens");
    const PopupItem replacement{utf8_literal("Other menu"), 77, popup_item_enabled};
    popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, 55, {&replacement, 1}, {30, 40, 31, 41},
                     {0, 0, 1000, 700}, 220);
    g_app.popup_owner_kind = PopupOwnerKind::channel;
    check(application_account_menu_synchronize() && g_app.popup_scene.active &&
              g_app.popup_scene.owner_id == 55,
          "account menu runtime: stale private owner never closes a replacement popup bank");
    popup_scene_close(&g_app.popup_scene);
    g_app.popup_owner_kind = PopupOwnerKind::none;
    settings = true;
    check(!application_account_menu_open(footer, anchor),
          "account menu runtime: opaque Settings prevents underlying account popover");
    settings = false;
    check(application_account_menu_open(footer, anchor),
          "account menu runtime: generation retirement fixture opens");
    application_account_menu_clear();
    ui_scene_release(&g_app.shell_scene);
    check(!application_account_menu_open(footer, anchor) && application_account_menu_synchronize(),
          "account menu runtime: released footer generation cannot revive a cached menu source");
    application_account_menu_release();
    popup_scene_release(&g_app.popup_scene);
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
}
} // namespace maicord::account_menu_runtime_test
namespace maicord {
void renderer_bind_focus_owner(D3D11Renderer* renderer, const UiWidgetManager* owner) noexcept {
    renderer->focus_owner = owner;
}
bool clipboard_write_utf8(HWND, Utf8View text) noexcept {
    if (text.bytes > sizeof(account_menu_runtime_test::copied))
        return false;
    copy_bytes(account_menu_runtime_test::copied, text.data, text.bytes);
    account_menu_runtime_test::copied_bytes = text.bytes;
    ++account_menu_runtime_test::copies;
    return true;
}
namespace app_detail {
bool application_account_logout_available() noexcept {
    return account_menu_runtime_test::logout_available;
}
bool application_account_logout_blocked() noexcept {
    return account_menu_runtime_test::logout_blocked;
}
bool application_account_logout_open() noexcept {
    ++account_menu_runtime_test::logout_opens;
    return true;
}
bool application_account_switch_available() noexcept {
    return account_menu_runtime_test::switch_available;
}
bool application_account_switch_panel(UiAccountSwitcherModel* model, u64 owner,
                                      UiAccountSwitcherPanel panel) noexcept {
    const UiAccountSwitcherAccount accounts[2]{
        {~u64{0}, utf8_literal("Native Account"), utf8_literal("native.user"), 0, false, true, false},
        {42, utf8_literal("Second"), utf8_literal("second.user"), 1, false, false, false}};
    ++account_menu_runtime_test::switch_panels;
    return ui_account_switcher_build(model, owner, panel, {accounts, 2}, true);
}
bool application_account_switch_action(UiAccountSwitcherPanel panel, u32 action) noexcept {
    ++account_menu_runtime_test::switch_actions;
    account_menu_runtime_test::switch_panel = panel;
    account_menu_runtime_test::switch_action = action;
    return true;
}
GatewayClientSelfStatusSnapshot application_self_status_snapshot() noexcept {
    return account_menu_runtime_test::self_status;
}
bool application_self_status_request(GatewayUserStatus status) noexcept {
    ++account_menu_runtime_test::status_requests;
    account_menu_runtime_test::requested_status = status;
    return true;
}
bool application_settings_active() noexcept {
    return account_menu_runtime_test::settings;
}
bool application_custom_status_available() noexcept {
    return false;
}
bool application_custom_status_snapshot(SelfCustomStatus*) noexcept {
    return false;
}
bool application_custom_status_request(const SelfCustomStatus*) noexcept {
    return false;
}
bool application_custom_status_open() noexcept {
    return false;
}
bool application_friends_requests_active() noexcept {
    return false;
}
UiRect application_viewport_dips() noexcept {
    return {0, 0, 1000, 700};
}
bool refresh_account_frame(bool) noexcept {
    ++account_menu_runtime_test::refreshes;
    return true;
}
void application_tooltip_clear() noexcept {
}
void set_popup_owner(PopupOwnerKind kind) noexcept {
    g_app.popup_owner_kind = kind;
}
bool dismiss_popup() noexcept {
    popup_scene_close(&g_app.popup_scene);
    set_popup_owner(PopupOwnerKind::none);
    return true;
}
bool application_profile_open(u64 user, UiRect, ProfileAnchor) noexcept {
    ++account_menu_runtime_test::views;
    account_menu_runtime_test::viewed_user = user;
    return true;
}
bool application_profile_open_edit(u64 user, UiRect, ProfileAnchor) noexcept {
    ++account_menu_runtime_test::edits;
    account_menu_runtime_test::edited_user = user;
    return true;
}
} // namespace app_detail
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_account_menu_runtime_entry() noexcept {
    maicord::account_menu_runtime_test::run();
    constexpr char passed[] = "PASS account menu runtime\n", failed[] = "FAIL account menu runtime\n";
    const bool failure = maicord::account_menu_runtime_test::failures != 0;
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failure ? failed : passed, sizeof(passed) - 1, &written,
              nullptr);
    ExitProcess(failure ? 1 : 0);
}
