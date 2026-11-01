#pragma once

#include "maicord/ui/shell/navigation_view.hpp"

namespace maicord {

// The open server folders (folder header row ids) and the rules "Server Folders"
// adds to opening them. The host owns the array; the observer hears each folder
// that opens or closes, in order, so it can animate the rail.
struct UiFolderOpenSet {
    u64* ids;
    u32 count;
    u32 capacity;
};
using UiFolderOpenObserver = void (*)(void* context, u64 folder_row_id, bool open) noexcept;

// Opens or closes one folder. Opening first closes every other folder when
// close_others is set. False, with nothing changed, when a full set cannot take
// another folder.
bool ui_folder_open_set(UiFolderOpenSet* set, u64 folder_row_id, bool open, bool close_others,
                        UiFolderOpenObserver observer, void* context) noexcept;
// Closes every open folder.
void ui_folder_open_close_all(UiFolderOpenSet* set, UiFolderOpenObserver observer, void* context) noexcept;
// "Open the folder of the selected server": when the selected server's row sits in a
// closed folder, opens that folder. True when it opened one.
bool ui_folder_open_for_selection(UiFolderOpenSet* set, const GuildViewRow* selected, bool close_others,
                                  UiFolderOpenObserver observer, void* context) noexcept;

} // namespace maicord
