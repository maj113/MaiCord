#pragma once

// SIMD vector types and intrinsics: the SSE through AVX ones MaiCord and
// maiopus use (sys/libc's <immintrin.h> and its siblings include this, so
// it is C as well as C++). Defined as clang's headers define them, so the
// generated code is the same: vector extensions and the compiler's builtins,
// usable in C++ constant expressions where clang's are. Every unit compiles
// with /arch:AVX, so they need no target attributes; the AES intrinsics also need
// the C++ build's -maes. Add what you use.

#if defined(MAICORD_SDK_INCLUDES)
#include <immintrin.h>
#include <wmmintrin.h>
#else

#if !defined(__AVX__)
#error "maicord/sys/simd.hpp needs /arch:AVX"
#endif

typedef float __m128 __attribute__((__vector_size__(16), __aligned__(16)));
typedef float __m128_u __attribute__((__vector_size__(16), __aligned__(1)));
typedef double __m128d __attribute__((__vector_size__(16), __aligned__(16)));
typedef long long __m128i __attribute__((__vector_size__(16), __aligned__(16)));
typedef long long __m128i_u __attribute__((__vector_size__(16), __aligned__(1)));
typedef long long __m64 __attribute__((__vector_size__(8), __aligned__(8)));
typedef float __m256 __attribute__((__vector_size__(32), __aligned__(32)));
typedef float __m256_u __attribute__((__vector_size__(32), __aligned__(1)));

typedef float __v4sf __attribute__((__vector_size__(16)));
typedef long long __v2di __attribute__((__vector_size__(16)));
typedef unsigned long long __v2du __attribute__((__vector_size__(16)));
typedef int __v4si __attribute__((__vector_size__(16)));
typedef unsigned int __v4su __attribute__((__vector_size__(16)));
typedef short __v8hi __attribute__((__vector_size__(16)));
typedef unsigned short __v8hu __attribute__((__vector_size__(16)));
typedef char __v16qi __attribute__((__vector_size__(16)));
typedef signed char __v16qs __attribute__((__vector_size__(16)));
typedef unsigned char __v16qu __attribute__((__vector_size__(16)));
typedef float __v8sf __attribute__((__vector_size__(32)));
typedef unsigned int __v8su __attribute__((__vector_size__(32)));

// MAICORD_SIMD functions are constexpr in C++, which also folds constant
// operands where they initialize a constant; MAICORD_SIMD_RUNTIME ones (memory
// access and a few others) are not, matching clang's headers.
#if defined(__cplusplus)
#define MAICORD_SIMD_CONSTEXPR constexpr
#else
#define MAICORD_SIMD_CONSTEXPR
#endif
#define MAICORD_SIMD_RUNTIME(width)                                                                                    \
    static __inline__ __attribute__((__always_inline__, __nodebug__, __min_vector_width__(width)))
#define MAICORD_SIMD(width) MAICORD_SIMD_RUNTIME(width) MAICORD_SIMD_CONSTEXPR

#define _MM_SHUFFLE(z, y, x, w) (((z) << 6) | ((y) << 4) | ((x) << 2) | (w))

// Immediate operands: macros, as the builtins need constants.
#define _mm256_blend_ps(V1, V2, M)                                                                                     \
    ((__m256)__builtin_ia32_blendps256((__v8sf)(__m256)(V1), (__v8sf)(__m256)(V2), (int)(M)))
#define _mm256_permute2f128_ps(V1, V2, M)                                                                              \
    ((__m256)__builtin_ia32_vperm2f128_ps256((__v8sf)(__m256)(V1), (__v8sf)(__m256)(V2), (int)(M)))
#define _mm256_shuffle_ps(a, b, mask)                                                                                  \
    ((__m256)__builtin_ia32_shufps256((__v8sf)(__m256)(a), (__v8sf)(__m256)(b), (int)(mask)))
