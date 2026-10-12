"""First-party Kannada original-cluster inputs; no font payload."""
CONSONANTS=(*range(0xc95,0xca9),*range(0xcaa,0xcb4),*range(0xcb5,0xcba),0xcdd,0xcde)
SIGNS=(0xcbc,0xcbe,0xcbf,0xcc0,0xcc1,0xcc2,0xcc3,0xcc4,0xcc6,0xcc7,0xcc8,0xcca,0xccb,0xccc,0xccd,0xc81,0xc82,0xc83,0xcf3,0xcd5,0xcd6,0xce2,0xce3)

def corpus():
    cases={}
    def add(s,g):cases.setdefault(tuple(s),g)
    for c in CONSONANTS:
        add((c,),'nominal')
        for sign in SIGNS:add((c,sign),'signs')
        for other in CONSONANTS:
            add((c,0xccd,other),'conjuncts')
            add((c,0xccd,other,0xcc6),'conjunct-upper-vowel')
            add((c,0xccd,other,0xcc8),'conjunct-split-vowel')
        for control in (0x200c,0x200d):add((c,0xccd,control),'controls')
    for sign in SIGNS:add((sign,),'orphans')
    for scalar in (0xc85,0xc86,0xc87,0xc88,0xc89,0xc8a,0xc8b,0xc8c,0xc8f,0xc90,0xc93,0xc94,0xce0,0xce1):
        add((scalar,),'independent')
    return list(cases.items())


def capacity_corpus():
    return [((*prefix,0xcc8,*((0xcc1,)*(count-len(prefix)-1))),'preparation-capacity')
            for prefix in ((0xc95,),(0x200d,0xccd,0xc95)) for count in range(27,32)]


def edge_corpus():
    cases = {}
    prefixes = [(0xc95,0x200d,0xc95),(0xc95,0xa0,0xc95),(0xcb0,0xccd,0x200d,0xc95),
                (0xc95,0xccd,0x200d,0xc95),(0xc85,0x200d,0xc95),(0x25cc,0x200d,0xc95)]
    tails = [(),(0xcbf,),(0xcc8,),(0xccb,),(0xc81,),(0xccd,),(0xcbf,0xc81),
             (0x200d,0xc95),(0x200d,0xc95,0xcbf)]
    for prefix in prefixes:
        for tail in tails:cases[(*prefix,*tail)]='direct-control'
    for prefix in [(0xc95,),(0x200d,0xccd,0xc95)]:
        for sign in (0xcc0,0xcc7,0xcc8,0xcca,0xccb):
            for count in range(27,32):cases[(*prefix,sign,*((0xcc1,)*(count-len(prefix)-1)))]='split-capacity'
    for first in (0xc92,0xc95,0xc85,0xcb0):
        for control in (0x200d,0x200c):cases[(first,0xccd,0xc95,0xccd,control)]='half-request'
    return list(cases.items())


def main():
    """Extract original clusters from immutable, verified boundary reports."""
    import argparse
    import hashlib
    import json
    from pathlib import Path
    parser=argparse.ArgumentParser(description=main.__doc__)
    parser.add_argument('--boundary-report',type=Path,action='append',default=[])
    parser.add_argument('--capacity',action='store_true')
    parser.add_argument('--edge-cases',action='store_true')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if sum((bool(args.boundary_report),args.capacity,args.edge_cases))!=1:
        parser.error('Choose boundary reports, --capacity or --edge-cases')
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
    if args.edge_cases:cases=dict.fromkeys(s for s,_ in edge_corpus())
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
    bases=(0xc95,0xc9c,0xca4,0xca6,0xcaf,0xcb0,0xcb5,0xcb9)
    signs=(0xcbf,0xcc1,0xcc6,0xcc8,0xccb,0xcc8,0xcbe,0xcc0)
    for i,(a,b,c) in enumerate(itertools.product(bases,repeat=3)):
        add((a,0xccd,b,0xccd,c,signs[i%len(signs)]),'three-consonants')
    for base in bases:
        for sign in signs:
            for prefix in ((base,0xcbc),(base,0xccd,0xcbc,0xc95),(0xcb0,0xccd,base),
                           (base,0xccd,0x200d,0xc95),(base,0xccd,0xc95),(base,0xccd,0xcaf)):
                add((*prefix,sign,0xc81),'marks-and-controls')
        for pair in ((0xcbe,0xcc1),(0xcbf,0xcc3),(0xcc6,0xcd6),(0xcc1,0xcc1),(0xcc8,0xcc1)):
            add((base,*pair),'multiple-matras')
    return list(cases.items())



if __name__=='__main__':main()
