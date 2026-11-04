#pragma once

// What the GPU golden tests share: reading a golden PNG (8-bit, non-interlaced, grayscale or RGBA, the formats
// tools/update_gpu_goldens.py writes) and comparing a render with it. A render matches when no channel is off by more
// than `maximum` and at most `outliers` pixels are off by more than `channel`: antialiased edges and gradients differ
// between GPUs by a few levels.

#include "maicord/core/heap.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/media/compression/deflate.hpp"

namespace maicord::png_golden {

inline u32 be32(const u8* bytes) noexcept {
    return (u32{bytes[0]} << 24) | (u32{bytes[1]} << 16) | (u32{bytes[2]} << 8) | bytes[3];
}

// Decodes `png`, which must be exactly width x height with `channels` 1 (grayscale) or 4 (RGBA), into `output`
// (width * height * channels bytes).
inline bool decode(ConstSlice<u8> png, u32 width, u32 height, u32 channels, u8* output) noexcept {
    static const u8 signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if ((channels != 1 && channels != 4) || png.count < 8 + 25 || !bytes_equal(png.data, signature, 8))
        return false;
    const u32 stride = width * channels;
    const u32 row = stride + 1;
    const u32 raw_bytes = row * height;
    u8* compressed = static_cast<u8*>(heap_allocate(png.count));
    u8* raw = static_cast<u8*>(heap_allocate(raw_bytes));
    DeflateWorkspace* workspace = deflate_workspace_allocate();
    bool ok = compressed && raw && workspace;
    u32 compressed_bytes = 0;
    bool header = false;
    for (u32 at = 8; ok && at + 12 <= png.count;) {
        const u32 length = be32(png.data + at);
        const u8* type = png.data + at + 4;
        const u8* body = png.data + at + 8;
        if (length > png.count - at - 12) {
            ok = false;
            break;
        }
        if (bytes_equal(type, "IHDR", 4)) {
            header = length == 13 && be32(body) == width && be32(body + 4) == height && body[8] == 8 &&
                     body[9] == (channels == 4 ? 6 : 0) && body[12] == 0;
            ok = header;
        } else if (bytes_equal(type, "IDAT", 4)) {
            copy_bytes(compressed + compressed_bytes, body, length);
            compressed_bytes += length;
        } else if (bytes_equal(type, "IEND", 4)) {
            break;
        }
        at += 12 + length;
    }
    if (ok && header) {
        const DeflateInputSpan input{compressed, compressed_bytes};
        ok = zlib_inflate({&input, 1}, {raw, raw_bytes}, raw_bytes, workspace).ok();
    } else {
        ok = false;
    }
    for (u32 y = 0; ok && y < height; ++y) {
        const u8 filter = raw[y * row];
        const u8* in = raw + y * row + 1;
        u8* out = output + y * stride;
        const u8* up = y ? out - stride : nullptr;
        for (u32 x = 0; x < stride; ++x) {
            const u32 left = x >= channels ? out[x - channels] : 0, above = up ? up[x] : 0,
                      corner = (up && x >= channels) ? up[x - channels] : 0;
            u32 predictor = 0;
            switch (filter) {
            case 0:
                break;
            case 1:
                predictor = left;
                break;
            case 2:
                predictor = above;
                break;
            case 3:
                predictor = (left + above) / 2;
                break;
            case 4: {
                const i32 estimate = static_cast<i32>(left) + static_cast<i32>(above) - static_cast<i32>(corner);
                auto distance = [&](u32 value) noexcept {
                    const i32 d = estimate - static_cast<i32>(value);
                    return d < 0 ? -d : d;
                };
                const i32 dl = distance(left), da = distance(above), dc = distance(corner);
                predictor = (dl <= da && dl <= dc) ? left : (da <= dc ? above : corner);
                break;
            }
            default:
                ok = false;
                break;
            }
            out[x] = static_cast<u8>(in[x] + predictor);
        }
    }
    if (compressed)
        heap_free(compressed);
    if (raw)
        heap_free(raw);
    deflate_workspace_free(workspace);
    return ok;
}

struct Tolerance {
    u32 channel;  // a channel difference above this counts the pixel as an outlier
    u32 maximum;  // no channel differs by more
    u32 outliers; // at most this many pixels are outliers
};

struct Difference {
    u32 maximum;          // the largest channel difference
    u32 over_tolerance;   // pixels with a channel difference above Tolerance::channel
    u32 first_x, first_y; // the first such pixel
};

inline Difference compare(const u8* actual, const u8* golden, u32 width, u32 height, u32 channels,
                          u32 channel_tolerance) noexcept {
    Difference result{};
    for (u32 pixel = 0; pixel < width * height; ++pixel) {
        u32 worst = 0;
        for (u32 channel = 0; channel < channels; ++channel) {
            const i32 d = static_cast<i32>(actual[pixel * channels + channel]) -
                          static_cast<i32>(golden[pixel * channels + channel]);
            const u32 magnitude = static_cast<u32>(d < 0 ? -d : d);
            worst = magnitude > worst ? magnitude : worst;
        }
        if (worst > result.maximum)
            result.maximum = worst;
        if (worst > channel_tolerance) {
            if (result.over_tolerance == 0) {
                result.first_x = pixel % width;
                result.first_y = pixel / width;
            }
            ++result.over_tolerance;
        }
    }
    return result;
}

inline bool within(const Difference& difference, const Tolerance& tolerance) noexcept {
    return difference.maximum <= tolerance.maximum && difference.over_tolerance <= tolerance.outliers;
}

// The mean square error per channel (0..255 units) of two images; the peak signal to noise ratio is
// 10 log10(255^2 / mse). Effects that run chaotic arithmetic (hash noise, long persistent chains) differ between
// compilers and GPUs by more than a few levels at some pixels, so they are compared by this and not pixel by pixel.
inline double mean_square_error(const u8* actual, const u8* golden, u32 count) noexcept {
    u64 sum = 0;
    for (u32 i = 0; i < count; ++i) {
        const i32 d = static_cast<i32>(actual[i]) - static_cast<i32>(golden[i]);
        sum += static_cast<u64>(d * d);
    }
    return count ? static_cast<double>(sum) / static_cast<double>(count) : 0.0;
}

// 10^(db / 10) for whole decibels, without a math library.
inline double decibels_to_ratio(u32 db) noexcept {
    static constexpr double kTenths[10] = {1.0,    1.25893, 1.58489, 1.99526, 2.51189,
                                           3.16228, 3.98107, 5.01187, 6.30957, 7.94328};
    double ratio = kTenths[db % 10];
    for (u32 decades = db / 10; decades; --decades)
        ratio *= 10.0;
    return ratio;
}

// The whole decibels of PSNR the mean square error reaches (capped at 99; an exact match reaches the cap).
inline u32 psnr_decibels(double mse) noexcept {
    u32 db = 0;
    while (db < 99 && mse * decibels_to_ratio(db + 1) <= 255.0 * 255.0)
        ++db;
    return db;
}

} // namespace maicord::png_golden
