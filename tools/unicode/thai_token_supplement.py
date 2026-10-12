"""Derive experimental lexical supplements from explicit reference observations.

This is an offline data compiler, not a runtime library. It never reads test
expected results to select individual exceptions. Every source entry is split
at its observed boundaries; a new token is retained if both observations keep
it whole and the original lexicon would split it. Contextual regressions remain
possible and must be reported by independent comparisons before any cutover.
"""
import argparse
import hashlib
import json
from pathlib import Path

from word_dictionary import words_from


def parts(word, boundaries):
    if boundaries != sorted(set(boundaries)) or any(not 0 < p < len(word) for p in boundaries):
        raise ValueError('Invalid observed token boundaries')
    positions = [0, *boundaries, len(word)]
    return [(word[a:b], a, b) for a, b in zip(positions, positions[1:])]


def baseline_trie(words):
    root = {}
    for word in words:
        node = root
        for character in word:
            node = node.setdefault(character, {})
        node[None] = word
    return root


def baseline_boundaries(text, trie, ending, continuing):
    count = len(text)
    costs, decisions = [(0, 0)] * (count + 1), [None] * count
    for first in range(count - 1, -1, -1):
        best = costs[first + 1][0] + 1, costs[first + 1][1] + 1
        following, word, node = first + 1, None, trie
        for last in range(first, min(count, first + 31)):
            node = node.get(text[last])
            if node is None:
                break
            if None in node:
                score = costs[last + 1][0], costs[last + 1][1] + 1
                if score <= best:
                    best, following, word = score, last + 1, node[None]
        costs[first], decisions[first] = best, (following, word)
    result, first = [], 0
    while first < count:
        following, word = decisions[first]
        if word:
            result.extend(first + p for p in (continuing if following < count else ending)[word])
            if following < count:
                result.append(following)
        first = following
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('base-words', 'source-words', 'ending', 'continuing', 'output-dir'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    base, source = words_from(args.base_words), words_from(args.source_words)
    ending = json.loads(args.ending.read_text(encoding='utf-8'))['annotations']
    continuing = json.loads(args.continuing.read_text(encoding='utf-8'))
    origins = {}
    for word in source:
        for label, annotations in [('ending', ending), ('continuing', continuing)]:
            for token, first, last in parts(word, annotations[word]):
                origins.setdefault(token, dict(source_word=word, observation=label, first=first, last=last))
    for word in set(base) | set(origins):
        if not word or len(word) > 31 or any(not 0xe00 <= ord(c) < 0xe80 for c in word):
            raise ValueError('Unexpected word encoding or length')
        parts(word, ending[word]); parts(word, continuing[word])
    trie = baseline_trie(base)
    additions = sorted(word for word in origins if not ending[word] and not continuing[word]
                       and baseline_boundaries(word, trie, ending, continuing))
    if set(additions) & set(base):
        raise ValueError('Supplement unexpectedly overlaps the baseline lexicon')
    words = sorted(set(base) | set(additions))
    output = args.output_dir
    output.mkdir(parents=True, exist_ok=True)
    (output / 'words.txt').write_text('\n'.join(words) + '\n', encoding='utf-8')
    (output / 'ending.json').write_text(json.dumps({'annotations': {w: ending[w] for w in words}}), encoding='utf-8')
    (output / 'continuing.json').write_text(json.dumps({w: continuing[w] for w in words}), encoding='utf-8')
    report = dict(base_words=len(base), source_words=len(source), observed_tokens=len(origins),
                  additions=len(additions), total_words=len(words),
                  inputs={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in
                          [args.base_words, args.source_words, args.ending, args.continuing]},
                  origins={w: origins[w] for w in additions},
                  selection_scope='Whole source lexicon; no held-out text or app sample selection',
                  full_context_parity=False)
    (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({k: report[k] for k in ('base_words', 'source_words', 'observed_tokens', 'additions', 'total_words')}))


if __name__ == '__main__':
    main()