#define _mm_alignr_epi8(a, b, n)                                                                                       \
    ((__m128i)__builtin_ia32_palignr128((__v16qi)(__m128i)(a), (__v16qi)(__m128i)(b), (n)))
#define _mm_blend_epi16(a, b, m)                                                                                       \
    ((__m128i)__builtin_ia32_pblendw128((__v8hi)(__m128i)(a), (__v8hi)(__m128i)(b), (int)(m)))
#define _mm_clmulepi64_si128(a, b, i)                                                                                  \
    ((__m128i)__builtin_ia32_pclmulqdq128((__v2di)(__m128i)(a), (__v2di)(__m128i)(b), (char)(i)))
#define _mm_extract_epi16(a, i) ((int)(unsigned short)__builtin_ia32_vec_ext_v8hi((__v8hi)(__m128i)(a), (int)(i)))
#define _mm_insert_epi16(a, b, i) ((__m128i)__builtin_ia32_vec_set_v8hi((__v8hi)(__m128i)(a), (int)(b), (int)(i)))
#define _mm_shuffle_epi32(a, i) ((__m128i)__builtin_ia32_pshufd((__v4si)(__m128i)(a), (int)(i)))
#define _mm_shufflelo_epi16(a, i) ((__m128i)__builtin_ia32_pshuflw((__v8hi)(__m128i)(a), (int)(i)))
#define _mm_shuffle_ps(a, b, m) ((__m128)__builtin_ia32_shufps((__v4sf)(__m128)(a), (__v4sf)(__m128)(b), (int)(m)))
#define _mm_slli_si128(a, i) ((__m128i)__builtin_ia32_pslldqi128_byteshift((__v16qi)(__m128i)(a), (int)(i)))
#define _mm_srli_si128(a, i) ((__m128i)__builtin_ia32_psrldqi128_byteshift((__v16qi)(__m128i)(a), (int)(i)))
#define _mm_aeskeygenassist_si128(a, i) ((__m128i)__builtin_ia32_aeskeygenassist128((__v2di)(__m128i)(a), (char)(i)))

MAICORD_SIMD_RUNTIME(128) __m128i _mm_aesenc_si128(__m128i __V, __m128i __R) {
    return (__m128i)__builtin_ia32_aesenc128((__v2di)__V, (__v2di)__R);
}
MAICORD_SIMD_RUNTIME(128) __m128i _mm_aesenclast_si128(__m128i __V, __m128i __R) {
    return (__m128i)__builtin_ia32_aesenclast128((__v2di)__V, (__v2di)__R);
}

