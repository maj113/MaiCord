#pragma once
#include "maicord/app/https_file_download.hpp"

namespace maicord {
enum class AccountDownloadOperation : u8 { start, status, cancel, release };
using AccountDownloadRequest = HttpsFileDownloadRequest;
struct AccountDownloadResult {
    HttpsFileDownloadResult transfer;
    u64 id;
    HttpsFileDownloadError submit_error;
    // Failed start: true means no owned transfer remains. False requires
    // status/cancel/release even when submit_error is nonzero.
    bool released;
};
static_assert(sizeof(AccountDownloadResult) == 56);
} // namespace maicord
