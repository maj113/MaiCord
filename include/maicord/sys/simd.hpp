#pragma once

// sys/simd, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <immintrin.h>
#include <wmmintrin.h>
#endif

// <xmmintrin.h>
#define _MM_SHUFFLE(fp3,fp2,fp1,fp0) (((fp3) << 6) | ((fp2) << 4) | ((fp1) << 2) | ((fp0)))
#define _mm_set1_ps _mm_set_ps1

// <immintrin.h>
#define _mm_loadu_si32(p) _mm_cvtsi32_si128(*(unsigned int const*)(p))
#define _mm_storeu_si32(p, a) (void)(*(int*)(p) = _mm_cvtsi128_si32((a)))

MAICORD_SYS_TYPES_BEGIN
struct __m128d;
union __m128;
union __m128i;
union __m256;
union __m256i;
union __m64;

// <mmintrin.h>
union __declspec(intrin_type) __declspec(align(8)) __m64 {
    unsigned __int64 m64_u64;
    float m64_f32[2];
    __int8 m64_i8[8];
    __int16 m64_i16[4];
    __int32 m64_i32[2];
    __int64 m64_i64;
    unsigned __int8 m64_u8[8];
    unsigned __int16 m64_u16[4];
    unsigned __int32 m64_u32[2];
};
// <xmmintrin.h>
union __declspec(intrin_type) __declspec(align(16)) __m128 {
    float m128_f32[4];
    unsigned __int64 m128_u64[2];
    __int8 m128_i8[16];
    __int16 m128_i16[8];
    __int32 m128_i32[4];
    __int64 m128_i64[2];
    unsigned __int8 m128_u8[16];
    unsigned __int16 m128_u16[8];
    unsigned __int32 m128_u32[4];
};
// <emmintrin.h>
union __declspec(intrin_type) __declspec(align(16)) __m128i {
    __int8 m128i_i8[16];
    __int16 m128i_i16[8];
    __int32 m128i_i32[4];
    __int64 m128i_i64[2];
    unsigned __int8 m128i_u8[16];
    unsigned __int16 m128i_u16[8];
    unsigned __int32 m128i_u32[4];
    unsigned __int64 m128i_u64[2];
};
struct __declspec(intrin_type) __declspec(align(16)) __m128d {
    double m128d_f64[2];
};
// <immintrin.h>
union __declspec(intrin_type) __declspec(align(32)) __m256 {
    float m256_f32[8];
};
union __declspec(intrin_type) __declspec(align(32)) __m256i {
    __int8 m256i_i8[32];
    __int16 m256i_i16[16];
    __int32 m256i_i32[8];
    __int64 m256i_i64[4];
    unsigned __int8 m256i_u8[32];
    unsigned __int16 m256i_u16[16];
    unsigned __int32 m256i_u32[8];
    unsigned __int64 m256i_u64[4];
};
MAICORD_SYS_TYPES_END

// <xmmintrin.h>
extern "C" extern __m128 _mm_add_ss(__m128 _A, __m128 _B);
extern "C" extern __m128 _mm_add_ps(__m128 _A, __m128 _B);
extern "C" extern __m128 _mm_sub_ss(__m128 _A, __m128 _B);
extern "C" extern __m128 _mm_sub_ps(__m128 _A, __m128 _B);
extern "C" extern __m128 _mm_mul_ss(__m128 _A, __m128 _B);
extern "C" extern __m128 _mm_mul_ps(__m128 _A, __m128 _B);
extern "C" extern __m128 _mm_div_ss(__m128 _A, __m128 _B);
extern "C" extern __m128 _mm_min_ps(__m128 _A, __m128 _B);
extern "C" extern __m128 _mm_max_ps(__m128 _A, __m128 _B);
extern "C" extern __m128 _mm_andnot_ps(__m128 _A, __m128 _B);
extern "C" extern __m128 _mm_xor_ps(__m128 _A, __m128 _B);
extern "C" extern __m128 _mm_cmpeq_ss(__m128 _A, __m128 _B);
extern "C" extern float _mm_cvtss_f32(__m128 _A);
extern "C" extern __m128 _mm_shuffle_ps(__m128 _A, __m128 _B, unsigned int _Imm8);
extern "C" extern void _mm_storel_pi(__m64*, __m128);
extern "C" extern __m128 _mm_set_ss(float _A);
extern "C" extern __m128 _mm_set_ps1(float _A);
extern "C" extern __m128 _mm_setzero_ps();
extern "C" extern __m128 _mm_loadu_ps(float const* _A);
extern "C" extern void _mm_store_ss(float* _V, __m128 _A);
extern "C" extern void _mm_storeu_ps(float* _V, __m128 _A);

