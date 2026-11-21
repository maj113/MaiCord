// en-US system fallback policy observed through the public DirectWrite
// fallback API; method in tools/font_reference/system_fallback_data.py.
// Solver input model SHA-256: 746e410e22f71ba6d68b961dd5165e60341cd519d8e8dfbed24851417ce59e17
constexpr u32 system_fallback_firsts[]{
    0x0,0x530,0x590,0x600,0x700,0x750,0x7c0,0x800,0x860,0x870,
    0x8a0,0x900,0x980,0xa00,0xa80,0xb00,0xb80,0xc00,0xc80,0xd00,
    0xd80,0xe00,0xe80,0xf00,0x1000,0x10a0,0x1100,0x1200,0x13a0,0x1400,
    0x1700,0x1780,0x1800,0x18f6,0x1900,0x1950,0x1980,0x19e0,0x1a00,0x1a20,
    0x1ab0,0x1b00,0x1c50,0x1c80,0x1cc0,0x1cd0,0x1cfc,0x1d00,0x2000,0x202f,
    0x2030,0x20d0,0x2100,0x2139,0x213a,0x2150,0x2300,0x231a,0x231c,0x2328,
    0x2329,0x23cf,0x23d0,0x23e9,0x23f4,0x23f8,0x23fb,0x2400,0x2440,0x2460,
    0x25fd,0x25ff,0x2600,0x2605,0x260e,0x260f,0x2611,0x2612,0x2614,0x2616,
    0x2618,0x2619,0x2620,0x2621,0x2622,0x2624,0x2626,0x2627,0x262a,0x262b,
    0x262e,0x2630,0x2638,0x263b,0x2640,0x2641,0x2642,0x2643,0x2648,0x2654,
    0x265f,0x2660,0x2668,0x2669,0x267b,0x267c,0x267e,0x2680,0x2692,0x2698,
    0x2699,0x269a,0x269b,0x269d,0x26a0,0x26a2,0x26a7,0x26a8,0x26aa,0x26ac,
    0x26b0,0x26b2,0x26bd,0x26bf,0x26c4,0x26c6,0x26c8,0x26c9,0x26ce,0x26d0,
    0x26d1,0x26d2,0x26d3,0x26d5,0x26e9,0x26eb,0x26f0,0x26f6,0x26f7,0x26f9,
    0x26fa,0x26fb,0x26fd,0x26fe,0x2702,0x2703,0x2705,0x2706,0x2708,0x270d,
    0x270f,0x2710,0x2712,0x2713,0x2714,0x2715,0x2716,0x2717,0x271d,0x271e,
    0x2721,0x2722,0x2728,0x2729,0x2733,0x2735,0x2744,0x2745,0x2747,0x2748,
    0x274c,0x274d,0x274e,0x274f,0x2753,0x2756,0x2757,0x2758,0x2763,0x2765,
    0x2795,0x2798,0x27a1,0x27a2,0x27b0,0x27b1,0x27bf,0x27c0,0x2b1b,0x2b1d,
    0x2b50,0x2b51,0x2b55,0x2b56,0x2c00,0x2c60,0x2c80,0x2d00,0x2d30,0x2d80,
    0x2de0,0x2e00,0x2e80,0x2f00,0x2fe0,0x3000,0x3100,0x3130,0x31b8,0x31e4,
    0x321f,0x3250,0x3251,0x3280,0x32cc,0x32d0,0x3377,0x337b,0x3380,0x33e0,
    0x3400,0x4e00,0xa4d0,0xa500,0xa640,0xa6a0,0xa700,0xa720,0xa800,0xa830,
    0xa840,0xa880,0xa8e0,0xa900,0xa960,0xa980,0xa9e0,0xaa00,0xaa60,0xaa80,
    0xab00,0xab30,0xabc0,0xac00,0xd7b0,0xe000,0xe76c,0xe76d,0xe78d,0xe797,
    0xe7c7,0xe7c9,0xe7e7,0xe7f4,0xe815,0xe865,0xeeb8,0xf303,0xf34c,0xf3a0,
    0xf5f2,0xf634,0xf849,0xf900,0xfb07,0xfb10,0xfb1d,0xfb50,0xfc00,0xfdd0,
    0xfdf0,0xfe00,0xfe1a,0xfe27,0xfe30,0xfe70,0xfeff,0xff61,0xffe0,0xfff0,
    0xfffe,0x10000,0x10080,0x10100,0x10140,0x10280,0x102e0,0x10300,0x10350,0x10380,
    0x103e0,0x10450,0x10480,0x104b0,0x10500,0x10780,0x107c0,0x10800,0x10840,0x10860,
    0x108b0,0x108e0,0x10900,0x10940,0x10980,0x10a00,0x10a80,0x10b40,0x10b80,0x10bb0,
    0x10c49,0x10c50,0x10c80,0x10ec0,0x10f00,0x11000,0x11080,0x110d0,0x11100,0x11150,
    0x111e0,0x11200,0x11250,0x11280,0x11660,0x11680,0x116d0,0x11700,0x11ac0,0x11b00,
    0x11c00,0x12000,0x12480,0x12f90,0x13000,0x14400,0x14680,0x16a39,0x16a40,0x16b90,
    0x16e40,0x17000,0x1b002,0x1b130,0x1b170,0x1bcb0,0x1d000,0x1d100,0x1d200,0x1d250,
    0x1d2e0,0x1d300,0x1d360,0x1d400,0x1d800,0x1df05,0x1e000,0x1e030,0x1e100,0x1e300,
    0x1e4d0,0x1e500,0x1e7e0,0x1e800,0x1e8e0,0x1e900,0x1e960,0x1ec70,0x1ecc0,0x1ed00,
    0x1ed50,0x1f020,0x1f021,0x1f18e,0x1f18f,0x1f191,0x1f19b,0x1f1e6,0x1f200,0x1f201,
    0x1f202,0x1f21a,0x1f21b,0x1f22f,0x1f230,0x1f232,0x1f237,0x1f238,0x1f23b,0x1f250,
    0x1f252,0x1f300,0x1f322,0x1f324,0x1f394,0x1f396,0x1f398,0x1f399,0x1f39c,0x1f39e,
    0x1f3f1,0x1f3f3,0x1f3f6,0x1f3f7,0x1f4fe,0x1f4ff,0x1f53e,0x1f549,0x1f54f,0x1f550,
    0x1f568,0x1f56f,0x1f571,0x1f573,0x1f57b,0x1f587,0x1f588,0x1f58a,0x1f58e,0x1f590,
    0x1f591,0x1f595,0x1f597,0x1f5a4,0x1f5a6,0x1f5a8,0x1f5a9,0x1f5b1,0x1f5b3,0x1f5bc,
    0x1f5bd,0x1f5c2,0x1f5c5,0x1f5d1,0x1f5d4,0x1f5dc,0x1f5df,0x1f5e1,0x1f5e2,0x1f5e3,
    0x1f5e4,0x1f5e8,0x1f5e9,0x1f5ef,0x1f5f0,0x1f5f3,0x1f5f4,0x1f5fa,0x1f650,0x1f680,
    0x1f6c6,0x1f6cb,0x1f6d3,0x1f6d5,0x1f6d9,0x1f6dc,0x1f6e6,0x1f6e9,0x1f6ea,0x1f6eb,
    0x1f6ed,0x1f6f0,0x1f6f1,0x1f6f3,0x1f6fd,0x1f774,0x1f7d9,0x1f7e0,0x1f7ec,0x1f7f0,
    0x1f7f1,0x1f800,0x1f900,0x1f90c,0x1f93b,0x1f93c,0x1f946,0x1f947,0x1fa00,0x1fa70,
    0x1fa7d,0x1fa80,0x1fa8b,0x1fa8e,0x1fac7,0x1fac8,0x1fac9,0x1facd,0x1fadd,0x1fadf,
    0x1faeb,0x1faef,0x1faf9,0x1fb00,0x2a6e0,0x2b81e,0x2b820,0x2ee5e,0x2ee60,0x2f800,
    0x2fa20,0x30000,0x323b0,
};
constexpr u16 system_fallback_lists[]{
    native_fallback_list(0,5),native_fallback_list(5,2),native_fallback_list(7,2),native_fallback_list(9,1),native_fallback_list(10,1),native_fallback_list(11,2),
    native_fallback_list(13,1),native_fallback_list(14,1),native_fallback_list(15,0),native_fallback_list(15,0),native_fallback_list(9,1),native_fallback_list(15,1),
    native_fallback_list(15,1),native_fallback_list(15,1),native_fallback_list(15,1),native_fallback_list(15,1),native_fallback_list(15,1),native_fallback_list(15,1),
    native_fallback_list(15,1),native_fallback_list(15,1),native_fallback_list(15,1),native_fallback_list(16,1),native_fallback_list(16,1),native_fallback_list(17,1),
    native_fallback_list(18,1),native_fallback_list(9,1),native_fallback_list(19,1),native_fallback_list(13,1),native_fallback_list(20,1),native_fallback_list(21,2),
    native_fallback_list(14,1),native_fallback_list(16,1),native_fallback_list(23,2),native_fallback_list(15,0),native_fallback_list(14,1),native_fallback_list(25,1),
    native_fallback_list(26,1),native_fallback_list(16,1),native_fallback_list(16,1),native_fallback_list(14,1),native_fallback_list(27,5),native_fallback_list(14,1),
    native_fallback_list(15,1),native_fallback_list(0,5),native_fallback_list(32,3),native_fallback_list(35,4),native_fallback_list(14,1),native_fallback_list(0,5),
    native_fallback_list(39,2),native_fallback_list(41,1),native_fallback_list(39,2),native_fallback_list(42,1),native_fallback_list(39,2),native_fallback_list(43,1),
    native_fallback_list(39,2),native_fallback_list(44,4),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),
    native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),
    native_fallback_list(39,2),native_fallback_list(42,1),native_fallback_list(42,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),native_fallback_list(43,1),native_fallback_list(39,2),
    native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),
    native_fallback_list(43,1),native_fallback_list(48,2),native_fallback_list(10,1),native_fallback_list(9,1),native_fallback_list(10,1),native_fallback_list(9,1),
    native_fallback_list(13,1),native_fallback_list(13,1),native_fallback_list(9,1),native_fallback_list(39,2),native_fallback_list(50,1),native_fallback_list(50,1),
    native_fallback_list(51,1),native_fallback_list(52,2),native_fallback_list(54,1),native_fallback_list(55,3),native_fallback_list(54,1),native_fallback_list(58,2),
    native_fallback_list(60,2),native_fallback_list(50,1),native_fallback_list(58,2),native_fallback_list(60,2),native_fallback_list(50,1),native_fallback_list(60,2),
    native_fallback_list(50,1),native_fallback_list(50,1),native_fallback_list(60,2),native_fallback_list(50,1),native_fallback_list(62,4),native_fallback_list(66,4),
    native_fallback_list(9,1),native_fallback_list(13,1),native_fallback_list(9,1),native_fallback_list(13,1),native_fallback_list(9,1),native_fallback_list(70,4),
    native_fallback_list(14,1),native_fallback_list(15,1),native_fallback_list(74,1),native_fallback_list(14,1),native_fallback_list(15,1),native_fallback_list(14,1),
    native_fallback_list(19,1),native_fallback_list(75,1),native_fallback_list(18,1),native_fallback_list(14,1),native_fallback_list(18,1),native_fallback_list(14,1),
    native_fallback_list(13,1),native_fallback_list(76,3),native_fallback_list(14,1),native_fallback_list(19,1),native_fallback_list(19,1),native_fallback_list(15,0),
    native_fallback_list(15,0),native_fallback_list(15,0),native_fallback_list(15,0),native_fallback_list(15,0),native_fallback_list(15,0),native_fallback_list(15,0),
    native_fallback_list(15,0),native_fallback_list(15,0),native_fallback_list(15,0),native_fallback_list(15,0),native_fallback_list(15,0),native_fallback_list(15,0),
    native_fallback_list(15,0),native_fallback_list(15,0),native_fallback_list(15,0),native_fallback_list(15,0),native_fallback_list(15,0),native_fallback_list(79,3),
    native_fallback_list(14,1),native_fallback_list(9,1),native_fallback_list(7,2),native_fallback_list(7,2),native_fallback_list(9,1),native_fallback_list(15,0),
    native_fallback_list(9,1),native_fallback_list(54,1),native_fallback_list(39,2),native_fallback_list(27,5),native_fallback_list(54,1),native_fallback_list(9,1),
    native_fallback_list(82,2),native_fallback_list(84,2),native_fallback_list(50,1),native_fallback_list(9,1),native_fallback_list(15,0),native_fallback_list(14,1),
    native_fallback_list(14,1),native_fallback_list(14,1),native_fallback_list(15,0),native_fallback_list(10,1),native_fallback_list(14,1),native_fallback_list(10,1),
    native_fallback_list(14,1),native_fallback_list(10,1),native_fallback_list(42,1),native_fallback_list(10,1),native_fallback_list(13,1),native_fallback_list(20,1),
    native_fallback_list(14,1),native_fallback_list(32,3),native_fallback_list(15,0),native_fallback_list(10,1),native_fallback_list(10,1),native_fallback_list(14,1),
    native_fallback_list(15,0),native_fallback_list(14,1),native_fallback_list(10,1),native_fallback_list(15,0),native_fallback_list(86,3),native_fallback_list(10,1),
    native_fallback_list(14,1),native_fallback_list(10,1),native_fallback_list(14,1),native_fallback_list(10,1),native_fallback_list(14,1),native_fallback_list(15,0),
    native_fallback_list(14,1),native_fallback_list(15,0),native_fallback_list(14,1),native_fallback_list(10,1),native_fallback_list(14,1),native_fallback_list(15,1),
    native_fallback_list(15,1),native_fallback_list(14,1),native_fallback_list(15,1),native_fallback_list(14,1),native_fallback_list(15,0),native_fallback_list(14,1),
    native_fallback_list(41,1),native_fallback_list(14,1),native_fallback_list(18,1),native_fallback_list(89,2),native_fallback_list(14,1),native_fallback_list(15,0),
    native_fallback_list(14,1),native_fallback_list(10,1),native_fallback_list(15,0),native_fallback_list(14,1),native_fallback_list(10,1),native_fallback_list(14,1),
    native_fallback_list(10,1),native_fallback_list(15,0),native_fallback_list(14,1),native_fallback_list(15,0),native_fallback_list(14,1),native_fallback_list(50,1),
    native_fallback_list(15,0),native_fallback_list(15,0),native_fallback_list(14,1),native_fallback_list(15,0),native_fallback_list(14,1),native_fallback_list(42,1),
    native_fallback_list(14,1),native_fallback_list(15,0),native_fallback_list(14,1),native_fallback_list(42,1),native_fallback_list(15,0),native_fallback_list(48,2),
    native_fallback_list(5,2),native_fallback_list(32,3),native_fallback_list(14,1),native_fallback_list(15,0),native_fallback_list(14,1),native_fallback_list(15,0),
    native_fallback_list(14,1),native_fallback_list(15,0),native_fallback_list(13,1),native_fallback_list(14,1),native_fallback_list(15,0),native_fallback_list(13,1),
    native_fallback_list(15,0),native_fallback_list(14,1),native_fallback_list(15,0),native_fallback_list(14,1),native_fallback_list(42,1),native_fallback_list(91,2),
    native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),
    native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),
    native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),
    native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),
    native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),
    native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),
    native_fallback_list(15,0),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),
    native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),
    native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),
    native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),
    native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),
    native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),
    native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),
    native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(15,0),native_fallback_list(43,1),native_fallback_list(15,0),native_fallback_list(43,1),
    native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(15,0),native_fallback_list(43,1),
    native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(42,1),native_fallback_list(39,2),native_fallback_list(15,0),native_fallback_list(43,1),
    native_fallback_list(15,0),native_fallback_list(43,1),native_fallback_list(15,0),native_fallback_list(48,2),native_fallback_list(15,0),native_fallback_list(43,1),
    native_fallback_list(42,1),native_fallback_list(43,1),native_fallback_list(15,0),native_fallback_list(43,1),native_fallback_list(15,0),native_fallback_list(43,1),
    native_fallback_list(15,0),native_fallback_list(43,1),native_fallback_list(15,0),native_fallback_list(43,1),native_fallback_list(15,0),native_fallback_list(43,1),
    native_fallback_list(15,0),native_fallback_list(43,1),native_fallback_list(15,0),native_fallback_list(43,1),native_fallback_list(15,0),native_fallback_list(43,1),
    native_fallback_list(15,0),native_fallback_list(93,2),native_fallback_list(93,2),native_fallback_list(15,0),native_fallback_list(95,2),native_fallback_list(15,0),
    native_fallback_list(15,0),native_fallback_list(97,3),native_fallback_list(15,0),native_fallback_list(100,1),native_fallback_list(15,0),
};
// A token is the byte offset of a family name in system_fallback_names.
constexpr u16 system_fallback_tokens[]{23,0,7,54,32,23,54,23,32,23,62,23,80,0,32,88,99,113,132,145,159,62,159,166,159,182,199,0,7,23,54,32,23,54,32,0,23,88,32,23,7,166,7,221,23,7,236,249,7,32,236,256,236,256,275,145,236,275,145,236,7,236,7,236,275,256,297,236,275,256,23,7,32,316,322,340,23,32,159,23,236,275,23,236,236,275,62,7,32,32,159,221,7,354,367,367,379,236,354,391,379};
constexpr char system_fallback_names[]=
    "Ebrima\0"
    "Segoe UI Symbol\0"
    "Segoe UI\0"
    "Sans Serif Collection\0"
    "Calibri\0"
    "Segoe UI Historic\0"
    "MV Boli\0"
    "Nirmala UI\0"
    "Leelawadee UI\0"
    "Microsoft Himalaya\0"
    "Myanmar Text\0"
    "Malgun Gothic\0"
    "Gadugi\0"
    "Mongolian Baiti\0"
    "Microsoft Tai Le\0"
    "Microsoft New Tai Lue\0"
    "Segoe UI Emoji\0"
    "Yu Gothic UI\0"
    "Tahoma\0"
    "Microsoft YaHei UI\0"
    "Microsoft JhengHei UI\0"
    "Microsoft Yi Baiti\0"
    "Arial\0"
    "Microsoft PhagsPa\0"
    "Javanese Text\0"
    "MingLiU-ExtB\0"
    "SimSun-ExtB\0"
    "SimSun-ExtG\0"
    "MingLiU_MSCS-ExtB\0"
    ;
constexpr u32 system_fallback_max_families=5;
// Every name is nonempty, ASCII and shorter than 64 bytes, and every token is the start of a name.
static_assert([]() constexpr {
    u32 run=0;
    for(u32 i=0;i+1<sizeof(system_fallback_names);++i) {
        const u8 c=static_cast<u8>(system_fallback_names[i]);
        if(c>=128)return false;
        if(c){if(++run>=64)return false;}else{if(!run)return false;run=0;}
    }
    for(const u16 token:system_fallback_tokens)if(!system_fallback_names[token]||(token&&system_fallback_names[token-1]))return false;
    return run==0;
}());
constexpr NativeFallbackCascade system_fallback_policy=[]() constexpr {
    NativeFallbackCascade policy{};
    native_fallback_cascade_open({system_fallback_firsts,503},{system_fallback_lists,503},{system_fallback_tokens,101},sizeof(system_fallback_names),&policy);
    return policy;
}();
static_assert(system_fallback_policy.firsts.count==503);
