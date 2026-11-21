#include "maicord/media/image/png_encode.hpp"

#include "maicord/core/adler32.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/crc32.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/intrinsics.hpp"
#include "maicord/sys/simd_haswell.hpp"
#include "../compression/deflate_tables.hpp"
#include "png_common.hpp"

namespace maicord {
namespace {

constexpr u32 kWindow = 32'768;
constexpr u32 kWindowMask = kWindow - 1;
// 16384 heads. Larger tables changed the output by less than 0.01% and touched more pages and cache lines.
constexpr u32 kHashBits = 14;
constexpr u32 kHashSize = 1u << kHashBits;
constexpr u32 kBlockBytes = 65'535;
constexpr u32 kMinMatch = 4;
constexpr u32 kMaxMatch = 258;
// Match search effort: the hash chain candidates tried for each position.
constexpr u32 kMaxChain = 12;
constexpr u32 kNone = ~0u;
// A block's sequences, one u32 each: the literals before a match (9 bits), the match length minus kMinMatch (8 bits)
// and its distance minus one (15 bits). The literals come from the input. A sequence with the length field kNoMatch
// has no match: it carries kMaxRun literals or the literals at the end of the block.
constexpr u32 kMaxRun = 511;
constexpr u32 kNoMatch = 255;
// Every sequence but the last one in a block covers at least kMinMatch input bytes.
constexpr u32 kSequenceCapacity = (kBlockBytes - 1) / kMinMatch + 1;
static_assert((kHashSize + kWindow + kSequenceCapacity) * 4u == kZlibCompressWorkspaceBytes,
              "the workspace holds the heads, the window's links and one block's sequences");
constexpr u32 kEndOfBlock = 256;
constexpr u32 kLiteralSymbols = 286;
constexpr u32 kDistanceSymbols = 30;

// LSB-first bit packer. It counts the bytes past capacity and does not write them. It holds fewer than 32 bits
// between calls and takes at most 32 bits in one call.
struct Bits {
    u8* out;
    u64 capacity;
    u64 at;
    u64 buffer;
    u32 count;

