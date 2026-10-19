// CPU decode benchmark over the synthetic corpus written by
// tools/generate_media_decode_benchmark_corpus.py (path in MAICORD_DECODE_CORPUS).
// Every item runs the production decoder entry points in the order the media
// worker calls them, with the worker's workspace and buffer shapes. One
// verification run hashes the complete decoder output (golden hash); timed runs
// then repeat the decode and report minimum/median TSC cycles per output pixel
// (per PCM frame for audio). Lines are "decode key=value ..." on stdout.
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/animated_webp.hpp"
#include "maicord/media/apng.hpp"
#include "maicord/media/gif.hpp"
#include "maicord/media/jpeg.hpp"
#include "maicord/media/lossy_webp.hpp"
#include "maicord/media/mp3_stream.hpp"
#include "maicord/media/ogg_opus.hpp"
#include "maicord/media/opus_decoder.hpp"
#include "maicord/media/png.hpp"
#include "maicord/media/png_stream.hpp"
#include "maicord/media/webp_thumbnail.hpp"

#include <intrin.h>
#include <new>
#include <Windows.h>
#include <psapi.h>

namespace maicord {
namespace {

enum Kind : u32 {
    kind_png = 1,
    kind_apng,
    kind_gif,
    kind_jpeg,
    kind_webp,
    kind_animated_webp,
    kind_jpeg_thumbnail,
    kind_webp_thumbnail,
    kind_png_stream,
    kind_ogg_opus,
    kind_mp3,
};

const char* kind_name(u32 kind) noexcept {
    constexpr const char* names[] = {"none", "png", "apng", "gif", "jpeg", "webp", "animated_webp",
                                     "jpeg_thumbnail", "webp_thumbnail", "png_stream", "ogg_opus", "mp3"};
    return kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "unknown";
}

struct Line {
    char text[1024];
    u32 used = 0;
    void append(const char* value) noexcept {
        while (*value && used + 3 < sizeof(text))
            text[used++] = *value++;
    }
    void append(const char* value, u32 count) noexcept {
        for (u32 i = 0; i < count && used + 3 < sizeof(text); ++i)
            text[used++] = value[i];
    }
    void number(u64 value) noexcept {
        char digits[21];
        u32 count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value);
        while (count && used + 3 < sizeof(text))
            text[used++] = digits[--count];
    }
    void field(const char* key, u64 value) noexcept {
        append(" ");
        append(key);
        append("=");
        number(value);
    }
    void hex(const char* key, u64 value) noexcept {
        append(" ");
        append(key);
        append("=");
        for (i32 shift = 60; shift >= 0; shift -= 4)
            text[used++] = "0123456789abcdef"[(value >> shift) & 15u];
    }
    void write() noexcept {
        text[used++] = '\r';
        text[used++] = '\n';
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, used, &written, nullptr);
    }
};

u8* reserve(usize bytes) noexcept {
    return static_cast<u8*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}

// Large buffers shared by all items, sized for the decoders' maxima.
struct Buffers {
    u8* scanlines;       // PNG/APNG scanlines, GIF indices, WebP RGBA/alpha scratch
    u32 scanline_bytes;
    u8* scratch;         // thumbnail/stream scratch
    usize scratch_bytes;
    u8* row;             // thumbnail rows
    DeflateWorkspace deflate;
    JpegWorkspace* jpeg;
    LossyWebpWorkspace* lossy;
    Vp8lWorkspace* lossless;
    ApngWorkspace* apng;
    GifWorkspace* gif;
    AnimatedWebpWorkspace* animated;
    i16* pcm;
    float* pcm_float;
    u8* audio_state;
    u8* ogg_scratch;
};
Buffers g{};
// MAICORD_DECODE_STAGES=1: lossy WebP items also report the minimum TSC
// cycles of each VP8 stage over the timed runs.
bool g_stages = false;
// Minimum container-parse cycles of an animation (the per-frame cost when a
// lane has no retained parse for the source).
u64 g_parse_minimum;
template <typename Parse>
bool timed_parse(Parse parse) noexcept {
    const u64 begin = __rdtsc();
    const bool ok = parse();
    g_parse_minimum = minimum(g_parse_minimum, __rdtsc() - begin);
    return ok;
}
u64 g_stage_minimum[5];

