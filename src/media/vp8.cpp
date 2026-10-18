#include "maicord/media/vp8.hpp"
#include "maicord/core/byte_order.hpp"

#include "maicord/sys/intrin.hpp"
#include "maicord/sys/simd.hpp"

#pragma intrinsic(_byteswap_uint64)

namespace maicord {
namespace {

// RFC 6386 boolean decoder with the bit-serial normalization loop replaced by
// one shift: identical state after every call, including the truncation
// point (the shift that needs a byte past the end) and the state left there.
__forceinline u32 bool_read(Vp8BoolReader* reader, u32 probability) noexcept {
    if (reader->error != Vp8BoolError::none) {
        return 0;
    }
    const u32 split = 1u + ((reader->range - 1u) * probability >> 8);
    const u32 threshold = split << 8;
    u32 bit = 0;
    u32 range = split;
    if (reader->value >= threshold) {
        bit = 1;
        range = reader->range - split;
        reader->value -= threshold;
    }
    if (range < 128) {
        unsigned long top = 0;
        _BitScanReverse(&top, range);
        const u32 shift = 7u - top;
        const u32 room = 8u - reader->bit_count;
        if (shift < room) {
            reader->value <<= shift;
            reader->bit_count = static_cast<u8>(reader->bit_count + shift);
        } else {
            reader->value <<= room;
            if (reader->byte_offset >= reader->bytes.count) {
                reader->range = range << room;
                reader->bit_count = 0;
                reader->error = Vp8BoolError::truncated;
                return bit;
            }
            reader->value |= reader->bytes.data[reader->byte_offset++];
            reader->value <<= shift - room;
            reader->bit_count = static_cast<u8>(shift - room);
        }
        range <<= shift;
    }
    reader->range = range;
    return bit;
}

// Windowed form of the same decoder for the macroblock loops: up to 56
// lookahead bits, refilled seven bytes at a time, so a decision is a few
// register operations. to_window/from_window convert exactly to and from the
// bit-serial state, and a read fails at the same shift as the bit-serial
// reader: the one that leaves no lookahead bit and no byte to load.
struct WindowReader {
    const u8* data;
    u64 value; // the active window is value >> bits; `bits` lookahead bits below it
    u32 count;
    u32 offset; // next byte not yet in value
    i32 bits;
    u32 range; // 128..255 between reads
    Vp8BoolError error;

    constexpr bool ok() const noexcept {
        return error == Vp8BoolError::none;
    }
};

__forceinline WindowReader to_window(const Vp8BoolReader& reader) noexcept {
    WindowReader window{};
    window.data = reader.bytes.data;
    window.count = reader.bytes.count;
    window.offset = reader.byte_offset;
    // The low bit_count bits of the bit-serial window are not loaded yet.
    window.value = reader.value >> reader.bit_count;
    window.bits = 8 - static_cast<i32>(reader.bit_count);
    window.range = reader.range;
    window.error = reader.error;
    return window;
}

void from_window(const WindowReader& window, Vp8BoolReader* reader) noexcept {
    if (!window.ok()) {
        if (reader->ok()) {
            // window_read left the bit-serial state of its failing shift.
            reader->value = static_cast<u32>(window.value);
            reader->range = window.range;
            reader->bit_count = 0;
            reader->byte_offset = window.count;
            reader->error = window.error;
        }
        return;
    }
    u64 value = window.value;
    i32 bits = window.bits;
    u32 offset = window.offset;
    while (bits < 1 && offset < window.count) {
        value = value << 8 | window.data[offset++];
        bits += 8;
    }
    while (bits > 8) {
        value >>= 8;
        bits -= 8;
        --offset;
    }
    const u32 pending = static_cast<u32>(8 - bits);
    reader->value = static_cast<u32>(value << pending);
    reader->bit_count = static_cast<u8>(pending);
    reader->byte_offset = offset;
    reader->range = window.range;
}

struct WindowRefill {
    u64 value;
    u32 offset;
    i32 bits;
};

// Once per seven input bytes: out of line, by value so the caller's reader
// stays in registers.
__declspec(noinline) WindowRefill window_refill(const u8* data, u32 count, u64 value, u32 offset,
                                                i32 bits) noexcept {
    if (count - offset >= 8) {
        const u64 raw =
            static_cast<u64>(_mm_cvtsi128_si64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(data + offset))));
        return WindowRefill{value << 56 | _byteswap_uint64(raw) >> 8, offset + 7, bits + 56};
    }
    while (bits < 0 && offset < count) {
        value = value << 8 | data[offset++];
        bits += 8;
    }
    return WindowRefill{value, offset, bits};
}

__forceinline u32 window_read(WindowReader* reader, u32 probability) noexcept {
    if (reader->error != Vp8BoolError::none) {
        return 0;
    }
    if (reader->bits < 0) {
        const WindowRefill refill =
            window_refill(reader->data, reader->count, reader->value, reader->offset, reader->bits);
        reader->value = refill.value;
        reader->offset = refill.offset;
        reader->bits = refill.bits;
    }
    const u32 split = 1u + ((reader->range - 1u) * probability >> 8);
    u32 bit = 0;
    u32 range = split;
    if (static_cast<u32>(reader->value >> reader->bits) >= split) {
        bit = 1;
        range = reader->range - split;
        reader->value -= static_cast<u64>(split) << reader->bits;
    }
    if (range < 128) {
        unsigned long top = 0;
        _BitScanReverse(&top, range);
        const u32 shift = 7u - top;
        if (reader->offset >= reader->count && shift >= static_cast<u32>(reader->bits)) {
            // The bit-serial reader shifts out its last lookahead bit, then
            // finds no byte to load: keep that state for from_window.
            reader->range = range << reader->bits;
            reader->value <<= 8;
            reader->error = Vp8BoolError::truncated;
            return bit;
        }
        reader->bits -= static_cast<i32>(shift);
        range <<= shift;
    }
    reader->range = range;
    return bit;
}

struct CoefficientUpdateProbabilityException {
    u16 index;
    u8 probability;
    u8 reserved;
};

// The non-255 entries of RFC 6386 section 13.4's 4 x 8 x 3 x 11
// update-probability table, in traversal order; every other entry is 255.
constexpr CoefficientUpdateProbabilityException kCoefficientUpdateProbabilityExceptions[] = {
    {33, 176},  {34, 246},  {44, 223},  {45, 241},  {46, 252},   {55, 249},   {56, 253},  {57, 253},
    {67, 244},  {68, 252},  {77, 234},  {78, 254},  {79, 254},   {88, 253},   {100, 246}, {101, 254},
    {110, 239}, {111, 253}, {112, 254}, {121, 254}, {123, 254},  {133, 248},  {134, 254}, {143, 251},
    {145, 254}, {166, 253}, {167, 254}, {176, 251}, {177, 254},  {178, 254},  {187, 254}, {189, 254},
    {199, 254}, {200, 253}, {202, 254}, {209, 250}, {211, 254},  {213, 254},  {220, 254}, {264, 217},
    {275, 225}, {276, 252}, {277, 241}, {278, 253}, {281, 254},  {286, 234},  {287, 250}, {288, 241},
    {289, 250}, {290, 253}, {292, 253}, {293, 254}, {298, 254},  {308, 223},  {309, 254}, {310, 254},
    {319, 238}, {320, 253}, {321, 254}, {322, 254}, {331, 248},  {332, 254},  {341, 249}, {342, 254},
    {364, 253}, {374, 247}, {375, 254}, {397, 253}, {398, 254},  {407, 252},  {430, 254}, {431, 254},
    {440, 253}, {463, 254}, {464, 253}, {473, 250}, {484, 254},  {528, 186},  {529, 251}, {530, 250},
    {539, 234}, {540, 251}, {541, 244}, {542, 254}, {550, 251},  {551, 251},  {552, 243}, {553, 253},
    {554, 254}, {556, 254}, {562, 253}, {563, 254}, {572, 236},  {573, 253},  {574, 254}, {583, 251},
    {584, 253}, {585, 253}, {586, 254}, {587, 254}, {595, 254},  {596, 254},  {605, 254}, {606, 254},
    {607, 254}, {628, 254}, {638, 254}, {639, 254}, {649, 254},  {671, 254},  {792, 248}, {803, 250},
    {804, 254}, {805, 252}, {806, 254}, {814, 248}, {815, 254},  {816, 249},  {817, 253}, {826, 253},
    {827, 253}, {836, 246}, {837, 253}, {838, 253}, {847, 252},  {848, 254},  {849, 251}, {850, 254},
    {851, 254}, {859, 254}, {860, 252}, {869, 248}, {870, 254},  {871, 253},  {880, 253}, {882, 254},
    {883, 254}, {892, 251}, {893, 254}, {902, 245}, {903, 251},  {904, 254},  {913, 253}, {914, 253},
    {915, 254}, {925, 251}, {926, 253}, {935, 252}, {936, 253},  {937, 254},  {947, 254}, {958, 252},
    {968, 249}, {970, 254}, {981, 254}, {992, 253}, {1001, 250}, {1034, 254},
};

