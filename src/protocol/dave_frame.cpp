#include "maicord/protocol/dave_frame.hpp"
#include "maicord/core/byte_range.hpp"

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
ConstSlice<u8> raw(ConstSlice<DaveClearRange> ranges) noexcept {
    return {reinterpret_cast<const u8*>(ranges.data),
            ranges.count * static_cast<u32>(sizeof(DaveClearRange))};
}
bool range_storage(ConstSlice<DaveClearRange> ranges) noexcept {
    return ranges.count <= ~u32{0} / sizeof(DaveClearRange) && byte_range_valid(raw(ranges));
}
bool valid_view(const DaveFrameView& view) noexcept {
    if (!byte_range_valid(view.frame) || view.frame.count < 12 || view.body_bytes > view.frame.count - 12 ||
        !range_storage(view.clear_ranges) || view.clear_ranges.count > kDaveMaximumClearRanges)
        return false;
    u32 end = 0, clear = 0;
    for (u32 i = 0; i != view.clear_ranges.count; ++i) {
        const auto& range = view.clear_ranges[i];
        if (range.offset < end || range.offset > view.body_bytes ||
            range.bytes > view.body_bytes - range.offset)
            return false;
        end = range.offset + range.bytes;
        clear += range.bytes;
    }
    return clear == view.clear_bytes;
}
bool metadata_alias(const DaveFrameView& view, ConstSlice<u8> output) noexcept {
    return byte_ranges_overlap(output, raw(view.clear_ranges)) ||
           byte_ranges_overlap(output, {reinterpret_cast<const u8*>(&view), sizeof(view)});
}
DaveFrameParseResult failure(DaveFrameError error) noexcept {
    return {0, error};
}
} // namespace
DaveFrameParseResult dave_frame_parse(ConstSlice<u8> frame, Slice<DaveClearRange> ranges,
                                      DaveFrameView* output) noexcept {
    const ConstSlice<DaveClearRange> range_span{ranges.data, ranges.count};
    if (!output || !byte_range_valid(frame) || !range_storage(range_span))
        return failure(DaveFrameError::invalid_input);
    const auto range_bytes = raw(range_span);
    const ConstSlice<u8> output_bytes{reinterpret_cast<const u8*>(output), sizeof(*output)};
    if (!byte_range_valid(output_bytes) || byte_ranges_overlap(frame, range_bytes) ||
        byte_ranges_overlap(frame, output_bytes) || byte_ranges_overlap(range_bytes, output_bytes))
        return failure(DaveFrameError::invalid_input);
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
DaveFrameError dave_frame_gather(const DaveFrameView& view, Slice<u8> aad, Slice<u8> ciphertext) noexcept {
    if (!valid_view(view) || !byte_range_valid({aad.data, aad.count}) ||
        !byte_range_valid({ciphertext.data, ciphertext.count}))
        return DaveFrameError::invalid_input;
    const u32 encrypted = view.body_bytes - view.clear_bytes;
    if (aad.count < view.clear_bytes || ciphertext.count < encrypted)
        return DaveFrameError::output_capacity;
    const ConstSlice<u8> a{aad.data, view.clear_bytes}, c{ciphertext.data, encrypted};
    if (byte_ranges_overlap(a, c) || metadata_alias(view, a) || metadata_alias(view, c) ||
        byte_ranges_overlap(view.frame, a) ||
        (byte_ranges_overlap(view.frame, c) && (view.clear_bytes || ciphertext.data != view.frame.data)))
        return DaveFrameError::invalid_input;
    if (!view.clear_bytes && ciphertext.data == view.frame.data)
        return DaveFrameError::none;
    u32 read = 0, clear = 0, cipher = 0;
    for (u32 i = 0; i != view.clear_ranges.count; ++i) {
        const auto& range = view.clear_ranges[i];
        const u32 gap = range.offset - read;
        if (gap)
            copy_bytes(ciphertext.data + cipher, view.frame.data + read, gap);
        if (range.bytes)
            copy_bytes(aad.data + clear, view.frame.data + range.offset, range.bytes);
        cipher += gap;
        clear += range.bytes;
        read = range.offset + range.bytes;
    }
    if (read < view.body_bytes && ciphertext.data + cipher != view.frame.data + read)
        copy_bytes(ciphertext.data + cipher, view.frame.data + read, view.body_bytes - read);
    return DaveFrameError::none;
}
DaveFrameError dave_frame_reconstruct(const DaveFrameView& view, ConstSlice<u8> plaintext,
                                      Slice<u8> output) noexcept {
    if (!valid_view(view) || !byte_range_valid(plaintext) ||
        !byte_range_valid({output.data, output.count}) ||
        plaintext.count != view.body_bytes - view.clear_bytes)
        return DaveFrameError::invalid_input;
    if (output.count < view.body_bytes)
        return DaveFrameError::output_capacity;
    const ConstSlice<u8> written{output.data, view.body_bytes};
    if (metadata_alias(view, written) ||
        (byte_ranges_overlap(written, view.frame) && output.data != view.frame.data) ||
        (byte_ranges_overlap(written, plaintext) && (view.clear_bytes || output.data != plaintext.data)))
        return DaveFrameError::invalid_input;
    if (!view.clear_bytes && plaintext.data == output.data)
        return DaveFrameError::none;
    u32 frame = 0, cipher = 0;
    for (u32 i = 0; i != view.clear_ranges.count; ++i) {
        const auto& range = view.clear_ranges[i];
        const u32 gap = range.offset - frame;
        if (gap)
            copy_bytes(output.data + frame, plaintext.data + cipher, gap);
        if (range.bytes && output.data != view.frame.data)
            copy_bytes(output.data + range.offset, view.frame.data + range.offset, range.bytes);
        cipher += gap;
        frame = range.offset + range.bytes;
    }
    if (frame < view.body_bytes && output.data + frame != plaintext.data + cipher)
        copy_bytes(output.data + frame, plaintext.data + cipher, view.body_bytes - frame);
    return DaveFrameError::none;
}
} // namespace maicord
