"""Windows observations for cached first-coverage membership; QA only."""
from pathlib import Path
import argparse,copy
import gsub_execution_oracle as oracle

def scenarios():
    cases=[]
    def add(name,text,lookups):
        cases.append(dict(name=name,input_glyphs=list(text),text=''.join(oracle.TEXT[g] for g in text),lookups=lookups,
                          purpose='First-coverage cache preserves ordered, context-sensitive and nested actions',expected_enabled=None))
    for chained in (False,True):
        for count in (16,65,256,257):
            subs=[copy.deepcopy(oracle.context('XX',[],chained=chained).SubTable[0]) for _ in range(count)]
            subs[0]=oracle.context('AB',[(0,1)],chained=chained).SubTable[0]
            subs[count//2]=oracle.context('AC',[],chained=chained).SubTable[0]
            subs[-1]=oracle.context('AC',[(0,1)],chained=chained).SubTable[0]
            add(f'ordered-{int(chained)}-{count}','ABAC'*16,[oracle.lookup(6 if chained else 5,*subs),oracle.marker()])
    for count in (16,65,256):
        subs=[copy.deepcopy(oracle.context('XX',[]).SubTable[0]) for _ in range(count)]
        subs[-1]=oracle.context('AD',[(0,1)]).SubTable[0]
        add(f'negative-{count}','A'*256,[oracle.lookup(5,*subs),oracle.marker()])
    subs=[oracle.context('XX',[]).SubTable[0] for _ in range(64)]
    for i,glyph in enumerate(oracle.BASE):subs[i*5]=oracle.context(glyph,[(0,1)]).SubTable[0]
    add('eviction-eleven-glyphs',oracle.BASE*12,[oracle.lookup(5,*subs),oracle.marker()])
    outer=[oracle.context('XX',[]).SubTable[0] for _ in range(65)]
    outer[-1]=oracle.context('AB',[(0,1)]).SubTable[0]
    inner=[oracle.context('YY',[]).SubTable[0] for _ in range(65)]
    inner[-1]=oracle.context('A',[(0,2)]).SubTable[0]
    add('nested-cache-isolation','ABAC'*32,[oracle.lookup(5,*outer),oracle.lookup(5,*inner),oracle.marker()])
    return cases

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('folder',type=Path);parser.add_argument('--exe',required=True,type=Path)
    args=parser.parse_args();oracle.scenarios=scenarios;oracle.generate(args.folder)
    observed=args.folder/'observations.json';oracle.record(args.folder,args.exe,observed)
    oracle.export(args.folder,observed,args.folder/'coverage-cache.bin',None)

if __name__=='__main__':main()
