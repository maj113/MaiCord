#include "account_ui_runtime.hpp"
#include "account_permission_storage.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/model/permission_bits.hpp"

namespace maicord::account_permission_storage_test {
using namespace app_detail;
u32 failures = 0;
void check(bool valid, const char* name) noexcept {
    test_check(&failures, valid, name);
}
bool vm(const void* address, DWORD expected) noexcept {
    Win32MemoryInformation info{};
    return address && VirtualQuery(address, info.native(), sizeof(info)) == sizeof(info) && info.State == expected;
}
usize pages(usize bytes, usize page) noexcept { return (bytes + page - 1) & ~(page - 1); }
bool prefix(const StableStorage& storage, u32 capacity, usize page) noexcept {
    const usize bytes = static_cast<usize>(capacity) * storage.element_bytes;
    const usize committed = pages(bytes, page);
    if (storage.capacity != capacity || storage.arena.offset != bytes || storage.arena.committed != committed)
        return false;
    if (committed && (!vm(storage.arena.base, MEM_COMMIT) ||
        !vm(storage.arena.base + committed - 1, MEM_COMMIT))) return false;
    return committed == storage.arena.reserved || vm(storage.arena.base + committed, MEM_RESERVE);
}
bool zero(const void* address, usize bytes) noexcept {
    const auto* data = static_cast<const u8*>(address);
    for (usize i = 0; i < bytes; ++i) if (data[i]) return false;
    return true;
}
bool initialize_model(AccountUiRuntime& owner) noexcept {
    return chat_state_store_initialize(&owner.chat_state,
        {owner.guild_record_storage, kChatStateGuildCapacity}, {owner.guild_order_storage, kChatStateGuildCapacity},
        {owner.channel_record_storage, kChatStateChannelCapacity}, {owner.channel_order_storage, kChatStateChannelCapacity},
        {owner.chat_state_name_storage, kChatStateNameBytes});
}
bool live_unchanged(const AccountUiRuntime& owner, const ChatStateStore& before,
    const RoleRecord& role, const PermissionOverwriteRecord& overwrite) noexcept {
    const auto& after = owner.chat_state;
    const auto& current = after.roles[0]; const auto& current_overwrite = after.overwrites[0];
    return after.role_count == before.role_count && after.overwrite_count == before.overwrite_count &&
        after.role_backed_count == before.role_backed_count && after.overwrite_backed_count == before.overwrite_backed_count &&
        after.name_used == before.name_used && after.name_wasted == before.name_wasted &&
        after.permission_mutations == before.permission_mutations && after.compactions == before.compactions &&
        current.id == role.id && current.guild_id == role.guild_id && current.permissions == role.permissions &&
        current.name_offset == role.name_offset && current.name_bytes == role.name_bytes &&
        current.self_assigned == role.self_assigned && current_overwrite.id == overwrite.id &&
        current_overwrite.allow == overwrite.allow && current_overwrite.deny == overwrite.deny &&
        current_overwrite.type == overwrite.type;
}
void run() noexcept {
    Win32SystemInfo system{}; GetSystemInfo(system.native());
    const usize page = system.dwPageSize;
    check(page && !(page & (page - 1)), "permission owner OS page alignment");
    if (!page || (page & (page - 1))) return;
    auto* owner = static_cast<AccountUiRuntime*>(VirtualAlloc(nullptr, sizeof(AccountUiRuntime),
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(owner != nullptr, "permission production owner allocation");
    if (!owner) return;
    Win32MemoryInformation owner_vm{};
    check(VirtualQuery(owner, owner_vm.native(), sizeof(owner_vm)) == sizeof(owner_vm) &&
        owner_vm.State == MEM_COMMIT && owner_vm.AllocationBase == owner &&
        owner_vm.RegionSize == pages(sizeof(AccountUiRuntime), page), "permission actual owner commitment follows compiled size");
    const bool initialized = initialize_model(*owner) && account_permission_storage_initialize(*owner);
    check(initialized, "permission production owner binds both zero-backed reserved banks");
    if (!initialized) { account_permission_storage_release(*owner); VirtualFree(owner, 0, MEM_RELEASE); return; }
    auto& state = owner->chat_state;
    const auto* roles = owner->role_storage.arena.base;
    const auto* overwrites = owner->overwrite_storage.arena.base;
    check(roles && overwrites && roles != overwrites && state.roles == reinterpret_cast<const RoleRecord*>(roles) &&
        state.overwrites == reinterpret_cast<const PermissionOverwriteRecord*>(overwrites) &&
        state.role_capacity == kChatStateRoleCapacity && state.overwrite_capacity == kChatStateOverwriteCapacity &&
        !state.role_backed_count && !state.overwrite_backed_count &&
        prefix(owner->role_storage, 0, page) && prefix(owner->overwrite_storage, 0, page),
        "permission empty production bindings retain maximum caps with actual uncommitted VM");
    check(!account_permission_storage_initialize(*owner) && owner->role_storage.arena.base == roles &&
        owner->overwrite_storage.arena.base == overwrites, "permission repeated initialization preserves installed banks");
    check(chat_state_store_prepare_permissions(&state, 0, 0) &&
        !owner->role_storage.arena.committed && !owner->overwrite_storage.arena.committed,
        "permission zero admission commits no VM");
    const bool seeded = chat_state_store_upsert_guild(&state, {7, utf8_literal("Guild"), GuildMutationKind::create, 70}).ok() &&
        chat_state_store_upsert_channel(&state, {71, 7, 0, utf8_literal("chat"), 0, 0, 0}).ok();
    check(seeded && state.channels[0].occupied && !state.channels[0].permissions_resolved &&
        !state.channels[0].actions && !chat_state_store_channel_visible(state, state.channels[0]) &&
        !owner->role_storage.arena.committed && !owner->overwrite_storage.arena.committed,
        "permission nonnull zero-backed production store remains unresolved and denies guild actions");
    if (!seeded) { account_permission_storage_release(*owner); VirtualFree(owner, 0, MEM_RELEASE); return; }
    const u64 permissions = kPermissionViewChannel | kPermissionSendMessages;
    const bool admitted = chat_state_store_upsert_role(&state, {7, 7, permissions, utf8_literal("everyone")}) &&
        chat_state_store_append_channel_overwrite(&state, 71, {7, permissions, 0, 0}) &&
        chat_state_store_set_self_roles(&state, 7, 70, 700, {});
    check(admitted, "permission first production records admit writable pages");
    if (!admitted) { account_permission_storage_release(*owner); VirtualFree(owner, 0, MEM_RELEASE); return; }
    chat_state_store_recompute_guild_permissions(&state, 7);
    check(state.role_count == 1 && state.overwrite_count == 1 && state.role_backed_count == 1 && state.overwrite_backed_count == 1 &&
        prefix(owner->role_storage, 1, page) && prefix(owner->overwrite_storage, 1, page) &&
        state.channels[0].permissions_resolved && (state.channels[0].actions & channel_action_send_messages),
        "permission tiny production state commits one OS page per unchanged-stride bank and resolves actions");
    const u32 role_commits = owner->role_storage.arena.commit_count;
    const u32 overwrite_commits = owner->overwrite_storage.arena.commit_count;
    check(chat_state_store_upsert_role(&state, {7, 7, permissions, {}}) &&
        chat_state_store_prepare_permissions(&state, 0, 0) &&
        owner->role_storage.arena.commit_count == role_commits &&
        owner->overwrite_storage.arena.commit_count == overwrite_commits &&
        owner->role_storage.capacity == 1 && owner->overwrite_storage.capacity == 1,
        "permission resident updates reuse commitment without capacity growth");
    const auto before = state; const auto before_role = state.roles[0]; const auto before_overwrite = state.overwrites[0];
    owner->role_storage.reserved_capacity = 0;
    check(!chat_state_store_prepare_permissions(&state, 103, 164) &&
        live_unchanged(*owner, before, before_role, before_overwrite) &&
        owner->role_storage.capacity == 1 && owner->overwrite_storage.capacity == 1,
        "permission deterministic first owner-bank admission failure preserves model and storage");
    owner->role_storage.reserved_capacity = kChatStateRoleCapacity;
    owner->overwrite_storage.reserved_capacity = 1;
    check(!chat_state_store_prepare_permissions(&state, 103, 164) &&
        live_unchanged(*owner, before, before_role, before_overwrite) &&
        prefix(owner->role_storage, 103, page) && prefix(owner->overwrite_storage, 1, page),
        "permission second owner-bank failure retains first physical growth with unchanged model admission and live records");
    owner->overwrite_storage.reserved_capacity = kChatStateOverwriteCapacity;
    check(chat_state_store_prepare_permissions(&state, 103, 164) &&
        state.role_backed_count == 103 && state.overwrite_backed_count == 164 &&
        prefix(owner->role_storage, 103, page) && prefix(owner->overwrite_storage, 164, page) &&
        zero(state.roles + 1, 102 * sizeof(RoleRecord)) &&
        zero(state.overwrites + 1, 163 * sizeof(PermissionOverwriteRecord)) &&
        owner->role_storage.arena.base == roles && owner->overwrite_storage.arena.base == overwrites,
        "permission retry admits exact two-page prefixes at stable bases and zeroes only newly writable records");
    const u32 crossing_role_commits = owner->role_storage.arena.commit_count;
    const u32 crossing_overwrite_commits = owner->overwrite_storage.arena.commit_count;
    check(!chat_state_store_prepare_permissions(&state, kChatStateRoleCapacity + 1, 0) &&
        !chat_state_store_prepare_permissions(&state, 0, kChatStateOverwriteCapacity + 1) &&
        owner->role_storage.arena.commit_count == crossing_role_commits &&
        owner->overwrite_storage.arena.commit_count == crossing_overwrite_commits,
        "permission logical cap rejection does not commit storage");
    const bool maximum_admitted = chat_state_store_prepare_permissions(&state, kChatStateRoleCapacity, kChatStateOverwriteCapacity);
    check(maximum_admitted &&
        prefix(owner->role_storage, kChatStateRoleCapacity, page) &&
        prefix(owner->overwrite_storage, kChatStateOverwriteCapacity, page) &&
        owner->overwrite_storage.element_bytes == 25 && owner->overwrite_storage.alignment == 1 &&
        owner->role_storage.element_bytes == 48 && owner->role_storage.alignment == 8,
        "permission maximum admission preserves packed 25-byte overwrite and 40-byte role strides and original caps");
    if (!maximum_admitted) { account_permission_storage_release(*owner); VirtualFree(owner, 0, MEM_RELEASE); return; }
    state = {}; account_permission_storage_release(*owner);
    check(!owner->role_storage.arena.base && !owner->overwrite_storage.arena.base &&
        vm(roles, MEM_FREE) && vm(overwrites, MEM_FREE), "permission detached production owner releases both actual VM reservations");
    account_permission_storage_release(*owner);
    RoleRecord fixed_roles[1]{}; PermissionOverwriteRecord fixed_overwrites[1]{};
    const bool fixed = initialize_model(*owner) && chat_state_store_bind_permissions(&state, {fixed_roles, 1}, {fixed_overwrites, 1});
    check(fixed && !account_permission_storage_initialize(*owner) && state.roles == fixed_roles && state.overwrites == fixed_overwrites &&
        !owner->role_storage.arena.base && !owner->overwrite_storage.arena.base,
        "permission failed production binding releases newly reserved banks and preserves existing fixed model binding");
    VirtualFree(owner, 0, MEM_RELEASE);
}
}

extern "C" __declspec(noreturn) void WINAPI maicord_account_permission_storage_entry() noexcept {
    maicord::account_permission_storage_test::run();
    ExitProcess(maicord::account_permission_storage_test::failures ? 1 : 0);
}
