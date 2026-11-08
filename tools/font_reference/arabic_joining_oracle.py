"""First-party Arabic joining QA font: every joining character and its isol/fina/medi/init form glyphs."""
from fontTools.fontBuilder import FontBuilder
from fontTools.ttLib import newTable
from fontTools.ttLib.tables._g_l_y_f import Glyph
from feature_plan_oracle import layout,language,script
from gsub_execution_oracle import lookup,single

SCALARS=[0x628,0x627,0x621,0x640,0x64e,0x200c,0x200d]
FORMS=['isol','fina','medi','init']
def make(path):
    base={c:f'u{c:04x}' for c in SCALARS};order=['.notdef','space']+list(base.values())+[n+'_'+f for n in base.values() for f in FORMS]
    fb=FontBuilder(1000,isTTF=True);fb.setupGlyphOrder(order);fb.setupCharacterMap({32:'space',**base});glyphs={}
    for n in order:g=Glyph();g.numberOfContours=0;glyphs[n]=g
    fb.setupGlyf(glyphs);fb.setupHorizontalMetrics({n:(600,0) for n in order});fb.setupHorizontalHeader(ascent=800,descent=-200)
    fb.setupOS2(sTypoAscender=800,sTypoDescender=-200,usWinAscent=800,usWinDescent=200)
    fb.setupNameTable({'familyName':'MaiCord Arabic Joining QA','styleName':'Regular','fullName':'MaiCord Arabic Joining QA','psName':'MaiCordArabicJoiningQA','uniqueFontIdentifier':'MaiCord Arabic Joining QA'})
    fb.setupPost();fb.setupMaxp();font=fb.font;features=sorted((tag,[i]) for i,tag in enumerate(FORMS));lookups=[lookup(1,single({n:n+'_'+tag for n in base.values()})) for tag in FORMS]
    font['GSUB']=newTable('GSUB');font['GSUB'].table=layout('GSUB',features,lookups,{'arab':script(language(range(4)))})
    font['head'].created=font['head'].modified=3406620153;font.save(path);return order

