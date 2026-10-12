"""Write the synthetic media corpus read by MaiCordMediaDecodeBenchmark.

Every image and sound is drawn from formulas and a fixed seed; nothing is
downloaded and nothing is committed. Sizes follow the production limits:
CDN stills at the proxy sizes the client requests (JPEG/WebP <= 512 px,
PNG/GIF <= 2048 px), local-file thumbnails at camera/screen sizes (4K JPEG,
large lossy WebP and PNG), long GIF/APNG/WebP animations, Ogg Opus and MP3.
PNGs come from a writer here (adaptive per-row filters, Adam7); JPEG, WebP,
GIF and APNG from Pillow; audio from PyAV (libopus, libmp3lame). Pillow, NumPy
and PyAV are development-only dependencies.

Record: u32 kind, u32 parameter, u32 name bytes, u32 payload bytes, name, payload.
Usage: python tools/generate_media_decode_benchmark_corpus.py OUTPUT [--quick]
"""
import io
import struct
import sys
import zlib

import numpy as np
from PIL import Image

KIND = {
    'png': 1, 'apng': 2, 'gif': 3, 'jpeg': 4, 'webp': 5, 'animated_webp': 6,
    'jpeg_thumbnail': 7, 'webp_thumbnail': 8, 'png_stream': 9, 'ogg_opus': 10, 'mp3': 11,
}
RNG = np.random.default_rng(20261011)


# ---- Synthetic content --------------------------------------------------------
def photo(width, height, seed, noise=5.0):
    """Smooth lighting, blobs, mid-frequency texture and sensor noise."""
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:height, 0:width].astype(np.float32)
    u, v = x / width, y / height
    image = np.empty((height, width, 3), np.float32)
    for c in range(3):
        base = 90 + 70 * np.sin(2.1 * u + 1.3 * v + c) + 40 * np.cos(3.3 * v - 1.7 * u + 2 * c)
        for _ in range(6):
            cx, cy, r = rng.random(), rng.random(), 0.05 + 0.25 * rng.random()
            base += (rng.random() - 0.4) * 120 * np.exp(-((u - cx) ** 2 + (v - cy) ** 2) / (r * r))
        base += 18 * np.sin(x * 0.21 + c) * np.sin(y * 0.17) + 9 * np.sin((x + 2 * y) * 0.9)
        image[..., c] = base
    image += rng.normal(0, noise, image.shape).astype(np.float32)
    return np.clip(image, 0, 255).astype(np.uint8)


