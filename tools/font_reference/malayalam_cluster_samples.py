"""Deterministic original-input Malayalam boundary corpus; QA only."""
import argparse
import itertools
import json
import random
from pathlib import Path


def corpus():
    cases={}
    def add(s,group):
        s=tuple(map(ord,s)) if isinstance(s,str) else tuple(s)
        if s:cases.setdefault(s,group)
    for scalar in range(0xd00,0xd80):
        for s in ((scalar,),(0xd15,scalar),(scalar,0xd4d,0xd15),(0xd15,scalar,0xd46),(0xd15,scalar,scalar)):
            add(s,'scalar-coverage')
    alphabet=(0xd15,0xd30,0xd05,0xd3b,0xd3c,0xd4d,0x200c,0x200d,0xd46,0xd3f,0xd3e,0xd41,0xd4b,0xd02)
    for base in (0xd15,0xd30,0xd05,0xa0,0x25cc,0xd46,0x200d):
        for pair in itertools.product(alphabet,repeat=2):add((base,*pair),'pairs')
    for prefix in ((0xd15,0xd4d),(0xd15,0xd46),(0xd30,0xd4d),(0xd15,0xd4d,0x200d),(0xd15,0xd4d,0x200c)):
        for pair in itertools.product(alphabet,repeat=2):add((*prefix,*pair),'tails')
    for word in 'മലയാളം ഭാഷ നമസ്കാരം കേരളം വാർത്ത സുഹൃത്ത് കമ്പ്യൂട്ടർ തിരുവനന്തപുരം ശ്രദ്ദ ക്ഷേത്രം കൊ കോ കൗ ൻ ർ ൽ ൾ ൿ'.split():
        add(word,'words')
        for end in range(1,len(word)):add(word[:end],'partial-input')
    for base in (0xd54,0xd55,0xd56,*range(0xd7a,0xd80),0xd4e):
        for other in (*range(0xd3b,0xd4f),0xd57,0xd62,0xd63,0xd15,0x200c,0x200d,0xa0,0x25cc):
            add((base,other),'chillu-reph')
            add((0xd15,0xd4d,base,other),'chillu-conjunct')
    for base in (0xd05,0xd06,0xd0b,0xd12,0xd15,0xd30,0xd7b,0x25cc):
        for signs in itertools.product((0xd3e,0xd41,0xd43,0xd46,0xd47,0xd48,0xd4a,0xd4b,0xd4c,0xd57),repeat=2):
            add((base,*signs),'vowel-pairs')
    for c in (0xd15,0xd23,0xd28,0xd30,0xd32,0xd33):
        for link in ((0xd4d,),(0xd4d,0x200d),(0xd4d,0x200c),(0x200d,)):
            add((c,*link)*40+(0xd46,),'long')
    for sign in (0xd46,0xd3e,0xd4b,0xd02,0xd03,0x200c):add((0xd15,)+(sign,)*100,'long')
    return list(cases.items())


def matra_corpus():
    signs=(0xd3b,0xd3c,0xd3e,0xd3f,0xd40,0xd41,0xd42,0xd43,0xd44,0xd46,0xd47,0xd48,0xd4a,0xd4b,0xd4c,0xd57,0xd62,0xd63)
    cases={(0xd15,*tail):'matra-triples' for tail in itertools.product(signs,repeat=3)}
    for prefix in ((0xd05,),(0xd0e,),(0xd12,),(0xd15,0xd4d,0xd2f),(0xd4e,0xd15),(0xd7b,),(0xa0,),(0x25cc,)):
        for tail in itertools.product(signs,repeat=2):cases[(*prefix,*tail)]='matra-prefixes'
    return list(cases.items())


