"""Deterministic Kannada run holdouts and bounded-window crossings; QA only."""
import argparse
import itertools
import json
from pathlib import Path
import random


def corpus():
    cases={}
    def add(value):cases.setdefault(tuple(value),None)
    clusters=((0xcb0,0xccd,0xc95,0xccb),(0xc95,0x200d,0xc95,0xccb),
              (0xc85,0xccd,0xc95,0xcbc,0xccb),(0xc95,0xccd,0xcb0,0xccd,0xcb0,0xcc8),
              (0x25cc,0x200d,0xc95,0xcd5),(0x200d,0xccd,0xc95,0xcc8))
    for padding in range(65):
        for cluster in clusters:
            add((0xc95,)*padding+cluster+(0xc95,)*5)
    for left,right in itertools.product(clusters,repeat=2):
        for between in ((),(0x20,),(0x200d,),(0x200c,),(0xcbc,),(0x952,)):
            add(left+between+right)
    alphabet=(0xc95,0xcb0,0xcb1,0xcb3,0xcdd,0xc85,0xc8b,0xc92,0xcbc,0xccd,
              0xcbe,0xcbf,0xcc0,0xcc1,0xcc3,0xcc6,0xcc8,0xccb,0xcd5,0xcd6,0xce2,
              0xc80,0xc81,0xc82,0xc83,0x951,0x952,0x953,0x1ce2,0x200c,0x200d,
              0x25cc,0xa0,0x20,0x21,0x31)
    rng=random.Random(2026093001)
    for _ in range(2048):
        add(rng.choices(alphabet,k=rng.randint(1,96)))
    return list(cases)


def control_capacity_corpus():
    prefixes=((0x200d,0xccd,0xc95),(0x25cc,0x200d,0xccd,0xc95),
              (0xa0,0x200d,0xccd,0xc95),(0xc95,0x200d,0xccd,0xc95),
              (0xc85,0x200d,0xccd,0xc95))
    return [(*prefix,sign,*((0xcc1,)*count)) for prefix in prefixes
            for sign in (0xcc0,0xcc7,0xcc8,0xcca,0xccb,0xcbe) for count in range(28)]


def length_order_corpus():
    prefixes=((0xc95,0xccd,0xc95),(0xc95,),(0x25cc,),(0xc95,0x200d,0xc95))
    signs=((0xcc8,),(0xcc3,0xcc1),(0xcc0,0xccd,0xcc3,0xcc1))
    return [(*prefix,*tail,*((0xcc1,)*count)) for prefix in prefixes
            for tail in signs for count in range(16)]


def control_matrix_corpus():
    links=[s for n in (1,2,3) for s in itertools.product((0xccd,0xcbc,0x200c,0x200d,0xa0),repeat=n)]
    return [(base,*link,0xc95,*sign) for base in (0xc95,0xc85,0xc92,0x25cc,0xa0,0xcb0)
            for link in links for sign in ((),(0xcbf,),(0xccb,))]


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--corpus',choices=('holdouts','control-capacity','length-order','control-matrix'),default='holdouts')
    args=parser.parse_args()
    if args.output.exists():parser.error('Preserve existing evidence; choose a new output file')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    samples={'holdouts':corpus,'control-capacity':control_capacity_corpus,'length-order':length_order_corpus,'control-matrix':control_matrix_corpus}[args.corpus]()
    args.output.write_text(json.dumps(samples)+'\n',encoding='utf-8')
    print(len(samples),'run holdouts')
