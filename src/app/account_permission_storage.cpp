#include "account_permission_storage.hpp"
#include "account_ui_runtime.hpp"

namespace maicord::app_detail {
namespace {
bool reserve_permissions(void* context, u32 roles, u32 overwrites) noexcept {
    auto& runtime = *static_cast<AccountUiRuntime*>(context);
    return (!roles || stable_storage_reserve(&runtime.role_storage, roles)) &&
        (!overwrites || stable_storage_reserve(&runtime.overwrite_storage, overwrites));
}
}

bool account_permission_storage_initialize(AccountUiRuntime& runtime) noexcept {
    if (runtime.role_storage.arena.base || runtime.overwrite_storage.arena.base)
        return false;
    if (!stable_storage_initialize(&runtime.role_storage, kChatStateRoleCapacity,
            0, sizeof(RoleRecord), alignof(RoleRecord), true) ||
        !stable_storage_initialize(&runtime.overwrite_storage, kChatStateOverwriteCapacity,
            0, sizeof(PermissionOverwriteRecord), alignof(PermissionOverwriteRecord), true) ||
        !chat_state_store_bind_permissions_reserved(&runtime.chat_state,
            {stable_storage_data<RoleRecord>(runtime.role_storage), kChatStateRoleCapacity},
            {stable_storage_data<PermissionOverwriteRecord>(runtime.overwrite_storage),
                kChatStateOverwriteCapacity}, reserve_permissions, &runtime)) {
        account_permission_storage_release(runtime);
        return false;
    }
    return true;
}

void account_permission_storage_release(AccountUiRuntime& runtime) noexcept {
    stable_storage_release(&runtime.role_storage);
    stable_storage_release(&runtime.overwrite_storage);
}
}
