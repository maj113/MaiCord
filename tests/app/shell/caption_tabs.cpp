// The title bar's channel tabs end to end: the caption controller's real pointer and key handling over the real tab
// model and strip layout, in the headless window. Navigation, popups, the renderer and updates are observed at their
// host boundaries.
#include "../../support/window_fixture.hpp"
#include "../../support/test_output.hpp"
#include "../../support/model_store_fixtures.hpp"
#include "maicord/model/guild/permission_bits.hpp"
#include "shell/caption.hpp"
#include "shell/state.hpp"
#include "shell/tabs.hpp"
#include "shell/window_state.hpp"
#include "shell/frame_presentation.hpp"
#include "shell/tooltip.hpp"
#include "update/updates.hpp"
#include "chat/application_channel_memory.hpp"
#include "chat/messages/interaction.hpp"
#include "maicord/app/settings/client_preferences.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/window/key.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/ui/accessibility/accessibility.hpp"
#include "../../../src/app/account/account_ui_runtime.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::caption_tabs_test {
using namespace app_detail;
using window_test::pointer_down;
using window_test::pointer_move;
using window_test::pointer_up;

u32 failures, menu_opens, list_opens;
u32 menu_index;
UiRect menu_anchor;
ChannelMemory memory;
u64 memory_profile;
AccountUiRuntime* ui;
GuildRecord guilds[2];
u16 guild_order[2];
ChannelRecord channels[24];
u16 channel_order[24];
RoleRecord roles[2];
PermissionOverwriteRecord overwrites[2];
u8 names[1024];
UiAccessNode access_nodes[64];
char access_text[2048];
u32 access_slots[128], access_keys[128];
constexpr u64 rights = kPermissionViewChannel | kPermissionReadMessageHistory | kPermissionSendMessages;

void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}

ChannelMemoryTab saved(u64 channel, const char* label) noexcept {
    ChannelMemoryTab tab{channel, 10, 0, {}};
    while (label[tab.label_bytes])
        tab.label[tab.label_bytes] = label[tab.label_bytes], ++tab.label_bytes;
    return tab;
}

bool fixture(u32 width) noexcept {
    clear_bytes(&g_app, sizeof(g_app));
    g_app.preferences = client_preferences_defaults();
    g_app.preferences.tabs_enabled = true;
    application_tabs_reset();
    clear_bytes(&memory, sizeof(memory));
    memory_profile = 9;
    menu_opens = list_opens = 0;
    clear_bytes(&g_window_state, sizeof(g_window_state));
    g_window_state.dpi = 96;
    g_window_state.client_width = static_cast<i32>(width);
    g_window_state.client_height = 600;
    g_app.window = window_test::open();
    g_app.account_ui = ui;
    g_app.account.instance = ui;
    g_app.account.status.running = true;
    g_app.account.status.profile_id = 9;
    clear_bytes(guilds, sizeof(guilds));
    clear_bytes(channels, sizeof(channels));
    auto& state = ui->chat_state;
    if (!chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, 24}, {channel_order, 24},
                                     {names, sizeof(names)}) ||
        !chat_state_store_bind_permissions(&state, {roles, 2}, {overwrites, 2}) ||
        !chat_state_store_upsert_guild(&state, {10, utf8_literal("Guild"), GuildMutationKind::create, 99}).ok() ||
        !chat_state_store_upsert_role(&state, {10, 10, rights, {}}) ||
        !chat_state_store_set_self_roles(&state, 10, 99, 5, {}))
        return false;
    static const Utf8View first_names[]{utf8_literal("one"),  utf8_literal("two"), utf8_literal("three"),
                                        utf8_literal("four"), utf8_literal("five"), utf8_literal("six")};
    for (u64 id = 100; id < 120; ++id)
        if (!chat_state_store_upsert_channel(
                 &state, {id, 10, 0, id < 106 ? first_names[id - 100] : utf8_literal("General"), 0, 0, 0})
                 .ok())
            return false;
    chat_state_store_recompute_guild_permissions(&state, 10);
    state.self_user_id = 99;
    if (!chat_state_store_select_channel(&state, 100).ok())
        return false;
    application_caption_enable();
    g_app.renderer.dpi = 96;
    g_app.renderer.width = width;
    g_app.renderer.caption_height = application_caption_pixels(96);
    return g_app.window != nullptr && g_app.renderer.caption_height != 0;
}

