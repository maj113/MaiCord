// Differential tests: the table-driven inflater must match the frozen
// bit-serial reference on random valid, mutated, truncated and capacity-bound
// zlib streams and sync-flushed zlib-stream sequences.
#include "deflate_reference.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

constexpr u32 kMaximumRaw = 140 * 1024;
constexpr u32 kMaximumEncoded = 200 * 1024;
u8 g_raw[kMaximumRaw];
u8 g_encoded[kMaximumEncoded];
u8 g_mutated[kMaximumEncoded];
u8 g_actual[kMaximumRaw + 512];
u8 g_reference[kMaximumRaw + 512];
ZlibStreamInflater g_stream;
DeflateWorkspace g_workspace;

constexpr u16 kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23,  27,
                                 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr u8 kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr u16 kDistanceBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                   33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                   1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr u8 kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr u8 kPrecodeOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct Random {
    u64 state;
    u32 next() noexcept {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<u32>(state >> 16);
    }
    u32 below(u32 limit) noexcept { return limit ? next() % limit : 0; }
    bool chance(u32 percent) noexcept { return below(100) < percent; }
};

struct BitWriter {
    u8* data;
    u32 capacity;
    u32 bytes;
    u64 buffer;
    u32 count;
    bool overflow;
    void put(u32 value, u32 bits) noexcept {
        buffer |= static_cast<u64>(value) << count;
        count += bits;
        while (count >= 8) {
            if (bytes < capacity)
                data[bytes++] = static_cast<u8>(buffer);
            else
                overflow = true;
            buffer >>= 8;
            count -= 8;
        }
    }
    void code(u32 value, u32 length) noexcept {
        u32 reversed = 0;
        for (u32 bit = 0; bit < length; ++bit)
            reversed |= ((value >> bit) & 1u) << (length - 1 - bit);
        put(reversed, length);
    }
    void align() noexcept {
        if (count)
            put(0, 8 - count);
    }
};

void canonical_codes(const u8* lengths, u32 count, u16* codes) noexcept {
    u16 per_length[16]{}, next[16]{};
    for (u32 i = 0; i < count; ++i)
        ++per_length[lengths[i]];
    per_length[0] = 0;
    u32 code = 0;
    for (u32 bits = 1; bits < 16; ++bits) {
        code = (code + per_length[bits - 1]) << 1;
        next[bits] = static_cast<u16>(code);
    }
    for (u32 i = 0; i < count; ++i)
        codes[i] = lengths[i] ? next[lengths[i]]++ : 0;
}

// Random complete prefix code: split leaves until `leaves` exist. Biased
// splitting of the newest leaf produces deep codes that need subtables.
void random_code(Random& random, const bool* used, u32 symbols, u32 maximum_length, u8* lengths) noexcept {
    u32 required = 0;
    for (u32 i = 0; i < symbols; ++i)
        required += used[i];
    const u32 lower = maximum<u32>(required, 2);
    const u32 leaves = lower + random.below(symbols - lower + 1);
    u8 depth[288]{};
    u32 count = 1;
    depth[0] = 0;
    const bool deep = random.chance(40);
    while (count < leaves) {
        u32 pick = deep && random.chance(70) ? count - 1 : random.below(count);
        if (depth[pick] >= maximum_length) {
            pick = ~0u;
            for (u32 i = 0; i < count && pick == ~0u; ++i)
                if (depth[i] < maximum_length)
                    pick = i;
            if (pick == ~0u)
                break;
        }
        ++depth[pick];
        depth[count++] = depth[pick];
    }
    // Assign leaves to all used symbols first, then to random unused ones.
    u16 order[288];
    u32 order_count = 0;
    for (u32 i = 0; i < symbols; ++i)
        if (used[i])
            order[order_count++] = static_cast<u16>(i);
    u32 unused_start = order_count;
    for (u32 i = 0; i < symbols; ++i)
        if (!used[i])
            order[order_count++] = static_cast<u16>(i);
    for (u32 i = order_count; i > unused_start + 1; --i) {
        const u32 j = unused_start + random.below(i - unused_start);
        const u16 t = order[i - 1];
        order[i - 1] = order[j];
        order[j] = t;
    }
    for (u32 i = 0; i < symbols; ++i)
        lengths[i] = 0;
    for (u32 i = 0; i < count; ++i)
        lengths[order[i]] = depth[i];
}