// Construction.
MAICORD_SIMD(128) __m128i _mm_setzero_si128(void) { return __extension__(__m128i)(__v2di){0, 0}; }
MAICORD_SIMD(128) __m128 _mm_setzero_ps(void) { return __extension__(__m128){0.0f, 0.0f, 0.0f, 0.0f}; }
MAICORD_SIMD(128) __m128d _mm_setzero_pd(void) { return __extension__(__m128d){0.0, 0.0}; }
MAICORD_SIMD(128) __m128 _mm_set_ss(float __w) { return __extension__(__m128){__w, 0.0f, 0.0f, 0.0f}; }
MAICORD_SIMD(128) __m128d _mm_set_sd(double __w) { return __extension__(__m128d){__w, 0.0}; }
MAICORD_SIMD(128) __m128 _mm_set1_ps(float __w) { return __extension__(__m128){__w, __w, __w, __w}; }
MAICORD_SIMD(128) __m128i _mm_set1_epi8(char __b) {
    return __extension__(__m128i)(__v16qi){__b, __b, __b, __b, __b, __b, __b, __b,
                                           __b, __b, __b, __b, __b, __b, __b, __b};
}
MAICORD_SIMD(128) __m128i _mm_set1_epi16(short __w) {
    return __extension__(__m128i)(__v8hi){__w, __w, __w, __w, __w, __w, __w, __w};
}
MAICORD_SIMD(128) __m128i _mm_set1_epi32(int __i) { return __extension__(__m128i)(__v4si){__i, __i, __i, __i}; }
MAICORD_SIMD(128) __m128i _mm_set_epi32(int __i3, int __i2, int __i1, int __i0) {
    return __extension__(__m128i)(__v4si){__i0, __i1, __i2, __i3};
}
MAICORD_SIMD(128) __m128i _mm_set_epi8(char __b15, char __b14, char __b13, char __b12, char __b11, char __b10,
                                       char __b9, char __b8, char __b7, char __b6, char __b5, char __b4, char __b3,
                                       char __b2, char __b1, char __b0) {
    return __extension__(__m128i)(__v16qi){__b0, __b1, __b2,  __b3,  __b4,  __b5,  __b6,  __b7,
                                           __b8, __b9, __b10, __b11, __b12, __b13, __b14, __b15};
}
MAICORD_SIMD(128) __m128i _mm_setr_epi8(char __b0, char __b1, char __b2, char __b3, char __b4, char __b5, char __b6,
                                        char __b7, char __b8, char __b9, char __b10, char __b11, char __b12,
                                        char __b13, char __b14, char __b15) {
    return _mm_set_epi8(__b15, __b14, __b13, __b12, __b11, __b10, __b9, __b8, __b7, __b6, __b5, __b4, __b3, __b2,
                        __b1, __b0);
}
MAICORD_SIMD(128) __m128i _mm_cvtsi32_si128(int __a) { return __extension__(__m128i)(__v4si){__a, 0, 0, 0}; }
MAICORD_SIMD(128) __m128i _mm_cvtsi64_si128(long long __a) { return __extension__(__m128i)(__v2di){__a, 0}; }

// Loads and stores: unaligned and may-alias, as through memcpy.
MAICORD_SIMD_RUNTIME(128) __m128i _mm_loadu_si128(__m128i_u const* __p) {
    struct __loadu_si128 { __m128i_u __v; } __attribute__((__packed__, __may_alias__));
    return ((const struct __loadu_si128*)__p)->__v;
}
MAICORD_SIMD_RUNTIME(128) __m128i _mm_loadl_epi64(__m128i_u const* __p) {
    struct __loadl_epi64 { long long __u; } __attribute__((__packed__, __may_alias__));
    return __extension__(__m128i){((const struct __loadl_epi64*)__p)->__u, 0};
}
MAICORD_SIMD_RUNTIME(128) __m128i _mm_loadu_si32(void const* __p) {
    struct __loadu_si32 { int __v; } __attribute__((__packed__, __may_alias__));
    return __extension__(__m128i)(__v4si){((const struct __loadu_si32*)__p)->__v, 0, 0, 0};
}
MAICORD_SIMD_RUNTIME(128) __m128 _mm_loadu_ps(const float* __p) {
    struct __loadu_ps { __m128_u __v; } __attribute__((__packed__, __may_alias__));
    return ((const struct __loadu_ps*)__p)->__v;
}
MAICORD_SIMD_RUNTIME(128) __m128 _mm_load_ss(const float* __p) {
    struct __load_ss { float __u; } __attribute__((__packed__, __may_alias__));
    return __extension__(__m128){((const struct __load_ss*)__p)->__u, 0, 0, 0};
}
MAICORD_SIMD_RUNTIME(128) void _mm_storeu_si128(__m128i_u* __p, __m128i __a) {
    struct __storeu_si128 { __m128i_u __v; } __attribute__((__packed__, __may_alias__));
    ((struct __storeu_si128*)__p)->__v = __a;
}
MAICORD_SIMD_RUNTIME(128) void _mm_storel_epi64(__m128i_u* __p, __m128i __a) {
    struct __storel_epi64 { long long __u; } __attribute__((__packed__, __may_alias__));
    ((struct __storel_epi64*)__p)->__u = __a[0];
}
MAICORD_SIMD_RUNTIME(128) void _mm_storeu_si32(void* __p, __m128i __a) {
    struct __storeu_si32 { int __v; } __attribute__((__packed__, __may_alias__));
    ((struct __storeu_si32*)__p)->__v = ((__v4si)__a)[0];
}
MAICORD_SIMD_RUNTIME(128) void _mm_storeu_ps(float* __p, __m128 __a) {
    struct __storeu_ps { __m128_u __v; } __attribute__((__packed__, __may_alias__));
    ((struct __storeu_ps*)__p)->__v = __a;
}
MAICORD_SIMD_RUNTIME(128) void _mm_store_ss(float* __p, __m128 __a) {
    struct __store_ss { float __u; } __attribute__((__packed__, __may_alias__));
    ((struct __store_ss*)__p)->__u = __a[0];
}
MAICORD_SIMD_RUNTIME(128) void _mm_storel_pi(__m64* __p, __m128 __a) {
    typedef float __v2sf __attribute__((__vector_size__(8)));
    struct __storel_pi { __v2sf __u; } __attribute__((__packed__, __may_alias__));
    ((struct __storel_pi*)__p)->__u = __builtin_shufflevector(__a, __a, 0, 1);
}