def screenshot(width, height, seed):
    """Flat UI panels, gradients, anti-aliased glyph runs and icons."""
    rng = np.random.default_rng(seed)
    image = np.empty((height, width, 3), np.uint8)
    image[:] = (49, 51, 56)
    image[:, : width // 6] = (43, 45, 49)
    image[: height // 14] = (30, 31, 34)
    for row in range(height // 14 + 12, height - 30, 22):
        x = width // 6 + 60
        while x < width - 80:
            word = int(rng.integers(3, 11))
            for _ in range(word):
                gw, gh = int(rng.integers(5, 9)), int(rng.integers(8, 13))
                glyph = (rng.random((gh, gw)) > 0.55).astype(np.uint8) * 210
                glyph = np.maximum(glyph, np.roll(glyph, 1, 1) // 2)
                region = image[row:row + gh, x:x + gw]
                region[...] = np.maximum(region, glyph[: region.shape[0], : region.shape[1], None])
                x += gw + 1
            x += 7
            if rng.random() < 0.08:
                break
    for _ in range(14):
        cx, cy = int(rng.integers(0, width - 64)), int(rng.integers(0, height - 64))
        yy, xx = np.mgrid[0:48, 0:48]
        mask = ((xx - 24) ** 2 + (yy - 24) ** 2) < 22 * 22
        color = rng.integers(40, 255, 3).astype(np.uint8)
        image[cy:cy + 48, cx:cx + 48][mask] = color
    gradient = np.linspace(0, 1, width, dtype=np.float32)[None, :, None]
    band = slice(height - 30, height)
    image[band] = (image[band] * (1 - gradient * 0.5) + np.array([88, 101, 242]) * gradient * 0.5).astype(np.uint8)
    return image


def with_alpha(rgb, seed):
    height, width, _ = rgb.shape
    y, x = np.mgrid[0:height, 0:width].astype(np.float32)
    alpha = 255 * np.clip(1.4 - np.hypot(x / width - 0.5, y / height - 0.5) * 2.2, 0, 1)
    alpha[(x.astype(int) // 37 + y.astype(int) // 41) % 5 == 0] = 0
    return np.dstack([rgb, alpha.astype(np.uint8)])


def animation_frames(width, height, count, seed, alpha=False):
    rng = np.random.default_rng(seed)
    # Upscaled content, like video-derived GIFs and stickers: smooth areas with
    # moving hard-edged sprites, so LZW/deflate see realistic string lengths.
    small = photo(max(8, width // 4), max(8, height // 4), seed, noise=2.0)
    background = np.array(Image.fromarray(small).resize((width, height), Image.Resampling.BILINEAR))
    sprites = [(rng.random() * width, rng.random() * height, rng.random() * 6 - 3, rng.random() * 6 - 3,
                rng.integers(30, 255, 3), 10 + rng.random() * width / 8) for _ in range(5)]
    y, x = np.mgrid[0:height, 0:width].astype(np.float32)
    frames = []
    for index in range(count):
        frame = background.copy()
        # Global drift keeps most pixels changing, like video-derived GIFs.
        frame = np.roll(frame, index % 7 - 3, axis=1)
        for sx, sy, dx, dy, color, radius in sprites:
            cx, cy = (sx + dx * index * 3) % width, (sy + dy * index * 3) % height
            frame[(x - cx) ** 2 + (y - cy) ** 2 < radius * radius] = color
        frames.append(with_alpha(frame, seed + index) if alpha else frame)
    return frames


# ---- PNG writer ----------------------------------------------------------------
def png_chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)


def filter_rows(rows, bpp):
    """libpng-style adaptive filtering: per row the filter with the least sum of |residual|."""
    out = bytearray()
    previous = np.zeros(rows.shape[1], np.int16)
    for row in rows.astype(np.int16):
        left = np.concatenate([np.zeros(bpp, np.int16), row[:-bpp]])
        upper_left = np.concatenate([np.zeros(bpp, np.int16), previous[:-bpp]])
        p = left + previous - upper_left
        pa, pb, pc = np.abs(p - left), np.abs(p - previous), np.abs(p - upper_left)
        paeth = np.where((pa <= pb) & (pa <= pc), left, np.where(pb <= pc, previous, upper_left))
        candidates = [row, row - left, row - previous, row - ((left + previous) >> 1), row - paeth]
        residuals = [(c & 255).astype(np.uint8) for c in candidates]
        costs = [np.abs(r.astype(np.int8).astype(np.int32)).sum() for r in residuals]
        kind = int(np.argmin(costs))
        out.append(kind)
        out += residuals[kind].tobytes()
        previous = row
    return bytes(out)


def png_bytes(pixels, color_type, depth=8, palette=None, interlace=False, idat=8192):
    height, width = pixels.shape[:2]
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    bpp = max(1, channels * depth // 8)

    def pack(block):
        if depth == 8:
            return block.reshape(block.shape[0], -1)
        per = 8 // depth
        padded = np.zeros((block.shape[0], -(-block.shape[1] // per) * per), np.uint8)
        padded[:, : block.shape[1]] = block
        packed = np.zeros((block.shape[0], padded.shape[1] // per), np.uint8)
        for i in range(per):
            packed |= (padded[:, i::per] << (8 - depth * (i + 1))).astype(np.uint8)
        return packed

    if interlace:
        raw = b''
        for x0, y0, dx, dy in ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)):
            sub = pixels[y0::dy, x0::dx]
            if sub.size:
                raw += filter_rows(pack(sub), bpp)
    else:
        raw = filter_rows(pack(pixels), bpp)
    body = b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, depth, color_type, 0, 0, int(interlace)))
    if palette is not None:
        body += png_chunk(b'PLTE', palette[:, :3].astype(np.uint8).tobytes())
        if palette.shape[1] == 4:
            body += png_chunk(b'tRNS', palette[:, 3].astype(np.uint8).tobytes())
    compressed = zlib.compress(raw, 6)
    for start in range(0, len(compressed), idat):
        body += png_chunk(b'IDAT', compressed[start:start + idat])
    return body + png_chunk(b'IEND', b'')


def quantized(rgb, colors):
    image = Image.fromarray(rgb).quantize(colors, dither=Image.Dither.NONE)
    palette = np.array(image.getpalette()[: colors * 3], np.uint8).reshape(-1, 3)
    indices = np.array(image)
    return indices, palette[: int(indices.max()) + 1]


# ---- Encoders through Pillow / PyAV ----------------------------------------------
def pillow(image, fmt, **options):
    out = io.BytesIO()
    image.save(out, fmt, **options)
    return out.getvalue()


def audio_signal(seconds, rate, channels, voice):
    t = np.arange(int(seconds * rate)) / rate
    if voice:
        pitch = 140 + 30 * np.sin(2 * np.pi * 0.7 * t)
        phase = 2 * np.pi * np.cumsum(pitch) / rate
        envelope = (np.sin(2 * np.pi * 3.1 * t) > -0.3) * (0.6 + 0.4 * np.sin(2 * np.pi * 0.37 * t))
        mono = envelope * sum(np.sin(k * phase) / k for k in range(1, 12)) * 0.25
        mono += RNG.normal(0, 0.01, t.shape)
        signal = mono[:, None]
    else:
        chord = sum(np.sin(2 * np.pi * f * t) for f in (220.0, 277.18, 329.63, 440.0, 659.25)) * 0.12
        beat = np.exp(-((t * 2) % 1) * 9) * np.sin(2 * np.pi * 60 * t) * 0.4
        hats = RNG.normal(0, 1, t.shape) * np.exp(-((t * 8) % 1) * 30) * 0.08
        left, right = chord + beat + hats, chord * 0.8 + beat + np.roll(hats, 37)
        signal = np.stack([left, right], 1)[:, :channels]
    return np.clip(signal * 32767, -32768, 32767).astype(np.int16)


def encode_audio(codec, container, rate, channels, bit_rate, seconds, voice):
    import av
    out = io.BytesIO()
    with av.open(out, 'w', format=container) as output:
        stream = output.add_stream(codec, rate=rate)
        stream.bit_rate = bit_rate
        stream.layout = 'stereo' if channels == 2 else 'mono'
        if codec == 'libopus':
            stream.options = {'application': 'voip' if voice else 'audio'}
        pcm = audio_signal(seconds, rate, channels, voice)
        frame_size = stream.codec_context.frame_size or 960
        for start in range(0, len(pcm), frame_size):
            chunk = np.ascontiguousarray(pcm[start:start + frame_size].T.reshape(1, -1)) if channels == 2 else \
                np.ascontiguousarray(pcm[start:start + frame_size, 0][None, :])
            frame = av.AudioFrame.from_ndarray(chunk, format='s16', layout=stream.layout.name)
            frame.sample_rate = rate
            for packet in stream.encode(frame):
                output.mux(packet)
        for packet in stream.encode(None):
            output.mux(packet)
    return out.getvalue()


# ---- Corpus --------------------------------------------------------------------
def entries(quick):
    scale = 2 if quick else 1
    shot = screenshot(1920 // scale, 1080 // scale, 1)
    picture = photo(1024 // scale, 1024 // scale, 2)
    yield 'png', 'png_screenshot_rgb_1920x1080', 0, png_bytes(shot, 2)
    yield 'png', 'png_photo_rgba_1024', 0, png_bytes(with_alpha(picture, 3), 6)
    indices, palette = quantized(shot, 64)
    yield 'png', 'png_palette8_1920x1080', 0, png_bytes(indices, 3, 8, palette)
    small_indices, small_palette = quantized(photo(800 // scale, 600 // scale, 4), 16)
    yield 'png', 'png_palette4_800x600', 0, png_bytes(small_indices, 3, 4, small_palette)
    yield 'png', 'png_gray8_1024', 0, png_bytes(picture[..., 1], 0)
    yield 'png', 'png_adam7_rgba_512', 0, png_bytes(with_alpha(photo(512, 512, 5), 6), 6, interlace=True)
    emoji = with_alpha(photo(128, 128, 7), 8)
    yield 'png', 'png_emoji_rgba_128', 0, png_bytes(emoji, 6)

    apng = animation_frames(256, 256, 48 // scale, 9, alpha=True)
    yield 'apng', 'apng_rgba_256x48', 0, pillow(Image.fromarray(apng[0]), 'PNG', save_all=True,
                                                   append_images=[Image.fromarray(f) for f in apng[1:]], duration=40, loop=0)
    gif = animation_frames(480, 270, 120 // scale, 10)
    yield 'gif', 'gif_480x270x120', 0, pillow(Image.fromarray(gif[0]).quantize(255), 'GIF', save_all=True,
                                                append_images=[Image.fromarray(f).quantize(255) for f in gif[1:]],
                                                duration=40, loop=0, optimize=False)
    big = animation_frames(1024 // scale, 768 // scale, 16, 11)
    yield 'gif', 'gif_1024x768x16', 0, pillow(Image.fromarray(big[0]).quantize(255), 'GIF', save_all=True,
                                                append_images=[Image.fromarray(f).quantize(255) for f in big[1:]],
                                                duration=60, loop=0, optimize=False)
    emoji_gif = animation_frames(96, 96, 32, 12)
    yield 'gif', 'gif_emoji_96x32', 0, pillow(Image.fromarray(emoji_gif[0]).quantize(128), 'GIF', save_all=True,
                                                append_images=[Image.fromarray(f).quantize(128) for f in emoji_gif[1:]],
                                                duration=50, loop=0)

    cdn = photo(512, 512, 13)
    yield 'jpeg', 'jpeg_baseline420_512', 0, pillow(Image.fromarray(cdn), 'JPEG', quality=85, subsampling=2)
    yield 'jpeg', 'jpeg_baseline444_512x384', 0, pillow(Image.fromarray(cdn[:384]), 'JPEG', quality=92, subsampling=0)
    yield 'jpeg', 'jpeg_progressive420_512', 0, pillow(Image.fromarray(cdn), 'JPEG', quality=85, subsampling=2,
                                                        progressive=True)
    yield 'jpeg', 'jpeg_gray_512', 0, pillow(Image.fromarray(cdn[..., 0]), 'JPEG', quality=85)
    yield 'jpeg', 'jpeg_restart420_512', 0, pillow(Image.fromarray(cdn), 'JPEG', quality=85, subsampling=2,
                                                   restart_marker_blocks=4)
    camera = photo(3840 // scale, 2160 // scale, 14)
    yield 'jpeg_thumbnail', 'jpeg_thumb_baseline420_3840x2160', 400, pillow(Image.fromarray(camera), 'JPEG',
                                                                              quality=90, subsampling=2)
    yield 'jpeg_thumbnail', 'jpeg_thumb_progressive420_3840x2160', 400, pillow(
        Image.fromarray(camera), 'JPEG', quality=85, subsampling=2, progressive=True)
    medium = photo(1600 // scale, 1200 // scale, 15)
    yield 'jpeg_thumbnail', 'jpeg_thumb_baseline420_1600x1200', 400, pillow(Image.fromarray(medium), 'JPEG',
                                                                              quality=88, subsampling=2)

    yield 'webp', 'webp_lossy_512', 0, pillow(Image.fromarray(cdn), 'WEBP', quality=80, method=4)
    yield 'webp', 'webp_lossy_alpha_512', 0, pillow(Image.fromarray(with_alpha(cdn, 16)), 'WEBP', quality=80,
                                                    method=4, alpha_quality=100)
    yield 'webp', 'webp_lossless_photo_512', 0, pillow(Image.fromarray(cdn), 'WEBP', lossless=True, quality=60)
    yield 'webp', 'webp_lossless_ui_512', 0, pillow(Image.fromarray(with_alpha(shot[:512, 300:812], 17)), 'WEBP',
                                                    lossless=True, quality=60)
    lossy = animation_frames(320, 320, 60 // scale, 18)
    yield 'animated_webp', 'awebp_lossy_320x60', 0, pillow(Image.fromarray(lossy[0]), 'WEBP', save_all=True,
                                                           append_images=[Image.fromarray(f) for f in lossy[1:]],
                                                           duration=40, loop=0, quality=75, minimize_size=False,
                                                           allow_mixed=False)
    lossless = animation_frames(256, 256, 40 // scale, 19, alpha=True)
    yield 'animated_webp', 'awebp_lossless_256x40', 0, pillow(Image.fromarray(lossless[0]), 'WEBP', save_all=True,
                                                              append_images=[Image.fromarray(f) for f in lossless[1:]],
                                                              duration=40, loop=0, lossless=True, quality=50,
                                                              minimize_size=False)
    yield 'webp_thumbnail', 'webp_thumb_lossy_2048x1536', 400, pillow(Image.fromarray(photo(2048 // scale, 1536 // scale, 20)),
                                                                      'WEBP', quality=80, method=4)
    yield 'png_stream', 'png_stream_screenshot_3840x2160', 400, png_bytes(screenshot(3840 // scale, 2160 // scale, 21), 2)

    seconds = 4 if quick else 20
    yield 'ogg_opus', 'opus_music_stereo_128k', 0, encode_audio('libopus', 'ogg', 48000, 2, 128000, seconds, False)
    yield 'ogg_opus', 'opus_voice_mono_24k', 0, encode_audio('libopus', 'ogg', 48000, 1, 24000, seconds, True)
    yield 'mp3', 'mp3_music_stereo_192k', 0, encode_audio('libmp3lame', 'mp3', 44100, 2, 192000, seconds, False)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    quick = '--quick' in sys.argv[2:]
    with open(sys.argv[1], 'wb') as output:
        for kind, name, parameter, payload in entries(quick):
            encoded = name.encode('ascii')
            output.write(struct.pack('<IIII', KIND[kind], parameter, len(encoded), len(payload)))
            output.write(encoded)
            output.write(payload)
            print(f'{name}: {len(payload)} bytes', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