struct Token {
    u16 length; // 0 = literal
    u16 distance;
    u8 literal;
    bool long_258; // encode 258 as symbol 284 + 31
};

struct Encoder {
    Random* random;
    BitWriter writer;
    u8* raw;
    u32 raw_bytes;
    u32 raw_capacity;
    u32 window;
    bool corrupt;
    Token tokens[4096];
};

u32 length_symbol(u32 length) noexcept {
    u32 index = 28;
    while (kLengthBase[index] > length)
        --index;
    return index;
}
u32 distance_symbol(u32 distance) noexcept {
    u32 index = 29;
    while (kDistanceBase[index] > distance)
        --index;
    return index;
}

u32 make_tokens(Encoder& encoder, u32 limit, bool literals_only = false) noexcept {
    Random& random = *encoder.random;
    const u32 alphabet = random.chance(50) ? 4 : 256;
    u32 count = 0;
    while (count < limit && encoder.raw_bytes + 258 < encoder.raw_capacity) {
        Token token{};
        const u32 history = minimum(encoder.raw_bytes, encoder.window);
        if (history != 0 && !literals_only && random.chance(45)) {
            const u32 pick = random.below(8);
            token.distance = static_cast<u16>(pick == 0 ? history
                                              : pick < 4 ? 1 + random.below(minimum<u32>(history, 16))
                                                         : 1 + random.below(history));
            const u32 shape = random.below(6);
            token.length = static_cast<u16>(shape == 0 ? 258 : shape == 1 ? 3 + random.below(14)
                                                                         : 3 + random.below(256));
            token.long_258 = token.length == 258 && random.chance(20);
            for (u32 i = 0; i < token.length; ++i, ++encoder.raw_bytes)
                encoder.raw[encoder.raw_bytes] = encoder.raw[encoder.raw_bytes - token.distance];
        } else {
            token.literal = static_cast<u8>(alphabet == 4 ? 'a' + random.below(4) : random.below(256));
            encoder.raw[encoder.raw_bytes++] = token.literal;
        }
        encoder.tokens[count++] = token;
    }
    return count;
}

// Deliberate stream damage at one token position.
enum Corruption : u32 {
    kIntact,
    kBadLitlenSymbol, // fixed code 286/287
    kBadDistanceSymbol, // fixed distance code 30/31
    kFarDistance, // fixed code, distance beyond history or window
    kSingleDistanceMiss, // unused codeword of a one-codeword distance code
    kEmptyDistanceLength, // length symbol with an empty distance code
    kSingleLitlenMiss, // unused codeword of a one-codeword literal/length code
};

void write_tokens(Encoder& encoder, u32 count, const u8* litlen, const u16* litlen_codes, const u8* distance,
                  const u16* distance_codes, u32 corruption = kIntact, u32 corrupt_index = 0) noexcept {
    BitWriter& w = encoder.writer;
    for (u32 i = 0; i <= count; ++i) {
        if (corruption != kIntact && i == corrupt_index) {
            if (corruption == kBadLitlenSymbol) {
                w.code(litlen_codes[286 + encoder.random->below(2)], 8);
            } else if (corruption == kSingleLitlenMiss) {
                w.put(1, 1);
            } else {
                w.code(litlen_codes[257], litlen[257]);
                if (corruption == kBadDistanceSymbol) {
                    w.code(distance_codes[30 + encoder.random->below(2)], 5);
                } else if (corruption == kFarDistance) {
                    w.code(distance_codes[29], 5);
                    w.put(32768 - kDistanceBase[29], 13);
                } else if (corruption == kSingleDistanceMiss) {
                    w.put(1, 1);
                }
            }
        }
        if (i == count)
            break;
        const Token& token = encoder.tokens[i];
        if (!token.length) {
            w.code(litlen_codes[token.literal], litlen[token.literal]);
            continue;
        }
        const u32 ls = token.long_258 ? 27 : length_symbol(token.length);
        w.code(litlen_codes[257 + ls], litlen[257 + ls]);
        w.put(token.length - kLengthBase[ls], kLengthExtra[ls]);
        const u32 ds = distance_symbol(token.distance);
        w.code(distance_codes[ds], distance[ds]);
        w.put(token.distance - kDistanceBase[ds], kDistanceExtra[ds]);
    }
    w.code(litlen_codes[256], litlen[256]);
}

