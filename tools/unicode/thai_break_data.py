"""Build experimental Thai boundary data from explicit lexical observations.

Word data is ICU data, optionally supplemented by observed PyThaiNLP subwords;
their respective Unicode/CC0 notices accompany the output. No library code is
included. End and
continuing boundary observations are QA inputs from the app's Windows reference.
This generator verifies every encoded terminal and both masks before publication.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
from word_dictionary import words_from


def varint(value):
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return result


def read_varint(data, at):
    result = shift = 0
    while True:
        byte = data[at]
        at += 1
        result |= (byte & 127) << shift
        if byte < 128:
            return result, at
        shift += 7


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--words', required=True, type=Path)
    parser.add_argument('--end', required=True, type=Path)
    parser.add_argument('--continuing', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--report', required=True, type=Path)
    parser.add_argument('--recognizers', type=Path,
                        help='Observed context-recognition words; a subset of --words.')
    parser.add_argument('--candidates', type=Path,
                        help='DP candidate words; other entries are recognition-only. Default: all words.')
    parser.add_argument('--reference-slots', type=int, default=64,
                        help='One-byte references to common suffix nodes, 0..128 (default: 64).')
    parser.add_argument('--word-source', choices=('icu', 'icu-and-pythainlp'), default='icu',
                        help='Select the provenance notice for the explicit input word list.')
    args = parser.parse_args()
    if not 0 <= args.reference_slots <= 128:
        parser.error('--reference-slots must be between 0 and 128')
    words = words_from(args.words)
    recognizers = set(words_from(args.recognizers)) if args.recognizers else set()
    candidates = set(words_from(args.candidates)) if args.candidates else set(words)
    if not recognizers <= set(words) or not candidates <= set(words) or not candidates:
        raise ValueError('Invalid recognizer/candidate subset')
    if set(words) - candidates - recognizers:
        raise ValueError('Recognition-only entries must be recognizers')
    ending = json.loads(args.end.read_text(encoding='utf-8'))['annotations']
    continuing = json.loads(args.continuing.read_text(encoding='utf-8'))
    if set(words) != set(ending) or set(words) != set(continuing):
        raise ValueError('Observation sets do not cover exactly the input lexicon')
    trie, masks, mask_ids = {}, [], {}
    for word in words:
        if not word or len(word) > 31 or any(not 0xe00 <= ord(c) < 0xe80 for c in word):
            raise ValueError('Unexpected word encoding or length')
        if any(values != sorted(set(values)) for values in (ending[word], continuing[word])):
            raise ValueError('Boundary indices must be sorted and unique')
        pair = tuple(sum(1 << i for i in values) for values in (ending[word], continuing[word]))
        if any(not 0 < i < len(word) for i in ending[word] + continuing[word]):
            raise ValueError('Invalid internal boundary')
        # Words have at most 31 scalars, so bit 31 cannot be an internal break.
        pair = (pair[0] | ((word in recognizers) << 31), pair[1])
        if pair not in mask_ids:
            mask_ids[pair] = len(masks)
            masks.append(pair)
        node = trie
        for cp in map(ord, word):
            node = node.setdefault(cp - 0xe00, {})
        node[-1] = (mask_ids[pair] << 1) | (word not in candidates)
    unique, nodes = {}, []

    def intern(node):
        key = node.get(-1, -1), tuple((c, intern(child)) for c, child in sorted(node.items()) if c >= 0)
        if key not in unique:
            unique[key] = len(nodes)
            nodes.append(key)
        return unique[key]

    root_id = intern(trie)
    if root_id != len(nodes) - 1:
        raise ValueError('Expected the root after all child nodes')

    def serialize(references, fixed_bitmaps=None):
        data, offsets, bitmaps = bytearray(), [], set()
        # The direct first-character table replaces the serialized root.
        for identifier, (terminal, edges) in enumerate(nodes[:root_id]):
            start = len(data)
            offsets.append(start)

            def reference(child):
                return varint(references[child] if child in references else
                              start - offsets[child] + len(references))

            if terminal < 0 and len(edges) == 1:
                label, child = edges[0]
                data.append(label)
                data += reference(child)
                continue
            distances = [start - offsets[child] for _, child in edges]
            width = 2 if not distances or max(distances) < 65536 else 3
            if distances and max(distances) >= 1 << 24:
                raise ValueError('Graph exceeds compact backward-reference capacity')
            only = terminal >= 0 and terminal & 1
            bitmap_size = 16 + width * len(edges)
            linear_size = sum(1 + len(varint(distance)) for distance in distances)
            use_bitmap = (len(edges) >= (28 if only else 32) or
                          (len(edges) >= 8 and bitmap_size <= linear_size))
            if fixed_bitmaps is not None:
                # Preserve rank lookup at the same high-degree nodes even
                # when short references would make a linear list smaller.
                use_bitmap = identifier in fixed_bitmaps
            if use_bitmap:
                data.append(248 + only * 4 + (width == 3) * 2 + (terminal >= 0))
                if terminal >= 0:
                    data += varint(terminal >> 1)
                mask = sum(1 << label for label, _ in edges)
                data += mask.to_bytes(16, 'little')
                for distance in distances:
                    data += distance.to_bytes(width, 'little')
                bitmaps.add(identifier)
                continue
            data.append(128 | (only * 64) | (len(edges) * 2 + (terminal >= 0)))
            if terminal >= 0:
                data += varint(terminal >> 1)
            for label, child in edges:
                data.append(label)
                data += reference(child)
        return data, offsets, bitmaps

    baseline, baseline_offsets, bitmaps = serialize({})
    gains = Counter()
    for identifier, (_, edges) in enumerate(nodes[:root_id]):
        if identifier not in bitmaps:
            for _, child in edges:
                gains[child] += len(varint(baseline_offsets[identifier] - baseline_offsets[child])) - 1
    selected = sorted((child for child in gains if gains[child] > 4),
                      key=lambda child: (-gains[child], child))[:args.reference_slots]
    references = {child: index for index, child in enumerate(selected)}
    data, offsets, bitmaps = serialize(references, bitmaps)
    common = [offsets[child] for child in selected]

    def target(node, value):
        return common[value] if value < len(common) else node - (value - len(common))

    # Verify the entire topology, not just successful word lookups. Identical
    # labels, targets and terminal metadata also preserve absent-edge results.
    verified_edges = 0
    for identifier, (terminal, edges) in enumerate(nodes[:root_id]):
        node = offsets[identifier]
        header, at = data[node], node + 1
        decoded_terminal, decoded_edges = -1, []
        if header < 128:
            value, at = read_varint(data, at)
            decoded_edges.append((header, target(node, value)))
        else:
            if header & 1:
                index, at = read_varint(data, at)
                only = header >= 252 if header >= 248 else bool(header & 64)
                decoded_terminal = (index << 1) | only
            if header >= 248:
                bitmap = int.from_bytes(data[at:at + 16], 'little')
                at += 16
                width = 3 if header & 2 else 2
                for label in range(128):
                    if bitmap & (1 << label):
                        distance = int.from_bytes(data[at:at + width], 'little')
                        at += width
                        decoded_edges.append((label, node - distance))
            else:
                for _ in range((header & 63) >> 1):
                    label = data[at]
                    value, at = read_varint(data, at + 1)
                    decoded_edges.append((label, target(node, value)))
        next_offset = offsets[identifier + 1] if identifier + 1 < root_id else len(data)
        if (at != next_offset or decoded_terminal != terminal or
                decoded_edges != [(label, offsets[child]) for label, child in edges]):
            raise ValueError('Encoded node topology differs')
        verified_edges += len(edges)
    initial = [0xffffffff] * 128
    for label, child in nodes[root_id][1]:
        initial[label] = offsets[child]
    for word in words:
        node = initial[ord(word[0]) - 0xe00]
        if node == 0xffffffff:
            raise ValueError('Initial edge missing')
        for cp in map(ord, word[1:]):
            at = node + 1
            if data[node] < 128:
                if data[node] != cp - 0xe00:
                    raise ValueError('Single edge missing')
                distance, _ = read_varint(data, at)
                node = target(node, distance)
                continue
            if data[node] & 1:
                _, at = read_varint(data, at)
            if data[node] >= 248:
                mask = int.from_bytes(data[at:at + 16], 'little')
                bit = 1 << (cp - 0xe00)
                if not mask & bit:
                    raise ValueError('Bitmap edge missing')
                width = 3 if data[node] & 2 else 2
                edge = at + 16 + (mask & (bit - 1)).bit_count() * width
                node -= int.from_bytes(data[edge:edge + width], 'little')
                continue
            found = None
            for _ in range((data[node] & 63) >> 1):
                label = data[at]
                distance, at = read_varint(data, at + 1)
                if label == cp - 0xe00:
                    found = target(node, distance)
                    break
            if found is None:
                raise ValueError('Encoded path missing')
            node = found
        if data[node] & 129 != 129:
            raise ValueError('Encoded terminal missing')
        index, _ = read_varint(data, node + 1)
        expected = tuple(sum(1 << i for i in v) for v in (ending[word], continuing[word]))
        expected = (expected[0] | ((word in recognizers) << 31), expected[1])
        if masks[index] != expected:
            raise ValueError('Encoded boundary masks differ')
        only = data[node] >= 252 if data[node] >= 248 else bool(data[node] & 64)
        if only != (word not in candidates):
            raise ValueError('Encoded candidate eligibility differs')
    text = ('// Generated by tools/unicode/thai_break_data.py; do not edit.\n'
            '// Word data: Copyright (C) 2016 and later Unicode, Inc. and others.\n'
            '// Copyright (c) 2006-2015 IBM, Apple Inc., and others. All rights reserved.\n'
            '// See assets/licenses/Unicode-3.0.txt.\n')
    if args.word_source == 'icu-and-pythainlp':
        text += '// Additional lexical data: PyThaiNLP words_th.txt, CC0-1.0.\n// See assets/licenses/PyThaiNLP-corpus.txt.\n'
    text += 'constexpr u8 thai_word_data[]{\n'
    text += ''.join('    ' + ','.join(str(b) for b in data[i:i + 32]) + ',\n' for i in range(0, len(data), 32))
    text += '};\nconstexpr u32 thai_word_masks[][2]{\n'
    text += ''.join(f'    {{{a}u,{b}u}},\n' for a, b in masks)
    text += '};\nconstexpr u32 thai_word_initial[128]{\n'
    text += ''.join('    ' + ','.join(f'{v}u' for v in initial[i:i + 8]) + ',\n' for i in range(0, 128, 8))
    text += '};\n'
    text += f'constexpr u32 thai_word_reference_count={len(common)};\n'
    text += 'constexpr u32 thai_word_references[]{\n'
    text += ''.join('    ' + ','.join(f'{v}u' for v in common[i:i + 8]) + ',\n' for i in range(0, len(common), 8)) if common else '    0u,\n'
    text += '};\n'
    args.output.write_text(text, encoding='utf-8')
    report = dict(words=len(words), nodes=len(nodes), masks=len(masks),
                  candidates=len(candidates), recognizers=len(recognizers),
                  bitmap_nodes=len(bitmaps), reference_slots=len(common), reference_bytes=len(common) * 4,
                  unpacked_graph_bytes=len(baseline),
                  word_source=args.word_source,
                  graph_bytes=len(data), mask_bytes=len(masks) * 8, initial_bytes=512, root_elided=True,
                  verified_words=len(words),
                  verified_nodes=root_id, verified_edges=verified_edges,
                  inputs={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in
                          [args.words, args.end, args.continuing, args.recognizers, args.candidates] if p},
                  output_sha256=hashlib.sha256(args.output.read_bytes()).hexdigest())
    args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
