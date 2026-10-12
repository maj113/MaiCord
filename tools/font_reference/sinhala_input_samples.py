"""Authored Sinhala input-preparation corpora; no shaping implementation."""
import itertools
import random


def corpus(stress=False):
    cases = {}
    def add(s, group):
        s = tuple(map(ord, s)) if isinstance(s, str) else tuple(s)
        if s:
            cases.setdefault(s, group)
    alphabet = (0xd9a, 0xdbb, 0xd85, 0xdca, 0xdd9, 0xdda, 0xddc, 0xddd,
                0xdde, 0xdd2, 0xdd4, 0xdcf, 0xddf, 0xd81, 0xd82, 0xd83,
                0x200c, 0x200d, 0x34f, 0xfe00)
    if stress:
        rng = random.Random(20260920)
        for _ in range(2048):
            add((rng.choice(alphabet + (0xd91, 0xd8b, 0xddb, 0xa0, 0x25cc, 0x20))
                 for _ in range(rng.randint(1, 70))), 'holdout')
        return list(cases.items())
    for scalar in range(0xd80, 0xe00):
        for s in ((scalar,), (0xd9a, scalar), (scalar, 0xdca, 0xd9a),
                  (0xd9a, scalar, 0xdd9), (0xd9a, scalar, scalar)):
            add(s, 'scalar-coverage')
    for base in (0xd9a, 0xdbb, 0xd85, 0xa0, 0x25cc, 0xdd9, 0x200d):
        for pair in itertools.product(alphabet, repeat=2):
            add((base, *pair), 'pairs')
    for prefix in ((0xd9a, 0xdca), (0xd9a, 0xdd9), (0xdbb, 0xdca),
                   (0xd9a, 0xdca, 0x200d), (0xd9a, 0xdca, 0x200c)):
        for pair in itertools.product(alphabet, repeat=2):
            add((*prefix, *pair), 'tails')
    for word in 'සිංහල භාෂාව ආයුබෝවන් ශ්‍රී ලංකාව ප්‍රවෘත්ති ක්‍රියා ක්ෂේත්‍රය කොළඹ ඥානය'.split():
        for end in range(1, len(word)+1):
            add(word[:end], 'words-and-partial-input')
    for base in range(0xd85, 0xd97):
        for sign in (0xdca, 0xdcf, 0xdd0, 0xdd1, 0xdd8, 0xdd9, 0xdda, 0xddc, 0xddd, 0xdde, 0xddf):
            add((base, sign), 'independent-vowels')
    for sign in alphabet[3:]:
        for n in (29, 30, 31, 32, 64):
            add((0xd9a,) + (sign,)*n, 'capacity')
    for link in ((0xdca,), (0xdca, 0x200d), (0x200d,), (0xdca, 0x200c)):
        add((0xd9a, *link)*40 + (0xdd9,), 'capacity')
    return list(cases.items())
