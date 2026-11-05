// platform/desktop/notification_identity.hpp on Linux: a program's notification identity is its desktop file (the
// desktop-entry hint of each notification names it, packaging/linux installs it), so there is nothing to enroll or
// repair and nothing in a registry to protect from another owner: a valid configuration is always "ours".
#include "maicord/platform/desktop/notification_identity.hpp"

namespace maicord {

bool notification_identity_valid(const NotificationIdentityConfig& config) noexcept {
    return config.app_id.count && config.executable_path.count && config.display_name.count;
}

NotificationIdentityResult notification_identity_check(const NotificationIdentityConfig& config) noexcept {
    if (!notification_identity_valid(config))
        return {NotificationIdentityError::invalid_input, 0, false};
    return {};
}

NotificationIdentityResult notification_identity_set_process(ConstSlice<wchar_t> app_id) noexcept {
    if (!app_id.count)
        return {NotificationIdentityError::invalid_input, 0, false};
    return {};
}

NotificationIdentityResult notification_identity_create(const NotificationIdentityConfig& config) noexcept {
    return notification_identity_check(config);
}

} // namespace maicord