// <emmintrin.h>
extern "C" extern __m128d _mm_sqrt_sd(__m128d _A, __m128d _B);
extern "C" extern __m128i _mm_cvtps_epi32(__m128 _A);
extern "C" extern __m128d _mm_set_sd(double _W);
extern "C" extern __m128d _mm_setzero_pd();
extern "C" extern __m128i _mm_add_epi8(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_add_epi16(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_add_epi32(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_avg_epu8(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_madd_epi16(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_max_epu8(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_sad_epu8(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_sub_epi8(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_sub_epi16(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_and_si128(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_or_si128(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_xor_si128(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_slli_si128(__m128i _A, int _Imm);
extern "C" extern __m128i _mm_slli_epi16(__m128i _A, int _Count);
extern "C" extern __m128i _mm_slli_epi32(__m128i _A, int _Count);
extern "C" extern __m128i _mm_slli_epi64(__m128i _A, int _Count);
extern "C" extern __m128i _mm_srai_epi16(__m128i _A, int _Count);
extern "C" extern __m128i _mm_srai_epi32(__m128i _A, int _Count);
extern "C" extern __m128i _mm_srli_si128(__m128i _A, int _Imm);
extern "C" extern __m128i _mm_srli_epi16(__m128i _A, int _Count);
extern "C" extern __m128i _mm_srli_epi32(__m128i _A, int _Count);
extern "C" extern __m128i _mm_srli_epi64(__m128i _A, int _Count);
extern "C" extern __m128i _mm_cmpeq_epi8(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_cmpgt_epi8(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_cmpgt_epi16(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_cmplt_epi8(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_cvtsi32_si128(int _A);
extern "C" extern int _mm_cvtsi128_si32(__m128i _A);
extern "C" extern __m128i _mm_packs_epi32(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_packus_epi16(__m128i _A, __m128i _B);
extern "C" extern int _mm_extract_epi16(__m128i _A, int _Imm);
extern "C" extern int _mm_movemask_epi8(__m128i _A);
extern "C" extern __m128i _mm_shuffle_epi32(__m128i _A, int _Imm);
extern "C" extern __m128i _mm_unpackhi_epi32(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_unpackhi_epi64(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_unpacklo_epi8(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_unpacklo_epi16(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_unpacklo_epi32(__m128i _A, __m128i _B);
extern "C" extern __m128i _mm_loadu_si128(__m128i const* _P);
extern "C" extern __m128i _mm_loadl_epi64(__m128i const* _P);
extern "C" extern __m128i _mm_set_epi32(int _I3, int _I2, int _I1, int _I0);
extern "C" extern __m128i _mm_set1_epi32(int _I);
extern "C" extern __m128i _mm_set1_epi16(short _W);
extern "C" extern __m128i _mm_set1_epi8(char _B);
extern "C" extern __m128i _mm_setr_epi8(char _B15, char _B14, char _B13, char _B12, char _B11, char _B10, char _B9,
    char _B8, char _B7, char _B6, char _B5, char _B4, char _B3, char _B2, char _B1, char _B0);
extern "C" extern __m128i _mm_setzero_si128();
extern "C" extern void _mm_storeu_si128(__m128i* _P, __m128i _B);
extern "C" extern void _mm_storel_epi64(__m128i* _P, __m128i _Q);
extern "C" extern double _mm_cvtsd_f64(__m128d _A);
extern "C" extern __m128 _mm_castsi128_ps(__m128i);
extern "C" extern __m128i _mm_cvtsi64_si128(__int64);
extern "C" extern __int64 _mm_cvtsi128_si64(__m128i);

// <tmmintrin.h>
extern "C" extern __m128i _mm_maddubs_epi16(__m128i, __m128i);
extern "C" extern __m128i _mm_shuffle_epi8(__m128i, __m128i);
extern "C" extern __m128i _mm_abs_epi16(__m128i);

// <smmintrin.h>
extern "C" extern __m128i _mm_blendv_epi8(__m128i, __m128i, __m128i mask);
extern "C" extern __m128i _mm_mullo_epi32(__m128i, __m128i);
extern "C" extern __m128i _mm_cvtepu8_epi16(__m128i);
extern "C" extern __m128i _mm_packus_epi32(__m128i, __m128i);

// <wmmintrin.h>
extern "C" extern __m128i _mm_clmulepi64_si128(__m128i, __m128i, const int);

// <immintrin.h>
extern "C" extern __m256 __cdecl _mm256_add_ps(__m256, __m256);
extern "C" extern __m256 __cdecl _mm256_mul_ps(__m256, __m256);
extern "C" extern __m256 __cdecl _mm256_load_ps(float const*);
extern "C" extern void __cdecl _mm256_store_ps(float*, __m256);
extern "C" extern __m256 __cdecl _mm256_setzero_ps();
extern "C" extern __m256i __cdecl _mm256_setzero_si256();
extern "C" extern __m256 __cdecl _mm256_set1_ps(float);
extern "C" extern __m256i __cdecl _mm256_set1_epi32(int);
extern "C" extern __m128i __cdecl _mm256_castsi256_si128(__m256i);
extern "C" extern __m256i __cdecl _mm256_max_epi32(__m256i, __m256i);
extern "C" extern __m256i __cdecl _mm256_min_epi32(__m256i, __m256i);
extern "C" extern __m256i __cdecl _mm256_add_epi32(__m256i, __m256i);
extern "C" extern __m256i __cdecl _mm256_sub_epi32(__m256i, __m256i);
extern "C" extern __m256i __cdecl _mm256_mullo_epi32(__m256i, __m256i);
extern "C" extern __m256i __cdecl _mm256_srai_epi32(__m256i, int);
extern "C" extern __m128i __cdecl _mm256_extracti128_si256(__m256i, const int);
extern "C" extern __m256i __cdecl _mm256_cvtepu8_epi32(__m128i);
