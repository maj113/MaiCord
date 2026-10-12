# Opus decoder and voice encoder subsets

Official libopus 1.6.1 (2026-01-14), verified against https://opus-codec.org/downloads/.
Archive: https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz
SHA-256: `6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1`.

Upstream files are unchanged. `upstream-manifest.json` records each retained file's
size and SHA-256; `COPYING` and the source notices keep the BSD terms and patent
links. Only the decoder and fixed-point voice encoder dependency closures are
present. Neural models (DNN/LPCNet/OSCE/DRED), tonality analysis, custom-mode
generation, multistream/projection, the repacketizer, architecture backends, build
generators, downloads, tools and tests are absent.

## Build configuration

MaiCord's CMake targets use scalar fixed point, PCM16 and hardened state
invariants. These integration hooks do not modify upstream files.

- Float APIs, neural features, custom modes and 96 kHz extensions are not enabled.
- Caller-owned mono/stereo state is initialized with the size/init API; the
  heap-based create/reallocate convenience paths return no storage.
- Scratch uses MSVC's per-call `_alloca` intrinsic, with no global pseudo-stack or
  worker.
- Memory operations use the freestanding runtime. Invariant failure calls
  `__fastfail`, without CRT diagnostics.

`maicord_opus_encoder` serves the account DLL's voice sender: 48 kHz VOIP, one 20 ms
VBR frame per packet. `maicord_encoder_profile.c` compiles upstream
`src/opus_encoder.c` with the multi-frame repacketizing and CBR padding call sites,
which that profile cannot reach, bound to the invariant failure. Encoder-only units
build at `/O1` except `celt_encoder.c`; shared CELT/SILK units come from the decoder
target. Packets are bit-identical to an unmodified upstream build with the same
feature set (fixed point, float API off), and the encoder tests pin those packet
digests.

## Updating

1. Verify the official archive hash.
2. Re-evaluate both dependency closures.
3. Replace the unchanged upstream files and refresh `upstream-manifest.json`.
4. Regenerate the encoder reference digests from an unmodified upstream build.
5. Run the real PCM decoder/encoder tests and the shipping PE/import checks.

Do not copy the upstream build systems or model assets into this directory.