// The bar as the accessibility tree and the tooltip see it: content DIPs, so the rows above the content are negative.
UiCaptionBarLayout bar() noexcept {
    return application_caption_layout();
}
float lifted(const UiCaptionBarLayout& layout, float y) noexcept {
    return y + layout.height;
}
float centre_x(const UiRect& r) noexcept {
    return (r.left + r.right) / 2;
}
float centre_y(const UiCaptionBarLayout& layout, const UiRect& r) noexcept {
    return lifted(layout, (r.top + r.bottom) / 2);
}
float tab_x(u32 index) noexcept {
    const auto layout = bar();
    return centre_x(ui_tab_strip_tab(layout.tabs, index));
}
float tab_y(u32 index) noexcept {
    const auto layout = bar();
    return centre_y(layout, ui_tab_strip_tab(layout.tabs, index));
}
bool send(const WindowEvent& event) noexcept {
    return application_caption_event(event);
}
void click(float x, float y, PointerButton button = PointerButton::left) noexcept {
    send(pointer_move(x, y));
    send(pointer_down(x, y, button));
    send(pointer_up(x, y, button));
}
u64 selected() noexcept {
    return ui->chat_state.selected_channel_id;
}
// The view of the strip now, settled (the motion host is instant here).
const UiTabStripView* view() noexcept {
    return application_caption_state().tabs;
}
u64 memory_tab(u32 i) noexcept {
    return memory.tabs[i].channel_id;
}
void seed(u32 count) noexcept {
    const char* const labels[]{"one", "two", "three", "four", "five", "six"};
    ChannelMemoryTab tabs[6];
    for (u32 i = 0; i < count; ++i)
        tabs[i] = saved(100 + i, labels[i]);
    channel_memory_set_tabs(&memory, {tabs, count});
    view();
}

void clicks() noexcept {
    check(fixture(1000), "caption tabs: fixture");
    seed(4);
    check(application_tabs_count() == 4 && view() && view()->count == 4 && (view()->items[0].flags & kUiTabActive),
          "caption tabs: the saved tabs show, the one for the selected channel active");
    const float x = tab_x(1), y = tab_y(1);
    send(pointer_move(x, y));
    check(view()->hovered.kind == UiTabHitKind::tab && view()->hovered.index == 1 &&
              application_caption_state().hovered == UiCaptionPart::tab,
          "caption tabs: the pointer over a tab hovers it");
    check(send(pointer_down(x, y)) && selected() == 101 && window_pointer_captured(g_app.window) &&
              (view()->items[1].flags & kUiTabActive) && !(view()->items[0].flags & kUiTabActive),
          "caption tabs: a press activates the tab and takes the pointer");
    check(send(pointer_up(x, y)) && !window_pointer_captured(g_app.window) && selected() == 101,
          "caption tabs: the release gives the pointer back");
    // "+" pins the channel that is selected when it is not a tab yet.
    check(chat_state_store_select_channel(&ui->chat_state, 105).ok() && view()->plus_enabled,
          "caption tabs: the pin is on for a channel without a tab");
    auto layout = bar();
    click(centre_x(layout.tabs.plus), centre_y(layout, layout.tabs.plus));
    check(application_tabs_count() == 5 && selected() == 105 && application_tabs_channel(4) == 105 &&
              !view()->plus_enabled && memory.tab_count == 5,
          "caption tabs: \"+\" pins the current channel and persists it");
    layout = bar();
    click(centre_x(layout.tabs.plus), centre_y(layout, layout.tabs.plus));
    check(application_tabs_count() == 5, "caption tabs: the disabled \"+\" pins nothing");
    // Middle click closes.
    click(tab_x(2), tab_y(2), PointerButton::middle);
    check(view()->count == 4 && !application_tabs_contains(102) && memory.tab_count == 4 && selected() == 105,
          "caption tabs: a middle click closes the tab without touching the selection");
    // The X of the active tab closes it and shows its neighbour.
    layout = bar();
    const UiRect close = ui_tab_strip_close(layout.tabs, 3);
    check(application_tabs_channel(3) == 105 && ui_rect_has_area(close), "caption tabs: the active tab shows its close button");
    click(centre_x(close), centre_y(layout, close));
    check(view()->count == 3 && !application_tabs_contains(105) && selected() == 103,
          "caption tabs: the close button closes the active tab and shows its left neighbour");
}

