#include "maicord/protocol/gateway/etf.hpp"

#include "maicord/core/byte_order.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/intrin.hpp"
#include "maicord/sys/simd.hpp"

namespace maicord {
namespace {

enum Tag : u8 {
    kNewFloat = 70,
    kSmallInteger = 97,
    kInteger = 98,
    kFloat = 99,
    kAtom = 100,
    kSmallTuple = 104,
    kLargeTuple = 105,
    kNil = 106,
    kString = 107,
    kList = 108,
    kBinary = 109,
    kSmallBig = 110,
    kLargeBig = 111,
    kSmallAtom = 115,
    kMap = 116,
    kAtomUtf8 = 118,
    kSmallAtomUtf8 = 119,
    kVersion = 131,
};

constexpr u64 kSafeInteger = 1ull << 53;

// ------------------------------------------------------------ JSON output
struct Writer {
    // *out as the last growth left it: a reservation reads the copy.
    Slice<u8>* out;
    u8* data;
    u32 capacity;
    u32 used;
    u32 maximum;
    EtfGrowOutput grow;
    void* context;
    // The structural index being filled (see etf_to_json_indexed): marks counts every structural written,
    // whether or not it fit.
    u32* index;
    u32 index_capacity;
    u32 marks;

    __forceinline void mark(u32 at) noexcept {
        if (marks < index_capacity)
            index[marks] = at;
        ++marks;
    }

    __declspec(noinline) u8* reserve_slow(u32 bytes) noexcept {
        if (bytes > maximum - used)
            return nullptr;
        // The owner's storage sets the growth policy (stable storage doubles).
        const u32 required = used + bytes;
        Slice<u8> next{};
        if (!grow || !grow(context, required, &next) || (data && next.data != data) || next.count < required)
            return nullptr;
        *out = next;
        data = next.data;
        capacity = next.count;
        return data + used;
    }
    __forceinline u8* reserve(u32 bytes) noexcept {
        return capacity - used >= bytes ? data + used : reserve_slow(bytes);
    }
    // Every single byte written this way is structural: a quote, comma, colon or bracket.
    __forceinline bool byte(u8 value) noexcept {
        u8* at = reserve(1);
        if (!at)
            return false;
        mark(used);
        *at = value;
        ++used;
        return true;
    }
    __forceinline bool bytes(const void* data, u32 count) noexcept {
        u8* at = reserve(count);
        if (!at)
            return false;
        copy_bytes(at, data, count);
        used += count;
        return true;
    }
};

constexpr char kDigitPairs[] = "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
                                "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
                                "8081828384858687888990919293949596979899";

__forceinline void write_pair(u8* out, u32 value) noexcept {
    copy_bytes(out, kDigitPairs + value * 2, 2);
}

// Writes 20 bytes: the digits, then bytes of no meaning. Returns the digit count.
__forceinline u32 write_decimal(u8* out, u64 value) noexcept {
    if (value < 10) {
        out[0] = static_cast<u8>('0' + value);
        return 1;
    }
    // Right-aligned in the first 20 bytes. The copy below reads 20 bytes from any digit position.
    u8 digits[40];
    u32 at = 20;
    // Eight digits at a time in 32-bit arithmetic, so a snowflake takes two 64-bit divisions.
    while (value >= 100'000'000) {
        const u32 chunk = static_cast<u32>(value % 100'000'000);
        value /= 100'000'000;
        const u32 high = chunk / 10'000, low = chunk % 10'000;
        at -= 8;
        write_pair(digits + at, high / 100);
        write_pair(digits + at + 2, high % 100);
        write_pair(digits + at + 4, low / 100);
        write_pair(digits + at + 6, low % 100);
    }
    u32 rest = static_cast<u32>(value);
    while (rest >= 100) {
        at -= 2;
        write_pair(digits + at, rest % 100);
        rest /= 100;
    }
    if (rest >= 10) {
        at -= 2;
        write_pair(digits + at, rest);
    } else {
        digits[--at] = static_cast<u8>('0' + rest);
    }
    copy_bytes(out, digits + at, 20);
    return 20 - at;
}

// An integer of the given magnitude: a number below 2^53, else a string. One reservation holds the two quotes,
// the sign and the 20 bytes that write_decimal writes.
bool write_integer(Writer* w, u64 magnitude, bool negative, bool as_name) noexcept {
    const bool quoted = as_name || magnitude >= kSafeInteger;
    u8* at = w->reserve(23);
    if (!at)
        return false;
    u32 n = 0;
    if (quoted) {
        w->mark(w->used);
        at[n++] = '"';
    }
    if (negative && magnitude != 0)
        at[n++] = '-';
    n += write_decimal(at + n, magnitude);
    if (quoted) {
        w->mark(w->used + n);
        at[n++] = '"';
    }
    w->used += n;
    return true;
}

const char kHex[] = "0123456789abcdef";

// One bit for each byte of v that a JSON string cannot hold as it is: a control, '"', '\\' or 0x80 and more.
__forceinline u32 special_mask(__m128i v) noexcept {
    return static_cast<u32>(_mm_movemask_epi8(
        _mm_or_si128(_mm_cmplt_epi8(v, _mm_set1_epi8(0x20)),
                     _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8('"')), _mm_cmpeq_epi8(v, _mm_set1_epi8('\\'))))));
}

