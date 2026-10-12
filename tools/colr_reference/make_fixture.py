# First-party synthetic outlines and paint trees; no system font data is copied.
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.colorLib.builder import buildCOLR,buildCPAL
from pathlib import Path
import json
shapes={}
for name,points in {'box':[(80,100),(650,100),(650,900),(80,900)],'triangle':[(250,0),(1000,300),(700,1000)],'full':[(0,0),(1000,0),(1000,1000),(0,1000)]}.items():
 pen=TTGlyphPen(None);pen.moveTo(points[0]);[pen.lineTo(p) for p in points[1:]];pen.closePath();shapes[name]=pen.glyph()
def solid(i,alpha=1):return {'Format':2,'PaletteIndex':i,'Alpha':alpha}
def mask(shape,paint):return {'Format':10,'Glyph':shape,'Paint':paint}
def line(extend=0):return {'Format':4,'ColorLine':{'Extend':extend,'ColorStop':[(0,0,.25),(.3,1,.75),(1,2,1)]},'x0':0,'y0':0,'x1':1000,'y1':0,'x2':0,'y2':1000}
def radial(extend=0):return {'Format':6,'ColorLine':{'Extend':extend,'ColorStop':[(0,0,1),(.5,1,.5),(1,2,1)]},'x0':300,'y0':300,'r0':50,'x1':650,'y1':500,'r1':400}
cases={}
for mode in range(28):cases[f'composite_{mode}']={'Format':32,'SourcePaint':mask('triangle',line()),'BackdropPaint':mask('box',solid(1,.6)),'CompositeMode':mode}
for extend in range(3):
 cases[f'linear_{extend}']=mask('full',line(extend));cases[f'radial_{extend}']=mask('full',radial(extend))
 for start,end in [(0,360),(-90,90),(90,450),(270,90),(45,45)]:
  cases[f'sweep_{extend}_{start}_{end}']=mask('full',{'Format':8,'ColorLine':line(extend)['ColorLine'],'centerX':500,'centerY':500,'startAngle':start,'endAngle':end})
cases['transform_clip']=mask('box',{'Format':12,'Transform':{'xx':1,'yx':.25,'xy':.5,'yy':.75,'dx':-200,'dy':50},'Paint':{'Format':1,'Layers':[mask('triangle',radial(2)),mask('box',line(1))]}})
cases['singular']={'Format':12,'Transform':{'xx':0,'yx':0,'xy':0,'yy':1,'dx':100,'dy':0},'Paint':mask('full',line())}
cases['nested_clip']=mask('box',mask('triangle',line()))
cases['reuse']={'Format':11,'Glyph':'linear_0'}
cases['foreground']=mask('full',solid(0xffff,.5))
# Exercise the wide evaluator and simple-contour decoding, including implied
# points, off-curve contour starts, and a metrics origin adjustment.
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphCoordinates
from array import array
curve=Glyph();curve.numberOfContours=1;curve.coordinates=GlyphCoordinates([(0,500),(500,1000),(1000,500),(500,0)]);curve.endPtsOfContours=[3];curve.flags=array('B',[0,0,0,0]);from fontTools.ttLib.tables.ttProgram import Program
curve.program=Program();curve.program.fromBytecode([]);shapes['implied_curve']=curve
cases['curve_outline']=mask('implied_curve',line())
deep=mask('box',solid(1,.6))
for i in range(10):deep={'Format':32,'SourcePaint':mask('triangle',solid(i%3,.25)),'BackdropPaint':deep,'CompositeMode':3}
cases['deep_stack']=deep
empty=TTGlyphPen(None).glyph();order=['.notdef']+list(shapes)+list(cases)
fb=FontBuilder(1000,isTTF=True);fb.setupGlyphOrder(order);fb.setupCharacterMap({0xE000+i:name for i,name in enumerate(cases)});fb.setupGlyf({n:shapes.get(n,empty) for n in order});fb.setupHorizontalMetrics({n:(1000,0) for n in order});fb.setupHorizontalHeader(ascent=1000,descent=0);fb.setupNameTable({'familyName':'MaiCord COLRv1 Tests','styleName':'Regular','uniqueFontIdentifier':'MaiCordColrTests1','fullName':'MaiCord COLRv1 Tests','psName':'MaiCordColrTests'});fb.setupOS2(sTypoAscender=1000,sTypoDescender=0,usWinAscent=1000,usWinDescent=0);fb.setupPost();fb.setupMaxp();fb.font['CPAL']=buildCPAL([[(1,.1,.1,1),(.2,.8,.2,1),(.1,.2,1,1)]]);fb.font['COLR']=buildCOLR(cases,version=1,glyphMap=fb.font.getReverseGlyphMap(),clipBoxes={n:(0,0,1000,1000) for n in cases});fb.save('paint-tests.ttf');Path('glyphs-synthetic.txt').write_text(''.join(f'{fb.font.getGlyphID(n)} 96\n' for n in cases));Path('synthetic-cases.json').write_text(json.dumps({fb.font.getGlyphID(n):n for n in cases},indent=2));print('Built',len(cases),'paint cases')
