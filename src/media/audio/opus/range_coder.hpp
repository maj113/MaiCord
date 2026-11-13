#pragma once
// The Opus range coder (RFC 6716 4.1 and 5.1): range-coded symbols from the front of a frame, raw bits from
// its end. Both directions keep the same 32-bit range, so `range` after the last symbol is the "final
// range" that encoder and decoder must agree on.
#include "maicord/core/base.hpp"

namespace maicord::opus {

using i8 = signed char;

// Bits needed to hold value: 0 for 0, 32 for 2^31 and above.
inline u32 bit_length(u32 value) noexcept {
    return value ? 32 - static_cast<u32>(__builtin_clz(value)) : 0;
}

inline constexpr u32 kFractionBits = 3; // tell_fraction() counts in 1/8 bit

struct RangeCoder {
    u8* data;
    u32 size;   // bytes of the frame; shrinks when a redundant frame is cut off its end
    u32 front;  // range-coded bytes read
    u32 back;   // raw bytes read from the end
    u32 window; // raw bits read but not yet returned
    u32 window_bits;
    u32 bits_total; // whole bits read, counting the bits still in `range`
    u32 range;
    u32 value;  // top of the range minus the coded value, minus one
    u32 scale;  // range / total of the symbol being decoded
    u32 last;   // the last front byte: its low bit belongs to the next symbol
    bool error; // a uniform integer above its bound: the frame is corrupt

    u32 next_front() noexcept {
        return front < size ? data[front++] : 0;
    }
    void normalize() noexcept {
        while (range <= 0x800000u) {
            bits_total += 8;
            range <<= 8;
            const u32 previous = last;
            last = next_front();
            value = ((value << 8) + (0xffu & ~((previous << 8 | last) >> 1))) & 0x7fffffffu;
        }
    }
    void start_decode(const u8* bytes, u32 count) noexcept {
        data = const_cast<u8*>(bytes), size = count;
        front = back = window = window_bits = 0;
        error = false;
        bits_total = 9;
        range = 128;
        last = next_front();
        value = 127 - (last >> 1);
        scale = 0;
        normalize();
    }
    // Bits used so far, rounded up; tell_fraction() in 1/8 bit.
    u32 tell() const noexcept {
        return bits_total - bit_length(range);
    }
    u32 tell_fraction() const noexcept {
        static constexpr u16 kCorrection[8]{35733, 38967, 42495, 46340, 50535, 55109, 60097, 65535};
        const u32 length = bit_length(range);
        const u32 top = range >> (length - 16);
        u32 eighths = (top >> 12) - 8;
        eighths += top > kCorrection[eighths];
        return (bits_total << kFractionBits) - ((length << kFractionBits) + eighths);
    }
    // A symbol of a distribution with `total` (or 2^bits) steps: the cumulative frequency it falls in, then
    // update() with the symbol's interval [low, high).
    u32 decode(u32 total) noexcept {
        scale = range / total;
        const u32 step = value / scale;
        return total - minimum(step + 1, total);
    }
    u32 decode_bits(u32 bits) noexcept {
        scale = range >> bits;
        const u32 step = value / scale;
        return (1u << bits) - minimum(step + 1, 1u << bits);
    }
    void update(u32 low, u32 high, u32 total) noexcept {
        const u32 skipped = scale * (total - high);
        value -= skipped;
        range = low ? scale * (high - low) : range - skipped;
        normalize();
    }
    // One bit that is set with probability 2^-log_probability.
    bool bit(u32 log_probability) noexcept {
        const u32 set = range >> log_probability;
        const bool result = value < set;
        if (!result)
            value -= set;
        range = result ? set : range - set;
        normalize();
        return result;
    }
    // A symbol of an inverse cumulative distribution table (2^bits - cumulative frequency, ending in 0).
    u32 icdf(const u8* table, u32 bits) noexcept {
        const u32 step = range >> bits;
        u32 high = range, low, symbol = 0;
        for (;; ++symbol) {
            low = step * table[symbol];
            if (value >= low)
                break;
            high = low;
        }
        value -= low;
        range = high - low;
        normalize();
        return symbol;
    }
    // The same for a table of two symbols {first, 0}: true for the second symbol.
    bool binary(u32 first) noexcept {
        const u32 low = (range >> 8) * first;
        const bool second = value < low;
        if (!second)
            value -= low;
        range = second ? low : range - low;
        normalize();
        return second;
    }
    // 1 to 25 raw bits from the end of the frame.
    u32 raw(u32 bits) noexcept {
        while (window_bits < bits) {
            window |= (back < size ? data[size - ++back] : 0u) << window_bits;
            window_bits += 8;
        }
        const u32 result = window & ((1u << bits) - 1);
        window >>= bits;
        window_bits -= bits;
        bits_total += bits;
        return result;
    }
    // A uniformly distributed integer below total (2 or more).
    u32 uniform(u32 total) noexcept {
        const u32 maximum_value = total - 1;
        u32 bits = bit_length(maximum_value);
        if (bits <= 8) {
            const u32 symbol = decode(total);
            update(symbol, symbol + 1, total);
            return symbol;
        }
        bits -= 8;
        const u32 top = (maximum_value >> bits) + 1;
        const u32 symbol = decode(top);
        update(symbol, symbol + 1, top);
        const u32 result = symbol << bits | raw(bits);
        if (result <= maximum_value)
            return result;
        error = true;
        return maximum_value;
    }

