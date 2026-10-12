"""Export recorded Windows range observations. QA inputs, no shipping dependency."""
from pathlib import Path
import argparse,json,struct

def export(root,repo):
    profiles=[json.loads((root/name).read_text())['profile'] for name in ['profile.json','interior.json']]
    flags=bytearray(0x110000)
    for a,b,k in profiles[0]:
        if 0<k<128:flags[a:b]=bytes([1])*(b-a)
    for a,b,k in profiles[1]:
        if k==129:
            for i in range(a,b):flags[i]|=2
    assert not any(x==2 for x in flags)
    packed=[];i=0
    while i<len(flags):
        if not flags[i]:i+=1;continue
        end=i+1
        while end<len(flags) and flags[end]==flags[i] and end-i<256:end+=1
        packed.append((i<<9)|(256 if flags[i]&2 else 0)|(end-i-1));i=end
    text='// First-party Windows 10.0.29648.1000 observations; not Unicode grapheme data.\nconstexpr u32 nominal_attachment_ranges[]{\n'
    text+='\n'.join('    '+','.join(f'0x{x:08x}u' for x in packed[i:i+8])+',' for i in range(0,len(packed),8))+'\n};\n'
    (repo/'src/text/generated/nominal_range_data.inc').write_text(text)
    words=lambda *v:struct.pack('<'+'I'*len(v),*v)
    data=b'MCRNG01\0'
    for profile in profiles:data+=words(len(profile))+b''.join(words(*row) for row in profile)
    cases=json.loads((root/'neutral-ranges.json').read_text());data+=words(len(cases))
    for case in cases:
        cps=case['scalars'];ranges=case['observed']['ranges'];data+=words(len(cps),len(ranges),*cps)
        for start,length,simple,glyphs in ranges:data+=words(start,length,simple)
    (root/'range-fixture.bin').write_bytes(data)
    text='// Recorded first-party Windows prefix/range expectations.\nconstexpr u8 nominal_range_fixture[]{\n'
    text+='\n'.join('    '+','.join(f'0x{x:02x}' for x in data[i:i+24])+',' for i in range(0,len(data),24))+'\n};\n'
    (repo/'tests/generated/nominal_range_fixture.inc').write_text(text)
    print(len(packed)*4,'shipping profile bytes;',len(data),'fixture bytes;',len(cases),'mixed cases')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--repo',type=Path,required=True);a=p.parse_args();export(a.root,a.repo)
