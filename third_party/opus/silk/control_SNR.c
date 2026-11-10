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


#include "main.h"
#include "tuning_parameters.h"

/* This table holds SNR values divided by 21 (so they fit in 8 bits)
   for different target bitrates spaced at 400 bps interval. The first
   10 values are omitted (0-4 kb/s) because they're all zeros.
   The table was obtained by running different SNRs through the
   encoder and measuring the active bitrate. */
static const unsigned char silk_TargetRate_WB_21[201 - 10] = {
                                              0,  0,  0,  8, 29, 41,
     49, 56, 62, 66, 70, 74, 77, 80, 83, 86, 88, 91, 93, 95, 97, 99,
    101,103,105,107,108,110,112,113,115,116,118,119,121,122,123,125,
    126,127,129,130,131,132,134,135,136,137,138,140,141,142,143,144,
    145,146,147,148,149,150,151,152,153,154,156,157,158,159,159,160,
    161,162,163,164,165,166,167,168,169,170,171,171,172,173,174,175,
    176,177,177,178,179,180,181,181,182,183,184,185,185,186,187,188,
    189,189,190,191,192,192,193,194,195,195,196,197,198,198,199,200,
    200,201,202,203,203,204,205,206,206,207,208,209,209,210,211,211,
    212,213,214,214,215,216,216,217,218,219,219,220,221,221,222,223,
    224,224,225,226,226,227,228,229,229,230,231,232,232,233,234,234,
    235,236,237,237,238,239,240,240,241,242,243,243,244,245,246,246,
    247,248,249,249,250,251,252,253,255
};

/* Control SNR of residual quantizer */
void silk_control_SNR(
    silk_encoder_state          *psEncC,                        /* I/O  Pointer to Silk encoder state               */
    opus_int32                  TargetRate_bps                  /* I    Target max bitrate (bps)                    */
)
{
    int id;

    id = (TargetRate_bps+200)/400;
    id = silk_min(id - 10, (int)sizeof(silk_TargetRate_WB_21)-1);
    if( id <= 0 ) {
        psEncC->SNR_dB_Q7 = 0;
    } else {
        psEncC->SNR_dB_Q7 = silk_TargetRate_WB_21[id]*21;
    }
}