    __forceinline void put(u32 value, u32 bits) noexcept {
        buffer |= static_cast<u64>(value) << count;
        count += bits;
        if (count >= 32) {
            if (at + 4 <= capacity)
                byte_store_le32(out + at, static_cast<u32>(buffer));
            at += 4;
            buffer >>= 32;
            count -= 32;
        }
    }
    // put out of line, for the block headers and the ends of the stream: an inline put costs about 40 bytes.
    __declspec(noinline) void write(u32 value, u32 bits) noexcept {
        put(value, bits);
    }
    // Pads to a byte boundary with zero bits and writes every held byte.
    void align() noexcept {
        for (count = (count + 7) & ~7u; count; count -= 8) {
            if (at < capacity)
                out[at] = static_cast<u8>(buffer);
            ++at;
            buffer >>= 8;
        }
    }
};

// Length-limited Huffman code lengths: Moffat-Katajainen minimum redundancy
// over the used symbols sorted by frequency, then Kraft repair to `limit`.
void huffman_lengths(const u32* frequency, u32 symbols, u32 limit, u8* lengths) noexcept {
    struct Entry {
        u32 key;
        u32 symbol;
    } e[288];
    i32 used = 0;
    for (u32 s = 0; s < symbols; ++s) {
        lengths[s] = 0;
        if (frequency[s])
            e[used++] = {frequency[s], s};
    }
    if (!used)
        return;
    if (used == 1) {
        lengths[e[0].symbol] = 1;
        return;
    }
    // Shell sort with Ciura's gaps: a plain insertion sort of up to 286 symbols costs too much in each block.
    constexpr i32 kGaps[]{132, 57, 23, 10, 4, 1};
    for (const i32 gap : kGaps)
        for (i32 i = gap; i < used; ++i) {
            const Entry v = e[i];
            i32 j = i;
            while (j >= gap && e[j - gap].key > v.key) {
                e[j] = e[j - gap];
                j -= gap;
            }
            e[j] = v;
        }
    e[0].key += e[1].key;
    i32 root = 0, leaf = 2, next = 1;
    for (; next < used - 1; ++next) {
        if (leaf >= used || e[root].key < e[leaf].key) {
            e[next].key = e[root].key;
            e[root++].key = static_cast<u32>(next);
        } else {
            e[next].key = e[leaf++].key;
        }
        if (leaf >= used || (root < next && e[root].key < e[leaf].key)) {
            e[next].key += e[root].key;
            e[root++].key = static_cast<u32>(next);
        } else {
            e[next].key += e[leaf++].key;
        }
    }
    e[used - 2].key = 0;
    for (i32 i = used - 3; i >= 0; --i)
        e[i].key = e[e[i].key].key + 1;
    i32 available = 1, taken = 0;
    u32 depth = 0;
    root = used - 2;
    next = used - 1;
    while (available > 0) {
        while (root >= 0 && e[root].key == depth) {
            ++taken;
            --root;
        }
        while (available > taken) {
            e[next--].key = depth;
            --available;
        }
        available = 2 * taken;
        ++depth;
        taken = 0;
    }
    u32 count[33]{};
    for (i32 i = 0; i < used; ++i)
        ++count[minimum(e[i].key, 32u)];
    for (u32 i = limit + 1; i <= 32; ++i) {
        count[limit] += count[i];
        count[i] = 0;
    }
    u32 total = 0;
    for (u32 i = 1; i <= limit; ++i)
        total += count[i] << (limit - i);
    while (total != 1u << limit) {
        --count[limit];
        for (u32 i = limit - 1; i; --i)
            if (count[i]) {
                --count[i];
                count[i + 1] += 2;
                break;
            }
        --total;
    }
    i32 k = 0;
    for (u32 length = limit; length >= 1; --length)
        for (u32 j = 0; j < count[length]; ++j)
            lengths[e[k++].symbol] = static_cast<u8>(length);
}

// Canonical codes, bit-reversed for the LSB-first packer.
void canonical(const u8* lengths, u32 symbols, u16* codes) noexcept {
    u32 count[16]{}, next[16]{};
    for (u32 s = 0; s < symbols; ++s)
        ++count[lengths[s]];
    count[0] = 0;
    u32 code = 0;
    for (u32 bits = 1; bits < 16; ++bits) {
        code = (code + count[bits - 1]) << 1;
        next[bits] = code;
    }
    // An unused symbol (length 0) gets code 0: the shift drops all 16 bits.
    for (u32 s = 0; s < symbols; ++s) {
        const u32 length = lengths[s];
        codes[s] = static_cast<u16>(__builtin_bitreverse16(static_cast<u16>(next[length]++)) >> (16 - length));
    }
}

// Every tree gets at least two used symbols so it is complete for any inflater.
void complete(u32* frequency, u32 symbols) noexcept {
    u32 used = 0, last = 0;
    for (u32 s = 0; s < symbols; ++s)
        if (frequency[s]) {
            ++used;
            last = s;
        }
    if (used == 0)
        frequency[0] = frequency[1] = 1;
    else if (used == 1)
        frequency[last == 0 ? 1 : 0] = 1;
}

struct Tree {
    u8 literal_lengths[288];
    u8 distance_lengths[32];
    u16 literal_codes[288];
    u16 distance_codes[32];
};

struct Header {
    u8 code_lengths[19];
    u16 code_codes[19];
    u8 rle[kLiteralSymbols + kDistanceSymbols];
    u8 rle_extra[kLiteralSymbols + kDistanceSymbols];
    u32 rle_count, hlit, hdist, hclen;
    u64 bits;
};

void rle_push(Header* h, u32 symbol, u32 extra) noexcept {
    h->rle[h->rle_count] = static_cast<u8>(symbol);
    h->rle_extra[h->rle_count++] = static_cast<u8>(extra);
}

// Dynamic tree for the block's frequencies and the exact header size. The frequencies are completed in place: an
// added symbol only raises the block's estimated size.
void build_dynamic(u32* literal_frequency, u32* distance_frequency, Tree* tree, Header* h) noexcept {
    complete(literal_frequency, kLiteralSymbols);
    complete(distance_frequency, kDistanceSymbols);
    huffman_lengths(literal_frequency, kLiteralSymbols, 15, tree->literal_lengths);
    huffman_lengths(distance_frequency, kDistanceSymbols, 15, tree->distance_lengths);
    tree->literal_lengths[286] = tree->literal_lengths[287] = 0;
    tree->distance_lengths[30] = tree->distance_lengths[31] = 0;
    canonical(tree->literal_lengths, 288, tree->literal_codes);
    canonical(tree->distance_lengths, 32, tree->distance_codes);
    h->hlit = kLiteralSymbols;
    while (h->hlit > 257 && !tree->literal_lengths[h->hlit - 1])
        --h->hlit;
    h->hdist = kDistanceSymbols;
    while (h->hdist > 1 && !tree->distance_lengths[h->hdist - 1])
        --h->hdist;
    u8 sequence[kLiteralSymbols + kDistanceSymbols];
    const u32 total = h->hlit + h->hdist;
    copy_bytes(sequence, tree->literal_lengths, h->hlit);
    copy_bytes(sequence + h->hlit, tree->distance_lengths, h->hdist);
    // Runs: 16 repeats the previous length 3-6 times, 17/18 zeros 3-10/11-138.
    h->rle_count = 0;
    for (u32 i = 0; i < total;) {
        const u8 value = sequence[i];
        u32 run = 1;
        while (i + run < total && sequence[i + run] == value)
            ++run;
        i += run;
        if (!value) {
            while (run >= 11) {
                const u32 r = minimum(run, 138u);
                rle_push(h, 18, r - 11);
                run -= r;
            }
            if (run >= 3) {
                rle_push(h, 17, run - 3);
                run = 0;
            }
            while (run--)
                rle_push(h, 0, 0);
            continue;
        }
        rle_push(h, value, 0);
        --run;
        while (run >= 3) {
            const u32 r = minimum(run, 6u);
            rle_push(h, 16, r - 3);
            run -= r;
        }
        while (run--)
            rle_push(h, value, 0);
    }
    u32 cf[19]{};
    for (u32 i = 0; i < h->rle_count; ++i)
        ++cf[h->rle[i]];
    complete(cf, 19);
    huffman_lengths(cf, 19, 7, h->code_lengths);
    canonical(h->code_lengths, 19, h->code_codes);
    h->hclen = 19;
    while (h->hclen > 4 && !h->code_lengths[kDeflateCodeLengthOrder[h->hclen - 1]])
        --h->hclen;
    h->bits = 3 + 5 + 5 + 4 + 3ull * h->hclen;
    for (u32 i = 0; i < h->rle_count; ++i) {
        const u32 s = h->rle[i];
        h->bits += h->code_lengths[s] + (s == 16 ? 2 : s == 17 ? 3 : s == 18 ? 7 : 0);
    }
}

struct Deflate {
    const u8* in;
    u32* sequences;
    Bits bits;
};

// The hash chains over the input. No pointer to a Matcher leaves zlib_compress, so its fields stay in registers.
struct Matcher {
    const u8* in;
    u32* head;
    u32* prev;
    u32 n, inserted, shift;
};

// The index of the last entry of kDeflateLengthBase that is not above a match length of 3 to 258.
constexpr u32 length_code(u32 length) noexcept {
    const u32 m = length - 3;
    if (m < 8)
        return m;
    if (length == kMaxMatch)
        return 28;
    const u32 top = 31 - static_cast<u32>(__builtin_clz(m));
    return 4 * (top - 1) + ((m >> (top - 2)) & 3);
}
// The index of the last entry of kDeflateDistanceBase that is not above a distance of 1 to 32768.
constexpr u32 distance_code(u32 distance) noexcept {
    const u32 m = distance - 1;
    if (m < 4)
        return m;
    const u32 top = 31 - static_cast<u32>(__builtin_clz(m));
    return 2 * top + ((m >> (top - 1)) & 1);
}
static_assert(
    [] {
        for (u32 i = 0; i != 29; ++i)
            if (length_code(kDeflateLengthBase[i]) != i || (i != 28 && length_code(kDeflateLengthBase[i + 1] - 1u) != i))
                return false;
        for (u32 i = 0; i != 30; ++i)
            if (distance_code(kDeflateDistanceBase[i]) != i ||
                distance_code(i == 29 ? kWindow : kDeflateDistanceBase[i + 1] - 1u) != i)
                return false;
        return true;
    }(),
    "the code formulas agree with the base tables");

__forceinline u32 hash(const u8* p, u32 shift) noexcept {
    return (byte_load_le32(p) * 0x9E37'79B1u) >> shift;
}

__forceinline void insert_until(Matcher* m, u32 position) noexcept {
    for (; m->inserted < position; ++m->inserted)
        if (m->inserted + 4 <= m->n) {
            const u32 h = hash(m->in + m->inserted, m->shift);
            m->prev[m->inserted & kWindowMask] = m->head[h];
            m->head[h] = m->inserted;
        }
}

// The count of equal bytes at a and b, at most limit.
__forceinline u32 match_length(const u8* a, const u8* b, u32 limit) noexcept {
    u32 length = 0;
    for (; length + 8 <= limit; length += 8)
        if (const u64 difference = byte_load_le64(a + length) ^ byte_load_le64(b + length))
            return length + lowest_set_bit(difference) / 8;
    while (length < limit && a[length] == b[length])
        ++length;
    return length;
}

// Longest match for `position` that ends by `end`; zero below kMinMatch.
__forceinline u32 find_match(const Matcher& m, u32 position, u32 end, u32* distance) noexcept {
    const u32 longest = minimum(kMaxMatch, end - position);
    if (longest < kMinMatch)
        return 0;
    const u8* b = m.in + position;
    const u32 limit = position > kWindow ? position - kWindow : 0;
    u32 candidate = m.head[hash(b, m.shift)], best = kMinMatch - 1, chain = kMaxChain;
    while (candidate != kNone && candidate >= limit && chain--) {
        const u8* a = m.in + candidate;
        // A longer match also has the byte after the best length: most candidates fail this one compare.
        if (a[best] == b[best]) {
            const u32 length = match_length(a, b, longest);
            if (length > best) {
                best = length;
                *distance = position - candidate;
                if (length == longest)
                    break;
            }
        }
        const u32 next = m.prev[candidate & kWindowMask];
        if (next >= candidate)
            break;
        candidate = next;
    }
    return best >= kMinMatch ? best : 0;
}

// The block's `count` sequences and the end of block. `at` is the block's first input byte.
void write_sequences(Deflate* d, const u8* at, u32 count, const Tree& tree) noexcept {
    for (u32 i = 0; i < count; ++i) {
        const u32 sequence = d->sequences[i];
        for (u32 run = sequence >> 23; run; --run, ++at)
            d->bits.put(tree.literal_codes[*at], tree.literal_lengths[*at]);
        if ((sequence >> 15 & 0xFFu) == kNoMatch)
            continue;
        const u32 length = (sequence >> 15 & 0xFFu) + kMinMatch;
        const u32 distance = (sequence & 0x7FFFu) + 1;
        // A code and its extra bits go out in one put: at most 15 + 5 and 15 + 13 bits.
        const u32 lc = length_code(length), length_bits = tree.literal_lengths[257 + lc];
        d->bits.put(tree.literal_codes[257 + lc] | (length - kDeflateLengthBase[lc]) << length_bits,
                    length_bits + kDeflateLengthExtra[lc]);
        const u32 dc = distance_code(distance), distance_bits = tree.distance_lengths[dc];
        d->bits.put(tree.distance_codes[dc] | (distance - kDeflateDistanceBase[dc]) << distance_bits,
                    distance_bits + kDeflateDistanceExtra[dc]);
        at += length;
    }
    d->bits.write(tree.literal_codes[kEndOfBlock], tree.literal_lengths[kEndOfBlock]);
}

// The code length of a literal or length symbol in the fixed Huffman code.
constexpr u32 fixed_length(u32 symbol) noexcept {
    return symbol < 144 ? 8 : symbol < 256 ? 9 : symbol < 280 ? 7 : 8;
}

// The smallest of stored, fixed and dynamic encodings of input [start, end).
void emit_block(Deflate* d, u32 start, u32 end, u32 count, u32* literal_frequency, u32* distance_frequency,
                bool final) noexcept {
    Tree tree;
    Header header;
    build_dynamic(literal_frequency, distance_frequency, &tree, &header);
    u64 extra_bits = 0;
    for (u32 c = 0; c < 29; ++c)
        extra_bits += static_cast<u64>(literal_frequency[257 + c]) * kDeflateLengthExtra[c];
    for (u32 c = 0; c < kDistanceSymbols; ++c)
        extra_bits += static_cast<u64>(distance_frequency[c]) * kDeflateDistanceExtra[c];
    u64 dynamic_bits = header.bits + extra_bits, fixed_bits = 3 + extra_bits;
    for (u32 s = 0; s < kLiteralSymbols; ++s) {
        dynamic_bits += static_cast<u64>(literal_frequency[s]) * tree.literal_lengths[s];
        fixed_bits += static_cast<u64>(literal_frequency[s]) * fixed_length(s);
    }
    for (u32 s = 0; s < kDistanceSymbols; ++s) {
        dynamic_bits += static_cast<u64>(distance_frequency[s]) * tree.distance_lengths[s];
        fixed_bits += static_cast<u64>(distance_frequency[s]) * 5u;
    }
    const u32 raw = end - start;
    const u64 stored_bits = 3 + (8 - (d->bits.count + 3) % 8) % 8 + 32 + 8ull * raw;
    if (stored_bits <= fixed_bits && stored_bits <= dynamic_bits) {
        d->bits.write(final ? 1 : 0, 3);
        d->bits.align();
        d->bits.write(raw | (raw ^ 0xFFFFu) << 16, 32); // LEN, then NLEN: its one's complement
        Bits& b = d->bits;
        if (b.at <= b.capacity && raw <= b.capacity - b.at)
            copy_bytes(b.out + b.at, d->in + start, raw);
        b.at += raw;
        return;
    }
    if (fixed_bits <= dynamic_bits) {
        d->bits.write(final ? 3 : 2, 3);
        for (u32 s = 0; s < 288; ++s)
            tree.literal_lengths[s] = static_cast<u8>(fixed_length(s));
        for (u32 s = 0; s < 32; ++s)
            tree.distance_lengths[s] = 5;
        canonical(tree.literal_lengths, 288, tree.literal_codes);
        canonical(tree.distance_lengths, 32, tree.distance_codes);
        write_sequences(d, d->in + start, count, tree);
        return;
    }
    d->bits.write(final ? 5 : 4, 3);
    d->bits.write(header.hlit - 257, 5);
    d->bits.write(header.hdist - 1, 5);
    d->bits.write(header.hclen - 4, 4);
    for (u32 i = 0; i < header.hclen; ++i)
        d->bits.write(header.code_lengths[kDeflateCodeLengthOrder[i]], 3);
    for (u32 i = 0; i < header.rle_count; ++i) {
        const u32 s = header.rle[i];
        d->bits.write(header.code_codes[s], header.code_lengths[s]);
        if (s >= 16)
            d->bits.write(header.rle_extra[i], s == 16 ? 2 : s == 17 ? 3 : 7);
    }
    write_sequences(d, d->in + start, count, tree);
}

MAICORD_HASWELL __forceinline __m256i byte_distance(__m256i x, __m256i y) noexcept {
    return _mm256_or_si256(_mm256_subs_epu8(x, y), _mm256_subs_epu8(y, x));
}
// All ones in each byte where x is not above y, without a sign.
MAICORD_HASWELL __forceinline __m256i not_above(__m256i x, __m256i y) noexcept {
    return _mm256_cmpeq_epi8(_mm256_min_epu8(x, y), x);
}
MAICORD_HASWELL __forceinline __m256i choose(__m256i mask, __m256i yes, __m256i no) noexcept {
    return _mm256_xor_si256(no, _mm256_and_si256(_mm256_xor_si256(yes, no), mask));
}

// Writes the length, type and CRC of the chunk at `at`, whose `bytes` of data are already in place after its
// header. Returns the chunk's end.
u8* close_chunk(u8* at, const char* type, u32 bytes) noexcept {
    byte_store_be32(at, bytes);
    copy_bytes(at + 4, type, 4);
    byte_store_be32(at + 8 + bytes, crc32_extend(0, at + 4, 4 + bytes));
    return at + 12 + bytes;
}

} // namespace

u64 zlib_compress_bound(u64 input_bytes) noexcept {
    const u64 blocks = input_bytes ? (input_bytes + kBlockBytes - 1) / kBlockBytes : 1;
    return 2 + input_bytes + 5 * blocks + 4;
}

u32 zlib_compress(ConstSlice<u8> input, Slice<u8> workspace, Slice<u8> output) noexcept {
    if (!byte_range_valid(input) || !workspace.data || workspace.count < kZlibCompressWorkspaceBytes ||
        (reinterpret_cast<usize>(workspace.data) & 3u) || !output.data || input.count > 0x7FFF'0000u ||
        byte_ranges_overlap(input, {output.data, output.count}) ||
        byte_ranges_overlap({workspace.data, workspace.count}, {output.data, output.count}))
        return 0;
    u32* const head = reinterpret_cast<u32*>(workspace.data);
    u32* const sequences = head + kHashSize + kWindow;
    const u8* const in = input.data;
    const u32 n = input.count;
    // A small input gets a small table, about one head for every four bytes: fewer pages to clear and touch.
    u32 bits = 10;
    while (bits < kHashBits && (4u << bits) < n)
        ++bits;
    Matcher m{in, head, head + kHashSize, n, 0, 32 - bits};
    for (u32 i = 0; i < 1u << bits; ++i)
        head[i] = kNone;
    Deflate d{in, sequences, {output.data, output.count, 0, 0, 0}};
    // CMF 0x78 (deflate, 32 KiB window), FLG 0x9C (default level, check bits).
    d.bits.write(0x9C78, 16);
    u32 position = 0;
    do {
        const u32 start = position, end = minimum(n, position + kBlockBytes);
        u32 literal_frequency[kLiteralSymbols]{}, distance_frequency[kDistanceSymbols]{}, count = 0, run = 0;
        while (position < end) {
            insert_until(&m, position);
            u32 distance = 0;
            const u32 length = find_match(m, position, end, &distance);
            if (!length) {
                ++literal_frequency[in[position++]];
                if (++run == kMaxRun) {
                    sequences[count++] = kMaxRun << 23 | kNoMatch << 15;
                    run = 0;
                }
                continue;
            }
            sequences[count++] = run << 23 | (length - kMinMatch) << 15 | (distance - 1);
            run = 0;
            ++literal_frequency[257 + length_code(length)];
            ++distance_frequency[distance_code(distance)];
            position += length;
        }
        if (run)
            sequences[count++] = run << 23 | kNoMatch << 15;
        literal_frequency[kEndOfBlock] = 1;
        emit_block(&d, start, position, count, literal_frequency, distance_frequency, position == n);
    } while (position < n);
    d.bits.align();
    // The packer writes the low byte first and Adler-32 goes high byte first.
    d.bits.write(byte_swap(adler32_update(1u, {in, n})), 32);
    return d.bits.at <= output.count ? static_cast<u32>(d.bits.at) : 0;
}

u64 png_encode_bound(u32 width, u32 height) noexcept {
    if (!width || !height || width > kPngEncodeDimensionMaximum || height > kPngEncodeDimensionMaximum)
        return 0;
    const u64 raw = static_cast<u64>(height) * (1ull + static_cast<u64>(width) * 4u);
    return kPngFrameBytes + zlib_compress_bound(raw);
}

u64 png_encode_workspace_bytes(u32 width, u32 height) noexcept {
    if (!width || !height || width > kPngEncodeDimensionMaximum || height > kPngEncodeDimensionMaximum)
        return 0;
    // The filtered stream, two rows with 32 zero bytes before each, and the five candidates of one row.
    const u64 row = static_cast<u64>(width) * 4u;
    return kZlibCompressWorkspaceBytes + static_cast<u64>(height) * (1ull + row) + 2 * (row + 32) + 5 * row;
}

MAICORD_HASWELL void png_filter_row(const u8* row, const u8* above, u32 bytes, u32 bpp, u8* candidates,
                                    u8* out) noexcept {
    const __m256i zero = _mm256_setzero_si256(), one = _mm256_set1_epi8(1);
    __m256i sums[5]{zero, zero, zero, zero, zero};
    u32 x = 0;
    for (; x + 32 <= bytes; x += 32) {
        const __m256i current = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(row + x));
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(row + x - bpp));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(above + x));
        const __m256i c = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(above + x - bpp));
        const __m256i average = _mm256_sub_epi8(_mm256_avg_epu8(a, b), _mm256_and_si256(_mm256_xor_si256(a, b), one));
        // Paeth: pa = |b - c|, pb = |a - c|, pc = |(a - c) + (b - c)|. With two steps of one sign pc is their sum
        // (255 when it is more: no pa or pb is above that). Else pc is the distance between pa and pb.
        const __m256i pa = byte_distance(b, c), pb = byte_distance(a, c);
        const __m256i opposite = _mm256_xor_si256(not_above(c, a), not_above(c, b));
        const __m256i pc = choose(opposite, byte_distance(pa, pb), _mm256_adds_epu8(pa, pb));
        const __m256i paeth =
            choose(_mm256_and_si256(not_above(pa, pb), not_above(pa, pc)), a, choose(not_above(pb, pc), b, c));
        const __m256i residuals[5]{current, _mm256_sub_epi8(current, a), _mm256_sub_epi8(current, b),
                                   _mm256_sub_epi8(current, average), _mm256_sub_epi8(current, paeth)};
        for (u32 f = 0; f != 5; ++f) {
            _mm256_storeu_si256(reinterpret_cast<__m256i_u*>(candidates + u64{f} * bytes + x), residuals[f]);
            sums[f] = _mm256_add_epi64(sums[f], _mm256_sad_epu8(_mm256_abs_epi8(residuals[f]), zero));
        }
    }
    u64 cost[5];
    for (u32 f = 0; f != 5; ++f) {
        u64 lanes[4];
        _mm256_storeu_si256(reinterpret_cast<__m256i_u*>(lanes), sums[f]);
        cost[f] = lanes[0] + lanes[1] + lanes[2] + lanes[3];
    }
    for (; x < bytes; ++x) {
        const u8 a = (row + x)[-static_cast<i32>(bpp)], b = above[x], c = (above + x)[-static_cast<i32>(bpp)];
        const i32 p = i32{a} + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
        const u8 predicted[5]{0, a, b, static_cast<u8>((u32{a} + b) >> 1), pa <= pb && pa <= pc ? a : pb <= pc ? b : c};
        for (u32 f = 0; f != 5; ++f) {
            const u8 residual = static_cast<u8>(row[x] - predicted[f]);
            candidates[u64{f} * bytes + x] = residual;
            cost[f] += residual < 128 ? residual : 256u - residual;
        }
    }
    u8 best = 0;
    for (u8 f = 1; f < 5; ++f)
        if (cost[f] < cost[best])
            best = f;
    out[0] = best;
    copy_bytes(out + 1, candidates + u64{best} * bytes, bytes);
}