struct Outcome {
    u64 hash;
    u64 pixels;       // output pixels (or PCM frames)
    u64 output_bytes; // bytes the decoder wrote for the consumer
    u64 scratch_bytes;
    u32 frames;
    bool ok;
};

u64 hash_bytes(u64 hash, const void* bytes, u64 count) noexcept {
    const u8* data = static_cast<const u8*>(bytes);
    while (count) {
        const u32 chunk = static_cast<u32>(minimum<u64>(count, 1u << 30));
        hash = hash64_bytes(hash, data, chunk);
        data += chunk;
        count -= chunk;
    }
    return hash;
}

// ---- Images -----------------------------------------------------------------------
Outcome decode_png(ConstSlice<u8> encoded, bool verify) noexcept {
    Outcome outcome{kHash64Seed};
    PngImage image{};
    const auto parsed = png_parse(encoded, &image);
    const u32 required = parsed.ok() ? png_decode_buffer_bytes(image) : 0;
    if (!parsed.ok() || !required || required > g.scanline_bytes)
        return outcome;
    const auto decoded = png_decode_parsed_scanlines(&image, &g.deflate, {g.scanlines, g.scanline_bytes});
    outcome.ok = decoded.ok();
    outcome.pixels = u64{image.width} * image.height;
    outcome.output_bytes = image.inflated_bytes;
    outcome.scratch_bytes = required;
    outcome.frames = 1;
    if (verify && outcome.ok)
        outcome.hash = hash_bytes(outcome.hash, g.scanlines, image.inflated_bytes);
    return outcome;
}

Outcome decode_apng(ConstSlice<u8> encoded, bool verify) noexcept {
    Outcome outcome{kHash64Seed};
    ApngImage animation{};
    if (!timed_parse([&]() noexcept { return apng_parse(encoded, &animation, g.apng).ok(); }))
        return outcome;
    outcome.ok = true;
    for (u32 index = 0; index < animation.frame_count && outcome.ok; ++index) {
        PngImage frame{};
        const auto decoded =
            apng_decode_parsed_frame(encoded, animation, index, &g.deflate, &frame, {g.scanlines, g.scanline_bytes});
        outcome.ok = decoded.ok();
        outcome.pixels += u64{frame.width} * frame.height;
        outcome.output_bytes += frame.inflated_bytes;
        outcome.scratch_bytes = maximum<u64>(outcome.scratch_bytes, frame.inflated_bytes);
        if (verify && outcome.ok)
            outcome.hash = hash_bytes(outcome.hash, g.scanlines, frame.inflated_bytes);
    }
    outcome.frames = animation.frame_count;
    return outcome;
}

Outcome decode_gif(ConstSlice<u8> encoded, bool verify) noexcept {
    Outcome outcome{kHash64Seed};
    GifImage animation{};
    if (!timed_parse([&]() noexcept { return gif_parse(encoded, &animation, g.gif).ok(); }))
        return outcome;
    outcome.ok = true;
    for (u32 index = 0; index < animation.frame_count && outcome.ok; ++index) {
        const auto decoded =
            gif_decode_parsed_frame(encoded, animation, index, g.gif, {g.scanlines, g.scanline_bytes});
        outcome.ok = decoded.ok();
        outcome.pixels += decoded.pixels;
        outcome.output_bytes += decoded.pixels;
        outcome.scratch_bytes = maximum<u64>(outcome.scratch_bytes, decoded.pixels);
        if (verify && outcome.ok) {
            outcome.hash = hash_bytes(outcome.hash, g.scanlines, decoded.pixels);
            outcome.hash = hash_bytes(outcome.hash, decoded.palette.rgba, decoded.palette.count * 4u);
        }
    }
    outcome.frames = animation.frame_count;
    return outcome;
}

