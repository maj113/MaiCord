"""First-party GPOS fixtures and installed Windows placement observations (QA only)."""
import argparse,copy,hashlib,json,platform,struct,subprocess,sys
from pathlib import Path
import fontTools
from fontTools.ttLib import TTFont,newTable
from fontTools.ttLib.tables.otBase import ValueRecord
from embed_fixture import write_embedded_array
from gsub_execution_oracle import obj,coverage,lookup,make_font,ORDER,BASE,TEXT,CLASSES,identity,ligature,single,multiple
from gsub_execution_oracle import context as gsub_context
from gsub_execution_oracle import context_format as gsub_context_format

def anchor(x,y):return obj('Anchor',Format=1,XCoordinate=x,YCoordinate=y)
def value(x=0,y=0,a=0,b=0,mask=15):
    v=ValueRecord(mask)
    for bit,name,n in [(1,'XPlacement',x),(2,'YPlacement',y),(4,'XAdvance',a),(8,'YAdvance',b)]:
        if mask&bit:setattr(v,name,n)
    return v
def adjust(glyph,x=0,y=0,a=0,b=0,mask=15):return lookup(1,obj('SinglePos',Format=1,Coverage=coverage([glyph]),ValueFormat=mask,Value=value(x,y,a,b,mask)))
def pair(second_mask=0,flags=0):
    rows=[]
    for first in 'AB':
        records=[obj('PairValueRecord',SecondGlyph=g,Value1=value(13,-17,-71,19),Value2=value(-23,29,31,-37,second_mask) if second_mask else None) for g in 'AB']
        rows.append(obj('PairSet',PairValueCount=2,PairValueRecord=records))
    return lookup(2,obj('PairPos',Format=1,Coverage=coverage('AB'),ValueFormat1=15,ValueFormat2=second_mask,PairSetCount=2,PairSet=rows),flags=flags)
def cursive(flags=0,missing=None):
    points=[((10,20),(530,90)),((40,-30),(420,180)),((-20,60),(590,-50))]
    rows=[]
    for i,(entry,exit) in enumerate(points):rows.append(obj('EntryExitRecord',EntryAnchor=None if missing==(i,0) else anchor(*entry),ExitAnchor=None if missing==(i,1) else anchor(*exit)))
    return lookup(3,obj('CursivePos',Format=1,Coverage=coverage('ABC'),EntryExitCount=3,EntryExitRecord=rows),flags=flags)
def marks():return obj('MarkArray',MarkCount=2,MarkRecord=[obj('MarkRecord',Class=i,MarkAnchor=anchor(*p)) for i,p in enumerate([(20,30),(-40,50)])])
def mark_base(flags=0,bases='AB',null=False):
    rows=[obj('BaseRecord',BaseAnchor=[anchor(300+100*i,700-200*i),None if null else anchor(310+110*i,-100-50*i)]) for i,g in enumerate(bases)]
    return lookup(4,obj('MarkBasePos',Format=1,MarkCoverage=coverage('MN'),BaseCoverage=coverage(bases),ClassCount=2,MarkArray=marks(),BaseArray=obj('BaseArray',BaseCount=len(rows),BaseRecord=rows)),flags=flags)
def mark_mark(flags=0,targets='MN',null=False):
    rows=[obj('Mark2Record',Mark2Anchor=[anchor(80+10*i,160+10*i),None if null else anchor(90+10*i,-160-10*i)]) for i,g in enumerate(targets)]
    return lookup(6,obj('MarkMarkPos',Format=1,Mark1Coverage=coverage('MN'),Mark2Coverage=coverage(targets),ClassCount=2,Mark1Array=marks(),Mark2Array=obj('Mark2Array',Mark2Count=len(rows),Mark2Record=rows)),flags=flags)
def mark_lig():
    components=[obj('ComponentRecord',LigatureAnchor=[anchor(100+300*i,700-100*i),anchor(110+310*i,-100-100*i)]) for i in range(3)]
    return lookup(5,obj('MarkLigPos',Format=1,MarkCoverage=coverage('MN'),LigatureCoverage=coverage('L'),ClassCount=2,MarkArray=marks(),LigatureArray=obj('LigatureArray',LigatureCount=1,LigatureAttach=[obj('LigatureAttach',ComponentCount=3,ComponentRecord=components)])))
def component_probe():
    targets=sorted('LXYZ',key=ORDER.index);rows=[]
    for target in targets:
        components=[obj('ComponentRecord',LigatureAnchor=[anchor(100+300*i,700-100*i),anchor(110+310*i,-100-100*i)]) for i in range(8)]
        rows.append(obj('LigatureAttach',ComponentCount=8,ComponentRecord=components))
    return lookup(5,obj('MarkLigPos',Format=1,MarkCoverage=coverage('MN'),LigatureCoverage=coverage(targets),ClassCount=2,MarkArray=marks(),LigatureArray=obj('LigatureArray',LigatureCount=len(rows),LigatureAttach=rows)))
def context(actions,flags=0,chained=False):
    rec=[obj('PosLookupRecord',SequenceIndex=i,LookupListIndex=j) for i,j in actions]
    if chained:
        return lookup(8,obj('ChainContextPos',Format=3,BacktrackGlyphCount=1,BacktrackCoverage=[coverage('A')],InputGlyphCount=2,InputCoverage=[coverage('B'),coverage('M')],LookAheadGlyphCount=1,LookAheadCoverage=[coverage('C')],PosCount=len(rec),PosLookupRecord=rec),flags=flags)
    return lookup(7,obj('ContextPos',Format=3,GlyphCount=2,Coverage=[coverage('A'),coverage('M')],PosCount=len(rec),PosLookupRecord=rec),flags=flags)
