/* Copyright (c) 2003-2008 Jean-Marc Valin
   Copyright (c) 2007-2008 CSIRO
   Copyright (c) 2007-2009 Xiph.Org Foundation
   Written by Jean-Marc Valin */
/**
   @file arch.h
   @brief Various architecture definitions for CELT
*/
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef ARCH_H
#define ARCH_H

#include "opus_types.h"
#include "opus_defines.h"
#include <intrin.h>

# if !defined(__GNUC_PREREQ)
#  if defined(__GNUC__)&&defined(__GNUC_MINOR__)
#   define __GNUC_PREREQ(_maj,_min) \
 ((__GNUC__<<16)+__GNUC_MINOR__>=((_maj)<<16)+(_min))
#  else
#   define __GNUC_PREREQ(_maj,_min) 0
#  endif
# endif

#if OPUS_GNUC_PREREQ(3, 0)
#define opus_likely(x)       (__builtin_expect(!!(x), 1))
#define opus_unlikely(x)     (__builtin_expect(!!(x), 0))
#else
#define opus_likely(x)       (!!(x))
#define opus_unlikely(x)     (!!(x))
#endif

#define CELT_SIG_SCALE 32768.f

/* Runtime checks. celt_packet_check() guards a decoder value derived from
   packet bits that bounds a memory access or the range decoder's state; a
   failure ends the process at once: __fastfail(FAST_FAIL_FATAL_APP_EXIT) is an
   inline int 0x29 that no exception or crash handler can intercept, so a check
   costs a compare and a never-taken branch, with no call. The other upstream
   assertions (encoder, caller and configuration input, decoder state, algorithm
   invariants) compile to nothing; MUST_SUCCEED still makes its call. */
#define celt_packet_check(cond) {if (!(cond)) __fastfail(7);}
#define celt_assert(cond) ((void)(cond))
#define celt_assert2(cond, message) ((void)(cond))
#define MUST_SUCCEED(call) ((void)(call))

#define celt_sig_assert(cond) ((void)(cond))

#define IMUL32(a,b) ((a)*(b))

#define MIN16(a,b) ((a) < (b) ? (a) : (b))   /**< Minimum 16-bit value.   */
#define MAX16(a,b) ((a) > (b) ? (a) : (b))   /**< Maximum 16-bit value.   */
#define MIN32(a,b) ((a) < (b) ? (a) : (b))   /**< Minimum 32-bit value.   */
#define MAX32(a,b) ((a) > (b) ? (a) : (b))   /**< Maximum 32-bit value.   */
#define IMIN(a,b) ((a) < (b) ? (a) : (b))   /**< Minimum int value.   */
#define IMAX(a,b) ((a) > (b) ? (a) : (b))   /**< Maximum int value.   */
#define FMIN(a,b) ((a) < (b) ? (a) : (b))   /**< Minimum float value.   */
#define FMAX(a,b) ((a) > (b) ? (a) : (b))   /**< Maximum float value.   */
#define UADD32(a,b) ((a)+(b))
#define USUB32(a,b) ((a)-(b))
#define MAXG(a,b) MAX32(a, b)
#define MING(a,b) MIN32(a, b)

/* Throughout the code, we use the following scaling for signals:
   FLOAT: used for float API, normalized to +/-1.
   INT16: used for 16-bit API, normalized to +/- 32768
   RES: internal Opus resolution, defined as +/-1. in float builds, or either 16-bit or 24-bit int for fixed-point builds
   SIG: internal CELT resolution: defined as +/- 32768. in float builds, or Q27 in fixed-point builds (int16 shifted by 12)
*/


/* Set this if opus_int64 is a native type of the CPU. */
/* Assume that all LP64 architectures have fast 64-bit types; also x86_64
   (which can be ILP32 for x32) and Win64 (which is LLP64). */
#if defined(__x86_64__) || defined(__LP64__) || defined(_WIN64) || defined (__mips)
#define OPUS_FAST_INT64 1
#else
#define OPUS_FAST_INT64 0
#endif


#define PRINT_MIPS(file)


typedef opus_int16 opus_val16;
typedef opus_int32 opus_val32;
typedef opus_int64 opus_val64;

typedef opus_val32 celt_sig;
typedef opus_val32 celt_norm;
typedef opus_val32 celt_ener;
typedef opus_val32 celt_glog;

typedef opus_val16 opus_res;
#define RES_SHIFT 0
#define SIG2RES(a)      SIG2WORD16(a)
#define RES2INT16(a)    (a)
#define RES2INT24(a)    SHL32(EXTEND32(a), 8)
#define RES2FLOAT(a)    ((1.f/32768.f)*(a))
#define INT16TORES(a)   (a)
#define INT24TORES(a)   SAT16(PSHR32(a, 8))
#define ADD_RES(a, b)   SAT16(ADD32((a), (b)));
#define FLOAT2RES(a)    FLOAT2INT16(a)
#define RES2SIG(a)      SHL32(EXTEND32(a), SIG_SHIFT)
#define MULT16_RES_Q15(a,b) MULT16_16_Q15(a,b)
#define MAX_ENCODING_DEPTH 16

#define RES2VAL16(a)    RES2INT16(a)
#define INT16TOSIG(a)   SHL32(EXTEND32(a), SIG_SHIFT)
#define INT24TOSIG(a)   SHL32(a, SIG_SHIFT-8)

#define NORM_SHIFT 24
typedef opus_val16 celt_coef;
#define COEF_ONE Q15ONE
#define MULT_COEF_32(a, b) MULT16_32_Q15(a,b)
#define MAC_COEF_32_ARM(a, b, c) MAC16_32_Q16(a,b,c)
#define MULT_COEF(a, b) MULT16_16_Q15(a,b)
#define MULT_COEF_TAPS(a, b) MULT16_16_P15(a,b)
#define COEF2VAL16(x) (x)

#define celt_isnan(x) 0

#define Q15ONE 32767
#define Q31ONE 2147483647

#define SIG_SHIFT 12
/* Safe saturation value for 32-bit signals. We need to make sure that we can
   add two sig values and that the first stages of the MDCT don't cause an overflow.
   The most constraining is the ARM_ASM comb filter where we shift left by one
   and then add two values. Because of that, we use 2^29-1. SIG_SAT must be large
   enough to fit a full-scale high-freq tone through the prefilter and comb filter,
   meaning 1.85*1.75*2^(15+SIG_SHIFT) =  434529895.
   so the limit should be about 2^31*sqrt(.5). */
#define SIG_SAT (536870911)

#define NORM_SCALING (1<<NORM_SHIFT)

#define DB_SHIFT 24

#define EPSILON 1
#define VERY_SMALL 0
#define VERY_LARGE16 ((opus_val16)32767)
#define Q15_ONE ((opus_val16)32767)


#define ABS16(x) ((x) < 0 ? (-(x)) : (x))
#define ABS32(x) ((x) < 0 ? (-(x)) : (x))

static OPUS_INLINE opus_int16 SAT16(opus_int32 x) {
   return x > 32767 ? 32767 : x < -32768 ? -32768 : (opus_int16)x;
}


#include "fixed_generic.h"


#ifndef GLOBAL_STACK_SIZE
#define GLOBAL_STACK_SIZE 120000
#endif

#endif /* ARCH_H */