Outcome decode_jpeg(ConstSlice<u8> encoded, bool verify) noexcept {
    Outcome outcome{kHash64Seed};
    JpegImage image{};
    JpegWorkspace* workspace = ::new (g.jpeg) JpegWorkspace;
    const auto decoded = jpeg_decode_coefficients(encoded, &image, workspace);
    outcome.ok = decoded.ok();
    outcome.pixels = u64{image.width} * image.height;
    outcome.output_bytes = u64{image.coefficient_blocks} * sizeof(JpegCoefficientBlock);
    outcome.scratch_bytes = sizeof(JpegWorkspace);
    outcome.frames = 1;
    if (verify && outcome.ok)
        outcome.hash = hash_bytes(outcome.hash, workspace->coefficients.blocks, outcome.output_bytes);
    return outcome;
}

u64 hash_vp8(u64 hash, const LossyWebpWorkspace& workspace) noexcept {
    hash = hash_bytes(hash, workspace.modes.macroblocks, u64{workspace.modes.macroblock_count} * sizeof(Vp8MacroblockMode));
    return hash_bytes(hash, workspace.coefficients.macroblocks,
                      u64{workspace.coefficients.macroblock_count} * sizeof(Vp8MacroblockCoefficients));
}

Outcome decode_webp(ConstSlice<u8> encoded, bool verify) noexcept {
    Outcome outcome{kHash64Seed};
    WebpImage parsed{};
    if (!webp_parse(encoded, &parsed).ok())
        return outcome;
    WebpImage image{};
    outcome.frames = 1;
    outcome.pixels = u64{parsed.width} * parsed.height;
    if (parsed.codec == WebpCodec::vp8l_lossless) {
        const auto decoded = webp_lossless_decode_rgba8(encoded, &image, g.lossless, {g.scanlines, g.scanline_bytes});
        outcome.ok = decoded.ok();
        outcome.output_bytes = decoded.rgba_bytes;
        outcome.scratch_bytes = sizeof(Vp8lWorkspace);
        if (verify && outcome.ok)
            outcome.hash = hash_bytes(outcome.hash, g.scanlines, decoded.rgba_bytes);
        return outcome;
    }
    const u32 pixels = parsed.width * parsed.height;
    LossyWebpWorkspace* workspace = ::new (g.lossy) LossyWebpWorkspace;
    Vp8Keyframe frame{};
    Vp8ControlHeader header{};
    if (g_stages && !verify) {
        u64 marks[6];
        marks[0] = __rdtsc();
        const bool envelope = vp8_parse_keyframe(parsed.bitstream, &frame).ok();
        marks[1] = __rdtsc();
        const bool control = envelope && vp8_parse_keyframe_control(frame, &header, &workspace->probabilities).ok();
        marks[2] = __rdtsc();
        const bool modes = control && vp8_parse_keyframe_modes(frame, header, &workspace->modes).ok();
        marks[3] = __rdtsc();
        const bool tokens = modes && vp8_decode_keyframe_coefficients(frame, header, workspace->probabilities,
                                                                     workspace->modes, &workspace->coefficients).ok();
        marks[4] = __rdtsc();
        bool alpha = true;
        if (tokens && parsed.alpha_bitstream.count != 0) {
            alpha = webp_alpha_decode(parsed.alpha_bitstream, parsed.width, parsed.height, g.lossless,
                                      {g.scanlines, pixels * 4u}, {g.scanlines + pixels * 4u, pixels})
                        .ok();
        }
        marks[5] = __rdtsc();
        for (u32 stage = 0; stage < 5; ++stage)
            g_stage_minimum[stage] = minimum(g_stage_minimum[stage], marks[stage + 1] - marks[stage]);
        outcome.ok = tokens && alpha;
        return outcome;
    }
    const auto decoded = lossy_webp_decode_keyframe(
        encoded, &image, &frame, &header, &workspace->probabilities, &workspace->modes, &workspace->coefficients,
        parsed.has_alpha ? g.lossless : nullptr,
        parsed.has_alpha ? Slice<u8>{g.scanlines, pixels * 4u} : Slice<u8>{},
        parsed.has_alpha ? Slice<u8>{g.scanlines + pixels * 4u, pixels} : Slice<u8>{});
    outcome.ok = decoded.ok();
    outcome.output_bytes = u64{workspace->coefficients.macroblock_count} * sizeof(Vp8MacroblockCoefficients) +
                           u64{workspace->modes.macroblock_count} * sizeof(Vp8MacroblockMode) + decoded.alpha_bytes;
    outcome.scratch_bytes = sizeof(LossyWebpWorkspace) + (parsed.has_alpha ? sizeof(Vp8lWorkspace) + pixels * 5u : 0);
    if (verify && outcome.ok) {
        outcome.hash = hash_vp8(outcome.hash, *workspace);
        outcome.hash = hash_bytes(outcome.hash, g.scanlines + pixels * 4u, decoded.alpha_bytes);
    }
    return outcome;
}

