"""Deterministic Telugu run holdouts and bounded-window crossings; QA only."""
import argparse
import itertools
import json
from pathlib import Path
import random


def corpus():
    cases={}
    def add(value):cases.setdefault(tuple(value),None)
    clusters=((0xc15,0xc4d,0xc30,0xc48),(0xc15,0x200d,0xc15,0xc3f),
              (0xc05,0xc4d,0xc15,0xc3c,0xc4b),(0xc15,0xc4d,0xc30,0xc4d,0xc30,0xc48),
              (0x25cc,0x200d,0xc15,0xc55),(0x200d,0xc4d,0xc15,0xc48))
    for padding in range(65):
        for cluster in clusters:
            add((0xc15,)*padding+cluster+(0xc15,)*5)
    for left,right in itertools.product(clusters,repeat=2):
        for between in ((),(0x20,),(0x200d,),(0x200c,),(0xc3c,),(0x952,)):
            add(left+between+right)
    alphabet=(0xc15,0xc30,0xc31,0xc34,0xc5d,0xc05,0xc0b,0xc12,0xc3c,0xc4d,
              0xc3e,0xc3f,0xc40,0xc41,0xc43,0xc46,0xc48,0xc4b,0xc55,0xc56,0xc62,
              0xc00,0xc01,0xc02,0xc03,0x951,0x952,0x953,0x1ce2,0x200c,0x200d,
              0x25cc,0xa0,0x20,0x21,0x31)
    rng=random.Random(2026092701)
    for _ in range(2048):
        add(rng.choices(alphabet,k=rng.randint(1,96)))
    return list(cases)


def control_capacity_corpus():
    prefixes=((0x200d,0xc4d,0xc15),(0x25cc,0x200d,0xc4d,0xc15),
              (0xa0,0x200d,0xc4d,0xc15),(0xc15,0x200d,0xc4d,0xc15),
              (0xc05,0x200d,0xc4d,0xc15))
    return [(*prefix,sign,*((0xc41,)*count)) for prefix in prefixes
            for sign in (0xc48,0xc3e) for count in range(28)]


def length_order_corpus():
    prefixes=((0xc15,0xc4d,0xc15),(0xc15,),(0x25cc,),(0xc15,0x200d,0xc15))
    signs=((0xc48,),(0xc43,0xc41),(0xc40,0xc4d,0xc43,0xc41))
    return [(*prefix,*tail,*((0xc41,)*count)) for prefix in prefixes
            for tail in signs for count in range(16)]


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--corpus',choices=('holdouts','control-capacity','length-order'),default='holdouts')
    args=parser.parse_args()
    if args.output.exists():parser.error('Preserve existing evidence; choose a new output file')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    samples={'holdouts':corpus,'control-capacity':control_capacity_corpus,'length-order':length_order_corpus}[args.corpus]()
    args.output.write_text(json.dumps(samples)+'\n',encoding='utf-8')
    print(len(samples),'run holdouts')
