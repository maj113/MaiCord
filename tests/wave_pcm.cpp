#include "maicord/media/wave_pcm.hpp"

namespace maicord {
namespace {
struct WaveFixture {
    u8 bytes[40000];
    u32 count, fmt_offset, data_offset;
    float samples[16];
    WavePcmSource source;
} fixture{};
void put16(u8* p, u16 v) noexcept {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
}
void put32(u8* p, u32 v) noexcept {
    for (u32 i = 0; i < 4; ++i)
        p[i] = static_cast<u8>(v >> (i * 8));
}
void begin() noexcept {
    fixture.count = 12;
    copy_bytes(fixture.bytes, "RIFF", 4);
    copy_bytes(fixture.bytes + 8, "WAVE", 4);
}
u32 chunk(const char* name, ConstSlice<u8> bytes) noexcept {
    const u32 start = fixture.count;
    copy_bytes(fixture.bytes + start, name, 4);
    put32(fixture.bytes + start + 4, bytes.count);
    copy_bytes(fixture.bytes + start + 8, bytes.data, bytes.count);
    fixture.count += 8 + bytes.count;
    if (bytes.count & 1)
        fixture.bytes[fixture.count++] = 0xa5; // Padding has no sample meaning.
    put32(fixture.bytes + 4, fixture.count - 8);
    return start + 8;
}
void format(u16 tag, u16 channels, u16 bits, u32 rate = 48000, bool extensible = false, u16 valid_bits = 0,
            u32 mask = 0) noexcept {
    u8 fmt[40]{};
    put16(fmt, extensible ? 0xfffe : tag);
    put16(fmt + 2, channels);
    put32(fmt + 4, rate);
    const u16 align = static_cast<u16>(channels * (bits / 8));
    put32(fmt + 8, rate * align);
    put16(fmt + 12, align);
    put16(fmt + 14, bits);
    if (extensible) {
        put16(fmt + 16, 22);
        put16(fmt + 18, valid_bits ? valid_bits : bits);
        put32(fmt + 20, mask);
        put32(fmt + 24, tag);
        const u8 tail[12]{0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
        copy_bytes(fmt + 28, tail, 12);
    }
    fixture.fmt_offset = chunk("fmt ", {fmt, extensible ? 40u : 16u});
}
void samples(ConstSlice<u8> bytes) noexcept {
    fixture.data_offset = chunk("data", bytes);
}
bool parse() noexcept {
    return wave_pcm_parse(&fixture.source, {fixture.bytes, fixture.count}) == WavePcmError::none;
}
bool pcm_sample_matches(float a, float b) noexcept {
    const float d = a - b;
    return d < 0.000001f && d > -0.000001f;
}
bool unchanged(const WavePcmSource& before) noexcept {
    return bytes_equal(&before, &fixture.source, sizeof(before));
}
} // namespace
void test_wave_pcm(void (*check)(bool, const char*) noexcept) noexcept {
    static constexpr u16 widths[]{8, 16, 24, 32};
    for (u16 bits : widths) {
        begin();
        format(1, 1, bits, 8000);
        u8 pcm[16]{};
        const u32 width = bits / 8;
        if (bits == 8) {
            pcm[0] = 0;
            pcm[1] = 128;
            pcm[2] = 255;
            pcm[3] = 64;
        } else {
            const u32 minimum_bits = u32{1} << (bits - 1), maximum_bits = minimum_bits - 1;
            for (u32 byte = 0; byte < width; ++byte) {
                pcm[byte] = static_cast<u8>(minimum_bits >> (byte * 8));
                pcm[2 * width + byte] = static_cast<u8>(maximum_bits >> (byte * 8));
                pcm[3 * width + byte] = static_cast<u8>((minimum_bits | (minimum_bits / 2)) >> (byte * 8));
            }
        }
        samples({pcm, 4 * width});
        check(parse() && fixture.source.total_frames == 4 && fixture.source.sample_rate == 8000 &&
                  fixture.source.data.data == fixture.bytes + fixture.data_offset &&
                  fixture.source.encoded.data == fixture.bytes && fixture.source.duration_ms == 0,
              "wave PCM parser retains immutable source data and exact frames for each integer width");
        const auto result = wave_pcm_decode(fixture.source, 0, 8, {fixture.samples, 16});
        const float maximum = bits == 8    ? 127.0f / 128.0f
                              : bits == 16 ? 32767.0f / 32768.0f
                              : bits == 24 ? 8388607.0f / 8388608.0f
                                           : 1.0f;
        check(result.ok() && result.frames == 4 && fixture.samples[0] == -1 && fixture.samples[1] == 0 &&
                  pcm_sample_matches(fixture.samples[2], maximum) && fixture.samples[3] == -0.5f,
              "wave PCM converter normalizes integer extrema and clips frame count only at EOF");
        u64 offset = 0;
        check(wave_pcm_seek_frame(fixture.source, 3, &offset) == WavePcmError::none &&
                  offset == fixture.data_offset + 3 * width &&
                  wave_pcm_decode(fixture.source, 3, 1, {fixture.samples, 1}).ok() &&
                  fixture.samples[0] == -0.5f,
              "wave PCM exact frame seek addresses the same sample without decoder delay");
    }
    begin();
    const u8 odd[3]{4, 5, 6};
    chunk("JUNK", {odd, 3});
    format(1, 2, 24, 192000);
    const u8 stereo[12]{0, 0, 0x80, 0, 0, 0, 0xff, 0xff, 0x7f, 0, 0, 0xc0};
    samples({stereo, 12});
    check(parse() && fixture.source.channels == 2 && fixture.source.total_frames == 2 &&
              wave_pcm_decode(fixture.source, 0, 2, {fixture.samples, 4}).ok() &&
              fixture.samples[0] == -1 && fixture.samples[1] == 0 &&
              pcm_sample_matches(fixture.samples[2], 8388607.0f / 8388608.0f) &&
              fixture.samples[3] == -0.5f,
          "wave PCM walks padded unknown chunks and preserves native stereo interleaving at 192 kHz");
    const auto before = fixture.source;
    fixture.samples[0] = 9;
    fixture.samples[1] = 9;
    check(wave_pcm_decode(fixture.source, 0, 2, {fixture.samples, 3}).error == WavePcmError::capacity &&
              fixture.samples[0] == 9 && fixture.samples[1] == 9 && unchanged(before),
          "wave PCM insufficient output capacity preserves every sample and immutable source");
    u64 offset = 77;
    check(wave_pcm_seek_frame(fixture.source, fixture.source.total_frames, &offset) == WavePcmError::none &&
              offset == fixture.data_offset + 12 &&
              wave_pcm_decode(fixture.source, 2, 1, {fixture.samples, 16}).error ==
                  WavePcmError::end_of_stream &&
              wave_pcm_seek_frame(fixture.source, 3, &offset) == WavePcmError::invalid_input &&
              offset == fixture.data_offset + 12,
          "wave PCM permits exact EOF seek and rejects beyond-EOF seeks without replacing the offset");
    check(wave_pcm_decode(fixture.source, 0, 8193, {fixture.samples, 16}).error ==
                  WavePcmError::invalid_input &&
              wave_pcm_decode(fixture.source, 0, 1, {reinterpret_cast<float*>(fixture.bytes), 1}).error ==
                  WavePcmError::invalid_input,
          "wave PCM decode bounds work per call and rejects overwriting retained encoded source");
    begin();
    format(1, 1, 24, 44100, true, 20, 4);
    const u8 valid20[9]{0, 0, 0x80, 0xf0, 0xff, 0x7f, 0, 0, 0xc0};
    samples({valid20, 9});
    check(parse() && fixture.source.valid_bits == 20 && fixture.source.channel_mask == 4 &&
              wave_pcm_decode(fixture.source, 0, 3, {fixture.samples, 3}).ok() &&
              fixture.samples[0] == -1 && pcm_sample_matches(fixture.samples[1], 524287.0f / 524288.0f) &&
              fixture.samples[2] == -0.5f,
          "wave extensible PCM honors left-aligned 20 valid bits in a 24-bit mono container");
    fixture.bytes[fixture.data_offset + 3] |= 1;
    fixture.samples[0] = fixture.samples[1] = fixture.samples[2] = 9;
    check(wave_pcm_decode(fixture.source, 0, 3, {fixture.samples, 3}).error == WavePcmError::malformed &&
              fixture.samples[0] == 9 && fixture.samples[1] == 9 && fixture.samples[2] == 9,
          "wave extensible PCM rejects nonzero low padding transactionally after an earlier valid sample");
    for (u32 ext = 0; ext < 2; ++ext) {
        begin();
        format(3, 2, 32, 48000, ext != 0, 32, ext ? 3 : 0);
        u8 ieee[16]{};
        put32(ieee, 0x3f000000);
        put32(ieee + 4, 0xbf000000);
        put32(ieee + 8, 0x40000000);
        put32(ieee + 12, 0xc0000000);
        samples({ieee, 16});
        check(parse() && fixture.source.encoding == WavePcmEncoding::ieee_float &&
                  wave_pcm_decode(fixture.source, 0, 2, {fixture.samples, 4}).ok() &&
                  fixture.samples[0] == 0.5f && fixture.samples[1] == -0.5f && fixture.samples[2] == 1 &&
                  fixture.samples[3] == -1,
              "wave standard and extensible IEEE float32 preserve stereo values with bounded clipping");
        put32(fixture.bytes + fixture.data_offset + 12, 0x7fc00001);
        for (auto& sample : fixture.samples)
            sample = 9;
        check(wave_pcm_decode(fixture.source, 0, 2, {fixture.samples, 4}).error ==
                      WavePcmError::malformed &&
                  fixture.samples[0] == 9 && fixture.samples[3] == 9,
              "wave float NaN later in a block fails before any output sample write");
        put32(fixture.bytes + fixture.data_offset + 12, 0x7f800000);
        check(wave_pcm_decode(fixture.source, 0, 2, {fixture.samples, 4}).error ==
                      WavePcmError::malformed &&
                  fixture.samples[0] == 9,
              "wave float infinity is rejected without relying on CRT math classification");
    }
    const u8 pcm16[4]{0, 0x80, 0, 0};
    begin();
    format(1, 1, 16);
    samples({pcm16, 4});
    check(parse(), "wave baseline before malformed input checks");
    const auto retained = fixture.source;
    const u32 original_count = fixture.count;
    for (u32 cut = 0; cut < original_count; ++cut)
        check(wave_pcm_parse(&fixture.source, {fixture.bytes, cut}) != WavePcmError::none &&
                  unchanged(retained),
              "wave every truncated prefix rejects without replacing retained metadata");
    put32(fixture.bytes + 4, fixture.count - 9);
    check(wave_pcm_parse(&fixture.source, {fixture.bytes, fixture.count}) == WavePcmError::malformed &&
              unchanged(retained),
          "wave exact RIFF length rejects trailing bytes instead of hiding a second container");
    put32(fixture.bytes + 4, fixture.count - 8);
    put32(fixture.bytes + fixture.data_offset - 4, 0xffffffffu);
    check(wave_pcm_parse(&fixture.source, {fixture.bytes, fixture.count}) == WavePcmError::truncated &&
              unchanged(retained),
          "wave overflowing chunk size is bounded using wide arithmetic before data access");
    begin();
    format(1, 1, 16);
    samples({pcm16, 3});
    check(wave_pcm_parse(&fixture.source, {fixture.bytes, fixture.count}) == WavePcmError::malformed,
          "wave partial PCM frame data cannot be silently discarded");
    begin();
    format(1, 1, 8);
    samples({pcm16, 1});
    --fixture.count;
    put32(fixture.bytes + 4, fixture.count - 8);
    check(wave_pcm_parse(&fixture.source, {fixture.bytes, fixture.count}) == WavePcmError::truncated,
          "wave odd data requires the RIFF word padding byte");
    begin();
    format(1, 1, 16);
    samples({pcm16, 4});
    samples({pcm16, 4});
    check(wave_pcm_parse(&fixture.source, {fixture.bytes, fixture.count}) == WavePcmError::malformed,
          "wave duplicate data chunks fail rather than selecting an arbitrary sample stream");
    begin();
    format(1, 1, 16);
    format(1, 1, 16);
    samples({pcm16, 4});
    check(wave_pcm_parse(&fixture.source, {fixture.bytes, fixture.count}) == WavePcmError::malformed,
          "wave duplicate format chunks fail instead of overriding earlier authority");
    begin();
    samples({pcm16, 4});
    format(1, 1, 16);
    check(wave_pcm_parse(&fixture.source, {fixture.bytes, fixture.count}) == WavePcmError::malformed,
          "wave requires format authority before data");
    begin();
    format(1, 1, 16);
    check(wave_pcm_parse(&fixture.source, {fixture.bytes, fixture.count}) == WavePcmError::malformed,
          "wave missing data is explicit malformed input");
    begin();
    format(1, 1, 16);
    samples({nullptr, 0});
    check(wave_pcm_parse(&fixture.source, {fixture.bytes, fixture.count}) == WavePcmError::malformed,
          "wave empty data has no playable source");
    for (u32 issue = 0; issue < 8; ++issue) {
        begin();
        format(1, 1, 24, 48000, true, 20, 4);
        samples({valid20, 9});
        auto* fmt = fixture.bytes + fixture.fmt_offset;
        switch (issue) {
        case 0:
            put16(fmt + 18, 0);
            break;
        case 1:
            put16(fmt + 18, 25);
            break;
        case 2:
            put16(fmt + 12, 4);
            break;
        case 3:
            put32(fmt + 8, 1);
            break;
        case 4:
            put16(fmt + 16, 21);
            break;
        case 5:
            put32(fmt + 20, 3);
            break;
        case 6:
            fmt[28] = 1;
            break;
        default:
            put32(fmt + 24, 6);
            break;
        }
        const auto error = wave_pcm_parse(&fixture.source, {fixture.bytes, fixture.count});
        check(error == (issue >= 5 ? WavePcmError::unsupported : WavePcmError::malformed),
              "wave extensible rejects invalid precision alignment byte rate extension size layout or "
              "subformat");
    }
    for (u32 issue = 0; issue < 5; ++issue) {
        begin();
        format(issue == 0   ? 6
               : issue == 1 ? 3
                            : 1,
               issue == 2 ? 3 : 1, 16,
               issue == 3   ? 7999
               : issue == 4 ? 192001
                            : 48000);
        samples({pcm16, 4});
        check(wave_pcm_parse(&fixture.source, {fixture.bytes, fixture.count}) == WavePcmError::unsupported,
              "wave unsupported codecs float width channel count and sample rates remain explicit");
    }
    begin();
    format(1, 1, 16);
    samples({pcm16, 4});
    copy_bytes(fixture.bytes, "RIFX", 4);
    check(wave_pcm_parse(&fixture.source, {fixture.bytes, fixture.count}) == WavePcmError::unsupported,
          "wave big-endian RIFX is not misread as little-endian RIFF");
    copy_bytes(fixture.bytes, "RF64", 4);
    check(wave_pcm_parse(&fixture.source, {fixture.bytes, fixture.count}) == WavePcmError::unsupported,
          "wave RF64 requires a distinct implementation and remains unsupported");
    begin();
    for (u32 i = 0; i < kWavePcmMaximumChunks - 2; ++i)
        chunk("JUNK", {nullptr, 0});
    format(1, 1, 16);
    samples({pcm16, 4});
    check(parse(), "wave accepts exactly the bounded chunk inventory without an index allocation");
    chunk("JUNK", {nullptr, 0});
    check(wave_pcm_parse(&fixture.source, {fixture.bytes, fixture.count}) == WavePcmError::capacity,
          "wave zero-length unknown chunks are bounded by an explicit chunk-count cap");
    check(wave_pcm_parse(nullptr, {fixture.bytes, fixture.count}) == WavePcmError::invalid_input,
          "wave rejects null metadata output before accessing input");
    check(wave_pcm_parse(&fixture.source, {nullptr, 12}) == WavePcmError::invalid_input,
          "wave rejects a null encoded source before accessing input");
    // The exaggerated input range includes fixture.source in the same static
    // fixture. Use independent stack output to isolate the size-cap check.
    WavePcmSource capacity_output{};
    const auto capacity_before = capacity_output;
    check(wave_pcm_parse(&capacity_output, {fixture.bytes, kWavePcmMaximumEncodedBytes + 1}) ==
                  WavePcmError::capacity &&
              bytes_equal(&capacity_output, &capacity_before, sizeof(capacity_output)),
          "wave encoded size cap rejects before reading beyond the fixture and preserves output");
    check(wave_pcm_parse(&fixture.source, {fixture.bytes, kWavePcmMaximumEncodedBytes + 1}) ==
              WavePcmError::invalid_input,
          "wave output alias has priority when the declared oversized source includes metadata output");
    check(wave_pcm_parse(reinterpret_cast<WavePcmSource*>(fixture.bytes), {fixture.bytes, fixture.count}) ==
              WavePcmError::invalid_input,
          "wave rejects metadata output within encoded source before any input access or output write");
    begin();
    format(1, 1, 8, 16000);
    fixture.data_offset = fixture.count + 8;
    copy_bytes(fixture.bytes + fixture.count, "data", 4);
    put32(fixture.bytes + fixture.count + 4, 32001);
    for (u32 i = 0; i < 32001; ++i)
        fixture.bytes[fixture.data_offset + i] = 128;
    fixture.count += 8 + 32002;
    fixture.bytes[fixture.count - 1] = 0;
    put32(fixture.bytes + 4, fixture.count - 8);
    check(parse() && fixture.source.total_frames == 32001 && fixture.source.duration_ms == 2000 &&
              wave_pcm_decode(fixture.source, 32000, 8192, {fixture.samples, 1}).ok() &&
              fixture.samples[0] == 0,
          "wave duration uses exact u64 frame arithmetic and seek retains a final sub-millisecond frame");
    auto forged = fixture.source;
    forged.total_frames = ~u64{0};
    fixture.samples[0] = 9;
    check(wave_pcm_decode(forged, 0, 1, {fixture.samples, 1}).error == WavePcmError::invalid_input &&
              fixture.samples[0] == 9,
          "wave converter rejects forged frame metadata before arithmetic or output writes");
}
} // namespace maicord