Outcome decode_animated_webp(ConstSlice<u8> encoded, bool verify) noexcept {
    Outcome outcome{kHash64Seed};
    AnimatedWebpImage animation{};
    *g.animated = AnimatedWebpWorkspace{};
    if (!timed_parse([&]() noexcept { return animated_webp_parse(encoded, &animation, g.animated).ok(); }))
        return outcome;
    outcome.ok = true;
    for (u32 index = 0; index < animation.frame_count && outcome.ok; ++index) {
        const AnimatedWebpFrame& requested = animation.frames[index];
        const u32 pixels = u32{requested.width} * requested.height;
        const bool external_alpha = requested.codec == AnimatedWebpCodec::vp8_lossy && requested.has_alpha;
        LossyWebpWorkspace* lossy = requested.codec == AnimatedWebpCodec::vp8_lossy
                                        ? ::new (g.lossy) LossyWebpWorkspace
                                        : g.lossy;
        Vp8Keyframe frame{};
        Vp8ControlHeader header{};
        const auto decoded = animated_webp_decode_parsed_frame(
            encoded, animation, index, g.lossless, lossy, &frame, &header, {g.scanlines, g.scanline_bytes},
            external_alpha ? Slice<u8>{g.scanlines, pixels * 4u} : Slice<u8>{},
            external_alpha ? Slice<u8>{g.scanlines + pixels * 4u, g.scanline_bytes - pixels * 4u} : Slice<u8>{});
        outcome.ok = decoded.ok();
        outcome.pixels += pixels;
        if (!outcome.ok)
            break;
        if (requested.codec == AnimatedWebpCodec::vp8l_lossless) {
            outcome.output_bytes += decoded.frame_decode.rgba_bytes;
            if (verify)
                outcome.hash = hash_bytes(outcome.hash, g.scanlines, decoded.frame_decode.rgba_bytes);
        } else {
            outcome.output_bytes += u64{lossy->coefficients.macroblock_count} * sizeof(Vp8MacroblockCoefficients) +
                                    decoded.lossy_decode.alpha_bytes;
            if (verify) {
                outcome.hash = hash_vp8(outcome.hash, *lossy);
                if (external_alpha)
                    outcome.hash = hash_bytes(outcome.hash, g.scanlines + pixels * 4u, decoded.lossy_decode.alpha_bytes);
            }
        }
    }
    outcome.frames = animation.frame_count;
    outcome.scratch_bytes = sizeof(Vp8lWorkspace) + sizeof(LossyWebpWorkspace) + sizeof(AnimatedWebpWorkspace);
    return outcome;
}

struct RowHash {
    u64 hash;
    u32 row_bytes;
    u32 rows;
    bool verify;
};

bool hash_row(void* context, u32, const u8* row) noexcept {
    auto& state = *static_cast<RowHash*>(context);
    ++state.rows;
    if (state.verify)
        state.hash = hash_bytes(state.hash, row, state.row_bytes);
    else
        state.hash += row[0];
    return true;
}