void drags() noexcept {
    check(fixture(1000), "caption tabs drag: fixture");
    seed(3);
    const float y = tab_y(0);
    const float width = centre_x(ui_tab_strip_tab(bar().tabs, 1)) - centre_x(ui_tab_strip_tab(bar().tabs, 0));
    float x = tab_x(0);
    send(pointer_move(x, y));
    send(pointer_down(x, y));
    send(pointer_move(x + 2, y));
    check(view() && !(view()->items[0].flags & kUiTabDragging) && application_tabs_channel(0) == 100,
          "caption tabs drag: a move under the threshold stays a click");
    send(pointer_move(x + width * 0.4f, y));
    check((view()->items[0].flags & kUiTabDragging) && application_tabs_channel(0) == 100,
          "caption tabs drag: past the threshold the tab follows the pointer inside its own slot");
    send(pointer_move(x + width * 1.1f, y));
    check(application_tabs_channel(0) == 101 && application_tabs_channel(1) == 100 &&
              (view()->items[1].flags & kUiTabDragging),
          "caption tabs drag: crossing the neighbour's midpoint swaps them");
    send(pointer_up(x + width * 1.1f, y));
    check(!(view()->items[1].flags & kUiTabDragging) && memory_tab(0) == 101 && memory_tab(1) == 100 &&
              !window_pointer_captured(g_app.window),
          "caption tabs drag: the drop keeps the order, persists it and ends the drag");
    // Esc puts the dragged tab back.
    x = tab_x(2);
    send(pointer_move(x, y));
    send(pointer_down(x, y));
    send(pointer_move(x - width * 0.6f, y));
    send(pointer_move(x - width * 1.4f, y));
    check(application_tabs_channel(1) == 102 && application_tabs_channel(2) == 100,
          "caption tabs drag: dragging left swaps with the neighbour on the left");
    check(send(window_test::key_down(key::escape)) && application_tabs_channel(0) == 101 &&
              application_tabs_channel(1) == 100 && application_tabs_channel(2) == 102 &&
              !(view()->items[2].flags & kUiTabDragging) && !window_pointer_captured(g_app.window),
          "caption tabs drag: Esc cancels the drag and restores the order");
    // A lost capture cancels it the same way.
    x = tab_x(0);
    send(pointer_move(x, y));
    send(pointer_down(x, y));
    send(pointer_move(x + width * 1.1f, y));
    const bool swapped = application_tabs_channel(0) == 100;
    send(window_test::capture_lost());
    check(swapped && application_tabs_channel(0) == 101 && !(view()->items[0].flags & kUiTabDragging),
          "caption tabs drag: a lost capture cancels the drag");
}

void menus() noexcept {
    check(fixture(1000), "caption tabs menus: fixture");
    seed(4);
    const float x = tab_x(1), y = tab_y(1);
    send(pointer_move(x, y));
    check(send(pointer_down(x, y, PointerButton::right)) && menu_opens == 1 && menu_index == 1 &&
              menu_anchor.bottom <= 0 && menu_anchor.right > menu_anchor.left && send(pointer_up(x, y, PointerButton::right)),
          "caption tabs menus: a right click opens the tab's menu at the tab and takes the press");
    check(application_tabs_menu_action(popup_action_tab_close_right, 101) && view()->count == 2 &&
              application_tabs_contains(100) && application_tabs_contains(101),
          "caption tabs menus: Close tabs to the right");
    check(application_tabs_open_channel(102, true) && application_tabs_menu_action(popup_action_tab_close_others, 102) &&
              view()->count == 1 && application_tabs_contains(102) && selected() == 102,
          "caption tabs menus: Close other tabs keeps the tab and shows it when the active one went");
    check(application_tabs_open_channel(103, true) && application_tabs_menu_action(popup_action_tab_close, 103) &&
              view()->count == 1 && !application_tabs_contains(103),
          "caption tabs menus: Close");
}

