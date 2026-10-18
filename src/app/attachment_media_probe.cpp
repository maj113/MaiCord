#include "maicord/app/attachment_media_probe.hpp"
#include "maicord/core/byte_order.hpp"

namespace maicord {
namespace {
bool type_is(const u8* header, const char* type) noexcept {
    return header[4] == static_cast<u8>(type[0]) && header[5] == static_cast<u8>(type[1]) &&
           header[6] == static_cast<u8>(type[2]) && header[7] == static_cast<u8>(type[3]);
}
// Finds a child box of type within [begin, end); returns its payload range.
bool find_box(u64 begin, u64 end, const char* type, AttachmentProbeRead read, void* context, u64* payload,
              u64* payload_end) noexcept {
    u64 offset = begin;
    for (u32 boxes = 0; offset + 8 <= end && boxes < 4096; ++boxes) {
        u8 header[16];
        if (!read(context, offset, {header, 8}))
            return false;
        u64 size = byte_load_be32(header);
        u64 header_bytes = 8;
        if (size == 1) {
            if (offset + 16 > end || !read(context, offset + 8, {header + 8, 8}))
                return false;
            size = byte_load_be64(header + 8);
            header_bytes = 16;
        } else if (size == 0) {
            size = end - offset;
        }
        if (size < header_bytes || size > end - offset)
            return false;
        if (type_is(header, type)) {
            *payload = offset + header_bytes;
            *payload_end = offset + size;
            return true;
        }
        offset += size;
    }
    return false;
}
} // namespace

bool attachment_mp4_duration(u64 file_bytes, AttachmentProbeRead read, void* context, u32* milliseconds) noexcept {
    if (!read || !milliseconds || file_bytes < 16)
        return false;
    *milliseconds = 0;
    u64 moov = 0, moov_end = 0, mvhd = 0, mvhd_end = 0;
    if (!find_box(0, file_bytes, "moov", read, context, &moov, &moov_end) ||
        !find_box(moov, moov_end, "mvhd", read, context, &mvhd, &mvhd_end) || mvhd_end - mvhd < 20)
        return false;
    u8 body[32];
    const u32 available = static_cast<u32>(mvhd_end - mvhd < sizeof(body) ? mvhd_end - mvhd : sizeof(body));
    if (!read(context, mvhd, {body, available}))
        return false;
    u32 timescale = 0;
    u64 duration = 0;
    if (body[0] == 1) {
        if (available < 32)
            return false;
        timescale = byte_load_be32(body + 20);
        duration = byte_load_be64(body + 24);
    } else if (body[0] == 0) {
        timescale = byte_load_be32(body + 12);
        duration = byte_load_be32(body + 16);
    } else {
        return false;
    }
    // All-ones durations mean "unknown" in the movie header.
    if (!timescale || !duration || duration == 0xffffffffull || duration == ~0ull)
        return false;
    const u64 ms = duration / timescale * 1000u + duration % timescale * 1000u / timescale;
    if (!ms || ms > 0xffffffffull)
        return false;
    *milliseconds = static_cast<u32>(ms);
    return true;
}

} // namespace maicord
