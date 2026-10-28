#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/app/profile_image.hpp"
#include "maicord/protocol/profile_edit.hpp"

namespace maicord {

// The profile editor's account-side work: its REST requests on the DETAILS
// lane (saves take the large-body path when they carry image data URIs) and
// the local preparation of a picked avatar or banner.
RestClientSubmitResult account_profile_edit_submit(RestClient* rest, const ProfileEditRequest* request,
                                                   u64 request_id) noexcept;

// A picked file: probe it (no crop) or crop and encode it into a data URI.
// The caller owns data_uri; a crop that does not fit reports the bytes it
// needs in data_uri_bytes and fails.
struct ProfileImageRequest {
    ConstSlice<u8> source;
    // Zero width: probe only.
    ImageCropOutput crop;
    ProfileImageInfo* info;
    Slice<u8> data_uri;
    u32 data_uri_bytes;
    u32 reserved;
};
bool account_profile_image(ProfileImageRequest* request) noexcept;
// The caller's data URI bank: the URI rides in one large PATCH body with a
// few hundred bytes of JSON; the bank is demand-zero, so only written pages
// cost memory.
inline constexpr u32 kAccountProfileImageUriCapacity = kRestClientMaximumLargeRequestBodyBytes - 8192;

} // namespace maicord
