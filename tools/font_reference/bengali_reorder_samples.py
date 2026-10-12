"""Reproduce the bounded Bengali NBSP/mark/control diagnostic corpus."""
import argparse
import json
from pathlib import Path

def corpus():
    cases={}
    def add(value):cases.setdefault(tuple(value),None)
    for base in (0x995,0x985,0x9b0,0x9f0,0x25cc):
        for other in (0x995,0x985,0x9b0,0x25cc):
            for join in (0xa0,0x200d):
                for tail in ((),(0x9cb,),(0x9c1,)):
                    add((base,0x9cd,join,other,*tail))
        for pre in (0x9bf,0x9c7):
            for below in (0x9c1,0x9c2,0x9c3):
                for after in ((0x9cb,),(0x9cc,),(0x9c7,0x9be),(0x9be,)):
                    add((base,pre,0x9cd,below,*after))
        for matra in (0x9bf,0x9c7,0x9cb,0x9cc,0x9c1,0x9be):
            for sign in (0x981,0x982,0x983):add((base,matra,sign,0x200c))
    add((0x995,0x9cd,0x9b0,0xa0,0x9b0,0x9c1))
    return list(cases)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    args.output.write_text(json.dumps(corpus()),encoding='utf-8')
