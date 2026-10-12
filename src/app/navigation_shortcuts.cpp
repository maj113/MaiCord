#include "maicord/app/navigation_shortcuts.hpp"
#include "maicord/app/shortcut_registry.hpp"
#include "maicord/ui/list_navigation.hpp"

namespace maicord {
namespace {

struct Candidates {
    const NavigationView* view;
    NavigationMode mode;
};
template <typename Row> u32 selected_row(const Row* rows, u32 count, u64 id) noexcept {
    for (u32 i = 0; i < count; ++i)
        if (rows[i].id == id)
            return i;
    return kUiListNoSelection;
}
bool eligible(void* opaque, u32 index) noexcept {
    const auto& candidates = *static_cast<Candidates*>(opaque);
    if (candidates.mode == NavigationMode::guild) {
        if (index == 0)
            return true;
        // Shortcuts walk every guild in display order, including collapsed folders.
        const auto& row = candidates.view->guild_rows[index - 1];
        return guild_view_row_is_guild(row) && !row.unavailable;
    }
    const auto& row = candidates.view->channel_rows[index];
    return row.id && row.selectable && row.type != 4 &&
           (candidates.mode != NavigationMode::unread || row.unread) &&
           (candidates.mode != NavigationMode::mention || row.mention_count != 0);
}
bool focus_eligible(void* opaque, u32 index) noexcept {
    const auto& candidates = *static_cast<Candidates*>(opaque);
    if (candidates.mode == NavigationMode::guild) {
        if (index == 0)
            return true;
        // Rail focus visits what the rail shows: folder headers and expanded members.
        const auto& row = candidates.view->guild_rows[index - 1];
        return row.id && !row.unavailable && !guild_view_row_hidden(row);
    }
    const auto& row = candidates.view->channel_rows[index];
    // Category controls participate in focus/context access, while ordinary
    // conversation shortcuts continue using the activation predicate above.
    return row.id && (row.selectable || row.type == 4);
}

} // namespace

NavigationShortcut navigation_shortcut(u32 key, bool control, bool alt, bool shift) noexcept {
    // The registry owns the chords; global scope applies in every focus.
    const ShortcutChord chord{key <= 0xff ? static_cast<u8>(key) : u8{0},
                              static_cast<u8>((control ? shortcut_ctrl : 0) | (alt ? shortcut_alt : 0) |
                                              (shift ? shortcut_shift : 0))};
    switch (shortcut_first({ShortcutFocus::text, false}, chord)) {
    case ShortcutAction::server_previous: return {NavigationMode::guild, true};
    case ShortcutAction::server_next: return {NavigationMode::guild, false};
    case ShortcutAction::channel_previous: return {NavigationMode::channel, true};
    case ShortcutAction::channel_next: return {NavigationMode::channel, false};
    case ShortcutAction::unread_previous: return {NavigationMode::unread, true};
    case ShortcutAction::unread_next: return {NavigationMode::unread, false};
    case ShortcutAction::mention_previous: return {NavigationMode::mention, true};
    case ShortcutAction::mention_next: return {NavigationMode::mention, false};
    default: return {};
    }
}

NavigationTarget navigation_target(const NavigationView& view,
                                   NavigationShortcut shortcut) noexcept {
    if (shortcut.mode == NavigationMode::none || shortcut.mode > NavigationMode::mention)
        return {};
    const bool guild = shortcut.mode == NavigationMode::guild;
    const u32 row_count = guild ? view.guild_count : view.channel_count;
    if (row_count > (guild ? view.guild_capacity : view.channel_capacity) || row_count == ~0u ||
        (row_count && (guild ? !view.guild_rows : !view.channel_rows)))
        return {};
    const u32 count = row_count + (guild ? 1u : 0u);
    const u64 selected = guild ? view.selected_guild_id : view.selected_channel_id;
    u32 current = guild && selected == 0 ? 0 : kUiListNoSelection;
    const u32 found = guild ? selected_row(view.guild_rows, row_count, selected)
                            : selected_row(view.channel_rows, row_count, selected);
    if (found != kUiListNoSelection)
        current = found + (guild ? 1u : 0u);
    Candidates candidates{&view, shortcut.mode};
    const u32 index = ui_list_next(count, current, shortcut.reverse, true, eligible, &candidates);
    if (index == kUiListNoSelection)
        return {};
    if (guild && index == 0)
        return {0, 0, false, true};
    const u32 source = index - (guild ? 1u : 0u);
    return {guild ? view.guild_rows[source].id : view.channel_rows[source].id, source, guild, false};
}

NavigationTarget navigation_focus_target(const NavigationView& view, bool guild, u64 focused_id,
                                         UiListMove move) noexcept {
    const u32 rows = guild ? view.guild_count : view.channel_count;
    if (rows > (guild ? view.guild_capacity : view.channel_capacity) || rows == ~0u ||
        (rows && (guild ? !view.guild_rows : !view.channel_rows)))
        return {};
    u32 current = guild && !focused_id ? 0 : kUiListNoSelection;
    const u32 found = guild ? selected_row(view.guild_rows, rows, focused_id)
                            : selected_row(view.channel_rows, rows, focused_id);
    if (found != kUiListNoSelection)
        current = found + (guild ? 1u : 0u);
    Candidates candidates{&view, guild ? NavigationMode::guild : NavigationMode::channel};
    const u32 index = ui_list_move(rows + (guild ? 1u : 0u), current, move, focus_eligible, &candidates);
    if (index == kUiListNoSelection)
        return {};
    if (guild && !index)
        return {0, 0, false, true};
    const u32 source = index - (guild ? 1u : 0u);
    return {guild ? view.guild_rows[source].id : view.channel_rows[source].id, source, guild, false};
}

} // namespace maicord