// Element access and conversion.
MAICORD_SIMD(128) float _mm_cvtss_f32(__m128 __a) { return __a[0]; }
MAICORD_SIMD(128) double _mm_cvtsd_f64(__m128d __a) { return __a[0]; }
MAICORD_SIMD(128) int _mm_cvtsi128_si32(__m128i __a) { return ((__v4si)__a)[0]; }
MAICORD_SIMD(128) long long _mm_cvtsi128_si64(__m128i __a) { return __a[0]; }
MAICORD_SIMD(128) int _mm_cvtss_si32(__m128 __a) { return __builtin_ia32_cvtss2si((__v4sf)__a); }
MAICORD_SIMD(128) __m128i _mm_cvtps_epi32(__m128 __a) { return (__m128i)__builtin_ia32_cvtps2dq((__v4sf)__a); }
MAICORD_SIMD(128) __m128 _mm_cvtepi32_ps(__m128i __a) { return (__m128)__builtin_convertvector((__v4si)__a, __v4sf); }
MAICORD_SIMD(128) __m128 _mm_castsi128_ps(__m128i __a) { return (__m128)__a; }
MAICORD_SIMD(128) __m128i _mm_castps_si128(__m128 __a) { return (__m128i)__a; }
MAICORD_SIMD(128) __m128i _mm_cvtepi8_epi32(__m128i __v) {
    return (__m128i)__builtin_convertvector(__builtin_shufflevector((__v16qs)__v, (__v16qs)__v, 0, 1, 2, 3), __v4si);
}
MAICORD_SIMD(128) __m128i _mm_cvtepi16_epi32(__m128i __v) {
    return (__m128i)__builtin_convertvector(__builtin_shufflevector((__v8hi)__v, (__v8hi)__v, 0, 1, 2, 3), __v4si);
}
MAICORD_SIMD(128) __m128i _mm_cvtepu8_epi16(__m128i __v) {
    return (__m128i)__builtin_convertvector(
        __builtin_shufflevector((__v16qu)__v, (__v16qu)__v, 0, 1, 2, 3, 4, 5, 6, 7), __v8hi);
}
MAICORD_SIMD(128) __m128i _mm_cvtepu8_epi32(__m128i __v) {
    return (__m128i)__builtin_convertvector(__builtin_shufflevector((__v16qu)__v, (__v16qu)__v, 0, 1, 2, 3), __v4si);
}
MAICORD_SIMD(128) __m128i _mm_cvttps_epi32(__m128 __a) { return (__m128i)__builtin_ia32_cvttps2dq((__v4sf)__a); }