void overflow() noexcept {
    check(fixture(624), "caption tabs overflow: fixture");
    seed(1);
    bool opened = true;
    for (u64 id = 101; id < 116; ++id)
        opened = application_tabs_open_channel(id, true) && opened;
    const auto layout = bar();
    check(opened && view()->count == kUiTabCapacity && layout.tabs.overflowed && layout.tabs.shown < kUiTabCapacity &&
              ui_rect_has_area(layout.tabs.overflow),
          "caption tabs overflow: sixteen tabs do not fit at the narrowest window, and the \"...\" appears");
    click(centre_x(layout.tabs.overflow), centre_y(layout, layout.tabs.overflow));
    check(list_opens == 1 && menu_anchor.bottom <= 0, "caption tabs overflow: the \"...\" opens the list of every tab");
    check(!application_tabs_open_channel(116, true), "caption tabs overflow: a seventeenth tab is refused");
}

UiAccessNode* find_node(UiAccessTree& tree, UiAccessRole role, Utf8View name, u32 under = 0) noexcept {
    for (u32 i = 1; i < tree.count; ++i) {
        UiAccessNode& node = tree.nodes[i];
        bool inside = under == 0;
        for (u32 p = node.parent; !inside && p != kUiAccessNone && p != 0; p = tree.nodes[p].parent)
            inside = p == under;
        if (node.role == role && inside && utf8_equal(ui_access_name(tree, i), name))
            return &node;
    }
    return nullptr;
}

void accessibility() noexcept {
    check(fixture(1000), "caption tabs accessibility: fixture");
    seed(3);
    static UiAccessTree tree{};
    clear_bytes(&tree, sizeof(tree));
    check(ui_access_tree_initialize(&tree, {access_nodes, 64}, {access_text, sizeof(access_text)}, {access_slots, 128},
                                    {access_keys, 128}) &&
              ui_access_tree_reset(&tree, utf8_literal("MaiCord"), {0, -32, 1000, 600}),
          "caption tabs accessibility: tree");
    UiAccessNodeDesc group{};
    group.key = ui_access_key(5, 0);
    group.rectangle = {0, -32, 1000, 0};
    group.name = utf8_literal("Title bar");
    group.role = UiAccessRole::group;
    group.surface = 5;
    const u32 parent = ui_access_add(&tree, 0, group);
    application_caption_describe_tabs(&tree, parent, 5);
    check(parent != kUiAccessNone && view() && tree.count > 3, "caption tabs accessibility: the nodes are added");
    const UiAccessNode* list = find_node(tree, UiAccessRole::tab_list, utf8_literal("Open tabs"));
    UiAccessNode* one = find_node(tree, UiAccessRole::tab, utf8_literal("one"));
    UiAccessNode* two = find_node(tree, UiAccessRole::tab, utf8_literal("two"));
    check(list && one && two, "caption tabs accessibility: the list and its named tabs exist");
    if (!list || !one || !two)
        return;
    check(list && one && two && find_node(tree, UiAccessRole::tab, utf8_literal("three")) &&
              (one->state & ui_access_selected) && !(two->state & ui_access_selected) &&
              (two->state & ui_access_selectable) && (two->actions & ui_access_action_select) &&
              (two->actions & ui_access_action_invoke) && one->parent == static_cast<u32>(list - tree.nodes.data),
          "caption tabs accessibility: a tab list of named tabs, the active one selected, each selectable and invokable");
    check(find_node(tree, UiAccessRole::button, utf8_literal("Close tab"), static_cast<u32>(one - tree.nodes.data)),
          "caption tabs accessibility: each tab has a close button");
    const UiAccessNode* plus = nullptr;
    for (u32 i = 1; i < tree.count; ++i)
        if (tree.nodes[i].role == UiAccessRole::button && tree.nodes[i].item == application_caption_tab_item({UiTabHitKind::plus, 0}))
            plus = &tree.nodes[i];
    check(plus && utf8_equal(ui_access_name(tree, static_cast<u32>(plus - tree.nodes.data)), ui_tab_strip_hit_name(*view(), {UiTabHitKind::plus, 0})),
          "caption tabs accessibility: the \"+\" button is named as its tooltip");
    check(application_caption_access(two->item) && selected() == 101 && (view()->items[1].flags & kUiTabActive),
          "caption tabs accessibility: invoking a tab selects its channel");
    const UiAccessNode* close = nullptr;
    for (u32 i = 1; i < tree.count; ++i)
        if (tree.nodes[i].item == application_caption_tab_item({UiTabHitKind::close, 2}))
            close = &tree.nodes[i];
    check(close && application_caption_access(close->item) && view()->count == 2 && !application_tabs_contains(102),
          "caption tabs accessibility: invoking a close button closes its tab");
}

