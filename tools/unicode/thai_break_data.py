"""Generate the Thai word-break lexicon from openly licensed word lists.

Inputs, pinned by release and SHA-256 and verified before use:

- ICU 78.3 thaidict.txt, Unicode License v3 and the ICU 1.8.1-57.1 license
  (assets/licenses/ICU-Thai-dictionary.txt)
- PyThaiNLP 5.3.8 words_th.txt, CC0-1.0 (assets/licenses/PyThaiNLP-corpus.txt)

Nothing else is read: no platform binary, library or observed segmentation.
Both lists are filtered to lexical Thai entries (consonants, vowels and
marks; no digits, punctuation, PAIYANNOI or MAIYAMOK, which the segmenter
handles itself) of 2 to 20 scalars. Every such ICU word is kept, plus the
PyThaiNLP entries that are not compounds of shorter kept words. The result
is written as a suffix-shared acyclic word graph. Every encoded node and
every input word is decoded again and checked before the output is written.
Run with `python -I`; the module imports only the standard library.

    python -I tools/unicode/thai_break_data.py --icu <thaidict.txt> --pythainlp <words_th.txt>
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path

SOURCES = {
    'icu': dict(
        title='ICU 78.3 icu4c/source/data/brkitr/dictionaries/thaidict.txt',
        url='https://raw.githubusercontent.com/unicode-org/icu/release-78.3/'
            'icu4c/source/data/brkitr/dictionaries/thaidict.txt',
        sha256='3166abde40c0f44ab91c28f5ce96d7d1472cb7882e1c0bda0a72f8f69dba4274',
        license='Unicode License v3, ICU 1.8.1-57.1 license; see assets/licenses/ICU-Thai-dictionary.txt'),
    'pythainlp': dict(
        title='PyThaiNLP 5.3.8 pythainlp/corpus/words_th.txt',
        url='https://raw.githubusercontent.com/PyThaiNLP/pythainlp/v5.3.8/'
            'pythainlp/corpus/words_th.txt',
        sha256='4bf0ab93a570d91b33ae7bbf627eddd5a56f81c6ba33a94c3950f38904235bda',
        license='CC0-1.0; see assets/licenses/PyThaiNLP-corpus.txt'),
}
OUTPUT = Path(__file__).resolve().parents[2] / 'src/text/generated/thai_lexical_data.inc'
LONGEST = 20  # ICU's longest entry; longer PyThaiNLP compounds are phrases.
REFERENCE_SLOTS = 64
# Consonants, dependent vowels, leading vowels, LAKKHANGYAO and the marks.
LEXICAL = {*range(0xe01, 0xe2f), *range(0xe30, 0xe3b), *range(0xe40, 0xe46), *range(0xe47, 0xe4f)}
LEADING = range(0xe40, 0xe45)
INITIAL = {*range(0xe01, 0xe2f), *LEADING}  # A word starts with a consonant or leading vowel.


def read_source(path, key):
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != SOURCES[key]['sha256']:
        raise ValueError(f'{path}: SHA-256 {digest} is not the pinned {SOURCES[key]["title"]}')
    return [line for raw in data.decode('utf-8-sig').splitlines()
            if (line := raw.strip()) and not line.startswith('#')]


def lexical(entry, excluded):
    codes = [ord(c) for c in entry]
    if any(c not in LEXICAL for c in codes):
        excluded['non-lexical scalar'] += 1
    elif not 2 <= len(codes) <= LONGEST:
        excluded['length'] += 1
    elif codes[0] not in INITIAL:
        excluded['dependent initial'] += 1
    elif codes[-1] in LEADING or codes[-1] == 0xe31:
        excluded['incomplete final'] += 1
    else:
        return True
    return False


def compound(word, lexicon):
    """True if the word is a sequence of two or more shorter lexicon words."""
    reachable = [True] + [False] * len(word)
    for start in range(len(word)):
        if reachable[start]:
            for end in range(start + 2, len(word) + 1):
                if end - start < len(word) and word[start:end] in lexicon:
                    reachable[end] = True
    return reachable[-1]


def select(icu, pythainlp):
    """Return the sorted lexicon and the count of entries left out per reason.

    Every lexical ICU word is kept. A PyThaiNLP-only entry is kept unless it
    is a compound of shorter lexicon words: such entries cannot add a break
    opportunity, only remove them, and they would more than double the table.
    A dropped compound's parts are shorter, so by induction they remain
    reachable through kept words.
    """
    excluded = Counter()
    lexicon = {w for w in {*icu, *pythainlp} if lexical(w, excluded)}
    candidates = lexicon - set(icu)
    compounds = {w for w in candidates if compound(w, lexicon)}
    excluded['PyThaiNLP compound of shorter words'] = len(compounds)
    return sorted(lexicon - compounds), excluded


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


def build(words):
    """Encode the word graph; the root becomes the 128-entry initial table.

    Node encodings, where a reference below `len(references)` names a shared
    node by absolute offset and any larger value is a backward distance:
    - 0..79: one edge with this label, then a reference; not a word end.
    - 80..127: one edge described by a shared u16 (label | distance << 8).
      The reader expands these and ordinary labels into a constexpr 128-entry
      table, so an absent edge needs only one label comparison.
    - 128 + 2 * edges + end, edges <= 59: (label, reference) pairs by label.
      A label's high bit names the terminal node at offset zero, with no reference.
    - 248 + 2 * wide + end: a 128-bit label mask, then one backward distance
      of 2 bytes (3 if wide) per set bit.
    """
    trie = {}
    for word in words:
        node = trie
        for c in word:
            if not 0 <= ord(c) - 0xe00 < 80:
                raise ValueError('Word label overlaps the compact unary headers')
            node = node.setdefault(ord(c) - 0xe00, {})
        node[-1] = True
    unique, nodes = {}, []

    def intern(node):
        key = -1 in node, tuple((c, intern(child)) for c, child in sorted(node.items()) if c >= 0)
        if key not in unique:
            unique[key] = len(nodes)
            nodes.append(key)
        return unique[key]

    root = intern(trie)
    if root != len(nodes) - 1:
        raise ValueError('Expected the root after all child nodes')
    if nodes[0] != (True, ()):
        raise ValueError('Expected the terminal node at offset zero')

    def serialize(references, fixed_bitmaps=None, terminal_edges=False, special=()):
        data, offsets, bitmaps = bytearray(), [], set()
        patterns = {value: index + 80 for index, value in enumerate(special)}
        frequencies = Counter()
        for identifier, (end, edges) in enumerate(nodes[:root]):
            start = len(data)
            offsets.append(start)

            def reference(child):
                return varint(references[child] if child in references else
                              start - offsets[child] + len(references))

            if not end and len(edges) == 1:
                label, child = edges[0]
                encoded = reference(child)
                distance = start - offsets[child]
                pattern = label | (distance << 8)
                if distance < 256:
                    frequencies[pattern] += len(encoded)
                if pattern in patterns:
                    data.append(patterns[pattern])
                else:
                    data.append(label)
                    data += encoded
                continue
            distances = [start - offsets[child] for _, child in edges]
            wide = bool(distances) and max(distances) >= 65536
            if distances and max(distances) >= 1 << 24:
                raise ValueError('Graph exceeds the 3-byte backward distance')
            linear = sum(1 + len(varint(d)) for d in distances)
            use_bitmap = len(edges) >= 32 or (len(edges) >= 8 and 16 + (3 if wide else 2) * len(edges) <= linear)
            if fixed_bitmaps is not None:
                # Keep rank lookup at the same nodes when shared references
                # would make a linear list smaller.
                use_bitmap = identifier in fixed_bitmaps
            if use_bitmap:
                data.append(248 + wide * 2 + end)
                data += sum(1 << label for label, _ in edges).to_bytes(16, 'little')
                for distance in distances:
                    data += distance.to_bytes(3 if wide else 2, 'little')
                bitmaps.add(identifier)
                continue
            if len(edges) > 59:
                raise ValueError('List node exceeds 59 edges')
            data.append(128 + len(edges) * 2 + end)
            for label, child in edges:
                if terminal_edges and child == 0:
                    data.append(label | 128)
                else:
                    data.append(label)
                    data += reference(child)
        return data, offsets, bitmaps, frequencies

    # Choose the same rank-lookup nodes and shared references as the untagged
    # encoding; terminal-edge packing only removes redundant references.
    baseline, baseline_offsets, bitmaps, _ = serialize({})
    gains = Counter()
    for identifier, (_, edges) in enumerate(nodes[:root]):
        if identifier not in bitmaps:
            for _, child in edges:
                gains[child] += len(varint(baseline_offsets[identifier] - baseline_offsets[child])) - 1
    shared = sorted((c for c in gains if gains[c] > 4), key=lambda c: (-gains[c], c))[:REFERENCE_SLOTS]
    references = {c: i for i, c in enumerate(shared)}
    data, offsets, _, frequencies = serialize(references, bitmaps, True)
    special = []
    best = (len(data) + 256, data, offsets, special)
    # Shrinking earlier nodes changes backward distances. Evaluate a bounded,
    # deterministic refinement and keep the smallest complete encoding.
    for _ in range(8):
        special = [value for value, _ in frequencies.most_common(48)]
        candidate, positions, _, frequencies = serialize(references, bitmaps, True, special)
        size = len(candidate) + 256
        if size < best[0]:
            best = (size, candidate, positions, special)
    _, data, offsets, special = best
    initial = [0xffffffff] * 128
    for label, child in nodes[root][1]:
        initial[label] = offsets[child]
    return dict(data=data, offsets=offsets, nodes=nodes, root=root, initial=initial,
                references=[offsets[c] for c in shared], special=special, bitmaps=len(bitmaps), unshared_bytes=len(baseline))


def decode(graph, node):
    """Decode one node independently of the encoder: (end, [(label, offset)], next)."""
    data, references = graph['data'], graph['references']

    def target(value):
        return references[value] if value < len(references) else node - (value - len(references))

    header, at = data[node], node + 1
    if 80 <= header < 128:
        pattern = graph['special'][header - 80]
        return False, [(pattern & 255, node - (pattern >> 8))], at
    if header < 128:
        value, at = read_varint(data, at)
        return False, [(header, target(value))], at
    edges = []
    if header >= 248:
        mask = int.from_bytes(data[at:at + 16], 'little')
        at += 16
        width = 3 if header & 2 else 2
        for label in range(128):
            if mask >> label & 1:
                edges.append((label, node - int.from_bytes(data[at:at + width], 'little')))
                at += width
        return bool(header & 1), edges, at
    for _ in range((header - 128) >> 1):
        label = data[at]
        at += 1
        if label & 128:
            edges.append((label & 127, 0))
        else:
            value, at = read_varint(data, at)
            edges.append((label, target(value)))
    return bool(header & 1), edges, at


def verify(graph, words):
    """Check every node's topology and the exact lexicon the graph accepts."""
    data, offsets, nodes, root = graph['data'], graph['offsets'], graph['nodes'], graph['root']
    for identifier, (end, edges) in enumerate(nodes[:root]):
        decoded_end, decoded_edges, at = decode(graph, offsets[identifier])
        following = offsets[identifier + 1] if identifier + 1 < root else len(data)
        if at != following or decoded_end != end or decoded_edges != [(l, offsets[c]) for l, c in edges]:
            raise ValueError('Encoded node topology differs')
    accepted, pending = set(), [(offset, chr(0xe00 + label)) for label, offset in
                                enumerate(graph['initial']) if offset != 0xffffffff]
    while pending:
        node, prefix = pending.pop()
        end, edges, _ = decode(graph, node)
        if end:
            accepted.add(prefix)
        pending.extend((child, prefix + chr(0xe00 + label)) for label, child in edges)
    if accepted != set(words):
        raise ValueError('Decoded lexicon differs from the input lexicon')
    return len(accepted)


