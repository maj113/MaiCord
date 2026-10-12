"""Reproduce Kannada scanner holdouts and optional feature-disabled QA font."""
import argparse
import itertools
import json
from pathlib import Path
import random

VOWELS = (0xc85,0xc86,0xc87,0xc88,0xc89,0xc8a,0xc8b,0xc8c,0xc8e,0xc8f,0xc90,0xc92,0xc93,0xc94,0xce0,0xce1)
CONSONANTS = (*range(0xc95,0xca9),*range(0xcaa,0xcb4),*range(0xcb5,0xcba),0xcdd,0xcde)
LINKS = ((0xccd,),(0xcbc,0xccd),(0xccd,0x200d),(0xccd,0x200c),(0x200d,),(0x200d,0xccd),(0xa0,),(0xccd,0x200d,0xcbc))
MARKS = (0xcbe,0xcbf,0xcc0,0xcc7,0xcc8,0xccb,0xccc,0xcd5,0xce2,0xcbc)


def vowel_joins():
    return list(dict.fromkeys((v,*link,c,m) for v,link,c,m in itertools.product(VOWELS,LINKS,CONSONANTS,MARKS)))


def vedic_controls():
    cases = {}
    vedic = (0x951,0x952,0x953,0x954,*range(0x1cd0,0x1d00),*range(0xa8e0,0xa900))
    for prefix in [(0xc95,),(0xc85,),(0xcb0,0xccd),(0xcb0,0xccd,0x200d),(0xc95,0xccd),(0xc95,0xcbf),(0xc95,0xc82),(0xc95,0xc81),(0xc95,0xc83),(0xa0,),(0x25cc,)]:
        for v,suffix in itertools.product(vedic,[(),(0x200c,),(0xc95,),(0x951,),(0x952,),(0xc82,),(0xc81,)]):
            cases[(*prefix,v,*suffix)] = None
    for control in [0x200d,0x200c,0xa0]:
        for scalar in range(0xc80,0xd00):
            for suffix in [(),(0xccd,),(0xcbc,),(0xc95,),(0xc85,),(0x200d,)]:
                cases[(0xcb0,0xccd,control,scalar,*suffix)] = None
    return list(cases)


def holdouts():
    cases = {}
    alphabet = (0xc95,0xc92,0xcb0,0xcdd,0xcde,0xc80,0xc81,0xc82,0xc83,0xcf3,
        0xcbc,0xccd,0x200c,0x200d,0xa0,0x25cc,0x1cfa,0x951,0x952,0x953,0x954,
        0xcbf,0xcc0,0xcc6,0xcc7,0xcc8,0xcca,0xccb,0xccc,0xcd5,0xcd6,0xce2)
    for prefix in [(0xcb0,0xccd,0x200d,0xcbc),(0xcb0,0xccd,0xa0,0xcbc),
                   (0xc95,0xccd,0x953),(0xc95,0xccd,0x954),
                   (0xc92,0xccd,0xcdd),(0xc92,0xccd,0xc95),(0x1cfa,)]:
        for tail in itertools.product(alphabet,repeat=2):
            cases[(*prefix,*tail)] = None
    for length in (8,9,10,11,12,13,28,29,30,31,32,33,63,64,65,100):
        for scalar in alphabet:
            cases[(0xc95,)+(scalar,)*length] = None
        for link in LINKS:
            # Keep within the observer's 256-scalar capacity.
            values = ((0xc95,*link)*length+(0xccb,))[:256]
            cases[values] = None
    rng = random.Random(202609202)
    for _ in range(2048):
        cases[tuple(rng.choice(alphabet+tuple(range(0xc80,0xd00))) for _ in range(rng.randint(1,48)))] = None
    return list(cases)


def vowel_controls():
    return list(dict.fromkeys((v,*link,c,control,tail) for v,link,c,control,tail in itertools.product(
        VOWELS,[(0xccd,),(0xccd,0x200d),(0x200d,),(0xccd,0x200d,0xcbc)],
        (0xc95,0xcdd,0xcb0),(0x200d,0x200c,0xa0),(0xc85,0xc92,0x1cfa,0xc95,0xcbc,0xccd,0xccc,0xc81,0xcf3,0x951))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--disable-feature',choices=('blwf','rphf'),default='blwf')
    parser.add_argument('--font', type=Path, help='Optional standalone font to derive without-<feature>.ttf')
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for name,build in [('vowel-joins',vowel_joins),('vedic-controls',vedic_controls),('holdouts',holdouts),('vowel-controls',vowel_controls)]:
        values = build()
        payload = json.dumps(values).encode('utf-8')
        path = args.output_dir/(name+'.json')
        if path.exists():
            if path.read_bytes()!=payload:
                raise ValueError('Existing corpus changed: '+str(path))
        else:
            path.write_bytes(payload)
        print(name,len(values))
    if args.font:
        from fontTools.ttLib import TTFont
        path = args.output_dir/('without-'+args.disable_feature+'.ttf')
        if path.exists():
            raise ValueError('Keep the existing font fixture: '+str(path))
        with TTFont(args.font) as font:
            font.recalcTimestamp = False
            for record in font['GSUB'].table.FeatureList.FeatureRecord:
                if record.FeatureTag==args.disable_feature:
                    record.Feature.LookupListIndex=[]
                    record.Feature.LookupCount=0
            font.save(path)


if __name__=='__main__':
    main()