Outcome decode_jpeg_thumbnail(ConstSlice<u8> encoded, u32 extent, bool verify) noexcept {
    Outcome outcome{kHash64Seed};
    JpegImage image{};
    JpegThumbnailLayout layout{};
    if (!jpeg_thumbnail_parse(encoded, extent, &image, ::new (g.jpeg) JpegWorkspace, &layout).ok() ||
        layout.coefficient_bytes + layout.plane_bytes > g.scratch_bytes)
        return outcome;
    RowHash rows{kHash64Seed, layout.width * 4u, 0, verify};
    const auto decoded = jpeg_thumbnail_decode_rows(
        encoded, image, layout, {g.scratch, static_cast<u32>(layout.coefficient_bytes + layout.plane_bytes)},
        {g.row, layout.width * 4u}, hash_row, &rows);
    outcome.ok = decoded.ok() && rows.rows == layout.height;
    outcome.pixels = u64{image.width} * image.height;
    outcome.output_bytes = layout.rgba_bytes;
    outcome.scratch_bytes = layout.coefficient_bytes + layout.plane_bytes;
    outcome.frames = 1;
    outcome.hash = rows.hash;
    return outcome;
}

Outcome decode_webp_thumbnail(ConstSlice<u8> encoded, bool verify) noexcept {
    Outcome outcome{kHash64Seed};
    WebpThumbnailLayout layout{};
    if (webp_thumbnail_parse(encoded, &layout) != WebpThumbnailError::none || layout.workspace_bytes > g.scratch_bytes)
        return outcome;
    RowHash rows{kHash64Seed, layout.width * 4u, 0, verify};
    const auto decoded = webp_thumbnail_decode(layout, {g.scratch, layout.workspace_bytes}, hash_row, &rows);
    outcome.ok = decoded == WebpThumbnailError::none && rows.rows == layout.height;
    outcome.pixels = u64{layout.width} * layout.height;
    outcome.output_bytes = outcome.pixels * 4u;
    outcome.scratch_bytes = layout.workspace_bytes;
    outcome.frames = 1;
    outcome.hash = rows.hash;
    return outcome;
}

Outcome decode_png_stream(ConstSlice<u8> encoded, bool verify) noexcept {
    Outcome outcome{kHash64Seed};
    PngImage image{};
    PngStreamLayout layout{};
    if (!png_stream_parse(encoded, &image, &layout).ok())
        return outcome;
    const u64 span_bytes = (u64{layout.idat_chunks} * sizeof(DeflateInputSpan) + 15u) & ~15ull;
    if (span_bytes + kPngStreamWindowBytes + layout.row_buffer_bytes > g.scratch_bytes)
        return outcome;
    RowHash rows{kHash64Seed, image.row_bytes, 0, verify};
    const auto decoded = png_stream_decode(
        encoded, image, layout, {reinterpret_cast<DeflateInputSpan*>(g.scratch), layout.idat_chunks},
        {g.scratch + span_bytes, kPngStreamWindowBytes},
        {g.scratch + span_bytes + kPngStreamWindowBytes, layout.row_buffer_bytes}, hash_row, &rows, &g.deflate);
    outcome.ok = decoded.ok() && rows.rows == image.height;
    outcome.pixels = u64{image.width} * image.height;
    outcome.output_bytes = layout.inflated_bytes;
    outcome.scratch_bytes = span_bytes + kPngStreamWindowBytes + layout.row_buffer_bytes;
    outcome.frames = 1;
    outcome.hash = rows.hash;
    return outcome;
}

