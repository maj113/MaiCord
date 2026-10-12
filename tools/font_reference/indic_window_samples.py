"""First-party runs crossing the 32-scalar decode boundary at every alignment."""
import argparse,json
from pathlib import Path

def corpus(script):
    block={'bengali':0x980,'gujarati':0xa80,'gurmukhi':0xa00}[script]
    ca,ra,halant,pre,post,nukta=(block+x for x in (0x15,0x30,0x4d,0x3f,0x3e,0x3c))
    tails=((ca,halant,ca,pre),(ra,halant,ca,pre),(0x200d,),
        (0x200d,ca,halant,0x200d,ca),(ca,halant,0x200c,ca,pre),
        (ca,)+(post,)*30,(block+0x4b,),(ca,halant,nukta,ca,pre))
    cases={}
    for offset in range(66):
        for prefix in (ca,0x20):
            for tail in tails:cases.setdefault((prefix,)*offset+tail+(0x20,ca,pre),None)
    return list(cases)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--script',choices=('bengali','gujarati','gurmukhi'),required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();cases=corpus(args.script)
    args.output.write_text(json.dumps(cases)+'\n',encoding='utf-8')
    print(len(cases),'window-alignment runs;',args.output)
if __name__=='__main__':main()