void stored_block(Encoder& encoder, bool final, u32 length) noexcept {
    length = minimum(length, encoder.raw_capacity - encoder.raw_bytes);
    BitWriter& w = encoder.writer;
    w.put(final ? 1 : 0, 1);
    w.put(0, 2);
    w.align();
    w.put(length & 0xFFFF, 16);
    w.put(~length & 0xFFFF, 16);
    for (u32 i = 0; i < length; ++i) {
        const u8 value = static_cast<u8>(encoder.random->below(256));
        encoder.raw[encoder.raw_bytes++] = value;
        w.put(value, 8);
    }
}

void fixed_block(Encoder& encoder, bool final, u32 limit) noexcept {
    u8 lengths[320];
    for (u32 s = 0; s < 288; ++s)
        lengths[s] = s <= 143 ? 8 : s <= 255 ? 9 : s <= 279 ? 7 : 8;
    for (u32 s = 0; s < 32; ++s)
        lengths[288 + s] = 5;
    u16 codes[320];
    canonical_codes(lengths, 288, codes);
    canonical_codes(lengths + 288, 32, codes + 288);
    encoder.writer.put(final ? 1 : 0, 1);
    encoder.writer.put(1, 2);
    const u32 count = make_tokens(encoder, limit);
    const u32 corruption = encoder.corrupt && encoder.random->chance(35) ? 1 + encoder.random->below(3) : kIntact;
    write_tokens(encoder, count, lengths, codes, lengths + 288, codes + 288, corruption,
                 encoder.random->below(count + 1));
}

