#!/usr/bin/env python3
"""Writes the Lottie documents tests/render/effects/lottie_program_test.cpp compiles (tests/render/effects/corpus).

The documents are drawn here, from seeded random curves, because the stickers the client shows belong to their authors
and cannot ship. They use what the sticker files in use do, in the proportions they do: closed smooth paths filled and
stroked with round caps and joins, merge paths that intersect, groups under animated transforms, alpha and luma mattes,
layer masks, precompositions, rounded rectangles and ellipses, repeaters, gradients and keyframed shape morphs.

    python tools/generate_lottie_corpus.py [directory]

The output is deterministic (no timestamps, a fixed generator); rerun it only to change the documents, then renew
tests/render/effects/golden/lottie_program.bin with Direct2D (see the test's header).
"""
import json
import math
import os
import random
import sys


def prop(value):
    return {"a": 0, "k": value}


def keyframes(times_values, easing=True):
    """times_values: [(frame, value), ...]; the last keyframe carries no end."""
    frames = []
    for index, (time, value) in enumerate(times_values):
        frame = {"t": time, "s": value}
        if index + 1 < len(times_values):
            frame["e"] = times_values[index + 1][1]
            if easing:
                frame["i"] = {"x": [0.4], "y": [1]}
                frame["o"] = {"x": [0.6], "y": [0]}
        frames.append(frame)
    return {"a": 1, "k": frames}


def transform(position=(0, 0), anchor=(0, 0), scale=(100, 100), rotation=0, opacity=100):
    def wrap(value):
        return value if isinstance(value, dict) and "k" in value else prop(value)

    return {"p": wrap(list(position) if not isinstance(position, dict) else position),
            "a": wrap(list(anchor)), "s": wrap(list(scale) if not isinstance(scale, dict) else scale),
            "r": wrap(rotation), "o": wrap(opacity)}


def group(items, **kwargs):
    return {"ty": "gr", "it": items + [dict(ty="tr", **transform(**kwargs))]}


def shape_value(vertices, ins, outs, closed=True):
    return {"c": closed, "v": [list(v) for v in vertices], "i": [list(v) for v in ins], "o": [list(v) for v in outs]}


def smooth_path(points, closed=True, tension=1 / 6):
    """A path through points with Catmull-Rom handles, as Lottie stores one: tangents relative to the vertex."""
    count = len(points)
    ins, outs = [], []
    for index in range(count):
        previous = points[(index - 1) % count] if closed or index else points[index]
        following = points[(index + 1) % count] if closed or index + 1 < count else points[index]
        tangent = ((following[0] - previous[0]) * tension, (following[1] - previous[1]) * tension)
        ins.append((-tangent[0], -tangent[1]))
        outs.append(tangent)
    return shape_value(points, ins, outs, closed)


def blob_points(rng, cx, cy, low, high, count):
    return [(round(cx + rng.uniform(low, high) * math.cos(2 * math.pi * i / count), 2),
             round(cy + rng.uniform(low, high) * math.sin(2 * math.pi * i / count), 2)) for i in range(count)]


def path(value):
    return {"ty": "sh", "ks": prop(value) if not (isinstance(value, dict) and "a" in value) else value}


def morph(values_at_times):
    frames = []
    for index, (time, value) in enumerate(values_at_times):
        frame = {"t": time, "s": [value]}
        if index + 1 < len(values_at_times):
            frame["e"] = [values_at_times[index + 1][1]]
            frame["i"] = {"x": [0.4], "y": [1]}
            frame["o"] = {"x": [0.6], "y": [0]}
        frames.append(frame)
    return {"a": 1, "k": frames}


def fill(color, opacity=100):
    return {"ty": "fl", "c": prop(list(color) + [1]), "o": prop(opacity)}


def stroke(color, width, cap=2, join=2, miter=4, opacity=100):
    return {"ty": "st", "c": prop(list(color) + [1]), "o": prop(opacity), "w": prop(width), "lc": cap, "lj": join,
            "ml": miter}


def gradient_fill(start, end, stops, kind=1, opacity=100):
    flat = []
    for position, (r, g, b) in stops:
        flat += [position, r, g, b]
    return {"ty": "gf", "o": prop(opacity), "s": prop(list(start)), "e": prop(list(end)), "t": kind,
            "g": {"p": len(stops), "k": prop(flat)}}