def extension(l):return lookup(9,*[obj('ExtensionPos',Format=1,ExtensionLookupType=l.LookupType,ExtSubTable=copy.deepcopy(s)) for s in l.SubTable],flags=l.LookupFlag)
def position_context(inputs,actions,flags=0,chained=False,back=(),ahead=()):
    records=[obj('PosLookupRecord',SequenceIndex=i,LookupListIndex=j) for i,j in actions]
    if chained:return lookup(8,obj('ChainContextPos',Format=3,BacktrackGlyphCount=len(back),BacktrackCoverage=[coverage(g) for g in back],InputGlyphCount=len(inputs),InputCoverage=[coverage(g) for g in inputs],LookAheadGlyphCount=len(ahead),LookAheadCoverage=[coverage(g) for g in ahead],PosCount=len(records),PosLookupRecord=records),flags=flags)
    return lookup(7,obj('ContextPos',Format=3,GlyphCount=len(inputs),Coverage=[coverage(g) for g in inputs],PosCount=len(records),PosLookupRecord=records),flags=flags)

def position_context_format(inputs,actions,fmt,chained=False):
    def convert(v):
        if isinstance(v,list):return [convert(x) for x in v]
        if not hasattr(v,'__dict__'):return v
        rename=lambda name:name.replace('Subst','Pos').replace('Sub','Pos')
        return obj(rename(type(v).__name__),**{rename(k):convert(x) for k,x in vars(v).items()})
    return lookup(8 if chained else 7,convert(gsub_context_format(inputs,actions,fmt,chained).SubTable[0]))

