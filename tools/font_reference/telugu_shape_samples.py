"""First-party Telugu original-cluster inputs; no font payload."""
CONSONANTS=(*range(0xc15,0xc29),*range(0xc2a,0xc3a),0xc58,0xc59,0xc5a)
SIGNS=(0xc3c,0xc3e,0xc3f,0xc40,0xc41,0xc42,0xc43,0xc44,0xc46,0xc47,0xc48,0xc4a,0xc4b,0xc4c,0xc4d,0xc00,0xc01,0xc02,0xc03,0xc04,0xc55,0xc56,0xc62,0xc63)

def corpus():
    cases={}
    def add(s,g):cases.setdefault(tuple(s),g)
    for c in CONSONANTS:
        add((c,),'nominal')
        for sign in SIGNS:add((c,sign),'signs')
        for other in CONSONANTS:
            add((c,0xc4d,other),'conjuncts')
            add((c,0xc4d,other,0xc46),'conjunct-upper-vowel')
            add((c,0xc4d,other,0xc48),'conjunct-split-vowel')
        for control in (0x200c,0x200d):add((c,0xc4d,control),'controls')
    for sign in SIGNS:add((sign,),'orphans')
    for scalar in (0xc05,0xc06,0xc07,0xc08,0xc09,0xc0a,0xc0b,0xc0c,0xc0f,0xc10,0xc13,0xc14,0xc60,0xc61):
        add((scalar,),'independent')
    return list(cases.items())


def capacity_corpus():
    return [((*prefix,0xc48,*((0xc41,)*(count-len(prefix)-1))),'preparation-capacity')
            for prefix in ((0xc15,),(0x200d,0xc4d,0xc15)) for count in range(27,32)]


def main():
    """Extract original clusters from immutable, verified boundary reports."""
    import argparse
    import hashlib
    import json
    from pathlib import Path
    parser=argparse.ArgumentParser(description=main.__doc__)
    parser.add_argument('--boundary-report',type=Path,action='append',default=[])
    parser.add_argument('--capacity',action='store_true')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if bool(args.boundary_report)==args.capacity:
        parser.error('Choose boundary reports or --capacity')
    manifest=args.output.with_suffix('.manifest.json')
    if args.output.exists() or manifest.exists():
        parser.error('Preserve existing evidence; choose new output files')
    cases={};sources=[]
    for path in args.boundary_report:
        data=path.read_bytes();report=json.loads(data)
        if report['status'] not in ('passed','incomplete') or report['mismatches']:
            raise ValueError('A verified boundary report is required')
        sources.append(dict(path=str(path.resolve()),sha256=hashlib.sha256(data).hexdigest()))
        for row in report['rows']:
            expected=row.get('expected');scalars=row['scalars']
            if expected and sum(expected)==1 and len(scalars)<=31:
                cases.setdefault(tuple(scalars),None)
    if args.capacity:cases=dict.fromkeys(s for s,_ in capacity_corpus())
    if not cases:raise ValueError('No original clusters selected')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    data=(json.dumps(list(cases))+'\n').encode('utf-8')
    args.output.write_bytes(data)
    manifest.write_text(json.dumps(dict(reports=sources,cases=len(cases),
        sha256=hashlib.sha256(data).hexdigest(),scope='Original clusters only; glyph observations are separate.'),indent=2)+'\n',encoding='utf-8')
    print(len(cases),'original clusters')


def complex_corpus():
    import itertools
    cases={}
    def add(s,g):cases.setdefault(tuple(s),g)
    bases=(0xc15,0xc1c,0xc24,0xc26,0xc2f,0xc30,0xc35,0xc39)
    signs=(0xc3f,0xc41,0xc46,0xc48,0xc4b,0xc48,0xc3e,0xc40)
    for i,(a,b,c) in enumerate(itertools.product(bases,repeat=3)):
        add((a,0xc4d,b,0xc4d,c,signs[i%len(signs)]),'three-consonants')
    for base in bases:
        for sign in signs:
            for prefix in ((base,0xc3c),(base,0xc4d,0xc3c,0xc15),(0xc30,0xc4d,base),
                           (base,0xc4d,0x200d,0xc15),(base,0xc4d,0xc15),(base,0xc4d,0xc2f)):
                add((*prefix,sign,0xc01),'marks-and-controls')
        for pair in ((0xc3e,0xc41),(0xc3f,0xc43),(0xc46,0xc56),(0xc41,0xc41),(0xc48,0xc41)):
            add((base,*pair),'multiple-matras')
    return list(cases.items())



if __name__=='__main__':main()
