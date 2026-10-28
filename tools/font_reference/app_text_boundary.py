"""QA capture of the actual MaiCord profile/style/emoji/fallback layout boundary.

Uses only synthetic public-script samples, never user messages. The C++ probe
calls renderer_text_system and text_shape_utf8_styled. It creates no graphics
device/window. The resulting font inventory describes this host and corpus;
it is neither a native fallback implementation nor an exhaustive Unicode test.
"""
from pathlib import Path
import argparse, collections, hashlib, json, os, platform, struct, subprocess

SAMPLES = [
    ('latin_ascii', 'AV To office ffi ffl fi fl 123'),
    ('latin_extended', 'Živjo, čudovit svet! Příliš žluťoučký kůň. Straße café.'),
    ('latin_combining', 'a\u0301 e\u0323\u0301 o\u0308\u0304 A\u030a n\u0303'),
    ('greek', 'Καλημέρα κόσμε! ἄνθρωπος Ὀδυσσεύς'),
    ('cyrillic', 'Привет, мир! Українська мова Српски језик'),
    ('hebrew', 'שָׁלוֹם עוֹלָם 123 (שלום)'),
    ('arabic', 'السَّلَامُ عَلَيْكُمْ لا لأ لإ لآ ١٢٣'),
    ('arabic_join_controls', 'بب ب\u200cب ب\u200dب سـلام'),
    ('persian_urdu', 'فارسی می\u200cنویسم پاکستان اردو نستعلیق'),
    ('syriac', 'ܫܠܡܐ ܥܠܝܟܘܢ ܒܰܒܳܐ'),
    ('thaana', 'ދިވެހި ރާއްޖެ'),
    ('devanagari', 'नमस्ते दुनिया हिन्दी क्ष त्र ज्ञ श्र किर्कि'),
    ('devanagari_controls', 'क्\u200dष क्\u200cष र्क र्कि क़ क़ि'),
    ('bengali', 'নমস্কার বাংলা বিশ্ব ক্ষ ত্র শ্র কি'),
    ('gurmukhi', 'ਸਤ ਸ੍ਰੀ ਅਕਾਲ ਪੰਜਾਬੀ ਭਾਸ਼ਾ'),
    ('gujarati', 'નમસ્તે ગુજરાતી વિશ્વ ક્ષિ'),
    ('odia', 'ନମସ୍କାର ଓଡ଼ିଆ ବିଶ୍ୱ'),
    ('tamil', 'வணக்கம் தமிழ் உலகம் ஸ்ரீ'),
    ('telugu', 'నమస్కారం తెలుగు ప్రపంచం క్షి'),
    ('kannada', 'ನಮಸ್ಕಾರ ಕನ್ನಡ ಪ್ರಪಂಚ ಕ್ಷಿ'),
    ('malayalam', 'നമസ്കാരം മലയാളം ലോകം ക്ഷി'),
    ('sinhala', 'ආයුබෝවන් සිංහල ලෝකය'),
    ('thai', 'สวัสดีชาวโลก ภาษาไทย น้ำ เก้า'),
    ('lao', 'ສະບາຍດີ ພາສາລາວ ນ້ຳ'),
    ('khmer', 'សួស្តី ពិភពលោក ភាសាខ្មែរ'),
    ('myanmar', 'မင်္ဂလာပါ မြန်မာဘာသာ ကျောင်း'),
    ('tibetan', 'བཀྲ་ཤིས་བདེ་ལེགས་ བོད་ཡིག'),
    ('mongolian', 'ᠮᠣᠩᠭᠣᠯ ᠪᠢᠴᠢᠭ'),
    ('armenian', 'Բարեւ աշխարհ Հայերեն'),
    ('georgian', 'გამარჯობა მსოფლიო ქართული'),
    ('ethiopic', 'ሰላም ዓለም አማርኛ'),
    ('cherokee', 'ᎣᏏᏲ ᏣᎳᎩ'),
    ('han', '你好世界，简体中文。繁體中文。'),
    ('japanese', 'こんにちは世界。日本語 カタカナ ｶﾀｶﾅ'),
    ('korean', '안녕하세요 세계 한글 한글'),
    ('supplementary_scripts', '\U00010400\U00010428 \U00010330\U00010331 \U00010300\U00010301 \U00012000'),
    ('symbols', '∑∫√∞ ≠≤≥ ↔→ ✓ ♫ ⠓⠑⠇⠇⠕'),
    ('emoji_zwj', 'Hi 👩🏽\u200d💻 👨\u200d👩\u200d👧\u200d👦 🏳️\u200d🌈 🧑\u200d🚀!'),
    ('emoji_presentation', '© ©\ufe0e ©\ufe0f ♥ ♥\ufe0e ♥\ufe0f 1 1️⃣ #️⃣ *️⃣'),
    ('emoji_flags_tags', '🇸🇮 🇺🇸 🏴\U000e0067\U000e0062\U000e0065\U000e006e\U000e0067\U000e007f'),
    ('mixed_bidi', 'Hello שלום (123) مرحبا 42 world!'),
    ('bidi_isolates', 'A \u2067שלום 123\u2069 B \u2068مرحبا\u2069 C'),
    ('mixed_paragraph', 'Živjo 世界 مرحبا नमस्ते 👩🏽\u200d💻!\nSecond line שלום.'),
    ('spaces_controls', 'A\u00a0B A\u202fB A\u2009B A\u200bB A\u00adB\tC\r\nD'),
    ('unsupported_scalar', 'Visible A \U00010fffd \u0378 B'),
]

