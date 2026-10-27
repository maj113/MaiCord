"""Create oblique expectations for the existing first-party instructed TT corpus."""
from pathlib import Path
import argparse,hashlib,json,struct,subprocess
from embed_fixture import write_embedded_array
p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--header',type=Path,required=True);a=p.parse_args()
repo=Path(__file__).resolve().parents[2];folder=a.root/'first-party-oblique';folder.mkdir(exist_ok=True);fonts=[]
for name in ['tt_outline_fixture','tt_recovery_fixture']:
 data=(repo/'tests/generated'/(name+'.bin')).read_bytes()
 assert data[:8]==b'MCTTGL1\0';count=struct.unpack_from('<I',data,8)[0];at=12
 for _ in range(count):
  length,glyphs=struct.unpack_from('<II',data,at);at+=8;assert glyphs==4
  font=data[at:at+length];at+=length;fonts.append(font)
  for _ in range(glyphs):
   commands=struct.unpack_from('<I',data,at+4)[0];at+=32+commands*28
 assert at==len(data)
result=bytearray(b'MCTTGL1\0'+struct.pack('<I',len(fonts)));rows=[];exe=a.root/'bin/Release/MaiCordFontReference.exe'
for i,font in enumerate(fonts):
 path=folder/f'{i:03d}.ttf';path.write_bytes(font)
 proc=subprocess.run([str(exe),str(path),'--simulate=2','--instructions-dump'],capture_output=True,text=True)
 (folder/f'{i:03d}.txt').write_text(proc.stdout+proc.stderr)
 outlines=[list(map(int,line.split('=',1)[1].split(','))) for line in proc.stdout.splitlines() if line.startswith('gvar_outline=')]
 assert proc.returncode==0 and len(outlines)==4,(i,proc.returncode)
 result+=struct.pack('<II',len(font),len(outlines))+font
 for glyph in outlines:result+=struct.pack('<'+'I'*len(glyph),*glyph)
 rows.append(dict(font=str(path),sha256=hashlib.sha256(font).hexdigest(),glyphs=len(outlines),exit=proc.returncode))
write_embedded_array(a.header,'// Existing first-party instruction fonts; Windows oblique outline expectations.\n','static constexpr u8 tt_outline_fixture[]',result)
(folder/'observations.json').write_text(json.dumps(dict(reference=str(exe),reference_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),cases=rows,bytes=len(result)),indent=2))
print(len(fonts),'fonts',len(fonts)*4,'Windows oblique outlines',len(result),'fixture bytes')