def emit(graph, words, inputs):
    header = ['// Generated by tools/unicode/thai_break_data.py; do not edit.',
              f'// Thai word list: {len(words)} words of 2-{LONGEST} scalars without digits or',
              '// punctuation: the ICU words, plus the PyThaiNLP words that are not',
              '// compounds of shorter listed words. Sources:']
    for key in ('icu', 'pythainlp'):
        source = SOURCES[key]
        header += [f'// - {source["title"]} ({inputs[key]} entries)',
                   f'//   {source["url"]}',
                   f'//   SHA-256 {source["sha256"]}',
                   f'//   {source["license"]}']
    header += ['// ICU data: Copyright (C) 2016 and later Unicode, Inc. and others;',
               '// Copyright (c) 2006-2015 International Business Machines Corporation,',
               '// Apple Inc., and others. All Rights Reserved.',
               '// PyThaiNLP words_th.txt: created by the PyThaiNLP project, CC0-1.0.']
    text = '\n'.join(header) + '\n'
    data = graph['data']
    text += f'constexpr u32 thai_word_longest={LONGEST};\nconstexpr u8 thai_word_graph[]{{\n'
    text += ''.join('    ' + ','.join(str(b) for b in data[i:i + 32]) + ',\n' for i in range(0, len(data), 32))
    text += '};\nconstexpr u32 thai_word_initial[128]{\n'
    text += ''.join('    ' + ','.join(f'{v}u' for v in graph['initial'][i:i + 8]) + ',\n' for i in range(0, 128, 8))
    references = graph['references']
    text += f'}};\nconstexpr u32 thai_word_reference_count={len(references)};\nconstexpr u32 thai_word_references[]{{\n'
    text += ''.join('    ' + ','.join(f'{v}u' for v in references[i:i + 8]) + ',\n' for i in range(0, len(references), 8))
    text += ('' if references else '    0u,\n') + '};\n'
    special = ','.join(map(str, graph['special'])) or '0'
    return text + 'constexpr u16 thai_word_special[]{' + special + '};\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--icu', required=True, type=Path, help='ICU 78.3 thaidict.txt')
    parser.add_argument('--pythainlp', required=True, type=Path, help='PyThaiNLP 5.3.8 words_th.txt')
    parser.add_argument('--output', type=Path, default=OUTPUT)
    parser.add_argument('--report', type=Path, help='Optional JSON generation report')
    args = parser.parse_args()
    icu, pythainlp = read_source(args.icu, 'icu'), read_source(args.pythainlp, 'pythainlp')
    words, excluded = select(icu, pythainlp)
    graph = build(words)
    verified = verify(graph, words)
    text = emit(graph, words, dict(icu=len(icu), pythainlp=len(pythainlp)))
    args.output.write_bytes(text.encode('utf-8'))
    icu_words, pythainlp_words = set(icu) & set(words), set(pythainlp) & set(words)
    report = dict(words=len(words), verified_words=verified, from_icu=len(icu_words),
                  from_pythainlp=len(pythainlp_words), pythainlp_only=len(pythainlp_words - icu_words),
                  excluded=dict(sorted(excluded.items())), longest=LONGEST,
                  nodes=graph['root'], bitmap_nodes=graph['bitmaps'], graph_bytes=len(graph['data']),
                  unshared_graph_bytes=graph['unshared_bytes'], reference_bytes=len(graph['references']) * 4,
                  initial_bytes=512, unary_bytes=256,
                  payload_bytes=len(graph['data']) + len(graph['references']) * 4 + 512 + 4 + 256,
                  sources={k: dict(url=v['url'], sha256=v['sha256']) for k, v in SOURCES.items()},
                  generator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  output_sha256=hashlib.sha256(args.output.read_bytes()).hexdigest())
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