void dynamic_block(Encoder& encoder, bool final, u32 limit) noexcept {
    Random& random = *encoder.random;
    const u32 corruption = encoder.corrupt && random.chance(35) ? kSingleDistanceMiss + random.below(3) : kIntact;
    const u32 count = make_tokens(encoder, corruption == kSingleLitlenMiss ? 0 : limit, corruption != kIntact);
    bool litlen_used[286]{}, distance_used[30]{};
    litlen_used[256] = true;
    if (corruption == kSingleDistanceMiss || corruption == kEmptyDistanceLength)
        litlen_used[257] = true;
    if (corruption == kSingleDistanceMiss)
        distance_used[0] = true;
    for (u32 i = 0; i < count; ++i) {
        const Token& token = encoder.tokens[i];
        if (!token.length) {
            litlen_used[token.literal] = true;
            continue;
        }
        litlen_used[257 + (token.long_258 ? 27 : length_symbol(token.length))] = true;
        distance_used[distance_symbol(token.distance)] = true;
    }
    u8 lengths[320]{};
    u32 distance_symbols_used = 0;
    for (bool used : distance_used)
        distance_symbols_used += used;
    if (corruption == kSingleLitlenMiss || (count == 0 && random.chance(30))) {
        lengths[256] = 1; // single-codeword literal/length code
    } else {
        random_code(random, litlen_used, 286, 15, lengths);
    }
    u32 hlit = 257;
    for (u32 s = 0; s < 286; ++s)
        if (lengths[s])
            hlit = maximum(hlit, s + 1);
    u8 distance_lengths[30]{};
    if (corruption == kEmptyDistanceLength || (distance_symbols_used == 0 && random.chance(50))) {
        // Empty distance code.
    } else if (corruption == kSingleDistanceMiss || (distance_symbols_used == 1 && random.chance(50))) {
        for (u32 s = 0; s < 30; ++s)
            if (distance_used[s])
                distance_lengths[s] = 1;
    } else {
        random_code(random, distance_used, 30, 15, distance_lengths);
    }
    u32 hdist = 1;
    for (u32 s = 0; s < 30; ++s)
        if (distance_lengths[s])
            hdist = maximum(hdist, s + 1);
    for (u32 s = 0; s < hdist; ++s)
        lengths[hlit + s] = distance_lengths[s];

    // Code-length sequence with random use of run-length symbols.
    u8 symbols[320];
    u8 extras[320];
    u32 symbol_count = 0;
    const u32 total = hlit + hdist;
    const bool rle = random.chance(75);
    for (u32 i = 0; i < total;) {
        const u8 value = lengths[i];
        u32 run = 1;
        while (i + run < total && lengths[i + run] == value)
            ++run;
        if (rle && value == 0 && run >= 11) {
            const u32 take = minimum<u32>(run, 11 + random.below(128));
            symbols[symbol_count] = 18;
            extras[symbol_count++] = static_cast<u8>(minimum<u32>(take, 138) - 11);
            i += minimum<u32>(take, 138);
        } else if (rle && value == 0 && run >= 3) {
            const u32 take = minimum<u32>(run, 10);
            symbols[symbol_count] = 17;
            extras[symbol_count++] = static_cast<u8>(take - 3);
            i += take;
        } else if (rle && i > 0 && lengths[i - 1] == value && run >= 3) {
            const u32 take = minimum<u32>(run, 6);
            symbols[symbol_count] = 16;
            extras[symbol_count++] = static_cast<u8>(take - 3);
            i += take;
        } else {
            symbols[symbol_count] = value;
            extras[symbol_count++] = 0;
            ++i;
        }
    }
    bool precode_used[19]{};
    for (u32 i = 0; i < symbol_count; ++i)
        precode_used[symbols[i]] = true;
    u8 precode[19];
    random_code(random, precode_used, 19, 7, precode);
    u32 hclen = 4;
    for (u32 i = 0; i < 19; ++i)
        if (precode[kPrecodeOrder[i]])
            hclen = maximum(hclen, i + 1);
    u16 precode_codes[19], codes[320];
    canonical_codes(precode, 19, precode_codes);
    canonical_codes(lengths, hlit, codes);
    canonical_codes(lengths + hlit, hdist, codes + hlit);

    BitWriter& w = encoder.writer;
    w.put(final ? 1 : 0, 1);
    w.put(2, 2);
    w.put(hlit - 257, 5);
    w.put(hdist - 1, 5);
    w.put(hclen - 4, 4);
    for (u32 i = 0; i < hclen; ++i)
        w.put(precode[kPrecodeOrder[i]], 3);
    for (u32 i = 0; i < symbol_count; ++i) {
        w.code(precode_codes[symbols[i]], precode[symbols[i]]);
        if (symbols[i] == 16)
            w.put(extras[i], 2);
        else if (symbols[i] == 17)
            w.put(extras[i], 3);
        else if (symbols[i] == 18)
            w.put(extras[i], 7);
    }
    write_tokens(encoder, count, lengths, codes, lengths + hlit, codes + hlit, corruption,
                 random.below(count + 1));
}

void random_block(Encoder& encoder, bool final) noexcept {
    Random& random = *encoder.random;
    const u32 kind = random.below(10);
    const u32 limit = random.chance(15) ? 4096 : random.below(400);
    if (kind == 0)
        stored_block(encoder, final, random.chance(20) ? 0 : random.below(random.chance(10) ? 65535 : 600));
    else if (kind < 4)
        fixed_block(encoder, final, limit);
    else
        dynamic_block(encoder, final, limit);
}

u32 adler(const u8* bytes, u32 count) noexcept {
    u32 a = 1, b = 0;
    for (u32 i = 0; i < count; ++i) {
        a = (a + bytes[i]) % 65521;
        b = (b + a) % 65521;
    }
    return b << 16 | a;
}

void zlib_header(BitWriter& w, u32 cinfo) noexcept {
    const u32 cmf = cinfo << 4 | 8;
    u32 flags = 0;
    while ((cmf * 256 + flags) % 31)
        ++flags;
    w.put(cmf, 8);
    w.put(flags, 8);
}

// Splits input into up to four spans, including empty ones.
u32 split(Random& random, const u8* data, u32 bytes, DeflateInputSpan* spans) noexcept {
    const u32 count = 1 + random.below(4);
    u32 offset = 0;
    for (u32 i = 0; i + 1 < count; ++i) {
        const u32 take = random.chance(30) ? 0 : random.below(bytes - offset + 1);
        spans[i] = {take ? data + offset : nullptr, take};
        offset += take;
    }
    spans[count - 1] = {data + offset, bytes - offset};
    return count;
}