def identity(path):
    path=Path(path);data=path.read_bytes()
    return dict(path=str(path),bytes=len(data),sha256=hashlib.sha256(data).hexdigest())

def prepare(root):
    root.mkdir(parents=True,exist_ok=True);cases=[];data=bytearray(b'MCTEXTB1'+bytes(4))
    for profile in range(21):
        for label,text in SAMPLES:
            for style in ([0,1,2,3,16] if profile==0 else [0,1,2,3]):
                for independent in (False,True):
                    for width in (1000000.,120.):
                        case=dict(id=len(cases),profile=profile,sample=label,text=text,style=style,independent=independent,width=width,height=1000000.)
                        encoded=text.encode('utf-8');data+=struct.pack('<IIIIffI',case['id'],profile,style,independent,width,case['height'],len(encoded))+encoded;cases.append(case)
    struct.pack_into('<I',data,8,len(cases));(root/'corpus.bin').write_bytes(data)
    (root/'corpus.json').write_text(json.dumps(dict(scope='Synthetic multilingual application boundary observations on the current host',cases=cases),ensure_ascii=False,indent=2),encoding='utf-8')
    print('Prepared',len(cases),'cases from',len(SAMPLES),'samples',flush=True)

def capture(root,config,repeat):
    exe=root/'MaiCordAppTextBoundary.exe';env=os.environ.copy();data=root/f'data-{config.lower()}-{repeat}';data.mkdir(exist_ok=True);env['LOCALAPPDATA']=str(data)
    output=root/f'observations-{config.lower()}-{repeat}.jsonl'
    with output.open('wb') as stream:
        result=subprocess.run([str(exe),str(root/'corpus.bin')],stdout=stream,stderr=subprocess.PIPE,env=env,creationflags=subprocess.CREATE_NO_WINDOW)
    (root/f'capture-{config.lower()}-{repeat}.json').write_text(json.dumps(dict(exit=result.returncode,stderr=result.stderr.decode('utf-8',errors='replace'),executable=identity(exe),input=identity(root/'corpus.bin'),output=identity(output)),indent=2))
    assert result.returncode==0,(result.returncode,result.stderr)
    print('Captured',output,output.stat().st_size,'bytes',flush=True)

