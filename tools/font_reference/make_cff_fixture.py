"""First-party CFF1 test fonts; fontTools is only a development font writer."""
import argparse
import math
import random
from pathlib import Path
from fontTools.fontBuilder import FontBuilder
from fontTools.misc.psCharStrings import T2CharString
from fontTools.cffLib import SubrsIndex


def make_font(output, units, matrix, expanded=False):
    programs = {
        ".notdef": ["endchar"],
        "A": [10,20,"rmoveto",100,0,0,100,-100,0,"rlineto","endchar"],
        "acute": [0,0,"rmoveto",20,30,10,-10,"rlineto","endchar"],
        "fractional": [10.25,20.75,"rmoveto",20.5,30.125,-10.25,40.375,50.5,-20.25,"rrcurveto","endchar"],
        "alternating": [10,20,"rmoveto",30,40,50,60,"hlineto",30,40,50,60,"vlineto","endchar"],
        "curves": [10,20,"rmoveto",1,2,3,4,5,6,7,8,"hvcurveto",1,2,3,4,5,"vhcurveto",1,2,3,4,5,"hhcurveto",1,2,3,4,5,"vvcurveto","endchar"],
        "mixed": [10,20,"rmoveto",1,2,3,4,5,6,7,8,"rcurveline",1,2,3,4,5,6,7,8,"rlinecurve","endchar"],
        "flex": [0,0,"rmoveto",1,2,3,4,5,6,7,"hflex",1,2,3,4,5,6,7,8,9,10,11,12,50,"flex",1,2,3,4,5,6,7,8,9,"hflex1",1,2,3,4,5,6,7,8,9,10,11,"flex1","endchar"],
        "hints": [50,10,"hstem",100,10,"vstem","hintmask",b"\xc0",0,0,"rmoveto",100,0,0,100,"rlineto","endchar"],
        "width": [400,0,0,"rmoveto",100,0,0,100,"rlineto","endchar"],
        "subrs": [10,20,"rmoveto",-107,"callsubr",-107,"callgsubr","endchar"],
        "accented": [40,100,65,194,"endchar"],
        "moveonly": [20,30,"rmoveto","endchar"],
        "arithmetic": [10,20,"add",5,"sub",2,"mul",2,"div",10,"rmoveto",100,"sqrt",-20,"abs","rlineto","endchar"],
        "stack": [10,"dup","exch",2,1,"roll",0,"index","drop","rmoveto",25,0,"put",0,"get",30,"rlineto","endchar"],
        "condition": [0,0,"rmoveto",10,20,1,2,"ifelse",0,"rlineto",1,0,"and","not",0,"or",1,"eq",10,"mul",5,"rlineto","endchar"],
    }
    if expanded:
        programs.update({
            "hflex_isolated":[0,0,"rmoveto",1,2,3,4,5,6,7,"hflex","endchar"],
            "hflex_equivalent":[0,0,"rmoveto",1,0,2,3,4,0,5,0,6,-3,7,0,"rrcurveto","endchar"],
            "flex_isolated":[0,0,"rmoveto",1,2,3,4,5,6,7,8,9,10,11,12,50,"flex","endchar"],
            "flex_equivalent":[0,0,"rmoveto",1,2,3,4,5,6,7,8,9,10,11,12,"rrcurveto","endchar"],
            "hflex1_isolated":[0,0,"rmoveto",1,2,3,4,5,6,7,8,9,"hflex1","endchar"],
            "hflex1_equivalent":[0,0,"rmoveto",1,2,3,4,5,0,6,0,7,8,9,-14,"rrcurveto","endchar"],
            "flex1_isolated":[0,0,"rmoveto",1,2,3,4,5,6,7,8,9,10,11,"flex1","endchar"],
            "flex1_equivalent":[0,0,"rmoveto",1,2,3,4,5,6,7,8,9,10,-25,11,"rrcurveto","endchar"],
        })
    extra_left = {}
    if expanded:
        rng=random.Random(0xCFF17)
        for kind in range(4):
            for sample in range(8):
                s=[rng.randrange(-200,201)/4 for _ in range((7,13,9,11)[kind])]
                if kind==0:
                    curves=[s[0],0,s[1],s[2],s[3],0,s[4],0,s[5],-s[2],s[6],0]
                elif kind==1:
                    s[12]=50
                    curves=s[:12]
                elif kind==2:
                    curves=[s[0],s[1],s[2],s[3],s[4],0,s[5],0,s[6],s[7],s[8],-(s[1]+s[3]+s[7])]
                else:
                    dx=sum(s[0:10:2]);dy=sum(s[1:10:2])
                    curves=s[:10]+([s[10],-dy] if abs(dx)>abs(dy) else [-dx,s[10]])
                x,y=10.125,-20.375
                xs=[x]
                for at in range(0,12,2):
                    x+=curves[at];y+=curves[at+1];xs.append(x)
                # A following ordinary line detects accidental mutation of the
                # charstring current point by the flex-only output round trip.
                xs.append(x+15.5)
                for variant in ("flex","cubic"):
                    name=f"stress_{kind}_{sample}_{variant}"
                    operator=("hflex","flex","hflex1","flex1")[kind] if variant=="flex" else "rrcurveto"
                    programs[name]=[10.125,-20.375,"rmoveto"]+(s if variant=="flex" else curves)+[operator,15.5,-7.25,"rlineto","endchar"]
                    extra_left[name]=min(xs)
    builder = FontBuilder(units, isTTF=False)
    order = list(programs)
    builder.setupGlyphOrder(order)
    builder.setupCharacterMap({0xE000+i:name for i,name in enumerate(order[1:])})
    # CFF widths are in charstring coordinates; hmtx is in head design units.
    # Keep them consistent even in deliberate noncanonical matrix probes.
    scale=round(matrix[0]*units*65536)/65536
    left={"A":10,"fractional":10.25,"alternating":10,"curves":10,
          "mixed":10,"subrs":10,"accented":10,"arithmetic":25,"stack":10}
    left.update(extra_left)
    builder.setupHorizontalMetrics({name:(round(500*scale),math.floor(left.get(name,0)*scale)) for name in order})
    builder.setupHorizontalHeader(ascent=800,descent=-200)
    builder.setupNameTable({"familyName":"MaiCord CFF Fixture","styleName":"Regular","uniqueFontIdentifier":output.stem,"fullName":output.stem,"psName":output.stem})
    builder.setupOS2(sTypoAscender=800,sTypoDescender=-200,usWinAscent=800,usWinDescent=200)
    builder.setupPost()
    builder.setupCFF(output.stem,{"FullName":output.stem,"FamilyName":"MaiCord CFF Fixture","Weight":"Regular","FontMatrix":matrix},
                     {name:T2CharString(program=program) for name,program in programs.items()}, {"defaultWidthX":500,"nominalWidthX":100})
    cff=builder.font["CFF "].cff
    top=cff.topDictIndex[0]
    top.Private.Subrs=SubrsIndex()
    top.Private.Subrs.append(T2CharString(program=[20,0,"rlineto","return"],private=top.Private,globalSubrs=cff.GlobalSubrs))
    cff.GlobalSubrs.append(T2CharString(program=[0,30,"rlineto","return"],private=top.Private,globalSubrs=cff.GlobalSubrs))
    builder.font["head"].created=builder.font["head"].modified=2082844800
    builder.font.recalcTimestamp=False
    # fontTools' bounding-box writer intentionally does not execute all Type2
    # arithmetic. Explicit test bounds avoid needing its interpreter as oracle.
    top.FontBBox=[-200,-200,1000,1000]
    builder.font.recalcBBoxes=False
    builder.save(output)
    print(output)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output",type=Path)
    parser.add_argument("--expanded",action="store_true",help="Add isolated flex/equivalent-cubic pairs")
    args=parser.parse_args(); args.output.mkdir(parents=True,exist_ok=True)
    for name,units,matrix in [
        ("canonical",1000,[.001,0,0,.001,0,0]),
        ("rounded",1024,[.00097656,0,0,.00097656,0,0]),
        ("scale",1000,[.002,0,0,.002,0,0]),
        ("inverse",1000,[.0011,0,0,.0011,0,0]),
        ("inverse2",1000,[.0012345,0,0,.0012345,0,0]),
        ("affine",1000,[.001,.0002,.0003,.002,.01,-.02]),
    ]:
        make_font(args.output/f"cff-{name}.otf",units,matrix,args.expanded)
    if args.expanded:
        for name,units,value in [("small",1000,.0007),("third",3000,.00033333),("scale107",1000,.00107)]:
            make_font(args.output/f"cff-{name}.otf",units,[value,0,0,value,0,0],True)


if __name__=="__main__":
    main()
