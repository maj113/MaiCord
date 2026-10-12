"""First-party Malayalam original-cluster inputs; no font payload."""
CONSONANTS=tuple(range(0xd15,0xd3b))
SIGNS=(0xd3b,0xd3c,*range(0xd3e,0xd45),0xd46,0xd47,0xd48,0xd4a,0xd4b,0xd4c,0xd4d,0xd57,0xd62,0xd63,0xd00,0xd01,0xd02,0xd03)

def corpus():
    cases={}
    def add(s,g):cases.setdefault(tuple(s),g)
    for c in CONSONANTS:
        add((c,),'nominal')
        for sign in SIGNS:add((c,sign),'signs')
        for other in CONSONANTS:
            add((c,0xd4d,other),'conjuncts')
            add((c,0xd4d,other,0xd46),'conjunct-left-vowel')
            add((c,0xd4d,other,0xd4a),'conjunct-split-vowel')
        for control in (0x200c,0x200d):add((c,0xd4d,control),'controls')
    for sign in SIGNS:add((sign,),'orphans')
    for scalar in (*range(0xd05,0xd0d),0xd0e,0xd0f,0xd10,0xd12,0xd13,0xd14,0xd60,0xd61):add((scalar,),'independent')
    for scalar in (0xd54,0xd55,0xd56,*range(0xd7a,0xd80)):add((scalar,),'atomic-chillu')
    for c in CONSONANTS:
        for tail in ((),(0xd46,),(0xd4a,)):add((0xd4e,c,*tail),'dot-reph')
    return list(cases.items())


def main():
    """Extract deduplicated, scanner-verified original clusters for shaping."""
    import argparse
    import hashlib
    import json
    from pathlib import Path
    parser=argparse.ArgumentParser(description=main.__doc__)
    parser.add_argument('--boundary-report',type=Path,action='append',required=True)
    parser.add_argument('--output-dir',type=Path,required=True)
    args=parser.parse_args();cases={};sources=[];identity=None
    for path in args.boundary_report:
        raw=path.read_bytes();report=json.loads(raw)
        if report['status'] not in ('passed','incomplete') or report['mismatches']:
            raise ValueError('A verified boundary report is required')
        keys=[v['sha256'] for v in report['inputs']]
        if identity is not None and keys!=identity:raise ValueError('Boundary font/observer identity differs')
        identity=keys;sources.append(dict(path=str(path.resolve()),sha256=hashlib.sha256(raw).hexdigest()))
        for row in report['rows']:
            expected=row.get('expected');scalars=row['scalars']
            if expected and sum(expected)==1 and len(scalars)<=31:
                if not row['exact']:raise ValueError('Unverified original cluster')
                cases.setdefault(tuple(scalars),None)
    if not cases:raise ValueError('No original clusters selected')
    args.output_dir.mkdir(parents=True,exist_ok=True);outputs=[]
    for batch,start in enumerate(range(0,len(cases),6000)):
        raw=json.dumps(list(cases)[start:start+6000]).encode('utf-8');path=args.output_dir/f'original-{batch}.json'
        if path.exists() and path.read_bytes()!=raw:raise ValueError('Existing corpus differs: '+str(path))
        if not path.exists():path.write_bytes(raw)
        outputs.append(dict(path=str(path.resolve()),sha256=hashlib.sha256(raw).hexdigest()))
    manifest=args.output_dir/'original-corpora.json'
    raw=(json.dumps(dict(cases=len(cases),inputs=identity,reports=sources,outputs=outputs),indent=2)+'\n').encode('utf-8')
    if manifest.exists() and manifest.read_bytes()!=raw:raise ValueError('Existing manifest differs')
    if not manifest.exists():manifest.write_bytes(raw)
    print(len(cases),'original clusters in',len(outputs),'bounded batches')


if __name__=='__main__':main()
