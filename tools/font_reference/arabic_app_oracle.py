"""Arabic shaping reference for the actual MaiCord fallback fonts. Local QA only."""
import argparse,json,struct,subprocess
from pathlib import Path
from gsub_execution_oracle import identity
from arabic_plan_oracle import OPTIONS,fixture

def samples():
    texts=['\u0633\u0644\u0627\u0645','\u0645\u0631\u062d\u0628\u0627','\u0627\u0644\u0639\u0631\u0628\u064a\u0629',
           '\u0644\u0627','\u0644\u0623','\u0644\u0625','\u0644\u0622','\u0628\u0628','\u0628\u0628\u0628',
           '\u0641\u0627\u0631\u0633\u06cc','\u0627\u0631\u062f\u0648','\u0623','\u0627\u0654',
           '\u0628\u200c\u0628','\u0628\u200d\u0628','\u0628\u200d','\u200d\u0628','\u0628\u200c',
           '\u0633\u0640\u0640\u0644\u0627\u0645']
    for marks in ['\u064e','\u064f','\u0650','\u0651','\u0652','\u064b','\u064c','\u064d',
                  '\u064e\u0651','\u0651\u064e','\u0651\u0650','\u0650\u0651','\u064e\u064e']:
        texts.append('\u0628'+marks+'\u0628')
    texts+=['\u064e','\u064e\u0628','\u0651\u064e\u0628',
        '\u0627\u0644\u0633\u064e\u0651\u0644\u064e\u0627\u0645\u064f \u0639\u064e\u0644\u064e\u064a\u0652\u0643\u064f\u0645\u0652 \u0644\u0627 \u0644\u0623 \u0644\u0625 \u0644\u0622 \u0661\u0662\u0663']
    return [[ord(c) for c in text] for text in texts]

def run(root,exe):
    root.mkdir(parents=True,exist_ok=True);cases=[]
    for path in [Path('C:/Windows/Fonts/segoeui.ttf'),Path('C:/Windows/Fonts/seguisb.ttf')]:
        for sample,scalars in enumerate(samples()):
            for size in [14.0,16.0,32.0]:
                em=struct.unpack('<I',struct.pack('<f',size))[0]
                for options in OPTIONS:
                    p=subprocess.run([str(exe),str(path),','.join(f'{v:x}' for v in scalars),options,str(em),'1','0','en-US'],capture_output=True,text=True,timeout=15,creationflags=0x08000000)
                    assert p.returncode==0,(path,sample,size,options,p.stdout,p.stderr)
                    result=json.loads(p.stdout);assert result['hresult']==result['placement_hresult']==0
                    cases.append(dict(name=f'{path.stem}-{sample}-{size}',sample=sample,font=identity(path),scalars=scalars,options=options,result=result))
    (root/'observations.json').write_text(json.dumps(dict(reference=identity(exe),cases=cases),indent=2))
    fixture([root],root/'corpus.bin',True);print(len(cases),'app font observations')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--exe',type=Path,required=True);a=p.parse_args();run(a.root.resolve(),a.exe.resolve())
