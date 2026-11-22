#include "maicord/platform/decode/aac_decoder.hpp"
#include "maicord/media/audio/aac_configuration.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
void test_aac_decoder(void (*check)(bool, const char*) noexcept) noexcept {
    u32 capacity = 0;
    check(aac_decoder_output_capacity(0, &capacity) && capacity == 16384 &&
              aac_decoder_output_capacity(16384, &capacity) && capacity == 16384 &&
              aac_decoder_output_capacity(49152, &capacity) && capacity == 49152 &&
              aac_decoder_output_capacity(65536, &capacity) && capacity == 65536 &&
              !aac_decoder_output_capacity(65537, &capacity) && !aac_decoder_output_capacity(~u32{0}, &capacity),
          "AAC buffers: the native output capacity is at least 16 KiB and at most 64 KiB");
    u32 mask = 0;
    check(aac_decoder_alignment_mask(0, &mask) && mask == 0 && aac_decoder_alignment_mask(1, &mask) &&
              mask == 0 && aac_decoder_alignment_mask(16, &mask) && mask == 15 &&
              aac_decoder_alignment_mask(4096, &mask) && mask == 4095 && !aac_decoder_alignment_mask(3, &mask) &&
              !aac_decoder_alignment_mask(4097, &mask) && !aac_decoder_alignment_mask(~u32{0}, &mask),
          "AAC buffers: power-of-two alignments up to 4096 become MF masks; others are refused");
    const u8 stereo44[]{0x12, 0x10}, mono48[]{0x11, 0x88}, absent_sbr[]{0x12, 0x10, 0x56, 0xe5, 0};
    check(aac_decoder_configuration_valid({stereo44, 2}, 44100, 2) &&
              aac_decoder_configuration_valid({mono48, 2}, 48000, 1) &&
              aac_decoder_configuration_valid({absent_sbr, 5}, 44100, 2),
          "AAC metadata: exact LC rates/channels and explicit absent-SBR extension admitted");
    const u8 he44[]{0x2b, 0x92, 0x08, 0}, he48mono[]{0x2b, 0x09, 0x88, 0}, ps44[]{0xeb, 0x8a, 0x08, 0};
    const u8 sync_he44[]{0x13, 0x90, 0x56, 0xe5, 0xa0};
    AacConfiguration configuration{};
    check(aac_configuration_decode({he44, 4}, &configuration) && configuration.core_sample_rate == 22050 &&
              configuration.sample_rate == 44100 && configuration.core_channels == 2 &&
              configuration.channels == 2 && configuration.frames_per_packet == 2048 && configuration.sbr &&
              !configuration.ps && aac_decoder_configuration_valid({he44, 4}, 44100, 2) &&
              aac_decoder_configuration_valid({sync_he44, 5}, 44100, 2) &&
              aac_decoder_configuration_valid({he48mono, 4}, 48000, 1) &&
              aac_decoder_configuration_valid({ps44, 4}, 44100, 2),
          "AAC metadata: actual explicit SBR and sync SBR preserve core clock while exact PCM rate and2048 "
          "packet frames double");
    check(aac_configuration_decode({ps44, 4}, &configuration) && configuration.core_channels == 1 &&
              configuration.channels == 2 && configuration.ps &&
              !aac_decoder_configuration_valid({ps44, 4}, 44100, 1) &&
              !aac_decoder_configuration_valid({he44, 4}, 22050, 2),
          "AAC metadata: explicit PS distinguishes mono core from stereo output and refuses core-as-output "
          "negotiation");
    const auto saved_configuration = configuration;
    const u8 he64[]{0x2a, 0x91, 0x08, 0}, single_rate_sbr[]{0x2b, 0x93, 0x88, 0};
    check(!aac_configuration_decode({he44, 3}, &configuration) &&
              !aac_configuration_decode({he64, 4}, &configuration) &&
              !aac_configuration_decode({single_rate_sbr, 4}, &configuration) &&
              bytes_equal(&saved_configuration, &configuration, sizeof(configuration)),
          "AAC metadata: truncated SBR, unsupported PCM rate and undeclared frame ratio refuse atomically");
    u8 bad[]{0x12, 0x10};
    check(!aac_decoder_configuration_valid({stereo44, 2}, 48000, 2) &&
              !aac_decoder_configuration_valid({stereo44, 2}, 44100, 1) &&
              !aac_decoder_configuration_valid({stereo44, 1}, 44100, 2) &&
              !aac_decoder_configuration_valid({}, 44100, 2),
          "AAC metadata: mismatched rate/channel and truncated config reject");
    bad[1] = 0x14;
    check(!aac_decoder_configuration_valid({bad, 2}, 44100, 2),
          "AAC metadata: 960-frame mode is not disguised as supported 1024 LC");
    bad[1] = 0x12;
    check(!aac_decoder_configuration_valid({bad, 2}, 44100, 2),
          "AAC metadata: core-coder dependency rejects");
    bad[1] = 0x11;
    check(!aac_decoder_configuration_valid({bad, 2}, 44100, 2), "AAC metadata: extensionFlag rejects");
    bad[0] = 0x2a;
    bad[1] = 0x10;
    check(!aac_decoder_configuration_valid({bad, 2}, 44100, 2),
          "AAC metadata: truncated explicit HE-AAC rejects");
    const u8 present_sbr[]{0x12, 0x10, 0x56, 0xe5, 0x80};
    check(!aac_decoder_configuration_valid({present_sbr, 5}, 44100, 2),
          "AAC metadata: out-of-domain SBR extension refuses rather than changing output rate silently");
    AacDecoder decoder{};
    check(aac_decoder_initialize(&decoder, {stereo44, 2}, 44100, 2).ok() &&
              aac_decoder_initialize(&decoder, {stereo44, 2}, 44100, 2).error == AacDecodeError::busy &&
              aac_decoder_submit(&decoder, {}, 0, 1024).error == AacDecodeError::invalid_input,
          "AAC lifecycle: a second initialization is busy and an empty packet is invalid");
    check(aac_decoder_drain(&decoder).ok() && aac_decoder_flush(&decoder).ok() && aac_decoder_release(&decoder).ok() &&
              aac_decoder_initialize(&decoder, {he44, 4}, 44100, 2).ok() && aac_decoder_release(&decoder).ok(),
          "AAC lifecycle: drain, flush and release without a packet; an HE configuration initializes");
}
} // namespace maicord