// ---- Audio ------------------------------------------------------------------------
Outcome decode_ogg_opus(ConstSlice<u8> encoded, bool verify) noexcept {
    Outcome outcome{kHash64Seed};
    OggOpusDemuxer demuxer{};
    if (ogg_opus_demux_initialize(&demuxer, encoded, {g.ogg_scratch, kOggOpusMaximumPacketBytes}) != OggOpusError::none)
        return outcome;
    const u32 storage = opus_decoder_storage_bytes(demuxer.channels);
    OpusDecoder decoder{};
    if (!storage || storage > 1u << 20 ||
        !opus_decoder_initialize(&decoder, {g.audio_state, storage}, demuxer.channels).ok())
        return outcome;
    outcome.ok = true;
    for (;;) {
        const auto packet = ogg_opus_demux_next(&demuxer, {g.ogg_scratch, kOggOpusMaximumPacketBytes});
        if (packet.error == OggOpusError::end_of_stream)
            break;
        if (!packet.ok()) {
            outcome.ok = false;
            break;
        }
        const auto decoded = opus_decoder_decode(&decoder, packet.bytes,
                                                 {g.pcm, kOpusDecoderMaximumFrames * demuxer.channels});
        if (!decoded.ok()) {
            outcome.ok = false;
            break;
        }
        outcome.pixels += decoded.frames;
        outcome.output_bytes += u64{decoded.frames} * demuxer.channels * sizeof(i16);
        ++outcome.frames;
        if (verify)
            outcome.hash = hash_bytes(outcome.hash, g.pcm, decoded.frames * demuxer.channels * sizeof(i16));
    }
    opus_decoder_release(&decoder);
    outcome.scratch_bytes = storage + kOggOpusMaximumPacketBytes;
    return outcome;
}

Outcome decode_mp3(ConstSlice<u8> encoded, bool verify) noexcept {
    Outcome outcome{kHash64Seed};
    Mp3Stream stream{};
    if (mp3_stream_initialize(&stream, encoded) != Mp3Error::none)
        return outcome;
    const u32 storage = mp3_decoder_storage_bytes();
    Mp3Decoder decoder{};
    if (!storage || storage > 1u << 20 ||
        mp3_decoder_initialize(&decoder, {g.audio_state, storage}, stream.sample_rate, stream.channels) != Mp3Error::none)
        return outcome;
    outcome.ok = true;
    for (;;) {
        const auto packet = mp3_stream_next(&stream);
        if (packet.error == Mp3Error::end_of_stream)
            break;
        if (!packet.ok()) {
            outcome.ok = false;
            break;
        }
        const auto decoded = mp3_decoder_decode(&decoder, packet.bytes, {g.pcm_float, kMp3MaximumFrames * 2u});
        if (decoded.error == Mp3Error::need_preroll)
            continue;
        if (!decoded.ok()) {
            outcome.ok = false;
            break;
        }
        outcome.pixels += decoded.frames;
        outcome.output_bytes += u64{decoded.frames} * stream.channels * sizeof(float);
        ++outcome.frames;
        if (verify)
            outcome.hash = hash_bytes(outcome.hash, g.pcm_float, decoded.frames * stream.channels * sizeof(float));
    }
    mp3_decoder_release(&decoder);
    outcome.scratch_bytes = storage;
    return outcome;
}

Outcome decode(u32 kind, u32 parameter, ConstSlice<u8> encoded, bool verify) noexcept {
    switch (kind) {
    case kind_png: return decode_png(encoded, verify);
    case kind_apng: return decode_apng(encoded, verify);
    case kind_gif: return decode_gif(encoded, verify);
    case kind_jpeg: return decode_jpeg(encoded, verify);
    case kind_webp: return decode_webp(encoded, verify);
    case kind_animated_webp: return decode_animated_webp(encoded, verify);
    case kind_jpeg_thumbnail: return decode_jpeg_thumbnail(encoded, parameter, verify);
    case kind_webp_thumbnail: return decode_webp_thumbnail(encoded, verify);
    case kind_png_stream: return decode_png_stream(encoded, verify);
    case kind_ogg_opus: return decode_ogg_opus(encoded, verify);
    case kind_mp3: return decode_mp3(encoded, verify);
    default: return Outcome{};
    }
}

u32 environment_number(const wchar_t* name, u32 fallback) noexcept {
    wchar_t text[32]{};
    const DWORD length = GetEnvironmentVariableW(name, text, 32);
    if (!length || length >= 32)
        return fallback;
    u32 value = 0;
    for (DWORD i = 0; i < length; ++i) {
        if (text[i] < L'0' || text[i] > L'9')
            return fallback;
        value = value * 10u + static_cast<u32>(text[i] - L'0');
    }
    return value;
}

