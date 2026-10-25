#include "application_upload_lane.hpp"
#include "maicord/core/memory.hpp"
#include "application_state.hpp"
namespace maicord::app_detail {
namespace {
ApplicationUploadLease upload_lane{};
u64 upload_serial{};
bool upload_started{};
bool same(const ApplicationUploadLease& lease) noexcept {
    return lease.id && lease.id == upload_lane.id && lease.instance == upload_lane.instance &&
        lease.owner == upload_lane.owner && lease.scope == upload_lane.scope &&
        lease.profile == upload_lane.profile && lease.self == upload_lane.self;
}
bool actor(const ApplicationUploadLease& lease) noexcept {
    return g_app.account.instance == lease.instance && g_app.account_ui &&
        lease.scope == g_app.plugin_account_scope_serial && lease.profile == g_app.account.status.profile_id &&
        lease.self == g_app.account_ui->chat_state.self_user_id;
}
}
bool application_upload_lane_available() noexcept { return !upload_lane.id; }
bool application_upload_lane_acquire(ApplicationUploadOwner owner, ApplicationUploadLease* output) noexcept {
    if (!output || owner == ApplicationUploadOwner::none || upload_lane.id || !g_app.account.instance ||
        !g_app.account_ui || !g_app.account.api.upload || !g_app.account.status.running || g_app.account.stop_failed)
        return false;
    if (!++upload_serial) ++upload_serial;
    upload_lane = {upload_serial, g_app.plugin_account_scope_serial, g_app.account.status.profile_id,
        g_app.account_ui->chat_state.self_user_id, g_app.account.instance, owner};
    *output = upload_lane;
    upload_started = false;
    return true;
}
AccountUploadResult application_upload_lane_operation(const ApplicationUploadLease& lease,
    AccountUploadOperation operation, const AccountUploadRequest* request) noexcept {
    if (!same(lease) || !g_app.account.instance || g_app.account.instance != lease.instance ||
        !g_app.account.api.upload || (operation == AccountUploadOperation::start && !actor(lease)))
        return {{}, lease.id, HttpsFileUploadError::invalid_input, false};
    const auto result = g_app.account.api.upload(g_app.account.instance, operation, lease.id, request);
    if (operation == AccountUploadOperation::start && same(lease)) upload_started = result.owns_transfer;
    return result;
}
void application_upload_lane_cancel(const ApplicationUploadLease& lease) noexcept {
    if (same(lease)) application_upload_lane_operation(lease, AccountUploadOperation::cancel);
}
bool application_upload_lane_release(ApplicationUploadLease* lease) noexcept {
    if (!lease || !lease->id) return true;
    if (!same(*lease)) return false;
    if (!upload_started || !g_app.account.instance || g_app.account.instance != lease->instance) {
        reset_value(&upload_lane); reset_value(lease); upload_started = false; return true;
    }
    const auto result = application_upload_lane_operation(*lease, AccountUploadOperation::release);
    if (!result.released) return false;
    reset_value(&upload_lane); reset_value(lease); upload_started = false; return true;
}
} // namespace maicord::app_detail
