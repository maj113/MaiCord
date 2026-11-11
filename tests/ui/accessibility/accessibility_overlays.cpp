#include "accessibility_internal.hpp"
#include "maicord/ui/accessibility/scenes.hpp"
#include "maicord/ui/voice/call_scene.hpp"
#include "maicord/ui/chat/threads/forum_post_scene.hpp"
#include "maicord/ui/media/image_viewer.hpp"
#include "maicord/ui/voice/audio_controls.hpp"
#include "maicord/ui/voice/video_scene.hpp"
#include "maicord/ui/chat/inbox_scene.hpp"
#include "maicord/ui/media/picker.hpp"
#include "maicord/ui/chat/pinned_messages_scene.hpp"
#include "maicord/ui/shell/quick_switcher_scene.hpp"
#include "maicord/ui/chat/search/scene.hpp"
#include "maicord/ui/chat/threads/browser_scene.hpp"
#include "../../support/fake_text_measure.hpp"

namespace maicord::accessibility_test {
namespace {

// ---- event decisions -----------------------------------------------------

struct EventShape {
    bool checked;
    u32 selected;
    u32 children;
    u32 focus;
    const char* live;
    u32 live_bytes;
    float range;
};

bool event_tree(UiAccessTree* tree, const EventShape& shape) noexcept {
    if (!ui_access_tree_reset(tree, utf8_literal("Events"), {0, 0, 100, 100}))
        return false;
    UiAccessNodeDesc list{};
    list.key = 1;
    list.rectangle = {0, 0, 50, 100};
    list.role = UiAccessRole::list;
    list.name = utf8_literal("List");
    const u32 list_node = ui_access_add(tree, 0, list);
    for (u32 index = 0; index < shape.children; ++index) {
        UiAccessNodeDesc item{};
        item.key = 10 + index;
        item.rectangle = {0, 10.0f * index, 50, 10.0f * index + 10};
        item.role = UiAccessRole::list_item;
        item.name = utf8_literal("Item");
        item.state = static_cast<u16>(ui_access_selectable | (index == shape.selected ? ui_access_selected : 0) |
                                      (index == shape.focus ? ui_access_focused : 0));
        item.actions = ui_access_action_select;
        ui_access_add(tree, list_node, item);
    }
    UiAccessNodeDesc toggle{};
    toggle.key = 2;
    toggle.rectangle = {60, 0, 100, 10};
    toggle.role = UiAccessRole::check_box;
    toggle.name = utf8_literal("Mute");
    toggle.state = static_cast<u16>(shape.checked ? ui_access_checked : 0);
    toggle.actions = ui_access_action_toggle;
    ui_access_add(tree, 0, toggle);
    UiAccessNodeDesc status{};
    status.key = 3;
    status.rectangle = {60, 20, 100, 30};
    status.role = UiAccessRole::status;
    status.name = {shape.live, shape.live_bytes};
    status.state = ui_access_live;
    ui_access_add(tree, 0, status);
    UiAccessNodeDesc slider{};
    slider.key = 4;
    slider.rectangle = {60, 40, 100, 50};
    slider.role = UiAccessRole::slider;
    slider.name = utf8_literal("Volume");
    slider.range = {shape.range, 0, 10, 1};
    slider.actions = ui_access_action_range;
    ui_access_add(tree, 0, slider);
    return ui_access_tree_finish(tree);
}

} // namespace

namespace accessibility_detail {

bool has_property(const UiAccessEventPlan& plan, const UiAccessTree& tree, u64 key, UiAccessProperty property) noexcept {
    for (u32 index = 0; index < plan.property_count; ++index)
        if (plan.properties[index].property == property && tree.nodes[plan.properties[index].node].key == key)
            return true;
    return false;
}

void test_events(UiAccessTree* tree) noexcept {
    static UiAccessObservation current[64], previous[64];
    UiAccessEventState state{};
    check(ui_access_event_state_initialize(&state, {current, 64}, {previous, 64}), "event state initializes");
    UiAccessEventPlan plan{};
    EventShape shape{false, 0, 3, 0, "Connecting", 10, 5};
    check(event_tree(tree, shape) && ui_access_plan_events(&state, *tree, &plan) && plan.focus_changed &&
              !plan.property_count && !plan.structure_count && !plan.structure_root,
          "the first snapshot only announces focus");
    check(event_tree(tree, shape) && ui_access_plan_events(&state, *tree, &plan) && !plan.focus_changed &&
              !plan.property_count && !plan.structure_count,
          "an unchanged snapshot raises nothing");
    shape.checked = true;
    shape.selected = 1;
    shape.range = 7;
    check(event_tree(tree, shape) && ui_access_plan_events(&state, *tree, &plan) &&
              has_property(plan, *tree, 2, UiAccessProperty::toggle) &&
              has_property(plan, *tree, 10, UiAccessProperty::selected) &&
              has_property(plan, *tree, 11, UiAccessProperty::selected) &&
              has_property(plan, *tree, 4, UiAccessProperty::range) && plan.properties[0].old_range >= 0,
          "toggle, selection and range changes raise property events");
    shape.children = 4;
    shape.focus = 3;
    check(event_tree(tree, shape) && ui_access_plan_events(&state, *tree, &plan) && plan.structure_count == 1 &&
              tree->nodes[plan.structures[0]].key == 1 && plan.focus_changed &&
              tree->nodes[plan.focus_node].key == 13,
          "a changed list raises a structure change and focus follows the new item");
    shape.live = "Connected";
    shape.live_bytes = 9;
    check(event_tree(tree, shape) && ui_access_plan_events(&state, *tree, &plan) &&
              has_property(plan, *tree, 3, UiAccessProperty::name) && !plan.focus_changed,
          "live status text changes are announced");

    UiAccessMessageCursor cursor{};
    check(!ui_access_message_should_announce(&cursor, 5, 100, false), "the first observed message only primes");
    check(!ui_access_message_should_announce(&cursor, 5, 100, false), "no new message, no announcement");
    check(ui_access_message_should_announce(&cursor, 5, 101, false), "a new message in the same channel announces");
    check(!ui_access_message_should_announce(&cursor, 5, 102, true), "own messages are not announced");
    check(!ui_access_message_should_announce(&cursor, 6, 500, false), "switching channels primes again");
    check(!ui_access_message_should_announce(&cursor, 6, 400, false), "older history never announces");
    check(ui_access_message_should_announce(&cursor, 6, 501, false), "the next new message after a switch announces");
}

} // namespace accessibility_detail

namespace {

// ---- overlay projections -------------------------------------------------

UiAccessNode g_overlay_nodes[2048];
char g_overlay_text[128 * 1024];
u32 g_overlay_slots[8192];
u32 g_overlay_keys[4096];
constexpr u8 kOverlaySurface = 20;

bool descends(const UiAccessTree& tree, u32 node, u32 ancestor) noexcept {
    for (u32 current = node; current != kUiAccessNone && current < tree.count; current = tree.nodes[current].parent)
        if (current == ancestor)
            return true;
    return false;
}

// First node (in reading order) with role under ancestor whose name starts with prefix.
u32 find_node(const UiAccessTree& tree, u32 ancestor, UiAccessRole role, Utf8View prefix = {}) noexcept {
    for (u32 index = 1; index < tree.count; ++index)
        if (tree.nodes[index].role == role && descends(tree, index, ancestor) &&
            utf8_starts_with(ui_access_name(tree, index), prefix))
            return index;
    return kUiAccessNone;
}

u32 count_role(const UiAccessTree& tree, u32 ancestor, UiAccessRole role) noexcept {
    u32 count = 0;
    for (u32 index = 1; index < tree.count; ++index)
        count += tree.nodes[index].role == role && descends(tree, index, ancestor) ? 1u : 0u;
    return count;
}

// A page and the player it hosts, projected as the application does.
struct PagePlayer {
    const UiImageViewer* page;
    const UiVideoScene* video;
    const UiAudioScene* audio;
};
u32 add_page_player(UiAccessTree* tree, u32 parent, const PagePlayer& player, u8 surface) noexcept {
    const u32 node = ui_access_add_image_viewer(tree, parent, *player.page, surface);
    if (node != kUiAccessNone && player.video)
        ui_access_add_video_controls(tree, node, *player.video, surface);
    if (node != kUiAccessNone && player.audio)
        ui_access_add_audio_controls(tree, node, *player.audio, surface);
    return node;
}

// Projects one overlay and its drawn text the way the application does.
template <typename Scene, typename Text>
u32 project_overlay(UiAccessTree* tree, const Scene& scene,
                    u32 (*add)(UiAccessTree*, u32, const Scene&, u8) noexcept, Text text) noexcept {
    if (!ui_access_tree_reset(tree, utf8_literal("Overlay"), {0, 0, 1600, 1000}))
        return kUiAccessNone;
    const u32 node = add(tree, 0, scene, kOverlaySurface);
    if (node == kUiAccessNone)
        return kUiAccessNone;
    static UiTextCommand commands[512];
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    if (ui_text_command_stream_initialize(&stream, {commands, 512}) && text(scene, &output))
        for (u32 index = 0; index < stream.count; ++index)
            ui_access_attribute_text(tree, commands[index].owner, commands[index].text, commands[index].rectangle);
    return ui_access_tree_finish(tree) ? (tree->count > 1 ? tree->nodes[0].first_child : kUiAccessNone)
                                       : kUiAccessNone;
}

bool quick_row(void*, u32 index, UiQuickSwitcherRow* row) noexcept {
    *row = {1000 + index, 71, 100 + index, 0, UiQuickSwitcherKind::text_channel, utf8_literal("release-planning"),
            utf8_literal("Native Lab"), utf8_literal("Channel"), {}, true};
    return true;
}
bool always_current(void*) noexcept {
    return true;
}

void test_quick_switcher(UiAccessTree* tree) noexcept {
    static UiQuickSwitcherScene scene{};
    UiQuickSwitcherSource source{nullptr, 100, 1, quick_row, always_current};
    UiQuickSwitcherContent content{};
    content.phase = UiQuickSwitcherPhase::ready;
    content.revision = 1;
    content.edit = {utf8_literal("release"), 3, 7, true, true, true};
    check(ui_quick_switcher_scene_update(&scene, &source, &content, {0, 0, 1280, 800}),
          "quick switcher scene builds");
    const u32 dialog = project_overlay(tree, scene, ui_access_add_quick_switcher,
                                       [](const UiQuickSwitcherScene& s, UiTextOutput* out) noexcept {
                                           return ui_quick_switcher_scene_text_output(s, kDefaultTheme, out);
                                       });
    check(dialog != kUiAccessNone && tree->nodes[dialog].role == UiAccessRole::dialog &&
              (tree->nodes[dialog].state & ui_access_modal) &&
              equal(ui_access_name(*tree, dialog), utf8_literal("Quick switcher")),
          "quick switcher: modal dialog");
    const u32 combo = find_node(*tree, dialog, UiAccessRole::combo_box);
    check(combo != kUiAccessNone && equal(ui_access_name(*tree, combo), utf8_literal("Search destinations")) &&
              equal(ui_access_value(*tree, combo), utf8_literal("release")) &&
              (tree->nodes[combo].actions & ui_access_action_text) && (tree->nodes[combo].state & ui_access_expanded) &&
              (tree->nodes[combo].state & ui_access_focused) && tree->nodes[combo].placement.selection_anchor == 3 &&
              tree->nodes[combo].placement.selection_caret == 7 && tree->focus == combo,
          "quick switcher: query is a focused combo box with value, text selection and an open list");
    const u32 list = find_node(*tree, dialog, UiAccessRole::list, utf8_literal("Results"));
    const u32 first = find_node(*tree, list, UiAccessRole::list_item);
    check(list != kUiAccessNone && first != kUiAccessNone &&
              equal(ui_access_name(*tree, first), utf8_literal("release-planning, text channel, Native Lab")) &&
              (tree->nodes[first].state & ui_access_selected) && tree->nodes[first].placement.position == 1 &&
              tree->nodes[first].placement.set_size == 100 && tree->nodes[first].item == 1 &&
              (tree->nodes[first].actions & ui_access_action_scroll_item) && tree->nodes[first].first_child == kUiAccessNone,
          "quick switcher: result rows are list items with kind, selection and position of the whole set");
    check(count_role(*tree, list, UiAccessRole::list_item) == scene.count && tree->nodes[list].scroll.vertical_percent == 0,
          "quick switcher: every materialized row is listed with the list scroll position");
    UiAccessNameBuilder summary{};
    check(ui_access_quick_switcher_summary(scene, &summary) && equal(summary.view(), utf8_literal("100 results")),
          "quick switcher: result count summary");
    ui_quick_switcher_scene_release(&scene);
}

bool search_row(void*, u32 index, UiSearchRow* row) noexcept {
    *row = {71, index + 101, utf8_literal("Ana"), utf8_literal("# general"), utf8_literal("Build is green"),
            utf8_literal("Today at 12:30"), false, index % 3 == 0};
    return true;
}

void test_search(UiAccessTree* tree) noexcept {
    static UiSearchScene scene{};
    UiSearchSource results{nullptr, 6, 1, search_row};
    UiSearchContent content{};
    content.edit = {utf8_literal("green"), 5, 5, true, true, true};
    content.panel = content.tabs = true;
    content.revision = 1;
    content.total = 6;
    content.phase = UiSearchPhase::ready;
    content.tab_known = 1;
    content.tab_totals[0] = 6;
    check(ui_search_scene_update(&scene, &results, &content, {0, 0, 1280, 900}, {0, 0, 1280, 48}),
          "search scene builds");
    const u32 pane = project_overlay(tree, scene, ui_access_add_search,
                                     [](const UiSearchScene& s, UiTextOutput* out) noexcept {
                                         return ui_search_scene_text_output(s, kDefaultTheme, out, nullptr);
                                     });
    check(pane != kUiAccessNone && tree->nodes[pane].role == UiAccessRole::pane, "search: pane");
    const u32 field = find_node(*tree, pane, UiAccessRole::edit, utf8_literal("Search messages"));
    check(field != kUiAccessNone && equal(ui_access_value(*tree, field), utf8_literal("green")) &&
              (tree->nodes[field].actions & ui_access_action_text),
          "search: query field with value and text pattern");
    const u32 tabs = find_node(*tree, pane, UiAccessRole::tab_list);
    const u32 messages = find_node(*tree, tabs, UiAccessRole::tab, utf8_literal("Messages"));
    check(tabs != kUiAccessNone && count_role(*tree, tabs, UiAccessRole::tab) == 5 && messages != kUiAccessNone &&
              equal(ui_access_name(*tree, messages), utf8_literal("Messages, 6 results")) &&
              (tree->nodes[messages].state & ui_access_selected),
          "search: result type tabs form a tab list with counts and selection");
    const u32 list = find_node(*tree, pane, UiAccessRole::list, utf8_literal("Search results"));
    const u32 row = find_node(*tree, list, UiAccessRole::list_item);
    check(list != kUiAccessNone && row != kUiAccessNone &&
              equal(ui_access_name(*tree, row), utf8_literal("Ana, Today at 12:30: Build is green, in # general")) &&
              tree->nodes[row].placement.position == 1 && tree->nodes[row].placement.set_size == 6 &&
              count_role(*tree, list, UiAccessRole::list_item) >= 2,
          "search: results list items carry author, time, text, channel and set position");
    const u32 next = find_node(*tree, pane, UiAccessRole::button, utf8_literal("Next page"));
    check(next != kUiAccessNone && find_node(*tree, pane, UiAccessRole::button, utf8_literal("Page 1, current page")) !=
                                       kUiAccessNone,
          "search: pagination names pages and marks the current one");
    // Over the thread browser the header field is scoped to its threads.
    UiSearchContent scoped{};
    scoped.edit = {{}, 0, 0, true, true, true};
    scoped.revision = 1;
    scoped.heading = scoped.placeholder = utf8_literal("Search threads in product");
    scoped.scope_chip = utf8_literal("Threads");
    check(ui_search_scene_update(&scene, nullptr, &scoped, {0, 0, 1280, 900}, {0, 0, 1280, 48}) &&
              scene.scope.generation,
          "search scene builds with a thread scope chip");
    const u32 scoped_pane = project_overlay(tree, scene, ui_access_add_search,
                                            [](const UiSearchScene& s, UiTextOutput* out) noexcept {
                                                return ui_search_scene_text_output(s, kDefaultTheme, out, nullptr);
                                            });
    const u32 chip = find_node(*tree, scoped_pane, UiAccessRole::button, utf8_literal("Remove search scope: Threads"));
    check(find_node(*tree, scoped_pane, UiAccessRole::edit, utf8_literal("Search threads in product")) != kUiAccessNone &&
              chip != kUiAccessNone && equal(ui_access_help(*tree, chip), utf8_literal("Search the whole server instead")),
          "search: a scoped field is named by its scope and the chip says how to widen it");
    ui_search_scene_release(&scene);
}

bool pin_row(void*, u32 index, UiPinnedMessagesRow* row) noexcept {
    *row = {71, 100 + index, 200 + index, utf8_literal("Ana"), utf8_literal("Oct 5"), utf8_literal("Ship it"),
            {}, {}, 24.f, true, false};
    return true;
}

void test_pins(UiAccessTree* tree) noexcept {
    static UiPinnedMessagesScene scene{};
    UiPinnedMessagesSource source{nullptr, 3, 1, pin_row, always_current};
    UiPinnedMessagesContent content{};
    content.channel = utf8_literal("# notes");
    content.channel_id = 71;
    content.revision = 1;
    content.phase = UiPinnedMessagesPhase::ready;
    check(ui_pinned_messages_scene_update(&scene, &source, &content, {100, 48, 1300, 780}), "pins scene builds");
    const u32 pane = project_overlay(tree, scene, ui_access_add_pinned_messages,
                                     [](const UiPinnedMessagesScene& s, UiTextOutput* out) noexcept {
                                         return ui_pinned_messages_scene_text_output(s, kDefaultTheme, out);
                                     });
    check(pane != kUiAccessNone && equal(ui_access_name(*tree, pane), utf8_literal("Pinned messages in # notes")),
          "pins: named pane");
    const u32 list = find_node(*tree, pane, UiAccessRole::list, utf8_literal("Pinned messages"));
    const u32 row = find_node(*tree, list, UiAccessRole::list_item);
    check(row != kUiAccessNone && equal(ui_access_name(*tree, row), utf8_literal("Ana, Oct 5: Ship it")) &&
              tree->nodes[row].item == 100 && tree->nodes[row].placement.set_size == 3 &&
              (tree->nodes[row].actions & (ui_access_action_invoke | ui_access_action_scroll_item)) ==
                  (ui_access_action_invoke | ui_access_action_scroll_item),
          "pins: pinned messages are list items routed by message id");
    check(find_node(*tree, list, UiAccessRole::button, utf8_literal("Unpin message from Ana")) != kUiAccessNone &&
              find_node(*tree, pane, UiAccessRole::button, utf8_literal("Close pinned messages")) != kUiAccessNone,
          "pins: unpin and close buttons are named");
    ui_pinned_messages_scene_release(&scene);
}

void test_threads(UiAccessTree* tree) noexcept {
    static UiThreadBrowserScene scene{};
    UiThreadBrowserRow rows[3]{{100, utf8_literal("Planning"), false, true, true, 11},
                               {101, utf8_literal("Secrets"), true, false, true, 12},
                               {102, {}, false, false, false, 10}};
    UiThreadBrowserView view{};
    view.rows = {rows, 3};
    view.title = utf8_literal("# product");
    view.source_publication = 1;
    view.revision = 1;
    view.total = 3;
    view.inventory_synced = true;
    check(ui_thread_browser_scene_update(&scene, &view, {320, 48, 1280, 784}), "thread browser scene builds");
    const u32 pane = project_overlay(tree, scene, ui_access_add_thread_browser,
                                     [](const UiThreadBrowserScene& s, UiTextOutput* out) noexcept {
                                         return ui_thread_browser_scene_text_output(s, kDefaultTheme, out);
                                     });
    check(pane != kUiAccessNone && equal(ui_access_name(*tree, pane), utf8_literal("Threads in # product")) &&
              equal(ui_access_help(*tree, pane), utf8_literal("Search thread names with the header search bar")),
          "threads: named pane points to the header search bar");
    const u32 tabs = find_node(*tree, pane, UiAccessRole::tab_list);
    const u32 active = find_node(*tree, tabs, UiAccessRole::tab, utf8_literal("Active threads"));
    check(tabs != kUiAccessNone && active != kUiAccessNone && (tree->nodes[active].state & ui_access_selected) &&
              count_role(*tree, tabs, UiAccessRole::tab) == 2,
          "threads: active/archived tabs form a tab list");
    const u32 list = find_node(*tree, pane, UiAccessRole::list, utf8_literal("Active threads"));
    check(list != kUiAccessNone &&
              find_node(*tree, list, UiAccessRole::list_item, utf8_literal("Planning, public thread, active, joined")) !=
                  kUiAccessNone &&
              find_node(*tree, list, UiAccessRole::list_item, utf8_literal("Secrets, private thread, archived")) !=
                  kUiAccessNone &&
              find_node(*tree, list, UiAccessRole::list_item, utf8_literal("Unnamed thread, announcement thread")) !=
                  kUiAccessNone,
          "threads: rows describe kind, archive and membership state");
    check(count_role(*tree, pane, UiAccessRole::edit) == 0 &&
              find_node(*tree, pane, UiAccessRole::button, utf8_literal("Search")) == kUiAccessNone,
          "threads: the panel has no search field or Search button of its own");
    ui_thread_browser_scene_release(&scene);
}

bool inbox_row(void*, u32 index, UiInboxRow* row) noexcept {
    *row = {UiInboxRowKind::mention, 500 + index, 70, 9, utf8_literal("Ana"), utf8_literal("#general"),
            utf8_literal("@you ship it"), utf8_literal("5 minutes ago")};
    return true;
}

void test_inbox(UiAccessTree* tree) noexcept {
    static UiInboxScene scene{};
    UiInboxSource source{nullptr, 4, 1, inbox_row, always_current};
    UiInboxContent content{};
    content.revision = 1;
    content.phase = UiInboxPhase::ready;
    content.this_server_available = true;
    check(ui_inbox_scene_update(&scene, &source, &content, {240, 48, 1280, 800}), "inbox scene builds");
    const u32 pane = project_overlay(tree, scene, ui_access_add_inbox,
                                     [](const UiInboxScene& s, UiTextOutput* out) noexcept {
                                         return ui_inbox_scene_text_output(s, kDefaultTheme, out);
                                     });
    const u32 tabs = find_node(*tree, pane, UiAccessRole::tab_list);
    const u32 mentions = find_node(*tree, tabs, UiAccessRole::tab, utf8_literal("Mentions"));
    check(pane != kUiAccessNone && equal(ui_access_name(*tree, pane), utf8_literal("Inbox")) && mentions != kUiAccessNone &&
              (tree->nodes[mentions].state & ui_access_selected) &&
              find_node(*tree, tabs, UiAccessRole::tab, utf8_literal("Unreads")) != kUiAccessNone,
          "inbox: mentions and unreads tabs");
    const u32 everyone = find_node(*tree, pane, UiAccessRole::check_box, utf8_literal("Include @everyone"));
    const u32 server = find_node(*tree, pane, UiAccessRole::check_box, utf8_literal("Only this server"));
    check(everyone != kUiAccessNone && (tree->nodes[everyone].state & ui_access_checked) && server != kUiAccessNone &&
              !(tree->nodes[server].state & ui_access_checked) &&
              (tree->nodes[server].actions & ui_access_action_toggle),
          "inbox: mention filters are check boxes with their state");
    const u32 list = find_node(*tree, pane, UiAccessRole::list, utf8_literal("Recent mentions"));
    const u32 row = find_node(*tree, list, UiAccessRole::list_item);
    check(row != kUiAccessNone &&
              equal(ui_access_name(*tree, row), utf8_literal("Ana, 5 minutes ago: @you ship it, in #general")) &&
              tree->nodes[row].placement.position == 1 && tree->nodes[row].placement.set_size == 4,
          "inbox: mentions list items carry author, time, text and channel");
    ui_inbox_scene_release(&scene);
}

void test_call(UiAccessTree* tree) noexcept {
    static UiCallScene scene{};
    UiCallParticipant people[3]{};
    people[0].user_id = 1;
    people[0].name = utf8_literal("Ana");
    people[0].self = true;
    people[1].user_id = 2;
    people[1].name = utf8_literal("Bo");
    people[1].speaking = people[1].muted = true;
    people[2].user_id = 3;
    people[2].name = utf8_literal("Cy");
    people[2].deafened = true;
    UiCallContent content{};
    content.title = utf8_literal("General");
    content.participants = {people, 3};
    content.connected = true;
    check(ui_call_scene_update(&scene, content, {300, 0, 1500, 900}, 1000, true), "call scene builds");
    const u32 pane = project_overlay(tree, scene, ui_access_add_call,
                                     [](const UiCallScene& s, UiTextOutput* out) noexcept {
                                         return ui_call_scene_text_output(s, kDefaultTheme, out);
                                     });
    check(pane != kUiAccessNone && equal(ui_access_name(*tree, pane), utf8_literal("Call: General")) &&
              count_role(*tree, pane, UiAccessRole::text) == 0,
          "call: named pane without duplicated drawn text");
    const u32 list = find_node(*tree, pane, UiAccessRole::list, utf8_literal("Participants, 3"));
    check(list != kUiAccessNone && count_role(*tree, list, UiAccessRole::list_item) == 3 &&
              find_node(*tree, list, UiAccessRole::list_item, utf8_literal("Ana, you")) != kUiAccessNone &&
              find_node(*tree, list, UiAccessRole::list_item, utf8_literal("Bo, speaking, muted")) != kUiAccessNone &&
              find_node(*tree, list, UiAccessRole::list_item, utf8_literal("Cy, deafened")) != kUiAccessNone,
          "call: participants carry speaking, mute and deafen state");
    const u32 bar = find_node(*tree, pane, UiAccessRole::group, utf8_literal("Call controls"));
    const u32 mute = find_node(*tree, bar, UiAccessRole::toggle_button, utf8_literal("Mute"));
    const u32 camera = find_node(*tree, bar, UiAccessRole::button, utf8_literal("Camera"));
    check(bar != kUiAccessNone && mute != kUiAccessNone && (tree->nodes[mute].actions & ui_access_action_toggle) &&
              !(tree->nodes[mute].state & ui_access_checked) && camera != kUiAccessNone &&
              (tree->nodes[camera].state & ui_access_disabled) &&
              find_node(*tree, bar, UiAccessRole::button, utf8_literal("Disconnect")) != kUiAccessNone,
          "call: control bar exposes toggles, disabled controls and disconnect");
    ui_call_scene_release(&scene);
}

void test_forms(UiAccessTree* tree) noexcept {
    static UiTextMeasureRecord records[64]{};
    static UiTextMeasureCache cache{};
    check(ui_text_measure_cache_initialize(&cache, {records, 64}, fake_text_measure<>, nullptr), "form measure cache");
    static TextFormScene form{};
    TextFormContent content{utf8_literal("Description"), utf8_literal("image.png"),
                            utf8_literal("Describe this attachment"), utf8_literal("Too long"),
                            utf8_literal("Cancel"), utf8_literal("Save"), utf8_literal("A\r\n\xF0\x9F\x98\x80"),
                            0, 5, true, true};
    check(text_form_update(&form, content, {100, 50, 900, 750}, &cache), "text form builds");
    const u32 dialog = project_overlay(tree, form, ui_access_add_text_form,
                                       [](const TextFormScene&, UiTextOutput*) noexcept { return true; });
    const u32 edit = find_node(*tree, dialog, UiAccessRole::edit);
    check(dialog != kUiAccessNone && tree->nodes[dialog].role == UiAccessRole::dialog &&
              equal(ui_access_name(*tree, dialog), utf8_literal("Description")) &&
              equal(ui_access_help(*tree, dialog), utf8_literal("image.png")) && edit != kUiAccessNone &&
              equal(ui_access_name(*tree, edit), utf8_literal("Describe this attachment")) &&
              tree->nodes[edit].placement.selection_caret == 7 && (tree->nodes[edit].state & ui_access_focused) &&
              (tree->nodes[edit].actions & ui_access_action_text),
          "text form: dialog with labelled field, UTF-16 caret mapped to bytes and focus");
    const u32 status = find_node(*tree, dialog, UiAccessRole::status);
    check(status != kUiAccessNone && (tree->nodes[status].state & ui_access_live) &&
              find_node(*tree, dialog, UiAccessRole::button, utf8_literal("Save")) != kUiAccessNone &&
              find_node(*tree, dialog, UiAccessRole::button, utf8_literal("Cancel")) != kUiAccessNone,
          "text form: error text is a live status; buttons are named");
    text_form_release(&form);

    static ForumPostScene forum{};
    ForumPostTagView tags[]{{1, utf8_literal("Questions"), false, true}, {2, utf8_literal("Review"), true, true}};
    ForumPostContent post{};
    post.heading = utf8_literal("Create a forum post");
    post.title_label = utf8_literal("Title");
    post.body_label = utf8_literal("Message");
    post.tags_hint = utf8_literal("Choose tags");
    post.cancel = utf8_literal("Cancel");
    post.accept = utf8_literal("Create post");
    post.fields[0] = {utf8_literal("Release"), 0, 7, {}, true};
    post.fields[1] = {utf8_literal("Line\nNext"), 0, 0, {}, false};
    post.tags = {tags, 2};
    post.editable = post.can_accept = true;
    check(forum_post_update(&forum, post, {0, 0, 640, 600}, &cache), "forum post builds");
    const u32 panel = project_overlay(tree, forum, ui_access_add_forum_post,
                                      [](const ForumPostScene&, UiTextOutput*) noexcept { return true; });
    const u32 title = find_node(*tree, panel, UiAccessRole::edit, utf8_literal("Title"));
    const u32 body = find_node(*tree, panel, UiAccessRole::edit, utf8_literal("Message"));
    const u32 group = find_node(*tree, panel, UiAccessRole::group, utf8_literal("Choose tags"));
    const u32 review = find_node(*tree, group, UiAccessRole::check_box, utf8_literal("Review"));
    check(panel != kUiAccessNone && tree->nodes[panel].role == UiAccessRole::dialog &&
              equal(ui_access_name(*tree, panel), utf8_literal("Create a forum post")) && title != kUiAccessNone &&
              equal(ui_access_value(*tree, title), utf8_literal("Release")) &&
              tree->nodes[title].placement.selection_caret == 7 && body != kUiAccessNone &&
              (tree->nodes[body].state & ui_access_multiline) && review != kUiAccessNone &&
              (tree->nodes[review].state & ui_access_checked) && tree->nodes[review].placement.set_size == 2,
          "forum: dialog with title/body fields and checkable tags in a group");
    forum_post_release(&forum);
}

void test_viewer(UiAccessTree* tree) noexcept {
    static UiImageViewer viewer{};
    check(ui_image_viewer_open(&viewer, {0, 0, 1200, 800}, {1600, 900}), "image viewer opens");
    const u32 dialog = project_overlay(tree, viewer, ui_access_add_image_viewer,
                                       [](const UiImageViewer& v, UiTextOutput* out) noexcept {
                                           return ui_image_viewer_text_output(v, kDefaultTheme, out);
                                       });
    const u32 image = find_node(*tree, dialog, UiAccessRole::image, utf8_literal("Image"));
    check(dialog != kUiAccessNone && equal(ui_access_name(*tree, dialog), utf8_literal("Image viewer")) &&
              (tree->nodes[dialog].state & ui_access_modal) && image != kUiAccessNone &&
              utf8_starts_with(ui_access_help(*tree, image), utf8_literal("Zoom ")),
          "media viewer: modal dialog around the image with its zoom");
    check(find_node(*tree, dialog, UiAccessRole::button, utf8_literal("Close image")) != kUiAccessNone &&
              find_node(*tree, dialog, UiAccessRole::button, utf8_literal("Zoom in")) != kUiAccessNone &&
              find_node(*tree, dialog, UiAccessRole::button, utf8_literal("Open original")) == kUiAccessNone,
          "media viewer: visible actions only");
}

// A media page: a group named by its title with the source line as help, in the window rather than modal, whose header
// names Close tab and Jump to message (dimmed without a message), and which says when its media cannot load again.
void test_page(UiAccessTree* tree) noexcept {
    static UiImageViewer viewer{};
    UiImageViewerPage page{utf8_literal("cat.png"), utf8_literal("#general - Native Lab - Mai"), UiIcon::image,
                           UiImageViewerPageKind::image, true, false};
    check(ui_image_viewer_open(&viewer, {0, 0, 1200, 800}, {1600, 900}) && ui_image_viewer_set_page(&viewer, page),
          "media page: the viewer turns into a page");
    const auto text = [](const UiImageViewer& v, UiTextOutput* out) noexcept {
        return ui_image_viewer_text_output(v, kDefaultTheme, out);
    };
    u32 group = project_overlay(tree, viewer, ui_access_add_image_viewer, text);
    u32 image = find_node(*tree, group, UiAccessRole::image, utf8_literal("Image"));
    check(group != kUiAccessNone && tree->nodes[group].role == UiAccessRole::group &&
              equal(ui_access_name(*tree, group), utf8_literal("cat.png")) &&
              equal(ui_access_help(*tree, group), utf8_literal("#general - Native Lab - Mai")) &&
              !(tree->nodes[group].state & ui_access_modal) && image != kUiAccessNone &&
              utf8_starts_with(ui_access_help(*tree, image), utf8_literal("Zoom ")),
          "media page: a group named by its title around the image, not a modal dialog");
    const u32 jump = find_node(*tree, group, UiAccessRole::button, utf8_literal("Jump to message"));
    check(find_node(*tree, group, UiAccessRole::button, utf8_literal("Close tab")) != kUiAccessNone &&
              find_node(*tree, group, UiAccessRole::button, utf8_literal("Close image")) == kUiAccessNone &&
              jump != kUiAccessNone && !(tree->nodes[jump].state & ui_access_disabled) &&
              find_node(*tree, group, UiAccessRole::button, utf8_literal("Retry")) == kUiAccessNone,
          "media page: the header names Close tab and Jump to message");
    page.can_jump = false;
    check(ui_image_viewer_set_page(&viewer, page), "media page: the message went away");
    group = project_overlay(tree, viewer, ui_access_add_image_viewer, text);
    const u32 dimmed = find_node(*tree, group, UiAccessRole::button, utf8_literal("Jump to message"));
    check(dimmed != kUiAccessNone && (tree->nodes[dimmed].state & ui_access_disabled),
          "media page: Jump to message is disabled without its message");
    check(ui_image_viewer_set_media(&viewer, UiImageViewerMedia::failed), "media page: the media failed");
    group = project_overlay(tree, viewer, ui_access_add_image_viewer, text);
    image = find_node(*tree, group, UiAccessRole::image, utf8_literal("Image"));
    check(image != kUiAccessNone && equal(ui_access_value(*tree, image), utf8_literal("Media unavailable")) &&
              find_node(*tree, group, UiAccessRole::button, utf8_literal("Retry")) != kUiAccessNone,
          "media page: an unavailable page says so and offers Retry");
    page.kind = UiImageViewerPageKind::audio;
    page.glyph = UiIcon::music;
    check(ui_image_viewer_set_page(&viewer, page) && ui_image_viewer_set_media(&viewer, UiImageViewerMedia::ready),
          "media page: the page holds audio");
    group = project_overlay(tree, viewer, ui_access_add_image_viewer, text);
    check(find_node(*tree, group, UiAccessRole::image, utf8_literal("Audio")) != kUiAccessNone,
          "media page: an audio page is named by its kind");

    // The queue and the player a page hosts: a list of its rows in view with the current track selected (the artist
    // as help), the play button of a player that has not started, then the hosted player's controls, each routed by
    // its item.
    static UiImageViewerQueue queue{};
    queue.rows[0] = {utf8_literal("One"), {}, {}, {}};
    queue.rows[1] = {utf8_literal("Two"), utf8_literal("Artist"), utf8_literal("1:05"), {}};
    queue.rows[2] = {utf8_literal("Three"), {}, {}, {}};
    queue.position = utf8_literal("2 of 3");
    queue.count = 3;
    queue.current = 2;
    page.kind = UiImageViewerPageKind::video;
    page.glyph = UiIcon::video;
    page.queue = true;
    check(ui_image_viewer_set_page(&viewer, page), "media page: a playlist video page");
    viewer.queue = &queue;
    viewer.idle = true;
    const auto player_text = [](const PagePlayer& p, UiTextOutput* out) noexcept {
        return ui_image_viewer_text_output(*p.page, kDefaultTheme, out);
    };
    group = project_overlay(tree, PagePlayer{&viewer, nullptr, nullptr}, add_page_player, player_text);
    const u32 rows = find_node(*tree, group, UiAccessRole::list, utf8_literal("Queue"));
    const u32 two = find_node(*tree, rows, UiAccessRole::list_item, utf8_literal("Two"));
    const u32 one = find_node(*tree, rows, UiAccessRole::list_item, utf8_literal("One"));
    check(rows != kUiAccessNone && equal(ui_access_help(*tree, rows), utf8_literal("2 of 3")) &&
              count_role(*tree, rows, UiAccessRole::list_item) == 3 && two != kUiAccessNone && one != kUiAccessNone &&
              (tree->nodes[two].state & ui_access_selected) && !(tree->nodes[one].state & ui_access_selected) &&
              tree->nodes[two].item == 2 && tree->nodes[two].placement.position == 2 &&
              tree->nodes[two].placement.set_size == 3 &&
              (tree->nodes[two].actions & ui_access_action_invoke) && equal(ui_access_help(*tree, two), utf8_literal("Artist")) &&
              ui_rect_has_area(tree->nodes[two].rectangle),
          "media page: the queue is a list of its rows, the current track selected and invocable by track number");
    const u32 play = find_node(*tree, group, UiAccessRole::button, utf8_literal("Play"));
    check(play != kUiAccessNone && tree->nodes[play].item == (kUiAccessPlayerItem | 1) &&
              (tree->nodes[play].actions & ui_access_action_invoke),
          "media page: a player that has not started offers Play");
    viewer.idle = false;
    static UiVideoScene video{};
    UiVideoPlaybackState playing{7, 8, 9, 35000, 200000, 40, true, false, false, false, true, true, true, true};
    playing.can_expand = true;
    playing.collection_count = 3;
    playing.collection_track = 2;
    playing.can_change_track = true;
    UiVideoControlsStyle controls{};
    controls.tracks_in_row = true;
    controls.expand = utf8_literal("Full screen");
    const UiRect stage = ui_image_viewer_page_layout(viewer).player;
    check(ui_video_scene_project(&video, playing, stage, stage, controls, {}, kDefaultTheme),
          "media page: the hosted video player projects");
    group = project_overlay(tree, PagePlayer{&viewer, &video, nullptr}, add_page_player, player_text);
    const u32 pause = find_node(*tree, group, UiAccessRole::button, utf8_literal("Pause"));
    const u32 seek = find_node(*tree, group, UiAccessRole::slider, utf8_literal("Seek"));
    const u32 volume = find_node(*tree, group, UiAccessRole::slider, utf8_literal("Volume"));
    check(find_node(*tree, group, UiAccessRole::button, utf8_literal("Play")) == kUiAccessNone && pause != kUiAccessNone &&
              tree->nodes[pause].item == (kUiAccessPlayerItem | 1) && seek != kUiAccessNone &&
              (tree->nodes[seek].actions & ui_access_action_range) && tree->nodes[seek].range.value == 1750 &&
              tree->nodes[seek].range.maximum == 10000 && equal(ui_access_value(*tree, seek), utf8_literal("0:35 / 3:20")) &&
              tree->nodes[seek].item == (kUiAccessPlayerItem | 2) && volume != kUiAccessNone &&
              tree->nodes[volume].range.value == 40 && tree->nodes[volume].range.maximum == 100 &&
              tree->nodes[volume].item == (kUiAccessPlayerItem | 5),
          "media page: play/pause, seek and volume are a button and two sliders with their values");
    const u32 mute = find_node(*tree, group, UiAccessRole::button, utf8_literal("Mute"));
    check(mute != kUiAccessNone && tree->nodes[mute].item == (kUiAccessPlayerItem | 4) &&
              find_node(*tree, group, UiAccessRole::text, utf8_literal("0:35")) != kUiAccessNone &&
              find_node(*tree, group, UiAccessRole::button, utf8_literal("Previous track")) != kUiAccessNone &&
              find_node(*tree, group, UiAccessRole::button, utf8_literal("Next track")) != kUiAccessNone &&
              find_node(*tree, group, UiAccessRole::button, utf8_literal("Full screen")) != kUiAccessNone,
          "media page: mute, the time, previous and next track and full screen are named");
    playing.can_change_track = false;
    check(ui_video_scene_project(&video, playing, stage, stage, controls, {}, kDefaultTheme),
          "media page: the player cannot change tracks");
    group = project_overlay(tree, PagePlayer{&viewer, &video, nullptr}, add_page_player, player_text);
    const u32 previous = find_node(*tree, group, UiAccessRole::button, utf8_literal("Previous track"));
    check(previous != kUiAccessNone && (tree->nodes[previous].state & ui_access_disabled),
          "media page: a control the player refuses is disabled");
    page.kind = UiImageViewerPageKind::audio;
    page.glyph = UiIcon::music;
    check(ui_image_viewer_set_page(&viewer, page), "media page: a playlist audio page");
    viewer.queue = &queue;
    static UiAudioScene card{};
    UiAudioPlaybackState song{};
    song.owner = 7;
    song.generation = 8;
    song.revision = 9;
    song.elapsed_ms = 30000;
    song.duration_ms = 120000;
    song.volume = 40;
    song.playing = song.can_play = song.can_seek = song.can_volume = song.can_download = song.has_duration = true;
    song.title = utf8_literal("Song");
    const UiRect audio_stage = ui_image_viewer_page_layout(viewer).player;
    check(ui_audio_scene_project(&card, song, ui_audio_page_card(audio_stage, kDefaultUiAudioControlsStyle), audio_stage,
                                 {&viewer.widgets, viewer.owner.handle}, UiAudioSourceSurface::page,
                                 kDefaultUiAudioControlsStyle, kDefaultTheme),
          "media page: the hosted audio card projects");
    group = project_overlay(tree, PagePlayer{&viewer, nullptr, &card}, add_page_player, player_text);
    const u32 audio_seek = find_node(*tree, group, UiAccessRole::slider, utf8_literal("Seek"));
    check(find_node(*tree, group, UiAccessRole::button, utf8_literal("Pause")) != kUiAccessNone && audio_seek != kUiAccessNone &&
              tree->nodes[audio_seek].range.value == 2500 && tree->nodes[audio_seek].range.maximum == 10000 &&
              find_node(*tree, group, UiAccessRole::slider, utf8_literal("Volume")) != kUiAccessNone &&
              find_node(*tree, group, UiAccessRole::button, utf8_literal("Mute")) != kUiAccessNone &&
              find_node(*tree, group, UiAccessRole::button, utf8_literal("Download")) != kUiAccessNone &&
              find_node(*tree, group, UiAccessRole::list, utf8_literal("Queue")) != kUiAccessNone,
          "media page: an audio page names the card's controls beside its queue");
}

bool picker_item(void*, u32 index, UiMediaPickerItem* item) noexcept {
    *item = {index + 1, index + 1001, utf8_literal("Party parrot"), {}, 0, 320, 180, UiMediaPickerItemKind::gif, true};
    return true;
}

void test_picker(UiAccessTree* tree) noexcept {
    static UiMediaPickerScene scene{};
    UiMediaPickerSource source{nullptr, 3, 7, 40, 1, {}, picker_item};
    UiMediaPickerContent content{};
    content.account_scope = 3;
    content.query_generation = 7;
    content.revision = 1;
    content.kind = UiMediaPickerKind::gif;
    content.phase = UiMediaPickerPhase::ready;
    content.available_tabs = 7;
    content.edit.active = true;
    check(ui_media_picker_scene_update(&scene, &source, &content, {0, 0, 1000, 800}, {560, 700, 940, 740}),
          "media picker builds");
    check(ui_media_picker_scene_select(&scene, 1), "media picker selects");
    const u32 dialog = project_overlay(tree, scene, ui_access_add_media_picker,
                                       [](const UiMediaPickerScene&, UiTextOutput*) noexcept { return true; });
    const u32 tabs = find_node(*tree, dialog, UiAccessRole::tab_list);
    const u32 gifs = find_node(*tree, tabs, UiAccessRole::tab, utf8_literal("GIFs"));
    check(dialog != kUiAccessNone && equal(ui_access_name(*tree, dialog), utf8_literal("GIF picker")) &&
              count_role(*tree, tabs, UiAccessRole::tab) == 3 && gifs != kUiAccessNone &&
              (tree->nodes[gifs].state & ui_access_selected),
          "picker: emoji/GIF/sticker tabs");
    const u32 grid = find_node(*tree, dialog, UiAccessRole::list, utf8_literal("GIFs"));
    const u32 first = find_node(*tree, grid, UiAccessRole::list_item);
    const u32 second = first != kUiAccessNone ? tree->nodes[first].next_sibling : kUiAccessNone;
    check(grid != kUiAccessNone && (tree->nodes[grid].actions & ui_access_action_grid) &&
              tree->nodes[grid].placement.grid_row >= 1 && tree->nodes[grid].placement.grid_column >= 2 &&
              first != kUiAccessNone && second != kUiAccessNone &&
              (tree->nodes[first].actions & ui_access_action_grid_item) && tree->nodes[first].placement.grid_row == 0 &&
              tree->nodes[first].placement.grid_column == 0 && tree->nodes[second].placement.grid_column == 1 &&
              (tree->nodes[second].state & ui_access_selected) &&
              equal(ui_access_name(*tree, first), utf8_literal("Party parrot")),
          "picker: cells form a grid with row/column positions and selection");
    check(find_node(*tree, dialog, UiAccessRole::edit, utf8_literal("Search GIFs")) != kUiAccessNone,
          "picker: search field");

    if (grid == kUiAccessNone || second == kUiAccessNone) {
        ui_media_picker_scene_release(&scene);
        return;
    }
    // Grid and GridItem patterns through the provider.
    HostContext context{tree, {}, 0};
    UiaProviderHost host{};
    check(uia_host_initialize(&host, nullptr, {&context, host_tree, host_act, host_scale}) && uia_host_load(&host),
          "picker provider host");
    IRawElementProviderSimple* grid_element = element(&host, tree->nodes[grid].key);
    IGridProvider* grid_pattern = pattern<IGridProvider>(grid_element, UIA_GridPatternId);
    IRawElementProviderSimple* cell = nullptr;
    int rows = 0, columns = 0;
    check(grid_pattern && SUCCEEDED(grid_pattern->get_RowCount(&rows)) &&
              SUCCEEDED(grid_pattern->get_ColumnCount(&columns)) && rows == tree->nodes[grid].placement.grid_row &&
              columns == tree->nodes[grid].placement.grid_column && SUCCEEDED(grid_pattern->GetItem(0, 1, &cell)) &&
              cell && grid_pattern->GetItem(rows, 0, &cell) == E_INVALIDARG,
          "Grid pattern reports dimensions and finds cells");
    IRawElementProviderSimple* second_element = element(&host, tree->nodes[second].key);
    IGridItemProvider* item = pattern<IGridItemProvider>(second_element, UIA_GridItemPatternId);
    IRawElementProviderSimple* container = nullptr;
    int column = -1;
    check(item && SUCCEEDED(item->get_Column(&column)) && column == 1 && SUCCEEDED(item->get_ContainingGrid(&container)) &&
              container && int_property(second_element, UIA_PositionInSetPropertyId) == -1,
          "GridItem pattern reports its cell and containing grid");
    IUnknown* release[]{grid_element, grid_pattern, cell, second_element, item, container};
    for (auto* unknown : release)
        if (unknown)
            unknown->Release();
    uia_host_shutdown(&host);
    ui_media_picker_scene_release(&scene);
}

} // namespace

namespace accessibility_detail {

void test_overlays() noexcept {
    static UiAccessTree tree{};
    check(ui_access_tree_initialize(&tree, {g_overlay_nodes, 2048}, {g_overlay_text, sizeof(g_overlay_text)},
                                    {g_overlay_slots, 8192}, {g_overlay_keys, 4096}),
          "overlay tree initializes");
    test_quick_switcher(&tree);
    test_search(&tree);
    test_pins(&tree);
    test_threads(&tree);
    test_inbox(&tree);
    test_call(&tree);
    test_forms(&tree);
    test_viewer(&tree);
    test_page(&tree);
    test_picker(&tree);
}

// ---- demand-committed capacity ----------------------------------------------

// Hosts commit snapshot storage on demand and rebuild when a region runs out:
// the snapshot must say which region was too small, and only on a real
// capacity shortfall (field limits and successful builds flag nothing).
void test_exhaustion(const Scene& scene) noexcept {
    static UiAccessNode nodes[256];
    static char text[16384];
    static u32 slots[512], keys[512];
    UiAccessTree tree{};
    check(ui_access_tree_initialize(&tree, {nodes, 256}, {text, sizeof(text)}, {slots, 512}, {keys, 512}) &&
              build_tree(&tree, scene) && tree.exhausted == 0,
          "full-capacity snapshot exhausts nothing");
    const u32 needed_nodes = tree.count, needed_text = tree.text_used;

    check(ui_access_tree_initialize(&tree, {nodes, 3}, {text, sizeof(text)}, {slots, 512}, {keys, 512}),
          "small node tree initializes");
    build_tree(&tree, scene);
    check(tree.exhausted == ui_access_exhausted_nodes, "node shortfall flags nodes only");
    check(ui_access_tree_initialize(&tree, {nodes, 256}, {text, sizeof(text)}, {slots, 2}, {keys, 512}),
          "small slot tree initializes");
    build_tree(&tree, scene);
    check((tree.exhausted & ui_access_exhausted_slots) && !(tree.exhausted & ui_access_exhausted_nodes),
          "slot shortfall flags slots");
    const u32 short_text = needed_text > 8 ? needed_text - 8 : 1;
    check(ui_access_tree_initialize(&tree, {nodes, 256}, {text, short_text}, {slots, 512}, {keys, 512}),
          "small text tree initializes");
    build_tree(&tree, scene);
    check((tree.exhausted & ui_access_exhausted_text) && !(tree.exhausted & ui_access_exhausted_nodes),
          "text shortfall flags text");
    check(ui_access_tree_reset(&tree, utf8_literal("Synthetic"), {0, 0, 400, 600}) && tree.exhausted == 0,
          "reset clears exhaustion");

    // Doubling until nothing is flagged reproduces the full-capacity snapshot.
    u32 node_count = 4, text_count = 64, slot_count = 4;
    for (u32 attempt = 0; attempt < 16; ++attempt) {
        check(ui_access_tree_initialize(&tree, {nodes, node_count}, {text, text_count}, {slots, slot_count},
                                        {keys, 512}),
              "growing tree initializes");
        const bool built = build_tree(&tree, scene) && tree.exhausted == 0;
        if (!tree.exhausted) {
            check(built && tree.count == needed_nodes && tree.text_used == needed_text,
                  "grown snapshot matches the full-capacity snapshot");
            break;
        }
        if (tree.exhausted & ui_access_exhausted_nodes) node_count = minimum(node_count * 2, 256u);
        if (tree.exhausted & ui_access_exhausted_slots) slot_count = minimum(slot_count * 2, 512u);
        if (tree.exhausted & ui_access_exhausted_text) text_count = minimum<u32>(text_count * 2, sizeof(text));
        check(attempt != 15, "demand growth converges");
    }
}

// ---- text pattern ----------------------------------------------------------

void test_text_units() noexcept {
    const Utf8View text = utf8_literal("one two, three\nfour \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD!");
    u32 start = 4, end = 4;
    ui_access_text_expand(text, UiAccessTextUnit::word, &start, &end);
    check(start == 4 && end == 7, "word unit: a word with its trailing space");
    start = 5;
    end = 5;
    ui_access_text_expand(text, UiAccessTextUnit::line, &start, &end);
    check(start == 0 && end == 15, "line unit ends after the line feed");
    start = 21;
    end = 21;
    ui_access_text_expand(text, UiAccessTextUnit::character, &start, &end);
    check(start == 20 && end == 28, "character unit keeps an emoji with its skin tone");
    check(ui_access_text_align(text, 22) == 20, "offsets align to cluster starts");
    start = 0;
    end = 3;
    check(ui_access_text_move(text, UiAccessTextUnit::word, 2, &start, &end) == 2 && start == 7 && end == 9,
          "Move by two words lands on the punctuation word");
    start = end = text.bytes;
    check(ui_access_text_move(text, UiAccessTextUnit::line, -1, &start, &end) == -1 && start == 15 && end == 15,
          "degenerate Move back one line from the end");
    u32 offset = 0;
    check(ui_access_text_move_endpoint(text, UiAccessTextUnit::document, 5, &offset) == 1 && offset == text.bytes &&
              ui_access_text_move_endpoint(text, UiAccessTextUnit::character, -100, &offset) < 0 && offset == 0,
          "endpoints move by document and clamp at the start");
    start = 0;
    end = 4;
    check(ui_access_text_move(text, UiAccessTextUnit::line, 5, &start, &end) == 1 && start == 15 && end == text.bytes,
          "Move never starts a unit at the text end");
    start = end = 0;
    ui_access_text_expand({}, UiAccessTextUnit::word, &start, &end);
    check(start == 0 && end == 0, "empty text expands to an empty range");
    u32 anchor = 0, caret = 0;
    ui_access_text_selection_unpack(ui_access_text_selection_pack(12, 7000), &anchor, &caret);
    check(anchor == 12 && caret == 7000, "selection packs into an action number");
}

} // namespace accessibility_detail
} // namespace maicord::accessibility_test
