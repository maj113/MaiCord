# Opus decoder and voice encoder subsets

Official libopus 1.6.1 (2026-01-14), verified against https://opus-codec.org/downloads/.
Archive: https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz
SHA-256: `6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1`.

Upstream files are unchanged. `upstream-manifest.json` records each retained file's
size and SHA-256; `COPYING` and source notices retain the BSD terms and patent links.
Only the decoder and fixed-point voice encoder dependency closures remain. Neural
models (DNN/LPCNet/OSCE/DRED), tonality analysis, custom-mode generation,
multistream/projection, the repacketizer, architecture backends, build generators,
downloads, tools and tests are absent.

MaiCord's CMake targets use scalar fixed point, PCM16 and hardened state invariants.
Float APIs, neural features, custom modes and 96 kHz extensions are not enabled.
Caller-owned mono/stereo state is initialized with the size/init API; heap-based
create/reallocate convenience paths return no storage. Scratch uses MSVC's per-call
`_alloca` intrinsic, without a global pseudo-stack or worker. Memory operations use
the existing freestanding runtime. Invariant failure uses `__fastfail`, without CRT
diagnostics. These integration hooks do not modify upstream files.

`maicord_opus_encoder` serves the account DLL's voice sender: 48 kHz VOIP, one 20 ms
VBR frame per packet. `maicord_encoder_profile.c` compiles upstream `src/opus_encoder.c`
with its multi-frame repacketizing and CBR padding call sites, unreachable in that
profile, bound to the invariant failure. Encoder-only units build at `/O1` except
`celt_encoder.c`; shared CELT/SILK units come from the decoder target. Packets are
bit-identical to an unmodified upstream build with the same feature set (fixed point,
float API off); the encoder tests pin those packet digests.

To update, verify the official archive hash, re-evaluate both dependency closures,
replace unchanged upstream files, refresh the source manifest, regenerate the encoder
reference digests from an unmodified upstream build, and validate the real PCM
decoder/encoder tests plus shipping PE/import checks. Do not copy the upstream build
systems or model assets into this directory.
