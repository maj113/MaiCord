"""Generate first-party prep missing-function recovery probes and Windows outlines."""
from pathlib import Path
import argparse,io,json,re,struct,subprocess
from fontTools.ttLib import TTFont,newTable
from fontTools.ttLib.tables.ttProgram import Program
from embed_fixture import write_embedded_array
p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--header',type=Path);args=p.parse_args()
root=args.root;folder=root/'instruction-recovery';folder.mkdir(exist_ok=True)
blob=(Path(__file__).resolve().parents[2]/'tests/generated/tt_outline_fixture.bin').read_bytes()
size,n=struct.unpack_from('<II',blob,12);original=blob[20:20+size];assert n==4
def code(assembly):p=Program();p.fromAssembly(assembly);return p
cases={
 'no-fpgm-empty-prep':(None,[]),
 'no-fpgm-fault-prep':(None,['PUSHB[ ]','89','CALL[ ]']),
 'empty-fpgm-fault-prep':([],['PUSHB[ ]','89','CALL[ ]']),
 'unrelated-fpgm-fault-prep':(['PUSHB[ ]','0','FDEF[ ]','ENDF[ ]'],['PUSHB[ ]','89','CALL[ ]']),
 'valid-call-prep':(['PUSHB[ ]','89','FDEF[ ]','ENDF[ ]'],['PUSHB[ ]','89','CALL[ ]']),
 'fault-after-disable':(None,['PUSHB[ ]','1','1','INSTCTRL[ ]','PUSHB[ ]','89','CALL[ ]']),
 'loopcall-missing':(None,['PUSHB[ ]','2','89','LOOPCALL[ ]']),
 'call-outside-declared':(None,['PUSHB[ ]','200','CALL[ ]']),
 'nested-call-missing':(['PUSHB[ ]','0','FDEF[ ]','PUSHB[ ]','89','CALL[ ]','ENDF[ ]'],['PUSHB[ ]','0','CALL[ ]']),
}
rows=[];exe=root/'bin/Release/MaiCordFontReference.exe'
for name,(fpgm,prep) in cases.items():
 f=TTFont(io.BytesIO(original),recalcTimestamp=False)
 if 'fpgm' in f:del f['fpgm']
 if fpgm is not None:f['fpgm']=newTable('fpgm');f['fpgm'].program=code(fpgm)
 f['prep']=newTable('prep');f['prep'].program=code(prep)
 f['glyf']['.notdef'].program=code(['SVTCA[1]','PUSHB[ ]','0','64','SHPIX[ ]'])
 f['maxp'].maxFunctionDefs=100;f['maxp'].maxStackElements=256
 path=folder/(name+'.ttf');f.save(path)
 outputs={}
 for mode in ['geometry-dump','instructions-dump']:
  result=subprocess.run([str(exe),str(path),'--'+mode],capture_output=True,text=True)
  (folder/(name+'-'+mode+'.txt')).write_text(result.stdout+result.stderr)
  outlines=[list(map(int,line.split('=',1)[1].split(','))) for line in result.stdout.splitlines() if line.startswith('gvar_outline=')]
  outputs[mode]=dict(exit=result.returncode,metrics={k:int(v) for k,v in re.findall(r'^(\w+)=(\d+)$',result.stdout,re.M)},outlines=outlines)
 rows.append(dict(name=name,outputs=outputs))
(folder/'observations.json').write_text(json.dumps(rows,indent=2))
for r in rows:
 print(r['name'],[(k,v['exit'],v['metrics'].get('outline_differences'),v['metrics'].get('instruction_control'),v['metrics'].get('instruction_prepare_status')) for k,v in r['outputs'].items()])
if args.header:
 data=bytearray(b'MCTTGL1\0'+struct.pack('<I',len(rows)))
 for row in rows:
  result=row['outputs']['geometry-dump'];outlines=result['outlines'];assert result['exit'] in [0,6] and len(outlines)==4
  font=(folder/(row['name']+'.ttf')).read_bytes();data+=struct.pack('<II',len(font),len(outlines))+font
  for outline in outlines:data+=struct.pack('<'+'I'*len(outline),*outline)
 write_embedded_array(args.header,'// First-party missing-function recovery fonts and Windows outline observations.\n','static constexpr u8 tt_recovery_fixture[]',data)
