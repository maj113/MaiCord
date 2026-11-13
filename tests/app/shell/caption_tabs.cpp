// The title bar's tabs end to end: the caption controller's real pointer and key handling over the real tab model
// (channel tabs and media tabs over a stand-in page store) and strip layout, in the headless window. Navigation,
// popups, the renderer and updates are observed at their host boundaries.
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
#include "media_page_fake.hpp"
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
u64 blocked_channel;
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
    media_page_fake::reset();
    clear_bytes(&memory, sizeof(memory));
    memory_profile = 9;
    menu_opens = list_opens = 0;
    blocked_channel = 0;
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
    // A frame ran since the last input: it advanced the strip, whose view the hit tests read.
    application_caption_state();
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
    check(application_tabs_count() == 5 && selected() == 105 && application_tabs_key(4) == 105 &&
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
    check(application_tabs_key(3) == 105 && ui_rect_has_area(close), "caption tabs: the active tab shows its close button");
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
    check(view() && !(view()->items[0].flags & kUiTabDragging) && application_tabs_key(0) == 100,
          "caption tabs drag: a move under the threshold stays a click");
    send(pointer_move(x + width * 0.4f, y));
    check((view()->items[0].flags & kUiTabDragging) && application_tabs_key(0) == 100,
          "caption tabs drag: past the threshold the tab follows the pointer inside its own slot");
    send(pointer_move(x + width * 1.1f, y));
    check(application_tabs_key(0) == 101 && application_tabs_key(1) == 100 &&
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
    check(application_tabs_key(1) == 102 && application_tabs_key(2) == 100,
          "caption tabs drag: dragging left swaps with the neighbour on the left");
    check(send(window_test::key_down(key::escape)) && application_tabs_key(0) == 101 &&
              application_tabs_key(1) == 100 && application_tabs_key(2) == 102 &&
              !(view()->items[2].flags & kUiTabDragging) && !window_pointer_captured(g_app.window),
          "caption tabs drag: Esc cancels the drag and restores the order");
    // A lost capture cancels it the same way.
    x = tab_x(0);
    send(pointer_move(x, y));
    send(pointer_down(x, y));
    send(pointer_move(x + width * 1.1f, y));
    const bool swapped = application_tabs_key(0) == 100;
    send(window_test::capture_lost());
    check(swapped && application_tabs_key(0) == 101 && !(view()->items[0].flags & kUiTabDragging),
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
    check(opened && view()->count == kUiTabCapacity && layout.tabs.overflowed &&
              ui_rect_has_area(layout.tabs.overflow),
          "caption tabs overflow: sixteen tabs do not fit at the narrowest window, and the \"...\" appears");
    click(centre_x(layout.tabs.overflow), centre_y(layout, layout.tabs.overflow));
    check(list_opens == 1 && menu_anchor.bottom <= 0, "caption tabs overflow: the \"...\" opens the list of every tab");
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

// The bar's accessibility tree, built afresh.
UiAccessTree access_tree;
bool describe_bar() noexcept {
    UiAccessTree& tree = access_tree;
    clear_bytes(&tree, sizeof(tree));
    if (!ui_access_tree_initialize(&tree, {access_nodes, 64}, {access_text, sizeof(access_text)}, {access_slots, 128},
                                   {access_keys, 128}) ||
        !ui_access_tree_reset(&tree, utf8_literal("MaiCord"), {0, -32, 1000, 600}))
        return false;
    UiAccessNodeDesc group{};
    group.key = ui_access_key(5, 0);
    group.rectangle = {0, -32, 1000, 0};
    group.name = utf8_literal("Title bar");
    group.role = UiAccessRole::group;
    group.surface = 5;
    const u32 parent = ui_access_add(&tree, 0, group);
    application_caption_describe_tabs(&tree, parent, 5);
    return parent != kUiAccessNone;
}

constexpr u64 page_key(u8 id) noexcept {
    return (1ull << 63) | id;
}

// Media tabs: pages the tabs open, show, hide, dim and close, kept out of the saved channel tabs.
void media() noexcept {
    using namespace media_page_fake;
    check(fixture(1000), "caption tabs media: fixture");
    seed(2);
    check(!view()->plus_enabled, "caption tabs media: the pin is off for a channel that is a tab");
    viewer_open = true;
    viewer_source = source(MediaPageKind::image, "cat.png");
    check(view()->plus_enabled, "caption tabs media: the pin is on while a message viewer is open");
    auto layout = bar();
    click(centre_x(layout.tabs.plus), centre_y(layout, layout.tabs.plus));
    check(application_tabs_count() == 3 && application_tabs_key(2) == page_key(1) && shown == 1 && !viewer_open &&
              view()->items[2].icon == UiIcon::image && (view()->items[2].flags & kUiTabActive) &&
              !(view()->items[0].flags & kUiTabActive) &&
              utf8_equal(application_tabs_label(2), utf8_literal("cat.png")) && memory.tab_count == 2 &&
              selected() == 100 && !view()->plus_enabled,
          "caption tabs media: \"+\" turns the open viewer into the active page tab, which is not saved");
    click(tab_x(0), tab_y(0));
    check(shown == 0 && pages[1].open && (view()->items[0].flags & kUiTabActive) &&
              !(view()->items[2].flags & kUiTabActive),
          "caption tabs media: a channel tab hides the page and keeps it");
    click(tab_x(2), tab_y(2));
    check(shown == 1 && selected() == 100 && (view()->items[2].flags & kUiTabActive) &&
              !(view()->items[0].flags & kUiTabActive),
          "caption tabs media: a media tab shows its page over the channel");
    check(application_tabs_open_media(source(MediaPageKind::video, "clip.mp4"), true) &&
              application_tabs_open_media(source(MediaPageKind::audio, "song.mp3"), true) && shown == 1 &&
              view()->items[3].icon == UiIcon::video && utf8_equal(application_tabs_label(3), utf8_literal("clip.mp4")) &&
              view()->items[4].icon == UiIcon::music &&
              utf8_equal(application_tabs_label(4), utf8_literal("song.mp3")) &&
              !(view()->items[3].flags & kUiTabActive),
          "caption tabs media: background video and audio tabs take their icon and label and leave the page that shows");
    pages[1].missing = pages[2].missing = true;
    check((view()->items[3].flags & kUiTabMissing) && (view()->items[2].flags & kUiTabMissing) &&
              view()->items[2].icon == UiIcon::image_off,
          "caption tabs media: a page that cannot load again dims its tab");
    pages[1].missing = pages[2].missing = false;
    check(!(view()->items[3].flags & kUiTabMissing) && !(view()->items[2].flags & kUiTabMissing) &&
              view()->items[2].icon == UiIcon::image,
          "caption tabs media: the dimming follows the page");
    // A page that owns the player: audio_lines while it plays; its icon pauses and resumes it, without showing it.
    playback[2] = VideoPagePlayback::playing;
    check(view()->items[3].icon == UiIcon::audio_lines && (view()->items[3].flags & kUiTabPlaying),
          "caption tabs media: a playing page's tab shows audio_lines");
    layout = bar();
    const UiRect playing = ui_tab_strip_icon(layout.tabs, 3);
    check(ui_rect_has_area(playing) &&
              ui_tab_strip_hit(layout.tabs, centre_x(playing), (playing.top + playing.bottom) * .5f).kind ==
                  UiTabHitKind::icon,
          "caption tabs media: the playing icon is a button");
    click(centre_x(playing), centre_y(layout, playing));
    check(toggles == 1 && playback[2] == VideoPagePlayback::paused && view()->items[3].icon == UiIcon::video &&
              (view()->items[3].flags & kUiTabPlaying) && shown == 1,
          "caption tabs media: clicking the icon pauses the page and leaves the page that shows");
    click(centre_x(playing), centre_y(layout, playing));
    check(toggles == 2 && playback[2] == VideoPagePlayback::playing && view()->items[3].icon == UiIcon::audio_lines,
          "caption tabs media: clicking it again resumes");
    playback[2] = VideoPagePlayback::none;
    check(view()->items[3].icon == UiIcon::video && !(view()->items[3].flags & kUiTabPlaying),
          "caption tabs media: a page without the player has its plain icon");
    bool opened = true;
    static const char* const filler[]{"x0", "x1", "x2", "x3", "x4"};
    for (u32 i = 0; i < 5; ++i)
        opened = application_tabs_open_media(source(MediaPageKind::image, filler[i]), true) && opened;
    check(opened && open_pages() == kMediaPageCapacity && view()->count == 10 && !g_app.toasts.count &&
              !application_tabs_open_media(source(MediaPageKind::image, "y"), true) && g_app.toasts.count == 1 &&
              open_pages() == kMediaPageCapacity && view()->count == 10,
          "caption tabs media: the ninth media tab is refused with a toast");
    layout = bar();
    const UiRect close = ui_tab_strip_close(layout.tabs, 2);
    check(ui_rect_has_area(close), "caption tabs media: the page that shows has a close button");
    click(centre_x(close), centre_y(layout, close));
    check(!pages[1].open && shown == 2 && view()->count == 9 && selected() == 100 &&
              (view()->items[2].flags & kUiTabActive) && application_tabs_key(2) == page_key(2),
          "caption tabs media: closing the page that shows closes it and shows the next tab");
    click(tab_x(2), tab_y(2), PointerButton::middle);
    check(!pages[2].open && view()->count == 8 && pages[3].open && shown == 3,
          "caption tabs media: a middle click closes a media tab and its page, the shown one showing its neighbour");
    check(application_tabs_menu_action(popup_action_tab_close_right, 101) && view()->count == 2 && open_pages() == 0,
          "caption tabs media: closing the tabs to the right closes their pages");
    // Closing the active channel tab shows a media neighbour; the keys cycle through every tab.
    check(application_tabs_open_media(source(MediaPageKind::audio, "a"), true) && application_tabs_activate(1) &&
              selected() == 101 && application_tabs_close(1) && shown == 1 && view()->count == 2 &&
              (view()->items[1].flags & kUiTabActive),
          "caption tabs media: closing the active channel tab shows the page next to it");
    check(application_tabs_shortcut(ShortcutAction::tab_next, false) && shown == 0 && selected() == 100 &&
              (view()->items[0].flags & kUiTabActive) && application_tabs_shortcut(ShortcutAction::tab_next, false) &&
              shown == 1,
          "caption tabs media: the next-tab key cycles between a channel tab and a page tab");
    check(application_tabs_shortcut(ShortcutAction::tab_close, false) && shown == 0 && view()->count == 1 &&
              open_pages() == 0,
          "caption tabs media: the close-tab key closes the page that shows");
    check(application_tabs_open_media(source(MediaPageKind::video, "v"), false) && shown != 0 &&
              application_tabs_close_page(shown) && shown == 0 && view()->count == 1 && open_pages() == 0 &&
              !application_tabs_close_page(7),
          "caption tabs media: the page's own close button closes its tab");
    // The strip hiding takes the pages, and keeps the channel tabs.
    check(application_tabs_open_media(source(MediaPageKind::image, "z"), true) && open_pages() == 1, "caption tabs media: a page opens");
    g_app.preferences.tabs_enabled = false;
    check(!application_tabs_visible() && open_pages() == 0 && resets != 0,
          "caption tabs media: hiding the strip closes the pages");
    g_app.preferences.tabs_enabled = true;
    check(application_tabs_visible() && view()->count == 1 && application_tabs_key(0) == 100,
          "caption tabs media: the channel tabs stay when the strip shows again");
    // Accessibility: the page tab says what it holds, is selected while its page shows and shows it when invoked.
    check(application_tabs_open_media(source(MediaPageKind::image, "cat.png"), false) && describe_bar(),
          "caption tabs media: the page tab is described");
    UiAccessNode* node = find_node(access_tree, UiAccessRole::tab, utf8_literal("cat.png"));
    const u32 index = node ? static_cast<u32>(node - access_tree.nodes.data) : 0;
    check(node && (node->state & ui_access_selected) &&
              utf8_equal(ui_access_help(access_tree, index), utf8_literal("Image")),
          "caption tabs media: the page tab is selected while its page shows and names its kind");
    pages[shown].missing = true;
    node = describe_bar() ? find_node(access_tree, UiAccessRole::tab, utf8_literal("cat.png")) : nullptr;
    check(node && utf8_equal(ui_access_help(access_tree, static_cast<u32>(node - access_tree.nodes.data)),
                             utf8_literal("Media unavailable")),
          "caption tabs media: a page that cannot load again says so");
    const u8 page = shown;
    check(application_caption_access(application_caption_tab_item({UiTabHitKind::tab, 0})) && shown == 0 &&
              application_caption_access(application_caption_tab_item({UiTabHitKind::tab, 1})) && shown == page,
          "caption tabs media: invoking the tabs hides and shows the page");
}

void accessibility() noexcept {
    check(fixture(1000), "caption tabs accessibility: fixture");
    seed(3);
    UiAccessTree& tree = access_tree;
    check(describe_bar(), "caption tabs accessibility: tree");
    const UiAccessNode* list = find_node(tree, UiAccessRole::tab_list, utf8_literal("Open tabs"));
    UiAccessNode* one = find_node(tree, UiAccessRole::tab, utf8_literal("one"));
    UiAccessNode* two = find_node(tree, UiAccessRole::tab, utf8_literal("two"));
    check(list && one && two, "caption tabs accessibility: the list and its named tabs exist");
    if (!list || !one || !two)
        return;
    check(find_node(tree, UiAccessRole::tab, utf8_literal("three")) &&
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
// A tab that cannot be reached is dimmed and selects nothing, and still drags.
void dimmed() noexcept {
    check(fixture(1000), "caption tabs dimmed: fixture");
    blocked_channel = 101;
    seed(3);
    check((view()->items[1].flags & kUiTabMissing) && !(view()->items[0].flags & kUiTabMissing),
          "caption tabs dimmed: an unreachable channel's tab is dimmed");
    const float y = tab_y(0);
    const float width = tab_x(1) - tab_x(0);
    const float x = tab_x(1);
    send(pointer_move(x, y));
    send(pointer_down(x, y));
    check(selected() == 100 && (view()->items[0].flags & kUiTabActive),
          "caption tabs dimmed: a press on it selects nothing");
    send(pointer_move(x + width * 0.4f, y));
    check((view()->items[1].flags & kUiTabDragging) && application_tabs_key(1) == 101,
          "caption tabs dimmed: but it drags");
    send(pointer_move(x + width * 1.1f, y));
    check(application_tabs_key(1) == 102 && application_tabs_key(2) == 101 && (view()->items[2].flags & kUiTabDragging) &&
              view()->hovered.index == 2 && view()->pressed.index == 2,
          "caption tabs dimmed: it swaps past its neighbour and stays the held tab");
    send(pointer_up(x + width * 1.1f, y));
    check(!(view()->items[2].flags & kUiTabDragging) && memory_tab(1) == 102 && memory_tab(2) == 101 && selected() == 100,
          "caption tabs dimmed: the drop keeps the order");
}

// A drag follows its tab by identity: tabs closing before it shift its index, not the drag.
void identity() noexcept {
    check(fixture(1000), "caption tabs identity: fixture");
    seed(4);
    const float y = tab_y(0);
    const float width = tab_x(1) - tab_x(0);
    const float x = tab_x(2);
    send(pointer_move(x, y));
    send(pointer_down(x, y));
    send(pointer_move(x + width * 0.4f, y));
    check((view()->items[2].flags & kUiTabDragging) && application_tabs_drag_index() == 2,
          "caption tabs identity: the tab is dragged");
    // A channel before it is deleted: the strip closes that tab and its neighbours move up.
    check(chat_state_store_remove_channel(&ui->chat_state, 100) && view()->count == 3 &&
              application_tabs_drag_index() == 1 && application_tabs_key(1) == 102 &&
              (view()->items[1].flags & kUiTabDragging) && !(view()->items[0].flags & kUiTabDragging),
          "caption tabs identity: a tab closing before the dragged one does not change which one is dragged");
    send(pointer_move(x + width * 0.5f, y));
    check(application_tabs_key(2) == 102 && application_tabs_key(1) == 103 && (view()->items[2].flags & kUiTabDragging),
          "caption tabs identity: moves keep acting on it, past its new neighbour");
    send(pointer_up(x + width * 0.5f, y));
    check(!(view()->items[2].flags & kUiTabDragging) && !window_pointer_captured(g_app.window),
          "caption tabs identity: and the drop ends the drag");
}

// The tab list's smaller truths: a disabled "+" named for what it pins, nodes that keep their identity when the tabs
// reorder, a node for every tab (hidden ones included) with its close button, and why a dimmed tab is dimmed.
void access_details() noexcept {
    check(fixture(1000), "caption tabs access details: fixture");
    blocked_channel = 102;
    seed(3);
    check(describe_bar(), "caption tabs access details: tree");
    const auto index_of = [](const UiAccessNode* node) noexcept { return static_cast<u32>(node - access_tree.nodes.data); };
    const auto plus_node = []() noexcept -> const UiAccessNode* {
        for (u32 i = 1; i < access_tree.count; ++i)
            if (access_tree.nodes[i].item == application_caption_tab_item({UiTabHitKind::plus, 0}))
                return &access_tree.nodes[i];
        return nullptr;
    };
    const UiAccessNode* plus = plus_node();
    check(plus && (plus->state & ui_access_disabled) && !(plus->actions & ui_access_action_invoke),
          "caption tabs access details: a \"+\" with nothing to pin is announced disabled");
    const UiAccessNode* three = find_node(access_tree, UiAccessRole::tab, utf8_literal("three"));
    check(three && utf8_equal(ui_access_help(access_tree, index_of(three)), utf8_literal("Channel unavailable")),
          "caption tabs access details: a dimmed channel tab says it is unavailable");
    const UiAccessNode* one = find_node(access_tree, UiAccessRole::tab, utf8_literal("one"));
    const u64 key_one = one ? one->key : 0;
    check(one && three && one->key != three->key && key_one, "caption tabs access details: each tab has its own key");
    check(application_tabs_move(0, 2) && describe_bar(), "caption tabs access details: the tabs reorder");
    one = find_node(access_tree, UiAccessRole::tab, utf8_literal("one"));
    check(one && one->key == key_one && one->item == application_caption_tab_item({UiTabHitKind::tab, 2}),
          "caption tabs access details: a reorder moves a tab's node without changing its key");
    // An open message viewer turns into a page: the "+" is on and says so.
    media_page_fake::viewer_open = true;
    media_page_fake::viewer_source = media_page_fake::source(MediaPageKind::image, "cat.png");
    check(describe_bar(), "caption tabs access details: tree with a viewer");
    plus = plus_node();
    check(plus && !(plus->state & ui_access_disabled) && (plus->actions & ui_access_action_invoke) &&
              utf8_equal(ui_access_name(access_tree, index_of(plus)), utf8_literal("Pin this media")),
          "caption tabs access details: with a viewer open the \"+\" is on and names the media");

    // Sixteen tabs in the narrowest strip: the hidden ones and the narrow ones are described too.
    check(fixture(624), "caption tabs access details: overflow fixture");
    seed(1);
    bool opened = true;
    for (u64 id = 101; id < 116; ++id)
        opened = application_tabs_open_channel(id, true) && opened;
    check(opened && bar().tabs.overflowed && describe_bar(), "caption tabs access details: overflowed tree");
    u32 tabs = 0, closes = 0;
    for (u32 i = 1; i < access_tree.count; ++i) {
        tabs += access_tree.nodes[i].role == UiAccessRole::tab;
        closes += access_tree.nodes[i].role == UiAccessRole::button &&
                  utf8_equal(ui_access_name(access_tree, i), utf8_literal("Close tab"));
    }
    check(tabs == kUiTabCapacity && closes == kUiTabCapacity,
          "caption tabs access details: every tab, hidden or narrow, has a node and a close button");
}

// The platform reports the pointer leaving the frame's parts as it moves onto a tab, or as a press on one takes the
// capture: the press goes on, is released as a click, and the next tab hovered is the one under the pointer, lit and
// named in its tooltip ("#channel · Server") from its own place.
void frame_leave() noexcept {
    check(fixture(1000), "caption tabs frame leave: fixture");
    seed(3);
    const float y = tab_y(0);
    float x = tab_x(2);
    send(pointer_move(x, y));
    send(pointer_down(x, y));
    application_caption_leave();
    check(send(pointer_up(x, y)) && !window_pointer_captured(g_app.window) && selected() == 102,
          "caption tabs frame leave: the press survives and its release gives the pointer back");
    x = tab_x(0);
    send(pointer_move(x, y));
    application_caption_leave();
    const UiCaptionBarState state = application_caption_state();
    const auto layout = bar();
    const UiRect one = ui_tab_strip_tab(layout.tabs, 0);
    check(!(view()->items[2].flags & kUiTabDragging) && application_tabs_key(2) == 102 &&
              view()->hovered == UiTabHit{UiTabHitKind::tab, 0} && state.hovered == UiCaptionPart::tab &&
              layout.tab_hover.left == one.left && layout.tab_hover.right == one.right,
          "caption tabs frame leave: the click dragged nothing and the tab under the pointer is the hovered one");
    check(utf8_equal(ui_caption_bar_part_name(UiCaptionPart::tab, state), utf8_literal("#one \xC2\xB7 Guild")),
          "caption tabs frame leave: a channel tab's tooltip is \"#channel \xC2\xB7 Server\"");
}

// A media tab shows its page with the channel it came from selected (sidebar, member list); a channel the account
// no longer reaches leaves the selection alone.
void origin() noexcept {
    using namespace media_page_fake;
    check(fixture(1000), "caption tabs origin: fixture");
    seed(2);
    MediaPageSource song = source(MediaPageKind::audio, "song");
    song.channel_id = 103;
    check(application_tabs_open_media(song, true) && selected() == 100 && shown == 0,
          "caption tabs origin: a background media tab");
    click(tab_x(1), tab_y(1));
    click(tab_x(2), tab_y(2));
    check(shown == 1 && selected() == 103 && (view()->items[2].flags & kUiTabActive) &&
              !(view()->items[1].flags & kUiTabActive),
          "caption tabs origin: activating it selects the channel its media came from and shows the page");
    click(tab_x(0), tab_y(0));
    blocked_channel = 103;
    click(tab_x(2), tab_y(2));
    check(shown == 1 && selected() == 100, "caption tabs origin: an unreachable origin keeps the selection");
}

void run() noexcept {
    clicks();
    drags();
    frame_leave();
    origin();
    dimmed();
    identity();
    menus();
    overflow();
    media();
    accessibility();
    access_details();
    window_test::close();
}
} // namespace maicord::caption_tabs_test

namespace maicord {
namespace app_detail {
bool application_navigate_known_message(u64 channel_id, u64) noexcept {
    using namespace caption_tabs_test;
    // The real navigation hides a page that shows.
    const bool selected = channel_id != blocked_channel && chat_state_store_select_channel(&ui->chat_state, channel_id).ok();
    if (selected)
        application_tabs_note_navigation();
    return selected;
}
bool application_channel_navigable(u64 channel_id) noexcept {
    using namespace caption_tabs_test;
    const auto* channel = chat_state_store_find_channel(ui->chat_state, channel_id);
    return channel_id != blocked_channel && channel && channel->permissions_resolved &&
           (channel->actions & channel_action_view);
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