// Single and double precision.
MAICORD_SIMD(128) __m128 _mm_add_ss(__m128 __a, __m128 __b) { __a[0] += __b[0]; return __a; }
MAICORD_SIMD(128) __m128 _mm_sub_ss(__m128 __a, __m128 __b) { __a[0] -= __b[0]; return __a; }
MAICORD_SIMD(128) __m128 _mm_mul_ss(__m128 __a, __m128 __b) { __a[0] *= __b[0]; return __a; }
MAICORD_SIMD(128) __m128 _mm_div_ss(__m128 __a, __m128 __b) { __a[0] /= __b[0]; return __a; }
MAICORD_SIMD_RUNTIME(128) __m128 _mm_cmpeq_ss(__m128 __a, __m128 __b) {
    return (__m128)__builtin_ia32_cmpeqss((__v4sf)__a, (__v4sf)__b);
}
MAICORD_SIMD_RUNTIME(128) __m128d _mm_sqrt_sd(__m128d __a, __m128d __b) {
    return __extension__(__m128d){__builtin_elementwise_sqrt(__b[0]), __a[1]};
}
MAICORD_SIMD(128) __m128 _mm_add_ps(__m128 __a, __m128 __b) { return (__m128)((__v4sf)__a + (__v4sf)__b); }
MAICORD_SIMD(128) __m128 _mm_sub_ps(__m128 __a, __m128 __b) { return (__m128)((__v4sf)__a - (__v4sf)__b); }
MAICORD_SIMD(128) __m128 _mm_mul_ps(__m128 __a, __m128 __b) { return (__m128)((__v4sf)__a * (__v4sf)__b); }
MAICORD_SIMD(128) __m128 _mm_div_ps(__m128 __a, __m128 __b) { return (__m128)((__v4sf)__a / (__v4sf)__b); }
MAICORD_SIMD(128) __m128 _mm_min_ps(__m128 __a, __m128 __b) { return __builtin_ia32_minps((__v4sf)__a, (__v4sf)__b); }
MAICORD_SIMD(128) __m128 _mm_max_ps(__m128 __a, __m128 __b) { return __builtin_ia32_maxps((__v4sf)__a, (__v4sf)__b); }
MAICORD_SIMD(128) __m128 _mm_andnot_ps(__m128 __a, __m128 __b) { return (__m128)(~(__v4su)__a & (__v4su)__b); }
MAICORD_SIMD(128) __m128 _mm_xor_ps(__m128 __a, __m128 __b) { return (__m128)((__v4su)__a ^ (__v4su)__b); }

