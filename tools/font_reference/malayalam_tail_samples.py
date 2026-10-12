"""Authored Malayalam pre-base, lower-vowel and terminal-control variations."""
import argparse
import json
from pathlib import Path


def corpus(group,seed=2026092002,samples=512):
    cases={}
    if group=='pref':
        for base in (0xd15,0xd54,0xd05,0xd7b,0xd2f,0xd35):
            for link in [(0xd4d,),(0xa0,0xd4d),(0x200d,0xd4d),(0x200c,0xd4d),(0xa0,0xa0,0xd4d),(0xa0,0x200d,0xd4d),(0xd4d,0xa0),(0xd4d,0x200d)]:
                for tail in [(),(0xd46,),(0xd4a,),(0xd41,),(0xd02,),(0xd4c,0xd02)]:
                    cases[(base,)+link+(0xd30,)+tail]=None
    elif group=='lower':
        for first in (0xd15,0xd35,0xd05,0xd54):
            for link in [(0xd4d,),(0xd4d,0xa0),(0xd4d,0x200d),(0xd4d,0x200c),(0xa0,),(0x200d,)]:
                for last in (0xd2f,0xd35,0xd32,0xd30,0xd15):
                    for sign in (0xd43,0xd62,0xd41):
                        for tail in [(),(0xd03,)]:cases[(first,)+link+(last,sign)+tail]=None
    elif group in ('accents','accents-wide'):
        for first in ((),(0xd15,),(0xd54,),(0xd05,),(0xd2f,),(0xd35,),(0xd7b,)):
            for last in (0xd15,0xd54,0xd2f,0xd30,0xd35,0xd7b):
                links=((0xd4d,),(0xd4d,0x200d)) if group=='accents' else ((),(0x200d,),(0xa0,),(0xd4d,),(0xd4d,0x200d),(0xd4d,0xa0),(0x200c,0xd4d),(0x200d,0xd4d))
                for link in links if first else ((),):
                    for sign in (0xd01,0xd02,0xd03,0x951,0x952,0x953,0x954):
                        cases[first+link+(last,0xd4d,sign)]=None
    elif group=='control':
        for base in (0xd12,0xd15,0xd54,0xd05):
            for first in (0xd3f,0xd41,0xd46):
                for second in [(),(0xd4a,),(0xd46,),(0xd4c,)]:
                    for lower in (0xd43,0xd62,0xd41):
                        for end in [(0xd02,0x200c),(0xd03,0x200c)]:
                            cases[(base,first)+second+(0xd4d,lower)+end]=None
    else:
        from malayalam_cluster_samples import holdout_corpus
        cases={tuple(s):None for s,g in holdout_corpus(seed,samples) if g=='random-holdout'}
    return list(cases)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--corpus',choices=('pref','lower','control','accents','accents-wide','holdout'),required=True)
    parser.add_argument('--seed',type=int,default=2026092002)
    parser.add_argument('--samples',type=int,default=512)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if not 1<=args.samples<=2048:parser.error('Use 1..2048 samples per bounded corpus')
    cases=corpus(args.corpus,args.seed,args.samples);data=json.dumps(cases)
    if args.output.exists():
        if json.loads(args.output.read_text())!=json.loads(data):raise ValueError('Existing corpus differs')
    else:args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(data)
    print(len(cases),'original-input cases verified')


if __name__=='__main__':main()
