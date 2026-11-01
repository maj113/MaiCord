#pragma once

namespace maicord::app_detail {
struct AccountUiRuntime;

bool account_permission_storage_initialize(AccountUiRuntime& runtime) noexcept;
// Call only after model consumers have detached; the enclosing owner is next
// released. No model operation may borrow its banks after this boundary.
void account_permission_storage_release(AccountUiRuntime& runtime) noexcept;
}
