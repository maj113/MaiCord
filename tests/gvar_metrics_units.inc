#include "generated/gvar_metrics_fixture.inc"
bool gvar_metrics_units() noexcept {
    Reader reader{{gvar_metrics_fixture,sizeof(gvar_metrics_fixture)},8};u32 count=0;
    if(!bytes_equal(reader.bytes.data,"MCGMET1\0",8)||!reader.word(&count))return false;
    u32 checked=0,shapes=0;
    for(u32 f=0;f<count;++f){u32 size=0,cases=0;ConstSlice<u8> bytes{};SfntFace face{};SfntTrueType font{};FontAxes axes{};FontAxisMappings mappings{};AxisSegmentMap maps[16];SfntGvar gvar{};
        if(!reader.word(&size)||!reader.word(&cases)||!reader.take(size,&bytes)||sfnt_open(bytes,0,&face)!=SfntStatus::ok||
           sfnt_truetype_open(face,&font,true)!=SfntStatus::ok||font_axes_open(face,&axes)!=SfntStatus::ok||axes.axis_count>16||
           font_axis_mappings_open(face,axes.axis_count,{maps,16},&mappings)!=SfntStatus::ok||
           sfnt_gvar_open(font.gvar,axes.axis_count,font.metrics.glyph_count,&gvar)!=SfntStatus::ok)return fail(f,0,30);
        for(u32 c=0;c<cases;++c){u32 weight=0,glyphs=0;if(!reader.word(&weight)||!reader.word(&glyphs)||glyphs!=font.metrics.glyph_count)return false;
            const FontAxisSetting setting{sfnt_tag('w','g','h','t'),static_cast<double>(weight)};double normalized[16];SfntGvarInstance instance{};
            if(font_axis_coordinates(axes,mappings,{&setting,1},{normalized,16})!=SfntStatus::ok||
               sfnt_gvar_instance_prepare(gvar,{normalized,axes.axis_count},{shared_scalars,4096},&instance)!=SfntStatus::ok)return fail(f,0,31);
            for(u32 glyph=0;glyph<glyphs;++glyph){u32 expected=0,used=0;if(!reader.word(&expected))return false;u16 actual=0;
                if(sfnt_gvar_design_advance(font,instance,static_cast<u16>(glyph),&actual,1000000,&used)!=SfntStatus::ok||actual!=expected)return fail(f,glyph,32);
                const u32 required=used;
                for(u32 work=0;work<=required;++work){actual=0xffff;const auto status=sfnt_gvar_design_advance(font,instance,static_cast<u16>(glyph),&actual,work,&used);
                    if(status!=(work<required?SfntStatus::resource_limit:SfntStatus::ok)||used>work||actual!=(work<required?0:expected))return fail(f,glyph,33);}
                ++checked;
            }
            u32 outline_count=0;if(!reader.word(&outline_count))return false;
            for(u32 i=0;i<outline_count;++i){u32 glyph=0,n=0,fill=0,bounded=0,bounds[4];ConstSlice<u8> expected{};
                if(!reader.word(&glyph)||!reader.word(&n)||!reader.word(&fill)||!reader.word(&bounded)||n>1024)return false;
                for(u32& b:bounds)if(!reader.word(&b))return false;if(!reader.take(n*28,&expected))return false;
                const SfntOutlineVariation variation{&instance,{{shared_points,512},{private_points,512},{dx,512},{dy,512}},{deltas,1024}};
                SfntOutlineScratch scratch{{points,512},{ends,128},{active,64},0,0,1000000};GlyphOutline outline{};outline.commands=commands;outline.capacity=1024;
                if(sfnt_truetype_outline(font,static_cast<u16>(glyph),&scratch,&outline,SfntOutlineMode::text,&variation)!=SfntStatus::ok||
                   outline.count!=n||static_cast<u32>(outline.fill_rule)!=fill||outline.has_bounds!=(bounded!=0)||!bytes_equal(commands,expected.data,n*28))return fail(f,glyph,35);
                const float actual[]{outline.minimum_x,outline.minimum_y,outline.maximum_x,outline.maximum_y};if(!bytes_equal(actual,bounds,16))return fail(f,glyph,36);++shapes;
            }
            u16 value=1;if(sfnt_gvar_design_advance(font,instance,font.metrics.glyph_count,&value)!=SfntStatus::glyph_out_of_range||value||
                sfnt_gvar_design_advance(font,instance,0,nullptr)!=SfntStatus::invalid_input)return fail(f,0,34);
        }
    }
    print(utf8_literal("phantom_advances="));number(checked);print(utf8_literal("\nphantom_outlines="));number(shapes);print(utf8_literal("\n"));return reader.at==reader.bytes.count;
}
bool gvar_horizontal_prefix_cases() noexcept {
    const u8 full[]{1,10,30,0x81,0x83},unused[]{1,10,30,0xff,0xff},short_x[]{1,10},oversized[]{0x85},zero[]{0x83};
    const u8 vertical[]{1,0,2},bad_point[]{1,0,4},right[]{1,0,1,0,30},duplicate[]{2,1,1,0,1,10,20};
    struct Case {ConstSlice<u8> data;bool sparse;SfntStatus status;u16 advance;};
    const Case cases[]{
        {{full,5},false,SfntStatus::ok,620},{{unused,5},false,SfntStatus::ok,620},
        {{full,4},false,SfntStatus::ok,620},{{full,3},false,SfntStatus::ok,620},
        {{short_x,2},false,SfntStatus::truncated,0},{{oversized,1},false,SfntStatus::malformed,0},
        {{zero,1},false,SfntStatus::ok,600},{{vertical,3},true,SfntStatus::ok,600},
        {{bad_point,3},true,SfntStatus::malformed,0},{{right,3},true,SfntStatus::truncated,0},
        {{right,5},true,SfntStatus::ok,630},{{duplicate,7},true,SfntStatus::ok,630}};
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!memory)return false;
    struct Release {void* p;~Release() noexcept {VirtualFree(p,0,MEM_RELEASE);}} release{memory};
    DWORD old=0;if(!VirtualProtect(memory+page,page,PAGE_NOACCESS,&old))return false;
    const u8 loca[4]{},metrics[]{2,88,0,0};SfntTrueType font{};
    font.loca={loca,4};font.loca_stride=2;font.metrics.table={metrics,4};font.metrics.glyph_count=font.metrics.long_metrics=1;
    for(u32 i=0;i<sizeof(cases)/sizeof(*cases);++i){const auto& test=cases[i];const u32 size=38+test.data.count;
        auto* data=memory+page-size;clear_bytes(data,size);data[1]=data[5]=data[13]=data[15]=1;data[19]=28;
        data[27]=static_cast<u8>(10+test.data.count);data[29]=1;data[31]=10;data[33]=static_cast<u8>(test.data.count);
        data[34]=test.sparse?0xa0:0x80;data[36]=0x40;copy_bytes(data+38,test.data.data,test.data.count);
        SfntGvar gvar{};SfntGvarInstance instance{};const double coordinate=1;
        if(sfnt_gvar_open({data,size},1,1,&gvar)!=SfntStatus::ok||sfnt_gvar_instance_prepare(gvar,{&coordinate,1},{},&instance)!=SfntStatus::ok)return fail(i,0,37);
        u16 result=0xffff;u32 used=0;const auto status=sfnt_gvar_design_advance(font,instance,0,&result,4096,&used);
        if(status!=test.status||result!=test.advance||used>4096)return fail(i,0,38);
    }
    print(utf8_literal("horizontal_prefix_cases=12\n"));return true;
}
bool gvar_metrics_guards() noexcept {
    if(!gvar_horizontal_prefix_cases())return false;
    u8 table[26]{0,1},loca[]{0,0,0,9,0,18},glyf[36]{},metrics[]{2,88,0,0,2,188,0,0};
    table[13]=2;table[19]=26;for(u32 at=0;at<36;at+=18){glyf[at]=glyf[at+1]=255;glyf[at+10]=2;glyf[at+11]=3;}glyf[13]=1;
    SfntGvar gvar{};SfntGvarInstance instance{};
    if(sfnt_gvar_open({table,sizeof(table)},0,2,&gvar)!=SfntStatus::ok||sfnt_gvar_instance_prepare(gvar,{},{},&instance)!=SfntStatus::ok)return false;
    SfntTrueType font{};font.loca={loca,sizeof(loca)};font.glyf={glyf,sizeof(glyf)};font.loca_stride=2;font.metrics.table={metrics,sizeof(metrics)};font.metrics.glyph_count=font.metrics.long_metrics=2;
    u16 result=1;if(sfnt_gvar_design_advance(font,instance,0,&result)!=SfntStatus::malformed||result)return false;
    glyf[10]=0;if(sfnt_gvar_design_advance(font,instance,0,&result)!=SfntStatus::ok||result!=600)return false;
    glyf[13]=2;if(sfnt_gvar_design_advance(font,instance,0,&result)!=SfntStatus::glyph_out_of_range||result)return false;
    return true;
}