def scenarios():
    cases=[]
    def add(name,text,lookups,purpose,**extra):cases.append(dict(name=name,input_glyphs=list(text),text=''.join(TEXT[g] for g in text),lookups=lookups,purpose=purpose,**extra))
    add('single-all','AB',[adjust('A',13,-17,-71,19)],'Four adjustment components in both directions')
    add('single-repeat','AB',[adjust('A',13,-17,-71,19),adjust('A',23,29,31,-37)],'Ordered scalar accumulation')
    add('single-mark','AM',[adjust('M',13,-17,71,19)],'Explicit mark advance versus default mark handling')
    for text in ('AMN','AMNB','AMNM','AMBN','MNA'):
        add('single-mark-advance-'+text,text,[adjust('M',a=53)],'Final mark advance removal and following offsets')
    add('single-classified-mark','AXB',[adjust('X',a=53)],'Nonzero hmtx advance for glyph classified as a mark',classes={'X':3})
    for mask in (0,1,4,15):
        add(f'pair-mask-{mask}','ABA',[pair(mask)],'Second value presence and overlapping pair scan')
        add(f'pair-skip-{mask}','AMBA',[pair(mask,8)],'Pair matching across an ignored mark')
    zero=pair(1);zero.SubTable[0].PairSet[0].PairValueRecord[1].Value2=value(mask=1)
    add('pair-present-zero','ABA',[zero],'Present zero second adjustment still affects scan continuation')
    for flag in (0,1,8,9):
        for text in ('AB','ABC','AMBC'):
            add(f'cursive-{flag}-{text}',text,[cursive(flag)],'Cursive logical direction, cross-stream chain and mark filtering')
    add('cursive-missing-entry','ABC',[cursive(missing=(1,0))],'Missing entry breaks a connection')
    add('cursive-missing-exit','ABC',[cursive(missing=(1,1))],'Missing exit breaks a connection')
    for order in (0,1):
        seq=[cursive(),adjust('B',31,47,53,59)];add(f'cursive-adjust-{order}','ABC',seq if order==0 else seq[::-1],'Adjustment order and attachment propagation')
    add('cursive-widths','ABC',[cursive()],'Distinct nominal advances in cursive conversion',metrics={'A':503,'B':711,'C':429})
    add('cursive-adjust-first','ABC',[adjust('A',31,47,53,59),cursive()],'Placement/advance changes on the chain root')
    add('cursive-adjust-last','ABC',[adjust('C',31,47,53,59),cursive(1)],'Initial position on the cross-stream chain root')
    add('cursive-adjust-pair-end','AB',[adjust('B',31,47,53,59),cursive()], 'Placement at the terminal glyph of a pair')
    add('cursive-adjust-broken-end','ABC',[adjust('B',31,47,53,59),cursive(missing=(1,1))], 'Terminal correction at a missing exit')
    add('cursive-adjust-all','ABC',[adjust('A',11),adjust('B',23),adjust('C',37),cursive()], 'Distinct placement offsets at all chain nodes')
    add('cursive-reverse-adjust-mid','ABC',[adjust('B',31,47,53,59),cursive(1)], 'Lookup RTL and placement on an interior node')
    add('cursive-reverse-adjust-all','ABC',[adjust('A',11),adjust('B',23),adjust('C',37),cursive(1)], 'Lookup RTL and distinct placement offsets at every node')
    add('cursive-ignored-mark-advance','AMBC',[adjust('M',a=53),cursive(8)],'Nonzero temporary advance on an ignored intervening mark')
    add('cursive-covered-mark-class','ABC',[cursive()], 'Covered connecting glyph classified as a mark',classes={'B':3})
    add('cursive-mark-class-ignored','ABC',[cursive(8)], 'Covered mark-class glyph ignored during connection',classes={'B':3})
    add('cursive-null-then-subtable','ABC',[lookup(3,cursive(missing=(1,0)).SubTable[0],cursive().SubTable[0])],'Null entry fallback within one lookup')
    for flag in (0,1):
        ab=cursive().SubTable[0];ab.Coverage=coverage('AB');ab.EntryExitRecord=ab.EntryExitRecord[:2];ab.EntryExitCount=2
        bc=cursive().SubTable[0];bc.Coverage=coverage('BC');bc.EntryExitRecord=bc.EntryExitRecord[1:];bc.EntryExitCount=2
        add(f'cursive-cross-subtables-{flag}','ABC',[lookup(3,ab,bc,flags=flag)],'A connected chain spanning separate subtables')
        for order in (0,1):
            seq=[cursive(flag|8),mark_base()];add(f'cursive-mark-order-{flag}-{order}','AMBC',seq if order==0 else seq[::-1],'Mark placement before/after a cursive chain')
    for text in ('AM','AN','AMN','AMM','ABM','AXM','AML','ALM','M','MAM'):
        add('base-'+text,text,[mark_base()],'Base search, intervening marks and uncovered targets')
    for flag in (2,4,8,0x100):add(f'base-filter-{flag}','AMNM',[mark_base(flag)],'Lookup flag effect on mark/base selection',mark_attach={'M':1,'N':2})
    add('base-unclassified-gap','AXM',[mark_base()], 'Unclassified preceding glyph',classes={'X':0})
    add('base-mark-class-gap','AXM',[mark_base()], 'Preceding glyph classified as mark',classes={'X':3})
    add('base-covered-ligature','ALM',[mark_base(bases='ABL')],'Covered preceding ligature in mark-to-base data')
    add('base-covered-unassigned','AXM',[mark_base(bases='ABX')],'Covered unclassified target',classes={'X':0})
    add('base-covered-component','AXM',[mark_base(bases='ABX')],'Covered component-class target',classes={'X':4})
    add('base-input-base-class','AMN',[mark_base()],'Mark coverage input classified as base',classes={'M':1})
    add('base-mark-set','AMNM',[mark_base(16)],'Input filtering through a mark glyph set',mark_sets=[['M']])
    add('base-null-anchor','AN',[mark_base(null=True)],'Missing class anchor')
    add('base-null-one-target','AN',[mark_base(bases='A',null=True)],'Null target with a different array header')
    all_null=mark_base(null=True)
    for row in all_null.SubTable[0].BaseArray.BaseRecord:row.BaseAnchor=[None,None]
    add('base-null-all-slots','AN',[all_null],'Null target with zero offset fields in its parent')
    explicit_zero=mark_base(null=True)
    for row in explicit_zero.SubTable[0].BaseArray.BaseRecord:row.BaseAnchor[1]=anchor(0,0)
    add('base-explicit-zero-anchor','AN',[explicit_zero],'Explicit origin anchor versus absent target anchor')
    add('base-null-then-subtable','AN',[lookup(4,mark_base(null=True).SubTable[0],mark_base().SubTable[0])],'Does a missing target continue to the next subtable?')
    for order in (0,1):
        seq=[mark_base(),adjust('A',31,47,53,59)];add(f'base-adjust-{order}','AMN',seq if order==0 else seq[::-1],'Base movement before/after attachment')
        seq=[mark_base(),adjust('M',31,47,53,59)];add(f'mark-adjust-{order}','AMN',seq if order==0 else seq[::-1],'Attached mark adjustment order')
    for text in ('AMN','AMMN','AMNM','ANM','MNM','AMBN'):
        add('mark-'+text,text,[mark_base(),mark_mark()],'Stacked marks and preceding covered mark')
    add('mark-order-reversed','AMNM',[mark_mark(),mark_base()],'Reattachment and dependency order')
    add('mark-null-anchor','AMN',[mark_base(),mark_mark(null=True)],'Absent mark-to-mark target anchor')
    add('mark-null-one-target','AMN',[mark_base(),mark_mark(targets='M',null=True)],'Missing mark target with a different array header')
    add('mark-target-base-class','AMN',[mark_base(),mark_mark()],'Covered preceding mark classified as base',classes={'M':1})
    add('mark-target-unassigned','AMN',[mark_base(),mark_mark()],'Covered preceding mark unclassified',classes={'M':0})
    add('mark-mark-set','AMNM',[mark_base(),mark_mark(16)],'Mark-to-mark previous-input filtering with a mark set',mark_sets=[['M']])
    add('mark-set-ignore-marks','AMNM',[mark_base(),mark_mark(24)],'IgnoreMarks takes precedence over mark filtering set',mark_sets=[['M']])
    add('mark-ignore-base-gap','AMBN',[mark_base(),mark_mark(2)],'Mark attachment across an ignored base and a cluster boundary')
    add('mark-ignore-ligature-gap','AMLN',[mark_base(),mark_mark(4)],'Mark attachment across an ignored ligature')
    for flag in (8,0x100,0x200):add(f'mark-filter-{flag}','AMNM',[mark_base(),mark_mark(flag)],'Mark filtering within mark attachment',mark_attach={'M':1,'N':2})
    for text in ('LM','LMN','LNM','LAM'):
        add('lig-direct-'+text,text,[mark_lig()],'Ligature glyph without GSUB component provenance')
    null_lig=mark_lig();null_lig.SubTable[0].LigatureArray.LigatureAttach[0].ComponentRecord[0].LigatureAnchor[0]=None
    add('lig-null-component','LM',[null_lig],'Missing ligature-component target anchor')
    for text in ('ABM','AMB','AMBN','ABMN','AMBM','AMBNM','ABCM','AMBCN','AMBNCM'):
        add('lig-gsub-'+text,text,[mark_lig()],'Ignored marks and GSUB component provenance',gsub=[ligature('ABC' if 'C' in text else 'AB',flags=8)])
    provenance=[
        ('nested-left','AMBNCM',[ligature('AB',flags=8),ligature('LC',output='X',flags=8)]),
        ('nested-right','AMBNCM DN'.replace(' ',''),[ligature('BC',flags=8),ligature('ALD',output='X',flags=8)]),
        ('nested-no-marks','ABCM',[ligature('AB',flags=8),ligature('LC',output='X',flags=8)]),
        ('merge-two-ligatures','AMBNCM DNEM'.replace(' ',''),[ligature('AB',flags=8),ligature('CD',output='X',flags=8),ligature('LX',output='Y',flags=8)]),
        ('single-preserve','AMBN',[ligature('AB',flags=8),lookup(1,single({'L':'X'}))]),
        ('alternate-preserve','AMBN',[ligature('AB',flags=8),lookup(3,obj('AlternateSubst',alternates={'L':['X','Y']}))]),
        ('reverse-preserve','AMBN',[ligature('AB',flags=8),lookup(8,obj('ReverseChainSingleSubst',Format=1,Coverage=coverage('L'),BacktrackGlyphCount=0,BacktrackCoverage=[],LookAheadGlyphCount=0,LookAheadCoverage=[],GlyphCount=1,Substitute=['X']))]),
        ('multiple-one','AMBN',[ligature('AB',flags=8),multiple({'L':['X']})]),
        ('multiple-expand','AMBN',[ligature('AB',flags=8),multiple({'L':['X','Y']})]),
        ('multiple-expand-three','AMBN',[ligature('AB',flags=8),multiple({'L':['X','Y','Z']})]),
        ('multiple-three-to-two','AMBNCM',[ligature('ABC',flags=8),multiple({'L':['X','Y']})]),
        ('multiple-ordinary','AMN',[multiple({'A':['X','Y']})]),
        ('delete-intervening-base','ABXM',[ligature('AB',flags=8),multiple({'X':[]})]),
        ('expand-religate','AMBN',[ligature('AB',flags=8),multiple({'L':['X','Y']}),ligature('XY',flags=8)]),
        ('replace-mark','AMBN',[ligature('AB',flags=8),lookup(1,single({'M':'N'}))]),
        ('expand-mark','AMBN',[ligature('AB',flags=8),multiple({'M':['M','N']})]),
        ('consume-mark','AMN',[ligature('AM',flags=0)]),
        ('mark-first-ligature','MAN',[ligature('MA',flags=0)]),
        ('all-mark-ligature','MNM',[ligature('MN',flags=0)]),
        ('same-component','ABMN',[ligature('AB',flags=8)]),
        ('different-components','AMBN',[ligature('AB',flags=8)]),
        ('direct-default','LMN',[]),
        ('independent-groups','AMBNAMBN',[ligature('AB',flags=8)]),
    ]
    for name,text,gsub in provenance:
        stages=gsub or [lookup(1,single({'A':'A'}))]
        invalid=name=='delete-intervening-base'
        extra=dict(invalid_input='OpenType GSUB MultipleSubst prohibits zero-length output sequences; this is only a malformed-input observation') if invalid else {}
        purpose='Observe Windows handling of prohibited zero-length MultipleSubst; no deletion semantics are inferred' if invalid else 'Observe ligature-component identity after '+name
        add('components-'+name,text,[component_probe()],purpose,gsub=stages,gsub_scheduled=list(range(len(stages))),**extra)
        if name in ('nested-left','merge-two-ligatures','multiple-expand','expand-religate','expand-mark','same-component','different-components','direct-default','independent-groups'):
            add('components-'+name+'-stacked',text,[component_probe(),mark_mark()],'Observe mark-to-mark restrictions after '+name,gsub=copy.deepcopy(stages),gsub_scheduled=list(range(len(stages))))
    for count in (1,2,4):
        probe=component_probe()
        for row in probe.SubTable[0].LigatureArray.LigatureAttach:row.ComponentRecord=row.ComponentRecord[:count];row.ComponentCount=count
        add('components-anchor-count-'+str(count),'AMBNCM',[probe],'Component history longer or shorter than target anchor array',gsub=[ligature('ABC',flags=8)])
    for flag in (2,4,8):
        probe=component_probe();probe.LookupFlag=flag
        add('components-filter-'+str(flag),'AMBN',[probe],'Lookup filtering of ligature targets and input marks',gsub=[ligature('AB',flags=8)])
    for stage in ('before','after'):
        seq=[adjust('L',31,47,53),adjust('M',a=17),component_probe()]
        if stage=='after':seq=seq[2:]+seq[:2]
        add('components-adjust-'+stage,'AMBN',[*seq],'Attachment/advance adjustment order',gsub=[ligature('AB',flags=8)])
    null=component_probe();null.SubTable[0].LigatureArray.LigatureAttach[-1].ComponentRecord[1].LigatureAnchor[1]=None
    add('components-null-anchor','AMBN',[null],'Missing anchor on the second component',gsub=[ligature('AB',flags=8)])
    for component,mark_class,text in ((0,0,'AMBN'),(0,1,'ANBM'),(1,0,'ANBM')):
        probe=component_probe();probe.SubTable[0].LigatureArray.LigatureAttach[-1].ComponentRecord[component].LigatureAnchor[mark_class]=None
        add(f'components-null-{component}-{mark_class}',text,[probe],'Null target anchor at a distinct component/class slot',gsub=[ligature('AB',flags=8)])
    contextual=[
        ('context-ligate','AMBN',[gsub_context('AB',[(0,1)],flags=8),ligature('AB',flags=8)],[0]),
        ('context-expand','AMBN',[ligature('AB',flags=8),gsub_context('L',[(0,2)]),multiple({'L':['X','Y']})],[0,1]),
        ('context-ligate-expand','AMBN',[gsub_context('AB',[(0,1),(0,2)],flags=8),ligature('AB',flags=8),multiple({'L':['X','Y']})],[0]),
        ('chain-ligate-expand','CAMBND',[gsub_context('AB',[(0,1),(0,2)],flags=8,chained=True,back='C',ahead='D'),ligature('AB',flags=8),multiple({'L':['X','Y']})],[0]),
        ('nested-context','AMBN',[gsub_context('AB',[(0,1)],flags=8),gsub_context('AB',[(0,2),(0,3)],flags=8),ligature('AB',flags=8),multiple({'L':['X','Y']})],[0]),
    ]
    for name,text,stages,scheduled in contextual:
        add('components-'+name,text,[component_probe(),mark_mark()],'Component history through contextual GSUB actions',gsub=stages,gsub_scheduled=scheduled)
    for length in (32,64):
        text=('ABC'*22)[:length]
        add(f'context-cursive-long-{length}',text,[position_context(text,[(i,1) for i in range(length-1)]),cursive(1)],'Long reverse cursive context for exact coalescing checks',scheduled=[0])
    actions=[(i,1) for i in range(63)];actions.insert(16,(0,2))
    add('context-cursive-long-interrupted','A'*64,[position_context('A'*64,actions),cursive(1),adjust('A',y=47)],'A position write interrupts and resumes a long reverse chain',scheduled=[0])
    for fmt in (1,2):
        for chained in (False,True):
            add(f'context-format-{fmt}-{int(chained)}','ABA',[position_context_format('AB',[(0,1),(1,2)],fmt,chained),pair(15),adjust('B',31,47,53)],'Glyph/class contextual format action ordering',scheduled=[0])
    no_action=position_context('AB',[]).SubTable[0]
    add('context-empty-blocks','ABA',[lookup(7,no_action,position_context('AB',[(0,1)]).SubTable[0]),adjust('A',31,47,53)],'A matched context with no actions blocks later subtables',scheduled=[0])
    add('context-first-miss','ABA',[lookup(7,position_context('AC',[(0,1)]).SubTable[0],position_context('AB',[(0,1)]).SubTable[0]),adjust('A',31,47,53)],'A nonmatching context permits later subtables',scheduled=[0])
    add('context-nested-child-ignore','AB',[position_context('AB',[(0,1)]),position_context('AB',[(1,2)],flags=2),adjust('B',31,47,53)],'Nested context bypasses initial filtering while filtering its later inputs',scheduled=[0])
    add('context-nested-child-ignore-first','AM',[position_context('AM',[(0,1)]),position_context('AM',[(1,2)],flags=2),adjust('M',31,47,53)],'Nested context bypasses child initial base filtering',scheduled=[0])
    add('context-chain-outside-lookaround','CABD',[position_context('AB',[(0,1)]),position_context('AB',[(1,2)],chained=True,back='C',ahead='D'),adjust('B',31,47,53)],'Nested lookaround extends beyond the parent input range',scheduled=[0])
    add('context-trailing-eligible-child','ABMA',[position_context('AB',[(1,1)],flags=8),position_context('BM',[(1,2)]),adjust('M',31,47,53)],'Trailing parent-ignored glyph is available to a child context',scheduled=[0])
    for reverse in (0,1):
        add(f'context-cursive-only-first-{reverse}','ABC',[position_context('ABC',[(0,1)]),cursive(reverse)],'Single cursive action in a longer parent',scheduled=[0])
        add(f'context-cursive-only-middle-{reverse}','ABC',[position_context('ABC',[(1,1)]),cursive(reverse)],'Single middle cursive action',scheduled=[0])
        add(f'context-cursive-mixed-adjust-{reverse}','ABC',[position_context('ABC',[(0,1),(1,2),(1,1)]),cursive(reverse),adjust('B',y=47)],'Interleaved adjustment and reverse chain propagation',scheduled=[0])
        add(f'context-cursive-two-lookups-{reverse}','ABC',[position_context('ABC',[(0,1),(1,2)]),cursive(reverse),cursive(reverse)],'Cursive links across distinct children',scheduled=[0])
        for actions in ([(0,1),(1,1),(2,1)],[(2,1),(1,1),(0,1)]):
            add(f'context-cursive-chain-{reverse}-{actions[0][0]}','ABC',[position_context('ABC',actions),cursive(reverse)],'Nested cursive chain action ordering',scheduled=[0])
        add(f'context-cursive-adjust-{reverse}','ABC',[position_context('ABC',[(1,1),(2,2)]),cursive(reverse),adjust('C',y=47)],'Later adjustment to a nested cursive target',scheduled=[0])
    def cursive_four(flags=0):
        l=cursive(flags);s=l.SubTable[0];s.Coverage=coverage('ABCD');s.EntryExitRecord.append(copy.deepcopy(s.EntryExitRecord[0]));s.EntryExitCount=4;return l
    for name,actions in [('branch',[(0,1),(1,2),(2,2)]),('reparent',[(0,2),(0,1),(2,2)]),('repeat',[(0,2),(0,2),(1,2),(2,2)])]:
        add('context-cursive-'+name,'ABCD',[position_context('ABCD',actions),cursive_four(9),cursive_four(1)],'Reverse cursive forest '+name,scheduled=[0],classes={'B':3})
    add('context-cursive-direction-mix','ABC',[position_context('ABC',[(0,1),(1,2)]),cursive(1),cursive()], 'Opposite attachment directions in one context',scheduled=[0])
    add('context-cursive-direction-mix-descendant','ABCD',[position_context('ABCD',[(0,1),(1,1),(2,2)]),cursive_four(1),cursive_four()], 'Forward reattachment of a reverse-chain parent',scheduled=[0])
    add('context-trailing-ignored','AMBA',[position_context('A',[(0,1)],flags=8),pair(15)],'Parent ignored trailing mark stays inside the child range',scheduled=[0],classes={'M':1})
    add('context-trailing-mark','AMBA',[position_context('A',[(0,1)],flags=8),adjust('M',31,47,53)],'Action starts only on its selected input',scheduled=[0])
    add('context-child-filter-target','AMBA',[position_context('AMBA',[(0,1)]),pair(15,8)],'Child filter still selects the second input',scheduled=[0])
    add('context-ligature-history','AMBN',[position_context('LMN',[(1,1),(2,1)]),component_probe()], 'Nested ligature positioning uses native GSUB history',scheduled=[0],gsub=[ligature('AB',flags=8)])
    add('context-stacked-marks','AMN',[position_context('AMN',[(1,1),(2,1),(2,2)]),mark_base(),mark_mark(8)],'Nested mark-to-mark bypasses its initial filter',scheduled=[0])
    add('context-attach','AM',[context([(1,1)]),mark_base()],'Nested mark input can search before context start',scheduled=[0])
    add('context-ordered','AM',[context([(1,1),(0,2)]),mark_base(),adjust('A',31,47,53,59)],'Nested ordered attachment/base adjustment',scheduled=[0])
    add('chain-attach','ABMC',[context([(1,1)],chained=True),mark_base()],'Chained attachment input and lookaround',scheduled=[0])
    for index in (0,1):
        add(f'context-cursive-{index}','ABC',[position_context('AB',[(index,1)]),cursive()],'Which cursive connection belongs to a contextual action',scheduled=[0])
        add(f'context-cursive-reverse-{index}','ABC',[position_context('AB',[(index,1)]),cursive(1)],'Lookup direction on a contextual cursive connection',scheduled=[0])
    add('context-cursive-preceding','CAB',[position_context('AB',[(0,1)]),cursive()],'Cursive attachment before the input context',scheduled=[0])
    add('context-pair-outside','AB',[position_context('A',[(0,1)]),pair(15)],'Pair second glyph outside the context input',scheduled=[0])
    add('context-pair-inside','ABC',[position_context('AB',[(0,1)]),pair(15)],'A contextual pair applied once',scheduled=[0])
    add('context-pair-last','ABC',[position_context('AB',[(1,1)]),pair(15)],'Pair starts at the last context input',scheduled=[0])
    ignored_single=adjust('A',31,47,53);ignored_single.LookupFlag=2
    add('context-child-ignore-base','AM',[position_context('AM',[(0,1)]),ignored_single],'Child lookup filtering of the selected first glyph',scheduled=[0])
    add('context-child-ignore-mark','AM',[position_context('AM',[(1,1)]),mark_base(8)],'Child lookup filtering of a selected mark',scheduled=[0])
    add('context-parent-ignore-first','AM',[position_context('AM',[(0,1)],flags=2),adjust('A',31,47,53)],'Filtering of a root contextual first glyph',scheduled=[0])
    add('context-overlap','AAAA',[position_context('AA',[(0,1)]),adjust('A',31,47,53)],'Advance past a matched input context',scheduled=[0])
    add('context-nested-longer','ABC',[position_context('A',[(0,1)]),position_context('ABC',[(2,2)]),adjust('C',31,47,53)],'Nested input context exceeds its parent',scheduled=[0])
    add('context-nested-ordered','AM',[position_context('AM',[(0,1),(0,3)]),position_context('AM',[(1,2)]),mark_base(),adjust('A',31,47,53)],'Nested ordered actions preserve attachment timing',scheduled=[0])
    for delta in (-3,3):
        device=obj('Device',StartSize=13,EndSize=18,DeltaFormat=2,DeltaValue=[delta]*6)
        v=value(a=-71,mask=4);v.XAdvDevice=device
        add(f'single-device-{delta}','AB',[lookup(1,obj('SinglePos',Format=1,Coverage=coverage('A'),ValueFormat=68,Value=v))],'Classic device correction in natural DIP placement')
        devmark=mark_base();devmark.SubTable[0].BaseArray.BaseRecord[0].BaseAnchor[0]=obj('Anchor',Format=3,XCoordinate=300,YCoordinate=700,XDeviceTable=device,YDeviceTable=device)
        add(f'anchor-device-{delta}','AM',[devmark],'Anchor Device correction in natural DIP placement')
    # Direct and wrapped controls must be measured separately; equivalence is not assumed.
    for source in list(cases):
        if source['name'] in ('single-all','pair-mask-0','cursive-0-ABC','base-AMN','mark-AMNM','lig-gsub-AMBN','context-ordered'):
            c=copy.deepcopy(source);c['name']+='-extension';c['lookups']=[extension(l) for l in c['lookups']];cases.append(c)
    return cases

