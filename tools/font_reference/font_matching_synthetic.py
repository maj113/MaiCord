"""Create first-party single-face fonts that probe bold simulation boundaries."""
from pathlib import Path
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
import argparse,json,subprocess,hashlib
def identity(path):
    data=path.read_bytes();return dict(path=str(path),bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);a=p.parse_args();root=a.root.resolve();folder=root/'synthetic';folder.mkdir(exist_ok=True);rows=[]
    for weight in [100,300,400,450,499,500,501,549,550,551,599,600,650,700]:
        family=f'MaiCord Matching {weight}';font=FontBuilder(1000,isTTF=True);font.setupGlyphOrder(['.notdef','A']);font.setupCharacterMap({65:'A'})
        pen=TTGlyphPen(None);pen.moveTo((0,0));pen.lineTo((400,0));pen.lineTo((400,700));pen.lineTo((0,700));pen.closePath();glyph=pen.glyph()
        font.setupGlyf({'.notdef':glyph,'A':glyph});font.setupHorizontalMetrics({'.notdef':(500,0),'A':(500,0)});font.setupHorizontalHeader(ascent=800,descent=-200)
        font.setupNameTable(dict(familyName=family,styleName='Regular',uniqueFontIdentifier=family,fullName=family,psName=family.replace(' ','')))
        font.setupOS2(usWeightClass=weight,usWidthClass=5,sTypoAscender=800,sTypoDescender=-200,usWinAscent=800,usWinDescent=200);font.setupPost();font.setupMaxp()
        path=folder/f'{weight}.ttf';font.save(path)
        process=subprocess.run([str(root/'bin/Release/MaiCordFontMatchingReference.exe'),family,str(path)],capture_output=True,text=True);assert process.returncode==0,process.stdout
        (folder/f'{weight}.jsonl').write_text(process.stdout);data=[json.loads(l) for l in process.stdout.splitlines()];faces={r['id']:r for r in data if r['kind']=='face'}
        queries=[dict(request=r['request'],selected=faces[r['matches'][0]]) for r in data if r['kind']=='query' and r['request'][1]==5 and r['request'][2]==0]
        rows.append(dict(weight=weight,font=identity(path),observations=identity(folder/f'{weight}.jsonl'),queries=queries))
    result=dict(executable=identity(root/'bin/Release/MaiCordFontMatchingReference.exe'),rows=rows)
    (root/'synthetic.json').write_text(json.dumps(result,indent=2));print(len(rows),'synthetic fonts')
if __name__=='__main__':main()