def ellipse(center, size):
    return {"ty": "el", "p": prop(list(center)), "s": prop(list(size))}


def rectangle(center, size, radius=0):
    return {"ty": "rc", "p": prop(list(center)), "s": prop(list(size)), "r": prop(radius)}


def merge(mode):
    return {"ty": "mm", "mm": mode}


def repeater(copies, step):
    return {"ty": "rp", "c": prop(copies), "o": prop(0),
            "tr": {"p": prop(list(step)), "a": prop([0, 0]), "s": prop([100, 100]), "r": prop(0),
                   "so": prop(100), "eo": prop(30)}}


def layer(shapes, index, ip=0, op=60, ks=None, **extra):
    document = {"ty": 4, "ind": index, "ip": ip, "op": op, "st": 0, "ks": ks or transform(), "shapes": shapes}
    document.update(extra)
    return document


def solid(index, width, height, color, ks=None, **extra):
    document = {"ty": 1, "ind": index, "ip": 0, "op": 60, "st": 0, "sw": width, "sh": height, "sc": color,
                "ks": ks or transform()}
    document.update(extra)
    return document


def mask(points_value, mode="a", inverted=False):
    return {"mode": mode, "inv": inverted, "pt": prop(points_value), "o": prop(100)}


def document(layers, width=512, height=512, frames=60, assets=None):
    root = {"v": "5.7.4", "fr": 30, "ip": 0, "op": frames, "w": width, "h": height, "nm": "corpus", "ddd": 0,
            "layers": layers}
    if assets:
        root["assets"] = assets
    return root


PALETTE = [(0.95, 0.35, 0.3), (0.98, 0.78, 0.25), (0.3, 0.7, 0.45), (0.25, 0.5, 0.9), (0.6, 0.4, 0.85),
           (0.12, 0.12, 0.15), (0.98, 0.95, 0.92)]


def doc_blobs():
    rng = random.Random(101)
    layers = []
    for index in range(5):
        cx, cy = 100 + 80 * index, 150 + 50 * (index % 3)
        a = smooth_path(blob_points(rng, 0, 0, 40, 90, 8))
        b = smooth_path(blob_points(rng, 0, 0, 40, 90, 8))
        items = [group([path(a if index % 2 else {"a": 1, "k": morph([(0, a), (30, b), (59, a)])["k"]}),
                        fill(PALETTE[index % 5]), stroke(PALETTE[5], 7 + index * 2)],
                       position=keyframes([(0, [cx, cy]), (30, [cx + 30, cy - 20]), (59, [cx, cy])]),
                       rotation=keyframes([(0, 0), (59, 40 * (index - 2))]))]
        layers.append(layer(items, index + 1))
    return document(layers[::-1])


def doc_merges():
    rng = random.Random(202)
    layers = []
    for index, mode in enumerate([1, 2, 3, 4, 5]):
        cx, cy = 90 + 85 * index, 256
        first = smooth_path(blob_points(rng, 0, 0, 55, 90, 7))
        second = smooth_path(blob_points(rng, 20, 10, 45, 80, 9))
        items = [group([path(first), path(second), ellipse((-10, 30), (70, 50)), merge(mode),
                        fill(PALETTE[index]), stroke(PALETTE[5], 5)], position=[cx, cy])]
        layers.append(layer(items, index + 1))
    # Merges of curves that touch: shared vertices and a shared edge.
    edge_a = shape_value([(0, 0), (100, 0), (100, 100), (0, 100)], [(0, 0)] * 4, [(0, 0)] * 4)
    edge_b = shape_value([(100, 0), (200, 0), (200, 100), (100, 100)], [(0, 0)] * 4, [(0, 0)] * 4)
    layers.append(layer([group([path(edge_a), path(edge_b), merge(1), fill(PALETTE[2])], position=[100, 380])], 6))
    layers.append(layer([group([path(edge_a), path(edge_a), merge(5), fill(PALETTE[3])], position=[300, 380])], 7))
    layers.append(layer([group([rectangle((50, 50), (100, 100), 25), ellipse((50, 50), (100, 100)), merge(4),
                                fill(PALETTE[4])], position=[420, 380])], 8))
    return document(layers[::-1], frames=30)


def stripes(count, color_index):
    return [group([rectangle((256, 40 + 55 * i), (512, 24)), fill(PALETTE[(i + color_index) % 5])]) for i in range(count)]