// RFC 6386 section 13.5, flattened in section 9.9 traversal order.
constexpr u8 kDefaultCoefficientProbability[] = {
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 253, 136, 254, 255, 228, 219, 128, 128, 128,
    128, 128, 189, 129, 242, 255, 227, 213, 255, 219, 128, 128, 128, 106, 126, 227, 252, 214, 209, 255, 255,
    128, 128, 128, 1,   98,  248, 255, 236, 226, 255, 255, 128, 128, 128, 181, 133, 238, 254, 221, 234, 255,
    154, 128, 128, 128, 78,  134, 202, 247, 198, 180, 255, 219, 128, 128, 128, 1,   185, 249, 255, 243, 255,
    128, 128, 128, 128, 128, 184, 150, 247, 255, 236, 224, 128, 128, 128, 128, 128, 77,  110, 216, 255, 236,
    230, 128, 128, 128, 128, 128, 1,   101, 251, 255, 241, 255, 128, 128, 128, 128, 128, 170, 139, 241, 252,
    236, 209, 255, 255, 128, 128, 128, 37,  116, 196, 243, 228, 255, 255, 255, 128, 128, 128, 1,   204, 254,
    255, 245, 255, 128, 128, 128, 128, 128, 207, 160, 250, 255, 238, 128, 128, 128, 128, 128, 128, 102, 103,
    231, 255, 211, 171, 128, 128, 128, 128, 128, 1,   152, 252, 255, 240, 255, 128, 128, 128, 128, 128, 177,
    135, 243, 255, 234, 225, 128, 128, 128, 128, 128, 80,  129, 211, 255, 194, 224, 128, 128, 128, 128, 128,
    1,   1,   255, 128, 128, 128, 128, 128, 128, 128, 128, 246, 1,   255, 128, 128, 128, 128, 128, 128, 128,
    128, 255, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 198, 35,  237, 223, 193, 187, 162, 160, 145,
    155, 62,  131, 45,  198, 221, 172, 176, 220, 157, 252, 221, 1,   68,  47,  146, 208, 149, 167, 221, 162,
    255, 223, 128, 1,   149, 241, 255, 221, 224, 255, 255, 128, 128, 128, 184, 141, 234, 253, 222, 220, 255,
    199, 128, 128, 128, 81,  99,  181, 242, 176, 190, 249, 202, 255, 255, 128, 1,   129, 232, 253, 214, 197,
    242, 196, 255, 255, 128, 99,  121, 210, 250, 201, 198, 255, 202, 128, 128, 128, 23,  91,  163, 242, 170,
    187, 247, 210, 255, 255, 128, 1,   200, 246, 255, 234, 255, 128, 128, 128, 128, 128, 109, 178, 241, 255,
    231, 245, 255, 255, 128, 128, 128, 44,  130, 201, 253, 205, 192, 255, 255, 128, 128, 128, 1,   132, 239,
    251, 219, 209, 255, 165, 128, 128, 128, 94,  136, 225, 251, 218, 190, 255, 255, 128, 128, 128, 22,  100,
    174, 245, 186, 161, 255, 199, 128, 128, 128, 1,   182, 249, 255, 232, 235, 128, 128, 128, 128, 128, 124,
    143, 241, 255, 227, 234, 128, 128, 128, 128, 128, 35,  77,  181, 251, 193, 211, 255, 205, 128, 128, 128,
    1,   157, 247, 255, 236, 231, 255, 255, 128, 128, 128, 121, 141, 235, 255, 225, 227, 255, 255, 128, 128,
    128, 45,  99,  188, 251, 195, 217, 255, 224, 128, 128, 128, 1,   1,   251, 255, 213, 255, 128, 128, 128,
    128, 128, 203, 1,   248, 255, 255, 128, 128, 128, 128, 128, 128, 137, 1,   177, 255, 224, 255, 128, 128,
    128, 128, 128, 253, 9,   248, 251, 207, 208, 255, 192, 128, 128, 128, 175, 13,  224, 243, 193, 185, 249,
    198, 255, 255, 128, 73,  17,  171, 221, 161, 179, 236, 167, 255, 234, 128, 1,   95,  247, 253, 212, 183,
    255, 255, 128, 128, 128, 239, 90,  244, 250, 211, 209, 255, 255, 128, 128, 128, 155, 77,  195, 248, 188,
    195, 255, 255, 128, 128, 128, 1,   24,  239, 251, 218, 219, 255, 205, 128, 128, 128, 201, 51,  219, 255,
    196, 186, 128, 128, 128, 128, 128, 69,  46,  190, 239, 201, 218, 255, 228, 128, 128, 128, 1,   191, 251,
    255, 255, 128, 128, 128, 128, 128, 128, 223, 165, 249, 255, 213, 255, 128, 128, 128, 128, 128, 141, 124,
    248, 255, 255, 128, 128, 128, 128, 128, 128, 1,   16,  248, 255, 255, 128, 128, 128, 128, 128, 128, 190,
    36,  230, 255, 236, 255, 128, 128, 128, 128, 128, 149, 1,   255, 128, 128, 128, 128, 128, 128, 128, 128,
    1,   226, 255, 128, 128, 128, 128, 128, 128, 128, 128, 247, 192, 255, 128, 128, 128, 128, 128, 128, 128,
    128, 240, 128, 255, 128, 128, 128, 128, 128, 128, 128, 128, 1,   134, 252, 255, 255, 128, 128, 128, 128,
    128, 128, 213, 62,  250, 255, 255, 128, 128, 128, 128, 128, 128, 55,  93,  255, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 202, 24,  213, 235, 186, 191,
    220, 160, 240, 175, 255, 126, 38,  182, 232, 169, 184, 228, 174, 255, 187, 128, 61,  46,  138, 219, 151,
    178, 240, 170, 255, 216, 128, 1,   112, 230, 250, 199, 191, 247, 159, 255, 255, 128, 166, 109, 228, 252,
    211, 215, 255, 174, 128, 128, 128, 39,  77,  162, 232, 172, 180, 245, 178, 255, 255, 128, 1,   52,  220,
    246, 198, 199, 249, 220, 255, 255, 128, 124, 74,  191, 243, 183, 193, 250, 221, 255, 255, 128, 24,  71,
    130, 219, 154, 170, 243, 182, 255, 255, 128, 1,   182, 225, 249, 219, 240, 255, 224, 128, 128, 128, 149,
    150, 226, 252, 216, 205, 255, 171, 128, 128, 128, 28,  108, 170, 242, 183, 194, 254, 223, 255, 255, 128,
    1,   81,  230, 252, 204, 203, 255, 192, 128, 128, 128, 123, 102, 209, 247, 188, 196, 255, 233, 128, 128,
    128, 20,  95,  153, 243, 164, 173, 255, 203, 128, 128, 128, 1,   222, 248, 255, 216, 213, 128, 128, 128,
    128, 128, 168, 175, 246, 252, 235, 205, 255, 255, 128, 128, 128, 47,  116, 215, 255, 211, 212, 255, 255,
    128, 128, 128, 1,   121, 236, 253, 212, 214, 255, 255, 128, 128, 128, 141, 84,  213, 252, 201, 202, 255,
    219, 128, 128, 128, 42,  80,  160, 240, 162, 185, 255, 205, 128, 128, 128, 1,   1,   255, 128, 128, 128,
    128, 128, 128, 128, 128, 244, 1,   255, 128, 128, 128, 128, 128, 128, 128, 128, 238, 1,   255, 128, 128,
    128, 128, 128, 128, 128, 128,
};

