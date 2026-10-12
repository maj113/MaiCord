"""First-party identity font for Myanmar source-boundary and reorder probes."""
def make_font(path, *, script_tag='mym2', scalars=None, base=0x1000, label='Myanmar'):
    from fontTools.fontBuilder import FontBuilder
    from fontTools.ttLib import newTable
    from fontTools.ttLib.tables._g_l_y_f import Glyph
    from feature_plan_oracle import layout, script, language
    from gsub_execution_oracle import lookup, single, obj
    if scalars is None:scalars = list(range(0x1000,0x10a0))+list(range(0xaa60,0xaa80))+list(range(0xa9e0,0xaa00))+[0x20,0x2d,0x41,0xa0,0xd7,0x34f,0x200c,0x200d,0x2012,0x2022,0x25cc,0x25fb,0xfe00,0xfe01,0xe0100,0x1f600,0x2060]
    order = ['.notdef'] + [f'u{c:04x}' for c in scalars]
    builder = FontBuilder(1000, isTTF=True)
    builder.setupGlyphOrder(order)
    builder.setupCharacterMap(dict(zip(scalars, order[1:])))
    builder.setupGlyf({g:Glyph() for g in order})
    builder.setupHorizontalMetrics({g:(600,0) for g in order})
    builder.setupHorizontalHeader(ascent=800, descent=-200)
    builder.setupNameTable(dict(familyName=f'MaiCord {label} Input', styleName='Regular',
        uniqueFontIdentifier=f'MaiCord{label}Input', fullName=f'MaiCord {label} Input',
        psName=f'MaiCord{label}Input'))
    builder.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    builder.setupPost();builder.setupMaxp()
    builder.font['GSUB'] = newTable('GSUB')
    glyph=f'u{base:04x}'
    builder.font['GSUB'].table = layout('GSUB', [('ccmp',[0])], [lookup(1,single({glyph:glyph}))],
        {script_tag:script(language([0]))})
    from gpos_execution_oracle import adjust
    builder.font['GPOS']=newTable('GPOS')
    builder.font['GPOS'].table=layout('GPOS',[('dist',[0])],[adjust(glyph,0,0,0)],{script_tag:script(language([0]))})
    # Keep all shaping tables present; missing-table admission is a separate test.
    builder.font['GDEF']=newTable('GDEF')
    builder.font['GDEF'].table=obj('GDEF',Version=0x10000,GlyphClassDef=None,AttachList=None,LigCaretList=None,MarkAttachClassDef=None)
    builder.font['head'].created = builder.font['head'].modified = 2082844800
    builder.save(path)
    return [None, *scalars]