def generate(folder):
    folder.mkdir(parents=True,exist_ok=True);records=[]
    for case in scenarios():
        path=folder/(case['name']+'.ttf');make_font(path,dict(name=case['name'],lookups=case.get('gsub',[lookup(1,single({'A':'A'}))])))
        with TTFont(path,recalcTimestamp=False) as f:
            # GSUB provenance is identical for disabled/enabled GPOS controls.
            f['GSUB'].table.FeatureList.FeatureRecord[0].FeatureTag='rlig'
            if 'gsub_scheduled' in case:
                feature=f['GSUB'].table.FeatureList.FeatureRecord[0].Feature;feature.LookupListIndex=case['gsub_scheduled'];feature.LookupCount=len(feature.LookupListIndex)
            gdef=f['GDEF'].table;gdef.GlyphClassDef.classDefs.update(case.get('classes',{}))
            for glyph,width in case.get('metrics',{}).items():f['hmtx'].metrics[glyph]=(width,0)
            if 'mark_attach' in case:gdef.MarkAttachClassDef=obj('ClassDef',classDefs=case['mark_attach'])
            if 'mark_sets' in case:
                gdef.Version=0x10002;gdef.MarkGlyphSetsDef=obj('MarkGlyphSetsDef',MarkSetTableFormat=1,MarkSetCount=len(case['mark_sets']),Coverage=[coverage(names) for names in case['mark_sets']])
                for l in case['lookups']:
                    if l.LookupFlag&16:l.MarkFilteringSet=0
            script=copy.deepcopy(f['GSUB'].table.ScriptList);scheduled=case.get('scheduled',list(range(len(case['lookups']))))
            f['GPOS']=newTable('GPOS');f['GPOS'].table=obj('GPOS',Version=0x10000,ScriptList=script,
                FeatureList=obj('FeatureList',FeatureCount=1,FeatureRecord=[obj('FeatureRecord',FeatureTag='test',Feature=obj('Feature',FeatureParams=None,LookupCount=len(scheduled),LookupListIndex=scheduled))]),
                LookupList=obj('LookupList',LookupCount=len(case['lookups']),Lookup=case['lookups']))
            f.save(path)
        with TTFont(path) as f:
            assert f['GPOS'].table.FeatureList.FeatureRecord[0].Feature.LookupListIndex==scheduled
            f.saveXML(path.with_suffix('.ttx'),tables=['GlyphOrder','cmap','hmtx','GDEF','GSUB','GPOS'])
        records.append({k:v for k,v in case.items() if k not in ('lookups','gsub')}|dict(font=identity(path),decoded=identity(path.with_suffix('.ttx'))))
    (folder/'fixtures.json').write_text(json.dumps(dict(scope='First-party placement fixtures; Windows outputs are measured',fonttools=fontTools.__version__,glyph_order=ORDER,cases=records),indent=2)+'\n')
    print(f'Generated {len(records)} placement fixtures')