static_assert(sizeof(kDefaultCoefficientProbability) == kVp8CoefficientProbabilityCount);

constexpr u64 probability_sum(const u8* values, u32 count) noexcept {
    u64 result = 0;
    for (u32 index = 0; index < count; ++index) {
        result += values[index];
    }
    return result;
}

constexpr u64 probability_weighted_sum(const u8* values, u32 count) noexcept {
    u64 result = 0;
    for (u32 index = 0; index < count; ++index) {
        result += static_cast<u64>(index + 1u) * values[index];
    }
    return result;
}

constexpr u64 update_probability_sum() noexcept {
    u64 result = static_cast<u64>(kVp8CoefficientProbabilityCount) * 255u;
    for (u32 index = 0; index < sizeof(kCoefficientUpdateProbabilityExceptions) /
                                    sizeof(kCoefficientUpdateProbabilityExceptions[0]);
         ++index) {
        result -= 255u - kCoefficientUpdateProbabilityExceptions[index].probability;
    }
    return result;
}

constexpr u64 update_probability_weighted_sum() noexcept {
    u64 result = 0;
    u32 exception_cursor = 0;
    for (u32 index = 0; index < kVp8CoefficientProbabilityCount; ++index) {
        u8 probability = 255;
        if (exception_cursor < sizeof(kCoefficientUpdateProbabilityExceptions) /
                                   sizeof(kCoefficientUpdateProbabilityExceptions[0]) &&
            kCoefficientUpdateProbabilityExceptions[exception_cursor].index == index) {
            probability = kCoefficientUpdateProbabilityExceptions[exception_cursor].probability;
            ++exception_cursor;
        }
        result += static_cast<u64>(index + 1u) * probability;
    }
    return result;
}

