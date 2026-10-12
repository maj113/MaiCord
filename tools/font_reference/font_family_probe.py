"""QA-only first-party fonts for English WWS naming and property repair."""
from pathlib import Path
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
import argparse,json,subprocess,hashlib
def identity(path):
    data=path.read_bytes();return dict(path=str(path),bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
def main():
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--reuse',action='store_true');a=p.parse_args();root=a.root.resolve();folder=root/'synthetic';folder.mkdir(exist_ok=True);cases=[]
    words=['Bold','Semibold','Semi Bold','Demi','DemiBold','Light','Thin','Black','Heavy','Medium','Regular','Normal','Roman','Book','Italic','Oblique','Condensed','Narrow','Expanded','Wide','Ultra Condensed','Extra Bold']
    for word in words:
        for location in ['family','subfamily']:
            for wws in [False,True]:cases.append(dict(family='Probe '+word if location=='family' else 'Probe',subfamily='Regular' if location=='family' else word,wws=wws))
    for family in ['Bold Probe','Probe Bold Tail','Probe-Bold','ProbeBold','Probe bold','Probe Bold Italic','Probe Condensed Bold Italic','Probe  Bold','Probe Bold MT']:
        cases.append(dict(family=family,subfamily='Regular',wws=False))
    for wws in [False,True]:
        for pref in [None,'Preferred','Preferred Bold']:
            for dedicated in [None,'Dedicated','Dedicated Bold']:
                cases.append(dict(family='Legacy Bold',subfamily='Regular',wws=wws,preferred=pref,dedicated=dedicated))
    for weight in [300,400,500,600,700,900]:
        for sub in ['Regular','Light','Bold','Medium']:cases.append(dict(family='Probe Bold',subfamily=sub,wws=False,weight=weight))
    for sub in ['Caption','Display','Text','Book Italic','Semilight','ExtraLight','Roman Bold']:
        cases.append(dict(family='Probe',subfamily=sub,wws=False))
    rows=[];exe=root/'bin/Release/MaiCordFontMatchingReference.exe'
    for i,case in enumerate(cases):
        f=FontBuilder(1000,isTTF=True);f.setupGlyphOrder(['.notdef','A']);f.setupCharacterMap({65:'A'});pen=TTGlyphPen(None);pen.moveTo((0,0));pen.lineTo((400,0));pen.lineTo((400,700));pen.lineTo((0,700));pen.closePath();glyph=pen.glyph()
        f.setupGlyf({'.notdef':glyph,'A':glyph});f.setupHorizontalMetrics({'.notdef':(500,0),'A':(500,0)});f.setupHorizontalHeader(ascent=800,descent=-200)
        names=dict(familyName=case['family'],styleName=case['subfamily'],uniqueFontIdentifier=f'MaiCordFamily{i}',fullName=case['family']+' '+case['subfamily'],psName=f'MaiCordFamily{i}')
        if case.get('preferred'):names['typographicFamily']=case['preferred'];names['typographicSubfamily']=case['subfamily']
        f.setupNameTable(names)
        if case.get('dedicated'):
            for platform,encoding,language in [(3,1,0x409),(1,0,0)]:
                f.font['name'].setName(case['dedicated'],21,platform,encoding,language);f.font['name'].setName(case['subfamily'],22,platform,encoding,language)
        f.setupOS2(usWeightClass=case.get('weight',400),usWidthClass=5,fsSelection=0x40|(256 if case['wws'] else 0),sTypoAscender=800,sTypoDescender=-200,usWinAscent=800,usWinDescent=200);f.font['OS/2'].version=4;f.setupPost();f.setupMaxp()
        path=folder/f'case-{i:03d}.ttf'
        if a.reuse:assert path.is_file()
        else:f.save(path)
        process=subprocess.run([str(exe),'--families',str(path)],capture_output=True,text=True);assert process.returncode==0,process.stdout
        raw=folder/f'case-{i:03d}.jsonl';raw.write_text(process.stdout);observed=[json.loads(l) for l in process.stdout.splitlines()];families=[r for r in observed if r['kind']=='collection_family'];faces=[r for r in observed if r['kind']=='face' and not r['simulations']];assert len(families)==len(faces)==1
        row=dict(input=case,font=identity(path),observations=identity(raw),expected=dict(family=families[0]['name'],properties=faces[0]['properties']));rows.append(row)
    (root/'family-probes.json').write_text(json.dumps(dict(executable=identity(exe),rows=rows),indent=2));print(len(rows),'probes')
    for row in rows:
        c=row['input']
        if not c['wws']:print(c,'=>',row['expected'])
if __name__=='__main__':main()