def bits(v):return struct.unpack('<I',struct.pack('<f',v))[0]
def floats(v):return [[struct.unpack('<f',struct.pack('<I',x))[0] for x in row] for row in v]
CONTROLS={
    'single-all':[[529,13,-17],[600,0,0]],
    'single-repeat':[[560,36,12],[600,0,0]],
    'pair-mask-0':[[529,13,-17],[529,13,-17],[600,0,0]],
    'pair-present-zero':[[529,13,-17],[600,0,0],[600,0,0]],
    'cursive-0-ABC':[[490,0,0],[440,0,120],[600,0,240]],
    'cursive-1-ABC':[[490,0,-240],[440,0,-120],[600,0,0]],
    'base-AM':[[600,0,0],[0,-320,670]],
    'mark-AMN':[[600,0,0],[0,-320,670],[0,-190,460]],
    'lig-gsub-AMBN':[[600,0,0],[0,-520,670],[0,-140,-250]],
    'context-attach':[[600,0,0],[0,-320,670]],
}
def record(folder,exe,out):
    manifest=json.loads((folder/'fixtures.json').read_text());observations=[];module=None
    for case in manifest['cases']:
        path=folder/(case['name']+'.ttf');assert identity(path)['sha256']==case['font']['sha256']
        for em in (1000.0,16.0,13.25,17.3):
            for rtl in (0,1):
                outputs=[]
                for enabled in (0,1):
                    command=[str(exe.resolve()),str(path.resolve()),','.join(f'{ord(c):x}' for c in case['text']),str(enabled),str(bits(em)),str(rtl),'0']
                    run=subprocess.run(command,capture_output=True,text=True,timeout=15)
                    if run.returncode:raise RuntimeError(f'{case["name"]}, {em}, {rtl}, {enabled}: {run.returncode}: {run.stdout} {run.stderr}')
                    v=json.loads(run.stdout);assert v['hresult']==v['placement_hresult']==0 and v['em_bits']==bits(em) and v['rtl']==rtl and v['sideways']==0
                    assert v['feature_enabled']==enabled and len(v['glyphs'])==len(v['placement_bits'])==len(v['glyph_properties'])
                    assert len(v['clusters'])==len(case['text'].encode('utf-16-le'))//2 and all(i<len(v['glyphs']) for i in v['clusters'])
                    v['glyph_names']=[ORDER[g] for g in v['glyphs']];v['placement_values']=floats(v['placement_bits']);outputs.append(v)
                    loaded=Path(v['dwrite_module']).resolve()
                    if module is None:module=loaded
                    assert loaded==module
                assert outputs[0]['glyphs']==outputs[1]['glyphs'] and outputs[0]['clusters']==outputs[1]['clusters']
                if em==1000 and not rtl and case['name'] in CONTROLS:assert outputs[1]['placement_values']==CONTROLS[case['name']],(case['name'],outputs[1])
                observations.append(dict(name=case['name'],em_bits=bits(em),rtl=rtl,disabled=outputs[0],enabled=outputs[1]))
    out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(dict(scope='Installed GetGlyphs/GetGlyphPlacements; one Latin script run, horizontal, both direction flags, four DIP sizes, custom GPOS test feature and always-on GSUB rlig',os=platform.platform(),windows_version=list(sys.getwindowsversion()),executable=identity(exe),fixtures=identity(folder/'fixtures.json'),dwrite=identity(module),placement_fields=['advance','advanceOffset','ascenderOffset'],observations=observations),indent=2)+'\n')
    print(f'Recorded {len(observations)} enabled/disabled placement pairs')