// Escapes and validates UTF-8 (latin1: bytes are code points). readable: bytes from s to the input's end.
bool write_string_slow(Writer* w, const u8* s, u32 n, bool latin1, u32 readable) noexcept {
    if (!w->byte('"'))
        return false;
    u32 i = 0;
    while (i < n) {
        if (readable - i >= 16) {
            // Bytes that need nothing are copied 16 at a time. The copy can pass the run and the string: those
            // output bytes have no meaning and the next write replaces them.
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i));
            u8* at = w->reserve(16);
            if (!at)
                return false;
            _mm_storeu_si128(reinterpret_cast<__m128i*>(at), v);
            unsigned long plain = 0;
            _BitScanForward(&plain, special_mask(v) | 1u << 16);
            const u32 run = minimum(static_cast<u32>(plain), n - i);
            w->used += run;
            i += run;
            if (plain == 16 || i == n)
                continue;
        } else {
            u32 run = i;
            while (run < n && s[run] >= 0x20 && s[run] < 0x80 && s[run] != '"' && s[run] != '\\')
                ++run;
            if (run != i) {
                if (!w->bytes(s + i, run - i))
                    return false;
                i = run;
                continue;
            }
        }
        const u8 c = s[i];
        u8* at = w->reserve(6);
        if (!at)
            return false;
        if (c < 0x20 || c == '"' || c == '\\') {
            at[0] = '\\';
            switch (c) {
            case '"': at[1] = '"'; break;
            case '\\': at[1] = '\\'; break;
            case '\n': at[1] = 'n'; break;
            case '\r': at[1] = 'r'; break;
            case '\t': at[1] = 't'; break;
            case '\b': at[1] = 'b'; break;
            case '\f': at[1] = 'f'; break;
            default:
                at[1] = 'u';
                at[2] = '0';
                at[3] = '0';
                at[4] = static_cast<u8>(kHex[c >> 4]);
                at[5] = static_cast<u8>(kHex[c & 15]);
                w->used += 6;
                ++i;
                continue;
            }
            w->used += 2;
            ++i;
            continue;
        }
        if (latin1) {
            at[0] = static_cast<u8>(0xC0 | c >> 6);
            at[1] = static_cast<u8>(0x80 | (c & 0x3F));
            w->used += 2;
            ++i;
            continue;
        }
        // A run of UTF-8 sequences: each one whole, or U+FFFD for an invalid byte.
        for (;;) {
            const u8 lead = s[i];
            u32 length = 0, minimum_code = 0, code = 0;
            if (lead >= 0xC2 && lead <= 0xDF)
                length = 2, code = lead & 0x1F, minimum_code = 0x80;
            else if (lead >= 0xE0 && lead <= 0xEF)
                length = 3, code = lead & 0x0F, minimum_code = 0x800;
            else if (lead >= 0xF0 && lead <= 0xF4)
                length = 4, code = lead & 0x07, minimum_code = 0x10000;
            at[0] = lead;
            bool valid = length != 0 && n - i >= length;
            for (u32 k = 1; valid && k < length; ++k) {
                valid = (s[i + k] & 0xC0) == 0x80;
                code = code << 6 | (s[i + k] & 0x3F);
                at[k] = s[i + k];
            }
            if (valid && (code < minimum_code || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)))
                valid = false;
            if (!valid) {
                at[0] = 0xEF;
                at[1] = 0xBF;
                at[2] = 0xBD;
                length = 3;
            }
            w->used += length;
            i += valid ? length : 1;
            if (i == n || s[i] < 0x80)
                break;
            at = w->reserve(4);
            if (!at)
                return false;
        }
    }
    return w->byte('"');
}