void run() noexcept {
    clicks();
    drags();
    menus();
    overflow();
    accessibility();
    window_test::close();
}
} // namespace maicord::caption_tabs_test

namespace maicord {
namespace app_detail {
bool application_navigate_known_message(u64 channel_id, u64) noexcept {
    using namespace caption_tabs_test;
    return chat_state_store_select_channel(&ui->chat_state, channel_id).ok();
}
bool application_channel_navigable(u64 channel_id) noexcept {
    using namespace caption_tabs_test;
    const auto* channel = chat_state_store_find_channel(ui->chat_state, channel_id);
    return channel && channel->permissions_resolved && (channel->actions & channel_action_view);
}
bool application_friends_active() noexcept {
    return false;
}
u64 application_channel_memory_profile() noexcept {
    return caption_tabs_test::memory_profile;
}
ConstSlice<ChannelMemoryTab> application_channel_memory_tabs() noexcept {
    return {caption_tabs_test::memory.tabs, caption_tabs_test::memory.tab_count};
}
void application_channel_memory_set_tabs(ConstSlice<ChannelMemoryTab> tabs) noexcept {
    channel_memory_set_tabs(&caption_tabs_test::memory, tabs);
}
void application_resize_client() noexcept {
}
// Motion settles at once; the hover groups run the shared transitions.
float application_motion_tick(UiMotionTransition*, const UiMotionScope&, float target, u64, const UiMotionStyle&) noexcept {
    return target;
}
bool application_motion_hover_tick(UiMotionHover* hover, u64 id, u64 now, const UiMotionStyle& style) noexcept {
    return ui_motion_hover_tick(hover, id, now, style, false);
}
void application_tooltip_clear() noexcept {
}
bool dismiss_popup() noexcept {
    return true;
}
bool application_tab_menu_open(u32 index, const UiRect& anchor) noexcept {
    ++caption_tabs_test::menu_opens;
    caption_tabs_test::menu_index = index;
    caption_tabs_test::menu_anchor = anchor;
    return true;
}
bool application_tab_list_open(const UiRect& anchor) noexcept {
    ++caption_tabs_test::list_opens;
    caption_tabs_test::menu_anchor = anchor;
    return true;
}
bool application_input_blocked() noexcept {
    return false;
}
bool application_settings_open() noexcept {
    return true;
}
bool application_settings_open_page(UiPageKey) noexcept {
    return true;
}
ApplicationUpdatesView application_updates_view() noexcept {
    return {};
}
bool application_updates_restart() noexcept {
    return false;
}
} // namespace app_detail
bool renderer_set_caption(D3D11Renderer*, const UiCaptionBarState&, const UiCaptionTitle&) noexcept {
    return false;
}
void renderer_set_caption_height(D3D11Renderer* renderer, u32 height) noexcept {
    renderer->caption_height = height;
}
bool app_icon_decode(u32, u32*) noexcept {
    return false;
}
bool renderer_set_caption_icon(D3D11Renderer*, const u32*, u32) noexcept {
    return false;
}
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_application_caption_tabs_entry() noexcept {
    using namespace maicord;
    caption_tabs_test::ui = static_cast<app_detail::AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(app_detail::AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!caption_tabs_test::ui)
        ExitProcess(1);
    caption_tabs_test::run();
    VirtualFree(caption_tabs_test::ui, 0, MEM_RELEASE);
    ExitProcess(caption_tabs_test::failures ? 1 : 0);
}