// Integer arithmetic (wrapping), logic and comparison.
MAICORD_SIMD(128) __m128i _mm_add_epi8(__m128i __a, __m128i __b) { return (__m128i)((__v16qu)__a + (__v16qu)__b); }
MAICORD_SIMD(128) __m128i _mm_add_epi16(__m128i __a, __m128i __b) { return (__m128i)((__v8hu)__a + (__v8hu)__b); }
MAICORD_SIMD(128) __m128i _mm_add_epi32(__m128i __a, __m128i __b) { return (__m128i)((__v4su)__a + (__v4su)__b); }
MAICORD_SIMD(128) __m128i _mm_add_epi64(__m128i __a, __m128i __b) { return (__m128i)((__v2du)__a + (__v2du)__b); }
MAICORD_SIMD(128) __m128i _mm_sub_epi8(__m128i __a, __m128i __b) { return (__m128i)((__v16qu)__a - (__v16qu)__b); }
MAICORD_SIMD(128) __m128i _mm_sub_epi16(__m128i __a, __m128i __b) { return (__m128i)((__v8hu)__a - (__v8hu)__b); }
MAICORD_SIMD(128) __m128i _mm_sub_epi32(__m128i __a, __m128i __b) { return (__m128i)((__v4su)__a - (__v4su)__b); }
MAICORD_SIMD(128) __m128i _mm_mullo_epi16(__m128i __a, __m128i __b) { return (__m128i)((__v8hu)__a * (__v8hu)__b); }
MAICORD_SIMD(128) __m128i _mm_mulhi_epu16(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_ia32_pmulhuw128((__v8hu)__a, (__v8hu)__b);
}
MAICORD_SIMD(128) __m128i _mm_adds_epu16(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_elementwise_add_sat((__v8hu)__a, (__v8hu)__b);
}
MAICORD_SIMD(128) __m128i _mm_subs_epu16(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_elementwise_sub_sat((__v8hu)__a, (__v8hu)__b);
}
MAICORD_SIMD(128) __m128i _mm_andnot_si128(__m128i __a, __m128i __b) { return (__m128i)(~(__v2du)__a & (__v2du)__b); }
MAICORD_SIMD(128) __m128i _mm_mullo_epi32(__m128i __a, __m128i __b) { return (__m128i)((__v4su)__a * (__v4su)__b); }
MAICORD_SIMD(128) __m128i _mm_adds_epi16(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_elementwise_add_sat((__v8hi)__a, (__v8hi)__b);
}
MAICORD_SIMD(128) __m128i _mm_min_epi16(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_elementwise_min((__v8hi)__a, (__v8hi)__b);
}
MAICORD_SIMD(128) __m128i _mm_max_epi16(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_elementwise_max((__v8hi)__a, (__v8hi)__b);
}
MAICORD_SIMD(128) __m128i _mm_hadd_epi32(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_ia32_phaddd128((__v4si)__a, (__v4si)__b);
}
MAICORD_SIMD(128) __m128i _mm_mul_epi32(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_ia32_pmuldq128((__v4si)__a, (__v4si)__b);
}
MAICORD_SIMD(128) __m128i _mm_mulhi_epi16(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_ia32_pmulhw128((__v8hi)__a, (__v8hi)__b);
}
MAICORD_SIMD(128) __m128i _mm_madd_epi16(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_ia32_pmaddwd128((__v8hi)__a, (__v8hi)__b);
}
MAICORD_SIMD(128) __m128i _mm_maddubs_epi16(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_ia32_pmaddubsw128((__v16qi)__a, (__v16qi)__b);
}
MAICORD_SIMD(128) __m128i _mm_sad_epu8(__m128i __a, __m128i __b) {
    return __builtin_ia32_psadbw128((__v16qu)__a, (__v16qu)__b);
}
MAICORD_SIMD(128) __m128i _mm_avg_epu8(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_ia32_pavgb128((__v16qu)__a, (__v16qu)__b);
}
MAICORD_SIMD(128) __m128i _mm_max_epu8(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_elementwise_max((__v16qu)__a, (__v16qu)__b);
}
MAICORD_SIMD(128) __m128i _mm_min_epi32(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_elementwise_min((__v4si)__a, (__v4si)__b);
}
MAICORD_SIMD(128) __m128i _mm_max_epi32(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_elementwise_max((__v4si)__a, (__v4si)__b);
}
MAICORD_SIMD(128) __m128i _mm_abs_epi16(__m128i __a) { return (__m128i)__builtin_elementwise_abs((__v8hi)__a); }
MAICORD_SIMD(128) __m128i _mm_abs_epi32(__m128i __a) { return (__m128i)__builtin_elementwise_abs((__v4si)__a); }
MAICORD_SIMD(128) __m128i _mm_and_si128(__m128i __a, __m128i __b) { return (__m128i)((__v2du)__a & (__v2du)__b); }
MAICORD_SIMD(128) __m128i _mm_or_si128(__m128i __a, __m128i __b) { return (__m128i)((__v2du)__a | (__v2du)__b); }
MAICORD_SIMD(128) __m128i _mm_xor_si128(__m128i __a, __m128i __b) { return (__m128i)((__v2du)__a ^ (__v2du)__b); }
MAICORD_SIMD(128) __m128i _mm_cmpeq_epi8(__m128i __a, __m128i __b) { return (__m128i)((__v16qi)__a == (__v16qi)__b); }
MAICORD_SIMD(128) __m128i _mm_cmpgt_epi8(__m128i __a, __m128i __b) { return (__m128i)((__v16qs)__a > (__v16qs)__b); }
MAICORD_SIMD(128) __m128i _mm_cmplt_epi8(__m128i __a, __m128i __b) { return (__m128i)((__v16qs)__b > (__v16qs)__a); }
MAICORD_SIMD(128) __m128i _mm_cmpgt_epi16(__m128i __a, __m128i __b) { return (__m128i)((__v8hi)__a > (__v8hi)__b); }
MAICORD_SIMD(128) __m128i _mm_blendv_epi8(__m128i __a, __m128i __b, __m128i __m) {
    return (__m128i)__builtin_ia32_pblendvb128((__v16qi)__a, (__v16qi)__b, (__v16qi)__m);
}
MAICORD_SIMD(128) int _mm_movemask_epi8(__m128i __a) { return __builtin_ia32_pmovmskb128((__v16qi)__a); }