def attachment_seed(case,names):
    # Explicit fixture inputs to positioning. This is not a GSUB implementation.
    none=(0xffffffff,0);name=case['name'].removesuffix('-extension').removesuffix('-stacked')
    if name.startswith('lig-direct-') or name=='lig-null-component':
        target=0xffffffff;result=[]
        for i,g in enumerate(names):
            result.append((target,0xffffffff) if g in 'MN' else none)
            if g not in 'MN':target=i
        return result
    if name.startswith('lig-gsub-'):
        result=[none];component=-1
        for g in case['input_glyphs']:
            if g in 'ABC':component+=1
            else:result.append((0,component))
        return result
    name=name.removeprefix('components-');single=[none,(0,0),(0,1)]
    fixed={
      'nested-left':[none,(0,0),(0,1),(0,2)],'nested-right':[none,(0,0),(0,1),(0,2),(0,3)],'nested-no-marks':[none,(0,2)],
      'merge-two-ligatures':[none,(0,0),(0,1),(0,2),(0,3),none,(5,0)],
      'single-preserve':single,'alternate-preserve':single,'reverse-preserve':single,'multiple-one':single,
      'multiple-expand':[none,none,(0,0),(1,0)],'multiple-expand-three':[none,none,none,(0,0),(1,0)],
      'multiple-three-to-two':[none,none,(0,0),(1,0),(1,1)],'multiple-ordinary':[none,none,(0,0),(0,0)],
      'expand-religate':single,'replace-mark':single,'expand-mark':[none,(0,0),(0,0),(0,1)],
      'consume-mark':[none,(0,1)],'mark-first-ligature':[none,(0,1)],'all-mark-ligature':[none,(0,1)],
      'same-component':[none,(0,1),(0,1)],'different-components':single,'direct-default':[none,(0,0xffffffff),(0,0xffffffff)],
      'independent-groups':[none,(0,0),(0,1),none,(3,0),(3,1)],'null-anchor':single,
      'context-ligate':single,'context-expand':[none,none,(0,0),(1,0)],'context-ligate-expand':[none,none,(0,0),(1,0)],
      'chain-ligate-expand':[none,none,none,(1,0),(2,0),none],'nested-context':[none,none,(0,0),(1,0)],
    }
    if name.startswith('anchor-count-'):return [none,(0,0),(0,1),(0,2)]
    if name.startswith(('filter-','adjust-','null-')):return single
    assert name in fixed,name
    return fixed[name]

