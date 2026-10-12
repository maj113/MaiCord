#include "maicord/ui/accessibility_scenes.hpp"

#include "maicord/core/hash.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/call_scene.hpp"
#include "maicord/ui/forum_post_scene.hpp"
#include "maicord/ui/image_viewer.hpp"
#include "maicord/ui/inbox_scene.hpp"
#include "maicord/ui/media_picker.hpp"
#include "maicord/ui/pinned_messages_scene.hpp"
#include "maicord/ui/quick_switcher_scene.hpp"
#include "maicord/ui/search_scene.hpp"
#include "maicord/ui/text_form.hpp"
#include "maicord/ui/thread_browser_scene.hpp"

#include "call_stage.hpp"

namespace maicord {
namespace {

// Describers compose names here; ui_access_add copies them at once.
UiAccessNameBuilder g_name;
// Group nodes are described while a member's info (and its name in g_name)
// is still pending, so they compose into their own buffer.
UiAccessNameBuilder g_group_name;

bool same(UiWidgetHandle a, UiWidgetHandle b) noexcept {
    return a.generation != 0 && a.slot == b.slot && a.generation == b.generation;
}

Utf8View pick(Utf8View text, Utf8View fallback) noexcept {
    return text.bytes ? text : fallback;
}

UiAccessScrollState scroll_state(float offset, float maximum_offset, float viewport) noexcept {
    if (maximum_offset <= 0.5f || viewport <= 0)
        return {-1.0f, 100.0f};
    const float percent = offset * 100.0f / maximum_offset;
    return {percent < 0 ? 0.0f : percent > 100.0f ? 100.0f : percent,
            viewport * 100.0f / (viewport + maximum_offset)};
}

void set_position(UiAccessWidgetInfo* info, u64 position, u64 size) noexcept {
    info->placement.position = static_cast<u32>(position < 0xFFFF'FFFFull ? position : 0xFFFF'FFFFull);
    info->placement.set_size = static_cast<u32>(size < 0xFFFF'FFFFull ? size : 0xFFFF'FFFFull);
}

// Single-line or multiline text field over a host editor (UTF-8 byte offsets).
void edit_field(UiAccessWidgetInfo* info, Utf8View name, Utf8View placeholder, Utf8View text, u32 anchor, u32 caret,
                bool focused) noexcept {
    info->role = UiAccessRole::edit;
    info->leaf = true;
    info->name = name;
    info->help = placeholder;
    info->value = text;
    info->placement.selection_anchor = anchor < text.bytes ? anchor : text.bytes;
    info->placement.selection_caret = caret < text.bytes ? caret : text.bytes;
    info->actions = ui_access_action_value | ui_access_action_text | ui_access_action_focus;
    info->state |= ui_access_focusable;
    if (focused)
        info->state |= ui_access_focused;
}

u32 byte_offset(Utf8View text, u32 utf16) noexcept {
    u32 byte = 0;
    return utf8_byte_offset_for_utf16_position(text, utf16, &byte) ? byte : text.bytes;
}

void add_count(UiAccessNameBuilder* name, u64 count, Utf8View one, Utf8View many) noexcept {
    name->number(count).add(utf8_literal(" ")).add(count == 1 ? one : many);
}

u32 add_widgets(UiAccessTree* tree, u32 parent, const UiWidgetManager& manager, UiAccessDescribe describe,
                void* context, u8 surface, UiAccessDescribeGroup group = nullptr, bool focus_owner = false) noexcept {
    if (!tree || !manager.count)
        return kUiAccessNone;
    const u32 first = tree->count;
    const UiAccessScope scope{describe, context, surface, focus_owner, group};
    const u32 node = ui_access_add_widgets(tree, parent, manager, scope);
    return node == kUiAccessNone || tree->count == first ? kUiAccessNone : node;
}

// Quick switcher

Utf8View quick_kind(UiQuickSwitcherKind kind) noexcept {
    switch (kind) {
    case UiQuickSwitcherKind::text_channel: return utf8_literal("text channel");
    case UiQuickSwitcherKind::voice_channel: return utf8_literal("voice channel");
    case UiQuickSwitcherKind::private_channel: return utf8_literal("direct message");
    case UiQuickSwitcherKind::group_private: return utf8_literal("group message");
    case UiQuickSwitcherKind::guild: return utf8_literal("server");
    case UiQuickSwitcherKind::user: return utf8_literal("user");
    default: return {};
    }
}

bool quick_checked(const UiQuickSwitcherContent& content, u64 key) noexcept {
    for (u32 index = 0; index < content.checked.count; ++index)
        if (content.checked[index] == key)
            return true;
    return false;
}

void describe_quick(void* context, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget&,
                    const UiLayoutNode&, UiAccessWidgetInfo* info) noexcept {
    const auto& s = *static_cast<const UiQuickSwitcherScene*>(context);
    const auto* content = s.content;
    const bool picker = content && content->picker;
    if (same(handle, s.root)) {
        info->role = UiAccessRole::dialog;
        info->state |= ui_access_modal;
        info->name = pick(content ? content->heading : Utf8View{},
                          picker ? utf8_literal("Select friends") : utf8_literal("Quick switcher"));
        return;
    }
    if (same(handle, s.edit)) {
        const UiEditView view = content ? content->edit : UiEditView{};
        edit_field(info, picker ? utf8_literal("Search friends") : utf8_literal("Search destinations"),
                   picker ? utf8_literal("Type the username of a friend") : utf8_literal("Where would you like to go?"),
                   view.text, view.anchor, view.caret, view.focused);
        // Combo box over the result list: typing filters, arrows move the selection.
        info->role = UiAccessRole::combo_box;
        info->state |= s.count ? ui_access_expanded : ui_access_collapsed;
        return;
    }
    if (same(handle, s.close)) {
        info->name = picker ? utf8_literal("Close") : utf8_literal("Close quick switcher");
        info->leaf = true;
        return;
    }
    if (same(handle, s.confirm)) {
        info->name = pick(content ? content->action : Utf8View{}, utf8_literal("Confirm"));
        info->leaf = true;
        return;
    }
    if (same(handle, s.body_owner)) {
        info->role = UiAccessRole::list;
        g_name.clear().add(picker ? utf8_literal("Friends") : utf8_literal("Results"));
        info->name = g_name.view();
        info->actions = ui_access_action_scroll;
        info->scroll = scroll_state(s.scroll_dips, ui_quick_switcher_scene_scroll_maximum(s), ui_rect_height(s.body));
        return;
    }
    for (u32 slot = 0; slot < s.count && slot < kUiQuickSwitcherVisibleRows; ++slot) {
        if (!same(handle, s.rows[slot]))
            continue;
        const auto& row = s.visible[slot];
        const u32 index = s.first + slot;
        info->leaf = true;
        info->ignore_text = true;
        info->role = UiAccessRole::list_item;
        info->key = hash64_mix(row.key, static_cast<u64>(row.kind) + 1);
        info->item = static_cast<u64>(index) + 1;
        g_name.clear().part(row.label).part(quick_kind(row.kind)).part(row.detail);
        info->name = g_name.view();
        info->state |= ui_access_selectable;
        if (index == s.selected)
            info->state |= ui_access_selected;
        if (!row.enabled)
            info->state |= ui_access_disabled;
        info->actions = ui_access_action_select | ui_access_action_invoke | ui_access_action_scroll_item;
        if (picker && content) {
            info->state |= ui_access_toggleable | (quick_checked(*content, row.key) ? ui_access_checked : 0);
            info->actions |= ui_access_action_toggle;
        }
        set_position(info, static_cast<u64>(index) + 1, s.accepted_count);
        return;
    }
}

// Search

enum SearchGroup : u8 {
    search_tabs = 1,
    search_results,
    search_filters,
    search_sort,
};

Utf8View search_tab_label(u32 tab) noexcept {
    switch (tab) {
    case 0: return utf8_literal("Messages");
    case 1: return utf8_literal("Media");
    case 2: return utf8_literal("Files");
    case 3: return utf8_literal("Links");
    case 4: return utf8_literal("Pins");
    default: return {};
    }
}

Utf8View search_sort_label(UiSearchAction action) noexcept {
    return action == UiSearchAction::newest   ? utf8_literal("Newest")
           : action == UiSearchAction::oldest ? utf8_literal("Oldest")
                                              : utf8_literal("Most relevant");
}

void search_group(void* context, u8 group, UiAccessNodeDesc* description) noexcept {
    const auto& s = *static_cast<const UiSearchScene*>(context);
    switch (group) {
    case search_tabs:
        description->role = UiAccessRole::tab_list;
        description->name = utf8_literal("Result types");
        break;
    case search_results:
        description->role = UiAccessRole::list;
        g_group_name.clear().add(utf8_literal("Search results"));
        if (s.count_bytes)
            g_group_name.part({s.count_text, s.count_bytes});
        description->name = g_group_name.view();
        description->actions = ui_access_action_scroll;
        description->scroll = scroll_state(s.scroll_dips, ui_search_scene_scroll_maximum(s), ui_rect_height(s.body));
        break;
    case search_filters:
        description->role = UiAccessRole::list;
        description->name = utf8_literal("Search options");
        break;
    case search_sort:
        description->role = UiAccessRole::group;
        description->name = utf8_literal("Sort results by");
        description->actions = ui_access_action_select;
        break;
    default:
        break;
    }
}

void describe_search(void* context, const UiWidgetManager& manager, UiWidgetHandle handle, const UiWidget&,
                     const UiLayoutNode&, UiAccessWidgetInfo* info) noexcept {
    const auto& s = *static_cast<const UiSearchScene*>(context);
    const auto* content = s.content;
    if (same(handle, s.root)) {
        info->role = UiAccessRole::pane;
        info->name = pick(content ? content->heading : Utf8View{}, utf8_literal("Search"));
        return;
    }
    if (same(handle, s.edit)) {
        const UiEditView view = content ? content->edit : UiEditView{};
        // The placeholder names the scope ("Search threads in general"), so it
        // is the field's name; a scope chip is announced on its own node.
        const Utf8View scope = pick(content ? content->placeholder : Utf8View{}, utf8_literal("Search messages"));
        edit_field(info, scope, scope, view.text, view.anchor, view.caret, view.focused);
        return;
    }
    if (same(handle, s.scope) && content) {
        g_name.clear().add(utf8_literal("Remove search scope: ")).add(content->scope_chip);
        info->name = g_name.view();
        info->help = utf8_literal("Search the whole server instead");
        info->leaf = true;
        info->actions = ui_access_action_invoke;
        return;
    }
    if (!s.targets || handle.slot >= manager.count || !content)
        return;
    const UiSearchTarget& target = s.targets[handle.slot];
    switch (target.action) {
    case UiSearchAction::submit:
        info->name = utf8_literal("Search");
        info->leaf = true;
        return;
    case UiSearchAction::close:
        info->name = utf8_literal("Close search");
        info->leaf = true;
        return;
    case UiSearchAction::filters:
        info->name = utf8_literal("Filters");
        info->leaf = true;
        info->state |= content->filters ? ui_access_expanded : ui_access_collapsed;
        return;
    case UiSearchAction::sort:
        g_name.clear().add(utf8_literal("Sort: ")).add(search_sort_label(
            content->sort == 0 ? UiSearchAction::newest
            : content->sort == 1 ? UiSearchAction::oldest
                                 : UiSearchAction::relevance));
        info->name = g_name.view();
        info->leaf = true;
        info->state |= content->sort_menu ? ui_access_expanded : ui_access_collapsed;
        return;
    case UiSearchAction::newest:
    case UiSearchAction::oldest:
    case UiSearchAction::relevance: {
        const u8 index = target.action == UiSearchAction::newest ? 0 : target.action == UiSearchAction::oldest ? 1 : 2;
        info->role = UiAccessRole::radio_button;
        info->group = search_sort;
        info->leaf = true;
        info->name = search_sort_label(target.action);
        info->state |= ui_access_selectable | (content->sort == index ? ui_access_selected | ui_access_checked : 0);
        info->actions = ui_access_action_select | ui_access_action_invoke;
        set_position(info, index + 1u, 3);
        return;
    }
    case UiSearchAction::previous:
        info->name = utf8_literal("Previous page");
        info->leaf = true;
        return;
    case UiSearchAction::next:
        info->name = utf8_literal("Next page");
        info->leaf = true;
        return;
    case UiSearchAction::page: {
        const u32 size = content->page_size ? content->page_size : 25;
        const u32 page = target.page_offset / size;
        g_name.clear().add(utf8_literal("Page ")).number(page + 1u);
        if (page == content->offset / size) {
            g_name.add(utf8_literal(", current page"));
            info->state |= ui_access_selected;
        }
        info->name = g_name.view();
        info->leaf = true;
        return;
    }
    case UiSearchAction::tab: {
        const u32 tab = target.source_index;
        info->role = UiAccessRole::tab;
        info->group = search_tabs;
        info->leaf = true;
        g_name.clear().add(search_tab_label(tab));
        if (tab < 5 && ((content->tab_known >> tab) & 1u))
            g_name.add(utf8_literal(", ")).number(content->tab_totals[tab]).add(utf8_literal(" results"));
        info->name = g_name.view();
        info->state |= ui_access_selectable | (content->tab == tab ? ui_access_selected : 0);
        info->actions = ui_access_action_select;
        set_position(info, tab + 1u, 5);
        return;
    }
    case UiSearchAction::result: {
        info->group = search_results;
        info->role = UiAccessRole::list_item;
        info->leaf = true;
        info->ignore_text = true;
        info->key = target.message_id ? target.message_id : hash64_mix(kHash64Seed, target.source_index);
        info->item = static_cast<u64>(target.source_index) + 1;
        const UiSearchRow* row = nullptr;
        for (u32 slot = 0; slot < s.count && slot < kUiSearchVisibleRows; ++slot)
            if (same(handle, s.rows[slot]))
                row = &s.visible[slot];
        g_name.clear();
        if (row && row->unavailable) {
            g_name.add(utf8_literal("Unavailable message"));
        } else if (row) {
            g_name.part(pick(row->author, utf8_literal("Unknown author"))).part(row->detail);
            if (row->body.bytes)
                g_name.add(utf8_literal(": ")).add(row->body);
            if (row->channel.bytes)
                g_name.add(utf8_literal(", in ")).add(row->channel);
        } else {
            g_name.add(utf8_literal("Search result"));
        }
        info->name = g_name.view();
        info->state |= ui_access_focusable;
        info->actions = ui_access_action_invoke | ui_access_action_scroll_item;
        const u64 total = content->total ? content->total : (s.source ? s.source->count : 0);
        set_position(info, static_cast<u64>(content->offset) + target.source_index + 1, total);
        return;
    }
    case UiSearchAction::rich_item:
        info->group = search_results;
        return;
    case UiSearchAction::filter_value: {
        info->group = search_filters;
        info->role = UiAccessRole::list_item;
        info->leaf = true;
        info->state |= ui_access_selectable | (target.source_index == s.choice_selected ? ui_access_selected : 0);
        info->actions = ui_access_action_select | ui_access_action_invoke;
        set_position(info, static_cast<u64>(target.source_index) + 1, s.choice_total);
        return;
    }
    case UiSearchAction::recent:
        info->group = search_filters;
        info->role = UiAccessRole::list_item;
        info->leaf = true;
        info->help = utf8_literal("Recent search");
        info->actions = ui_access_action_invoke;
        return;
    case UiSearchAction::clear_recent:
        info->group = search_filters;
        info->name = utf8_literal("Clear recent searches");
        info->leaf = true;
        return;
    default:
        if (ui_search_action_filter(target.action) || target.action == UiSearchAction::more_filters) {
            info->group = search_filters;
            info->role = UiAccessRole::list_item;
            info->leaf = true;
            info->actions = ui_access_action_invoke;
        }
        return;
    }
}

// Pinned messages

void describe_pins(void* context, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget&,
                   const UiLayoutNode&, UiAccessWidgetInfo* info) noexcept {
    const auto& s = *static_cast<const UiPinnedMessagesScene*>(context);
    const auto* content = s.content;
    const auto focused = s.focused;
    if (same(handle, s.root)) {
        info->role = UiAccessRole::pane;
        g_name.clear().add(utf8_literal("Pinned messages"));
        if (content && content->channel.bytes)
            g_name.add(utf8_literal(" in ")).add(content->channel);
        info->name = g_name.view();
        return;
    }
    struct Named {
        UiWidgetHandle handle;
        Utf8View name;
        UiPinnedMessagesAction action;
    };
    const Named buttons[]{{s.close, utf8_literal("Close pinned messages"), UiPinnedMessagesAction::close},
                          {s.refresh, utf8_literal("Refresh"), UiPinnedMessagesAction::refresh},
                          {s.previous, utf8_literal("Newer pins"), UiPinnedMessagesAction::previous},
                          {s.next, utf8_literal("Older pins"), UiPinnedMessagesAction::next},
                          {s.retry, utf8_literal("Retry loading pins"), UiPinnedMessagesAction::retry}};
    for (const auto& button : buttons)
        if (same(handle, button.handle)) {
            info->name = button.name;
            info->leaf = true;
            if (focused.action == button.action)
                info->state |= ui_access_focused;
            return;
        }
    if (same(handle, s.body_owner)) {
        info->role = UiAccessRole::list;
        info->name = utf8_literal("Pinned messages");
        info->actions = ui_access_action_scroll;
        info->scroll =
            scroll_state(s.scroll_dips, ui_pinned_messages_scene_scroll_maximum(s), ui_rect_height(s.body));
        return;
    }
    for (u32 slot = 0; slot < s.count && slot < kUiPinnedMessagesVisibleRows; ++slot) {
        const auto& row = s.visible[slot];
        if (same(handle, s.rows[slot])) {
            info->role = UiAccessRole::list_item;
            info->leaf = true;
            info->ignore_text = true;
            info->key = row.message_id;
            info->item = row.message_id;
            g_name.clear().part(pick(row.author, utf8_literal("Unknown author"))).part(row.timestamp);
            g_name.add(utf8_literal(": ")).add(pick(row.body, utf8_literal("Message has no text"))).part(row.detail);
            info->name = g_name.view();
            info->help = utf8_literal("Jump to message");
            info->state |= ui_access_focusable;
            if (!row.jump_enabled)
                info->state |= ui_access_disabled;
            if (focused.action == UiPinnedMessagesAction::jump && focused.message_id == row.message_id)
                info->state |= ui_access_focused | ui_access_selected;
            info->actions = ui_access_action_invoke | ui_access_action_focus | ui_access_action_scroll_item;
            set_position(info, static_cast<u64>(s.first) + slot + 1, s.source ? s.source->count : s.count);
            return;
        }
        if (same(handle, s.unpin[slot])) {
            g_name.clear().add(utf8_literal("Unpin message from ")).add(pick(row.author, utf8_literal("unknown author")));
            info->name = g_name.view();
            info->leaf = true;
            if (row.unpin_pending)
                info->state |= ui_access_disabled;
            if (focused.action == UiPinnedMessagesAction::unpin && focused.message_id == row.message_id)
                info->state |= ui_access_focused;
            return;
        }
    }
}

// Thread browser

Utf8View thread_kind(u8 type) noexcept {
    return type == 10 ? utf8_literal("announcement thread")
           : type == 12 ? utf8_literal("private thread")
                        : utf8_literal("public thread");
}

void thread_group(void*, u8, UiAccessNodeDesc* description) noexcept {
    description->role = UiAccessRole::tab_list;
    description->name = utf8_literal("Thread lists");
}

void describe_threads(void* context, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget&,
                      const UiLayoutNode&, UiAccessWidgetInfo* info) noexcept {
    const auto& s = *static_cast<const UiThreadBrowserScene*>(context);
    const auto* view = s.view;
    const bool focused = same(handle, s.focused.widget);
    if (focused)
        info->state |= ui_access_focused;
    if (same(handle, s.root)) {
        info->role = UiAccessRole::pane;
        g_name.clear().add(s.forum ? utf8_literal("Posts") : utf8_literal("Threads"));
        if (view && view->title.bytes)
            g_name.add(utf8_literal(" in ")).add(view->title);
        info->name = g_name.view();
        // The panel has no field of its own; the header search bar is scoped to it.
        info->help = s.forum ? utf8_literal("Search post titles with the header search bar")
                             : utf8_literal("Search thread names with the header search bar");
        return;
    }
    if (same(handle, s.active_tab) || same(handle, s.archived_tab)) {
        const bool active = same(handle, s.active_tab);
        info->role = UiAccessRole::tab;
        info->group = 1;
        info->leaf = true;
        info->name = active ? utf8_literal("Active threads") : utf8_literal("Archived threads");
        const bool selected = active ? s.tab == UiThreadBrowserTab::active : s.tab == UiThreadBrowserTab::archived;
        info->state |= ui_access_selectable | (selected ? ui_access_selected : 0);
        info->actions = ui_access_action_select;
        set_position(info, active ? 1 : 2, 2);
        return;
    }
    struct Named {
        UiWidgetHandle handle;
        Utf8View name;
    };
    const Named buttons[]{{s.close, utf8_literal("Close thread browser")}, {s.previous, utf8_literal("Previous page")},
                          {s.next, utf8_literal("Next page")},          {s.retry, utf8_literal("Retry loading threads")},
                          {s.create_post, utf8_literal("Create post")}};
    for (const auto& button : buttons)
        if (same(handle, button.handle)) {
            info->name = same(handle, s.create_post) && s.forum ? utf8_literal("New post") : button.name;
            info->leaf = true;
            return;
        }
    if (view && view->forum && s.backing) {
        const auto& forum = *view->forum;
        if (same(handle, s.forum_sort)) {
            g_name.clear().add(utf8_literal("Sort by: ")).add(ui_forum_sort_label(forum.sort));
            info->name = g_name.view();
            info->leaf = true;
            return;
        }
        if (same(handle, s.forum_list) || same(handle, s.forum_gallery)) {
            const bool list = same(handle, s.forum_list);
            info->name = list ? utf8_literal("List") : utf8_literal("Gallery");
            info->leaf = true;
            const bool chosen = (forum.layout == UiForumLayout::list) == list;
            info->state |= ui_access_toggleable | (chosen ? ui_access_checked : 0);
            return;
        }
        for (u32 i = 0; i < forum.tags.count && i < kUiForumTagCapacity; ++i) {
            if (!same(handle, s.backing->chips[i]))
                continue;
            g_name.clear().part(pick(forum.tags[i].name, utf8_literal("Tag"))).part(utf8_literal("tag filter"));
            info->name = g_name.view();
            info->leaf = true;
            info->state |= ui_access_toggleable | (forum.tags[i].selected ? ui_access_checked : 0);
            return;
        }
    }
    if (same(handle, s.body_owner)) {
        info->role = UiAccessRole::list;
        g_name.clear().add(s.tab == UiThreadBrowserTab::archived ? utf8_literal("Archived threads")
                           : s.tab == UiThreadBrowserTab::search  ? utf8_literal("Matching threads")
                                                                  : utf8_literal("Active threads"));
        info->name = g_name.view();
        info->actions = ui_access_action_scroll;
        info->scroll = scroll_state(s.scroll_dips, ui_thread_browser_scene_scroll_maximum(s), ui_rect_height(s.body));
        return;
    }
    if (!s.backing || !view)
        return;
    for (u32 index = 0; index < s.count && index < view->rows.count && index < kUiThreadBrowserPageCapacity; ++index) {
        if (!same(handle, s.backing->rows[index]))
            continue;
        const auto& row = view->rows[index];
        info->role = UiAccessRole::list_item;
        info->leaf = true;
        info->ignore_text = true;
        info->key = row.thread_id;
        info->item = row.thread_id;
        if (view->forum && index < view->forum->posts.count) {
            // Forum cards read as posts: title, pinned, tags, author, messages, activity.
            const auto& forum = *view->forum;
            const auto& post = forum.posts[index];
            g_name.clear().part(pick(row.name, utf8_literal("Untitled post")));
            if (post.pinned)
                g_name.part(utf8_literal("pinned"));
            for (u32 tag = 0; tag < post.tag_count; ++tag)
                for (u32 known = 0; known < forum.tags.count; ++known)
                    if (forum.tags[known].id == post.tags[tag])
                        g_name.part(forum.tags[known].name);
            if (post.author_name.bytes)
                g_name.part(utf8_literal("by ")).add(post.author_name);
            if (post.metadata_known) {
                char digits[12];
                u32 bytes = 0, value = post.reply_count;
                char reversed[12];
                u32 count = 0;
                do { reversed[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
                while (count) digits[bytes++] = reversed[--count];
                g_name.part({digits, bytes}).add(post.reply_count == 1 ? utf8_literal(" message") : utf8_literal(" messages"));
            }
            if (post.activity.bytes)
                g_name.part(post.activity);
        } else {
            g_name.clear().part(pick(row.name, utf8_literal("Unnamed thread"))).part(thread_kind(row.type));
            g_name.part(row.archived ? utf8_literal("archived") : utf8_literal("active"));
            if (row.membership_known && row.joined)
                g_name.part(utf8_literal("joined"));
        }
        info->name = g_name.view();
        info->state |= ui_access_focusable | (focused ? ui_access_selected : 0);
        info->actions = ui_access_action_invoke | ui_access_action_focus | ui_access_action_scroll_item;
        const u64 page = static_cast<u64>(view->page_index) * kUiThreadBrowserPageCapacity;
        const u64 total = view->tab != UiThreadBrowserTab::archived && view->total ? view->total : page + view->rows.count;
        set_position(info, page + index + 1, total);
        return;
    }
}

// Inbox

void inbox_group(void*, u8, UiAccessNodeDesc* description) noexcept {
    description->role = UiAccessRole::tab_list;
    description->name = utf8_literal("Inbox sections");
}

void describe_inbox(void* context, const UiWidgetManager& manager, UiWidgetHandle handle, const UiWidget&,
                    const UiLayoutNode&, UiAccessWidgetInfo* info) noexcept {
    const auto& s = *static_cast<const UiInboxScene*>(context);
    const auto* content = s.content;
    if (handle.slot < manager.count && handle.slot < kUiInboxWidgetCapacity && s.focused.action != UiInboxAction::none &&
        ui_inbox_target_equal(s.targets[handle.slot], s.focused))
        info->state |= ui_access_focused;
    if (same(handle, s.root)) {
        info->role = UiAccessRole::pane;
        info->name = utf8_literal("Inbox");
        return;
    }
    for (u32 tab = 0; tab < 2; ++tab)
        if (same(handle, s.tabs[tab])) {
            info->role = UiAccessRole::tab;
            info->group = 1;
            info->leaf = true;
            info->name = tab == 0 ? utf8_literal("Mentions") : utf8_literal("Unreads");
            const bool selected = static_cast<u32>(s.tab) == tab;
            info->state |= ui_access_selectable | (selected ? ui_access_selected : 0);
            info->actions = ui_access_action_select;
            set_position(info, tab + 1, 2);
            return;
        }
    if (same(handle, s.close)) {
        info->name = utf8_literal("Close inbox");
        info->leaf = true;
        return;
    }
    for (u32 chip = 0; chip < 3; ++chip)
        if (same(handle, s.chips[chip])) {
            info->role = UiAccessRole::check_box;
            info->leaf = true;
            info->name = chip == 0   ? utf8_literal("Only this server")
                         : chip == 1 ? utf8_literal("Include @everyone mentions")
                                     : utf8_literal("Include @role mentions");
            const bool checked = content && (chip == 0 ? content->this_server : chip == 1 ? content->everyone
                                                                                           : content->roles);
            info->state |= ui_access_toggleable | (checked ? ui_access_checked : 0);
            info->actions = ui_access_action_toggle;
            return;
        }
    if (same(handle, s.body_owner)) {
        info->role = UiAccessRole::list;
        info->name = s.tab == UiInboxTab::mentions ? utf8_literal("Recent mentions") : utf8_literal("Unread channels");
        info->actions = ui_access_action_scroll;
        info->scroll = scroll_state(s.scroll_dips, ui_inbox_scene_scroll_maximum(s), ui_rect_height(s.body));
        return;
    }
    for (u32 slot = 0; slot < s.count && slot < kUiInboxVisibleRows; ++slot) {
        const auto& row = s.visible[slot];
        if (same(handle, s.rows[slot])) {
            info->role = UiAccessRole::list_item;
            info->leaf = true;
            info->ignore_text = true;
            info->key = hash64_mix(row.id, static_cast<u64>(row.kind) + 1);
            info->item = static_cast<u64>(s.first) + slot + 1;
            g_name.clear();
            if (row.kind == UiInboxRowKind::mention) {
                g_name.part(pick(row.title, utf8_literal("Unknown author"))).part(row.timestamp);
                if (row.body.bytes)
                    g_name.add(utf8_literal(": ")).add(row.body);
                if (row.detail.bytes)
                    g_name.add(utf8_literal(", in ")).add(row.detail);
            } else {
                g_name.part(row.title);
                if (row.mention_count)
                    g_name.add(utf8_literal(", ")).number(row.mention_count).add(
                        row.mention_count == 1 ? utf8_literal(" mention") : utf8_literal(" mentions"));
                else
                    g_name.part(utf8_literal("unread"));
                if (row.muted)
                    g_name.part(utf8_literal("muted"));
            }
            info->name = g_name.view();
            info->state |= ui_access_focusable;
            info->actions = ui_access_action_invoke | ui_access_action_focus | ui_access_action_scroll_item;
            set_position(info, static_cast<u64>(s.first) + slot + 1, s.source ? s.source->count : s.count);
            return;
        }
        if (same(handle, s.actions[slot])) {
            g_name.clear();
            if (row.kind == UiInboxRowKind::group)
                g_name.add(utf8_literal("Mark all as read in ")).add(row.title);
            else
                g_name.add(utf8_literal("Mark ")).add(row.title).add(utf8_literal(" as read"));
            info->name = g_name.view();
            info->leaf = true;
            return;
        }
    }
}

// Forum post

void describe_forum(void* context, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget&,
                    const UiLayoutNode&, UiAccessWidgetInfo* info) noexcept {
    const auto& s = *static_cast<const ForumPostScene*>(context);
    const auto& c = s.content;
    const ForumPostTarget focus = forum_post_focused(s);
    auto element = [&](ForumPostElement e) noexcept { return same(handle, s.elements[static_cast<u32>(e)]); };
    if (focus.element != ForumPostElement::count) {
        const UiWidgetHandle focused = focus.element == ForumPostElement::tags && focus.tag < kForumPostTagCapacity
                                           ? s.tags[focus.tag]
                                       : focus.attachment_widget.generation ? focus.attachment_widget
                                                                            : s.elements[static_cast<u32>(focus.element)];
        if (same(handle, focused))
            info->state |= ui_access_focused;
    }
    if (element(ForumPostElement::root)) {
        info->transparent = true;
        return;
    }
    if (element(ForumPostElement::panel)) {
        info->role = UiAccessRole::dialog;
        info->state |= ui_access_modal;
        info->name = pick(c.heading, utf8_literal("New post"));
        info->help = c.context;
        return;
    }
    if (element(ForumPostElement::heading)) {
        info->role = UiAccessRole::header;
        return;
    }
    for (u32 field = 0; field < 2; ++field) {
        if (!element(field == 0 ? ForumPostElement::title : ForumPostElement::body))
            continue;
        const auto& view = c.fields[field];
        const bool focused = focus.element == (field == 0 ? ForumPostElement::title : ForumPostElement::body);
        edit_field(info,
                   pick(field == 0 ? c.title_label : c.body_label,
                        field == 0 ? utf8_literal("Post title") : utf8_literal("Post content")),
                   field == 0 ? Utf8View{} : c.hint, view.text, byte_offset(view.text, view.anchor_utf16),
                   byte_offset(view.text, view.caret_utf16), focused);
        if (field == 1)
            info->state |= ui_access_multiline;
        if (!c.editable)
            info->state |= ui_access_read_only;
        return;
    }
    if (element(ForumPostElement::tags)) {
        info->role = UiAccessRole::group;
        info->name = pick(c.tags_hint, utf8_literal("Tags"));
        return;
    }
    for (u32 tag = 0; tag < c.tags.count && tag < kForumPostTagCapacity; ++tag)
        if (same(handle, s.tags[tag])) {
            info->role = UiAccessRole::check_box;
            info->leaf = true;
            info->name = forum_post_tag_label(c.tags[tag].name);
            info->state |= ui_access_toggleable | (c.tags[tag].selected ? ui_access_checked : 0);
            info->actions = ui_access_action_toggle;
            set_position(info, tag + 1, c.tags.count);
            return;
        }
    if (element(ForumPostElement::risk)) {
        info->role = UiAccessRole::check_box;
        info->leaf = true;
        info->name = pick(c.risk, utf8_literal("I understand the risk"));
        info->state |= ui_access_toggleable | (c.risk_acknowledged ? ui_access_checked : 0);
        info->actions = ui_access_action_toggle;
        return;
    }
    if (element(ForumPostElement::status) || element(ForumPostElement::attachment_status)) {
        info->role = UiAccessRole::status;
        info->state |= ui_access_live;
        info->leaf = true;
        return;
    }
    if (element(ForumPostElement::attachments)) {
        info->role = UiAccessRole::list;
        info->name = utf8_literal("Attachments");
        return;
    }
}

// Text form

void describe_text_form(void* context, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget&,
                        const UiLayoutNode&, UiAccessWidgetInfo* info) noexcept {
    const auto& s = *static_cast<const TextFormScene*>(context);
    const auto& c = s.content;
    const TextFormElement focus = text_form_focused(s);
    auto element = [&](TextFormElement e) noexcept { return same(handle, s.elements[static_cast<u32>(e)]); };
    if (focus != TextFormElement::count && element(focus))
        info->state |= ui_access_focused;
    if (element(TextFormElement::root)) {
        info->transparent = true;
        return;
    }
    if (element(TextFormElement::panel)) {
        info->role = UiAccessRole::dialog;
        info->state |= ui_access_modal;
        info->name = pick(c.title, utf8_literal("Dialog"));
        info->help = c.context;
        return;
    }
    if (element(TextFormElement::title)) {
        info->role = UiAccessRole::header;
        return;
    }
    if (element(TextFormElement::edit)) {
        edit_field(info, pick(c.label, pick(c.title, utf8_literal("Text"))), Utf8View{}, c.text,
                   byte_offset(c.text, c.anchor_utf16), byte_offset(c.text, c.caret_utf16),
                   focus == TextFormElement::edit);
        if (!c.single_line)
            info->state |= ui_access_multiline;
        if (!c.editable)
            info->state |= ui_access_read_only;
        return;
    }
    if (element(TextFormElement::status)) {
        info->role = UiAccessRole::status;
        info->state |= ui_access_live;
        info->leaf = true;
        return;
    }
    if (element(TextFormElement::accept)) {
        info->leaf = true;
        info->name = pick(c.accept, utf8_literal("OK"));
        if (c.danger)
            info->help = utf8_literal("This action cannot be undone");
        return;
    }
    if (element(TextFormElement::cancel)) {
        info->leaf = true;
        info->name = pick(c.cancel, utf8_literal("Cancel"));
        return;
    }
    if (element(TextFormElement::option)) {
        info->role = UiAccessRole::check_box;
        info->leaf = true;
        info->name = c.option;
        if (c.option_checked)
            info->state |= ui_access_checked;
        return;
    }
}

// Image viewer

Utf8View viewer_button(u32 index, const UiImageViewer& v) noexcept {
    switch (index) {
    case ui_image_viewer_zoom_out: return utf8_literal("Zoom out");
    case ui_image_viewer_zoom_in: return utf8_literal("Zoom in");
    case ui_image_viewer_fit: return utf8_literal("Fit to window");
    case ui_image_viewer_close: return v.video ? utf8_literal("Close video") : utf8_literal("Close image");
    case ui_image_viewer_save: return utf8_literal("Save");
    case ui_image_viewer_open_original: return utf8_literal("Open original");
    case ui_image_viewer_copy: return v.copied ? utf8_literal("Copied") : utf8_literal("Copy image");
    case ui_image_viewer_previous: return utf8_literal("Previous");
    case ui_image_viewer_next: return utf8_literal("Next");
    default: return {};
    }
}

void mount_viewer_media(void* context, UiAccessTree* tree, u32 node) noexcept {
    const auto& v = *static_cast<const UiImageViewer*>(context);
    UiAccessNodeDesc media{};
    media.key = ui_access_key(tree->nodes[node].surface, 0x494D4147ull);
    media.rectangle = ui_rect_has_area(v.display) ? v.display : v.image;
    media.visible = ui_rect_intersection(media.rectangle, v.viewport);
    media.role = UiAccessRole::image;
    media.surface = tree->nodes[node].surface;
    g_name.clear().add(v.video ? utf8_literal("Video") : utf8_literal("Image"));
    if (v.item_count > 1)
        g_name.add(utf8_literal(" ")).number(v.item + 1u).add(utf8_literal(" of ")).number(v.item_count);
    media.name = g_name.view();
    UiAccessNameBuilder help{};
    if (!v.video && v.zoom > 0) {
        help.add(utf8_literal("Zoom ")).number(static_cast<u64>(v.zoom * 100.0f + 0.5f)).add(utf8_literal("%"));
        media.help = help.view();
    }
    ui_access_add(tree, node, media);
}

void describe_viewer(void* context, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget&,
                     const UiLayoutNode&, UiAccessWidgetInfo* info) noexcept {
    const auto& v = *static_cast<const UiImageViewer*>(context);
    if (handle.slot == 0) {
        info->role = UiAccessRole::dialog;
        info->state |= ui_access_modal;
        g_name.clear().add(v.video ? utf8_literal("Video viewer") : utf8_literal("Image viewer"));
        if (v.item_count > 1)
            g_name.add(utf8_literal(", ")).number(v.item + 1u).add(utf8_literal(" of ")).number(v.item_count);
        info->name = g_name.view();
        info->mount = mount_viewer_media;
        info->mount_context = context;
        return;
    }
    for (u32 index = 0; index < ui_image_viewer_button_count; ++index)
        if (same(handle, v.buttons[index])) {
            if (!ui_rect_has_area(ui_image_viewer_button(v, index))) {
                info->skip = true;
                return;
            }
            info->role = UiAccessRole::button;
            info->leaf = true;
            info->name = viewer_button(index, v);
            return;
        }
}

// Media picker

struct PickerContext {
    const UiMediaPickerScene* scene;
    u16 row[kUiMediaPickerVisibleCells], column[kUiMediaPickerVisibleCells];
    u16 rows, columns;
};
PickerContext g_picker;

Utf8View picker_noun(UiMediaPickerKind kind) noexcept {
    return kind == UiMediaPickerKind::gif ? utf8_literal("GIFs")
           : kind == UiMediaPickerKind::sticker ? utf8_literal("Stickers")
                                                : utf8_literal("Emoji");
}

void picker_group(void* context, u8 group, UiAccessNodeDesc* description) noexcept {
    const auto& p = *static_cast<const PickerContext*>(context);
    const auto& s = *p.scene;
    switch (group) {
    case 1:
        description->role = UiAccessRole::tab_list;
        description->name = utf8_literal("Picker sections");
        break;
    case 2:
        description->role = UiAccessRole::list;
        description->name = picker_noun(s.content ? s.content->kind : s.kind);
        description->actions = ui_access_action_scroll | ui_access_action_grid;
        description->placement.grid_row = p.rows;
        description->placement.grid_column = p.columns;
        description->scroll =
            scroll_state(s.scroll_dips, s.content_height - ui_rect_height(s.body), ui_rect_height(s.body));
        break;
    case 3:
        description->role = UiAccessRole::list;
        description->name = utf8_literal("Categories");
        break;
    default:
        break;
    }
}

const UiMediaPickerSection* picker_section(const UiMediaPickerScene& s, u32 source_index) noexcept {
    if (!s.source)
        return nullptr;
    for (u32 index = 0; index < s.source->sections.count; ++index) {
        const auto& section = s.source->sections[index];
        if (source_index >= section.first && source_index < section.first + section.count)
            return &section;
    }
    return nullptr;
}

void describe_picker(void* context, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget&,
                     const UiLayoutNode&, UiAccessWidgetInfo* info) noexcept {
    const auto& p = *static_cast<const PickerContext*>(context);
    const auto& s = *p.scene;
    const auto* content = s.content;
    const UiMediaPickerKind kind = content ? content->kind : s.kind;
    if (same(handle, s.root)) {
        info->role = UiAccessRole::dialog;
        info->name = kind == UiMediaPickerKind::gif       ? utf8_literal("GIF picker")
                     : kind == UiMediaPickerKind::sticker ? utf8_literal("Sticker picker")
                                                          : utf8_literal("Emoji picker");
        return;
    }
    for (u32 tab = 0; tab < 3; ++tab)
        if (same(handle, s.tab_widgets[tab])) {
            info->role = UiAccessRole::tab;
            info->group = 1;
            info->leaf = true;
            info->name = picker_noun(static_cast<UiMediaPickerKind>(tab));
            info->state |= ui_access_selectable | (static_cast<u32>(kind) == tab ? ui_access_selected : 0);
            info->actions = ui_access_action_select;
            return;
        }
    if (same(handle, s.edit)) {
        const UiEditView view = content ? content->edit : UiEditView{};
        g_name.clear().add(utf8_literal("Search ")).add(kind == UiMediaPickerKind::gif ? utf8_literal("GIFs")
                                                        : kind == UiMediaPickerKind::sticker ? utf8_literal("stickers")
                                                                                             : utf8_literal("emoji"));
        edit_field(info, g_name.view(), content ? content->placeholder : Utf8View{}, view.text, view.anchor, view.caret,
                   view.focused);
        return;
    }
    if (same(handle, s.back)) {
        info->name = utf8_literal("Back");
        info->leaf = true;
        return;
    }
    if (!s.projection)
        return;
    const auto& projection = *s.projection;
    for (u32 index = 0; index < s.header_count && index < kUiMediaPickerVisibleSections; ++index)
        if (same(handle, projection.headers[index].widget)) {
            const auto section = projection.headers[index].section_index;
            info->role = UiAccessRole::header;
            info->group = 2;
            info->leaf = true;
            if (s.source && section < s.source->sections.count) {
                info->name = s.source->sections[section].label;
                info->state |= s.source->sections[section].expanded ? ui_access_expanded : ui_access_collapsed;
            }
            info->actions = ui_access_action_invoke;
            return;
        }
    for (u32 index = 0; index < s.rail_count && index < kUiMediaPickerVisibleSections; ++index)
        if (same(handle, projection.rail[index].widget)) {
            const auto section = projection.rail[index].section_index;
            info->role = UiAccessRole::list_item;
            info->group = 3;
            info->leaf = true;
            if (s.source && section < s.source->sections.count)
                info->name = s.source->sections[section].label;
            info->actions = ui_access_action_invoke;
            return;
        }
    for (u32 index = 0; index < s.cell_count && index < kUiMediaPickerVisibleCells; ++index) {
        const auto& cell = projection.cells[index];
        if (!same(handle, cell.widget))
            continue;
        const auto& item = cell.item;
        info->role = UiAccessRole::list_item;
        info->group = 2;
        info->leaf = true;
        info->ignore_text = true;
        info->key = item.id ? item.id : hash64_mix(kHash64Seed, cell.source_index);
        info->item = static_cast<u64>(cell.source_index) + 1;
        g_name.clear();
        if (!item.enabled)
            g_name.part(pick(item.label, utf8_literal("Item"))).part(utf8_literal("unavailable"));
        else
            g_name.part(pick(item.label, item.glyph)).part(item.detail);
        if (item.kind == UiMediaPickerItemKind::category)
            g_name.part(utf8_literal("category"));
        if (item.favorite)
            g_name.part(utf8_literal("favorite"));
        info->name = g_name.view();
        info->state |= ui_access_selectable | ui_access_focusable;
        if (cell.source_index == s.selected_index)
            info->state |= ui_access_selected | ui_access_focused;
        if (!item.enabled)
            info->state |= ui_access_disabled;
        info->actions = ui_access_action_select | ui_access_action_invoke | ui_access_action_scroll_item |
                        ui_access_action_grid_item;
        info->placement.grid_row = p.row[index];
        info->placement.grid_column = p.column[index];
        if (const auto* section = picker_section(s, cell.source_index))
            set_position(info, cell.source_index - section->first + 1, section->count);
        return;
    }
}

// Visual rows of the visible cells, in layout order.
void measure_picker_grid(PickerContext* context) noexcept {
    const auto& s = *context->scene;
    context->rows = context->columns = 0;
    if (!s.projection)
        return;
    float top = 0;
    u16 row = 0, column = 0;
    for (u32 index = 0; index < s.cell_count && index < kUiMediaPickerVisibleCells; ++index) {
        const auto& cell = s.projection->cells[index];
        if (index == 0) {
            top = cell.rectangle.top;
        } else if (cell.rectangle.top > top + 0.5f) {
            top = cell.rectangle.top;
            ++row;
            column = 0;
        }
        context->row[index] = row;
        context->column[index] = column++;
        if (column > context->columns)
            context->columns = column;
        context->rows = static_cast<u16>(row + 1);
    }
}

// Call

constexpr u64 kCallKey = 0x43414C4C00000000ull;

u32 add_synthetic(UiAccessTree* tree, u32 parent, u8 surface, u64 identity, UiAccessRole role, UiRect rectangle,
                  UiRect clip, Utf8View name, u16 state = 0, u16 actions = 0) noexcept {
    UiAccessNodeDesc description{};
    description.key = ui_access_key(surface, kCallKey ^ identity);
    description.rectangle = rectangle;
    description.visible = ui_rect_intersection(rectangle, clip);
    description.role = role;
    description.name = name;
    description.state = state;
    description.actions = actions;
    description.surface = surface;
    if (!ui_rect_has_area(description.visible)) {
        description.state |= ui_access_offscreen;
        description.visible = {};
    }
    return ui_access_add(tree, parent, description);
}

bool call_toggle(UiCallControl control) noexcept {
    return control == UiCallControl::mute || control == UiCallControl::deafen ||
           control == UiCallControl::fullscreen || control == UiCallControl::stage_hand ||
           control == UiCallControl::soundboard;
}

void add_call_control(UiAccessTree* tree, u32 parent, u8 surface, const UiCallScene& s, UiCallControl control,
                      u64 user = 0, UiRect rectangle = {}) noexcept {
    if (!ui_rect_has_area(rectangle))
        rectangle = s.controls[static_cast<u32>(control)];
    if (!ui_rect_has_area(rectangle))
        return;
    const bool toggle = call_toggle(control);
    u16 state = ui_access_focusable;
    if (!ui_call_control_enabled(s, control))
        state |= ui_access_disabled;
    if (toggle)
        state |= ui_access_toggleable | (ui_call_control_active(s, control) ? ui_access_checked : 0);
    Utf8View name = ui_call_control_label(s, control);
    if (control == UiCallControl::stage_invite)
        name = utf8_literal("Invite to speak");
    else if (control == UiCallControl::stream_dismiss && !name.bytes)
        name = utf8_literal("Dismiss");
    add_synthetic(tree, parent, surface, (static_cast<u64>(control) << 48) ^ user,
                  toggle ? UiAccessRole::toggle_button : UiAccessRole::button, rectangle, s.viewport,
                  pick(name, utf8_literal("Call control")), state,
                  toggle ? ui_access_action_toggle : ui_access_action_invoke);
}

void call_tile_name(const UiCallParticipant& p, UiAccessNameBuilder* name) noexcept {
    name->clear();
    if (p.stream != UiCallStream::none) {
        name->add(pick(p.name, utf8_literal("Someone"))).add(utf8_literal("'s stream"));
        switch (p.stream) {
        case UiCallStream::available: name->part(utf8_literal("live, not watching")); break;
        case UiCallStream::connecting: name->part(utf8_literal("connecting")); break;
        case UiCallStream::live:
            name->part(utf8_literal("watching"));
            if (p.paused)
                name->part(utf8_literal("paused"));
            break;
        case UiCallStream::reconnecting: name->part(utf8_literal("reconnecting")); break;
        case UiCallStream::ended:
            name->part(pick(ui_call_stream_notice_text(p.notice), utf8_literal("ended")));
            break;
        default: break;
        }
        if (p.viewers)
            name->add(utf8_literal(", ")).number(p.viewers).add(p.viewers == 1 ? utf8_literal(" viewer")
                                                                               : utf8_literal(" viewers"));
        return;
    }
    name->add(pick(p.name, utf8_literal("Participant")));
    if (p.self)
        name->part(utf8_literal("you"));
    if (p.speaking)
        name->part(utf8_literal("speaking"));
    if (p.deafened)
        name->part(utf8_literal("deafened"));
    else if (p.muted)
        name->part(utf8_literal("muted"));
    if (p.video_ssrc)
        name->part(utf8_literal("camera on"));
    if (p.live)
        name->part(utf8_literal("live"));
}

void mount_call(void* context, UiAccessTree* tree, u32 node) noexcept {
    const auto& s = *static_cast<const UiCallScene*>(context);
    const u8 surface = tree->nodes[node].surface;
    // Header: title (live stage topic), status, header controls.
    const Utf8View title = s.stage_active && s.stage_topic_bytes ? Utf8View{s.stage_topic, s.stage_topic_bytes}
                                                                  : Utf8View{s.title, s.title_bytes};
    if (title.bytes)
        add_synthetic(tree, node, surface, 1, UiAccessRole::header, s.header, s.viewport, title);
    if (!s.connected)
        add_synthetic(tree, node, surface, 2, UiAccessRole::status, s.header, s.viewport, utf8_literal("Connecting"),
                      ui_access_live);
    if (s.stage_notice_bytes)
        add_synthetic(tree, node, surface, 3, UiAccessRole::status, s.header, s.viewport,
                      {s.stage_notice, s.stage_notice_bytes}, ui_access_live);
    static constexpr UiCallControl kHeader[]{UiCallControl::stage_start, UiCallControl::stage_topic,
                                             UiCallControl::stage_end, UiCallControl::close};
    for (const auto control : kHeader)
        add_call_control(tree, node, surface, s, control);
    if (ui_rect_has_area(s.stage_banner)) {
        add_synthetic(tree, node, surface, 4, UiAccessRole::text, s.stage_banner, s.viewport,
                      utf8_literal("You're invited to speak"));
        add_call_control(tree, node, surface, s, UiCallControl::stage_accept);
        add_call_control(tree, node, surface, s, UiCallControl::stage_decline);
    }
    // Participants (stage speakers) with speaking and mute state.
    g_name.clear().add(s.stage_active ? utf8_literal("Speakers") : utf8_literal("Participants"));
    g_name.add(utf8_literal(", ")).number(s.tile_count);
    const UiRect area = s.stage_active && ui_rect_has_area(s.stage_speakers) ? s.stage_speakers : s.stage;
    const u32 list = add_synthetic(tree, node, surface, 5, UiAccessRole::list, area, s.viewport, g_name.view(), 0,
                                   ui_access_action_scroll);
    if (list != kUiAccessNone) {
        const float maximum_scroll = s.content_height - ui_rect_height(s.stage);
        tree->nodes[list].scroll = scroll_state(s.scroll, maximum_scroll, ui_rect_height(s.stage));
        for (u32 index = 0; index < s.tile_count && index < kUiCallMaximumTiles; ++index) {
            const auto& tile = s.tiles[index];
            if (tile.participant >= s.participant_count)
                continue;
            const auto& participant = s.participants[tile.participant];
            UiAccessNameBuilder name{};
            call_tile_name(participant, &name);
            const u16 state = static_cast<u16>(ui_access_selectable | (tile.focused ? ui_access_selected : 0));
            const u32 item = add_synthetic(tree, list, surface, 0x100000000ull ^ tile.user_id, UiAccessRole::list_item,
                                           tile.display, ui_rect_intersection(tile.clip, s.viewport), name.view(),
                                           state, ui_access_action_invoke);
            if (item == kUiAccessNone)
                break;
            tree->nodes[item].item = tile.user_id;
            tree->nodes[item].placement.position = index + 1;
            tree->nodes[item].placement.set_size = s.tile_count;
            static constexpr UiCallControl kTile[]{UiCallControl::stream_watch, UiCallControl::stream_dismiss,
                                                   UiCallControl::stage_move};
            for (const auto control : kTile)
                add_call_control(tree, item, surface, s, control, tile.user_id,
                                 ui_call_scene_tile_control(s, tile.user_id, control));
        }
    }
    if (ui_rect_has_area(s.stage_audience)) {
        g_name.clear().add(utf8_literal("Audience, ")).number(s.stage_audience_count);
        const u32 audience =
            add_synthetic(tree, node, surface, 6, UiAccessRole::list, s.stage_audience, s.viewport, g_name.view());
        for (u32 index = 0; audience != kUiAccessNone && index < s.stage_audience_shown; ++index) {
            UiCallTileMedia media{};
            const UiRect rectangle = ui_call_scene_stage_avatar(s, index, &media) ? media.avatar : s.stage_audience;
            add_synthetic(tree, audience, surface, 0x200000000ull ^ s.stage_people[index].user_id,
                          UiAccessRole::list_item, rectangle, s.stage_audience,
                          pick(s.stage_people[index].name, utf8_literal("Listener")));
        }
    }
    if (ui_rect_has_area(s.stage_requests)) {
        g_name.clear().add(utf8_literal("Requests to speak, ")).number(s.stage_request_count);
        const u32 requests =
            add_synthetic(tree, node, surface, 7, UiAccessRole::list, s.stage_requests, s.viewport, g_name.view());
        for (u32 index = 0; requests != kUiAccessNone && index < s.stage_request_shown; ++index) {
            const auto& person = s.stage_people[kUiCallStageAudienceShown + index];
            UiCallTileMedia media{};
            const UiRect rectangle = ui_call_scene_stage_avatar(s, s.stage_audience_shown + index, &media)
                                         ? media.avatar
                                         : s.stage_requests;
            const u32 row = add_synthetic(tree, requests, surface, 0x300000000ull ^ person.user_id,
                                          UiAccessRole::list_item, rectangle, s.stage_requests,
                                          pick(person.name, utf8_literal("Listener")));
            if (row != kUiAccessNone)
                add_call_control(tree, row, surface, s, UiCallControl::stage_invite, person.user_id,
                                 call_stage::invite_button(s, index));
        }
    }
    const u32 bar = add_synthetic(tree, node, surface, 8, UiAccessRole::group, s.bar, s.viewport,
                                  utf8_literal("Call controls"));
    if (bar == kUiAccessNone)
        return;
    static constexpr UiCallControl kBar[]{UiCallControl::mute,           UiCallControl::deafen,
                                          UiCallControl::camera,         UiCallControl::screen_share,
                                          UiCallControl::stage_hand,     UiCallControl::stage_become,
                                          UiCallControl::stage_audience, UiCallControl::soundboard,
                                          UiCallControl::disconnect,     UiCallControl::fullscreen};
    for (const auto control : kBar)
        add_call_control(tree, bar, surface, s, control);
}

void describe_call(void* context, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget&,
                   const UiLayoutNode&, UiAccessWidgetInfo* info) noexcept {
    const auto& s = *static_cast<const UiCallScene*>(context);
    if (handle.slot != 0)
        return;
    info->role = UiAccessRole::pane;
    g_name.clear().add(s.stage_active ? utf8_literal("Stage") : utf8_literal("Call"));
    if (s.title_bytes)
        g_name.add(utf8_literal(": ")).add({s.title, s.title_bytes});
    info->name = g_name.view();
    info->leaf = true;
    info->ignore_text = true; // names, badges and labels are projected by mount_call
    info->mount = mount_call;
    info->mount_context = context;
}

} // namespace

u32 ui_access_add_quick_switcher(UiAccessTree* tree, u32 parent, const UiQuickSwitcherScene& scene,
                                 u8 surface) noexcept {
    if (!scene.active)
        return kUiAccessNone;
    return add_widgets(tree, parent, scene.widgets, describe_quick, const_cast<UiQuickSwitcherScene*>(&scene), surface);
}

u32 ui_access_add_search(UiAccessTree* tree, u32 parent, const UiSearchScene& scene, u8 surface) noexcept {
    if (!scene.active)
        return kUiAccessNone;
    return add_widgets(tree, parent, scene.widgets, describe_search, const_cast<UiSearchScene*>(&scene), surface,
                       search_group);
}

u32 ui_access_add_pinned_messages(UiAccessTree* tree, u32 parent, const UiPinnedMessagesScene& scene,
                                  u8 surface) noexcept {
    if (!scene.active)
        return kUiAccessNone;
    return add_widgets(tree, parent, scene.widgets, describe_pins, const_cast<UiPinnedMessagesScene*>(&scene), surface);
}

u32 ui_access_add_thread_browser(UiAccessTree* tree, u32 parent, const UiThreadBrowserScene& scene,
                                 u8 surface) noexcept {
    if (!scene.active)
        return kUiAccessNone;
    return add_widgets(tree, parent, scene.widgets, describe_threads, const_cast<UiThreadBrowserScene*>(&scene),
                       surface, thread_group);
}

u32 ui_access_add_inbox(UiAccessTree* tree, u32 parent, const UiInboxScene& scene, u8 surface) noexcept {
    if (!scene.active)
        return kUiAccessNone;
    return add_widgets(tree, parent, scene.widgets, describe_inbox, const_cast<UiInboxScene*>(&scene), surface,
                       inbox_group);
}

u32 ui_access_add_call(UiAccessTree* tree, u32 parent, const UiCallScene& scene, u8 surface) noexcept {
    if (!scene.active)
        return kUiAccessNone;
    return add_widgets(tree, parent, scene.widgets, describe_call, const_cast<UiCallScene*>(&scene), surface);
}

u32 ui_access_add_forum_post(UiAccessTree* tree, u32 parent, const ForumPostScene& scene, u8 surface) noexcept {
    if (!scene.active)
        return kUiAccessNone;
    return add_widgets(tree, parent, scene.widgets, describe_forum, const_cast<ForumPostScene*>(&scene), surface);
}

u32 ui_access_add_text_form(UiAccessTree* tree, u32 parent, const TextFormScene& scene, u8 surface) noexcept {
    if (!scene.active)
        return kUiAccessNone;
    return add_widgets(tree, parent, scene.widgets, describe_text_form, const_cast<TextFormScene*>(&scene), surface);
}

u32 ui_access_add_image_viewer(UiAccessTree* tree, u32 parent, const UiImageViewer& viewer, u8 surface) noexcept {
    if (!viewer.active)
        return kUiAccessNone;
    return add_widgets(tree, parent, viewer.widgets, describe_viewer, const_cast<UiImageViewer*>(&viewer), surface,
                       nullptr, viewer.keyboard_focus);
}

u32 ui_access_add_media_picker(UiAccessTree* tree, u32 parent, const UiMediaPickerScene& scene,
                               u8 surface) noexcept {
    if (!scene.active)
        return kUiAccessNone;
    g_picker.scene = &scene;
    measure_picker_grid(&g_picker);
    return add_widgets(tree, parent, scene.widgets, describe_picker, &g_picker, surface, picker_group);
}

bool ui_access_quick_switcher_summary(const UiQuickSwitcherScene& scene, UiAccessNameBuilder* output) noexcept {
    if (!output || !scene.active || !scene.content)
        return false;
    output->clear();
    switch (scene.content->phase) {
    case UiQuickSwitcherPhase::ready:
        add_count(output, scene.accepted_count, utf8_literal("result"), utf8_literal("results"));
        return true;
    case UiQuickSwitcherPhase::no_matches:
        output->add(utf8_literal("No matches"));
        return true;
    case UiQuickSwitcherPhase::unavailable:
        output->add(utf8_literal("Results unavailable"));
        return true;
    default:
        return false;
    }
}

bool ui_access_search_summary(const UiSearchScene& scene, UiAccessNameBuilder* output) noexcept {
    if (!output || !scene.active || !scene.content || !scene.count_bytes ||
        scene.content->phase == UiSearchPhase::empty)
        return false;
    output->clear().add({scene.count_text, scene.count_bytes});
    return true;
}

} // namespace maicord