bool same(const ZlibInflateResult& left, const ZlibInflateResult& right, const u8* left_bytes,
          const u8* right_bytes) noexcept {
    if (left.error != right.error)
        return false;
    if (!left.ok())
        return true;
    return left.input_bytes == right.input_bytes && left.output_bytes == right.output_bytes &&
           left.blocks == right.blocks && left.literals == right.literals && left.copies == right.copies &&
           bytes_equal(left_bytes, right_bytes, left.output_bytes);
}

void mutate(Random& random, const u8* source, u32 bytes, u8* target, u32* target_bytes) noexcept {
    copy_bytes(target, source, bytes);
    *target_bytes = bytes;
    const u32 mode = random.below(4);
    if (mode == 0 && bytes > 1) {
        *target_bytes = random.below(bytes);
    } else if (mode == 1 || mode == 2) {
        for (u32 flips = 1 + random.below(3); flips; --flips) {
            const u32 bit = random.below(bytes * 8);
            target[bit / 8] ^= static_cast<u8>(1u << (bit & 7));
        }
    } else if (bytes + 4 < kMaximumEncoded) {
        for (u32 i = 0, extra = 1 + random.below(4); i < extra; ++i)
            target[(*target_bytes)++] = static_cast<u8>(random.below(256));
    }
}

void zlib_cases(void (*check)(bool, const char*) noexcept) noexcept {
    Random random{0x9E3779B97F4A7C15ull};
    bool encoder_valid = true, identical = true;
    u32 seen = 0;
    for (u32 iteration = 0; iteration < 260; ++iteration) {
        Encoder encoder{};
        encoder.random = &random;
        encoder.corrupt = random.chance(25);
        encoder.raw = g_raw;
        encoder.raw_capacity = random.chance(10) ? kMaximumRaw : 24 * 1024;
        const u32 cinfo = random.chance(80) ? 7 : random.below(8);
        encoder.window = 1u << (cinfo + 8);
        encoder.writer = {g_encoded, kMaximumEncoded, 0, 0, 0, false};
        zlib_header(encoder.writer, cinfo);
        const u32 blocks = 1 + random.below(random.chance(20) ? 12 : 3);
        for (u32 block = 0; block < blocks; ++block)
            random_block(encoder, block + 1 == blocks);
        encoder.writer.align();
        const u32 checksum = adler(g_raw, encoder.raw_bytes);
        for (u32 shift = 24;; shift -= 8) {
            encoder.writer.put((checksum >> shift) & 0xFF, 8);
            if (!shift)
                break;
        }
        if (encoder.writer.overflow)
            continue;
        const u32 encoded_bytes = encoder.writer.bytes;
        for (u32 variant = 0; variant < 6; ++variant) {
            const u8* input = g_encoded;
            u32 input_bytes = encoded_bytes;
            if (variant >= 2) {
                mutate(random, g_encoded, encoded_bytes, g_mutated, &input_bytes);
                input = g_mutated;
            }
            u32 capacity = encoder.raw_bytes + random.below(64);
            u32 expected = random.chance(50) ? ~0u : encoder.raw_bytes;
            if (variant == 1 && encoder.raw_bytes) {
                const bool undersized = random.chance(50);
                capacity = undersized ? random.below(encoder.raw_bytes) : encoder.raw_bytes + 16;
                expected = undersized ? ~0u : encoder.raw_bytes - 1;
            }
            DeflateInputSpan spans[4];
            const u32 span_count = split(random, input, input_bytes, spans);
            const auto reference = deflate_reference::zlib_inflate({spans, span_count}, {g_reference, capacity},
                                                                   expected);
            const auto actual = zlib_inflate({spans, span_count}, {g_actual, capacity}, expected, &g_workspace);
            seen |= 1u << static_cast<u32>(reference.error);
            if (variant == 0 && !encoder.corrupt)
                encoder_valid = encoder_valid && reference.ok() && reference.output_bytes == encoder.raw_bytes &&
                                bytes_equal(g_reference, g_raw, encoder.raw_bytes);
            identical = identical && same(actual, reference, g_actual, g_reference);
        }
    }
    check(encoder_valid, "DEFLATE differential encoder emits streams the reference accepts");
    constexpr ZlibInflateError classes[] = {
        ZlibInflateError::none,           ZlibInflateError::truncated,        ZlibInflateError::invalid_block,
        ZlibInflateError::invalid_huffman, ZlibInflateError::invalid_symbol,  ZlibInflateError::invalid_distance,
        ZlibInflateError::output_capacity, ZlibInflateError::output_size,     ZlibInflateError::checksum,
        ZlibInflateError::trailing_data};
    u32 required = 0;
    for (ZlibInflateError error : classes)
        required |= 1u << static_cast<u32>(error);
    check((seen & required) == required, "DEFLATE differential cases reach every decoder error class");
    check(identical, "DEFLATE table decoder matches the reference on valid, mutated and bounded streams");
}