def export_basic(folder,observed,out,header,marks=False,cursive=False,ligatures=False):
    manifest=json.loads((folder/'fixtures.json').read_text());windows=json.loads(observed.read_text());assert windows['fixtures']['sha256']==identity(folder/'fixtures.json')['sha256']
    observations={}
    for o in windows['observations']:observations.setdefault(o['name'],[]).append(o)
    blobs=[];selected=[]
    word=lambda *v:struct.pack('<'+'I'*len(v),*v)
    for case in manifest['cases']:
        if ligatures and case.get('invalid_input'):continue
        path=folder/(case['name']+'.ttf');assert identity(path)['sha256']==case['font']['sha256']
        with TTFont(path) as f:
            table=f['GPOS'].table;lookups=table.LookupList.Lookup
            if any((s.ExtensionLookupType if l.LookupType==9 else l.LookupType) not in ((1,2,3,4,5,6) if ligatures else (1,2,3,4,6) if cursive else (1,2,4,6) if marks else (1,2)) for l in lookups for s in l.SubTable):continue
            configs=observations[case['name']];seed=configs[0]['disabled'];glyphs=seed['glyphs'];flags=[int(p[2] or p[3])|int(p[1])*2 for p in seed['glyph_properties']]
            classes=f['GDEF'].table.GlyphClassDef.classDefs;scheduled=table.FeatureList.FeatureRecord[0].Feature.LookupListIndex
            has_associations=ligatures and any((sub.ExtensionLookupType if l.LookupType==9 else l.LookupType)==5 for l in lookups for sub in l.SubTable)
            target_glyphs=[];associations=[]
            if has_associations:
                associations=attachment_seed(case,[ORDER[g] for g in glyphs]);assert len(associations)==len(glyphs),(case['name'],associations,glyphs)
                target_glyphs=sorted({v[0] for v in associations if v[0]!=0xffffffff});assert target_glyphs
                associations=[(target_glyphs.index(target) if target!=0xffffffff else target,component) for target,component in associations]
            data=word(path.stat().st_size,len(glyphs),len(scheduled),len(configs))+(word(len(target_glyphs)) if ligatures else b'')+path.read_bytes()
            data+=b''.join(struct.pack('<HBB',g,classes.get(ORDER[g],0),flag) for g,flag in zip(glyphs,flags))
            if has_associations:data+=word(*target_glyphs)+b''.join(word(*v) for v in associations)
            data+=struct.pack('<'+'H'*len(scheduled),*scheduled)
            for o in configs:
                for state in ('disabled','enabled'):
                    assert o[state]['glyphs']==glyphs and [int(p[2] or p[3])|int(p[1])*2 for p in o[state]['glyph_properties']]==flags
                data+=word(o['em_bits'],o['rtl'])
                for state in ('disabled','enabled'):data+=b''.join(word(*v) for v in o[state]['placement_bits'])
            blobs.append(data);selected.append(dict(name=case['name'],glyphs=len(glyphs),configurations=len(configs),font_sha256=case['font']['sha256']))
    raw=(b'MCGLP01\0' if ligatures else b'MCGCU01\0' if cursive else b'MCGMK01\0' if marks else b'MCGPX01\0')+word(len(blobs))+b''.join(blobs);out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(raw)
    scope='Native basic/mark/cursive positioning with recorded glyphs/shaping flags; no independent script policy, ligature attachment or nested execution' if cursive else 'Native single/pair and mark positioning with recorded glyphs/shaping flags; no independent script policy, cursive, ligature attachment or nested execution' if marks else 'Native basic positioning using recorded glyphs and shaping flags; no independent script policy or attachment execution'
    if ligatures:scope='Native root positioning with recorded glyphs/shaping flags and explicit fixture target/component inputs; no native GSUB provenance or independent script policy'
    out.with_suffix('.json').write_text(json.dumps(dict(scope=scope,observations=identity(observed),cases=selected,corpus_sha256=hashlib.sha256(raw).hexdigest()),indent=2)+'\n')
    if header:write_embedded_array(header,('// First-party Windows placement fixture with explicit target/component seeds; generated by gpos_execution_oracle.py.\n' if ligatures else '// First-party Windows cursive placement fixture; generated by gpos_execution_oracle.py.\n' if cursive else '// First-party Windows mark placement fixture; generated by gpos_execution_oracle.py.\n' if marks else '// First-party Windows basic placement fixture; generated by gpos_execution_oracle.py.\n'),'constexpr u8 position_fixture[]',raw)
    print(f'Exported {len(blobs)} positioning fonts, {sum(c["configurations"] for c in selected)} configurations, {len(raw)} bytes')
def main():
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='mode',required=True);g=sub.add_parser('generate');g.add_argument('folder',type=Path);r=sub.add_parser('record');r.add_argument('folder',type=Path);r.add_argument('--exe',type=Path,required=True);r.add_argument('--out',type=Path,required=True)
    for mode in ('export-basic','export-marks','export-cursive','export-ligatures'):
        e=sub.add_parser(mode);e.add_argument('folder',type=Path);e.add_argument('--observations',type=Path,required=True);e.add_argument('--out',type=Path,required=True);e.add_argument('--header',type=Path)
    a=p.parse_args()
    if a.mode=='generate':generate(a.folder)
    elif a.mode=='record':record(a.folder,a.exe,a.out)
    else:export_basic(a.folder,a.observations,a.out,a.header,a.mode=='export-marks',a.mode in ('export-cursive','export-ligatures'),a.mode=='export-ligatures')
if __name__=='__main__':main()
