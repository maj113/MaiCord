"""Render the current CJK paragraph failures at four DPI scales and four origins."""
import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def identity(path):
    return dict(path=str(path),bytes=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def cases(directory):
    source=(ROOT/'tests/cjk_paragraph_cases.inc').read_text(encoding='utf-8')
    block=source.split('bool native_cjk_paragraph_corpus',1)[1].split('const float widths',1)[0]
    samples=[ast.literal_eval('b"'+value+'"').decode('utf-8')
             for value in re.findall(r'utf8_literal\("([^"\n]+)"\)',block)]
    assert len(samples)==20
    result=[]
    for family in ['Microsoft YaHei UI','Microsoft JhengHei UI']:
        path=directory/('cjk-paragraph-'+family.replace(' ','-')+'.log')
        log=path.read_text(encoding='utf-8')
        for size,section in enumerate(log.split('CJK failed cases=')[:-1]):
            rows=re.findall(r'CJK paragraph sample/pattern/flags/width/independent/nowrap=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)',section)
            assert not rows or size==0,'Pixel probe currently covers the 14 px failures'
            for row in rows:
                sample,pattern,flags,width,independent,nowrap=map(int,row)
                width=struct.unpack('<f',struct.pack('<I',width))[0]
                assert independent==0 and flags==128 and width==int(width)
                result.append(dict(family=family,sample=sample,text=samples[sample],
                                   pattern=pattern,flags=flags,width=int(width),nowrap=nowrap))
    assert result,'No current paragraph failures supplied'
    return result


def frames(raw,count=16):
    output=[];at=0
    for _ in range(count):
        assert raw[at:at+2]==b'BM','Missing streamed BMP'
        size=struct.unpack_from('<I',raw,at+2)[0]
        assert size==54+1200*970*4
        assert struct.unpack_from('<IiiHHI',raw,at+14)==(40,1200,-970,1,32,0)
        frame=memoryview(raw)[at:at+size]
        assert len(frame)==size
        assert any(value&0xffffff for value in frame[54:].cast('I')),'Blank capture'
        output.append(frame);at+=size
    assert at==len(raw),'Unexpected output after the final frame'
    return output


def render(exe,case,directory):
    directory.mkdir(parents=True);data=directory/'data';data.mkdir()
    env={k:v for k,v in os.environ.items() if not k.startswith('MAICORD_')}
    env.update(LOCALAPPDATA=str(data),MAICORD_CJK_PIXEL_STREAM='1',
               MAICORD_CJK_PIXEL_FAMILY=case['family'],MAICORD_CJK_PIXEL_TEXT=case['text'],
               MAICORD_CJK_PIXEL_CASE=f"{case['pattern']} {case['flags']} {case['width']} {case['nowrap']}")
    run=subprocess.run([str(exe)],cwd=ROOT,env=env,stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE,timeout=20)
    assert run.returncode==0,(directory,run.returncode,run.stderr.decode(errors='replace'))
    log=data/'MaiCord/logs/maicord.log'
    assert 'cjk_pixel_frames=16' in log.read_text(encoding='utf-8')
    return frames(run.stdout)


def difference(first,second):
    changed=0;maximum=0;box=[1200,970,0,0]
    for index,(a,b) in enumerate(zip(first[54:].cast('I'),second[54:].cast('I'))):
        if a==b:continue
        changed+=1;x=index%1200;y=index//1200
        box=[min(box[0],x),min(box[1],y),max(box[2],x+1),max(box[3],y+1)]
        maximum=max(maximum,*(abs(((a>>shift)&255)-((b>>shift)&255)) for shift in [0,8,16,24]))
    return dict(pixels=changed,maximum_channel_delta=maximum,bounds=box)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-exe',type=Path,required=True)
    parser.add_argument('--dwrite-exe',type=Path,required=True)
    parser.add_argument('--layout-results',type=Path,required=True)
    parser.add_argument('--output-dir',type=Path,required=True)
    args=parser.parse_args();out=args.output_dir.resolve();out.mkdir(parents=True,exist_ok=False)
    providers={name:path.resolve() for name,path in [('native',args.native_exe),('dwrite',args.dwrite_exe)]}
    binaries={name:identity(path) for name,path in providers.items()}
    selected=cases(args.layout_results.resolve());report=dict(status='running',scope=__doc__,
        binaries=binaries,cases=len(selected),dpi=[96,120,144,192],
        origins=[[16,32],[16.25,32.5],[16.5,32.25],[19.125,14.7]],pairs=0,differences=0,rows=[])
    saved=0
    try:
        for index,case in enumerate(selected):
            images={name:render(exe,case,out/'workers'/f'{index:03}'/name) for name,exe in providers.items()}
            row=dict(case=case,frames=[])
            for frame,(native,dwrite) in enumerate(zip(images['native'],images['dwrite'])):
                equal=native==dwrite;item=dict(dpi=report['dpi'][frame//4],origin=frame%4,exact=equal,
                    native_sha256=hashlib.sha256(native).hexdigest(),dwrite_sha256=hashlib.sha256(dwrite).hexdigest())
                report['pairs']+=1;report['differences']+=not equal
                if not equal:item['difference']=difference(native,dwrite)
                if (not equal and saved<1) or (index==0 and frame==0):
                    target=out/'images';target.mkdir(exist_ok=True)
                    for name,image in [('native',native),('dwrite',dwrite)]:
                        (target/f'{index:03}-{frame:02}-{name}.bmp').write_bytes(image)
                    saved+=not equal
                row['frames'].append(item)
            report['rows'].append(row)
            (out/'results.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
            print(index+1,'/',len(selected),'cases;',report['differences'],'different frames',flush=True)
        assert all(identity(path)==binaries[name] for name,path in providers.items())
        report['status']='exact' if not report['differences'] else 'differences-found'
    finally:
        (out/'results.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    return bool(report['differences'])


if __name__=='__main__':
    sys.exit(main())
