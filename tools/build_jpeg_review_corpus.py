"""Build a reproducible, public-image JPEG corpus for the native GPU probe.

Pillow/NumPy are development-only. Corpus stays outside the shipping tree.
Source URLs and encoded hashes are recorded in manifest.json.
"""
import concurrent.futures
import hashlib
import json
from pathlib import Path
import sys
import urllib.request
import numpy as np
from PIL import Image, ImageDraw, ImageCms

root = Path(sys.argv[1]); root.mkdir(parents=True, exist_ok=True)
sources = root / 'sources'; sources.mkdir(exist_ok=True)
urls = [f'https://r0k.us/graphics/kodak/kodak/kodim{i:02d}.png' for i in range(1, 25)]
urls += ['https://raw.githubusercontent.com/libjpeg-turbo/libjpeg-turbo/main/testimages/' + n
         for n in ['testorig.png', 'shira_bird8.bmp', 'vgl_5674_0098.png', 'vgl_6434_0018a.bmp', 'vgl_6548_0026a.bmp']]

def download(url):
    path = sources / url.rsplit('/', 1)[-1]
    if not path.exists():
        request = urllib.request.Request(url, headers={'User-Agent': 'MaiCord-JPEG-validation'})
        path.write_bytes(urllib.request.urlopen(request, timeout=40).read())
    return path, url

with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
    downloaded = list(pool.map(download, urls))

manifest = []
def save(image, label, quality, subsampling, progressive, **kw):
    name = f'{len(manifest):04d}_{label}_q{quality}_s{subsampling}_p{int(progressive)}.jpg'
    path = root / name
    image.save(path, quality=quality, subsampling=subsampling, progressive=progressive, **kw)
    manifest.append(dict(file=name, width=image.width, height=image.height, mode=image.mode,
        quality=quality, subsampling=subsampling, progressive=progressive,
        bytes=path.stat().st_size, sha256=hashlib.sha256(path.read_bytes()).hexdigest()))

for i, (path, url) in enumerate(downloaded):
    image = Image.open(path).convert('RGB'); image.thumbnail((512, 512))
    for quality in [5, 25, 50, 75, 95, 100]:
        for subsampling in [0, 1, 2]:
            save(image, path.stem, quality, subsampling, bool((quality + subsampling + i) % 2))
    # Grayscale and CMYK exercise independent component layouts.
    for mode in ['L', 'CMYK']:
        save(image.convert(mode), path.stem + '_' + mode, 90, 0, False)

rng = np.random.default_rng(927)
for width, height in [(1, 1), (7, 13), (8, 8), (17, 31), (127, 65), (256, 256), (511, 509), (512, 512)]:
    y, x = np.indices((height, width))
    variants = {
        'noise': rng.integers(0, 256, (height, width, 3), dtype=np.uint8),
        'checker': np.repeat((((x + y) % 2) * 255)[..., None], 3, axis=2).astype('uint8'),
        'gradient': np.stack([x * 255 // max(width-1, 1), y * 255 // max(height-1, 1),
                              (x+y) * 255 // max(width+height-2, 1)], axis=2).astype('uint8'),
        'flat': np.full((height, width, 3), 127, dtype='uint8'),
    }
    for label, pixels in variants.items():
        for quality in [5, 50, 95, 100]:
            save(Image.fromarray(pixels), f'{label}_{width}x{height}', quality,
                 quality % 3, bool(quality % 2))

image = Image.open(downloaded[0][0]).convert('RGB'); image.thumbnail((511, 509))
profile = ImageCms.ImageCmsProfile(ImageCms.createProfile('sRGB')).tobytes()
for orientation in range(1, 9):
    exif = Image.Exif(); exif[274] = orientation
    save(image, f'exif{orientation}_icc', 95, 2, bool(orientation % 2),
         exif=exif, icc_profile=profile)
for restart in [1, 3, 7]:
    save(image, f'restart{restart}', 85, 2, False, restart_marker_blocks=restart)

(root / 'manifest.json').write_text(json.dumps({'sources': [dict(file=p.name, url=u,
    sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p, u in downloaded], 'images': manifest}, indent=2))
print(f'{len(manifest)} JPEGs, {sum(x["width"]*x["height"] for x in manifest):,} source pixels')