// A string of fewer than 16 bytes that needs nothing is copied whole when 16
// bytes of input may be read (readable: bytes from s to the input's end).
__forceinline bool write_string(Writer* w, const u8* s, u32 n, bool latin1, u32 readable) noexcept {
    if (n < 16 && readable >= 16) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
        const u32 mask = special_mask(v) & ((1u << n) - 1);
        if (!mask) {
            u8* at = w->reserve(18);
            if (!at)
                return false;
            at[0] = '"';
            _mm_storeu_si128(reinterpret_cast<__m128i*>(at + 1), v);
            at[1 + n] = '"';
            w->mark(w->used);
            w->mark(w->used + 1 + n);
            w->used += n + 2;
            return true;
        }
    }
    return write_string_slow(w, s, n, latin1, readable);
}

// Floats are rare in Gateway payloads and Big only runs for non-integral doubles: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
// ------------------------------------------------------------- doubles
// Exact decimal digits of a double, rounded half-even to 17 significant
// digits (always enough to round-trip); integral values below 2^63 exactly.
struct Big {
    u32 words[84];
    u32 count;
    void multiply(u32 factor) noexcept {
        u64 carry = 0;
        for (u32 i = 0; i < count; ++i) {
            const u64 v = static_cast<u64>(words[i]) * factor + carry;
            words[i] = static_cast<u32>(v);
            carry = v >> 32;
        }
        if (carry)
            words[count++] = static_cast<u32>(carry);
    }
    u32 divide(u32 divisor) noexcept {
        u64 rest = 0;
        for (u32 i = count; i-- > 0;) {
            const u64 v = rest << 32 | words[i];
            words[i] = static_cast<u32>(v / divisor);
            rest = v % divisor;
        }
        while (count && !words[count - 1])
            --count;
        return static_cast<u32>(rest);
    }
};

u32 format_double(u64 bits, char* out) noexcept {
    const bool negative = bits >> 63;
    const u32 exponent = static_cast<u32>(bits >> 52) & 0x7FF;
    const u64 fraction = bits & ((1ull << 52) - 1);
    if (exponent == 0x7FF) {
        copy_bytes(out, "null", 4); // NaN and infinities have no JSON form.
        return 4;
    }
    if (!exponent && !fraction) {
        out[0] = '0';
        return 1;
    }
    const u64 mantissa = exponent ? fraction | 1ull << 52 : fraction;
    const i32 power = exponent ? static_cast<i32>(exponent) - 1075 : -1074;
    u32 n = 0;
    if (negative)
        out[n++] = '-';
    if (power >= 0 && power <= 10) {
        n += decimal_write(out + n, mantissa << power);
        return n;
    }
    if (power < 0 && power > -64 && !(mantissa & ((1ull << -power) - 1))) {
        n += decimal_write(out + n, mantissa >> -power);
        return n;
    }
    Big big;
    big.words[0] = static_cast<u32>(mantissa);
    big.words[1] = static_cast<u32>(mantissa >> 32);
    big.count = big.words[1] ? 2 : 1;
    i32 decimal_shift = 0;
    if (power > 0) {
        for (i32 i = 0; i < power; ++i)
            big.multiply(2);
    } else {
        for (i32 i = 0; i < -power; ++i)
            big.multiply(5);
        decimal_shift = power;
    }
    char digits[800];
    u32 count = 0;
    while (big.count) {
        u32 chunk = big.divide(1000000000);
        for (u32 k = 0; k < 9; ++k) {
            digits[count++] = static_cast<char>('0' + chunk % 10);
            chunk /= 10;
        }
    }
    while (count > 1 && digits[count - 1] == '0')
        --count;
    for (u32 a = 0, b = count - 1; a < b; ++a, --b) {
        const char t = digits[a];
        digits[a] = digits[b];
        digits[b] = t;
    }
    i32 scientific = static_cast<i32>(count) - 1 + decimal_shift;
    u32 keep = count;
    if (count > 17) {
        keep = 17;
        bool up = digits[17] > '5';
        if (digits[17] == '5') {
            bool rest = false;
            for (u32 k = 18; k < count; ++k)
                rest |= digits[k] != '0';
            up = rest || ((digits[16] - '0') & 1);
        }
        if (up) {
            u32 k = 17;
            while (k > 0 && digits[k - 1] == '9')
                digits[--k] = '0';
            if (k == 0) {
                digits[0] = '1';
                ++scientific;
            } else {
                ++digits[k - 1];
            }
        }
    }
    while (keep > 1 && digits[keep - 1] == '0')
        --keep;
    out[n++] = digits[0];
    if (keep > 1) {
        out[n++] = '.';
        copy_bytes(out + n, digits + 1, keep - 1);
        n += keep - 1;
    }
    out[n++] = 'e';
    if (scientific < 0) {
        out[n++] = '-';
        scientific = -scientific;
    }
    n += decimal_write(out + n, static_cast<u64>(scientific));
    return n;
}