static_assert(probability_sum(kDefaultCoefficientProbability, kVp8CoefficientProbabilityCount) == 174'918u);
static_assert(probability_weighted_sum(kDefaultCoefficientProbability, kVp8CoefficientProbabilityCount) ==
              92'112'737u);
static_assert(update_probability_sum() == 268'469u);
static_assert(update_probability_weighted_sum() == 142'003'330u);

// RFC 6386 section 11.5: [above mode][left mode][tree node].
constexpr u8 kSubblockModeProbability[] = {
    231, 120, 48,  89,  115, 113, 120, 152, 112, 152, 179, 64,  126, 170, 118, 46,  70,  95,  175, 69,  143,
    80,  85,  82,  72,  155, 103, 56,  58,  10,  171, 218, 189, 17,  13,  152, 114, 26,  17,  163, 44,  195,
    21,  10,  173, 121, 24,  80,  195, 26,  62,  44,  64,  85,  144, 71,  10,  38,  171, 213, 144, 34,  26,
    170, 46,  55,  19,  136, 160, 33,  206, 71,  63,  20,  8,   114, 114, 208, 12,  9,   226, 81,  40,  11,
    96,  182, 84,  29,  16,  36,  134, 183, 89,  137, 98,  101, 106, 165, 148, 72,  187, 100, 130, 157, 111,
    32,  75,  80,  66,  102, 167, 99,  74,  62,  40,  234, 128, 41,  53,  9,   178, 241, 141, 26,  8,   107,
    74,  43,  26,  146, 73,  166, 49,  23,  157, 65,  38,  105, 160, 51,  52,  31,  115, 128, 104, 79,  12,
    27,  217, 255, 87,  17,  7,   87,  68,  71,  44,  114, 51,  15,  186, 23,  47,  41,  14,  110, 182, 183,
    21,  17,  194, 66,  45,  25,  102, 197, 189, 23,  18,  22,  88,  88,  147, 150, 42,  46,  45,  196, 205,
    43,  97,  183, 117, 85,  38,  35,  179, 61,  39,  53,  200, 87,  26,  21,  43,  232, 171, 56,  34,  51,
    104, 114, 102, 29,  93,  77,  39,  28,  85,  171, 58,  165, 90,  98,  64,  34,  22,  116, 206, 23,  34,
    43,  166, 73,  107, 54,  32,  26,  51,  1,   81,  43,  31,  68,  25,  106, 22,  64,  171, 36,  225, 114,
    34,  19,  21,  102, 132, 188, 16,  76,  124, 62,  18,  78,  95,  85,  57,  50,  48,  51,  193, 101, 35,
    159, 215, 111, 89,  46,  111, 60,  148, 31,  172, 219, 228, 21,  18,  111, 112, 113, 77,  85,  179, 255,
    38,  120, 114, 40,  42,  1,   196, 245, 209, 10,  25,  109, 88,  43,  29,  140, 166, 213, 37,  43,  154,
    61,  63,  30,  155, 67,  45,  68,  1,   209, 100, 80,  8,   43,  154, 1,   51,  26,  71,  142, 78,  78,
    16,  255, 128, 34,  197, 171, 41,  40,  5,   102, 211, 183, 4,   1,   221, 51,  50,  17,  168, 209, 192,
    23,  25,  82,  138, 31,  36,  171, 27,  166, 38,  44,  229, 67,  87,  58,  169, 82,  115, 26,  59,  179,
    63,  59,  90,  180, 59,  166, 93,  73,  154, 40,  40,  21,  116, 143, 209, 34,  39,  175, 47,  15,  16,
    183, 34,  223, 49,  45,  183, 46,  17,  33,  183, 6,   98,  15,  32,  183, 57,  46,  22,  24,  128, 1,
    54,  17,  37,  65,  32,  73,  115, 28,  128, 23,  128, 205, 40,  3,   9,   115, 51,  192, 18,  6,   223,
    87,  37,  9,   115, 59,  77,  64,  21,  47,  104, 55,  44,  218, 9,   54,  53,  130, 226, 64,  90,  70,
    205, 40,  41,  23,  26,  57,  54,  57,  112, 184, 5,   41,  38,  166, 213, 30,  34,  26,  133, 152, 116,
    10,  32,  134, 39,  19,  53,  221, 26,  114, 32,  73,  255, 31,  9,   65,  234, 2,   15,  1,   118, 73,
    75,  32,  12,  51,  192, 255, 160, 43,  51,  88,  31,  35,  67,  102, 85,  55,  186, 85,  56,  21,  23,
    111, 59,  205, 45,  37,  192, 55,  38,  70,  124, 73,  102, 1,   34,  98,  125, 98,  42,  88,  104, 85,
    117, 175, 82,  95,  84,  53,  89,  128, 100, 113, 101, 45,  75,  79,  123, 47,  51,  128, 81,  171, 1,
    57,  17,  5,   71,  102, 57,  53,  41,  49,  38,  33,  13,  121, 57,  73,  26,  1,   85,  41,  10,  67,
    138, 77,  110, 90,  47,  114, 115, 21,  2,   10,  102, 255, 166, 23,  6,   101, 29,  16,  10,  85,  128,
    101, 196, 26,  57,  18,  10,  102, 102, 213, 34,  20,  43,  117, 20,  15,  36,  163, 128, 68,  1,   26,
    102, 61,  71,  37,  34,  53,  31,  243, 192, 69,  60,  71,  38,  73,  119, 28,  222, 37,  68,  45,  128,
    34,  1,   47,  11,  245, 171, 62,  17,  19,  70,  146, 85,  55,  62,  70,  37,  43,  37,  154, 100, 163,
    85,  160, 1,   63,  9,   92,  136, 28,  64,  32,  201, 85,  75,  15,  9,   9,   64,  255, 184, 119, 16,
    86,  6,   28,  5,   64,  255, 25,  248, 1,   56,  8,   17,  132, 137, 255, 55,  116, 128, 58,  15,  20,
    82,  135, 57,  26,  121, 40,  164, 50,  31,  137, 154, 133, 25,  35,  218, 51,  103, 44,  131, 131, 123,
    31,  6,   158, 86,  40,  64,  135, 148, 224, 45,  183, 128, 22,  26,  17,  131, 240, 154, 14,  1,   209,
    45,  16,  21,  91,  64,  222, 7,   1,   197, 56,  21,  39,  155, 60,  138, 23,  102, 213, 83,  12,  13,
    54,  192, 255, 68,  47,  28,  85,  26,  85,  85,  128, 128, 32,  146, 171, 18,  11,  7,   63,  144, 171,
    4,   4,   246, 35,  27,  10,  146, 174, 171, 12,  26,  128, 190, 80,  35,  99,  180, 80,  126, 54,  45,
    85,  126, 47,  87,  176, 51,  41,  20,  32,  101, 75,  128, 139, 118, 146, 116, 128, 85,  56,  41,  15,
    176, 236, 85,  37,  9,   62,  71,  30,  17,  119, 118, 255, 17,  18,  138, 101, 38,  60,  138, 55,  70,
    43,  26,  142, 146, 36,  19,  30,  171, 255, 97,  27,  20,  138, 45,  61,  62,  219, 1,   81,  188, 64,
    32,  41,  20,  117, 151, 142, 20,  21,  163, 112, 19,  12,  61,  195, 128, 48,  4,   24,
};

static_assert(sizeof(kSubblockModeProbability) == 10u * 10u * 9u);
static_assert(probability_sum(kSubblockModeProbability, sizeof(kSubblockModeProbability)) == 77'557u);
static_assert(probability_weighted_sum(kSubblockModeProbability, sizeof(kSubblockModeProbability)) ==
              34'036'952u);

// RFC 6386 section 14.1 dequantization lookup tables.
constexpr u8 kVp8DcQuantizer[128] = {
    4,   5,   6,   7,   8,   9,   10,  10,  11,  12,  13,  14,  15,  16,  17,  17,  18,  19,  20,
    20,  21,  21,  22,  22,  23,  23,  24,  25,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,
    35,  36,  37,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  46,  47,  48,  49,  50,  51,
    52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,
    71,  72,  73,  74,  75,  76,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,
    89,  91,  93,  95,  96,  98,  100, 101, 102, 104, 106, 108, 110, 112, 114, 116, 118, 122, 124,
    126, 128, 130, 132, 134, 136, 138, 140, 143, 145, 148, 151, 154, 157,
};

constexpr u16 kVp8AcQuantizer[128] = {
    4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,
    23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,
    42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  60,  62,
    64,  66,  68,  70,  72,  74,  76,  78,  80,  82,  84,  86,  88,  90,  92,  94,  96,  98,  100,
    102, 104, 106, 108, 110, 112, 114, 116, 119, 122, 125, 128, 131, 134, 137, 140, 143, 146, 149,
    152, 155, 158, 161, 164, 167, 170, 173, 177, 181, 185, 189, 193, 197, 201, 205, 209, 213, 217,
    221, 225, 229, 234, 239, 245, 249, 254, 259, 264, 269, 274, 279, 284,
};

constexpr u8 kCoefficientBand[17] = {
    0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0,
};
constexpr u8 kCoefficientZigzag[16] = {
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15,
};
constexpr u8 kCoefficientCategory3[] = {173, 148, 140};
constexpr u8 kCoefficientCategory4[] = {176, 155, 140, 135};
constexpr u8 kCoefficientCategory5[] = {180, 157, 141, 134, 130};
constexpr u8 kCoefficientCategory6[] = {
    254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129,
};

struct Vp8QuantMatrix {
    u16 y1_dc;
    u16 y1_ac;
    u16 y2_dc;
    u16 y2_ac;
    u16 uv_dc;
    u16 uv_ac;
};

struct Vp8DecodedCoefficientBlock {
    u32 end_position;
    bool has_nonzero;
    bool range_error;
};

static_assert(probability_sum(kVp8DcQuantizer, sizeof(kVp8DcQuantizer)) == 8'168u);

constexpr u64 quantizer_sum(const u16* values, u32 count) noexcept {
    u64 result = 0;
    for (u32 index = 0; index < count; ++index) {
        result += values[index];
    }
    return result;
}

static_assert(quantizer_sum(kVp8AcQuantizer, sizeof(kVp8AcQuantizer) / sizeof(kVp8AcQuantizer[0])) ==
              12'723u);

Vp8ParseResult failure(Vp8Error error) noexcept {
    return Vp8ParseResult{error};
}

// Frame-header fields are read once per frame; they share vp8_bool_read's
// out-of-line decoder instead of inlining bool_read at every field.
i32 read_signed_literal(Vp8BoolReader* reader, u32 magnitude_bits) noexcept {
    const i32 magnitude = static_cast<i32>(vp8_bool_read_literal(reader, magnitude_bits));
    return vp8_bool_read(reader, 128) != 0 ? -magnitude : magnitude;
}

i32 read_optional_signed_literal(Vp8BoolReader* reader, u32 magnitude_bits) noexcept {
    return vp8_bool_read(reader, 128) != 0 ? read_signed_literal(reader, magnitude_bits) : 0;
}

// RFC 6386 bmode_tree: a positive entry is the next node pair, otherwise the
// negated mode; node i reads with probability[i / 2].
inline constexpr signed char kSubblockModeTree[18] = {
    0, 2, -1, 4, -2, 6, 8, 12, -3, 10, -4, -5, -6, 14, -7, 16, -8, -9,
};

__forceinline Vp8SubblockMode read_subblock_mode(WindowReader* reader, Vp8SubblockMode above,
                                                 Vp8SubblockMode left) noexcept {
    const u32 probability_offset = (static_cast<u32>(above) * 10u + static_cast<u32>(left)) * 9u;
    const u8* probability = kSubblockModeProbability + probability_offset;
    u32 node = 0;
    i32 next = 0;
    while ((next = kSubblockModeTree[node + window_read(reader, probability[node >> 1])]) > 0) {
        node = static_cast<u32>(next);
    }
    return static_cast<Vp8SubblockMode>(-next);
}

// One macroblock header (RFC 6386 sections 19.3 and 11). above holds the
// bottom subblock modes of the macroblock above, left the left macroblock;
// null means the frame edge (B_DC_PRED context). One out-of-line copy serves
// the frame and row parsers; the reader lives in a local (registers) per call.
void parse_macroblock_mode(WindowReader* state, const Vp8ControlHeader& header, const Vp8SubblockMode* above_modes,
                           const Vp8MacroblockMode* left_mode, Vp8MacroblockMode* output) noexcept {
    WindowReader local = *state;
    WindowReader* const reader = &local;
    Vp8MacroblockMode& mode = *output;
    mode = Vp8MacroblockMode{};
    if (header.segmentation.update_map) {
        if (window_read(reader, header.segmentation.tree_probability[0]) == 0) {
            mode.segment_id = static_cast<u8>(window_read(reader, header.segmentation.tree_probability[1]));
        } else {
            mode.segment_id =
                static_cast<u8>(2u + window_read(reader, header.segmentation.tree_probability[2]));
        }
    }
    if (header.use_skip_probability) {
        mode.skip_coefficients = static_cast<u8>(window_read(reader, header.skip_probability));
    }
    if (window_read(reader, 145) == 0) {
        mode.luma = Vp8IntraMode::subblocks;
        for (u32 block_y = 0; block_y < 4; ++block_y) {
            for (u32 block_x = 0; block_x < 4; ++block_x) {
                Vp8SubblockMode above = Vp8SubblockMode::dc;
                if (block_y != 0) {
                    above = mode.luma_subblock[(block_y - 1u) * 4u + block_x];
                } else if (above_modes) {
                    above = above_modes[block_x];
                }
                Vp8SubblockMode left = Vp8SubblockMode::dc;
                if (block_x != 0) {
                    left = mode.luma_subblock[block_y * 4u + block_x - 1u];
                } else if (left_mode) {
                    left = left_mode->luma_subblock[block_y * 4u + 3u];
                }
                mode.luma_subblock[block_y * 4u + block_x] = read_subblock_mode(reader, above, left);
            }
        }
    } else {
        if (window_read(reader, 156) != 0) {
            mode.luma =
                window_read(reader, 128) != 0 ? Vp8IntraMode::true_motion : Vp8IntraMode::horizontal;
        } else {
            mode.luma = window_read(reader, 163) != 0 ? Vp8IntraMode::vertical : Vp8IntraMode::dc;
        }
        const Vp8SubblockMode context_mode = static_cast<Vp8SubblockMode>(mode.luma);
        for (u32 block = 0; block < 16; ++block) {
            mode.luma_subblock[block] = context_mode;
        }
    }
    if (window_read(reader, 142) == 0) {
        mode.chroma = Vp8IntraMode::dc;
    } else if (window_read(reader, 114) == 0) {
        mode.chroma = Vp8IntraMode::vertical;
    } else {
        mode.chroma = window_read(reader, 183) != 0 ? Vp8IntraMode::true_motion : Vp8IntraMode::horizontal;
    }
    *state = local;
}

i32 clamp_quantizer(i32 value, i32 upper) noexcept {
    return value < 0 ? 0 : value > upper ? upper : value;
}

Vp8QuantMatrix make_quant_matrix(const Vp8ControlHeader& header, u32 segment_id) noexcept {
    i32 quantizer = header.quantization.base_quantizer;
    if (header.segmentation.enabled) {
        quantizer = header.segmentation.quantizer[segment_id];
        if (!header.segmentation.absolute_delta) {
            quantizer += header.quantization.base_quantizer;
        }
    }
    quantizer = clamp_quantizer(quantizer, 127);
    Vp8QuantMatrix result{};
    result.y1_dc = kVp8DcQuantizer[clamp_quantizer(quantizer + header.quantization.y1_dc_delta, 127)];
    result.y1_ac = kVp8AcQuantizer[quantizer];
    result.y2_dc = static_cast<u16>(
        kVp8DcQuantizer[clamp_quantizer(quantizer + header.quantization.y2_dc_delta, 127)] * 2u);
    result.y2_ac = static_cast<u16>(
        kVp8AcQuantizer[clamp_quantizer(quantizer + header.quantization.y2_ac_delta, 127)] * 101'581u >>
        16);
    if (result.y2_ac < 8) {
        result.y2_ac = 8;
    }
    result.uv_dc = kVp8DcQuantizer[clamp_quantizer(quantizer + header.quantization.uv_dc_delta, 117)];
    result.uv_ac = kVp8AcQuantizer[clamp_quantizer(quantizer + header.quantization.uv_ac_delta, 127)];
    return result;
}

__forceinline const u8* coefficient_probability(const Vp8ProbabilityWorkspace& probabilities, u32 type, u32 coefficient,
                                  u32 context) noexcept {
    const u32 index =
        type * 8u * 3u * 11u + static_cast<u32>(kCoefficientBand[coefficient]) * 3u * 11u + context * 11u;
    return probabilities.coefficient + index;
}

__forceinline u32 read_large_coefficient(WindowReader* reader, const u8* probability) noexcept {
    if (window_read(reader, probability[3]) == 0) {
        if (window_read(reader, probability[4]) == 0) {
            return 2;
        }
        return 3u + window_read(reader, probability[5]);
    }
    if (window_read(reader, probability[6]) == 0) {
        if (window_read(reader, probability[7]) == 0) {
            return 5u + window_read(reader, 159);
        }
        return 7u + 2u * window_read(reader, 165) + window_read(reader, 145);
    }
    const u32 high_category = window_read(reader, probability[8]);
    const u32 low_category = window_read(reader, probability[9u + high_category]);
    const u32 category = high_category * 2u + low_category;
    const u8* category_probability = category == 0   ? kCoefficientCategory3
                                     : category == 1 ? kCoefficientCategory4
                                     : category == 2 ? kCoefficientCategory5
                                                     : kCoefficientCategory6;
    // Extra magnitude bits of categories 3 to 6 (RFC 6386 section 13.2);
    // category 6 takes 11, not a 6-bit continuation of the pattern.
    constexpr u8 kCategoryBitCount[] = {3, 4, 5, 11};
    const u32 category_bits = kCategoryBitCount[category];
    u32 value = 0;
    for (u32 bit = 0; bit < category_bits; ++bit) {
        value = value * 2u + window_read(reader, category_probability[bit]);
    }
    return value + 3u + (8u << category);
}

__forceinline Vp8DecodedCoefficientBlock finish(WindowReader* state, const WindowReader& local,
                                                Vp8DecodedCoefficientBlock result) noexcept {
    *state = local;
    return result;
}

Vp8DecodedCoefficientBlock decode_coefficient_block(WindowReader* state,
                                                    const Vp8ProbabilityWorkspace& probabilities, u32 type,
                                                    u32 context, u16 dc_quantizer, u16 ac_quantizer,
                                                    u32 start, i16* output) noexcept {
    // Local reader state stays in registers; written back once per block.
    WindowReader local = *state;
    WindowReader* const reader = &local;
    u32 coefficient = start;
    u32 coefficient_context = context;
    bool has_nonzero = false;
    while (coefficient < 16 && reader->ok()) {
        const u8* probability =
            coefficient_probability(probabilities, type, coefficient, coefficient_context);
        if (window_read(reader, probability[0]) == 0) {
            return finish(state, local, Vp8DecodedCoefficientBlock{coefficient, has_nonzero, false});
        }
        while (window_read(reader, probability[1]) == 0) {
            ++coefficient;
            if (coefficient == 16) {
                return finish(state, local, Vp8DecodedCoefficientBlock{16, has_nonzero, false});
            }
            probability = coefficient_probability(probabilities, type, coefficient, 0);
        }
        u32 magnitude = 1;
        if (window_read(reader, probability[2]) != 0) {
            magnitude = read_large_coefficient(reader, probability);
        }
        coefficient_context = magnitude == 1 ? 1u : 2u;
        const i32 signed_value =
            window_read(reader, 128) != 0 ? -static_cast<i32>(magnitude) : static_cast<i32>(magnitude);
        const i32 dequantized =
            signed_value * static_cast<i32>(coefficient == 0 ? dc_quantizer : ac_quantizer);
        if (dequantized < -32'768 || dequantized > 32'767) {
            return finish(state, local, Vp8DecodedCoefficientBlock{coefficient + 1u, has_nonzero, true});
        }
        output[kCoefficientZigzag[coefficient]] = static_cast<i16>(dequantized);
        has_nonzero = true;
        ++coefficient;
    }
    return finish(state, local, Vp8DecodedCoefficientBlock{coefficient, has_nonzero, false});
}

bool inverse_wht(const i16* input, Vp8MacroblockCoefficients* output) noexcept {
    i32 temporary[16]{};
    for (u32 column = 0; column < 4; ++column) {
        const i32 a0 = input[column] + input[12u + column];
        const i32 a1 = input[4u + column] + input[8u + column];
        const i32 a2 = input[4u + column] - input[8u + column];
        const i32 a3 = input[column] - input[12u + column];
        temporary[column] = a0 + a1;
        temporary[8u + column] = a0 - a1;
        temporary[4u + column] = a3 + a2;
        temporary[12u + column] = a3 - a2;
    }
    for (u32 row = 0; row < 4; ++row) {
        const u32 base = row * 4u;
        const i32 dc = temporary[base] + 3;
        const i32 a0 = dc + temporary[base + 3u];
        const i32 a1 = temporary[base + 1u] + temporary[base + 2u];
        const i32 a2 = temporary[base + 1u] - temporary[base + 2u];
        const i32 a3 = dc - temporary[base + 3u];
        const i32 transformed[4] = {
            (a0 + a1) >> 3,
            (a3 + a2) >> 3,
            (a0 - a1) >> 3,
            (a3 - a2) >> 3,
        };
        for (u32 column = 0; column < 4; ++column) {
            if (transformed[column] < -32'768 || transformed[column] > 32'767) {
                return false;
            }
            output->values[base + column][0] = static_cast<i16>(transformed[column]);
        }
    }
    return true;
}

void clear_coefficients(Vp8MacroblockCoefficients* output) noexcept {
    for (u32 block = 0; block < 24; ++block) {
        for (u32 coefficient = 0; coefficient < 16; ++coefficient) {
            output->values[block][coefficient] = 0;
        }
    }
    output->has_coefficients = 0;
    output->reserved = 0;
}

// Tokens of one macroblock (RFC 6386 section 13): dequantized residuals in
// raster order, the Y2 inverse WHT folded into the luma DC positions.
Vp8CoefficientParseResult decode_macroblock_tokens(WindowReader* token_reader, const Vp8ControlHeader& header,
                                                   const Vp8ProbabilityWorkspace& probabilities,
                                                   const Vp8MacroblockMode& mode, Vp8TokenContext* top_context,
                                                   Vp8TokenContext* left_context,
                                                   Vp8MacroblockCoefficients* coefficients) noexcept {
    if (mode.segment_id >= 4) {
        return {Vp8CoefficientError::invalid_input};
    }
    WindowReader& reader = *token_reader;
    Vp8MacroblockCoefficients& output = *coefficients;
    clear_coefficients(&output);
    Vp8TokenContext& top = *top_context;
    Vp8TokenContext& left = *left_context;
    if (mode.skip_coefficients) {
        for (u32 index = 0; index < 4; ++index) {
            top.y[index] = 0;
            left.y[index] = 0;
        }
        for (u32 index = 0; index < 2; ++index) {
            top.u[index] = 0;
            top.v[index] = 0;
            left.u[index] = 0;
            left.v[index] = 0;
        }
        // I4x4 macroblocks have no Y2 block. VP8 retains the Y2
        // neighbour contexts across them; only a skipped I16 block
        // contributes an explicit zero Y2 context.
        if (mode.luma != Vp8IntraMode::subblocks) {
            top.y2 = 0;
            left.y2 = 0;
        }
        return Vp8CoefficientParseResult{Vp8CoefficientError::none};
    }

    const Vp8QuantMatrix quantizer = make_quant_matrix(header, mode.segment_id);
    u8 has_coefficients = 0;
    u32 y_start = 0;
    u32 y_type = 3;
    if (mode.luma != Vp8IntraMode::subblocks) {
        i16 y2[16]{};
        const u32 y2_context = top.y2 + left.y2;
        const Vp8DecodedCoefficientBlock y2_result = decode_coefficient_block(
            &reader, probabilities, 1, y2_context, quantizer.y2_dc, quantizer.y2_ac, 0, y2);
        if (!reader.ok()) {
            return {Vp8CoefficientError::boolean_stream};
        }
        if (y2_result.range_error) {
            return {Vp8CoefficientError::coefficient_range};
        }
        const u8 y2_nonzero = static_cast<u8>(y2_result.has_nonzero);
        top.y2 = y2_nonzero;
        left.y2 = y2_nonzero;
        has_coefficients |= y2_nonzero;
        if (y2_result.end_position > 1) {
            if (!inverse_wht(y2, &output)) {
                return {Vp8CoefficientError::coefficient_range};
            }
        } else {
            const i32 dc = (static_cast<i32>(y2[0]) + 3) >> 3;
            if (dc < -32'768 || dc > 32'767) {
                return {Vp8CoefficientError::coefficient_range};
            }
            for (u32 block = 0; block < 16; ++block) {
                output.values[block][0] = static_cast<i16>(dc);
            }
        }
        y_start = 1;
        y_type = 0;
    }

    for (u32 block_y = 0; block_y < 4; ++block_y) {
        for (u32 block_x = 0; block_x < 4; ++block_x) {
            const u32 context = left.y[block_y] + top.y[block_x];
            const u32 block = block_y * 4u + block_x;
            const Vp8DecodedCoefficientBlock result =
                decode_coefficient_block(&reader, probabilities, y_type, context, quantizer.y1_dc,
                                         quantizer.y1_ac, y_start, output.values[block]);
            if (!reader.ok()) {
                return {Vp8CoefficientError::boolean_stream};
            }
            if (result.range_error) {
                return {Vp8CoefficientError::coefficient_range};
            }
            const u8 nonzero = static_cast<u8>(result.has_nonzero);
            left.y[block_y] = nonzero;
            top.y[block_x] = nonzero;
            has_coefficients |= nonzero;
        }
    }

    for (u32 channel = 0; channel < 2; ++channel) {
        u8* top_chroma = channel == 0 ? top.u : top.v;
        u8* left_chroma = channel == 0 ? left.u : left.v;
        const u32 first_block = 16u + channel * 4u;
        for (u32 block_y = 0; block_y < 2; ++block_y) {
            for (u32 block_x = 0; block_x < 2; ++block_x) {
                const u32 context = left_chroma[block_y] + top_chroma[block_x];
                const u32 block = first_block + block_y * 2u + block_x;
                const Vp8DecodedCoefficientBlock result =
                    decode_coefficient_block(&reader, probabilities, 2, context, quantizer.uv_dc,
                                             quantizer.uv_ac, 0, output.values[block]);
                if (!reader.ok()) {
                    return {Vp8CoefficientError::boolean_stream};
                }
                if (result.range_error) {
                    return {Vp8CoefficientError::coefficient_range};
                }
                const u8 nonzero = static_cast<u8>(result.has_nonzero);
                left_chroma[block_y] = nonzero;
                top_chroma[block_x] = nonzero;
                has_coefficients |= nonzero;
            }
        }
    }
    output.has_coefficients = has_coefficients;
    return Vp8CoefficientParseResult{Vp8CoefficientError::none};
}

} // namespace

bool vp8_bool_reader_initialize(Vp8BoolReader* reader, ConstSlice<u8> bytes) noexcept {
    if (!reader) {
        return false;
    }
    *reader = Vp8BoolReader{};
    reader->bytes = bytes;
    if (!bytes.data) {
        reader->error = Vp8BoolError::invalid_input;
        return false;
    }
    if (bytes.count < 2) {
        reader->error = Vp8BoolError::truncated;
        return false;
    }
    reader->byte_offset = 2;
    reader->value = static_cast<u32>(bytes[0]) << 8 | bytes[1];
    reader->range = 255;
    reader->error = Vp8BoolError::none;
    return true;
}

u32 vp8_bool_read(Vp8BoolReader* reader, u8 probability) noexcept {
    return bool_read(reader, probability);
}

Vp8ControlParseResult vp8_parse_keyframe_control(const Vp8Keyframe& frame, Vp8ControlHeader* header,
                                                 Vp8ProbabilityWorkspace* probabilities) noexcept {
    if (!header || !probabilities || !frame.first_partition.data || frame.first_partition.count < 2 ||
        !frame.remaining_data.data) {
        return {Vp8ControlError::invalid_input};
    }
    *header = Vp8ControlHeader{};
    for (u32 index = 0; index < kVp8CoefficientProbabilityCount; ++index) {
        probabilities->coefficient[index] = kDefaultCoefficientProbability[index];
    }
    header->segmentation.absolute_delta = 1;
    header->segmentation.tree_probability[0] = 255;
    header->segmentation.tree_probability[1] = 255;
    header->segmentation.tree_probability[2] = 255;

    Vp8BoolReader reader{};
    if (!vp8_bool_reader_initialize(&reader, frame.first_partition)) {
        return {Vp8ControlError::boolean_stream};
    }

    header->color_space = static_cast<u8>(vp8_bool_read(&reader, 128));
    header->clamping_type = static_cast<u8>(vp8_bool_read(&reader, 128));

    Vp8SegmentationHeader& segment = header->segmentation;
    segment.enabled = static_cast<u8>(vp8_bool_read(&reader, 128));
    if (segment.enabled) {
        segment.update_map = static_cast<u8>(vp8_bool_read(&reader, 128));
        segment.update_data = static_cast<u8>(vp8_bool_read(&reader, 128));
        if (segment.update_data) {
            segment.absolute_delta = static_cast<u8>(vp8_bool_read(&reader, 128));
            for (u32 index = 0; index < 4; ++index) {
                segment.quantizer[index] = read_optional_signed_literal(&reader, 7);
            }
            for (u32 index = 0; index < 4; ++index) {
                segment.filter_strength[index] = read_optional_signed_literal(&reader, 6);
            }
        }
        if (segment.update_map) {
            for (u32 index = 0; index < 3; ++index) {
                if (vp8_bool_read(&reader, 128) != 0) {
                    segment.tree_probability[index] = static_cast<u8>(vp8_bool_read_literal(&reader, 8));
                }
            }
        }
    }

    Vp8LoopFilterHeader& filter = header->loop_filter;
    filter.simple = static_cast<u8>(vp8_bool_read(&reader, 128));
    filter.level = static_cast<u8>(vp8_bool_read_literal(&reader, 6));
    filter.sharpness = static_cast<u8>(vp8_bool_read_literal(&reader, 3));
    filter.use_delta = static_cast<u8>(vp8_bool_read(&reader, 128));
    if (filter.use_delta) {
        filter.update_delta = static_cast<u8>(vp8_bool_read(&reader, 128));
        if (filter.update_delta) {
            for (u32 index = 0; index < 4; ++index) {
                if (vp8_bool_read(&reader, 128) != 0) {
                    filter.reference_delta[index] = read_signed_literal(&reader, 6);
                }
            }
            for (u32 index = 0; index < 4; ++index) {
                if (vp8_bool_read(&reader, 128) != 0) {
                    filter.mode_delta[index] = read_signed_literal(&reader, 6);
                }
            }
        }
    }

    const u32 token_partition_log2 = vp8_bool_read_literal(&reader, 2);
    if (!reader.ok()) {
        header->macroblock_reader = reader;
        return {Vp8ControlError::boolean_stream};
    }
    header->token_partition_count = static_cast<u8>(1u << token_partition_log2);

    const u32 explicit_partition_count = header->token_partition_count - 1u;
    const u32 partition_table_bytes = explicit_partition_count * 3u;
    if (frame.remaining_data.count < partition_table_bytes) {
        header->macroblock_reader = reader;
        return {Vp8ControlError::token_partition_table};
    }
    u32 token_offset = partition_table_bytes;
    for (u32 index = 0; index < explicit_partition_count; ++index) {
        const u32 partition_bytes = byte_load_le24(frame.remaining_data.data + index * 3u);
        if (partition_bytes < 2 || partition_bytes > frame.remaining_data.count - token_offset) {
            header->macroblock_reader = reader;
            return {Vp8ControlError::token_partition_bounds};
        }
        header->token_partitions[index].bytes =
            ConstSlice<u8>{frame.remaining_data.data + token_offset, partition_bytes};
        token_offset += partition_bytes;
    }
    header->token_partitions[explicit_partition_count].bytes =
        ConstSlice<u8>{frame.remaining_data.data + token_offset, frame.remaining_data.count - token_offset};
    if (header->token_partitions[explicit_partition_count].bytes.count < 2) {
        header->macroblock_reader = reader;
        return {Vp8ControlError::token_partition_bounds};
    }

    Vp8QuantizationHeader& quantization = header->quantization;
    quantization.base_quantizer = static_cast<u8>(vp8_bool_read_literal(&reader, 7));
    quantization.y1_dc_delta = read_optional_signed_literal(&reader, 4);
    quantization.y2_dc_delta = read_optional_signed_literal(&reader, 4);
    quantization.y2_ac_delta = read_optional_signed_literal(&reader, 4);
    quantization.uv_dc_delta = read_optional_signed_literal(&reader, 4);
    quantization.uv_ac_delta = read_optional_signed_literal(&reader, 4);

    header->refresh_entropy_probabilities = static_cast<u8>(vp8_bool_read(&reader, 128));

    u32 exception_cursor = 0;
    for (u32 index = 0; index < kVp8CoefficientProbabilityCount; ++index) {
        u8 update_probability = 255;
        if (exception_cursor < sizeof(kCoefficientUpdateProbabilityExceptions) /
                                   sizeof(kCoefficientUpdateProbabilityExceptions[0]) &&
            kCoefficientUpdateProbabilityExceptions[exception_cursor].index == index) {
            update_probability = kCoefficientUpdateProbabilityExceptions[exception_cursor].probability;
            ++exception_cursor;
        }
        if (bool_read(&reader, update_probability) != 0) {
            probabilities->coefficient[index] = static_cast<u8>(vp8_bool_read_literal(&reader, 8));
            ++header->coefficient_probability_updates;
        }
    }

    header->use_skip_probability = static_cast<u8>(vp8_bool_read(&reader, 128));
    if (header->use_skip_probability) {
        header->skip_probability = static_cast<u8>(vp8_bool_read_literal(&reader, 8));
    }
    header->macroblock_reader = reader;
    if (!reader.ok()) {
        return {Vp8ControlError::boolean_stream};
    }
    return Vp8ControlParseResult{Vp8ControlError::none};
}

u32 vp8_bool_read_literal(Vp8BoolReader* reader, u32 bit_count) noexcept {
    if (!reader->ok()) {
        return 0;
    }
    if (bit_count > 32) {
        reader->error = Vp8BoolError::literal_width;
        return 0;
    }
    u32 value = 0;
    while (bit_count != 0 && reader->ok()) {
        value = value << 1 | vp8_bool_read(reader, 128);
        --bit_count;
    }
    return value;
}

Vp8ModeParseResult vp8_parse_keyframe_modes(const Vp8Keyframe& frame, const Vp8ControlHeader& header,
                                            Vp8ModeWorkspace* workspace) noexcept {
    if (!workspace || !header.macroblock_reader.ok() ||
        header.macroblock_reader.bytes.data != frame.first_partition.data ||
        header.macroblock_reader.bytes.count != frame.first_partition.count ||
        header.macroblock_reader.byte_offset > frame.first_partition.count ||
        header.macroblock_reader.bit_count >= 8 || header.macroblock_reader.range < 128 ||
        header.macroblock_reader.range > 255) {
        return {Vp8ModeError::invalid_input};
    }
    const u32 columns = (static_cast<u32>(frame.width) + 15u) / 16u;
    const u32 rows = (static_cast<u32>(frame.height) + 15u) / 16u;
    const u32 macroblock_count = columns * rows;
    if (frame.width == 0 || frame.height == 0 || columns > kVp8MacroblockDimensionMaximum ||
        rows > kVp8MacroblockDimensionMaximum || macroblock_count > kVp8MacroblockMaximum) {
        return {Vp8ModeError::dimensions};
    }

    workspace->macroblock_count = 0;
    workspace->columns = static_cast<u16>(columns);
    workspace->rows = static_cast<u16>(rows);
    // Decoded in a local window (registers), published to the workspace at exit.
    WindowReader reader = to_window(header.macroblock_reader);

    for (u32 macroblock_y = 0; macroblock_y < rows; ++macroblock_y) {
        for (u32 macroblock_x = 0; macroblock_x < columns; ++macroblock_x) {
            const u32 macroblock_index = macroblock_y * columns + macroblock_x;
            Vp8MacroblockMode& mode = workspace->macroblocks[macroblock_index];
            parse_macroblock_mode(&reader, header,
                                  macroblock_y != 0
                                      ? workspace->macroblocks[macroblock_index - columns].luma_subblock + 12
                                      : nullptr,
                                  macroblock_x != 0 ? &workspace->macroblocks[macroblock_index - 1u] : nullptr,
                                  &mode);
            if (!reader.ok()) {
                workspace->reader = header.macroblock_reader;
                from_window(reader, &workspace->reader);
                return {Vp8ModeError::boolean_stream};
            }
            ++workspace->macroblock_count;
        }
    }
    workspace->reader = header.macroblock_reader;
    from_window(reader, &workspace->reader);
    return Vp8ModeParseResult{Vp8ModeError::none};
}

Vp8CoefficientParseResult vp8_decode_keyframe_coefficients(const Vp8Keyframe& frame,
                                                           const Vp8ControlHeader& header,
                                                           const Vp8ProbabilityWorkspace& probabilities,
                                                           const Vp8ModeWorkspace& modes,
                                                           Vp8CoefficientWorkspace* workspace) noexcept {
    const u32 partition_count = header.token_partition_count;
    if (!workspace || partition_count == 0 || partition_count > 8 ||
        (partition_count & (partition_count - 1u)) != 0) {
        return {Vp8CoefficientError::invalid_input};
    }
    const u32 columns = (static_cast<u32>(frame.width) + 15u) / 16u;
    const u32 rows = (static_cast<u32>(frame.height) + 15u) / 16u;
    const u32 macroblock_count = columns * rows;
    if (frame.width == 0 || frame.height == 0 || columns > kVp8MacroblockDimensionMaximum ||
        rows > kVp8MacroblockDimensionMaximum || macroblock_count > kVp8MacroblockMaximum ||
        modes.columns != columns || modes.rows != rows || modes.macroblock_count != macroblock_count) {
        return {Vp8CoefficientError::dimensions};
    }

    for (u32 index = 0; index < 8; ++index) {
        workspace->token_reader[index] = Vp8BoolReader{};
    }
    for (u32 index = 0; index < partition_count; ++index) {
        if (!vp8_bool_reader_initialize(&workspace->token_reader[index],
                                        header.token_partitions[index].bytes)) {
            return {Vp8CoefficientError::token_initialize};
        }
    }
    for (u32 column = 0; column < columns; ++column) {
        workspace->top_context[column] = Vp8TokenContext{};
    }
    workspace->left_context = Vp8TokenContext{};
    workspace->macroblock_count = 0;
    workspace->columns = static_cast<u16>(columns);
    workspace->rows = static_cast<u16>(rows);

    for (u32 macroblock_y = 0; macroblock_y < rows; ++macroblock_y) {
        workspace->left_context = Vp8TokenContext{};
        Vp8BoolReader& partition = workspace->token_reader[macroblock_y & (partition_count - 1u)];
        WindowReader reader = to_window(partition);
        for (u32 macroblock_x = 0; macroblock_x < columns; ++macroblock_x) {
            const u32 macroblock_index = macroblock_y * columns + macroblock_x;
            const Vp8MacroblockMode& mode = modes.macroblocks[macroblock_index];
            const auto decoded = decode_macroblock_tokens(&reader, header, probabilities, mode,
                                                          &workspace->top_context[macroblock_x],
                                                          &workspace->left_context,
                                                          &workspace->macroblocks[macroblock_index]);
            if (decoded.error != Vp8CoefficientError::none) {
                from_window(reader, &partition);
                return decoded;
            }
            ++workspace->macroblock_count;
        }
        from_window(reader, &partition);
    }
    return Vp8CoefficientParseResult{Vp8CoefficientError::none};
}

namespace {
Vp8ParseResult parse_keyframe(ConstSlice<u8> payload, Vp8Keyframe* frame, u32 maximum_bytes,
                              u32 maximum_dimension, u64 maximum_pixels) noexcept {
    if (!frame || (!payload.data && payload.count != 0)) {
        return failure(Vp8Error::invalid_input);
    }
    *frame = Vp8Keyframe{};
    if (payload.count > maximum_bytes) {
        return failure(Vp8Error::encoded_limit);
    }
    if (payload.count < 10) {
        return failure(Vp8Error::truncated);
    }
    const u32 tag = byte_load_le24(payload.data);
    if ((tag & 1u) != 0) {
        return failure(Vp8Error::interframe);
    }
    const u8 version = static_cast<u8>((tag >> 1) & 7u);
    if (version > 3) {
        return failure(Vp8Error::unsupported_version);
    }
    if ((tag & (1u << 4)) == 0) {
        return failure(Vp8Error::invisible_frame);
    }
    if (payload[3] != 0x9Du || payload[4] != 0x01u || payload[5] != 0x2Au) {
        return failure(Vp8Error::signature);
    }
    const u16 width_and_scale = byte_load_le16(payload.data + 6);
    const u16 height_and_scale = byte_load_le16(payload.data + 8);
    const u32 width = width_and_scale & 0x3FFFu;
    const u32 height = height_and_scale & 0x3FFFu;
    if (width == 0 || height == 0 || width > maximum_dimension || height > maximum_dimension ||
        static_cast<u64>(width) * height > maximum_pixels) {
        return failure(Vp8Error::dimensions);
    }
    const u32 first_partition_bytes = tag >> 5;
    const u64 first_partition_end = 10u + static_cast<u64>(first_partition_bytes);
    if (first_partition_bytes == 0 || first_partition_end > payload.count) {
        return failure(Vp8Error::first_partition);
    }

    frame->first_partition = ConstSlice<u8>{payload.data + 10, first_partition_bytes};
    frame->remaining_data = ConstSlice<u8>{payload.data + static_cast<u32>(first_partition_end),
                                           payload.count - static_cast<u32>(first_partition_end)};
    frame->payload_bytes = payload.count;
    frame->first_partition_bytes = first_partition_bytes;
    frame->width = static_cast<u16>(width);
    frame->height = static_cast<u16>(height);
    frame->version = version;
    frame->horizontal_scale = static_cast<u8>(width_and_scale >> 14);
    frame->vertical_scale = static_cast<u8>(height_and_scale >> 14);
    return Vp8ParseResult{Vp8Error::none};
}
} // namespace

Vp8ParseResult vp8_parse_keyframe(ConstSlice<u8> payload, Vp8Keyframe* frame) noexcept {
    return parse_keyframe(payload, frame, kWebpEncodedMaximumBytes, kWebpDimensionMaximum, kWebpPixelMaximum);
}

Vp8ParseResult vp8_parse_keyframe_bounded(ConstSlice<u8> payload, u32 maximum_bytes,
                                          Vp8Keyframe* frame) noexcept {
    return parse_keyframe(payload, frame, maximum_bytes, kVp8DimensionMaximum,
                          u64{kVp8DimensionMaximum} * kVp8DimensionMaximum);
}

Vp8CoefficientParseResult vp8_row_parser_initialize(Vp8RowParser* parser, const Vp8Keyframe& frame,
                                                    const Vp8ControlHeader& header,
                                                    Slice<Vp8TokenContext> top_context,
                                                    Slice<Vp8SubblockMode> above_modes) noexcept {
    const u32 partitions = header.token_partition_count;
    const u32 columns = (static_cast<u32>(frame.width) + 15u) / 16u;
    const u32 rows = (static_cast<u32>(frame.height) + 15u) / 16u;
    if (!parser || !frame.width || !frame.height || partitions == 0 || partitions > 8 ||
        (partitions & (partitions - 1u)) != 0 || !top_context.data || top_context.count < columns ||
        !above_modes.data || above_modes.count < columns * 4u || !header.macroblock_reader.ok() ||
        header.macroblock_reader.bytes.data != frame.first_partition.data ||
        header.macroblock_reader.bytes.count != frame.first_partition.count)
        return {Vp8CoefficientError::invalid_input};
    *parser = Vp8RowParser{};
    parser->modes = header.macroblock_reader;
    for (u32 index = 0; index < partitions; ++index)
        if (!vp8_bool_reader_initialize(&parser->tokens[index], header.token_partitions[index].bytes))
            return {Vp8CoefficientError::token_initialize};
    parser->top_context = top_context.data;
    parser->above_modes = above_modes.data;
    for (u32 column = 0; column < columns; ++column)
        parser->top_context[column] = Vp8TokenContext{};
    for (u32 index = 0; index < columns * 4u; ++index)
        parser->above_modes[index] = Vp8SubblockMode::dc;
    parser->columns = columns;
    parser->rows = rows;
    parser->partitions = partitions;
    return Vp8CoefficientParseResult{Vp8CoefficientError::none};
}

Vp8CoefficientParseResult vp8_row_parser_next(Vp8RowParser* parser, const Vp8ControlHeader& header,
                                              const Vp8ProbabilityWorkspace& probabilities,
                                              Slice<Vp8MacroblockMode> modes,
                                              Slice<Vp8MacroblockCoefficients> coefficients) noexcept {
    if (!parser || !parser->columns || parser->row >= parser->rows || !modes.data ||
        modes.count < parser->columns || !coefficients.data || coefficients.count < parser->columns)
        return {Vp8CoefficientError::invalid_input};
    // All macroblock headers live in the first partition, in raster order;
    // this row's tokens come from partition row % count.
    WindowReader mode_reader = to_window(parser->modes);
    for (u32 x = 0; x < parser->columns; ++x) {
        parse_macroblock_mode(&mode_reader, header, parser->row ? parser->above_modes + x * 4u : nullptr,
                              x ? &modes[x - 1] : nullptr, &modes[x]);
        if (!mode_reader.ok()) {
            from_window(mode_reader, &parser->modes);
            return {Vp8CoefficientError::boolean_stream};
        }
    }
    from_window(mode_reader, &parser->modes);
    Vp8BoolReader& partition = parser->tokens[parser->row & (parser->partitions - 1u)];
    WindowReader reader = to_window(partition);
    parser->left_context = Vp8TokenContext{};
    for (u32 x = 0; x < parser->columns; ++x) {
        const auto decoded = decode_macroblock_tokens(&reader, header, probabilities, modes[x],
                                                      &parser->top_context[x], &parser->left_context,
                                                      &coefficients[x]);
        if (decoded.error != Vp8CoefficientError::none) {
            from_window(reader, &partition);
            return decoded;
        }
        for (u32 block = 0; block < 4; ++block)
            parser->above_modes[x * 4u + block] = modes[x].luma_subblock[12u + block];
    }
    from_window(reader, &partition);
    ++parser->row;
    return Vp8CoefficientParseResult{Vp8CoefficientError::none};
}

} // namespace maicord
