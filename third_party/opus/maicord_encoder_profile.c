/* MaiCord's voice encoder profile: exactly one 20 ms 48 kHz frame per packet,
 * always VBR. Upstream src/opus_encoder.c is compiled unchanged here; only the
 * call sites for multi-frame repacketizing (frames above 20 ms) and CBR
 * padding, both unreachable in this profile, become invariant failures, so
 * repacketizer.c and extension generation are not part of the build. The
 * MaiCord wrapper rejects other frame sizes and never disables VBR. */
#include <intrin.h>
#include "opus.h"
#include "opus_private.h"

static __declspec(noreturn) void maicord_opus_encoder_unreachable(void) {
    __fastfail(7); /* FAST_FAIL_FATAL_APP_EXIT, as celt_fatal. */
}
#define opus_repacketizer_init(rp) (maicord_opus_encoder_unreachable(), (rp))
#define opus_repacketizer_cat(...) (maicord_opus_encoder_unreachable(), 0)
#define opus_repacketizer_out_range_impl(...) (maicord_opus_encoder_unreachable(), 0)
#define opus_packet_pad(...) (maicord_opus_encoder_unreachable(), 0)
#define opus_packet_pad_impl(...) (maicord_opus_encoder_unreachable(), 0)
#include "src/opus_encoder.c"