// FLOAT_EXT: "%.20e" text, NUL padded; it must already be a JSON number.
bool float_text(const u8* p, u32* length) noexcept {
    u32 n = 0;
    while (n < 31 && p[n])
        ++n;
    u32 i = 0;
    if (i < n && p[i] == '-')
        ++i;
    const u32 integral = i;
    while (i < n && p[i] >= '0' && p[i] <= '9')
        ++i;
    if (i == integral || (p[integral] == '0' && i - integral > 1))
        return false;
    if (i < n && p[i] == '.') {
        const u32 fraction = ++i;
        while (i < n && p[i] >= '0' && p[i] <= '9')
            ++i;
        if (i == fraction)
            return false;
    }
    if (i < n && (p[i] == 'e' || p[i] == 'E')) {
        ++i;
        if (i < n && (p[i] == '+' || p[i] == '-'))
            ++i;
        const u32 digits = i;
        while (i < n && p[i] >= '0' && p[i] <= '9')
            ++i;
        if (i == digits)
            return false;
    }
    for (u32 k = n; k < 31; ++k)
        if (p[k])
            return false;
    *length = n;
    return i == n;
}
#pragma clang attribute pop

// ----------------------------------------------------------- ETF input
struct Reader {
    const u8* data;
    u32 size;
    u32 at;
    bool has(u32 bytes) const noexcept {
        return size - at >= bytes;
    }
};

// Integers that JSON carries as strings: IDs and permission sets under these
// names, and the elements of *_ids lists.
enum class Quoting : u8 { none, value, elements };
__forceinline bool same(const u8* s, const char* text, u32 n) noexcept {
    for (u32 i = 0; i < n; ++i)
        if (s[i] != static_cast<u8>(text[i]))
            return false;
    return true;
}
// The bytes of a name of at most eight characters as one little-endian load reads them.
consteval u64 packed(const char* text) noexcept {
    u64 value = 0;
    for (u32 i = 0; text[i]; ++i)
        value |= static_cast<u64>(static_cast<u8>(text[i])) << (8 * i);
    return value;
}
// "id", "*_id" and the permission names quote their value. "*_ids" quotes the elements of its list.
__forceinline Quoting key_quoting(const u8* s, u32 n) noexcept {
    if (n < 4)
        return n == 2 && byte_load_le16(s) == packed("id") ? Quoting::value : Quoting::none;
    const u32 tail = byte_load_le32(s + n - 4);
    if (tail >> 8 == packed("_id"))
        return Quoting::value;
    if (tail == packed("_ids"))
        return n > 4 ? Quoting::elements : Quoting::none;
    const u64 head = n >= 8 ? byte_load_le64(s) : byte_load_le32(s);
    return (n == 4 && tail == packed("deny")) || (n == 5 && head == packed("allo") && tail == packed("llow")) ||
                   (n == 8 && head == packed("deny_new")) ||
                   (n == 9 && head == packed("allow_ne") && tail == packed("_new")) ||
                   (n == 11 && head == packed("permissi") && tail == packed("ions")) ||
                   (n == 15 && head == packed("permissi") && byte_load_le64(s + 7) == packed("ions_new"))
               ? Quoting::value
               : Quoting::none;
}

struct Frame {
    u32 remaining;
    u8 kind; // 0 list (a tail follows), 1 tuple, 2 map (remaining counts pairs)
    bool first;
    Quoting quoting; // a map's current key, or the list's own
};

enum class AtomValue : u8 { text, null, truth, falsity };
__forceinline AtomValue atom_value(const u8* s, u32 n) noexcept {
    if ((n == 3 && same(s, "nil", 3)) || (n == 4 && same(s, "null", 4)))
        return AtomValue::null;
    if (n == 4 && same(s, "true", 4))
        return AtomValue::truth;
    if (n == 5 && same(s, "false", 5))
        return AtomValue::falsity;
    return AtomValue::text;
}

// Reads an atom header at r.at (tag already consumed): its text and encoding.
__forceinline bool read_atom(Reader* r, u8 tag, const u8** text, u32* length) noexcept {
    const bool small = tag == kSmallAtom || tag == kSmallAtomUtf8;
    if (!r->has(small ? 1 : 2))
        return false;
    const u32 n = small ? r->data[r->at] : (static_cast<u32>(r->data[r->at]) << 8 | r->data[r->at + 1]);
    r->at += small ? 1 : 2;
    if (!r->has(n))
        return false;
    *text = r->data + r->at;
    *length = n;
    r->at += n;
    return true;
}

