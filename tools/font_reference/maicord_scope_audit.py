"""Read-only inventory of MaiCord font choices and relevant Windows registrations."""
import argparse,json,re,hashlib,winreg
from pathlib import Path
from fontTools.ttLib import TTFont
def identity(p):return dict(path=str(p.resolve()),bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest())
def describe(p):
    with TTFont(p,fontNumber=0,lazy=True) as f:
        result=dict(file=identity(p),family=f['name'].getBestFamilyName(),subfamily=f['name'].getBestSubFamilyName(),outline='CFF2' if 'CFF2' in f else 'CFF' if 'CFF ' in f else 'glyf' if 'glyf' in f else 'other',glyphs=f['maxp'].numGlyphs,axes=[dict(tag=a.axisTag,minimum=a.minValue,default=a.defaultValue,maximum=a.maxValue) for a in f['fvar'].axes] if 'fvar' in f else [],variation_tables=[t for t in ['gvar','CFF2','HVAR','VVAR','MVAR','avar'] if t in f],colr_version=f['COLR'].version if 'COLR' in f else None)
        for kind in ['GSUB','GPOS']:
            if kind not in f:result[kind]=dict(scripts=[],features=[],lookup_types=[]);continue
            t=f[kind].table;result[kind]=dict(scripts=[s.ScriptTag for s in t.ScriptList.ScriptRecord],features=sorted({r.FeatureTag for r in t.FeatureList.FeatureRecord}),lookup_types=sorted({s.ExtensionLookupType if l.LookupType==(7 if kind=='GSUB' else 9) else l.LookupType for l in t.LookupList.Lookup for s in l.SubTable}))
        return result
def audit(repo,out):
    source=repo/'src/render/d3d11_renderer_text.cpp';text=source.read_text();section=text.split('constexpr FontAsset text_fonts[]',1)[1].split('static_assert',1)[0]
    choices=[dict(family=m[0],file=None if m[1]=='nullptr' else m[1][2:-1],tracking_expression=m[2].strip()) for m in re.findall(r'\{L"([^"]+)",\s*(nullptr|L"[^"]+"),\s*([^}]+)\}',section)]
    assert len(choices)==21;bundled=[describe(repo/'assets/fonts'/c['file']) for c in choices if c['file']];assert len(bundled)==15
    families={c['family'] for c in choices if not c['file']}|{'Segoe UI Emoji'};registered=[];system=[];seen=set()
    for hive,hive_name in [(winreg.HKEY_LOCAL_MACHINE,'HKLM'),(winreg.HKEY_CURRENT_USER,'HKCU')]:
        try:key=winreg.OpenKey(hive,r'SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts')
        except FileNotFoundError:continue
        with key:
            for i in range(winreg.QueryInfoKey(key)[1]):
                name,value,_=winreg.EnumValue(key,i)
                if not isinstance(value,str) or not any(name.casefold().startswith(family.casefold()) for family in families):continue
                p=Path(value);p=p if p.is_absolute() else Path('C:/Windows/Fonts')/p
                if not p.is_file() or str(p).casefold() in seen:continue
                seen.add(str(p).casefold());record=describe(p)
                if record['family'] not in families and not any(name.casefold().startswith(family.casefold()+' (') for family in families):continue
                registered.append(dict(hive=hive_name,name=name,value=value));system.append(record)
    paths=['src/ui/text.cpp','include/maicord/ui/text.hpp','src/render/d3d11_renderer_text.cpp','src/render/d3d11_glyph_text.cpp','src/render/dwrite_edit_layout.cpp','src/render/dwrite_message_hit_test.cpp','include/maicord/ui/text_style.hpp']
    patterns=['CreateTextFormat','text_system_initialize','SetLocaleName','AddFontFeature','GetGlyphRunOutline','CreatePaintReader','SetReadingDirection','SetFlowDirection','SetVerticalGlyphOrientation','CreateGdiCompatible','SetFontAxisValues']
    calls=[]
    for p in sorted((repo/'src').rglob('*.cpp')):
        for n,line in enumerate(p.read_text(encoding='utf-8-sig').splitlines(),1):
            if any(pattern in line for pattern in patterns):calls.append(dict(path=str(p.resolve()),line=n,text=line.strip()))
    result=dict(scope='MaiCord call-site and selected-font inventory; not proof of runtime fallback-face coverage',font_choices_source=identity(source),font_choices=choices,bundled=bundled,registered_selected_system_fonts=system,registrations=registered,sources=[identity(repo/p) for p in paths],calls=calls,explicit_feature_controls=['liga=0','clig=0','kern=0'],production_locale='en-US',writing_mode='horizontal',outline_capture_em=1.0,scope_revision='Only features required by MaiCord; not full DirectWrite compatibility')
    out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(dict(choices=len(choices),bundled=len(bundled),bundled_truetype=sum(f['outline']=='glyf' for f in bundled),bundled_cff=sum(f['outline']=='CFF' for f in bundled),bundled_variable=sum(bool(f['axes']) for f in bundled),selected_system_files=len(system),call_sites=len(calls)),indent=2))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--repo',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args();audit(a.repo,a.out)
