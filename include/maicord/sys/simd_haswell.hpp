#pragma once

// AVX2 for a hot loop that is written by hand. The compiler baseline has the other instructions of Haswell (BMI1,
// BMI2, LZCNT, MOVBE, F16C) for all code, but not AVX2. A function with MAICORD_HASWELL holds the
// whole loop: the compiler does not inline it into a caller without the attribute. There is one version of
// each loop and no CPU dispatch. Declare an intrinsic here, with the same attribute, when a loop needs one.
#define MAICORD_HASWELL __attribute__((__target__("avx2")))
// A hand loop that also uses FMA, only where no output must keep its exact rounding. Both systems compile with
// -ffp-contract=off, and its body starts with `#pragma clang fp contract(off)`, so it fuses only where the code calls
// an fmadd intrinsic.
#define MAICORD_HASWELL_FMA __attribute__((__target__("avx2,fma")))

#include "maicord/sys/simd.hpp"

#if !defined(MAICORD_SDK_INCLUDES)

// The 256-bit integer intrinsics, defined as clang's headers define them.
typedef long long __m256i __attribute__((__vector_size__(32), __aligned__(32)));
typedef long long __m256i_u __attribute__((__vector_size__(32), __aligned__(1)));
typedef long long __v4di __attribute__((__vector_size__(32)));
typedef unsigned long long __v4du __attribute__((__vector_size__(32)));
typedef int __v8si __attribute__((__vector_size__(32)));
typedef short __v16hi __attribute__((__vector_size__(32)));
typedef unsigned short __v16hu __attribute__((__vector_size__(32)));
typedef unsigned char __v32qu __attribute__((__vector_size__(32)));
typedef char __v32qi __attribute__((__vector_size__(32)));
typedef signed char __v32qs __attribute__((__vector_size__(32)));

#define MAICORD_SIMD_HASWELL                                                                                           \
    static __inline__ __attribute__((__always_inline__, __nodebug__, __target__("avx2"), __min_vector_width__(256)))

#define _mm256_permute4x64_epi64(V, M) ((__m256i)__builtin_ia32_permdi256((__v4di)(__m256i)(V), (int)(M)))

