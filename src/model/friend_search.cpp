#include "maicord/model/friend_search.hpp"
#include "maicord/core/utf.hpp"

namespace maicord { namespace {
// Compact numeric tables generated from Unicode 17.0.0 UnicodeData,
// SpecialCasing and DerivedCoreProperties. Unicode License v3:
// Copyright (c) 1991-2025 Unicode, Inc. See assets/licenses/Unicode-3.0.txt.
// UnicodeProperties lacks Cased/Case_Ignorable; these exact derived properties
// avoid approximating Final_Sigma with general letter categories.
struct LowerRange {u32 first;i32 delta;u16 span;u8 stride;};
static_assert(sizeof(LowerRange) == 12);
constexpr LowerRange lowercase_ranges[]{
    {0x41,32,25,1},
    {0xC0,32,22,1},
    {0xD8,32,6,1},
    {0x100,1,46,2},
    {0x130,-199,0,1},
    {0x132,1,4,2},
    {0x139,1,14,2},
    {0x14A,1,44,2},
    {0x178,-121,0,1},
    {0x179,1,4,2},
    {0x181,210,0,1},
    {0x182,1,2,2},
    {0x186,206,0,1},
    {0x187,1,0,1},
    {0x189,205,1,1},
    {0x18B,1,0,1},
    {0x18E,79,0,1},
    {0x18F,202,0,1},
    {0x190,203,0,1},
    {0x191,1,0,1},
    {0x193,205,0,1},
    {0x194,207,0,1},
    {0x196,211,0,1},
    {0x197,209,0,1},
    {0x198,1,0,1},
    {0x19C,211,0,1},
    {0x19D,213,0,1},
    {0x19F,214,0,1},
    {0x1A0,1,4,2},
    {0x1A6,218,0,1},
    {0x1A7,1,0,1},
    {0x1A9,218,0,1},
    {0x1AC,1,0,1},
    {0x1AE,218,0,1},
    {0x1AF,1,0,1},
    {0x1B1,217,1,1},
    {0x1B3,1,2,2},
    {0x1B7,219,0,1},
    {0x1B8,1,0,1},
    {0x1BC,1,0,1},
    {0x1C4,2,0,1},
    {0x1C5,1,0,1},
    {0x1C7,2,0,1},
    {0x1C8,1,0,1},
    {0x1CA,2,0,1},
    {0x1CB,1,16,2},
    {0x1DE,1,16,2},
    {0x1F1,2,0,1},
    {0x1F2,1,2,2},
    {0x1F6,-97,0,1},
    {0x1F7,-56,0,1},
    {0x1F8,1,38,2},
    {0x220,-130,0,1},
    {0x222,1,16,2},
    {0x23A,10795,0,1},
    {0x23B,1,0,1},
    {0x23D,-163,0,1},
    {0x23E,10792,0,1},
    {0x241,1,0,1},
    {0x243,-195,0,1},
    {0x244,69,0,1},
    {0x245,71,0,1},
    {0x246,1,8,2},
    {0x370,1,2,2},
    {0x376,1,0,1},
    {0x37F,116,0,1},
    {0x386,38,0,1},
    {0x388,37,2,1},
    {0x38C,64,0,1},
    {0x38E,63,1,1},
    {0x391,32,16,1},
    {0x3A3,32,8,1},
    {0x3CF,8,0,1},
    {0x3D8,1,22,2},
    {0x3F4,-60,0,1},
    {0x3F7,1,0,1},
    {0x3F9,-7,0,1},
    {0x3FA,1,0,1},
    {0x3FD,-130,2,1},
    {0x400,80,15,1},
    {0x410,32,31,1},
    {0x460,1,32,2},
    {0x48A,1,52,2},
    {0x4C0,15,0,1},
    {0x4C1,1,12,2},
    {0x4D0,1,94,2},
    {0x531,48,37,1},
    {0x10A0,7264,37,1},
    {0x10C7,7264,0,1},
    {0x10CD,7264,0,1},
    {0x13A0,38864,79,1},
    {0x13F0,8,5,1},
    {0x1C89,1,0,1},
    {0x1C90,-3008,42,1},
    {0x1CBD,-3008,2,1},
    {0x1E00,1,148,2},
    {0x1E9E,-7615,0,1},
    {0x1EA0,1,94,2},
    {0x1F08,-8,7,1},
    {0x1F18,-8,5,1},
    {0x1F28,-8,7,1},
    {0x1F38,-8,7,1},
    {0x1F48,-8,5,1},
    {0x1F59,-8,6,2},
    {0x1F68,-8,7,1},
    {0x1F88,-8,7,1},
    {0x1F98,-8,7,1},
    {0x1FA8,-8,7,1},
    {0x1FB8,-8,1,1},
    {0x1FBA,-74,1,1},
    {0x1FBC,-9,0,1},
    {0x1FC8,-86,3,1},
    {0x1FCC,-9,0,1},
    {0x1FD8,-8,1,1},
    {0x1FDA,-100,1,1},
    {0x1FE8,-8,1,1},
    {0x1FEA,-112,1,1},
    {0x1FEC,-7,0,1},
    {0x1FF8,-128,1,1},
    {0x1FFA,-126,1,1},
    {0x1FFC,-9,0,1},
    {0x2126,-7517,0,1},
    {0x212A,-8383,0,1},
    {0x212B,-8262,0,1},
    {0x2132,28,0,1},
    {0x2160,16,15,1},
    {0x2183,1,0,1},
    {0x24B6,26,25,1},
    {0x2C00,48,47,1},
    {0x2C60,1,0,1},
    {0x2C62,-10743,0,1},
    {0x2C63,-3814,0,1},
    {0x2C64,-10727,0,1},
    {0x2C67,1,4,2},
    {0x2C6D,-10780,0,1},
    {0x2C6E,-10749,0,1},
    {0x2C6F,-10783,0,1},
    {0x2C70,-10782,0,1},
    {0x2C72,1,0,1},
    {0x2C75,1,0,1},
    {0x2C7E,-10815,1,1},
    {0x2C80,1,98,2},
    {0x2CEB,1,2,2},
    {0x2CF2,1,0,1},
    {0xA640,1,44,2},
    {0xA680,1,26,2},
    {0xA722,1,12,2},
    {0xA732,1,60,2},
    {0xA779,1,2,2},
    {0xA77D,-35332,0,1},
    {0xA77E,1,8,2},
    {0xA78B,1,0,1},
    {0xA78D,-42280,0,1},
    {0xA790,1,2,2},
    {0xA796,1,18,2},
    {0xA7AA,-42308,0,1},
    {0xA7AB,-42319,0,1},
    {0xA7AC,-42315,0,1},
    {0xA7AD,-42305,0,1},
    {0xA7AE,-42308,0,1},
    {0xA7B0,-42258,0,1},
    {0xA7B1,-42282,0,1},
    {0xA7B2,-42261,0,1},
    {0xA7B3,928,0,1},
    {0xA7B4,1,14,2},
    {0xA7C4,-48,0,1},
    {0xA7C5,-42307,0,1},
    {0xA7C6,-35384,0,1},
    {0xA7C7,1,2,2},
    {0xA7CB,-42343,0,1},
    {0xA7CC,1,14,2},
    {0xA7DC,-42561,0,1},
    {0xA7F5,1,0,1},
    {0xFF21,32,25,1},
    {0x10400,40,39,1},
    {0x104B0,40,35,1},
    {0x10570,39,10,1},
    {0x1057C,39,14,1},
    {0x1058C,39,6,1},
    {0x10594,39,1,1},
    {0x10C80,64,50,1},
    {0x10D50,32,21,1},
    {0x118A0,32,31,1},
    {0x16E40,32,31,1},
    {0x16EA0,27,24,1},
    {0x1E900,34,33,1},
};
constexpr u32 cased_first[]{
    0x41,0x61,0xAA,0xB5,0xBA,0xC0,0xD8,0xF8,
    0x1BC,0x1C4,0x296,0x2C0,0x2E0,0x345,0x370,0x376,
    0x37A,0x37F,0x386,0x388,0x38C,0x38E,0x3A3,0x3F7,
    0x48A,0x531,0x560,0x10A0,0x10C7,0x10CD,0x10D0,0x10FC,
    0x13A0,0x13F8,0x1C80,0x1C90,0x1CBD,0x1D00,0x1E00,0x1F18,
    0x1F20,0x1F48,0x1F50,0x1F59,0x1F5B,0x1F5D,0x1F5F,0x1F80,
    0x1FB6,0x1FBE,0x1FC2,0x1FC6,0x1FD0,0x1FD6,0x1FE0,0x1FF2,
    0x1FF6,0x2071,0x207F,0x2090,0x2102,0x2107,0x210A,0x2115,
    0x2119,0x2124,0x2126,0x2128,0x212A,0x212F,0x2139,0x213C,
    0x2145,0x214E,0x2160,0x2183,0x24B6,0x2C00,0x2CEB,0x2CF2,
    0x2D00,0x2D27,0x2D2D,0xA640,0xA680,0xA722,0xA78B,0xA790,
    0xA7F1,0xA7F8,0xAB30,0xAB5C,0xAB70,0xFB00,0xFB13,0xFF21,
    0xFF41,0x10400,0x104B0,0x104D8,0x10570,0x1057C,0x1058C,0x10594,
    0x10597,0x105A3,0x105B3,0x105BB,0x10780,0x10783,0x10787,0x107B2,
    0x10C80,0x10CC0,0x10D50,0x10D70,0x118A0,0x16E40,0x16EA0,0x16EBB,
    0x1D400,0x1D456,0x1D49E,0x1D4A2,0x1D4A5,0x1D4A9,0x1D4AE,0x1D4BB,
    0x1D4BD,0x1D4C5,0x1D507,0x1D50D,0x1D516,0x1D51E,0x1D53B,0x1D540,
    0x1D546,0x1D54A,0x1D552,0x1D6A8,0x1D6C2,0x1D6DC,0x1D6FC,0x1D716,
    0x1D736,0x1D750,0x1D770,0x1D78A,0x1D7AA,0x1D7C4,0x1DF00,0x1DF0B,
    0x1DF25,0x1E030,0x1E900,0x1F130,0x1F150,0x1F170,
};
constexpr u16 cased_span[]{
    25,25,0,0,0,22,30,194,
    3,207,34,1,4,0,3,1,
    3,0,0,2,0,19,82,138,
    165,37,40,37,0,0,42,3,
    85,5,10,42,2,191,277,5,
    37,5,7,0,0,0,30,52,
    6,0,2,6,3,5,12,2,
    6,0,0,12,0,0,9,0,
    4,0,0,0,3,5,0,3,
    4,0,31,1,51,228,3,1,
    37,0,0,45,29,101,3,76,
    5,2,42,13,79,6,4,25,
    25,79,35,35,10,14,6,1,
    10,14,6,1,0,2,41,8,
    50,50,21,21,63,63,24,24,
    84,70,1,0,1,3,11,0,
    6,64,3,7,6,27,3,4,
    0,6,339,24,24,30,24,30,
    24,30,24,30,24,7,9,19,
    5,61,67,25,25,25,
};
constexpr u32 case_ignorable_first[]{
    0x27,0x2E,0x3A,0x5E,0x60,0xA8,0xAD,0xAF,
    0xB4,0xB7,0x2B0,0x374,0x37A,0x384,0x387,0x483,
    0x559,0x55F,0x591,0x5BF,0x5C1,0x5C4,0x5C7,0x5F4,
    0x600,0x610,0x61C,0x640,0x64B,0x670,0x6D6,0x6DF,
    0x6EA,0x70F,0x711,0x730,0x7A6,0x7EB,0x7FA,0x7FD,
    0x816,0x859,0x888,0x890,0x897,0x8C9,0x93A,0x93C,
    0x941,0x94D,0x951,0x962,0x971,0x981,0x9BC,0x9C1,
    0x9CD,0x9E2,0x9FE,0xA01,0xA3C,0xA41,0xA47,0xA4B,
    0xA51,0xA70,0xA75,0xA81,0xABC,0xAC1,0xAC7,0xACD,
    0xAE2,0xAFA,0xB01,0xB3C,0xB3F,0xB41,0xB4D,0xB55,
    0xB62,0xB82,0xBC0,0xBCD,0xC00,0xC04,0xC3C,0xC3E,
    0xC46,0xC4A,0xC55,0xC62,0xC81,0xCBC,0xCBF,0xCC6,
    0xCCC,0xCE2,0xD00,0xD3B,0xD41,0xD4D,0xD62,0xD81,
    0xDCA,0xDD2,0xDD6,0xE31,0xE34,0xE46,0xEB1,0xEB4,
    0xEC6,0xEC8,0xF18,0xF35,0xF37,0xF39,0xF71,0xF80,
    0xF86,0xF8D,0xF99,0xFC6,0x102D,0x1032,0x1039,0x103D,
    0x1058,0x105E,0x1071,0x1082,0x1085,0x108D,0x109D,0x10FC,
    0x135D,0x1712,0x1732,0x1752,0x1772,0x17B4,0x17B7,0x17C6,
    0x17C9,0x17D7,0x17DD,0x180B,0x1843,0x1885,0x18A9,0x1920,
    0x1927,0x1932,0x1939,0x1A17,0x1A1B,0x1A56,0x1A58,0x1A60,
    0x1A62,0x1A65,0x1A73,0x1A7F,0x1AA7,0x1AB0,0x1AE0,0x1B00,
    0x1B34,0x1B36,0x1B3C,0x1B42,0x1B6B,0x1B80,0x1BA2,0x1BA8,
    0x1BAB,0x1BE6,0x1BE8,0x1BED,0x1BEF,0x1C2C,0x1C36,0x1C78,
    0x1CD0,0x1CD4,0x1CE2,0x1CED,0x1CF4,0x1CF8,0x1D2C,0x1D78,
    0x1D9B,0x1FBD,0x1FBF,0x1FCD,0x1FDD,0x1FED,0x1FFD,0x200B,
    0x2018,0x2024,0x2027,0x202A,0x2060,0x2066,0x2071,0x207F,
    0x2090,0x20D0,0x2C7C,0x2CEF,0x2D6F,0x2D7F,0x2DE0,0x2E2F,
    0x3005,0x302A,0x3031,0x303B,0x3099,0x30FC,0xA015,0xA4F8,
    0xA60C,0xA66F,0xA674,0xA67F,0xA69C,0xA6F0,0xA700,0xA770,
    0xA788,0xA7F1,0xA7F8,0xA802,0xA806,0xA80B,0xA825,0xA82C,
    0xA8C4,0xA8E0,0xA8FF,0xA926,0xA947,0xA980,0xA9B3,0xA9B6,
    0xA9BC,0xA9CF,0xA9E5,0xAA29,0xAA31,0xAA35,0xAA43,0xAA4C,
    0xAA70,0xAA7C,0xAAB0,0xAAB2,0xAAB7,0xAABE,0xAAC1,0xAADD,
    0xAAEC,0xAAF3,0xAAF6,0xAB5B,0xAB69,0xABE5,0xABE8,0xABED,
    0xFB1E,0xFBB2,0xFE00,0xFE13,0xFE20,0xFE52,0xFE55,0xFEFF,
    0xFF07,0xFF0E,0xFF1A,0xFF3E,0xFF40,0xFF70,0xFF9E,0xFFE3,
    0xFFF9,0x101FD,0x102E0,0x10376,0x10780,0x10787,0x107B2,0x10A01,
    0x10A05,0x10A0C,0x10A38,0x10A3F,0x10AE5,0x10D24,0x10D4E,0x10D69,
    0x10D6F,0x10EAB,0x10EC5,0x10EFA,0x10F46,0x10F82,0x11001,0x11038,
    0x11070,0x11073,0x1107F,0x110B3,0x110B9,0x110BD,0x110C2,0x110CD,
    0x11100,0x11127,0x1112D,0x11173,0x11180,0x111B6,0x111C9,0x111CF,
    0x1122F,0x11234,0x11236,0x1123E,0x11241,0x112DF,0x112E3,0x11300,
    0x1133B,0x11340,0x11366,0x11370,0x113BB,0x113CE,0x113D0,0x113D2,
    0x113E1,0x11438,0x11442,0x11446,0x1145E,0x114B3,0x114BA,0x114BF,
    0x114C2,0x115B2,0x115BC,0x115BF,0x115DC,0x11633,0x1163D,0x1163F,
    0x116AB,0x116AD,0x116B0,0x116B7,0x1171D,0x1171F,0x11722,0x11727,
    0x1182F,0x11839,0x1193B,0x1193E,0x11943,0x119D4,0x119DA,0x119E0,
    0x11A01,0x11A33,0x11A3B,0x11A47,0x11A51,0x11A59,0x11A8A,0x11A98,
    0x11B60,0x11B62,0x11B66,0x11C30,0x11C38,0x11C3F,0x11C92,0x11CAA,
    0x11CB2,0x11CB5,0x11D31,0x11D3A,0x11D3C,0x11D3F,0x11D47,0x11D90,
    0x11D95,0x11D97,0x11DD9,0x11EF3,0x11F00,0x11F36,0x11F40,0x11F42,
    0x11F5A,0x13430,0x13447,0x1611E,0x1612D,0x16AF0,0x16B30,0x16B40,
    0x16D40,0x16D6B,0x16F4F,0x16F8F,0x16FE0,0x16FE3,0x16FF2,0x1AFF0,
    0x1AFF5,0x1AFFD,0x1BC9D,0x1BCA0,0x1CF00,0x1CF30,0x1D167,0x1D173,
    0x1D185,0x1D1AA,0x1D242,0x1DA00,0x1DA3B,0x1DA75,0x1DA84,0x1DA9B,
    0x1DAA1,0x1E000,0x1E008,0x1E01B,0x1E023,0x1E026,0x1E030,0x1E08F,
    0x1E130,0x1E2AE,0x1E2EC,0x1E4EB,0x1E5EE,0x1E6E3,0x1E6E6,0x1E6EE,
    0x1E6F5,0x1E6FF,0x1E8D0,0x1E944,0x1F3FB,0xE0001,0xE0020,0xE0100,
};
constexpr u8 case_ignorable_span[]{
    0,0,0,0,0,0,0,0,
    0,1,191,1,0,1,0,6,
    0,0,44,0,1,1,0,0,
    5,10,0,0,20,0,7,9,
    3,0,0,26,10,10,0,0,
    23,2,0,1,8,57,0,0,
    7,0,6,1,0,0,0,3,
    0,1,0,1,0,1,1,2,
    0,1,0,1,0,4,1,0,
    1,5,0,0,0,3,0,1,
    1,0,0,0,0,0,0,2,
    2,3,1,1,0,0,0,0,
    1,1,1,1,3,0,1,0,
    0,2,0,0,6,8,0,8,
    0,6,1,0,0,0,13,4,
    1,10,35,0,3,5,1,1,
    1,2,3,0,1,0,0,0,
    2,2,1,1,1,1,6,0,
    10,0,0,4,0,1,0,2,
    1,0,2,1,0,0,6,0,
    0,7,9,0,0,45,11,3,
    0,4,0,0,8,1,3,1,
    2,0,1,0,2,7,1,5,
    2,12,6,0,0,1,62,0,
    100,0,2,2,2,2,1,4,
    1,0,0,4,4,9,0,0,
    12,32,1,2,0,0,31,0,
    0,3,4,0,5,2,0,5,
    0,3,9,0,3,1,33,0,
    2,3,1,0,0,0,1,0,
    1,17,0,7,10,2,0,3,
    1,0,1,5,1,1,0,0,
    0,0,0,2,1,1,0,0,
    1,1,0,4,2,0,0,0,
    0,16,15,0,15,0,0,0,
    0,0,0,0,0,0,1,0,
    2,0,0,4,5,41,8,2,
    1,3,2,0,1,3,0,4,
    0,1,0,5,10,3,0,14,
    0,1,2,3,1,0,0,0,
    2,4,7,0,1,8,3,0,
    2,0,1,0,0,0,7,1,
    1,0,6,4,5,0,0,0,
    1,7,2,0,0,5,0,1,
    1,3,1,1,1,7,0,1,
    0,0,5,0,0,0,3,4,
    8,1,1,0,0,3,1,0,
    9,5,3,0,5,2,12,1,
    0,2,0,6,5,0,21,6,
    1,1,5,0,1,6,0,1,
    0,0,0,1,1,4,0,0,
    0,16,14,11,2,4,6,3,
    2,1,0,16,1,1,1,3,
    6,1,1,3,45,22,2,15,
    6,3,2,54,49,0,0,4,
    14,6,16,6,1,4,61,0,
    13,0,3,4,1,0,0,1,
    0,0,6,7,4,0,95,239,
};
// Spans keep the inclusive endpoint without struct alignment padding per entry.
// Bounds and ordering are validated at compile time; nothing is retained in the
// binary and there is no startup work.
template <usize N, class Span> constexpr bool valid_property_ranges(const u32 (&starts)[N], const Span (&spans)[N]) noexcept {
    for (usize i = 0; i < N; ++i) {
        if (spans[i] > 339 || starts[i] > 0x10FFFF || spans[i] > 0x10FFFF - starts[i] ||
            (i && starts[i - 1] + spans[i - 1] >= starts[i])) return false;
    }
    return true;
}
constexpr bool valid_lowercase_ranges() noexcept {
    for (usize i = 0; i < sizeof(lowercase_ranges) / sizeof(lowercase_ranges[0]); ++i) {
        const auto range = lowercase_ranges[i];
        if (range.span > 148 || (range.stride != 1 && range.stride != 2) || range.span % range.stride ||
            range.first > 0x10FFFF || range.span > 0x10FFFF - range.first ||
            (i && lowercase_ranges[i - 1].first + lowercase_ranges[i - 1].span >= range.first)) return false;
    }
    return true;
}
static_assert(valid_lowercase_ranges());
struct LowerTables {
    u32 first[186];i32 delta[186];u8 span[186],stride[186];
};
consteval LowerTables lower_tables() noexcept {
    LowerTables result{};
    for(u32 i=0;i<186;++i){const auto range=lowercase_ranges[i];
        result.first[i]=range.first;result.delta[i]=range.delta;
        result.span[i]=static_cast<u8>(range.span);result.stride[i]=range.stride;}
    return result;
}
static_assert(sizeof(lowercase_ranges)/sizeof(lowercase_ranges[0])==186);
constexpr auto lowercase_tables=lower_tables();
static_assert(valid_property_ranges(cased_first, cased_span));
static_assert(valid_property_ranges(case_ignorable_first, case_ignorable_span));
static_assert(sizeof(lowercase_tables) + sizeof(cased_first) + sizeof(cased_span) +
    sizeof(case_ignorable_first) + sizeof(case_ignorable_span) == 5128);
template <usize N, class Span> bool contains(const u32 (&starts)[N], const Span (&spans)[N], u32 value) noexcept {
    usize first = 0, last = N;
    while (first < last) {
        const usize middle = first + (last - first) / 2;
        const u32 start = starts[middle];
        if (value < start) last = middle;
        else if (value > start + spans[middle]) first = middle + 1;
        else return true;
    }
    return false;
}
u32 lowercase(u32 value) noexcept {
    if (value <= 0x7F) return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
    usize first = 0, last = sizeof(lowercase_ranges) / sizeof(lowercase_ranges[0]);
    while (first < last) {
        const usize middle = first + (last - first) / 2;
        const u32 start=lowercase_tables.first[middle];
        if (value < start) last = middle;
        else if (value > start + lowercase_tables.span[middle]) first = middle + 1;
        else return ((value - start) & (lowercase_tables.stride[middle]-1)) == 0 ?
            static_cast<u32>(static_cast<i32>(value) + lowercase_tables.delta[middle]) : value;
    }
    return value;
}
struct Folded {u32 scalars[kFriendSearchMaximumBytes];u32 count;};
bool final_sigma(ConstSlice<u32> original, u32 index) noexcept {
    bool preceding = false;
    for (u32 i = index; i; ) {
        const u32 value = original[--i];
        if (contains(case_ignorable_first, case_ignorable_span, value)) continue;
        preceding = contains(cased_first, cased_span, value);break;
    }
    if (!preceding) return false;
    for (u32 i = index + 1; i < original.count; ++i) {
        const u32 value = original[i];
        if (contains(case_ignorable_first, case_ignorable_span, value)) continue;
        return !contains(cased_first, cased_span, value);
    }
    return true;
}
bool fold(Utf8View input, Folded* output) noexcept {
    if (!output || input.bytes > kFriendSearchMaximumBytes || (!input.data && input.bytes)) return false;
    u32 original[kFriendSearchMaximumBytes];u32 count = 0;
    for (u32 offset = 0; offset < input.bytes;) {
        const auto decoded = utf8_decode_one(input.data + offset, input.bytes - offset);
        if (!decoded.valid) return false;
        original[count++] = decoded.codepoint;offset += decoded.width;
    }
    output->count = 0;
    // U+0130 is the only full default lowercase expansion. Its two UTF-8
    // source bytes bound its two output scalars, so 128 slots always suffice.
    for (u32 i = 0; i < count; ++i) {
        const u32 value = original[i];
        if (value == 0x130) {
            output->scalars[output->count++] = 0x69;
            output->scalars[output->count++] = 0x307;
        } else output->scalars[output->count++] = value == 0x3A3 && final_sigma({original, count}, i) ?
            0x3C2 : lowercase(value);
    }
    return true;
}
bool substring(const Folded& name, const Folded& query) noexcept {
    if (query.count > name.count) return false;
    for (u32 start = 0; start <= name.count - query.count; ++start) {
        bool same = true;
        for (u32 i = 0; i < query.count; ++i) if (name.scalars[start + i] != query.scalars[i]) {same = false;break;}
        if (same) return true;
    }
    return false;
}
struct Utf16Cursor {const Folded* text;u32 index;u16 pending_low;};
bool next_unit(Utf16Cursor* cursor, u16* output) noexcept {
    if (cursor->pending_low) {*output = cursor->pending_low;cursor->pending_low = 0;return true;}
    if (cursor->index == cursor->text->count) return false;
    const u32 value = cursor->text->scalars[cursor->index++];
    if (value < 0x10000) *output = static_cast<u16>(value);
    else {
        const u32 pair = value - 0x10000;
        *output = static_cast<u16>(0xD800 + (pair >> 10));
        cursor->pending_low = static_cast<u16>(0xDC00 + (pair & 0x3FF));
    }
    return true;
}
} // namespace
FriendSearchResult friend_search(const Utf8View& query, const Utf8View& username,
    const Utf8View& nickname, const Utf8View& display_name) noexcept {
    Folded needle, name;
    if (!fold(query, &needle)) return {FriendSearchError::invalid_query, false};
    bool matched = false;
    const Utf8View fields[]{username, nickname, display_name};
    for (const auto field : fields) {
        if (!fold(field, &name)) return {FriendSearchError::invalid_name, false};
        matched = substring(name, needle) || matched;
    }
    return {FriendSearchError::none, matched};
}
FriendCompareResult friend_name_compare(Utf8View left, Utf8View right) noexcept {
    Folded a, b;
    if (!fold(left, &a) || !fold(right, &b)) return {FriendSearchError::invalid_name, 0};
    Utf16Cursor first{&a, 0, 0}, second{&b, 0, 0};
    for (;;) {
        u16 x = 0, y = 0;
        const bool has_x = next_unit(&first, &x), has_y = next_unit(&second, &y);
        if (!has_x || !has_y) return {FriendSearchError::none, has_x ? 1 : has_y ? -1 : 0};
        if (x != y) return {FriendSearchError::none, x < y ? -1 : 1};
    }
}
} // namespace maicord
