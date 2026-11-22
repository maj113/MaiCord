#pragma once
#include "maicord/core/memory.hpp"
#include "maicord/model/user/store.hpp"

namespace maicord {

// A user store over `Users` records and two name banks of `NameBytes / 2` bytes each, held here: the small banks the
// eviction and compaction tests need (user_store_initialize_owned rounds a bank up to a page). init sets the state
// user_store_initialize_owned sets, with every record and both banks committed at once; when that function's layout
// changes, change this one with it.
template <u32 Users, u32 NameBytes> struct TestUserBank {
    static_assert(Users != 0 && NameBytes >= 2 && NameBytes % 2 == 0);
    UserRecord records[Users];
    u32 order[Users];
    u8 names[NameBytes];

    // Always true, so that it can stand in a chain of checks.
    bool init(UserStore* store) noexcept {
        reset_value<UserStore>(store);
        store->pin_mark = 1;
        store->records = records;
        store->order = order;
        store->names = names;
        store->capacity = store->committed = Users;
        store->name_bank_bytes = store->name_committed[0] = store->name_committed[1] = NameBytes / 2;
        for (u32 index = 0; index < Users; ++index) {
            reset_value<UserRecord>(&records[index]);
            order[index] = ~0u;
        }
        return true;
    }
};

} // namespace maicord
