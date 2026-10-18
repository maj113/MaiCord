#pragma once
#include "maicord/media/asset.hpp"
#include "maicord/media/encoded_video_limits.hpp"
#include "maicord/platform/file_identity.hpp"
#include "maicord/platform/upload_source.hpp"
#include "maicord/sys/d3d11.hpp"

struct ID3D11Device;

namespace maicord {
enum class AccountLocalPreviewOperation : u8 {
    request,
    retire,
    uploaded,
    presented,
};
// Retire carries only key/owner_scope/owner_generation; all source fields are
// zero. Cancellation never follows a caller-owned path or source pointer.
// Uploaded carries the same exact owner plus lease_serial, with no source.
// Presented takes a null request after an actual successful renderer Present.
// Both operations require zero time arguments. Only pending needs a later
// presented call; ready also covers a disabled optimization or completed lease.
// All pointers are borrowed during admission. The lane retains content or
// copies the path before returning queued; file access belongs to that lane.
struct AccountMediaLocalPreviewRequest {
    UploadSource content;
    FileSnapshot snapshot;
    ConstSlice<wchar_t> path;
    MediaAssetKey key;
    u64 owner_scope;
    u32 owner_generation;
    u32 reserved;
    // Composer MP4/MOV posters decode their first frame on this device. It is
    // borrowed from the host and must outlive account stop; null leaves video
    // tiles on their file icon. Image previews ignore it.
    ID3D11Device* video_device;
    // Uploaded carries an exact worker lease. Request and retire require zero.
    u64 lease_serial;
};
inline constexpr u32 kLocalDraftPreviewPathMaximum = 1023;
// Composer upload-strip previews set this variant bit. It keeps their keys
// apart from forum previews and asks the codec lane for a bounded thumbnail.
inline constexpr u32 kLocalDraftPreviewThumbnailVariant = 0x80000000u;
constexpr bool local_draft_preview_thumbnail(MediaAssetKey key) noexcept {
    return key.kind == MediaAssetKind::local_draft_preview && (key.variant & kLocalDraftPreviewThumbnailVariant);
}
// Thumbnails decode large camera JPEGs at reduced scale (jpeg_thumbnail_*).
inline constexpr u32 kLocalDraftPreviewThumbnailJpegBytes = 48u * 1024u * 1024u;
// Large PNGs stream row by row through a sliding inflate window, so only the
// encoded file is resident (png_thumbnail_*).
inline constexpr u32 kLocalDraftPreviewThumbnailPngBytes = 48u * 1024u * 1024u;
// Large WebPs decode on the CPU at bounded memory (webp_thumbnail_*).
inline constexpr u32 kLocalDraftPreviewThumbnailWebpBytes = 16u * 1024u * 1024u;
constexpr u32 local_draft_preview_encoded_limit(MediaAttachmentFormat format) noexcept {
    return format == MediaAttachmentFormat::png || format == MediaAttachmentFormat::gif
               ? 16u * 1024u * 1024u
           : format == MediaAttachmentFormat::jpeg || format == MediaAttachmentFormat::webp
               ? 2u * 1024u * 1024u
               : 0;
}
// Composer thumbnails: video files are never read whole. The poster lane
// streams the movie header and the first samples into a sparse source.
constexpr u64 local_draft_preview_thumbnail_limit(MediaAttachmentFormat format) noexcept {
    return format == MediaAttachmentFormat::jpeg   ? kLocalDraftPreviewThumbnailJpegBytes
           : format == MediaAttachmentFormat::png  ? kLocalDraftPreviewThumbnailPngBytes
           : format == MediaAttachmentFormat::webp ? kLocalDraftPreviewThumbnailWebpBytes
           : format == MediaAttachmentFormat::mp4  ? kMediaMaximumProgressiveVideoBytes
                                                   : local_draft_preview_encoded_limit(format);
}
inline bool local_draft_preview_request_valid(const AccountMediaLocalPreviewRequest& r) noexcept {
    if (r.reserved || r.lease_serial || !r.owner_scope || !r.owner_generation ||
        r.key.kind != MediaAssetKind::local_draft_preview || !media_asset_key_valid(r.key) ||
        r.snapshot.identity.reserved || !r.snapshot.bytes ||
        r.snapshot.bytes > (local_draft_preview_thumbnail(r.key)
                                ? local_draft_preview_thumbnail_limit(media_asset_catalog_format(r.key))
                                : local_draft_preview_encoded_limit(media_asset_catalog_format(r.key))))
        return false;
    if (r.content.context)
        return upload_source_valid(r.content) && r.content.bytes == r.snapshot.bytes && !r.path.count;
    if (r.content.ops || r.content.bytes || !r.path.data || !r.path.count ||
        r.path.count > kLocalDraftPreviewPathMaximum || r.snapshot.identity.reserved)
        return false;
    for (u32 i = 0; i < r.path.count; ++i)
        if (!r.path[i])
            return false;
    return true;
}
inline bool local_draft_preview_snapshot_equal(const FileSnapshot& a, const FileSnapshot& b) noexcept {
    return a.bytes == b.bytes && a.identity.index == b.identity.index &&
           a.identity.last_write == b.identity.last_write && a.identity.volume == b.identity.volume &&
           a.identity.reserved == b.identity.reserved;
}
static_assert(sizeof(AccountMediaLocalPreviewRequest) == 120);
} // namespace maicord