MAICORD_SIMD_HASWELL __m256i _mm256_setzero_si256(void) { return __extension__(__m256i)(__v4di){0, 0, 0, 0}; }
MAICORD_SIMD_HASWELL __m256i _mm256_set1_epi16(short __w) {
    return __extension__(__m256i)(__v16hi){__w, __w, __w, __w, __w, __w, __w, __w,
                                           __w, __w, __w, __w, __w, __w, __w, __w};
}
MAICORD_SIMD_HASWELL __m256i _mm256_set1_epi32(int __i) {
    return __extension__(__m256i)(__v8si){__i, __i, __i, __i, __i, __i, __i, __i};
}
MAICORD_SIMD_HASWELL __m256i _mm256_loadu_si256(__m256i_u const* __p) {
    struct __loadu_si256 { __m256i_u __v; } __attribute__((__packed__, __may_alias__));
    return ((const struct __loadu_si256*)__p)->__v;
}
MAICORD_SIMD_HASWELL void _mm256_storeu_si256(__m256i_u* __p, __m256i __a) {
    struct __storeu_si256 { __m256i_u __v; } __attribute__((__packed__, __may_alias__));
    ((struct __storeu_si256*)__p)->__v = __a;
}
MAICORD_SIMD_HASWELL int _mm256_testz_si256(__m256i __a, __m256i __b) {
    return __builtin_ia32_ptestz256((__v4di)__a, (__v4di)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_cvtepi16_epi32(__m128i __V) {
    return (__m256i)__builtin_convertvector((__v8hi)__V, __v8si);
}
MAICORD_SIMD_HASWELL __m256i _mm256_abs_epi16(__m256i __a) { return (__m256i)__builtin_elementwise_abs((__v16hi)__a); }
MAICORD_SIMD_HASWELL __m256i _mm256_abs_epi32(__m256i __a) { return (__m256i)__builtin_elementwise_abs((__v8si)__a); }
MAICORD_SIMD_HASWELL __m256i _mm256_add_epi16(__m256i __a, __m256i __b) {
    return (__m256i)((__v16hu)__a + (__v16hu)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_add_epi32(__m256i __a, __m256i __b) { return (__m256i)((__v8su)__a + (__v8su)__b); }
MAICORD_SIMD_HASWELL __m256i _mm256_add_epi64(__m256i __a, __m256i __b) { return (__m256i)((__v4du)__a + (__v4du)__b); }
MAICORD_SIMD_HASWELL __m256i _mm256_adds_epu16(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_elementwise_add_sat((__v16hu)__a, (__v16hu)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_sub_epi16(__m256i __a, __m256i __b) {
    return (__m256i)((__v16hu)__a - (__v16hu)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_sub_epi32(__m256i __a, __m256i __b) { return (__m256i)((__v8su)__a - (__v8su)__b); }
MAICORD_SIMD_HASWELL __m256i _mm256_subs_epu16(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_elementwise_sub_sat((__v16hu)__a, (__v16hu)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_min_epu16(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_elementwise_min((__v16hu)__a, (__v16hu)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_min_epu32(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_elementwise_min((__v8su)__a, (__v8su)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_min_epi32(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_elementwise_min((__v8si)__a, (__v8si)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_max_epi32(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_elementwise_max((__v8si)__a, (__v8si)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_cmpgt_epi32(__m256i __a, __m256i __b) {
    return (__m256i)((__v8si)__a > (__v8si)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_mullo_epi16(__m256i __a, __m256i __b) {
    return (__m256i)((__v16hu)__a * (__v16hu)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_mullo_epi32(__m256i __a, __m256i __b) {
    return (__m256i)((__v8su)__a * (__v8su)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_mulhi_epu16(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_ia32_pmulhuw256((__v16hu)__a, (__v16hu)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_madd_epi16(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_ia32_pmaddwd256((__v16hi)__a, (__v16hi)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_sad_epu8(__m256i __a, __m256i __b) {
    return __builtin_ia32_psadbw256((__v32qu)__a, (__v32qu)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_sign_epi16(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_ia32_psignw256((__v16hi)__a, (__v16hi)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_sign_epi32(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_ia32_psignd256((__v8si)__a, (__v8si)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_packs_epi32(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_ia32_packssdw256((__v8si)__a, (__v8si)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_and_si256(__m256i __a, __m256i __b) { return (__m256i)((__v4du)__a & (__v4du)__b); }
MAICORD_SIMD_HASWELL __m256i _mm256_srli_epi16(__m256i __a, int __count) {
    return (__m256i)__builtin_ia32_psrlwi256((__v16hi)__a, __count);
}
MAICORD_SIMD_HASWELL __m256i _mm256_srli_epi32(__m256i __a, int __count) {
    return (__m256i)__builtin_ia32_psrldi256((__v8si)__a, __count);
}
MAICORD_SIMD_HASWELL __m256i _mm256_srli_epi64(__m256i __a, int __count) {
    return __builtin_ia32_psrlqi256((__v4di)__a, __count);
}
MAICORD_SIMD_HASWELL __m256i _mm256_srai_epi16(__m256i __a, int __count) {
    return (__m256i)__builtin_ia32_psrawi256((__v16hi)__a, __count);
}
MAICORD_SIMD_HASWELL __m256i _mm256_srai_epi32(__m256i __a, int __count) {
    return (__m256i)__builtin_ia32_psradi256((__v8si)__a, __count);
}
MAICORD_SIMD_HASWELL __m256 _mm256_cvtepi32_ps(__m256i __a) {
    return (__m256)__builtin_convertvector((__v8si)__a, __v8sf);
}
MAICORD_SIMD_HASWELL __m256i _mm256_cvttps_epi32(__m256 __a) {
    return (__m256i)__builtin_ia32_cvttps2dq256((__v8sf)__a);
}
MAICORD_SIMD_HASWELL __m256i _mm256_cvtps_epi32(__m256 __a) {
    return (__m256i)__builtin_ia32_cvtps2dq256((__v8sf)__a);
}
MAICORD_SIMD_HASWELL __m256 _mm256_div_ps(__m256 __a, __m256 __b) { return (__m256)((__v8sf)__a / (__v8sf)__b); }
MAICORD_SIMD_HASWELL __m256i _mm256_set1_epi8(char __b) {
    return __extension__(__m256i)(__v32qi){__b, __b, __b, __b, __b, __b, __b, __b, __b, __b, __b, __b, __b, __b, __b, __b,
                                           __b, __b, __b, __b, __b, __b, __b, __b, __b, __b, __b, __b, __b, __b, __b, __b};
}
MAICORD_SIMD_HASWELL __m256i _mm256_or_si256(__m256i __a, __m256i __b) { return (__m256i)((__v4du)__a | (__v4du)__b); }
MAICORD_SIMD_HASWELL __m256i _mm256_xor_si256(__m256i __a, __m256i __b) { return (__m256i)((__v4du)__a ^ (__v4du)__b); }
MAICORD_SIMD_HASWELL __m256i _mm256_add_epi8(__m256i __a, __m256i __b) { return (__m256i)((__v32qu)__a + (__v32qu)__b); }
MAICORD_SIMD_HASWELL __m256i _mm256_sub_epi8(__m256i __a, __m256i __b) { return (__m256i)((__v32qu)__a - (__v32qu)__b); }
MAICORD_SIMD_HASWELL __m256i _mm256_abs_epi8(__m256i __a) { return (__m256i)__builtin_elementwise_abs((__v32qs)__a); }
MAICORD_SIMD_HASWELL __m256i _mm256_avg_epu8(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_ia32_pavgb256((__v32qu)__a, (__v32qu)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_adds_epu8(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_elementwise_add_sat((__v32qu)__a, (__v32qu)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_subs_epu8(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_elementwise_sub_sat((__v32qu)__a, (__v32qu)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_min_epu8(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_elementwise_min((__v32qu)__a, (__v32qu)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_cmpeq_epi8(__m256i __a, __m256i __b) {
    return (__m256i)((__v32qi)__a == (__v32qi)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_cmpgt_epi8(__m256i __a, __m256i __b) {
    return (__m256i)((__v32qs)__a > (__v32qs)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_maddubs_epi16(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_ia32_pmaddubsw256((__v32qi)__a, (__v32qi)__b);
}

// For the image decoders.
#define _mm256_permute2x128_si256(V1, V2, M)                                                                           \
    ((__m256i)__builtin_ia32_permti256((__m256i)(V1), (__m256i)(V2), (int)(M)))
static __inline__ __attribute__((__always_inline__, __nodebug__, __target__("lzcnt")))
unsigned int _lzcnt_u32(unsigned int __X) { return __builtin_ia32_lzcnt_u32(__X); }
MAICORD_SIMD_HASWELL __m128i _mm256_castsi256_si128(__m256i __a) {
    return __builtin_shufflevector((__v4di)__a, (__v4di)__a, 0, 1);
}
MAICORD_SIMD_HASWELL __m256i _mm256_cvtepu8_epi16(__m128i __V) {
    return (__m256i)__builtin_convertvector((__v16qu)__V, __v16hi);
}
MAICORD_SIMD_HASWELL __m256i _mm256_min_epi16(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_elementwise_min((__v16hi)__a, (__v16hi)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_max_epi16(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_elementwise_max((__v16hi)__a, (__v16hi)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_cmpgt_epi16(__m256i __a, __m256i __b) {
    return (__m256i)((__v16hi)__a > (__v16hi)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_andnot_si256(__m256i __a, __m256i __b) {
    return (__m256i)(~(__v4du)__a & (__v4du)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_slli_epi32(__m256i __a, int __n) {
    return (__m256i)__builtin_ia32_pslldi256((__v8si)__a, __n);
}
MAICORD_SIMD_HASWELL __m256i _mm256_slli_epi16(__m256i __a, int __n) {
    return (__m256i)__builtin_ia32_psllwi256((__v16hi)__a, __n);
}
MAICORD_SIMD_HASWELL __m256i _mm256_blendv_epi8(__m256i __a, __m256i __b, __m256i __m) {
    return (__m256i)__builtin_ia32_pblendvb256((__v32qi)__a, (__v32qi)__b, (__v32qi)__m);
}
MAICORD_SIMD_HASWELL __m256i _mm256_packus_epi16(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_ia32_packuswb256((__v16hi)__a, (__v16hi)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_unpacklo_epi16(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_shufflevector((__v16hi)__a, (__v16hi)__b, 0, 16, 1, 17, 2, 18, 3, 19, 8, 24, 9, 25, 10,
                                            26, 11, 27);
}
MAICORD_SIMD_HASWELL __m256i _mm256_unpackhi_epi16(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_shufflevector((__v16hi)__a, (__v16hi)__b, 4, 20, 5, 21, 6, 22, 7, 23, 12, 28, 13, 29,
                                            14, 30, 15, 31);
}
MAICORD_SIMD_HASWELL __m256i _mm256_shuffle_epi8(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_ia32_pshufb256((__v32qi)__a, (__v32qi)__b);
}
MAICORD_SIMD_HASWELL __m256i _mm256_permutevar8x32_epi32(__m256i __a, __m256i __b) {
    return (__m256i)__builtin_ia32_permvarsi256((__v8si)__a, (__v8si)__b);
}
// For the ChaCha20 rows.
#define _mm256_shuffle_epi32(A, I) ((__m256i)__builtin_ia32_pshufd256((__v8si)(__m256i)(A), (int)(I)))
MAICORD_SIMD_HASWELL __m256i _mm256_broadcastsi128_si256(__m128i __X) {
    return (__m256i)__builtin_shufflevector((__v2di)__X, (__v2di)__X, 0, 1, 0, 1);
}
MAICORD_SIMD_HASWELL int _mm256_movemask_epi8(__m256i __a) { return __builtin_ia32_pmovmskb256((__v32qi)__a); }

// FMA, for float loops whose rounding is not part of an output that must stay the same.
#define MAICORD_SIMD_FMA(width)                                                                                        \
    static __inline__ __attribute__((__always_inline__, __nodebug__, __target__("fma"), __min_vector_width__(width)))
MAICORD_SIMD_FMA(128) __m128 _mm_fmadd_ps(__m128 __A, __m128 __B, __m128 __C) {
    return (__m128)__builtin_elementwise_fma((__v4sf)__A, (__v4sf)__B, (__v4sf)__C);
}
MAICORD_SIMD_FMA(256) __m256 _mm256_fmadd_ps(__m256 __A, __m256 __B, __m256 __C) {
    return (__m256)__builtin_elementwise_fma((__v8sf)__A, (__v8sf)__B, (__v8sf)__C);
}
MAICORD_SIMD_FMA(128) __m128 _mm_fmadd_ss(__m128 __A, __m128 __B, __m128 __C) {
    __A[0] = __builtin_elementwise_fma(__A[0], __B[0], __C[0]);
    return __A;
}
#undef MAICORD_SIMD_FMA

#undef MAICORD_SIMD_HASWELL

#endif
