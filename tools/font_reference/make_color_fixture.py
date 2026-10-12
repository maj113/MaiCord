"""First-party COLR format/palette fixture; fontTools is a development writer."""
import argparse
from pathlib import Path
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.colorLib.builder import buildCOLR, buildCPAL
from fontTools.varLib.builder import buildVarRegionList, buildVarData, buildVarStore


def build(path,stress=False,empty_index_map=False,avar_remap=False,map_last_active=False,outline_variation=False):
    def solid():return {'Format':2,'PaletteIndex':1,'Alpha':.5}
    def line(variable):
        stops=[{'StopOffset':offset,'PaletteIndex':palette,'Alpha':alpha} for offset,palette,alpha in [(1,0,.25),(0,1,.5),(0,0xffff,1)]]
        if variable:
            for i,stop in enumerate(stops):stop['VarIndexBase']=i*2
        return {'Extend':2,'ColorStop':stops}
    transforms={14:{'dx':-25,'dy':35},16:{'scaleX':.75,'scaleY':-1.25},
                18:{'scaleX':.75,'scaleY':-1.25,'centerX':100,'centerY':-200},
                20:{'scale':.75},22:{'scale':.75,'centerX':100,'centerY':-200},
                24:{'angle':45},26:{'angle':45,'centerX':100,'centerY':-200},
                28:{'xSkewAngle':30,'ySkewAngle':-15},
                30:{'xSkewAngle':30,'ySkewAngle':-15,'centerX':100,'centerY':-200}}
    cases={}
    for f in range(1,33):
        variable=f in (3,5,7,9) or (13<=f<=31 and f%2==1)
        base=f-1 if variable else f
        p={'Format':f}
        if f==1:p['Layers']=[{'Format':10,'Glyph':'box','Paint':solid()},solid()]
        elif base==2:p.update(PaletteIndex=0xffff,Alpha=.75)
        elif base in (4,6,8):
            p['ColorLine']=line(variable)
            if base==4:p.update(x0=-10,y0=20,x1=900,y1=30,x2=50,y2=1000)
            elif base==6:p.update(x0=-10,y0=20,r0=2 if stress and variable else 40000,x1=900,y1=30,r1=3 if stress and variable else 300)
            else:p.update(centerX=250,centerY=500,startAngle=-90,endAngle=270)
        elif f==10:p.update(Glyph='box',Paint=solid())
        elif f==11:p.update(Glyph='paint_10')
        elif base==12:
            p.update(Paint=solid(),Transform={'xx':.75,'yx':.25,'xy':-.125,'yy':1.25,'dx':-15.5,'dy':20.25})
        elif f<=31:p.update(Paint=solid(),**transforms[base])
        else:p.update(SourcePaint=solid(),BackdropPaint={'Format':10,'Glyph':'box','Paint':solid()},CompositeMode=27)
        if variable:
            (p['Transform'] if base==12 else p)['VarIndexBase']=0
        cases[f'paint_{f}']=p
    if stress:
        cases['paint_33']={'Format':3,'PaletteIndex':0,'Alpha':.3,'VarIndexBase':100}
        cases['paint_34']={'Format':15,'Paint':solid(),'dx':-25,'dy':35,'VarIndexBase':0xffffffff}
    if outline_variation:
        cases['paint_35']={'Format':10,'Glyph':'compound','Paint':{'Format':3,'PaletteIndex':0,'Alpha':.75,'VarIndexBase':0}}
        cases['paint_36']={'Format':10,'Glyph':'nested','Paint':solid()}
    order=['.notdef','box']+(['compound','nested'] if outline_variation else [])+list(cases)
    empty=TTGlyphPen(None).glyph();pen=TTGlyphPen(None)
    if outline_variation:
        pen.moveTo((80,30));pen.lineTo((880,90));pen.qCurveTo((1080,520),(820,930))
        pen.lineTo((140,880));pen.qCurveTo((-60,460),(80,30));pen.closePath()
    else:
        pen.moveTo((0,0));pen.lineTo((1000,0));pen.lineTo((1000,1000));pen.lineTo((0,1000));pen.closePath()
    glyphs={name:pen.glyph() if name=='box' else empty for name in order}
    if outline_variation:
        pen=TTGlyphPen(glyphs);pen.addComponent('box',(.5,0,0,.75,40,70));pen.addComponent('box',(.4,0,0,.4,430,350));glyphs['compound']=pen.glyph()
        pen=TTGlyphPen(glyphs);pen.addComponent('compound',(.75,0,0,.75,60,-20));pen.addComponent('box',(.25,0,0,.25,720,40));glyphs['nested']=pen.glyph()
    builder=FontBuilder(1000,isTTF=True);builder.setupGlyphOrder(order)
    builder.setupCharacterMap({0xe000+i:name for i,name in enumerate(cases)})
    builder.setupGlyf(glyphs)
    builder.setupHorizontalMetrics({name:(1000,0) for name in order})
    builder.setupHorizontalHeader(ascent=1000,descent=0)
    builder.setupNameTable({'familyName':'MaiCord Color Table Fixture','styleName':'Regular',
        'uniqueFontIdentifier':'MaiCordColorTable1','fullName':'MaiCord Color Table Fixture','psName':'MaiCordColorTable'})
    builder.setupOS2(sTypoAscender=1000,sTypoDescender=0,usWinAscent=1000,usWinDescent=0)
    builder.setupPost();builder.setupMaxp();builder.setupFvar([('wght',100,400,900,'Weight')]+([('wdth',50,100,200,'Width')] if stress else []),[])
    if avar_remap:
        from fontTools.ttLib import newTable
        builder.font['avar']=newTable('avar')
        builder.font['avar'].segments={'wght':{-1:-1,-.75:-.5,0:0,.25:.5,.5:.75,1:1}}
        if stress:builder.font['avar'].segments['wdth']={-1:-1,-.5:-.25,0:0,.5:.875,1:1}
    builder.font['CPAL']=buildCPAL([[(1,.1,.2,1),(.2,.8,.3,.5),(.1,.2,1,.25)],
                                  [(.2,.8,.3,.5),(.1,.2,1,.25),(1,.1,.2,1)]],
        paletteTypes=[1,2],paletteLabels=['Light','Dark'],paletteEntryLabels=['Fill',None,'Outline'],nameTable=builder.font['name'])
    mapping=None
    if stress:
        from fontTools.ttLib.tables import otTables
        regions=buildVarRegionList([{'wght':(0,1,1)},{'wght':(-1,-1,0),'wdth':(0,1,1)},
                                   {'wght':(0,.5,1),'wdth':(-1,-.5,0)}],['wght','wdth'])
        normal=[[100,-20,35],[50,30,-25],[25,-300,100],[0,-70,10],[-25,80,-90],[-50,100,-120],[75,-10,30],[12,1,-1]]
        large=[[2000000000,-100000,20],[-2000000000,100000,-20],[300000,10,2],[-400000,20,3],
               [500000,30,4],[-600000,40,5],[700000,50,6],[800000,60,7]]
        store=buildVarStore(regions,[buildVarData([0,1,2],normal),buildVarData([0,1,2],large)])
        mapping=otTables.DeltaSetIndexMap();mapping.mapping=[0x10000,1,2,0x10003,4,5,0x10006,0 if map_last_active else 0xffffffff]
    else:
        regions=buildVarRegionList([{'wght':(0,1,1)}],['wght'])
        store=buildVarStore(regions,[buildVarData([0],[[100],[50],[25],[0],[-25],[-50],[75],[12]])])
    if empty_index_map:
        from fontTools.ttLib.tables import otTables
        mapping=otTables.DeltaSetIndexMap();mapping.mapping=[]
    clips={name:(0,0,1000,1000,0) if int(name.split('_')[1])%2 else (0,0,1000,1000) for name in cases}
    builder.font['COLR']=buildCOLR(cases,version=1,glyphMap=builder.font.getReverseGlyphMap(),varStore=store,varIndexMap=mapping,clipBoxes=clips)
    if outline_variation:
        from fontTools.ttLib.tables.TupleVariation import TupleVariation
        count=len(glyphs['box'].coordinates)
        positive=[((i%3-1)*35,(i%4-1)*25) for i in range(count)]+[(5,0),(65,0),(0,0),(0,0)]
        negative=[((1-i%3)*20,(2-i%4)*15) for i in range(count)]+[(-5,0),(-45,0),(0,0),(0,0)]
        variations={'box':[TupleVariation({'wght':(0,1,1)},positive),TupleVariation({'wght':(-1,-1,0)},negative)],
                    'compound':[TupleVariation({'wght':(0,1,1)},[(20,10),(-15,5)]+[(0,0)]*4)],
                    'nested':[TupleVariation({'wght':(0,1,1)},[(-15,0),(25,-10)]+[(0,0)]*4)]}
        if stress:
            variations['box'].append(TupleVariation({'wdth':(0,1,1)},[(20*(i%2),10*(1-i%3)) for i in range(count)]+[(0,0),(50,0),(0,0),(0,0)]))
            variations['compound'].append(TupleVariation({'wght':(-1,-1,0),'wdth':(0,1,1)},[(-20,15),(25,-15)]+[(0,0)]*4))
        builder.setupGvar(variations)
        # Variable glyf requires matching LSB/xMin; phantom deltas then exercise
        # origin shifts independently of the paint graph and clip variation.
        for name in order:
            glyphs[name].recalcBounds(builder.font['glyf'])
            builder.font['hmtx'].metrics[name]=(1000,getattr(glyphs[name],'xMin',0))
        builder.font['head'].flags|=2
    builder.font['head'].created=builder.font['head'].modified=2082844800
    builder.font.recalcTimestamp=False
    builder.save(path)
    print(path)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('output',type=Path)
    parser.add_argument('--variation-stress',action='store_true')
    parser.add_argument('--empty-index-map',action='store_true')
    parser.add_argument('--avar-remap',action='store_true')
    parser.add_argument('--map-last-active',action='store_true')
    parser.add_argument('--outline-variation',action='store_true',help='Add curved/simple/nested composite gvar outlines and phantom-point deltas')
    args=parser.parse_args()
    if args.map_last_active and not args.variation_stress:parser.error('--map-last-active requires --variation-stress')
    args.output.parent.mkdir(parents=True,exist_ok=True);build(args.output,args.variation_stress,args.empty_index_map,args.avar_remap,args.map_last_active,args.outline_variation)
