"""Embed Windows oblique outputs for eight first-party uniform CFF stress fonts."""
from pathlib import Path
import argparse,hashlib,json,struct,subprocess
from make_cff_fixture import make_font
from embed_fixture import write_embedded_array
p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--header',type=Path,required=True);a=p.parse_args()
folder=a.root/'first-party-cff';folder.mkdir(exist_ok=True);exe=a.root/'bin/Release/MaiCordFontReference.exe';rows=[]
cases=[('canonical',1000,.001),('rounded',1024,.00097656),('scale',1000,.002),('inverse',1000,.0011),('inverse2',1000,.0012345),('small',1000,.0007),('third',3000,.00033333),('scale107',1000,.00107)]
data=bytearray(b'MCCFFS1\0'+struct.pack('<I',len(cases)))
for name,units,scale in cases:
 path=folder/f'cff-{name}.otf';make_font(path,units,[scale,0,0,scale,0,0],True)
 r=subprocess.run([str(exe),str(path),'--simulate=2','--instructions-dump'],capture_output=True,text=True)
 (folder/(path.stem+'.txt')).write_text(r.stdout+r.stderr)
 outlines=[list(map(int,line.split('=',1)[1].split(','))) for line in r.stdout.splitlines() if line.startswith('gvar_outline=')]
 assert r.returncode==0 and len(outlines)==88,(name,r.returncode)
 font=path.read_bytes();data+=struct.pack('<II',len(font),len(outlines))+font
 for row in outlines:data+=struct.pack('<'+'I'*len(row),*row)
 rows.append(dict(path=str(path),sha256=hashlib.sha256(font).hexdigest(),glyphs=len(outlines),exit=r.returncode))
write_embedded_array(a.header,'// First-party CFF stress fonts and Windows oblique outline observations.\n','static constexpr u8 cff_oblique_fixture[]',data)
(folder/'observations.json').write_text(json.dumps(dict(executable=str(exe),reference_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),cases=rows,bytes=len(data)),indent=2))
print(len(cases),'fonts',len(cases)*88,'outlines',len(data),'fixture bytes')
