#pragma once
#include "maicord/app/https_file_upload.hpp"
namespace maicord {
enum class AccountUploadOperation : u8 { start, status, cancel, release };
struct AccountUploadRequest {
    Utf8View url;
    ConstSlice<wchar_t> path;
    FileSnapshot source;
    UploadSource content;
    HttpsUploadContentType content_type{};
};
struct AccountUploadResult {
    HttpsFileUploadResult transfer;
    u64 id;
    HttpsFileUploadError submit_error;
    bool released;
    // Exact requested ID still owns a transport; errors do not imply release.
    bool owns_transfer;
};
static_assert(sizeof(AccountUploadRequest) == 96);
static_assert(sizeof(AccountUploadResult) == 48);
}