// Shifts by a count.
MAICORD_SIMD(128) __m128i _mm_slli_epi16(__m128i __a, int __n) {
    return (__m128i)__builtin_ia32_psllwi128((__v8hi)__a, __n);
}
MAICORD_SIMD(128) __m128i _mm_slli_epi32(__m128i __a, int __n) {
    return (__m128i)__builtin_ia32_pslldi128((__v4si)__a, __n);
}
MAICORD_SIMD(128) __m128i _mm_slli_epi64(__m128i __a, int __n) { return __builtin_ia32_psllqi128((__v2di)__a, __n); }
MAICORD_SIMD(128) __m128i _mm_srli_epi16(__m128i __a, int __n) {
    return (__m128i)__builtin_ia32_psrlwi128((__v8hi)__a, __n);
}
MAICORD_SIMD(128) __m128i _mm_srli_epi32(__m128i __a, int __n) {
    return (__m128i)__builtin_ia32_psrldi128((__v4si)__a, __n);
}
MAICORD_SIMD(128) __m128i _mm_srli_epi64(__m128i __a, int __n) { return __builtin_ia32_psrlqi128((__v2di)__a, __n); }
MAICORD_SIMD(128) __m128i _mm_srai_epi16(__m128i __a, int __n) {
    return (__m128i)__builtin_ia32_psrawi128((__v8hi)__a, __n);
}
MAICORD_SIMD(128) __m128i _mm_srai_epi32(__m128i __a, int __n) {
    return (__m128i)__builtin_ia32_psradi128((__v4si)__a, __n);
}
MAICORD_SIMD(128) __m128i _mm_sll_epi32(__m128i __a, __m128i __count) {
    return (__m128i)__builtin_ia32_pslld128((__v4si)__a, (__v4si)__count);
}
MAICORD_SIMD(128) __m128i _mm_sra_epi32(__m128i __a, __m128i __count) {
    return (__m128i)__builtin_ia32_psrad128((__v4si)__a, (__v4si)__count);
}

// Packing, shuffles and interleaving.
MAICORD_SIMD(128) __m128i _mm_packs_epi32(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_ia32_packssdw128((__v4si)__a, (__v4si)__b);
}
MAICORD_SIMD(128) __m128i _mm_packus_epi16(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_ia32_packuswb128((__v8hi)__a, (__v8hi)__b);
}
MAICORD_SIMD(128) __m128i _mm_shuffle_epi8(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_ia32_pshufb128((__v16qi)__a, (__v16qi)__b);
}
MAICORD_SIMD(128) __m128i _mm_unpacklo_epi8(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_shufflevector((__v16qi)__a, (__v16qi)__b, 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6,
                                            22, 7, 23);
}
MAICORD_SIMD(128) __m128i _mm_unpackhi_epi8(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_shufflevector((__v16qi)__a, (__v16qi)__b, 8, 16 + 8, 9, 16 + 9, 10, 16 + 10, 11,
                                            16 + 11, 12, 16 + 12, 13, 16 + 13, 14, 16 + 14, 15, 16 + 15);
}
MAICORD_SIMD(128) __m128i _mm_unpacklo_epi16(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_shufflevector((__v8hi)__a, (__v8hi)__b, 0, 8, 1, 9, 2, 10, 3, 11);
}
MAICORD_SIMD(128) __m128i _mm_unpackhi_epi16(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_shufflevector((__v8hi)__a, (__v8hi)__b, 4, 12, 5, 13, 6, 14, 7, 15);
}
MAICORD_SIMD(128) __m128i _mm_unpacklo_epi32(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_shufflevector((__v4si)__a, (__v4si)__b, 0, 4, 1, 5);
}
MAICORD_SIMD(128) __m128i _mm_unpackhi_epi32(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_shufflevector((__v4si)__a, (__v4si)__b, 2, 6, 3, 7);
}
MAICORD_SIMD(128) __m128i _mm_unpacklo_epi64(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_shufflevector((__v2di)__a, (__v2di)__b, 0, 2);
}
MAICORD_SIMD(128) __m128i _mm_unpackhi_epi64(__m128i __a, __m128i __b) {
    return (__m128i)__builtin_shufflevector((__v2di)__a, (__v2di)__b, 1, 3);
}