def doc_mattes():
    rng = random.Random(303)
    layers = []
    index = 1
    for matte_type in (1, 2, 3, 4):
        cx, cy = 128 + 256 * ((matte_type - 1) % 2), 128 + 256 * ((matte_type - 1) // 2)
        source_shape = smooth_path(blob_points(rng, 0, 0, 50, 100, 8))
        source = layer([group([path(source_shape), fill((1, 1, 1))], position=[cx, cy],
                              rotation=keyframes([(0, 0), (59, 90)]))], index, td=1)
        content = layer([group(stripes(5, matte_type) + [stroke(PALETTE[5], 6)])], index + 1, tt=matte_type)
        # The source sits above the layer it shapes (the earlier entry of the file's list).
        layers.append(content)
        layers.append(source)
        index += 2
    return document(layers[::-1])


def doc_masks():
    rng = random.Random(404)
    layers = []
    modes = [("a", False), ("s", False), ("i", False), ("a", True)]
    for index, (mode, inverted) in enumerate(modes):
        cx, cy = 128 + 256 * (index % 2), 128 + 256 * (index // 2)
        hole = smooth_path(blob_points(rng, cx, cy, 40, 80, 7))
        second = smooth_path(blob_points(rng, cx + 30, cy + 20, 40, 70, 6))
        body = [group([path(smooth_path(blob_points(rng, 0, 0, 90, 115, 9))), fill(PALETTE[index]),
                       stroke(PALETTE[5], 8)], position=[cx, cy])]
        masks = [mask(hole, mode, inverted)]
        if index == 3:
            masks.append(mask(second, "s"))
        layers.append(layer(body, index + 1, hasMask=True, masksProperties=masks))
    layers.append(solid(5, 200, 100, "#4080ff", ks=transform(position=[156, 12])))
    return document(layers[::-1], frames=30)


def doc_strokes():
    layers = []
    index = 1
    for cap in (1, 2, 3):
        for join in (1, 2, 3):
            cx, cy = 90 + 170 * (join - 1), 90 + 170 * (cap - 1)
            points = [(-50, 40), (-10, -50), (20, 30), (60, -40)]
            open_path = smooth_path(points, closed=False)
            sharp = shape_value([(-50, 50), (50, 0), (-50, -50)], [(0, 0)] * 3, [(0, 0)] * 3, closed=False)
            layers.append(layer([group([path(open_path), stroke(PALETTE[(cap + join) % 5], 14, cap, join)],
                                       position=[cx, cy - 20]),
                                 group([path(sharp), stroke(PALETTE[5], 10 + 4 * join, cap, join, miter=2.5)],
                                       position=[cx, cy + 50], scale=[70, 70])], index))
            index += 1
    return document(layers[::-1], frames=20)


def doc_nested():
    rng = random.Random(606)
    inner_layers = [layer([group([rectangle((0, 0), (120, 80), 20), fill(PALETTE[1]), stroke(PALETTE[5], 6)],
                                 rotation=keyframes([(0, 0), (59, 180)])),
                           group([ellipse((0, 0), (60, 40)), fill(PALETTE[0])], position=[30, 20])], 1)]
    assets = [{"id": "comp_0", "layers": inner_layers}]
    layers = [{"ty": 0, "ind": 1, "refId": "comp_0", "ip": 0, "op": 60, "st": 0, "sr": 1, "w": 256, "h": 256,
               "ks": transform(position=[130, 130], scale=[120, 120], rotation=15)},
              {"ty": 0, "ind": 2, "refId": "comp_0", "ip": 0, "op": 60, "st": 10, "sr": 1.5, "w": 256, "h": 256,
               "ks": transform(position=[380, 330], scale=[80, 80], rotation=-20)},
              layer([group([ellipse((0, 0), (80, 80)), repeater(4, (50, 10)), fill(PALETTE[3], 80),
                            stroke(PALETTE[5], 4)], position=[100, 400])], 3),
              layer([group([path(smooth_path(blob_points(rng, 0, 0, 30, 55, 7))), gradient_fill(
                  (-50, 0), (50, 0), [(0, PALETTE[0]), (1, PALETTE[4])]), stroke(PALETTE[5], 5)],
                            position=[400, 100])], 4),
              layer([group([path(smooth_path(blob_points(rng, 0, 0, 30, 55, 7))), gradient_fill(
                  (0, 0), (60, 0), [(0, PALETTE[1]), (1, PALETTE[2])], kind=2)], position=[256, 256])], 5,
                    ks=transform(position=[0, 0])),
              # A child layer follows its parent's animated transform; a hidden layer and a null stay out.
              {"ty": 3, "ind": 6, "ip": 0, "op": 60, "st": 0, "ks": transform(
                  position=keyframes([(0, [0, 0]), (59, [60, 40])]))},
              layer([group([rectangle((256, 60), (140, 50), 12), fill(PALETTE[2])])], 7, parent=6)]
    return document(layers[::-1], assets=assets)


def doc_character():
    """A sticker-like composition: body, face and limbs under keyframed transforms, clipped by a matte and a mask."""
    rng = random.Random(707)
    body = smooth_path(blob_points(rng, 0, 0, 100, 125, 10))
    body2 = smooth_path(blob_points(rng, 0, 0, 100, 125, 10))
    eyes = [group([ellipse((0, 0), (34, 44)), fill((1, 1, 1)), stroke(PALETTE[5], 5)], position=[-40, -25]),
            group([ellipse((0, 0), (34, 44)), fill((1, 1, 1)), stroke(PALETTE[5], 5)], position=[40, -25]),
            group([ellipse((0, 0), (12, 16)), fill(PALETTE[5])], position=[-36, -22]),
            group([ellipse((0, 0), (12, 16)), fill(PALETTE[5])], position=[44, -22])]
    mouth = smooth_path([(-45, 40), (0, 80), (45, 40)], closed=False)
    arm = smooth_path([(0, 0), (50, 20), (90, -30)], closed=False)
    limbs = [group([path(arm), stroke(PALETTE[0], 22, 2, 2)], position=[110, 40], rotation=keyframes(
                [(0, -20), (30, 25), (59, -20)])),
             group([path(arm), stroke(PALETTE[0], 22, 2, 2)], position=[-110, 40], scale=[-100, 100],
                   rotation=keyframes([(0, 20), (30, -25), (59, 20)]))]
    layers = [layer([group([path({"a": 1, "k": morph([(0, body), (30, body2), (59, body)])["k"]}),
                            gradient_fill((0, -120), (0, 120), [(0, (1, .85, .3)), (1, PALETTE[0])]),
                            stroke(PALETTE[5], 10)], position=[256, 280],
                           scale=keyframes([(0, [100, 100]), (30, [104, 96]), (59, [100, 100])]))], 1),
              layer(limbs, 2, ks=transform(position=[256, 280])),
              layer(eyes + [group([path(mouth), stroke(PALETTE[5], 9, 2, 2)], position=[0, 0])], 3,
                    ks=transform(position=[256, 270])),
              layer([group(stripes(6, 1))], 4, tt=1),
              layer([group([ellipse((0, 0), (470, 470)), fill((1, 1, 1))], position=[256, 256])], 5, td=1)]
    return document(layers[::-1])


def doc_dense():
    rng = random.Random(808)
    items = []
    for index in range(90):
        cx, cy = rng.uniform(30, 480), rng.uniform(30, 480)
        shapes = [path(smooth_path(blob_points(rng, 0, 0, 8, 26, rng.choice([5, 6, 7])))),
                  fill(rng.choice(PALETTE[:5]))]
        if index % 3 == 0:
            shapes.append(stroke(PALETTE[5], rng.uniform(2, 6), 2, 2))
        items.append(group(shapes, position=[cx, cy], rotation=rng.uniform(0, 90)))
    layers = [layer(items, 1)]
    clip = smooth_path(blob_points(rng, 256, 256, 150, 220, 9))
    layers[0]["hasMask"] = True
    layers[0]["masksProperties"] = [mask(clip)]
    return document(layers, frames=12)


DOCUMENTS = {"blobs": doc_blobs, "merges": doc_merges, "mattes": doc_mattes, "masks": doc_masks,
             "strokes": doc_strokes, "nested": doc_nested, "character": doc_character, "dense": doc_dense}


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "tests", "render",
                                                                    "effects", "corpus")
    os.makedirs(directory, exist_ok=True)
    for name, build in DOCUMENTS.items():
        with open(os.path.join(directory, name + ".json"), "w", newline="\n") as handle:
            json.dump(build(), handle, separators=(",", ":"))
        print(name)


if __name__ == "__main__":
    main()
