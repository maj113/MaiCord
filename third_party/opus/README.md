# maiopus

A minimal, freestanding fork of libopus 1.6.1 (BSD-3-Clause, see `COPYING`;
the changes and added files use the same terms):

- **Decoder: complete.** Every mode (SILK, hybrid, CELT), bandwidth, frame size
  (2.5 to 120 ms per packet), stereo/mono coding, in-band FEC, DTX and packet
  loss concealment, with 8, 12, 16, 24 or 48 kHz output.
- **Encoder: one voice profile.** 48 kHz mono or stereo PCM16, VOIP tuning, one
  20 ms VBR frame per packet. Bitrate, complexity, in-band FEC, packet loss, DTX
  and the VBR constraint are adjustable. For these settings the packets are
  bit-identical to upstream libopus.
- **Fixed point, no heap, no alloca, no C runtime.** States live in caller memory
  (`opus_*_get_size()`/`opus_*_init()`); `opus_encode()` and `opus_decode()`
  work in caller scratch of `OPUS_SCRATCH_BYTES` that holds nothing between
  calls, so one block per thread serves every stream (`celt/stack_alloc.h`).
  Decoder checks on values derived from packet bits that bound a memory access
  or the range decoder end the process with an inline `__fastfail(7)`
  (`celt_packet_check` in `celt/arch.h`); the other upstream assertions
  (encoder, caller and configuration input, decoder state validation, algorithm
  invariants) compile to nothing.
- **x86-64 SSE4.1 only** (`MAIOPUS_SSE4_1`, required, and `MAIOPUS_ARCH`), selected
  at compile time with no runtime CPU dispatch and no C fallbacks for the kernels:
  the SILK LPC analysis and synthesis filters, resampler interpolation, delayed
  decision quantizer, VAD, Burg and VQ, and the CELT pitch, comb filter, MDCT
  rotation and FFT butterfly kernels. They are bit-exact with upstream's C code; the
  AVX2 kernel needs an AVX2 baseline and is not included.

Upstream: <https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz>,
SHA-256 `6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1`.

## Provenance

`upstream-manifest.json` lists every upstream file kept, with its upstream
SHA-256 and a status:

- `unchanged`: byte-identical to the release;
- `derived`: the output of `tools/derive.py` on the upstream file. It resolves
  preprocessor conditionals on configuration macros (fixed point, float API off,
  no custom modes, QEXT, DRED, deep PLC, OSCE, RTCD, assertions or hardening) and
  inlines the `ARG_QEXT`, `ARG_FIXED` and `QEXT_SCALE` argument macros.
  Conditions on compiler, platform or SIMD macros are kept;
- `modified`: derived, then edited by hand; `changes` lists each divergence.

`python tools/check_upstream.py opus-1.6.1.tar.gz` verifies all of it against
the official tarball and fails on unlisted sources. Every upstream notice is
kept. Not included from the release: the repacketizer, packet extensions,
multistream and projection, tonality analysis, DNN/LPCNet/DRED/OSCE, custom
modes, the float build, other architectures, build systems, tools and tests.

## Build

`CMakeLists.txt` defines a decoder library and a voice encoder library that
links it. A parent project can set `MAIOPUS_DECODER_TARGET`,
`MAIOPUS_ENCODER_TARGET`, `MAIOPUS_LINK_CONTRACT` (an INTERFACE target for link
options), `MAIOPUS_LTCG`, `MAIOPUS_SSE4_1` and `MAIOPUS_ARCH`. Encoder-only
units build at `/O1` except the CELT encoder; the decoder builds with type-based
alias analysis (`-fstrict-aliasing`), as upstream's GCC and Clang builds do.

## Verification

- Decoder: the official RFC 6716/8251 test vectors decode range-exact (final
  range coder state equal to the encoder's for every packet), pass
  `opus_compare`, and must reproduce pinned PCM hashes of the unmodified
  fixed-point build, mono and stereo.
- Encoder: packet digests across bitrates, complexities, FEC, DTX, the VBR
  constraint and setting changes must equal unmodified upstream.

## Updating

1. Verify the new release's SHA-256.
2. Copy the kept files, run `tools/derive.py` on them, re-apply the
   `modified` changes, then `tools/check_upstream.py --write`.
3. Re-pin the decoder PCM hashes and encoder digests from an unmodified build.
