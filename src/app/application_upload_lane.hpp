#pragma once
#include "maicord/app/account_upload.hpp"
namespace maicord::app_detail {
enum class ApplicationUploadOwner : u8 { none, composer, forum };
struct ApplicationUploadLease {
    u64 id, scope, profile, self;
    const void* instance;
    ApplicationUploadOwner owner;
};
bool application_upload_lane_available() noexcept;
bool application_upload_lane_acquire(ApplicationUploadOwner, ApplicationUploadLease*) noexcept;
AccountUploadResult application_upload_lane_operation(const ApplicationUploadLease&, AccountUploadOperation,
    const AccountUploadRequest* = nullptr) noexcept;
bool application_upload_lane_release(ApplicationUploadLease*) noexcept;
void application_upload_lane_cancel(const ApplicationUploadLease&) noexcept;
} // namespace maicord::app_detail