def summarize(root,config,repeat):
    from fontTools.ttLib import TTFont
    rows=[json.loads(line) for line in (root/f'observations-{config.lower()}-{repeat}.jsonl').read_text().splitlines()]
    observations={r['id']:r for r in rows if r['kind']=='case'};faces={r['id']:r for r in rows if r['kind']=='face'}
    corpus=json.loads((root/'corpus.json').read_text(encoding='utf-8'))['cases'];failures=[r for r in observations.values() if r.get('error')]
    assert rows[-1]==dict(kind='summary',cases=len(corpus),faces=len(faces),app_failures=len(failures),ok=1)
    assert len(observations)==len(corpus) and set(observations)==set(range(len(corpus)))
    def tag(value):return struct.pack('<I',value).decode('ascii')
    face_usage={i:dict(cases=0,runs=0,glyphs=0,zero_glyphs=0,samples=set(),profiles=set(),scripts=set(),styles=set()) for i in faces}
    missing=[];scripts=collections.Counter();counts=collections.Counter()
    for source in corpus:
        row=observations[source['id']];units=len(source['text'].encode('utf-16-le'))//2
        assert row['profile']==source['profile'] and row['style']==source['style'] and bool(row['independent'])==source['independent']
        if row.get('error'):assert row['utf16_units']==units;continue
        assert row['utf16_units']==units==len(row['clusters']) and row['line_count']>=1
        script_end=0
        for first,length,_,iso,_ in row['scripts']:
            assert first==script_end and length>0 and first+length<=units;script_end=first+length;scripts[tag(iso)]+=length
        assert script_end==units
        assert all(c==65535 or c<len(row['glyphs']) for c in row['clusters'])
        used=set();glyph_covered=[False]*len(row['glyphs'])
        for face,first,length,gfirst,gcount,bidi,*rest in row['runs']:
            assert face in faces and first+length<=units and gfirst+gcount<=len(row['glyphs']) and bidi<=125
            for i in range(gfirst,gfirst+gcount):assert not glyph_covered[i];glyph_covered[i]=True
            used.add(face);usage=face_usage[face];usage['runs']+=1;usage['glyphs']+=gcount;usage['samples'].add(source['sample']);usage['profiles'].add(source['profile']);usage['styles'].add(source['style'])
            for sf,sl,_,iso,_ in row['scripts']:
                if max(first,sf)<min(first+length,sf+sl):usage['scripts'].add(tag(iso))
            zero=sum(g[0]==0 for g in row['glyphs'][gfirst:gfirst+gcount]);usage['zero_glyphs']+=zero
            if zero:missing.append(dict(case=source['id'],sample=source['sample'],profile=source['profile'],style=source['style'],face=face,zero_glyphs=zero))
            counts['rtl_runs']+=bidi%2;counts['runs']+=1;counts['glyphs']+=gcount
        assert all(glyph_covered)
        for face in used:face_usage[face]['cases']+=1
    files={};inventory=[]
    for id,face in faces.items():
        path=Path(face['path']);file_identity=files.setdefault(str(path),identity(path))
        font=TTFont(path,fontNumber=face['index'],lazy=True)
        layout={}
        for table in ['GSUB','GPOS']:
            records={}
            if table in font:
                t=font[table].table
                for script in t.ScriptList.ScriptRecord:
                    languages={}
                    langs=[('default',script.Script.DefaultLangSys)]+[(l.LangSysTag,l.LangSys) for l in script.Script.LangSysRecord]
                    for name,lang in langs:
                        if lang is None:continue
                        languages[name]=dict(required=None if lang.ReqFeatureIndex==65535 else t.FeatureList.FeatureRecord[lang.ReqFeatureIndex].FeatureTag,
                            features=[t.FeatureList.FeatureRecord[i].FeatureTag for i in lang.FeatureIndex])
                    records[script.ScriptTag]=languages
            layout[table]=records
        font.close();usage=face_usage[id]
        inventory.append(face|dict(file=file_identity,axis_values={tag(t):struct.unpack('<f',struct.pack('<I',b))[0] for t,b in face['axes']},usage={k:sorted(v) if isinstance(v,set) else v for k,v in usage.items()},layout=layout))
    summary=dict(scope='Host/corpus runtime observations; no native shaping or fallback parity claim',os=platform.platform(),windows_version=list(__import__('sys').getwindowsversion()),reference_dwrite=identity('C:/Windows/System32/dwrite.dll'),cases=len(corpus),samples=len(SAMPLES),profiles=21,faces=len(faces),file_count=len(files),counts=dict(counts),scripts=dict(scripts),
        simulated_faces=sum(bool(f['simulations']) for f in inventory),raw_preparation_failures=[f['id'] for f in inventory if f['native_raw']['error']],outline_preparation_failures=[f['id'] for f in inventory if f['native_outline']['error']],
        nonsimulated_raw_failures=[f['id'] for f in inventory if not f['simulations'] and f['native_raw']['error']],nonsimulated_outline_failures=[f['id'] for f in inventory if not f['simulations'] and f['native_outline']['error']],
        app_failures=len(failures),app_failure_samples=sorted({corpus[r['id']]['sample'] for r in failures}),
        reference_zero_glyph_runs=len(missing),reference_zero_glyph_samples=sorted({r['sample'] for r in missing}),files=list(files.values()),inventory=inventory,missing=missing,failures=failures)
    (root/f'summary-{config.lower()}-{repeat}.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in summary.items() if k not in ['inventory','files','missing','failures']},ensure_ascii=True,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',choices=['prepare','capture','summarize']);p.add_argument('root',type=Path);p.add_argument('--config',choices=['Release','Debug'],default='Release');p.add_argument('--repeat',type=int,default=1);a=p.parse_args()
    if a.action=='prepare':prepare(a.root)
    elif a.action=='capture':capture(a.root,a.config,a.repeat)
    else:summarize(a.root,a.config,a.repeat)