// The magnitude and sign of an integer term (tag already consumed): a byte, 32 bits, or a big of at most 8 bytes.
__forceinline bool read_integer(Reader* r, u8 tag, u64* magnitude, bool* negative, EtfError* error) noexcept {
    if (tag == kSmallInteger || tag == kInteger) {
        const u32 width = tag == kInteger ? 4 : 1;
        if (!r->has(width))
            return false;
        const i32 v = tag == kInteger ? static_cast<i32>(byte_load_be32(r->data + r->at)) : r->data[r->at];
        r->at += width;
        *negative = v < 0;
        *magnitude = v < 0 ? 0ull - static_cast<u64>(static_cast<i64>(v)) : static_cast<u64>(v);
        return true;
    }
    u32 digits;
    if (tag == kSmallBig) {
        if (!r->has(2))
            return false;
        digits = r->data[r->at++];
    } else {
        if (!r->has(5))
            return false;
        digits = byte_load_be32(r->data + r->at);
        r->at += 4;
    }
    *negative = r->data[r->at++] != 0;
    if (!r->has(digits))
        return false;
    if (digits > 8) {
        *error = EtfError::unsupported_term;
        return false;
    }
    // One load when eight bytes are readable, else byte by byte at the end of the input.
    u64 value = 0;
    if (r->has(8))
        value = byte_load_le64(r->data + r->at) & ~(~0ull << 4 * digits << 4 * digits);
    else
        for (u32 i = 0; i < digits; ++i)
            value |= static_cast<u64>(r->data[r->at + i]) << (8 * i);
    r->at += digits;
    *magnitude = value;
    return true;
}

// A map key that is not a short plain ATOM_EXT name, then ':'.
bool write_key_slow(Reader* r, Writer* w, EtfError* error, Quoting* quoting) noexcept {
    *error = EtfError::truncated;
    *quoting = Quoting::none;
    if (!r->has(1))
        return false;
    const u8 tag = r->data[r->at++];
    const u8* text = nullptr;
    u32 n = 0;
    u64 magnitude = 0;
    bool negative = false;
    switch (tag) {
    case kAtom:
    case kSmallAtom:
    case kAtomUtf8:
    case kSmallAtomUtf8:
        if (!read_atom(r, tag, &text, &n))
            return false;
        break;
    case kBinary:
        if (!r->has(4))
            return false;
        n = byte_load_be32(r->data + r->at);
        r->at += 4;
        if (!r->has(n))
            return false;
        text = r->data + r->at;
        r->at += n;
        break;
    case kSmallInteger:
    case kInteger:
    case kSmallBig:
    case kLargeBig:
        if (!read_integer(r, tag, &magnitude, &negative, error))
            return false;
        break;
    default:
        --r->at;
        *error = EtfError::invalid_key;
        return false;
    }
    *error = EtfError::output_capacity;
    bool written = false;
    if (!text) {
        written = write_integer(w, magnitude, negative, true);
    } else {
        *quoting = key_quoting(text, n);
        // erlpack turns nil into null, which names the property "null".
        if (tag != kBinary && atom_value(text, n) == AtomValue::null)
            written = write_string(w, reinterpret_cast<const u8*>("null"), 4, false, 0);
        else
            written = write_string(w, text, n, tag == kAtom || tag == kSmallAtom,
                                   static_cast<u32>(r->data + r->size - text));
    }
    return written && w->byte(':');
}

// A map key as a JSON name, then ':'. The Gateway's keys are short plain ATOM_EXT names: one store writes such a
// name with its ':'.
__forceinline bool write_key(Reader* r, Writer* w, EtfError* error, Quoting* quoting) noexcept {
    if (r->size - r->at >= 19 && r->data[r->at] == kAtom) {
        const u32 n = byte_load_be16(r->data + r->at + 1);
        const u8* text = r->data + r->at + 3;
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(text));
        if (n - 1 < 13 && !(n == 3 && same(text, "nil", 3)) && !(special_mask(v) & ((1u << n) - 1))) {
            u8* at = w->reserve(19);
            *error = EtfError::output_capacity;
            if (!at)
                return false;
            at[0] = '"';
            _mm_storeu_si128(reinterpret_cast<__m128i*>(at + 1), v);
            at[1 + n] = '"';
            at[2 + n] = ':';
            w->mark(w->used);
            w->mark(w->used + 1 + n);
            w->mark(w->used + 2 + n);
            w->used += n + 3;
            *quoting = key_quoting(text, n);
            r->at += 3 + n;
            return true;
        }
    }
    return write_key_slow(r, w, error, quoting);
}

} // namespace

