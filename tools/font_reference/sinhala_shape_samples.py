"""Authored valid Sinhala clusters covering every consonant and basic sign."""
import argparse
import json
from pathlib import Path


def corpus():
    consonants=(*range(0xd9a,0xdb2),*range(0xdb3,0xdbc),0xdbd,*range(0xdc0,0xdc7))
    signs=(0xdca,*range(0xdcf,0xdd5),0xdd6,*range(0xdd8,0xde0),0xdf2,0xdf3,0xd81,0xd82,0xd83)
    result={}
    def add(s):result.setdefault(tuple(s),None)
    for c in consonants:
        add((c,))
        for sign in signs:add((c,sign))
        for d in consonants:
            for join in ((),(0x200d,)):
                for suffix in ((),(0xdd9,),(0xddd,)):
                    add((c,0xdca,*join,d,*suffix))
    for sign in signs:add((sign,))
    return list(result)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);args=p.parse_args()
    args.output.write_text(json.dumps(corpus()),encoding='utf-8')