void stream_cases(void (*check)(bool, const char*) noexcept) noexcept {
    Random random{0xD1B54A32D192ED03ull};
    bool encoder_valid = true, identical = true;
    for (u32 iteration = 0; iteration < 40; ++iteration) {
        Encoder encoder{};
        encoder.random = &random;
        encoder.raw = g_raw;
        encoder.raw_capacity = kMaximumRaw;
        const u32 cinfo = random.chance(75) ? 7 : random.below(8);
        encoder.window = 1u << (cinfo + 8);
        encoder.writer = {g_encoded, kMaximumEncoded, 0, 0, 0, false};
        zlib_header(encoder.writer, cinfo);
        u32 boundaries[24]{}, raw_ends[24]{};
        const u32 segments = 2 + random.below(20);
        for (u32 segment = 0; segment < segments; ++segment) {
            for (u32 block = random.below(3); block; --block)
                random_block(encoder, false);
            stored_block(encoder, false, 0); // Z_SYNC_FLUSH marker
            boundaries[segment] = encoder.writer.bytes;
            raw_ends[segment] = encoder.raw_bytes;
        }
        if (encoder.writer.overflow)
            continue;
        const bool mutated = random.chance(50);
        u32 input_bytes = encoder.writer.bytes;
        const u8* input = g_encoded;
        if (mutated) {
            copy_bytes(g_mutated, g_encoded, input_bytes);
            const u32 bit = random.below(input_bytes * 8);
            g_mutated[bit / 8] ^= static_cast<u8>(1u << (bit & 7));
            input = g_mutated;
        }
        deflate_reference::stream_reset();
        zlib_stream_initialize(&g_stream);
        u32 start = 0, raw_start = 0;
        bool poisoned = false;
        for (u32 segment = 0; segment < segments; ++segment) {
            const u32 raw_bytes = raw_ends[segment] - raw_start;
            const u32 capacity = random.chance(10) ? random.below(raw_bytes + 1) : raw_bytes + random.below(300);
            DeflateInputSpan spans[4];
            const u32 span_count = split(random, input + start, boundaries[segment] - start, spans);
            const auto reference = deflate_reference::stream_flush({spans, span_count}, {g_reference, capacity});
            const auto actual = zlib_stream_inflate_flush(&g_stream, {spans, span_count}, {g_actual, capacity});
            if (!mutated && !poisoned && capacity >= raw_bytes)
                encoder_valid = encoder_valid && reference.ok() && reference.output_bytes == raw_bytes &&
                                bytes_equal(g_reference, g_raw + raw_start, raw_bytes);
            identical = identical && same(actual, reference, g_actual, g_reference);
            poisoned = poisoned || !reference.ok();
            start = boundaries[segment];
            raw_start = raw_ends[segment];
        }
    }
    zlib_stream_clear(&g_stream);
    check(encoder_valid, "DEFLATE stream differential encoder emits accepted flushes");
    check(identical, "DEFLATE stream decoder matches the reference across flushes, windows and poisoning");
}

} // namespace

void test_deflate_differential(void (*check)(bool, const char*) noexcept) noexcept {
    zlib_cases(check);
    stream_cases(check);
}

} // namespace maicord
