"""Compare native ASCII frontend output with Windows TextLayout (QA only)."""
from pathlib import Path
import argparse,hashlib,json,struct,subprocess

TEXTS=['a','office AV To','ffi ffl fl fi','AVATAR WaYo','Browse Channels',
       'Search','Message #general','1234567890','https://discord.com',
       'Hello, world!','ABCDEFGHIJKLMNOPQRSTUVWXYZ','abcdefghijklmnopqrstuvwxyz']
SIZES=[14.0,17.5,1000.0]

def run(args):
    result=subprocess.run([str(x) for x in args],capture_output=True,text=True)
    if result.returncode:
        raise RuntimeError((args,result.returncode,result.stdout,result.stderr))
    return result.stdout

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path)
    p.add_argument('--inventory',type=Path,required=True)
    p.add_argument('--config',choices=['Release','Debug'],required=True)
    p.add_argument('--reuse',type=Path)
    a=p.parse_args();native=a.root/f'MaiCordRunPlanTests.exe'
    reference=a.root/'bin/Release/MaiCordFeaturePlanReference.exe'
    previous=json.loads(a.reuse.read_text()) if a.reuse else None
    rows=[];differences=[];unsupported=0
    for item in json.loads(a.inventory.read_text()):
        font=item['font'];path=Path(font['path'])
        assert hashlib.sha256(path.read_bytes()).hexdigest()==font['sha256']
        for weight in ([0,400,600] if path.stem.lower()=='bahnschrift' else [0]):
            for text in TEXTS:
                cps=','.join(f'{ord(c):x}' for c in text)
                for independent in [False,True]:
                    features='liga=0,clig=0,kern=0' if independent else 'default'
                    for size in SIZES:
                        bits=struct.unpack('<I',struct.pack('<f',size))[0]
                        key=dict(font=font,weight=weight,text=text,independent=independent,size=size)
                        if previous is None:
                            args=[reference,path,cps,features,bits,0,0,'en-US','layout']
                            if weight:args.append(weight)
                            observed=json.loads(run(args))
                        else:
                            old=previous[len(rows)]
                            assert all(old[k]==v for k,v in key.items())
                            observed=old['reference']
                        actual=json.loads(run([native,path,weight,f'ascii:{bits}:{int(independent)}:{cps}']).splitlines()[0])
                        row=dict(**key,reference=observed,native=actual);rows.append(row)
                        assert observed['hresult']==observed['layout_hresult']==0
                        if actual['status']==8:
                            assert not actual['glyphs'] and not actual['clusters']
                            unsupported+=1
                        else:
                            assert actual['status']==0
                            if any(actual[k]!=observed['layout_'+k] for k in ['glyphs','clusters','placement_bits']):
                                differences.append(row)
        print(path.name,len(rows),len(differences),'differences',flush=True)
    if previous is not None:assert len(rows)==len(previous)
    suffix=a.config.lower()
    (a.root/f'ascii-{suffix}.json').write_text(json.dumps(rows,indent=2))
    (a.root/f'ascii-differences-{suffix}.json').write_text(json.dumps(differences,indent=2))
    print(f'{len(rows)} cases; {unsupported} require fallback; {len(differences)} differences')
    if differences:raise SystemExit(1)

if __name__=='__main__':main()
