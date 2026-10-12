"""Bounded first-party Malayalam run/capacity corpora; no font payload."""
import argparse
import json
from pathlib import Path
from malayalam_cluster_samples import corpus,holdout_corpus


def capacity_corpus():
    cases={}
    for prefix in [(0xd15,),(0x200d,0xd4d,0xd15),(0xd4e,0xd15),(0xd15,0xd4d,0xd30)]:
        for sign in [0xd4a,0xd4b,0xd4c]:
            for tail in [0xd41,0xd3e,0xd46]:
                for size in range(27,35):cases[(*prefix,sign,*([tail]*(size-len(prefix)-1)))]=None
    return list(cases)


def chains():
    return [tuple([c,link]*count)+tail for count in range(1,8)
            for c,link in [(0xd15,0x200d),(0xd30,0xd4d)] for tail in [(),(0xd46,),(0xd4a,)]]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--corpus',choices=['parser','holdouts','capacity','chains'],required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    cases=[s for s,_ in corpus()] if args.corpus=='parser' else [s for s,_ in holdout_corpus()] if args.corpus=='holdouts' else capacity_corpus() if args.corpus=='capacity' else chains()
    raw=json.dumps(cases)
    if args.output.exists():
        if json.loads(args.output.read_text())!=json.loads(raw):raise ValueError('Existing corpus differs')
    else:args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(raw)
    print(len(cases),'original-input cases verified')


if __name__=='__main__':main()