    // Encoding: `value` is the low end of the range, `last` the byte held back until no carry can reach it
    // (kNoByte before the first) and `scale` the count of 0xff bytes held after it.
    static constexpr u32 kNoByte = ~0u;
    void start_encode(u8* bytes, u32 capacity) noexcept {
        data = bytes, size = capacity;
        front = back = window = window_bits = 0;
        error = false;
        bits_total = 33;
        range = 0x80000000u;
        value = 0;
        last = kNoByte;
        scale = 0;
    }
    void put_front(u32 byte) noexcept {
        if (front + back < size)
            data[front++] = static_cast<u8>(byte);
        else
            error = true;
    }
    void put_back(u32 byte) noexcept {
        if (front + back < size)
            data[size - ++back] = static_cast<u8>(byte);
        else
            error = true;
    }
    // The next eight bits with a carry in bit 8.
    void carry_out(u32 symbol) noexcept {
        if (symbol == 0xff) {
            ++scale;
            return;
        }
        const u32 carry = symbol >> 8;
        if (last != kNoByte)
            put_front(last + carry);
        for (; scale; --scale)
            put_front((0xff + carry) & 0xff);
        last = symbol & 0xff;
    }
    void normalize_encode() noexcept {
        while (range <= 0x800000u) {
            carry_out(value >> 23);
            value = (value << 8) & 0x7fffffffu;
            range <<= 8;
            bits_total += 8;
        }
    }
    void encode(u32 low, u32 high, u32 total) noexcept {
        const u32 step = range / total;
        if (low) {
            value += range - step * (total - low);
            range = step * (high - low);
        } else {
            range -= step * (total - high);
        }
        normalize_encode();
    }
    void encode_bits(u32 low, u32 high, u32 bits) noexcept {
        const u32 step = range >> bits;
        if (low) {
            value += range - step * ((1u << bits) - low);
            range = step * (high - low);
        } else {
            range -= step * ((1u << bits) - high);
        }
        normalize_encode();
    }
    void encode_bit(bool set, u32 log_probability) noexcept {
        const u32 share = range >> log_probability, rest = range - share;
        if (set)
            value += rest;
        range = set ? share : rest;
        normalize_encode();
    }
    void encode_icdf(u32 symbol, const u8* table, u32 bits) noexcept {
        const u32 step = range >> bits;
        if (symbol) {
            value += range - step * table[symbol - 1];
            range = step * (table[symbol - 1] - table[symbol]);
        } else {
            range -= step * table[symbol];
        }
        normalize_encode();
    }
    void encode_raw(u32 symbol, u32 bits) noexcept {
        if (window_bits + bits > 32) {
            do {
                put_back(window & 0xff);
                window >>= 8;
                window_bits -= 8;
            } while (window_bits >= 8);
        }
        window |= symbol << window_bits;
        window_bits += bits;
        bits_total += bits;
    }
    void encode_uniform(u32 symbol, u32 total) noexcept {
        const u32 maximum_value = total - 1;
        u32 bits = bit_length(maximum_value);
        if (bits <= 8) {
            encode(symbol, symbol + 1, total);
            return;
        }
        bits -= 8;
        const u32 top = symbol >> bits;
        encode(top, top + 1, (maximum_value >> bits) + 1);
        encode_raw(symbol & ((1u << bits) - 1), bits);
    }
    // Out of line (opus_encoder.cpp): overwrite the first `count` bits, move the raw bits to a smaller
    // frame end, and flush both ends.
    void patch_initial_bits(u32 symbol, u32 count) noexcept;
    void shrink(u32 bytes) noexcept;
    void finish() noexcept;
};

} // namespace maicord::opus