EtfResult etf_to_json_indexed(ConstSlice<u8> etf, Slice<u8>* output, u32 maximum_output, EtfGrowOutput grow,
                              void* context, Slice<u32> index, u32* index_count) noexcept {
    *index_count = ~0u;
    if (!etf.data || !etf.count || !output || (!output->data && output->count) || (!index.data && index.count))
        return {0, EtfError::invalid_input};
    if (etf[0] != kVersion)
        return {0, EtfError::bad_version};
    Reader r{etf.data, etf.count, 1};
    Writer w{output, output->data, output->count, 0, maximum_output, grow, context, index.data, index.count, 0};
    Frame stack[kEtfMaximumDepth];
    u32 depth = 0;
    // The table of bracket ends (json_index_ends) lies in the upper half of the index storage. An index that
    // grows into that half overwrites the table, and no later pair is written. The levels of a container are
    // an upper bound: the deepest level of the message so far, counted from the container.
    u32 opened[kEtfMaximumDepth];
    u32 deepest = 0;
    const u32 half = index.count / 2;
    u32* const table = index.data + half;
    const auto pair = [&](u32 open, u32 closer, u32 levels) noexcept {
        if (closer < half) {
            table[open] = closer;
            table[closer] = levels;
        }
    };
    bool started = false;
    Quoting quoting = Quoting::none;
    const auto failed = [](EtfError error) noexcept { return EtfResult{0, error}; };
    for (;;) {
        if (depth) {
            Frame& f = stack[depth - 1];
            if (!f.remaining) {
                if (f.kind == 0) {
                    if (!r.has(1))
                        return failed(EtfError::truncated);
                    if (r.data[r.at] != kNil)
                        return failed(EtfError::improper_list);
                    ++r.at;
                }
                const u32 closer = w.marks;
                if (!w.byte(f.kind == 2 ? '}' : ']'))
                    return failed(EtfError::output_capacity);
                deepest = depth > deepest ? depth : deepest;
                pair(opened[depth - 1], closer, deepest - depth + 2);
                if (!--depth)
                    break;
                continue;
            }
            if (!f.first && !w.byte(','))
                return failed(EtfError::output_capacity);
            f.first = false;
            // A map pair is its key and then its value, in this iteration.
            if (f.kind == 2) {
                EtfError error = EtfError::none;
                if (!write_key(&r, &w, &error, &f.quoting))
                    return failed(error);
            }
            --f.remaining;
            quoting = f.quoting;
        } else if (started) {
            break;
        }
        started = true;
        // A map value under an ID key, or an element of a *_ids list.
        const bool quote = depth && (stack[depth - 1].kind == 2 ? quoting == Quoting::value
                                                                 : stack[depth - 1].quoting == Quoting::elements);
        if (!r.has(1))
            return failed(EtfError::truncated);
        const u32 term = r.at;
        const u8 tag = r.data[r.at++];
        bool ok = true;
        switch (tag) {
        case kSmallInteger:
        case kInteger:
        case kSmallBig:
        case kLargeBig: {
            u64 magnitude = 0;
            bool negative = false;
            EtfError error = EtfError::truncated;
            if (!read_integer(&r, tag, &magnitude, &negative, &error))
                return failed(error);
            ok = write_integer(&w, magnitude, negative, quote);
            break;
        }
        case kNewFloat: {
            if (!r.has(8))
                return failed(EtfError::truncated);
            u64 bits = 0;
            for (u32 i = 0; i < 8; ++i)
                bits = bits << 8 | r.data[r.at + i];
            r.at += 8;
            char text[40];
            ok = w.bytes(text, format_double(bits, text));
            break;
        }
        case kFloat: {
            u32 n = 0;
            if (!r.has(31))
                return failed(EtfError::truncated);
            if (!float_text(r.data + r.at, &n))
                return failed(EtfError::unsupported_term);
            ok = w.bytes(r.data + r.at, n);
            r.at += 31;
            break;
        }
        case kAtom:
        case kSmallAtom:
        case kAtomUtf8:
        case kSmallAtomUtf8: {
            const u8* text = nullptr;
            u32 n = 0;
            if (!read_atom(&r, tag, &text, &n))
                return failed(EtfError::truncated);
            switch (atom_value(text, n)) {
            case AtomValue::null: ok = w.bytes("null", 4); break;
            case AtomValue::truth: ok = w.bytes("true", 4); break;
            case AtomValue::falsity: ok = w.bytes("false", 5); break;
            default:
                ok = write_string(&w, text, n, tag == kAtom || tag == kSmallAtom,
                                  static_cast<u32>(r.data + r.size - text));
                break;
            }
            break;
        }
        case kBinary: {
            if (!r.has(4))
                return failed(EtfError::truncated);
            const u32 n = byte_load_be32(r.data + r.at);
            r.at += 4;
            if (!r.has(n))
                return failed(EtfError::truncated);
            ok = write_string(&w, r.data + r.at, n, false, r.size - r.at);
            r.at += n;
            break;
        }
        case kNil:
            pair(w.marks, w.marks + 1, 1);
            w.mark(w.used);
            w.mark(w.used + 1);
            ok = w.bytes("[]", 2);
            break;
        case kString: {
            if (!r.has(2))
                return failed(EtfError::truncated);
            const u32 n = static_cast<u32>(r.data[r.at]) << 8 | r.data[r.at + 1];
            r.at += 2;
            if (!r.has(n))
                return failed(EtfError::truncated);
            const u32 open = w.marks;
            ok = w.byte('[');
            for (u32 i = 0; ok && i < n; ++i)
                ok = (!i || w.byte(',')) && write_integer(&w, r.data[r.at + i], false, false);
            ok = ok && w.byte(']');
            if (ok)
                pair(open, w.marks - 1, 1);
            r.at += n;
            break;
        }
        case kList:
        case kSmallTuple:
        case kLargeTuple:
        case kMap: {
            const u32 width = tag == kSmallTuple ? 1 : 4;
            if (!r.has(width))
                return failed(EtfError::truncated);
            const u32 n = width == 1 ? r.data[r.at] : byte_load_be32(r.data + r.at);
            r.at += width;
            // Every element takes at least one byte (two for a map pair).
            if (n > (r.size - r.at) / (tag == kMap ? 2 : 1))
                return failed(EtfError::truncated);
            if (depth == kEtfMaximumDepth)
                return failed(EtfError::depth_limit);
            // A list under a *_ids key quotes its integers.
            const Quoting own = tag != kMap && depth && stack[depth - 1].kind == 2 ? quoting : Quoting::none;
            opened[depth] = w.marks;
            stack[depth++] = {n, static_cast<u8>(tag == kList ? 0 : tag == kMap ? 2 : 1), true, own};
            ok = w.byte(tag == kMap ? '{' : '[');
            break;
        }
        default:
            r.at = term;
            return failed(EtfError::unsupported_term);
        }
        if (!ok)
            return failed(EtfError::output_capacity);
        if (!depth)
            break;
    }
    if (r.at != r.size)
        return failed(EtfError::trailing_data);
    *index_count = w.marks;
    return {w.used, EtfError::none};
}

