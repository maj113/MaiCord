"""Evaluate compact first-party word data; no runtime ICU code is generated.

Input is an explicit UTF-8 word list. Generated output is a suffix-shared acyclic
graph, verified against every input word. This development prototype is not yet
connected to production line breaking; dictionary/license provenance belongs in
the caller's report alongside this generator's content hashes.
"""
import argparse
import hashlib
import json
from pathlib import Path


def words_from(path):
    return sorted({line.split('\t')[0] for raw in path.read_text(encoding='utf-8-sig').splitlines()
                   if (line := raw.strip()) and not line.startswith('#')})


def pack(words, base):
    trie = {}
    for word in words:
        node = trie
        for ch in word:
            value = ord(ch) - base
            if not 0 <= value < 128:
                raise ValueError('Word contains a character outside the selected block')
            node = node.setdefault(value, {})
        node[-1] = True
    unique, nodes = {}, []

    def intern(node):
        signature = (-1 in node, tuple((ch, intern(child)) for ch, child in sorted(node.items()) if ch >= 0))
        if signature not in unique:
            unique[signature] = len(nodes)
            nodes.append(signature)
        return unique[signature]

    root = intern(trie)
    data, offsets = bytearray(), []
    for terminal, edges in nodes:
        start = len(data)
        offsets.append(start)
        assert len(edges) < 128
        data.append(len(edges) * 2 + terminal)
        for ch, child in edges:
            data.append(ch)
            distance = start - offsets[child]
            assert distance > 0
            while distance >= 128:
                data.append((distance & 127) | 128)
                distance >>= 7
            data.append(distance)
    return bytes(data), offsets[root], len(nodes)


def matches(data, root, text, first, base):
    node = root
    for end in range(first, len(text)):
        cp = ord(text[end]) - base
        if not 0 <= cp < 128:
            return
        at, child = node + 1, None
        for _ in range(data[node] >> 1):
            label = data[at]
            at += 1
            distance = shift = 0
            while True:
                byte = data[at]
                at += 1
                distance |= (byte & 127) << shift
                if byte < 128:
                    break
                shift += 7
            if label == cp:
                child = node - distance
                break
            if label > cp:
                break
        if child is None:
            return
        node = child
        if data[node] & 1:
            yield end + 1


def segment(data, root, text, base):
    """Research baseline: minimize unknown scalars, then word count.

    This is deliberately named and measured as a candidate, not a Windows
    compatibility rule. Word lists alone do not establish segmentation parity.
    """
    count = len(text)
    costs, nexts = [(0, 0)] * (count + 1), [0] * count
    for first in range(count - 1, -1, -1):
        suffix = costs[first + 1]
        best, following = (suffix[0] + 1, suffix[1] + 1), first + 1
        for end in matches(data, root, text, first, base):
            suffix = costs[end]
            score = suffix[0], suffix[1] + 1
            if score <= best:
                best, following = score, end
        costs[first], nexts[first] = best, following
    breaks, at = [], 0
    while at < count:
        at = nexts[at]
        breaks.append(at)
    return breaks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--base', type=lambda x: int(x, 0), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    words = words_from(args.input)
    data, root, nodes = pack(words, args.base)
    for word in words:
        assert len(word) in matches(data, root, word, 0, args.base)
    args.output.write_bytes(data)
    report = dict(words=len(words), nodes=nodes, bytes=len(data), root=root,
                  base=args.base, maximum_word_scalars=max(map(len, words)),
                  input_sha256=hashlib.sha256(args.input.read_bytes()).hexdigest(),
                  output_sha256=hashlib.sha256(data).hexdigest(),
                  verified_words=len(words), production=False)
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
