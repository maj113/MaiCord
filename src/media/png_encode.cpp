#include "maicord/media/png_encode.hpp"

#include "maicord/core/byte_order.hpp"
#include "maicord/core/byte_range.hpp"
#include "png_common.hpp"

namespace maicord {
namespace {

constexpr u32 kWindow = 32'768;
constexpr u32 kWindowMask = kWindow - 1;
constexpr u32 kHashSize = 65'536;
constexpr u32 kBlockBytes = 65'535;
constexpr u32 kMinMatch = 3;
constexpr u32 kMaxMatch = 258;
// Match search effort: hash chain length, and the match length below which a
// lazy match is tried.
constexpr u32 kMaxChain = 48;
constexpr u32 kLazyLength = 32;
constexpr u32 kNiceLength = 160;
constexpr u32 kNone = ~0u;
constexpr u32 kMatchFlag = 0x8000'0000u;
constexpr u32 kEndOfBlock = 256;
constexpr u32 kLiteralSymbols = 286;
constexpr u32 kDistanceSymbols = 30;

constexpr u16 kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr u8 kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr u16 kDistanceBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                   193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr u8 kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr u8 kCodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// LSB-first bit packer; bytes past capacity are counted, not written.
struct Bits {
    u8* out;
    u64 capacity;
    u64 at;
    u64 buffer;
    u32 count;

