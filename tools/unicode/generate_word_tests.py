"""Transcribe every official Unicode 17 word-boundary test; no algorithm here."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

PIN='2066d1909b2ea93916ce092da1c0ee4808ea3ef8407c94b4f14f5b7eb263d28e'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ucd',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--report',required=True,type=Path)
    args=parser.parse_args()
    if hashlib.sha256(args.ucd.read_bytes()).hexdigest()!=PIN:
        raise ValueError('Wrong pinned Unicode archive')
    with zipfile.ZipFile(args.ucd) as archive:
        data=archive.read('auxiliary/WordBreakTest.txt')
    vectors=[];cases=scalars=0;maximum=0
    for line,raw in enumerate(data.decode('utf-8').splitlines(),1):
        text=raw.split('#')[0].strip()
        if not text:
            continue
        tokens=text.split()
        if len(tokens)%2!=1 or any(x not in ('÷','×') for x in tokens[::2]):
            raise ValueError(f'Invalid boundary transcription at {line}')
        cps=[int(x,16) for x in tokens[1::2]]
        if not cps or any(cp>0x10ffff or 0xd800<=cp<=0xdfff for cp in cps):
            raise ValueError(f'Invalid scalar vector at {line}')
        vectors.extend([len(cps)|(0x80000000 if tokens[-1]=='÷' else 0),line])
        vectors.extend(cp|(0x80000000 if tokens[2*i]=='÷' else 0) for i,cp in enumerate(cps))
        cases+=1;scalars+=len(cps);maximum=max(maximum,len(cps))
    output='// Generated from pinned Unicode 17 WordBreakTest.txt; do not edit.\n'
    output+=f'static constexpr u32 word_vector_count={cases};\nstatic constexpr u32 word_vectors[]={{\n'
    output+=''.join('    '+','.join(f'0x{x:08x}u' for x in vectors[i:i+10])+',\n' for i in range(0,len(vectors),10))+'};\n'
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(output,encoding='utf-8',newline='\n')
    result={'unicode_version':'17.0.0','ucd_sha256':PIN,'source_sha256':hashlib.sha256(data).hexdigest(),
            'cases':cases,'scalars':scalars,'boundary_checks':scalars+cases,'maximum_case_scalars':maximum,
            'generated_sha256':hashlib.sha256(args.output.read_bytes()).hexdigest()}
    args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result))


if __name__=='__main__':
    main()
