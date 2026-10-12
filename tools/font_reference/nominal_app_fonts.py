"""Compare native font participation with Windows on actual MaiCord font files."""
import argparse,json,string,subprocess,struct
from pathlib import Path
from fontTools.ttLib import TTFont
from feature_plan_oracle import identity

def invoke(command):
    r=subprocess.run([str(x) for x in command],capture_output=True,text=True,timeout=20)
    assert r.returncode==0,(command,r.returncode,r.stdout,r.stderr)
    return json.loads(r.stdout)

def run(scope,native,reference,out,printable=False,variable_weights=False):
    inventory=json.loads(scope.read_text());fonts=[];checks=0;mismatches=[]
    for entry in inventory['bundled']+inventory['registered_selected_system_fonts']:
        p=Path(entry['file']['path']);assert identity(p)==entry['file'];bits=invoke([native,p]);probes=[]
        if bits['status']==0:
            with TTFont(p,fontNumber=0,lazy=True) as f:
                cmap=f.getBestCmap();names=f.getGlyphOrder();indices={n:i for i,n in enumerate(names)}
                for weight in ([400,600] if variable_weights and entry['axes'] else [None]):
                    for char in (''.join(chr(cp) for cp in range(32,127)) if printable else string.ascii_letters):
                        glyph=indices.get(cmap.get(ord(char)),0)
                        value=invoke([reference,p,f'{ord(char):x}','default','1148846080','0','0','en-US','layout']+([str(weight)] if weight else []))
                        if weight is not None:
                            expected_weight=struct.unpack('<I',struct.pack('<f',weight))[0]
                            assert value['face_weight_bits']==value['layout_weight_bits']==expected_weight,(p,weight,value)
                        expected=bool(value['text_simple']);actual=glyph!=0 and not ((bits['bits'][glyph//64]>>(glyph%64))&1)
                        probes.append(dict(text=char,glyph=glyph,weight=weight,native_simple=actual,result=value));checks+=1
                        if actual!=expected:mismatches.append(dict(font=str(p),text=char,glyph=glyph,weight=weight,native_simple=actual,windows_simple=expected))
        fonts.append(dict(font=entry['file'],family=entry['family'],native=bits,probes=probes))
    faces=' at static default faces and variable weights 400/600' if variable_weights else ' at recorded default faces'
    data=dict(scope=('Printable ASCII' if printable else 'ASCII letters')+faces+'; font participation only, not fallback/whole-layout parity',native_executable=identity(native),reference_executable=identity(reference),fonts=fonts,checks=checks,mismatches=mismatches)
    out.write_text(json.dumps(data,indent=2)+'\n')
    print(json.dumps(dict(fonts=len(fonts),checks=checks,mismatches=mismatches,unsupported=[dict(font=f['font']['path'],status=f['native']['status']) for f in fonts if f['native']['status']]),indent=2))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--scope',type=Path,required=True);p.add_argument('--native',type=Path,required=True);p.add_argument('--reference',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--ascii-printable',action='store_true');p.add_argument('--variable-weights',action='store_true');a=p.parse_args();run(a.scope,a.native,a.reference,a.out,a.ascii_printable,a.variable_weights)
