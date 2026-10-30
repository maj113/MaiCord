#include "maicord/media/zstd.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/sys/simd.hpp"

namespace maicord {
namespace {

constexpr u32 kFrameMagic = 0xFD2FB528u;
constexpr u32 kSkippableMagic = 0x184D2A50u;
constexpr u32 kMaximumWindowLog = 23;
constexpr u32 kHuffmanMaximumBits = 11;
constexpr u32 kLiteralLengthMaximumLog = 9, kOffsetMaximumLog = 8, kMatchLengthMaximumLog = 9;
constexpr u32 kLiteralLengthMaximumSymbol = 35, kOffsetMaximumSymbol = 31, kMatchLengthMaximumSymbol = 52;

constexpr u32 kLiteralLengthBase[36] = {0,  1,  2,   3,   4,   5,   6,    7,    8,    9,     10,    11,
                                        12, 13, 14,  15,  16,  18,  20,   22,   24,   28,    32,    40,
                                        48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
constexpr u8 kLiteralLengthBits[36] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  1,  1,
                                       1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
constexpr u32 kMatchLengthBase[53] = {3,  4,  5,  6,  7,  8,  9,  10,  11,  12,  13,   14,   15,   16,
                                      17, 18, 19, 20, 21, 22, 23, 24,  25,  26,  27,   28,   29,   30,
                                      31, 32, 33, 34, 35, 37, 39, 41,  43,  47,  51,   59,   67,   83,
                                      99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
constexpr u8 kMatchLengthBits[53] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
                                     2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
constexpr i16 kDefaultLiteralLengths[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2,
                                            2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
constexpr i16 kDefaultMatchLengths[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
constexpr i16 kDefaultOffsets[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1,
                                     1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

__forceinline u32 highbit(u32 value) noexcept {
    unsigned long index = 0;
    _BitScanReverse(&index, value);
    return index;
}
__forceinline u64 load64(const u8* p) noexcept {
    return *reinterpret_cast<const u64 __unaligned*>(p);
}
__forceinline u32 load32(const u8* p) noexcept {
    return *reinterpret_cast<const u32 __unaligned*>(p);
}
__forceinline u32 load24(const u8* p) noexcept {
    return p[0] | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16;
}
__forceinline u64 rotl(u64 value, u32 bits) noexcept {
    return value << bits | value >> (64 - bits);
}

// ---------------------------------------------------------------- XXH64
constexpr u64 kPrime1 = 0x9E3779B185EBCA87ull, kPrime2 = 0xC2B2AE3D27D4EB4Full, kPrime3 = 0x165667B19E3779F9ull,
              kPrime4 = 0x85EBCA77C2B2AE63ull, kPrime5 = 0x27D4EB2F165667C5ull;
u64 hash_round(u64 accumulator, u64 input) noexcept {
    return rotl(accumulator + input * kPrime2, 31) * kPrime1;
}
void hash_reset(ZstdStreamDecoder* d) noexcept {
    d->hash_state[0] = kPrime1 + kPrime2;
    d->hash_state[1] = kPrime2;
    d->hash_state[2] = 0;
    d->hash_state[3] = 0 - kPrime1;
    d->hash_length = 0;
    d->hash_buffered = 0;
}
void hash_stripe(ZstdStreamDecoder* d, const u8* p) noexcept {
    for (u32 i = 0; i < 4; ++i)
        d->hash_state[i] = hash_round(d->hash_state[i], load64(p + 8 * i));
}
void hash_update(ZstdStreamDecoder* d, const u8* p, u32 bytes) noexcept {
    d->hash_length += bytes;
    if (d->hash_buffered) {
        const u32 take = minimum(bytes, 32 - d->hash_buffered);
        copy_bytes(d->hash_buffer + d->hash_buffered, p, take);
        d->hash_buffered += take;
        p += take;
        bytes -= take;
        if (d->hash_buffered < 32)
            return;
        hash_stripe(d, d->hash_buffer);
        d->hash_buffered = 0;
    }
    for (; bytes >= 32; p += 32, bytes -= 32)
        hash_stripe(d, p);
    copy_bytes(d->hash_buffer, p, bytes);
    d->hash_buffered = bytes;
}
u32 hash_digest(const ZstdStreamDecoder& d) noexcept {
    u64 h;
    if (d.hash_length >= 32) {
        h = rotl(d.hash_state[0], 1) + rotl(d.hash_state[1], 7) + rotl(d.hash_state[2], 12) +
            rotl(d.hash_state[3], 18);
        for (u32 i = 0; i < 4; ++i)
            h = (h ^ hash_round(0, d.hash_state[i])) * kPrime1 + kPrime4;
    } else {
        h = d.hash_state[2] + kPrime5;
    }
    h += d.hash_length;
    const u8* p = d.hash_buffer;
    u32 left = d.hash_buffered;
    for (; left >= 8; p += 8, left -= 8)
        h = rotl(h ^ hash_round(0, load64(p)), 27) * kPrime1 + kPrime4;
    if (left >= 4) {
        h = rotl(h ^ static_cast<u64>(load32(p)) * kPrime1, 23) * kPrime2 + kPrime3;
        p += 4;
        left -= 4;
    }
    for (; left; ++p, --left)
        h = rotl(h ^ *p * kPrime5, 11) * kPrime1;
    h ^= h >> 33;
    h *= kPrime2;
    h ^= h >> 29;
    h *= kPrime3;
    h ^= h >> 32;
    return static_cast<u32>(h);
}

// ------------------------------------------------------------ bitstreams
// A backward bitstream: reading starts below the final byte's highest set
// bit and moves toward the first byte. Positions below zero read as zero;
// a negative position means the stream was overread.
struct BackBits {
    const u8* data;
    u32 size;
    i64 bits;
};
bool back_init(BackBits* r, const u8* data, u32 size) noexcept {
    if (!size || !data[size - 1])
        return false;
    r->data = data;
    r->size = size;
    r->bits = static_cast<i64>(size - 1) * 8 + highbit(data[size - 1]);
    return true;
}
// count <= 56.
__forceinline u64 back_peek(const BackBits& r, i64 start, u32 count) noexcept {
    u32 shift = 0;
    if (start < 0) {
        if (start + count <= 0)
            return 0;
        shift = static_cast<u32>(-start);
        count -= shift;
        start = 0;
    }
    const u32 byte = static_cast<u32>(start >> 3);
    u64 value;
    if (byte + 8 <= r.size) {
        value = load64(r.data + byte);
    } else {
        value = 0;
        for (u32 i = 0; byte + i < r.size; ++i)
            value |= static_cast<u64>(r.data[byte + i]) << (8 * i);
    }
    value >>= start & 7;
    return count ? (value & (~0ull >> (64 - count))) << shift : 0;
}
__forceinline u64 back_read(BackBits* r, u32 count) noexcept {
    r->bits -= count;
    return back_peek(*r, r->bits, count);
}

// A forward little-endian bitstream for table descriptions.
struct ForwardBits {
    const u8* data;
    u32 size;
    u32 bit;
};
u32 forward_peek(const ForwardBits& r, u32 count) noexcept {
    u64 value = 0;
    const u32 byte = r.bit >> 3;
    for (u32 i = 0; i < 6 && byte + i < r.size; ++i)
        value |= static_cast<u64>(r.data[byte + i]) << (8 * i);
    return static_cast<u32>((value >> (r.bit & 7)) & ((1ull << count) - 1));
}

// ------------------------------------------------------------- FSE tables
// RFC 8878 4.1.1: normalized probabilities; -1 is "less than 1".
bool read_distribution(const u8* source, u32 size, i16* counts, u32 maximum_symbol, u32 maximum_log, u32* log,
                       u32* consumed) noexcept {
    ForwardBits r{source, size, 0};
    if (!size)
        return false;
    const u32 accuracy = forward_peek(r, 4) + 5;
    r.bit = 4;
    if (accuracy > maximum_log)
        return false;
    i32 remaining = (1 << accuracy) + 1;
    i32 threshold = 1 << accuracy;
    u32 bits = accuracy + 1;
    u32 symbol = 0;
    bool previous_zero = false;
    for (u32 i = 0; i <= maximum_symbol; ++i)
        counts[i] = 0;
    while (remaining > 1 && symbol <= maximum_symbol) {
        if (previous_zero) {
            u32 zeros = symbol;
            for (;;) {
                const u32 flag = forward_peek(r, 2);
                r.bit += 2;
                zeros += flag;
                if (flag != 3)
                    break;
                if (zeros > maximum_symbol + 1 || r.bit > size * 8)
                    return false;
            }
            if (zeros > maximum_symbol)
                return false;
            symbol = zeros;
        }
        const i32 maximum = 2 * threshold - 1 - remaining;
        const u32 low = forward_peek(r, bits - 1);
        i32 count;
        if (static_cast<i32>(low) < maximum) {
            count = static_cast<i32>(low);
            r.bit += bits - 1;
        } else {
            count = static_cast<i32>(forward_peek(r, bits));
            if (count >= threshold)
                count -= maximum;
            r.bit += bits;
        }
        --count;
        remaining -= count < 0 ? -count : count;
        if (remaining < 1)
            return false;
        counts[symbol++] = static_cast<i16>(count);
        previous_zero = count == 0;
        while (remaining < threshold) {
            --bits;
            threshold >>= 1;
        }
        if (r.bit > size * 8)
            return false;
    }
    if (remaining != 1 || r.bit > size * 8)
        return false;
    *log = accuracy;
    *consumed = (r.bit + 7) >> 3;
    return true;
}

bool build_table(ZstdFseEntry* table, const i16* counts, u32 symbols, u32 log) noexcept {
    const u32 size = 1u << log;
    u32 high = size - 1;
    u16 next[64];
    for (u32 s = 0; s < symbols; ++s) {
        if (counts[s] == -1) {
            table[high--].symbol = static_cast<u8>(s);
            next[s] = 1;
        } else {
            next[s] = static_cast<u16>(counts[s]);
        }
    }
    const u32 step = (size >> 1) + (size >> 3) + 3;
    const u32 mask = size - 1;
    u32 position = 0;
    for (u32 s = 0; s < symbols; ++s) {
        for (i32 i = 0; i < counts[s]; ++i) {
            table[position].symbol = static_cast<u8>(s);
            do
                position = (position + step) & mask;
            while (position > high);
        }
    }
    if (position != 0)
        return false;
    for (u32 u = 0; u < size; ++u) {
        const u32 state = next[table[u].symbol]++;
        const u32 bits = log - highbit(state);
        table[u].bits = static_cast<u8>(bits);
        table[u].baseline = static_cast<u16>((state << bits) - size);
    }
    return true;
}

// --------------------------------------------------------------- Huffman
bool huffman_weights(const u8* source, u32 size, u8* weights, u32* count, u32* consumed) noexcept {
    if (!size)
        return false;
    const u32 header = source[0];
    if (header >= 128) {
        const u32 n = header - 127;
        const u32 bytes = (n + 1) / 2;
        if (1 + bytes > size)
            return false;
        for (u32 i = 0; i < n; ++i)
            weights[i] = static_cast<u8>(i & 1 ? source[1 + i / 2] & 15 : source[1 + i / 2] >> 4);
        *count = n;
        *consumed = 1 + bytes;
        return true;
    }
    if (!header || 1 + header > size)
        return false;
    i16 counts[16];
    u32 log = 0, used = 0;
    if (!read_distribution(source + 1, header, counts, 15, 6, &log, &used) || used >= header)
        return false;
    ZstdFseEntry table[64];
    if (!build_table(table, counts, 16, log))
        return false;
    BackBits r;
    if (!back_init(&r, source + 1 + used, header - used))
        return false;
    u32 states[2];
    states[0] = static_cast<u32>(back_read(&r, log));
    states[1] = static_cast<u32>(back_read(&r, log));
    u32 n = 0;
    for (u32 which = 0;; which ^= 1) {
        if (n > 253)
            return false;
        const ZstdFseEntry entry = table[states[which]];
        weights[n++] = entry.symbol;
        states[which] = entry.baseline + static_cast<u32>(back_read(&r, entry.bits));
        if (r.bits < 0) {
            weights[n++] = table[states[which ^ 1]].symbol;
            break;
        }
    }
    *count = n;
    *consumed = 1 + header;
    return true;
}

bool build_huffman(ZstdStreamDecoder* d, u8* weights, u32 count) noexcept {
    u32 total = 0;
    u32 ranks[kHuffmanMaximumBits + 1] = {};
    for (u32 i = 0; i < count; ++i) {
        if (weights[i] > kHuffmanMaximumBits)
            return false;
        total += weights[i] ? 1u << (weights[i] - 1) : 0;
        ++ranks[weights[i]];
    }
    if (!total || count > 255)
        return false;
    const u32 bits = highbit(total) + 1;
    if (bits > kHuffmanMaximumBits)
        return false;
    const u32 rest = (1u << bits) - total;
    if (rest & (rest - 1))
        return false;
    weights[count++] = static_cast<u8>(highbit(rest) + 1);
    ++ranks[weights[count - 1]];
    // Entries run by weight, symbols ascending within one: each weight owns a span of the table. The spans
    // add up to total + rest = 1 << bits entries, so none is left unfilled.
    u32 span[kHuffmanMaximumBits + 1];
    u32 next = 0;
    for (u32 w = 1; w <= bits; ++w) {
        span[w] = next;
        next += ranks[w] << (w - 1);
    }
    for (u32 s = 0; s < count; ++s) {
        const u32 w = weights[s];
        if (!w)
            continue;
        const ZstdHuffmanEntry entry{static_cast<u8>(s), static_cast<u8>(bits + 1 - w)};
        for (u32 k = 0, n = 1u << (w - 1); k < n; ++k)
            d->huffman[span[w] + k] = entry;
        span[w] += 1u << (w - 1);
    }
    d->huffman_bits = static_cast<u8>(bits);
    return true;
}

__forceinline u8 huffman_symbol(const ZstdStreamDecoder& d, BackBits* r, u32 bits) noexcept {
    const ZstdHuffmanEntry entry = d.huffman[back_peek(*r, r->bits - bits, bits)];
    r->bits -= entry.bits;
    return entry.symbol;
}

// Decodes output[from, to) and reports whether the stream ended exactly there.
// The reader is a local copy: output bytes may alias the caller's.
__declspec(noinline) bool huffman_finish(const ZstdStreamDecoder& d, BackBits reader, u8* output, u32 from,
                                         u32 to) noexcept {
    const u32 bits = d.huffman_bits;
    for (u32 i = from; i < to; ++i)
        output[i] = huffman_symbol(d, &reader, bits);
    return reader.bits == 0;
}

bool huffman_stream(const ZstdStreamDecoder& d, const u8* source, u32 size, u8* output, u32 count) noexcept {
    BackBits r;
    return back_init(&r, source, size) && huffman_finish(d, r, output, 0, count);
}

// The four streams of a block in lockstep: each symbol waits on its stream's
// previous one, so interleaving keeps four lookups in flight instead of one.
// The last stream is shorter by at most three symbols.
bool huffman_four_streams(const ZstdStreamDecoder& d, const u8* source, const u32 sizes[4], u8* output,
                          u32 segment, u32 last) noexcept {
    BackBits r0, r1, r2, r3;
    if (!back_init(&r0, source, sizes[0]) || !back_init(&r1, source + sizes[0], sizes[1]) ||
        !back_init(&r2, source + sizes[0] + sizes[1], sizes[2]) ||
        !back_init(&r3, source + sizes[0] + sizes[1] + sizes[2], sizes[3]))
        return false;
    const u32 bits = d.huffman_bits;
    u8* const o1 = output + segment;
    u8* const o2 = o1 + segment;
    u8* const o3 = o2 + segment;
    u32 i = 0;
    for (; i < last; ++i) {
        output[i] = huffman_symbol(d, &r0, bits);
        o1[i] = huffman_symbol(d, &r1, bits);
        o2[i] = huffman_symbol(d, &r2, bits);
        o3[i] = huffman_symbol(d, &r3, bits);
    }
    return huffman_finish(d, r0, output, i, segment) && huffman_finish(d, r1, o1, i, segment) &&
           huffman_finish(d, r2, o2, i, segment) && r3.bits == 0;
}

// --------------------------------------------------------------- blocks
struct Literals {
    const u8* data;
    u32 count;
};

// RFC 8878 3.1.1.3.1. Decoded literals go to `scratch` (block maximum bytes).
bool decode_literals(ZstdStreamDecoder* d, const u8* source, u32 size, u8* scratch, Literals* literals,
                     u32* consumed) noexcept {
    if (!size)
        return false;
    const u32 type = source[0] & 3, format = (source[0] >> 2) & 3;
    if (type < 2) {
        u32 header = 1, regenerated = source[0] >> 3;
        if (format == 1) {
            if (size < 2)
                return false;
            header = 2;
            regenerated = (source[0] >> 4) | static_cast<u32>(source[1]) << 4;
        } else if (format == 3) {
            if (size < 3)
                return false;
            header = 3;
            regenerated = (source[0] >> 4) | static_cast<u32>(source[1]) << 4 | static_cast<u32>(source[2]) << 12;
        }
        if (regenerated > d->block_maximum)
            return false;
        if (type == 0) {
            if (size - header < regenerated)
                return false;
            *literals = {source + header, regenerated};
            *consumed = header + regenerated;
        } else {
            if (size - header < 1)
                return false;
            for (u32 i = 0; i < regenerated; ++i)
                scratch[i] = source[header];
            *literals = {scratch, regenerated};
            *consumed = header + 1;
        }
        return true;
    }
    u32 header, regenerated, compressed;
    const bool single = format == 0;
    if (format < 2) {
        if (size < 3)
            return false;
        const u32 v = load24(source);
        header = 3;
        regenerated = (v >> 4) & 0x3FF;
        compressed = (v >> 14) & 0x3FF;
    } else if (format == 2) {
        if (size < 4)
            return false;
        const u32 v = load32(source);
        header = 4;
        regenerated = (v >> 4) & 0x3FFF;
        compressed = v >> 18;
    } else {
        if (size < 5)
            return false;
        const u64 v = load32(source) | static_cast<u64>(source[4]) << 32;
        header = 5;
        regenerated = static_cast<u32>(v >> 4) & 0x3FFFF;
        compressed = static_cast<u32>(v >> 22) & 0x3FFFF;
    }
    if (regenerated > d->block_maximum || compressed > size - header)
        return false;
    const u8* p = source + header;
    u32 left = compressed;
    if (type == 2) {
        u8 weights[256];
        u32 count = 0, used = 0;
        if (!huffman_weights(p, left, weights, &count, &used) || !build_huffman(d, weights, count))
            return false;
        p += used;
        left -= used;
    } else if (!d->huffman_bits) {
        return false;
    }
    if (single) {
        if (!huffman_stream(*d, p, left, scratch, regenerated))
            return false;
    } else {
        if (left < 6 || regenerated < 6)
            return false;
        u32 sizes[4] = {p[0] | static_cast<u32>(p[1]) << 8, p[2] | static_cast<u32>(p[3]) << 8,
                        p[4] | static_cast<u32>(p[5]) << 8, 0};
        p += 6;
        left -= 6;
        if (sizes[0] > left || sizes[1] > left - sizes[0] || sizes[2] > left - sizes[0] - sizes[1])
            return false;
        sizes[3] = left - sizes[0] - sizes[1] - sizes[2];
        const u32 segment = (regenerated + 3) / 4;
        if (!huffman_four_streams(*d, p, sizes, scratch, segment, regenerated - 3 * segment))
            return false;
    }
    *literals = {scratch, regenerated};
    *consumed = header + compressed;
    return true;
}

bool sequence_table(u32 mode, const u8* source, u32 size, u32* consumed, const ZstdFseEntry** table, u8* log,
                    ZstdFseEntry* own, const ZstdFseEntry* defaults, u32 default_log, u32 maximum_symbol,
                    u32 maximum_log) noexcept {
    *consumed = 0;
    if (mode == 0) {
        *table = defaults;
        *log = static_cast<u8>(default_log);
        return true;
    }
    if (mode == 1) {
        if (!size || source[0] > maximum_symbol)
            return false;
        own[0] = {0, source[0], 0};
        *table = own;
        *log = 0;
        *consumed = 1;
        return true;
    }
    if (mode == 3)
        return *table != nullptr;
    i16 counts[64];
    u32 accuracy = 0;
    if (!read_distribution(source, size, counts, maximum_symbol, maximum_log, &accuracy, consumed) ||
        !build_table(own, counts, maximum_symbol + 1, accuracy))
        return false;
    *table = own;
    *log = static_cast<u8>(accuracy);
    return true;
}

__forceinline void copy16(u8* output, const u8* from) noexcept {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(output), _mm_loadu_si128(reinterpret_cast<const __m128i*>(from)));
}
// Writes may run up to 16 bytes past the match: the block's output has the
// literal guard behind it. A chunk never reads bytes it has not written yet
// once the offset is at least its width.
__forceinline void copy_match(u8* output, u32 offset, u32 length) noexcept {
    const u8* from = output - offset;
    if (offset >= 16) {
        for (u32 i = 0; i < length; i += 16)
            copy16(output + i, from + i);
    } else if (offset >= 8) {
        for (u32 i = 0; i < length; i += 8)
            *reinterpret_cast<u64 __unaligned*>(output + i) = *reinterpret_cast<const u64 __unaligned*>(from + i);
    } else {
        for (u32 i = 0; i < length; ++i)
            output[i] = from[i];
    }
}

// RFC 8878 3.1.1.3.2 and 3.1.1.4, decoding and executing each sequence in turn.
bool decode_sequences(ZstdStreamDecoder* d, const u8* source, u32 size, Literals literals, u8* output,
                      u32* produced) noexcept {
    if (!size)
        return false;
    u32 count = source[0], header = 1;
    if (count >= 128) {
        if (count < 255) {
            if (size < 2)
                return false;
            count = ((count - 128) << 8) + source[1];
            header = 2;
        } else {
            if (size < 3)
                return false;
            count = source[1] + (static_cast<u32>(source[2]) << 8) + 0x7F00;
            header = 3;
        }
    }
    u8* const base = d->storage.data + d->frame_start;
    u8* op = output;
    u8* const end = output + d->block_maximum;
    const u8* lit = literals.data;
    const u8* const lit_end = literals.data + literals.count;
    if (count) {
        if (header >= size)
            return false;
        const u32 modes = source[header++];
        if (modes & 3)
            return false;
        u32 used = 0;
        if (!sequence_table(modes >> 6, source + header, size - header, &used, &d->literal_lengths,
                            &d->literal_lengths_log, d->literal_length_table, d->default_literal_lengths, 6,
                            kLiteralLengthMaximumSymbol, kLiteralLengthMaximumLog))
            return false;
        header += used;
        if (!sequence_table((modes >> 4) & 3, source + header, size - header, &used, &d->offsets,
                            &d->offsets_log, d->offset_table, d->default_offsets, 5, kOffsetMaximumSymbol,
                            kOffsetMaximumLog))
            return false;
        header += used;
        if (!sequence_table((modes >> 2) & 3, source + header, size - header, &used, &d->match_lengths,
                            &d->match_lengths_log, d->match_length_table, d->default_match_lengths, 6,
                            kMatchLengthMaximumSymbol, kMatchLengthMaximumLog))
            return false;
        header += used;
        BackBits r;
        if (header > size || !back_init(&r, source + header, size - header))
            return false;
        const ZstdFseEntry* const ll_table = d->literal_lengths;
        const ZstdFseEntry* const of_table = d->offsets;
        const ZstdFseEntry* const ml_table = d->match_lengths;
        u32 ll_state = static_cast<u32>(back_read(&r, d->literal_lengths_log));
        u32 of_state = static_cast<u32>(back_read(&r, d->offsets_log));
        u32 ml_state = static_cast<u32>(back_read(&r, d->match_lengths_log));
        u32* const repeat = d->repeat;
        for (u32 i = 0; i < count; ++i) {
            const ZstdFseEntry ll_entry = ll_table[ll_state], of_entry = of_table[of_state],
                               ml_entry = ml_table[ml_state];
            const u32 of_code = of_entry.symbol;
            u32 offset_value = (1u << of_code) + static_cast<u32>(back_read(&r, of_code));
            const u32 match_length =
                kMatchLengthBase[ml_entry.symbol] + static_cast<u32>(back_read(&r, kMatchLengthBits[ml_entry.symbol]));
            const u32 literal_length = kLiteralLengthBase[ll_entry.symbol] +
                                       static_cast<u32>(back_read(&r, kLiteralLengthBits[ll_entry.symbol]));
            if (i + 1 != count) {
                ll_state = ll_entry.baseline + static_cast<u32>(back_read(&r, ll_entry.bits));
                ml_state = ml_entry.baseline + static_cast<u32>(back_read(&r, ml_entry.bits));
                of_state = of_entry.baseline + static_cast<u32>(back_read(&r, of_entry.bits));
            }
            u32 offset;
            if (offset_value > 3) {
                offset = offset_value - 3;
                repeat[2] = repeat[1];
                repeat[1] = repeat[0];
                repeat[0] = offset;
            } else {
                const u32 index = offset_value - 1 + (literal_length == 0);
                if (index == 0) {
                    offset = repeat[0];
                } else {
                    offset = index == 3 ? repeat[0] - 1 : repeat[index];
                    if (index != 1)
                        repeat[2] = repeat[1];
                    repeat[1] = repeat[0];
                    repeat[0] = offset;
                }
            }
            if (literal_length > static_cast<u32>(lit_end - lit) || literal_length > static_cast<u32>(end - op))
                return false;
            // Short runs copy 16 bytes when that many literals remain.
            if (literal_length <= 16 && lit_end - lit >= 16)
                copy16(op, lit);
            else
                copy_bytes(op, lit, literal_length);
            op += literal_length;
            lit += literal_length;
            if (match_length > static_cast<u32>(end - op) || !offset || offset > d->window_bytes ||
                offset > static_cast<u32>(op - base))
                return false;
            copy_match(op, offset, match_length);
            op += match_length;
        }
        if (r.bits != 0)
            return false;
    } else if (header != size) {
        return false;
    }
    const u32 rest = static_cast<u32>(lit_end - lit);
    if (rest > static_cast<u32>(end - op))
        return false;
    copy_bytes(op, lit, rest);
    op += rest;
    *produced = static_cast<u32>(op - output);
    return true;
}

bool ensure_storage(ZstdStreamDecoder* d, u32 required) noexcept {
    if (required <= d->storage.count)
        return true;
    if (!d->grow || required > d->maximum_storage)
        return false;
    Slice<u8> grown{};
    if (!d->grow(d->grow_context, required, &grown) || grown.data != d->storage.data || grown.count < required)
        return false;
    d->storage = grown;
    return true;
}

void start_frame_tables(ZstdStreamDecoder* d) noexcept {
    d->repeat[0] = 1;
    d->repeat[1] = 4;
    d->repeat[2] = 8;
    d->literal_lengths = d->offsets = d->match_lengths = nullptr;
    d->huffman_bits = 0;
    hash_reset(d);
}

// Frame header, or a skippable frame (in_frame stays unset). Both lie whole in the message.
ZstdError read_frame_header(ZstdStreamDecoder* d, const u8* p, u32 left, u32* consumed) noexcept {
    if (left < 4)
        return ZstdError::truncated;
    const u32 magic = load32(p);
    if ((magic & 0xFFFFFFF0u) == kSkippableMagic) {
        if (left < 8 || load32(p + 4) > left - 8)
            return ZstdError::truncated;
        *consumed = 8 + load32(p + 4);
        return ZstdError::none;
    }
    if (magic != kFrameMagic)
        return ZstdError::bad_magic;
    if (left < 5)
        return ZstdError::truncated;
    const u32 descriptor = p[4];
    const u32 size_flag = descriptor >> 6, single = (descriptor >> 5) & 1, dictionary_flag = descriptor & 3;
    if (descriptor & 8)
        return ZstdError::invalid_frame_header;
    const u32 size_bytes = size_flag == 0 ? single : 1u << size_flag;
    const u32 dictionary_bytes = dictionary_flag == 3 ? 4 : dictionary_flag;
    const u32 header = 5 + (single ? 0 : 1) + dictionary_bytes + size_bytes;
    if (left < header)
        return ZstdError::truncated;
    u32 at = 5;
    u64 window = 0;
    if (!single) {
        const u32 exponent = p[at] >> 3, mantissa = p[at] & 7;
        ++at;
        const u32 window_log = 10 + exponent;
        if (window_log > kMaximumWindowLog)
            return ZstdError::window_too_large;
        const u64 window_base = 1ull << window_log;
        window = window_base + (window_base >> 3) * mantissa;
    }
    u32 dictionary = 0;
    for (u32 i = 0; i < dictionary_bytes; ++i)
        dictionary |= static_cast<u32>(p[at + i]) << (8 * i);
    at += dictionary_bytes;
    if (dictionary)
        return ZstdError::unsupported_dictionary;
    u64 content = 0;
    for (u32 i = 0; i < size_bytes; ++i)
        content |= static_cast<u64>(p[at + i]) << (8 * i);
    if (size_bytes == 2)
        content += 256;
    if (single)
        window = content;
    if (window > kZstdMaximumWindowBytes)
        return ZstdError::window_too_large;
    d->window_bytes = static_cast<u32>(window);
    d->block_maximum = minimum(d->window_bytes, kZstdBlockMaximumBytes);
    d->content_size = content;
    d->has_content_size = size_bytes != 0;
    d->has_checksum = (descriptor >> 2) & 1;
    d->frame_output = 0;
    d->frame_start = d->position;
    d->in_frame = true;
    start_frame_tables(d);
    *consumed = header;
    return ZstdError::none;
}

ZstdDecodeResult fail(ZstdStreamDecoder* d, ZstdError error, u32 blocks) noexcept {
    d->failed = true;
    return {0, blocks, error};
}

void build_defaults(ZstdStreamDecoder* d) noexcept {
    build_table(d->default_literal_lengths, kDefaultLiteralLengths, 36, 6);
    build_table(d->default_match_lengths, kDefaultMatchLengths, 53, 6);
    build_table(d->default_offsets, kDefaultOffsets, 29, 5);
}

} // namespace

bool zstd_stream_initialize(ZstdStreamDecoder* decoder, Slice<u8> storage, u32 maximum_storage,
                            ZstdGrowStorage grow, void* context) noexcept {
    if (!decoder || !storage.data || storage.count < kZstdMessageSlackBytes || maximum_storage < storage.count)
        return false;
    reset_value(decoder);
    decoder->storage = storage;
    decoder->maximum_storage = maximum_storage;
    decoder->grow = grow;
    decoder->grow_context = context;
    build_defaults(decoder);
    decoder->initialized = true;
    return true;
}

void zstd_stream_clear(ZstdStreamDecoder* decoder) noexcept {
    if (!decoder)
        return;
    if (decoder->storage.data)
        secure_clear_bytes(decoder->storage.data, decoder->storage.count);
    secure_clear_bytes(decoder, sizeof(*decoder));
}

u32 zstd_stream_compact(ZstdStreamDecoder* d) noexcept {
    if (!d || !d->initialized)
        return 0;
    const u32 keep = d->in_frame ? minimum(d->position - d->frame_start, d->window_bytes) : 0;
    const u32 from = d->position - keep;
    if (from) {
        copy_bytes(d->storage.data, d->storage.data + from, keep);
        secure_clear_bytes(d->storage.data + keep, from);
    }
    d->position = keep;
    d->frame_start = 0;
    return keep;
}

bool zstd_stream_set_storage(ZstdStreamDecoder* d, Slice<u8> storage) noexcept {
    if (!d || storage.data != d->storage.data || storage.count < d->position + kZstdMessageSlackBytes ||
        storage.count > d->maximum_storage)
        return false;
    d->storage = storage;
    return true;
}

ZstdDecodeResult zstd_stream_decompress(ZstdStreamDecoder* d, ConstSlice<u8> input, u32 maximum_output,
                                        ConstSlice<u8>* output) noexcept {
    if (!d || !d->initialized || !output || (!input.data && input.count))
        return {0, 0, ZstdError::invalid_input};
    if (d->failed)
        return {0, 0, ZstdError::poisoned};
    if (d->position + kZstdMessageSlackBytes > d->storage.count)
        zstd_stream_compact(d);
    const u32 start = d->position;
    const u8* p = input.data;
    u32 left = input.count;
    u32 blocks = 0;
    while (left) {
        if (!d->in_frame) {
            u32 used = 0;
            const ZstdError header = read_frame_header(d, p, left, &used);
            if (header != ZstdError::none)
                return fail(d, header, blocks);
            p += used;
            left -= used;
            continue;
        }
        if (left < 3)
            return fail(d, ZstdError::truncated, blocks);
        const u32 block_header = load24(p);
        const bool last = block_header & 1;
        const u32 type = (block_header >> 1) & 3, size = block_header >> 3;
        p += 3;
        left -= 3;
        if (type == 3 || size > d->block_maximum)
            return fail(d, ZstdError::invalid_block, blocks);
        const u32 stored = type == 1 ? 1 : size;
        if (stored > left)
            return fail(d, ZstdError::truncated, blocks);
        // The block's output, a guard, then its decoded literals.
        const u32 need = 2 * d->block_maximum + kZstdLiteralGuardBytes;
        if (d->position - start > maximum_output || !ensure_storage(d, d->position + need))
            return fail(d, ZstdError::output_capacity, blocks);
        u8* const out = d->storage.data + d->position;
        u32 produced = size;
        if (type == 0) {
            copy_bytes(out, p, size);
        } else if (type == 1) {
            for (u32 i = 0; i < size; ++i)
                out[i] = p[0];
        } else {
            u8* const scratch = out + d->block_maximum + kZstdLiteralGuardBytes;
            Literals literals{};
            u32 used = 0;
            if (!decode_literals(d, p, size, scratch, &literals, &used))
                return fail(d, ZstdError::invalid_literals, blocks);
            if (!decode_sequences(d, p + used, size - used, literals, out, &produced))
                return fail(d, ZstdError::invalid_sequences, blocks);
        }
        p += stored;
        left -= stored;
        ++blocks;
        if (d->has_checksum)
            hash_update(d, out, produced);
        d->position += produced;
        d->frame_output += produced;
        if (d->position - start > maximum_output)
            return fail(d, ZstdError::output_capacity, blocks);
        if (last) {
            if (d->has_content_size && d->frame_output != d->content_size)
                return fail(d, ZstdError::content_size, blocks);
            if (d->has_checksum) {
                if (left < 4)
                    return fail(d, ZstdError::truncated, blocks);
                if (load32(p) != hash_digest(*d))
                    return fail(d, ZstdError::checksum, blocks);
                p += 4;
                left -= 4;
            }
            d->in_frame = false;
        }
    }
    *output = {d->storage.data + start, d->position - start};
    return {d->position - start, blocks, ZstdError::none};
}

} // namespace maicord