def join_corpus():
    vowels=(0xd05,0xd06,0xd07,0xd08,0xd09,0xd0a,0xd0b,0xd0c,0xd0e,0xd0f,0xd10,0xd12,0xd13,0xd14,0xd5f,0xd60,0xd61)
    bases=(0xd15,0xd2f,0xd30,0xd35,0xd3a,0xd54,0xd55,0xd56,*range(0xd7a,0xd80),0x25cc)
    cases={}
    for vowel in vowels:
        for link in ((0xd4d,),(0xd4d,0x200d),(0xd4d,0x200c),(0x200d,),(0x200d,0xd4d),(0xa0,)):
            for base in bases:
                for sign in (0xd3e,0xd46,0xd4c,0xd57,0xd3b,0x952):cases[(vowel,*link,base,sign)]='vowel-joins'
    for scalar in range(0xd00,0xd80):
        for tail in ((),(0xd4d,0xd15),(0xd3e,),(0x200d,0xd15)):
            cases[(0xd4e,scalar,*tail)]='dot-reph'
    return list(cases.items())


def holdout_corpus(seed=2026100101,samples=2048):
    cases={}
    alphabet=(0xd00,0xd01,0xd02,0xd03,0xd04,0xd05,0xd0e,0xd12,0xd15,0xd28,0xd2f,0xd30,0xd35,0xd3b,0xd3c,0xd3e,0xd3f,0xd41,0xd43,0xd46,0xd48,0xd4a,0xd4b,0xd4c,0xd4d,0xd4e,0xd54,0xd57,0xd62,0xd7b,0xd7f,0x951,0x952,0x953,0x954,0x1ce2,0x1cfa,0xa8e0,0xa8fe,0x200c,0x200d,0xa0,0x25cc,0x20)
    rng=random.Random(seed)
    for _ in range(samples):cases[tuple(rng.choices(alphabet,k=rng.randint(1,96)))]='random-holdout'
    for base in (0xd15,0xd30,0xd4e,0xd7b,0x25cc):
        for link in itertools.product((0xd4d,0xd3b,0x200c,0x200d,0xa0),repeat=3):
            cases[(base,*link,0xd15,0xd4b)]='control-holdout'
    return list(cases.items())


def vowel_controls_corpus():
    vowels=(0xd05,0xd07,0xd09,0xd0e,0xd12,0x1cfa)
    links=((0xd4d,),(0xd4d,0x200d),(0x200d,),(0x200d,0xd4d))
    return [(s,'vowel-controls') for s in dict.fromkeys((v,*link,c,control,tail)
        for v,link,c,control,tail in itertools.product(vowels,links,(0xd15,0xd2f,0xd30,0xd35,0xd7b),
        (0x200d,0x200c,0xa0),(0xd05,0xd0e,0x1cfa,0xd15,0xd4d,0xd3b,0xd3e,0xd46,0xd57,0xd01,0xd03,0x951)))]


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--corpus',choices=('basic','matras','joins','holdouts','vowel-controls'),default='basic')
    parser.add_argument('--font',type=Path,help='Optional font to derive with post-base feature lookups disabled')
    parser.add_argument('--without-pstf',type=Path)
    args=parser.parse_args()
    if bool(args.font)!=bool(args.without_pstf):parser.error('--font and --without-pstf must be supplied together')
    if args.output.exists():parser.error('Preserve existing evidence; choose a new output file')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    samples={"basic":corpus,"matras":matra_corpus,"joins":join_corpus,"holdouts":holdout_corpus,"vowel-controls":vowel_controls_corpus}[args.corpus]();args.output.write_text(json.dumps([s for s,g in samples])+'\n',encoding='utf-8')
    print(len(samples),'original-input samples')

    if args.font:
        from fontTools.ttLib import TTFont
        if args.without_pstf.exists():raise ValueError('Preserve existing font fixture')
        with TTFont(args.font,recalcTimestamp=False) as font:
            for record in font['GSUB'].table.FeatureList.FeatureRecord:
                if record.FeatureTag=='pstf':record.Feature.LookupListIndex=[];record.Feature.LookupCount=0
            args.without_pstf.parent.mkdir(parents=True,exist_ok=True);font.save(args.without_pstf)