bool name_selected(const char* name, u32 length, const char* filter, u32 filter_length) noexcept {
    if (!filter_length)
        return true;
    for (u32 start = 0; start + filter_length <= length; ++start)
        if (bytes_equal(name + start, filter, filter_length))
            return true;
    return false;
}

void sort(u64* values, u32 count) noexcept {
    for (u32 i = 1; i < count; ++i)
        for (u32 j = i; j > 0 && values[j - 1] > values[j]; --j) {
            const u64 swap = values[j];
            values[j] = values[j - 1];
            values[j - 1] = swap;
        }
}

int run() noexcept {
    wchar_t path[1024]{};
    const DWORD path_length = GetEnvironmentVariableW(L"MAICORD_DECODE_CORPUS", path, 1024);
    if (!path_length || path_length >= 1024)
        return 2;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return 3;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 512ll * 1024 * 1024) {
        CloseHandle(file);
        return 4;
    }
    u8* corpus = reserve(static_cast<usize>(size.QuadPart));
    DWORD got = 0;
    const bool loaded = corpus && ReadFile(file, corpus, static_cast<DWORD>(size.QuadPart), &got, nullptr) &&
                        got == size.QuadPart;
    CloseHandle(file);
    if (!loaded)
        return 5;
    char filter[64]{};
    const u32 filter_length = GetEnvironmentVariableA("MAICORD_DECODE_FILTER", filter, sizeof(filter));
    const u32 budget_ms = environment_number(L"MAICORD_DECODE_BUDGET_MS", 400);
    const u32 maximum_runs = minimum<u32>(environment_number(L"MAICORD_DECODE_RUNS", 64), 64);
    g_stages = environment_number(L"MAICORD_DECODE_STAGES", 0) != 0;

    g.scanline_bytes = kPngDecodeBufferMaximumBytes;
    g.scanlines = reserve(g.scanline_bytes);
    g.scratch_bytes = 192ull * 1024 * 1024;
    g.scratch = reserve(g.scratch_bytes);
    g.row = reserve(kJpegThumbnailDimensionMaximum * 4u);
    g.jpeg = reinterpret_cast<JpegWorkspace*>(reserve(sizeof(JpegWorkspace)));
    g.lossy = reinterpret_cast<LossyWebpWorkspace*>(reserve(sizeof(LossyWebpWorkspace)));
    g.lossless = reinterpret_cast<Vp8lWorkspace*>(reserve(sizeof(Vp8lWorkspace)));
    g.apng = reinterpret_cast<ApngWorkspace*>(reserve(sizeof(ApngWorkspace)));
    g.gif = reinterpret_cast<GifWorkspace*>(reserve(sizeof(GifWorkspace)));
    g.animated = reinterpret_cast<AnimatedWebpWorkspace*>(reserve(sizeof(AnimatedWebpWorkspace)));
    g.pcm = reinterpret_cast<i16*>(reserve(kOpusDecoderMaximumFrames * 2u * sizeof(i16)));
    g.pcm_float = reinterpret_cast<float*>(reserve(kMp3MaximumFrames * 2u * sizeof(float)));
    g.audio_state = reserve(1u << 20);
    g.ogg_scratch = reserve(kOggOpusMaximumPacketBytes);
    if (!g.scanlines || !g.scratch || !g.row || !g.jpeg || !g.lossy || !g.lossless || !g.apng || !g.gif ||
        !g.animated || !g.pcm || !g.pcm_float || !g.audio_state || !g.ogg_scratch)
        return 6;
    // Touch every buffer once so first-run page faults do not land in an item.
    for (usize i = 0; i < g.scanline_bytes; i += 4096)
        g.scanlines[i] = 0;
    for (usize i = 0; i < g.scratch_bytes; i += 4096)
        g.scratch[i] = 0;

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadAffinityMask(GetCurrentThread(), 1ull << 2);
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    // TSC ticks per microsecond, for the budget and the reported wall time.
    LARGE_INTEGER before{}, after{};
    QueryPerformanceCounter(&before);
    const u64 tsc_before = __rdtsc();
    Sleep(50);
    const u64 tsc_after = __rdtsc();
    QueryPerformanceCounter(&after);
    const u64 tsc_per_us = (tsc_after - tsc_before) * static_cast<u64>(frequency.QuadPart) /
                           (static_cast<u64>(after.QuadPart - before.QuadPart) * 1'000'000ull);
    Line header{};
    header.append("decode_header");
    header.field("tsc_mhz", tsc_per_us);
    header.write();

    usize cursor = 0;
    u32 failures = 0;
    while (cursor + 16 <= static_cast<usize>(size.QuadPart)) {
        u32 record[4];
        copy_bytes(record, corpus + cursor, 16);
        cursor += 16;
        const u32 kind = record[0], parameter = record[1], name_bytes = record[2], payload_bytes = record[3];
        if (name_bytes > 128 || cursor + name_bytes + payload_bytes > static_cast<usize>(size.QuadPart))
            return 7;
        const char* name = reinterpret_cast<const char*>(corpus + cursor);
        const ConstSlice<u8> encoded{corpus + cursor + name_bytes, payload_bytes};
        cursor += name_bytes + payload_bytes;
        if (!name_selected(name, name_bytes, filter, filter_length))
            continue;
        for (u64& stage : g_stage_minimum)
            stage = ~0ull;
        g_parse_minimum = ~0ull;
        const Outcome verified = decode(kind, parameter, encoded, true);
        u64 samples[64];
        u32 runs = 0;
        u64 total = 0;
        bool consistent = verified.ok;
        while (consistent && runs < maximum_runs && (runs < 3 || total < budget_ms * 1000ull * tsc_per_us)) {
            const u64 begin = __rdtsc();
            const Outcome timed = decode(kind, parameter, encoded, false);
            const u64 elapsed = __rdtsc() - begin;
            consistent = timed.ok && timed.pixels == verified.pixels;
            samples[runs++] = elapsed;
            total += elapsed;
        }
        sort(samples, runs);
        Line line{};
        line.append("decode name=");
        line.append(name, name_bytes);
        line.append(" kind=");
        line.append(kind_name(kind));
        line.field("ok", consistent ? 1 : 0);
        line.field("bytes", payload_bytes);
        line.field("frames", verified.frames);
        line.field("pixels", verified.pixels);
        line.field("runs", runs);
        line.field("min_cycles", runs ? samples[0] : 0);
        line.field("median_cycles", runs ? samples[runs / 2] : 0);
        line.field("min_us", runs ? samples[0] / maximum<u64>(tsc_per_us, 1) : 0);
        line.field("cycles_per_pixel_x100", runs && verified.pixels ? samples[0] * 100 / verified.pixels : 0);
        line.field("output_bytes", verified.output_bytes);
        line.field("scratch_bytes", verified.scratch_bytes);
        if (g_parse_minimum != ~0ull)
            line.field("parse_cycles", g_parse_minimum);
        line.hex("hash", verified.hash);
        line.write();
        if (g_stages && g_stage_minimum[3] != ~0ull) {
            Line stages{};
            stages.append("decode_stages name=");
            stages.append(name, name_bytes);
            stages.field("envelope", g_stage_minimum[0]);
            stages.field("control", g_stage_minimum[1]);
            stages.field("modes", g_stage_minimum[2]);
            stages.field("tokens", g_stage_minimum[3]);
            stages.field("alpha", g_stage_minimum[4]);
            stages.write();
        }
        failures += consistent ? 0 : 1;
    }
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        Line memory{};
        memory.append("decode_memory");
        memory.field("peak_working_set", counters.PeakWorkingSetSize);
        memory.field("peak_pagefile", counters.PeakPagefileUsage);
        memory.write();
    }
    return failures ? 8 : 0;
}

} // namespace
} // namespace maicord

extern "C" void maicord_media_decode_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
