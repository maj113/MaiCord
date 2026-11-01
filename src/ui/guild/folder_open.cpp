#include "maicord/ui/guild/folder_open.hpp"

namespace maicord {
namespace {
u32 find(const UiFolderOpenSet& set, u64 id) noexcept {
    u32 at = 0;
    while (at < set.count && set.ids[at] != id)
        ++at;
    return at;
}
void tell(UiFolderOpenObserver observer, void* context, u64 id, bool open) noexcept {
    if (observer)
        observer(context, id, open);
}
} // namespace

bool ui_folder_open_set(UiFolderOpenSet* set, u64 folder_row_id, bool open, bool close_others,
                        UiFolderOpenObserver observer, void* context) noexcept {
    const u32 at = find(*set, folder_row_id);
    const bool is_open = at < set->count;
    if (is_open == open)
        return true;
    if (!open) {
        set->ids[at] = set->ids[--set->count];
        tell(observer, context, folder_row_id, false);
        return true;
    }
    if (close_others)
        while (set->count)
            tell(observer, context, set->ids[--set->count], false);
    if (set->count >= set->capacity)
        return false;
    set->ids[set->count++] = folder_row_id;
    tell(observer, context, folder_row_id, true);
    return true;
}

void ui_folder_open_close_all(UiFolderOpenSet* set, UiFolderOpenObserver observer, void* context) noexcept {
    while (set->count)
        tell(observer, context, set->ids[--set->count], false);
}

bool ui_folder_open_for_selection(UiFolderOpenSet* set, const GuildViewRow* selected, bool close_others,
                                  UiFolderOpenObserver observer, void* context) noexcept {
    return selected && selected->folder_role == GuildFolderRole::member && selected->folder_id &&
           !selected->folder_expanded &&
           ui_folder_open_set(set, selected->folder_id, true, close_others, observer, context);
}

} // namespace maicord
