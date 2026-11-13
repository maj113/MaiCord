// The parts of the range encoder that run once per frame (RFC 6716 5.1.5).
#include "range_coder.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::opus {

void RangeCoder::patch_initial_bits(u32 symbol, u32 count) noexcept {
    const u32 shift = 8 - count, mask = ((1u << count) - 1) << shift;
    if (front > 0) // the first byte is final
        data[0] = static_cast<u8>((data[0] & ~mask) | symbol << shift);
    else if (last != kNoByte) // it still waits for a carry
        last = (last & ~mask) | symbol << shift;
    else if (range <= 0x80000000u >> count) // nothing left the range yet
        value = (value & ~(mask << 23)) | symbol << (23 + shift);
    else
        error = true; // fewer than `count` bits coded so far
}

void RangeCoder::shrink(u32 bytes) noexcept {
    copy_bytes(data + bytes - back, data + size - back, back); // may overlap
    size = bytes;
}

void RangeCoder::finish() noexcept {
    // The fewest bits that decode to the symbols so far whatever follows them.
    i32 bits = 32 - static_cast<i32>(bit_length(range));
    u32 mask = 0x7fffffffu >> bits;
    u32 end = (value + mask) & ~mask;
    if ((end | mask) >= value + range) {
        ++bits;
        mask >>= 1;
        end = (value + mask) & ~mask;
    }
    for (; bits > 0; bits -= 8) {
        carry_out(end >> 23);
        end = (end << 8) & 0x7fffffffu;
    }
    if (last != kNoByte || scale)
        carry_out(0);
    for (; window_bits >= 8; window_bits -= 8, window >>= 8)
        put_back(window & 0xff);
    if (error)
        return;
    clear_bytes(data + front, size - front - back);
    if (!window_bits)
        return;
    // The last raw bits share a byte with the end of the range coder's data.
    if (back >= size) {
        error = true;
        return;
    }
    const i32 spare = -bits;
    if (front + back >= size && spare < static_cast<i32>(window_bits)) {
        window &= (1u << spare) - 1; // the range coder's bits matter more
        error = true;
    }
    data[size - back - 1] |= static_cast<u8>(window);
}

} // namespace maicord::opus
