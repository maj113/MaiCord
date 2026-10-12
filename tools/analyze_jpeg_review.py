"""Summarize repeated native JPEG A/B runs and render the worst-case comparison."""
import csv
import json
import math
from pathlib import Path
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

root = Path(sys.argv[1])
runs = [list(csv.DictReader(path.open())) for path in sorted(root.glob('gpu-results-run*.csv'))]
assert runs and runs[0]
reference = runs[0]
metrics = ['file', 'width', 'height', 'max_delta', 'changed_pixels', 'changed_channels',
           'abs_sum', 'squared_sum', 'idct_max_delta', 'idct_changed', 'error']
assert all(len(run) == len(reference) for run in runs)
assert all(all(a[key] == b[key] for key in metrics) for run in runs[1:] for a, b in zip(reference, run))
assert all(int(row['error']) == 0 for row in reference)
pixels = sum(int(row['width']) * int(row['height']) for row in reference)
changed = sum(int(row['changed_pixels']) for row in reference)
squared = sum(int(row['squared_sum']) for row in reference)
summary = dict(images=len(reference), pixels=pixels,
    changed_images=sum(int(row['changed_pixels']) > 0 for row in reference),
    changed_pixels=changed, changed_pixel_percent=100 * changed / pixels,
    max_rgba_delta=max(int(row['max_delta']) for row in reference),
    max_idct_delta=max(int(row['idct_max_delta']) for row in reference),
    rmse=math.sqrt(squared/(pixels*3)),
    psnr_db=10*math.log10(255**2/(squared/(pixels*3))) if squared else None, runs=[])
for i, run in enumerate(runs, 1):
    result = {'run': i}
    for stage in ['idct', 'upload']:
        old = sum(int(row[stage+'_ref_ns']) for row in run)
        new = sum(int(row[stage+'_candidate_ns']) for row in run)
        result[stage] = dict(original_ms=old/1e6, candidate_ms=new/1e6, saved_percent=100*(1-new/old))
    summary['runs'].append(result)
(root/'summary.json').write_text(json.dumps(summary, indent=2))

name, width, height = (root/'worst.txt').read_text().split(',')
width, height = int(width), int(height)
old = np.frombuffer((root/'worst.reference.rgba').read_bytes(), dtype=np.uint8).reshape(height, width, 4)
new = np.frombuffer((root/'worst.candidate.rgba').read_bytes(), dtype=np.uint8).reshape(height, width, 4)
delta = np.abs(old.astype('int16')-new.astype('int16'))[:, :, :3]
canvas = Image.new('RGB', (width*3+40, height+95), '#15171b'); draw = ImageDraw.Draw(canvas)
font = ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf', 18)
images = [('Original', Image.fromarray(old)), ('Separable IDCT', Image.fromarray(new)),
          ('Absolute difference x64', Image.fromarray(np.minimum(delta*64, 255).astype('uint8')))]
for i, (label, image) in enumerate(images):
    canvas.paste(image.convert('RGB'), (10+i*(width+10), 42))
    draw.text((10+i*(width+10), 10), label, font=font, fill='white')
draw.text((10, height+56), name+' | maximum channel change: '+str(delta.max())+'/255', font=font, fill='white')
canvas.save(root/'worst-comparison.png')
print(json.dumps(summary, indent=2))
