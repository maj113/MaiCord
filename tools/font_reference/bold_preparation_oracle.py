"""Inspect bold-preparation policy in a new, disposable QA process only."""
import argparse,json,re,subprocess
from pathlib import Path
from fontTools.ttLib import TTFont,newTable
from fontTools.ttLib.tables.ttProgram import Program
from bold_geometry_oracle import make_font,identity

def run(root,exe,inventory):
    root.mkdir(parents=True,exist_ok=True);files={}
    for line in inventory.read_text().splitlines():
        row=json.loads(line)
        if row['kind']=='face' and row['simulations']==1:files[row['path']]=(row['family'],0)
    for name,program in [('normal',None),('instctrl-disabled',b'\xb1\x01\x01\x8e'),('undefined-prep-call',b'\xb0\x00\x2b')]:
        path=root/f'{name}.ttf';make_font(path,2048)
        if program:
            font=TTFont(path,recalcTimestamp=False);font['prep']=newTable('prep');font['prep'].program=Program();font['prep'].program.fromBytecode(program)
            font['maxp'].maxStackElements=64;font.save(path)
        files[str(path)]=(name,1)
    cdb=Path('C:/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe')
    symbols='srv*'+str((root.parent/'symbols').resolve())+'*https://msdl.microsoft.com/download/symbols'
    command='bp DWrite!fsg_Embold; g; .printf "BOLD_POLICY grid=%d y=%d x=%d\\n", (@r8 & 0xff), wo(@rdx+0x1b4), wo(@rdx+0x1b6); q'
    records=[]
    for index,(file,(name,glyph)) in enumerate(files.items()):
        path=Path(file)
        p=subprocess.run([str(exe),str(path),'--simulate=1','--points',f'--instructions-state={glyph}'],capture_output=True,text=True,timeout=60,creationflags=0x08000000)
        log=root/f'{index}-prepare.txt';log.write_text(p.stdout+p.stderr);assert p.returncode in [0,6],(name,p.returncode)
        recovered=int(re.search(r'^instruction_prepare_missing_function=(\d+)$',p.stdout,re.M)[1])
        control=int(re.search(r'^instruction_control=(\d+)$',p.stdout,re.M)[1])
        d=subprocess.run([str(cdb),'-y',symbols,'-c',command,str(exe),str(path),'--simulate=1','--instructions'],capture_output=True,text=True,timeout=60,creationflags=0x08000000)
        debug=root/f'{index}-debug.txt';debug.write_text(d.stdout+d.stderr)
        policy=re.search(r'^BOLD_POLICY grid=(\d+) y=(\d+) x=(\d+)',d.stdout,re.M)
        assert d.returncode==0 and policy,(name,d.returncode,d.stdout[-1500:])
        grid,y,x=map(int,policy.groups());assert grid==int(not recovered),(name,grid,recovered)
        records.append(dict(name=name,font=identity(path),preparation=identity(log),debug=identity(debug),instruction_control=control,missing_function=bool(recovered),grid_fit=bool(grid),x=x,y=y))
        print(name,'grid',grid,'control',control,'recovered',recovered,'xy',x,y,flush=True)
    (root/'observations.json').write_text(json.dumps(dict(reference=identity(exe),debugger=identity(cdb),files=records),indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--exe',type=Path,required=True);p.add_argument('--inventory',type=Path,required=True)
    a=p.parse_args();run(a.root.resolve(),a.exe.resolve(),a.inventory.resolve())