    void put(u32 value, u32 bits) noexcept {
        buffer |= static_cast<u64>(value) << count;
        count += bits;
        while (count >= 8) {
            if (at < capacity)
                out[at] = static_cast<u8>(buffer);
            ++at;
            buffer >>= 8;
            count -= 8;
        }
    }
    void align() noexcept {
        if (count)
            put(0, 8 - count);
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
    for (i32 i = 1; i < used; ++i) {
        const Entry v = e[i];
        i32 j = i;
        while (j && e[j - 1].key > v.key) {
            e[j] = e[j - 1];
            --j;
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
    for (u32 s = 0; s < symbols; ++s) {
        const u32 length = lengths[s];
        if (!length) {
            codes[s] = 0;
            continue;
        }
        const u32 value = next[length]++;
        u32 reversed = 0;
        for (u32 b = 0; b < length; ++b)
            reversed |= (value >> b & 1u) << (length - 1 - b);
        codes[s] = static_cast<u16>(reversed);
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

// Dynamic tree for the block's frequencies and the exact header size.
void build_dynamic(const u32* literal_frequency, const u32* distance_frequency, Tree* tree, Header* h) noexcept {
    u32 lf[288]{}, df[32]{};
    copy_bytes(lf, literal_frequency, kLiteralSymbols * 4u);
    copy_bytes(df, distance_frequency, kDistanceSymbols * 4u);
    complete(lf, kLiteralSymbols);
    complete(df, kDistanceSymbols);
    huffman_lengths(lf, kLiteralSymbols, 15, tree->literal_lengths);
    huffman_lengths(df, kDistanceSymbols, 15, tree->distance_lengths);
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
    while (h->hclen > 4 && !h->code_lengths[kCodeLengthOrder[h->hclen - 1]])
        --h->hclen;
    h->bits = 3 + 5 + 5 + 4 + 3ull * h->hclen;
    for (u32 i = 0; i < h->rle_count; ++i) {
        const u32 s = h->rle[i];
        h->bits += h->code_lengths[s] + (s == 16 ? 2 : s == 17 ? 3 : s == 18 ? 7 : 0);
    }
}

struct Deflate {
    const u8* in;
    u32 n;
    u32 inserted;
    u32* head;
    u32* prev;
    u32* symbols;
    u8 length_code[kMaxMatch + 1];
    u8 distance_code[512];
    Tree fixed;
    Bits bits;
};

u32 distance_symbol(const Deflate& d, u32 distance) noexcept {
    const u32 v = distance - 1;
    return d.distance_code[v < 256 ? v : 256 + (v >> 7)];
}

u32 hash3(const u8* p) noexcept {
    const u32 v = static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16;
    return (v * 0x9E37'79B1u) >> 16;
}

void insert_until(Deflate* d, u32 position) noexcept {
    for (; d->inserted < position; ++d->inserted)
        if (d->inserted + 2 < d->n) {
            const u32 h = hash3(d->in + d->inserted);
            d->prev[d->inserted & kWindowMask] = d->head[h];
            d->head[h] = d->inserted;
        }
}

// Longest match for `position` that ends by `end`; zero below kMinMatch.
u32 find_match(const Deflate& d, u32 position, u32 end, u32* distance) noexcept {
    const u32 longest = minimum(kMaxMatch, end - position);
    if (longest < kMinMatch)
        return 0;
    const u8* b = d.in + position;
    const u32 limit = position > kWindow ? position - kWindow : 0;
    u32 candidate = d.head[hash3(b)], best = kMinMatch - 1, chain = kMaxChain;
    while (candidate != kNone && candidate >= limit && chain--) {
        const u8* a = d.in + candidate;
        if (a[best] == b[best] && a[0] == b[0] && a[1] == b[1] && a[2] == b[2]) {
            u32 length = 3;
            while (length < longest && a[length] == b[length])
                ++length;
            if (length > best) {
                best = length;
                *distance = position - candidate;
                if (length >= longest || length >= kNiceLength)
                    break;
            }
        }
        const u32 next = d.prev[candidate & kWindowMask];
        if (next >= candidate)
            break;
        candidate = next;
    }
    return best >= kMinMatch ? best : 0;
}

void write_symbols(Deflate* d, u32 count, const Tree& tree) noexcept {
    for (u32 i = 0; i < count; ++i) {
        const u32 symbol = d->symbols[i];
        if (!(symbol & kMatchFlag)) {
            d->bits.put(tree.literal_codes[symbol], tree.literal_lengths[symbol]);
            continue;
        }
        const u32 length = (symbol >> 16 & 0xFFu) + kMinMatch;
        const u32 distance = (symbol & 0x7FFFu) + 1;
        const u32 lc = d->length_code[length];
        d->bits.put(tree.literal_codes[257 + lc], tree.literal_lengths[257 + lc]);
        if (kLengthExtra[lc])
            d->bits.put(length - kLengthBase[lc], kLengthExtra[lc]);
        const u32 dc = distance_symbol(*d, distance);
        d->bits.put(tree.distance_codes[dc], tree.distance_lengths[dc]);
        if (kDistanceExtra[dc])
            d->bits.put(distance - kDistanceBase[dc], kDistanceExtra[dc]);
    }
    d->bits.put(tree.literal_codes[kEndOfBlock], tree.literal_lengths[kEndOfBlock]);
}

// The smallest of stored, fixed and dynamic encodings of input [start, end).
void emit_block(Deflate* d, u32 start, u32 end, u32 count, const u32* literal_frequency,
                const u32* distance_frequency, u64 extra_bits, bool final) noexcept {
    Tree dynamic;
    Header header;
    build_dynamic(literal_frequency, distance_frequency, &dynamic, &header);
    u64 dynamic_bits = header.bits + extra_bits, fixed_bits = 3 + extra_bits;
    for (u32 s = 0; s < kLiteralSymbols; ++s) {
        dynamic_bits += static_cast<u64>(literal_frequency[s]) * dynamic.literal_lengths[s];
        fixed_bits += static_cast<u64>(literal_frequency[s]) * d->fixed.literal_lengths[s];
    }
    for (u32 s = 0; s < kDistanceSymbols; ++s) {
        dynamic_bits += static_cast<u64>(distance_frequency[s]) * dynamic.distance_lengths[s];
        fixed_bits += static_cast<u64>(distance_frequency[s]) * 5u;
    }
    const u32 raw = end - start;
    const u64 stored_bits = 3 + (8 - (d->bits.count + 3) % 8) % 8 + 32 + 8ull * raw;
    if (stored_bits <= fixed_bits && stored_bits <= dynamic_bits) {
        d->bits.put(final ? 1 : 0, 3);
        d->bits.align();
        d->bits.put(raw & 0xFFFFu, 16);
        d->bits.put(~raw & 0xFFFFu, 16);
        Bits& b = d->bits;
        if (b.at <= b.capacity && raw <= b.capacity - b.at)
            copy_bytes(b.out + b.at, d->in + start, raw);
        b.at += raw;
        return;
    }
    if (fixed_bits <= dynamic_bits) {
        d->bits.put(final ? 3 : 2, 3);
        write_symbols(d, count, d->fixed);
        return;
    }
    d->bits.put(final ? 5 : 4, 3);
    d->bits.put(header.hlit - 257, 5);
    d->bits.put(header.hdist - 1, 5);
    d->bits.put(header.hclen - 4, 4);
    for (u32 i = 0; i < header.hclen; ++i)
        d->bits.put(header.code_lengths[kCodeLengthOrder[i]], 3);
    for (u32 i = 0; i < header.rle_count; ++i) {
        const u32 s = header.rle[i];
        d->bits.put(header.code_codes[s], header.code_lengths[s]);
        if (s >= 16)
            d->bits.put(header.rle_extra[i], s == 16 ? 2 : s == 17 ? 3 : 7);
    }
    write_symbols(d, count, dynamic);
}

void setup_tables(Deflate* d) noexcept {
    for (u32 c = 0; c < 29; ++c)
        for (u32 length = kLengthBase[c]; length < kLengthBase[c] + (1u << kLengthExtra[c]) && length <= kMaxMatch;
             ++length)
            d->length_code[length] = static_cast<u8>(c);
    d->length_code[kMaxMatch] = 28;
    for (u32 c = 0; c < 30; ++c) {
        const u32 first = kDistanceBase[c] - 1u, last = first + (1u << kDistanceExtra[c]);
        for (u32 v = first; v < last; ++v)
            d->distance_code[v < 256 ? v : 256 + (v >> 7)] = static_cast<u8>(c);
    }
    u8* fixed = d->fixed.literal_lengths;
    for (u32 s = 0; s < 288; ++s)
        fixed[s] = static_cast<u8>(s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8);
    for (u32 s = 0; s < 32; ++s)
        d->fixed.distance_lengths[s] = 5;
    canonical(d->fixed.literal_lengths, 288, d->fixed.literal_codes);
    canonical(d->fixed.distance_lengths, 32, d->fixed.distance_codes);
}

u32 adler32(const u8* data, u32 count) noexcept {
    u32 a = 1, b = 0;
    while (count) {
        const u32 chunk = minimum(count, 5552u);
        for (u32 i = 0; i < chunk; ++i) {
            a += data[i];
            b += a;
        }
        a %= 65'521;
        b %= 65'521;
        data += chunk;
        count -= chunk;
    }
    return b << 16 | a;
}

u8 paeth(u32 a, u32 b, u32 c) noexcept {
    const i32 p = static_cast<i32>(a + b) - static_cast<i32>(c);
    const i32 pa = p > static_cast<i32>(a) ? p - static_cast<i32>(a) : static_cast<i32>(a) - p;
    const i32 pb = p > static_cast<i32>(b) ? p - static_cast<i32>(b) : static_cast<i32>(b) - p;
    const i32 pc = p > static_cast<i32>(c) ? p - static_cast<i32>(c) : static_cast<i32>(c) - p;
    return static_cast<u8>(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

u32 magnitude(u8 v) noexcept {
    return v < 128 ? static_cast<u32>(v) : 256u - v;
}

// Filter type 0-4 (least sum of absolute differences) and the filtered row.
void filter_row(const u8* row, const u8* prior, u32 bytes, u32 bpp, u8* out) noexcept {
    u32 sums[5]{};
    for (u32 i = 0; i < bytes; ++i) {
        const u32 x = row[i], a = i >= bpp ? row[i - bpp] : 0, b = prior[i], c = i >= bpp ? prior[i - bpp] : 0;
        sums[0] += magnitude(static_cast<u8>(x));
        sums[1] += magnitude(static_cast<u8>(x - a));
        sums[2] += magnitude(static_cast<u8>(x - b));
        sums[3] += magnitude(static_cast<u8>(x - ((a + b) >> 1)));
        sums[4] += magnitude(static_cast<u8>(x - paeth(a, b, c)));
    }
    u32 type = 0;
    for (u32 t = 1; t < 5; ++t)
        if (sums[t] < sums[type])
            type = t;
    out[0] = static_cast<u8>(type);
    for (u32 i = 0; i < bytes; ++i) {
        const u32 x = row[i], a = i >= bpp ? row[i - bpp] : 0, b = prior[i], c = i >= bpp ? prior[i - bpp] : 0;
        u32 v = x;
        switch (type) {
        case 1: v = x - a; break;
        case 2: v = x - b; break;
        case 3: v = x - ((a + b) >> 1); break;
        case 4: v = x - paeth(a, b, c); break;
        default: break;
        }
        out[1 + i] = static_cast<u8>(v);
    }
}

void write_chunk_header(u8* at, u32 bytes, const char* type) noexcept {
    byte_store_be32(at, bytes);
    copy_bytes(at + 4, type, 4);
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
    Deflate d{};
    d.in = input.data;
    d.n = input.count;
    d.head = reinterpret_cast<u32*>(workspace.data);
    d.prev = d.head + kHashSize;
    d.symbols = d.prev + kWindow;
    for (u32 i = 0; i < kHashSize; ++i)
        d.head[i] = kNone;
    setup_tables(&d);
    d.bits = {output.data, output.count, 0, 0, 0};
    // CMF 0x78 (deflate, 32 KiB window), FLG 0x9C (default level, check bits).
    d.bits.put(0x78, 8);
    d.bits.put(0x9C, 8);
    u32 position = 0;
    do {
        const u32 start = position, end = minimum(d.n, position + kBlockBytes);
        u32 literal_frequency[kLiteralSymbols]{}, distance_frequency[kDistanceSymbols]{}, count = 0;
        u64 extra = 0;
        const auto literal = [&](u32 at) noexcept {
            d.symbols[count++] = d.in[at];
            ++literal_frequency[d.in[at]];
        };
        while (position < end) {
            insert_until(&d, position);
            u32 distance = 0, length = find_match(d, position, end, &distance);
            if (length && length < kLazyLength && position + 1 < end) {
                insert_until(&d, position + 1);
                u32 later_distance = 0;
                const u32 later = find_match(d, position + 1, end, &later_distance);
                if (later > length) {
                    literal(position++);
                    length = later;
                    distance = later_distance;
                }
            }
            if (!length) {
                literal(position++);
                continue;
            }
            d.symbols[count++] = kMatchFlag | (length - kMinMatch) << 16 | (distance - 1);
            const u32 lc = d.length_code[length], dc = distance_symbol(d, distance);
            ++literal_frequency[257 + lc];
            ++distance_frequency[dc];
            extra += kLengthExtra[lc] + kDistanceExtra[dc];
            position += length;
        }
        literal_frequency[kEndOfBlock] = 1;
        emit_block(&d, start, position, count, literal_frequency, distance_frequency, extra, position == d.n);
    } while (position < d.n);
    d.bits.align();
    const u32 adler = adler32(d.in, d.n);
    for (u32 shift = 32; shift;) {
        shift -= 8;
        d.bits.put(adler >> shift & 0xFFu, 8);
    }
    return d.bits.at <= output.count ? static_cast<u32>(d.bits.at) : 0;
}

u64 png_encode_bound(u32 width, u32 height) noexcept {
    if (!width || !height || width > kPngEncodeDimensionMaximum || height > kPngEncodeDimensionMaximum)
        return 0;
    const u64 raw = static_cast<u64>(height) * (1ull + static_cast<u64>(width) * 4u);
    return 8 + 25 + 12 + zlib_compress_bound(raw) + 12;
}

u64 png_encode_workspace_bytes(u32 width, u32 height) noexcept {
    if (!width || !height || width > kPngEncodeDimensionMaximum || height > kPngEncodeDimensionMaximum)
        return 0;
    const u64 row = static_cast<u64>(width) * 4u;
    return kZlibCompressWorkspaceBytes + static_cast<u64>(height) * (1ull + row) + 2 * row;
}

u32 png_encode_rgba8(const u8* rgba, u32 width, u32 height, Slice<u8> workspace, Slice<u8> output) noexcept {
    const u64 needed = png_encode_workspace_bytes(width, height);
    const u64 pixels = static_cast<u64>(width) * height;
    if (!rgba || !needed || !workspace.data || workspace.count < needed || !output.data ||
        (reinterpret_cast<usize>(workspace.data) & 3u) || output.count < 8 + 25 + 12 + 12)
        return 0;
    bool opaque = true;
    for (u64 i = 0; i < pixels && opaque; ++i)
        opaque = rgba[i * 4 + 3] == 255;
    const u32 bpp = opaque ? 3 : 4, row_bytes = width * bpp;
    const u32 filtered_bytes = height * (1 + row_bytes);
    u8* filtered = workspace.data + kZlibCompressWorkspaceBytes;
    u8* prior = filtered + filtered_bytes;
    u8* current = prior + row_bytes;
    clear_bytes(prior, row_bytes);
    for (u32 y = 0; y < height; ++y) {
        const u8* source = rgba + static_cast<u64>(y) * width * 4;
        if (opaque)
            for (u32 x = 0; x < width; ++x)
                copy_bytes(current + x * 3, source + x * 4, 3);
        else
            copy_bytes(current, source, row_bytes);
        filter_row(current, prior, row_bytes, bpp, filtered + static_cast<u64>(y) * (1 + row_bytes));
        u8* swap = prior;
        prior = current;
        current = swap;
    }
    u8* out = output.data;
    copy_bytes(out, kPngSignature, 8);
    u8* ihdr = out + 8;
    write_chunk_header(ihdr, 13, "IHDR");
    byte_store_be32(ihdr + 8, width);
    byte_store_be32(ihdr + 12, height);
    ihdr[16] = 8;
    ihdr[17] = static_cast<u8>(opaque ? 2 : 6);
    ihdr[18] = ihdr[19] = ihdr[20] = 0;
    byte_store_be32(ihdr + 21, png_crc32(ihdr + 4, 17));
    u8* idat = ihdr + 25;
    const u32 room = output.count - (8 + 25 + 12 + 12);
    const u32 zlib = zlib_compress({filtered, filtered_bytes}, {workspace.data, kZlibCompressWorkspaceBytes},
                                   {idat + 8, room});
    if (!zlib)
        return 0;
    write_chunk_header(idat, zlib, "IDAT");
    byte_store_be32(idat + 8 + zlib, png_crc32(idat + 4, zlib + 4));
    u8* iend = idat + 12 + zlib;
    write_chunk_header(iend, 0, "IEND");
    byte_store_be32(iend + 8, png_crc32(iend + 4, 4));
    return static_cast<u32>(iend + 12 - out);
}

} // namespace maicord
