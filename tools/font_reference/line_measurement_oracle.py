"""First-party expanded/ligated cluster geometry probes; QA only."""
from pathlib import Path
import argparse,json,struct,subprocess
from fontTools.ttLib import TTFont
from embed_fixture import write_embedded_array
from gsub_execution_oracle import make_font,multiple,ligature

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--exe',type=Path,required=True);a=p.parse_args();a.root.mkdir(parents=True,exist_ok=True)
    cases=[('expand4',multiple({'A':list('BCDE')})),('expand16',multiple({'A':list('BCDE'*4)})),('ligature',ligature('AAA',output='X')),('trailing-space',ligature('AL',output='X')),('leading-space',ligature('LA',output='X'))]
    rows=[]
    for name,operation in cases:
        path=a.root/(name+'.ttf');make_font(path,dict(name='line-'+name,lookups=[operation]))
        with TTFont(path) as f:
            f['GSUB'].table.FeatureList.FeatureRecord[0].FeatureTag='ccmp'
            for table in f['cmap'].tables:
                if table.isUnicode():table.cmap[32]='L'
            for glyph,advance in dict(A=641,B=313,C=517,D=719,E=929,X=1337,L=277).items():f['hmtx'].metrics[glyph]=(advance,0)
            f.save(path)
        for text in ['a','aaa',' a ','aaaa aa ','a aa']:
            for size in [14.,17.5,1000.]:
                bits=struct.unpack('<I',struct.pack('<f',size))[0];cps=','.join(f'{ord(c):x}' for c in text)
                output=subprocess.run([str(a.exe),str(path),cps,'default',str(bits),'0','0','en-US','hit-layout'],capture_output=True,text=True,check=True)
                rows.append(dict(name=name,font=str(path),text=text,size=size,observed=json.loads(output.stdout)))
    (a.root/'observed.json').write_text(json.dumps(rows,indent=2));print(len(rows),'first-party line cases')
    words=lambda *v:struct.pack('<'+'I'*len(v),*v)
    data=b'MCLINE1\0'+words(len(rows))
    for row in rows:
        font=Path(row['font']).read_bytes();text=row['text'].encode();o=row['observed'];glyphs=o['layout_glyphs'];clusters=o['cluster_metrics']
        data+=words(len(font),len(text),len(glyphs),len(clusters),struct.unpack('<I',struct.pack('<f',row['size']))[0])+font+text
        for glyph,placement in zip(glyphs,o['layout_placement_bits']):data+=words(glyph,*placement)
        data+=words(*o['layout_clusters'],*o['layout_metrics_bits'],o['layout_lines'][0][4],o['layout_lines'][0][1])
        at=0
        for c in clusters:
            hit=o['position_hits'][at*2];at+=c[0];data+=words(at,hit[6],c[1])
        for hit in o['position_hits']:data+=words(hit[2],hit[3],hit[4],hit[6],hit[8])
    write_embedded_array(Path(__file__).resolve().parents[2]/'tests/generated/line_measurement_fixture.inc','// Recorded first-party line widths, clusters and caret positions.\n','constexpr u8 line_measurement_fixture[]',data)
    (a.root/'fixture.bin').write_bytes(data)

if __name__=='__main__':main()
