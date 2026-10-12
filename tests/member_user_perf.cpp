#include "maicord/model/user_store.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
void test_member_user_perf(void (*check)(bool, const char*) noexcept) noexcept {
    UserStore users{};
    UserRecord records[2]{};
    u32 order[2]{};
    u8 names[16]{};
    const bool initialized = user_store_initialize(&users, {records, 2}, {order, 2}, {names, 16}) &&
        user_store_upsert(&users, 1, utf8_literal("One"), utf8_literal("1")).ok() &&
        user_store_upsert(&users, 2, utf8_literal("Two"), utf8_literal("2")).ok() &&
        user_store_pin(&users, 1) && user_store_pin(&users, 2);
    check(initialized, "member user: full pinned record and name banks initialize");
    if (!initialized) return;
    const auto* first = user_store_find(users, 1);
    const u32 revision = users.revision, mutations = users.mutations, clock = users.clock;
    const u32 record_revision = first->revision, name_offset = first->name_offset;
    u8 original_names[16];
    copy_bytes(original_names, names, sizeof(names));
    const auto explicit_names = user_store_upsert(&users, 1, utf8_literal("One"), utf8_literal("1"));
    const auto aliased_names = user_store_upsert(&users, 1,
        user_store_name(users, *first), user_store_username(users, *first));
    const auto omitted_username = user_store_upsert(&users, 1, user_store_name(users, *first));
    check(explicit_names.ok() && !explicit_names.name_changed && !explicit_names.inserted && !explicit_names.evicted &&
        aliased_names.ok() && !aliased_names.name_changed && omitted_username.ok() && !omitted_username.name_changed &&
        users.clock == clock + 3 && first->last_seen == users.clock && first->revision == record_revision &&
        first->name_offset == name_offset && first->pinned && users.revision == revision && users.mutations == mutations &&
        users.name_used == 8 && !users.name_wasted && !users.evictions && !users.compactions &&
        bytes_equal(original_names, names, sizeof(names)),
        "member user: explicit, aliased and omitted unchanged names touch only LRU under full pinned pressure");
    const auto* second = user_store_find(users, 2);
    const auto changed_alias = user_store_upsert(&users, 1,
        user_store_name(users, *second), user_store_username(users, *first));
    first = user_store_find(users, 1);
    second = user_store_find(users, 2);
    check(changed_alias.ok() && changed_alias.name_changed && !changed_alias.evicted && users.compactions == 1 &&
        first && second && first->pinned && second->pinned &&
        utf8_equal(user_store_name(users, *first), utf8_literal("Two")) &&
        utf8_equal(user_store_username(users, *first), utf8_literal("1")) &&
        utf8_equal(user_store_name(users, *second), utf8_literal("Two")) &&
        utf8_equal(user_store_username(users, *second), utf8_literal("2")),
        "member user: changed bank aliases remain staged across compaction and retain pinned neighbors");
    const auto changed_omitted = user_store_upsert(&users, 1, utf8_literal("New"));
    first = user_store_find(users, 1);
    check(changed_omitted.ok() && changed_omitted.name_changed && users.compactions == 2 &&
        first && utf8_equal(user_store_name(users, *first), utf8_literal("New")) &&
        utf8_equal(user_store_username(users, *first), utf8_literal("1")) && !users.evictions,
        "member user: changed display preserves omitted borrowed username across compaction");
}
} // namespace maicord
