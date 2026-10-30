/***********************************************************************
Copyright (c) 2006-2011, Skype Limited. All rights reserved.
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
- Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
- Neither the name of Internet Society, IETF or IETF Trust, nor the
names of specific contributors, may be used to endorse or promote
products derived from this software without specific prior written
permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
***********************************************************************/


/* Filter coefficients for IIR/FIR polyphase resampling     *
 * Total size: 179 Words (358 Bytes)                        */

#include "resampler_private.h"

/* Matlab code for the notch filter coefficients: */
/* B = [1, 0.147, 1];  A = [1, 0.107, 0.89]; G = 0.93; freqz(G * B, A, 2^14, 16e3); axis([0, 8000, -10, 1]) */
/* fprintf('\t%6d, %6d, %6d, %6d\n', round(B(2)*2^16), round(-A(2)*2^16), round((1-A(3))*2^16), round(G*2^15)) */
/* const opus_int16 silk_resampler_up2_hq_notch[ 4 ] = { 9634,  -7012,   7209,  30474 }; */

/* Tables with IIR and FIR coefficients for fractional downsamplers (123 Words) */
silk_DWORD_ALIGN const opus_int16 silk_Resampler_3_4_COEFS[ 2 + 3 * RESAMPLER_DOWN_ORDER_FIR0 / 2 ] = {
    -20694, -13867,
       -49,     64,     17,   -157,    353,   -496,    163,  11047,  22205,
       -39,      6,     91,   -170,    186,     23,   -896,   6336,  19928,
       -19,    -36,    102,    -89,    -24,    328,   -951,   2568,  15909,
};

silk_DWORD_ALIGN const opus_int16 silk_Resampler_2_3_COEFS[ 2 + 2 * RESAMPLER_DOWN_ORDER_FIR0 / 2 ] = {
    -14457, -14019,
        64,    128,   -122,     36,    310,   -768,    584,   9267,  17733,
        12,    128,     18,   -142,    288,   -117,   -865,   4123,  14459,
};

silk_DWORD_ALIGN const opus_int16 silk_Resampler_1_2_COEFS[ 2 + RESAMPLER_DOWN_ORDER_FIR1 / 2 ] = {
       616, -14323,
       -10,     39,     58,    -46,    -84,    120,    184,   -315,   -541,   1284,   5380,   9024,
};

silk_DWORD_ALIGN const opus_int16 silk_Resampler_1_3_COEFS[ 2 + RESAMPLER_DOWN_ORDER_FIR2 / 2 ] = {
     16102, -15162,
       -13,      0,     20,     26,      5,    -31,    -43,     -4,     65,     90,      7,   -157,   -248,    -44,    593,   1583,   2612,   3271,
};

silk_DWORD_ALIGN const opus_int16 silk_Resampler_1_4_COEFS[ 2 + RESAMPLER_DOWN_ORDER_FIR2 / 2 ] = {
     22500, -15099,
         3,    -14,    -20,    -15,      2,     25,     37,     25,    -16,    -71,   -107,    -79,     50,    292,    623,    982,   1288,   1464,
};

silk_DWORD_ALIGN const opus_int16 silk_Resampler_1_6_COEFS[ 2 + RESAMPLER_DOWN_ORDER_FIR2 / 2 ] = {
     27540, -15257,
        17,     12,      8,      1,    -10,    -22,    -30,    -32,    -22,      3,     44,    100,    168,    243,    317,    381,    429,    455,
};

silk_DWORD_ALIGN const opus_int16 silk_Resampler_2_3_COEFS_LQ[ 2 + 2 * 2 ] = {
     -2797,  -6507,
      4697,  10739,
      1567,   8276,
};

/* Table with interpolation fractions of 1/24, 3/24, 5/24, ... , 23/24, each row the eight taps of one fractional
   position: the 4 coefficients of fraction t followed by those of fraction 11 - t in reverse order */
silk_DWORD_ALIGN const opus_int16 silk_resampler_frac_FIR_12[ 12 ][ RESAMPLER_ORDER_FIR_12 ] = {
    {  189,  -600,   617, 30567,  2996, -1375,   425,   -46 },
    {  117,  -159, -1070, 29704,  5784, -2143,   611,   -71 },
    {   52,   221, -2392, 28276,  8798, -2865,   773,   -91 },
    {   -4,   529, -3350, 26341, 11950, -3487,   896,  -103 },
    {  -48,   758, -3956, 23973, 15143, -3957,   967,  -107 },
    {  -80,   905, -4235, 21254, 18278, -4222,   972,   -99 },
    {  -99,   972, -4222, 18278, 21254, -4235,   905,   -80 },
    { -107,   967, -3957, 15143, 23973, -3956,   758,   -48 },
    { -103,   896, -3487, 11950, 26341, -3350,   529,    -4 },
    {  -91,   773, -2865,  8798, 28276, -2392,   221,    52 },
    {  -71,   611, -2143,  5784, 29704, -1070,  -159,   117 },
    {  -46,   425, -1375,  2996, 30567,   617,  -600,   189 },
};