u8* png_write_header(u8* out, u32 width, u32 height, u8 color_type) noexcept {
    copy_bytes(out, kPngSignature, 8);
    u8* ihdr = out + 8;
    byte_store_be32(ihdr + 8, width);
    byte_store_be32(ihdr + 12, height);
    ihdr[16] = 8;
    ihdr[17] = color_type;
    ihdr[18] = ihdr[19] = ihdr[20] = 0;
    return close_chunk(ihdr, "IHDR", 13);
}

u8* png_write_end(u8* idat, u32 zlib_bytes) noexcept {
    return close_chunk(close_chunk(idat, "IDAT", zlib_bytes), "IEND", 0);
}

u32 png_encode_filtered(ConstSlice<u8> filtered, u32 width, u32 height, u8 color_type, Slice<u8> workspace,
                        Slice<u8> output) noexcept {
    if (!output.data || output.count < kPngFrameBytes)
        return 0;
    u8* idat = png_write_header(output.data, width, height, color_type);
    const u32 zlib = zlib_compress(filtered, workspace, {idat + 8, output.count - kPngFrameBytes});
    return zlib ? static_cast<u32>(png_write_end(idat, zlib) - output.data) : 0;
}

u32 png_encode_rgba8(const u8* rgba, u32 width, u32 height, Slice<u8> workspace, Slice<u8> output) noexcept {
    const u64 needed = png_encode_workspace_bytes(width, height);
    const u64 pixels = static_cast<u64>(width) * height;
    if (!rgba || !needed || !workspace.data || workspace.count < needed || (reinterpret_cast<usize>(workspace.data) & 3u))
        return 0;
    bool opaque = true;
    for (u64 i = 0; i < pixels && opaque; ++i)
        opaque = rgba[i * 4 + 3] == 255;
    const u32 bpp = opaque ? 3 : 4, row_bytes = width * bpp, room = width * 4 + 32;
    const u32 filtered_bytes = height * (1 + row_bytes);
    u8* filtered = workspace.data + kZlibCompressWorkspaceBytes;
    u8* rows = filtered + filtered_bytes;
    // The second row starts zero: the row above the first one.
    clear_bytes(rows, 2 * room);
    for (u32 y = 0; y < height; ++y) {
        const u8* source = rgba + static_cast<u64>(y) * width * 4;
        u8* current = rows + 32 + (y & 1) * room;
        if (opaque)
            // Four bytes go out for each pixel: the next pixel writes over the fourth, and the row has room for
            // the fourth byte of the last pixel.
            for (u32 x = 0; x < width; ++x)
                byte_store_le32(current + x * 3, byte_load_le32(source + x * 4));
        else
            copy_bytes(current, source, row_bytes);
        png_filter_row(current, rows + 32 + (~y & 1) * room, row_bytes, bpp, rows + 2 * room,
                       filtered + static_cast<u64>(y) * (1 + row_bytes));
    }
    return png_encode_filtered({filtered, filtered_bytes}, width, height, opaque ? 2 : 6,
                               {workspace.data, kZlibCompressWorkspaceBytes}, output);
}

} // namespace maicord
