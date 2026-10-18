#include "maicord/model/reaction_preferences.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::reaction_preference_storage_test {
u32 failures;
void check(bool success, const char* message) noexcept {
    test_check(&failures, success, message);
}
void measurement(const char* name, usize value) noexcept {
    test_write_metric(name, value);
}
bool vm(const void* address, DWORD expected) noexcept {
    MEMORY_BASIC_INFORMATION information{};
    return VirtualQuery(address, &information, sizeof(information)) == sizeof(information) && information.State == expected;
}
void ownership() noexcept {
    ReactionPreferenceStore store{};
    check(reaction_preferences_initialize(&store) && !store.collapsed_guilds.arena.base &&
        !store.collapsed_guilds.arena.committed, "reaction storage: initialized account retains no guild bank reservation or commitment");
    check(reaction_preferences_toggle_favorite_emoji(&store, reaction_emoji_unicode(4)).ok() &&
        reaction_preferences_track_emoji(&store, ReactionPreferenceKind::reaction, reaction_emoji_unicode(4), 1000).ok() &&
        reaction_preferences_toggle_section(&store, 1).ok() && !store.collapsed_guilds.arena.base,
        "reaction storage: favorites, scoring and Unicode collapse do not allocate a guild bank");
    const auto first = reaction_preferences_toggle_section(&store, 16, 1007);
    auto* base = store.collapsed_guilds.arena.base;
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    check(first.ok() && first.changed && base && vm(base, MEM_COMMIT) &&
        store.collapsed_guilds.arena.committed == system.dwPageSize && store.collapsed_guilds.arena.commit_count == 1 &&
        reaction_preferences_section_collapsed(store, 16, 1007),
        "reaction storage: first guild collapse commits exactly one native page before publishing identity");
    measurement("MEASURE reaction_collapse_initial_committed_bytes=", 0);
    measurement("MEASURE reaction_collapse_first_committed_bytes=", store.collapsed_guilds.arena.committed);
    check(!reaction_preferences_initialize(&store) && store.collapsed_guilds.arena.base == base &&
        reaction_preferences_section_collapsed(store, 16, 1007),
        "reaction storage: duplicate reset cannot discard a live owned bank");
    for (u32 i = 0; i < kReactionPreferenceCollapsedGuildCapacity - 1; ++i)
        check(reaction_preferences_toggle_section(&store, 16, 2000 + i).ok(),
            "reaction storage: all 256 distinct guild identities remain supported");
    const u32 revision = store.revision;
    check(store.collapsed_guild_count == 256 && store.collapsed_guilds.capacity == 256 &&
        store.collapsed_guilds.arena.base == base && store.collapsed_guilds.arena.committed == system.dwPageSize &&
        store.collapsed_guilds.arena.commit_count == 1 &&
        reaction_preferences_toggle_section(&store, 16, 9000).error == ReactionPreferenceError::capacity &&
        store.collapsed_guild_count == 256 && store.revision == revision,
        "reaction storage: full-capacity overflow preserves state and stable bank with no extra page");
    check(reaction_preferences_toggle_section(&store, 16, 1007).ok() &&
        !reaction_preferences_section_collapsed(store, 16, 1007) && reaction_preferences_section_collapsed(store, 16, 2254),
        "reaction storage: removing first guild retains the final shifted identity");
    for (u32 i = 0; i < kReactionPreferenceCollapsedGuildCapacity - 1; ++i)
        check(reaction_preferences_toggle_section(&store, 16, 2000 + i).ok(), "reaction storage: every retained guild can expand");
    check(!store.collapsed_guild_count && !store.collapsed_guilds.arena.base && vm(base, MEM_FREE) &&
        reaction_preferences_is_favorite_emoji(store, reaction_emoji_unicode(4)) && reaction_preferences_section_collapsed(store, 1),
        "reaction storage: last expansion releases the bank without losing favorite or Unicode section state");
    check(reaction_preferences_toggle_section(&store, 16, 1007).ok(), "reaction storage: collapse can allocate again after complete expansion");
    base = store.collapsed_guilds.arena.base;
    reaction_preferences_release(&store);
    check(!store.next_sequence && !store.collapsed_guild_count && !store.collapsed_guilds.arena.base && vm(base, MEM_FREE),
        "reaction storage: explicit account release frees the bank and clears preference ownership");
    reaction_preferences_release(&store);
    check(reaction_preferences_initialize(&store), "reaction storage: released account owner can be initialized afresh");
    reaction_preferences_release(&store);
}
void commit_failure() noexcept {
    ReactionPreferenceStore store{};
    check(reaction_preferences_initialize(&store) && stable_storage_initialize(&store.collapsed_guilds,
        256, 0, sizeof(u64), alignof(u64), true), "reaction storage: reserve-only bank prepares actual native commit rejection fixture");
    auto& bank = store.collapsed_guilds;
    auto* original = bank.arena.base;
    if (!original) return;
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READONLY, 0,
        static_cast<DWORD>(bank.arena.reserved), nullptr);
    auto* view = mapping ? static_cast<u8*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, bank.arena.reserved)) : nullptr;
    check(view != nullptr, "reaction storage: read-only mapped page is available for real VirtualAlloc commit failure");
    if (view) {
        bank.arena.base = view;
        const u32 revision = store.revision;
        const auto result = reaction_preferences_toggle_section(&store, 16, 42);
        check(result.error == ReactionPreferenceError::capacity && !result.changed && store.revision == revision &&
            !store.collapsed_guild_count && !bank.capacity && !bank.arena.offset && !bank.arena.committed &&
            !reaction_preferences_section_collapsed(store, 16, 42) && bank.arena.base == view,
            "reaction storage: actual commit rejection preserves revision, identity set and retained owner for retry");
        bank.arena.base = original;
        check(UnmapViewOfFile(view) != FALSE, "reaction storage: fault view releases separately from production reservation");
    }
    if (mapping) check(CloseHandle(mapping) != FALSE, "reaction storage: fault mapping handle closes");
    check(reaction_preferences_toggle_section(&store, 16, 42).ok() && reaction_preferences_section_collapsed(store, 16, 42),
        "reaction storage: restoring the original reservation allows retry of the same guild identity");
    reaction_preferences_release(&store);
    check(vm(original, MEM_FREE), "reaction storage: retry owner releases its original VM reservation");
}
} // namespace maicord::reaction_preference_storage_test

extern "C" __declspec(noreturn) void WINAPI maicord_reaction_preference_storage_entry() noexcept {
    using namespace maicord::reaction_preference_storage_test;
    measurement("MEASURE reaction_preference_owner_bytes=", sizeof(maicord::ReactionPreferenceStore));
    DWORD before = 0, after = 0;
    const bool counted = GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE;
    ownership(); commit_failure();
    check(counted && GetProcessHandleCount(GetCurrentProcess(), &after) && before == after,
        "reaction storage: native fault views and mappings restore process handle baseline");
    ExitProcess(failures ? 1u : 0u);
}
