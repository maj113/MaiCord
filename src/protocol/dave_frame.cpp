#include "maicord/protocol/dave_frame.hpp"

namespace maicord {
namespace {
bool leb(ConstSlice<u8> bytes, u32& cursor, u64& value) noexcept {
    value = 0;
    for (u32 group = 0; group != 10; ++group) {
        if (cursor == bytes.count)
            return false;
        const u8 byte = bytes.data[cursor++];
        if (group == 9 && byte > 1)
            return false;
        value |= static_cast<u64>(byte & 0x7F) << (group * 7);
        if (!(byte & 0x80))
            return true;
    }
    return false;
}
DaveFrameParseResult failure(DaveFrameError error) noexcept {
    return {0, error};
}
} // namespace
DaveFrameParseResult dave_frame_parse(ConstSlice<u8> frame, Slice<DaveClearRange> ranges,
                                      DaveFrameView* output) noexcept {
    if (frame.count < 2 || frame.data[frame.count - 2] != 0xFA || frame.data[frame.count - 1] != 0xFA)
        return failure(DaveFrameError::not_encrypted);
    if (frame.count < 12)
        return failure(DaveFrameError::malformed);
    const u32 extra = frame.data[frame.count - 3];
    if (extra < 12 || extra > frame.count)
        return failure(DaveFrameError::malformed);
    const u32 body = frame.count - extra;
    const ConstSlice<u8> encoded{frame.data + body + kDaveFrameTagBytes, extra - kDaveFrameTagBytes - 3};
    u32 cursor = 0;
    u64 nonce = 0;
    if (!leb(encoded, cursor, nonce) || nonce > ~u32{0})
        return failure(DaveFrameError::malformed);
    const u32 ranges_begin = cursor;
    u32 count = 0, previous_end = 0, clear = 0;
    while (cursor != encoded.count) {
        u64 offset = 0, bytes = 0;
        if (!leb(encoded, cursor, offset) || !leb(encoded, cursor, bytes) || offset < previous_end ||
            offset > body || bytes > body - offset)
            return failure(DaveFrameError::malformed);
        previous_end = static_cast<u32>(offset + bytes);
        clear += static_cast<u32>(bytes);
        ++count;
    }
    if (count > ranges.count)
        return {count, DaveFrameError::range_capacity};
    // Metadata is at most 244 bytes. Revisit it to publish ranges atomically,
    // without a private maximum-sized range array or touching frame contents.
    cursor = ranges_begin;
    for (u32 i = 0; i != count; ++i) {
        u64 offset = 0, bytes = 0;
        leb(encoded, cursor, offset);
        leb(encoded, cursor, bytes);
        ranges.data[i] = {static_cast<u32>(offset), static_cast<u32>(bytes)};
    }
    *output = {frame, {ranges.data, count}, body, clear, static_cast<u32>(nonce)};
    return {count, DaveFrameError::none};
}
void dave_frame_gather(const DaveFrameView& view, u8* aad, u8* cipher) noexcept {
    u32 read = 0;
    for (u32 i = 0; i != view.clear_ranges.count; ++i) {
        const auto& range = view.clear_ranges[i];
        const u32 gap = range.offset - read;
        copy_bytes(cipher, view.frame.data + read, gap);
        copy_bytes(aad, view.frame.data + range.offset, range.bytes);
        cipher += gap;
        aad += range.bytes;
        read = range.offset + range.bytes;
    }
    copy_bytes(cipher, view.frame.data + read, view.body_bytes - read);
}
void dave_frame_reconstruct(const DaveFrameView& view, const u8* plaintext, u8* output) noexcept {
    const bool in_place = output == view.frame.data;
    u32 written = 0;
    for (u32 i = 0; i != view.clear_ranges.count; ++i) {
        const auto& range = view.clear_ranges[i];
        const u32 gap = range.offset - written;
        copy_bytes(output + written, plaintext, gap);
        if (!in_place)
            copy_bytes(output + range.offset, view.frame.data + range.offset, range.bytes);
        plaintext += gap;
        written = range.offset + range.bytes;
    }
    copy_bytes(output + written, plaintext, view.body_bytes - written);
}
} // namespace maicord