// The encoder writes outgoing commands only, one per request: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
// ---------------------------------------------------------- JSON input
namespace {

struct Encoder {
    const char* p;
    const char* end;
    u8* out;
    u32 capacity;
    u32 used;
    bool overflow;

    bool room(u32 bytes) noexcept {
        if (capacity - used < bytes) {
            overflow = true;
            return false;
        }
        return true;
    }
    void put(u8 value) noexcept {
        if (room(1))
            out[used++] = value;
    }
    void store32(u32 at, u32 value) noexcept {
        out[at] = static_cast<u8>(value >> 24);
        out[at + 1] = static_cast<u8>(value >> 16);
        out[at + 2] = static_cast<u8>(value >> 8);
        out[at + 3] = static_cast<u8>(value);
    }
    void put32(u32 value) noexcept {
        if (room(4)) {
            store32(used, value);
            used += 4;
        }
    }
    void skip_space() noexcept {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t'))
            ++p;
    }
    bool literal(const char* text, u32 n) noexcept {
        if (static_cast<u32>(end - p) < n || !bytes_equal(p, text, n))
            return false;
        p += n;
        return true;
    }
    void atom(const char* text, u32 n) noexcept {
        if (room(2 + n)) {
            out[used++] = kSmallAtom;
            out[used++] = static_cast<u8>(n);
            copy_bytes(out + used, text, n);
            used += n;
        }
    }
    bool string() noexcept {
        const char* const begin = ++p;
        bool escapes = false;
        while (p < end && *p != '"') {
            if (*p == '\\') {
                escapes = true;
                ++p;
            }
            ++p;
        }
        if (p >= end)
            return false;
        const JsonString text{{begin, static_cast<u32>(p - begin)}, 0, escapes};
        ++p;
        const auto size = json_string_decode_utf8(text, {});
        if (!size.ok() && size.error != JsonStringDecodeError::output_too_small)
            return false;
        put(kBinary);
        put32(size.bytes);
        if (!room(size.bytes))
            return true;
        if (!json_string_decode_utf8(text, {reinterpret_cast<char*>(out + used), size.bytes}).ok())
            return false;
        used += size.bytes;
        return true;
    }
    bool number() noexcept {
        const bool negative = *p == '-';
        if (negative)
            ++p;
        const char* const digits = p;
        u64 value = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            const u64 digit = static_cast<u64>(*p - '0');
            if (value > (~0ull - digit) / 10)
                return false;
            value = value * 10 + digit;
            ++p;
        }
        if (p == digits || (*digits == '0' && p - digits > 1) ||
            (p < end && (*p == '.' || *p == 'e' || *p == 'E')))
            return false;
        if (!negative && value <= 255) {
            put(kSmallInteger);
            put(static_cast<u8>(value));
        } else if (negative ? value <= 0x80000000ull : value <= 0x7FFFFFFFull) {
            put(kInteger);
            put32(static_cast<u32>(negative ? 0ull - value : value));
        } else if (!negative && value <= 0xFFFFFFFFull) {
            put(kSmallBig);
            put(4);
            put(0);
            for (u32 i = 0; i < 4; ++i)
                put(static_cast<u8>(value >> (8 * i)));
        } else {
            // A JavaScript number outside uint32: erlpack packs the double.
            if (value >= kSafeInteger)
                return false;
            u64 bits = 0;
            if (value) {
                unsigned long top = 0;
                _BitScanReverse64(&top, value);
                bits = static_cast<u64>(1023 + top) << 52 | ((value << (52 - top)) & ((1ull << 52) - 1));
            }
            if (negative)
                bits |= 1ull << 63;
            put(kNewFloat);
            put32(static_cast<u32>(bits >> 32));
            put32(static_cast<u32>(bits));
        }
        return true;
    }
    bool value(u32 depth) noexcept {
        skip_space();
        if (p >= end || depth > 32)
            return false;
        const char c = *p;
        if (c == '{') {
            ++p;
            put(kMap);
            const u32 arity_at = used;
            put32(0);
            u32 arity = 0;
            skip_space();
            if (p < end && *p == '}') {
                ++p;
                return true;
            }
            for (;;) {
                skip_space();
                if (p >= end || *p != '"' || !string())
                    return false;
                skip_space();
                if (p >= end || *p++ != ':' || !value(depth + 1))
                    return false;
                ++arity;
                skip_space();
                if (p < end && *p == ',') {
                    ++p;
                    continue;
                }
                if (p < end && *p == '}') {
                    ++p;
                    break;
                }
                return false;
            }
            if (!overflow)
                store32(arity_at, arity);
            return true;
        }
        if (c == '[') {
            ++p;
            skip_space();
            if (p < end && *p == ']') {
                ++p;
                put(kNil);
                return true;
            }
            put(kList);
            const u32 length_at = used;
            put32(0);
            u32 length = 0;
            for (;;) {
                if (!value(depth + 1))
                    return false;
                ++length;
                skip_space();
                if (p < end && *p == ',') {
                    ++p;
                    continue;
                }
                if (p < end && *p == ']') {
                    ++p;
                    break;
                }
                return false;
            }
            put(kNil);
            if (!overflow)
                store32(length_at, length);
            return true;
        }
        if (c == '"')
            return string();
        if (c == 't') {
            atom("true", 4);
            return literal("true", 4);
        }
        if (c == 'f') {
            atom("false", 5);
            return literal("false", 5);
        }
        if (c == 'n') {
            atom("nil", 3);
            return literal("null", 4);
        }
        if (c == '-' || (c >= '0' && c <= '9'))
            return number();
        return false;
    }
};

} // namespace

EtfResult etf_from_json(Utf8View json, Slice<u8> output) noexcept {
    if (!json.data || !json.bytes || (!output.data && output.count))
        return {0, EtfError::invalid_input};
    Encoder e{json.data, json.data + json.bytes, output.data, output.count, 0, false};
    e.put(kVersion);
    const bool valid = e.value(0);
    e.skip_space();
    if (!valid || e.p != e.end)
        return {0, EtfError::invalid_json};
    if (e.overflow)
        return {0, EtfError::output_capacity};
    return {e.used, EtfError::none};
}

#pragma clang attribute pop

} // namespace maicord
