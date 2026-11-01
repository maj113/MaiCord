#include "maicord/ui/guild/folder_open.hpp"

namespace maicord {
namespace {
struct Changes {
    u64 ids[16];
    bool open[16];
    u32 count;
};
void record(void* context, u64 folder_row_id, bool open) noexcept {
    auto& changes = *static_cast<Changes*>(context);
    if (changes.count < 16) {
        changes.ids[changes.count] = folder_row_id;
        changes.open[changes.count++] = open;
    }
}
bool holds(const UiFolderOpenSet& set, u64 id) noexcept {
    for (u32 i = 0; i < set.count; ++i)
        if (set.ids[i] == id)
            return true;
    return false;
}
} // namespace

// "Server Folders": the accordion, Home closing every folder, and the selection opening its folder.
void test_folder_open(void (*check)(bool, const char*) noexcept) noexcept {
    u64 ids[3]{};
    UiFolderOpenSet set{ids, 0, 3};
    Changes changes{};
    check(ui_folder_open_set(&set, 11, true, false, record, &changes) &&
              ui_folder_open_set(&set, 12, true, false, record, &changes) &&
              ui_folder_open_set(&set, 13, true, false, record, &changes) && set.count == 3 && changes.count == 3 &&
              changes.open[0] && changes.open[1] && changes.open[2],
          "folders: without the accordion every folder stays open, each change is announced once");
    check(!ui_folder_open_set(&set, 14, true, false, record, &changes) && set.count == 3 && changes.count == 3 &&
              !holds(set, 14),
          "folders: a full set refuses another folder and changes nothing");
    check(ui_folder_open_set(&set, 12, true, false, record, &changes) &&
              ui_folder_open_set(&set, 99, false, false, record, &changes) && set.count == 3 && changes.count == 3,
          "folders: opening an open folder or closing a closed one is silent");

    changes.count = 0;
    check(ui_folder_open_set(&set, 14, true, true, record, &changes) && set.count == 1 && set.ids[0] == 14 &&
              changes.count == 4 && !changes.open[0] && !changes.open[1] && !changes.open[2] && changes.open[3] &&
              changes.ids[3] == 14,
          "folders: with the accordion opening one folder first closes every other, then opens it");
    changes.count = 0;
    check(ui_folder_open_set(&set, 14, false, true, record, &changes) && set.count == 0 && changes.count == 1 &&
              !changes.open[0] && changes.ids[0] == 14,
          "folders: closing a folder announces only that folder");

    check(ui_folder_open_set(&set, 21, true, false, record, &changes) &&
              ui_folder_open_set(&set, 22, true, false, record, &changes) && set.count == 2,
          "folders: two folders open");
    changes.count = 0;
    ui_folder_open_close_all(&set, record, &changes);
    check(set.count == 0 && changes.count == 2 && !changes.open[0] && !changes.open[1] &&
              ((changes.ids[0] == 21 && changes.ids[1] == 22) || (changes.ids[0] == 22 && changes.ids[1] == 21)),
          "folders: Home closes every open folder and announces each");
    changes.count = 0;
    ui_folder_open_close_all(&set, record, &changes);
    check(changes.count == 0, "folders: closing all with none open is silent");

    // The selection opens its folder: only a member row of a closed folder qualifies.
    GuildViewRow row{};
    row.id = 5;
    row.folder_id = 30;
    row.folder_role = GuildFolderRole::member;
    check(ui_folder_open_for_selection(&set, &row, false, record, &changes) && holds(set, 30),
          "folders: the selected server's closed folder opens");
    changes.count = 0;
    row.folder_expanded = true;
    check(!ui_folder_open_for_selection(&set, &row, false, record, &changes) && changes.count == 0,
          "folders: an already open folder is left alone");
    row.folder_expanded = false;
    row.folder_role = GuildFolderRole::none;
    check(!ui_folder_open_for_selection(&set, &row, false, record, &changes) &&
              !ui_folder_open_for_selection(&set, nullptr, false, record, &changes) && changes.count == 0,
          "folders: a server outside a folder, or no selection, opens nothing");
    row.folder_role = GuildFolderRole::member;
    row.folder_id = 31;
    check(ui_folder_open_for_selection(&set, &row, true, record, &changes) && set.count == 1 && holds(set, 31) &&
              !holds(set, 30),
          "folders: following the selection honors the accordion");
}
} // namespace maicord