// 256-bit single precision.
MAICORD_SIMD(256) __m256 _mm256_setzero_ps(void) {
    return __extension__(__m256){0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
}
MAICORD_SIMD(256) __m256 _mm256_set1_ps(float __w) {
    return __extension__(__m256){__w, __w, __w, __w, __w, __w, __w, __w};
}
MAICORD_SIMD_RUNTIME(256) __m256 _mm256_load_ps(float const* __p) { return *(const __m256*)__p; }
MAICORD_SIMD_RUNTIME(256) __m256 _mm256_loadu_ps(float const* __p) {
    struct __loadu_ps { __m256_u __v; } __attribute__((__packed__, __may_alias__));
    return ((const struct __loadu_ps*)__p)->__v;
}
MAICORD_SIMD_RUNTIME(256) void _mm256_store_ps(float* __p, __m256 __a) { *(__m256*)__p = __a; }
MAICORD_SIMD_RUNTIME(256) void _mm256_storeu_ps(float* __p, __m256 __a) {
    struct __storeu_ps { __m256_u __v; } __attribute__((__packed__, __may_alias__));
    ((struct __storeu_ps*)__p)->__v = __a;
}
MAICORD_SIMD(256) __m256 _mm256_add_ps(__m256 __a, __m256 __b) { return (__m256)((__v8sf)__a + (__v8sf)__b); }
MAICORD_SIMD(256) __m256 _mm256_sub_ps(__m256 __a, __m256 __b) { return (__m256)((__v8sf)__a - (__v8sf)__b); }
MAICORD_SIMD(256) __m256 _mm256_mul_ps(__m256 __a, __m256 __b) { return (__m256)((__v8sf)__a * (__v8sf)__b); }
MAICORD_SIMD(256) __m256 _mm256_xor_ps(__m256 __a, __m256 __b) { return (__m256)((__v8su)__a ^ (__v8su)__b); }
MAICORD_SIMD(256) __m256 _mm256_blendv_ps(__m256 __a, __m256 __b, __m256 __c) {
    return (__m256)__builtin_ia32_blendvps256((__v8sf)__a, (__v8sf)__b, (__v8sf)__c);
}
MAICORD_SIMD(256) __m256 _mm256_unpacklo_ps(__m256 __a, __m256 __b) {
    return __builtin_shufflevector((__v8sf)__a, (__v8sf)__b, 0, 8, 0 + 1, 8 + 1, 4, 12, 4 + 1, 12 + 1);
}
MAICORD_SIMD(256) __m256 _mm256_unpackhi_ps(__m256 __a, __m256 __b) {
    return __builtin_shufflevector((__v8sf)__a, (__v8sf)__b, 2, 10, 2 + 1, 10 + 1, 6, 14, 6 + 1, 14 + 1);
}

#undef MAICORD_SIMD
#undef MAICORD_SIMD_RUNTIME
#undef MAICORD_SIMD_CONSTEXPR

#endif
