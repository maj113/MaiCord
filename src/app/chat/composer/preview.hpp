#pragma once
#include "maicord/app/media/account_media.hpp"
#include "maicord/core/ascii.hpp"

namespace maicord::app_detail {
// The extension after a file name's last dot, lowercase and packed into a u32 (its first byte lowest), when it
// has one to four bytes and no NUL, else 0. The composer and the forum form tell files apart by such extensions
// only, so the scan stops at the fifth byte from the end.
inline u32 filename_extension_code(const Utf8View& name) noexcept {
    u32 code = 0;
    for (u32 bytes = 0; bytes < 5 && bytes < name.bytes; ++bytes) {
        const char value = name.data[name.bytes - 1 - bytes];
        if (value == '.')
            return code;
        if (!value)
            return 0;
        code = code << 8 | static_cast<u8>(ascii_lower(value));
    }
    return 0;
}
// The code filename_extension_code gives for a lowercase extension of one to four bytes.
consteval u32 extension_code(const char* extension) noexcept {
    u32 code = 0;
    for (u32 i = 0; extension[i]; ++i)
        code |= u32{static_cast<u8>(extension[i])} << (8 * i);
    return code;
}
// Optional thumbnails for the composer upload strip. Images decode on the
// account media lanes (never on the UI thread) into the preview atlas; MP4/MOV
// files get a GPU-decoded first-frame poster reduced to the same extent. A
// missing, unsupported or failed preview leaves the file-type card.
struct ApplicationComposerPreview {
    MediaAssetKey key;
    bool ready, failed;
};
ApplicationComposerPreview application_composer_preview_view(u64 draft_id) noexcept;
// Format whose codec MaiCord owns for this filename, or unknown.
MediaAttachmentFormat application_composer_preview_format(const Utf8View& filename) noexcept;
// Admits current active-channel drafts and retires everything else.
bool application_composer_preview_request(u64 now_ms, u64 unix_now_seconds) noexcept;
bool application_composer_preview_key_current(MediaAssetKey) noexcept;
bool application_composer_preview_frame_current(const AccountMediaFrame&) noexcept;
void application_composer_preview_frame_uploaded(const AccountMediaFrame&) noexcept;
void application_composer_preview_rejected(MediaAssetKey) noexcept;
// Retires one draft's preview (0: all) and makes its atlas entry evictable.
void application_composer_preview_retire(u64 draft_id = 0) noexcept;
void application_composer_preview_release() noexcept;
u64 application_composer_preview_next_wake() noexcept;
u32 application_composer_preview_revision() noexcept;

// Local-preview routing shared by the forum form and the composer strip.
bool application_local_preview_key_current(const MediaAssetKey&) noexcept;
bool application_local_preview_frame_current(const AccountMediaFrame&) noexcept;
void application_local_preview_frame_uploaded(const AccountMediaFrame&) noexcept;
void application_local_preview_rejected(MediaAssetKey) noexcept;
} // namespace maicord::app_detail
