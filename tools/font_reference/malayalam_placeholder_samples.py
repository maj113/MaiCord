"""First-party Malayalam placeholder/dot-reph diagnostic sequences."""
import argparse
import json
from pathlib import Path


def corpus():
    prefixes=[(),(0x200d,),(0x200c,),(0xa0,),(0x25cc,),
              (0xa0,0x200d),(0xa0,0xa0),(0x200d,0x200d)]
    bases=[(),(0xd15,),(0xd2f,),(0xd30,),(0xd32,),(0xd35,),
           (0xd05,),(0xd7b,),(0x25cc,),(0xd54,)]
    tails=[(),(0xd46,),(0xd4c,),(0xd41,),(0xd02,),
           (0x200c,),(0x200d,),(0xd4d,),(0xd3b,),(0xd4c,0xd02)]
    return list(dict.fromkeys(prefix+(0xd4e,)+base+tail
                for prefix in prefixes for base in bases for tail in tails))


def capacity_corpus():
    return [tuple([prefix]*count)+(0xd4e,)+base+tail
            for prefix in (0x200d,0xa0) for count in range(1,35)
            for base in ((),(0xd15,),(0xd2f,)) for tail in ((),(0xd46,),(0xd4c,))]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--corpus',choices=('groups','capacity'),default='groups')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();cases=capacity_corpus() if args.corpus=='capacity' else corpus();data=json.dumps(cases)
    if args.output.exists():
        if json.loads(args.output.read_text())!=json.loads(data):raise ValueError('Existing corpus differs')
    else:
        args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(data)
    print(len(cases),'original-input cases verified')


if __name__=='__main__':main()
