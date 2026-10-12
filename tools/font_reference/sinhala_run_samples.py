"""Deterministic Sinhala run corpora; no expected-output synthesis."""
import argparse,json,random
from pathlib import Path


def cases(holdout=False):
    parts=[[0xd9a],[0xdbb,0xdca,0x200d,0xd9a,0xdd9],[0xd9a,0xdca,0x200d,0xdba,0xdd9],
      [0xd9a,0xddd],[0xdd9],[0x20],[0xd9a,0x34f],[0xd9a,0x200c],[0xdbb],[0xd9a,0xdda],[0xd85,0xdcf]]
    if not holdout:
        rng=random.Random(91321);values=[a+b for a in parts for b in parts]
        values += [sum((rng.choice(parts) for _ in range(rng.randrange(1,12))),[]) for _ in range(512)]
        values += [[0xd9a]*n for n in (31,32,33,63,64)]
        return values
    rng=random.Random(73041)
    alphabet=list(range(0xd80,0xe00))+[0x200c,0x200d,0x34f,0xfe0f,0xa0,0x25cc,0x20,0x2e]
    values=[[rng.choice(alphabet) for _ in range(rng.randrange(1,97))] for _ in range(512)]
    values += [[0xd9a,0xdca]*n+[0xd9a,0xddd,0xdbb] for n in (14,15,16,30,31,32,47,63)]
    return values


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);p.add_argument('--holdout',action='store_true');a=p.parse_args()
    a.output.write_text(json.dumps(cases(a.holdout)),encoding='utf-8')
