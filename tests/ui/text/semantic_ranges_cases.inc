inline bool semantic_compare(AppSystem system,Utf8View input,ConstSlice<TextStyleSpan> styles,ConstSlice<TextHitSpan> hits,bool required,float width,TextWrapping wrap) noexcept {
    ++cases;shaped_text_reset(&native_output.text);
    TextHitRange ranges[kTextHitSpanMaximum];
    for(u32 i=0;i<hits.count;++i){u32 first=0,end=0;CHECK(utf8_utf16_position_for_byte_offset(input,hits[i].byte_offset,&first)&&utf8_utf16_position_for_byte_offset(input,hits[i].byte_offset+hits[i].byte_count,&end));ranges[i]={first,end-first,hits[i].semantic_index,0};}
    const bool native=native_text_candidate_styled(system,{input,styles,width,1000000,wrap,TextOverflow::clip,{ranges,hits.count}},&native_output.text,nullptr);
    if(required&&!native){for(u32 i=0;i<hits.count;++i){shaped_text_reset(&native_output.text);
        if(!native_text_candidate_styled(system,{input,styles,width,1000000,wrap,TextOverflow::clip,{ranges+i,1}},&native_output.text,nullptr)){
            print("rejected hit first/count=");number(ranges[i].first_unit);print("/");number(ranges[i].unit_count);print("\n");break;}}shaped_text_reset(&native_output.text);}
    CHECK(!required||native);observe_value(static_cast<u32>(native));if(native)++native_cases;shaped_text_reset(&native_output.text);
    const auto shaped=text_shape_utf8(&system,{input,styles,width,1000000,wrap,TextOverflow::clip,hits},&native_output.text);
    observe(shaped);if(shaped.ok()){CHECK(native_output.text.native_fonts());observe(native_output.text,input,system);}
    return true;
}
#include "directional_semantic_cases.inc"
inline bool semantic_corpus(AppSystem system,bool required) noexcept {
    const Utf8View texts[]{utf8_literal("AV office caf\xc3\xa9 fi AV"),utf8_literal("AV office e\xcc\x81 fi AV\n\xc3\xa9 AV"),utf8_literal("AV\toffice\r\n\xc3\xa9 AV\n"),utf8_literal("AV office\n\n"),utf8_literal("AV office\t\t   "),utf8_literal("AV office\r\n\t\t")};
    const float widths[]{8.f,37.f,95.f,1000000.f};
    for(u32 sample=0;sample<6;++sample){const auto text=texts[sample];u32 offsets[256],boundaries=0;
        for(u32 at=0;at<text.bytes;){offsets[boundaries++]=at;const auto cp=utf8_decode_one(text.data+at,text.bytes-at);CHECK(cp.valid);at+=cp.width;}offsets[boundaries++]=text.bytes;
        for(u32 style=0;style<8;++style){TextStyleSpan spans[3]{};u32 style_count=0;
            if(style==1){spans[0]={0,text.bytes,text_style_link};style_count=1;}
            if(style==2){spans[0]={3,6,text_style_bold};style_count=1;}
            if(style==3){spans[0]={0,3,text_style_link};spans[1]={3,text.bytes-3,text_style_link};style_count=2;}
            if(style==4){spans[0]={0,3,text_style_underline};spans[1]={3,6,text_style_heading_3|text_style_underline};spans[2]={9,text.bytes-9,text_style_bold|text_style_underline};style_count=3;}
            if(style==5){spans[0]={9,text.bytes-9,text_style_spoiler};style_count=1;}
            if(style==6){spans[0]={0,text.bytes,text_style_monospace};style_count=1;}
            if(style==7){spans[0]={0,2,text_style_heading_1};spans[1]={2,1,text_style_subtext};spans[2]={3,text.bytes-3,text_style_mention};style_count=3;}
            for(u32 mode=0;mode<2;++mode){system.independent_letters=mode!=0;
            for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap){TextHitSpan hits[kTextHitSpanMaximum];u32 count=0;
                const auto submit=[&]() noexcept {return semantic_compare(system,text,{spans,style_count},{hits,count},required,width,nowrap?TextWrapping::no_wrap:TextWrapping::wrap);};
                for(u32 first=0;first+1<boundaries;++first)for(u32 last=boundaries-1;last>first;--last){hits[count]={offsets[first],offsets[last]-offsets[first],static_cast<u16>(65535-count),0};++count;
                    if(count==kTextHitSpanMaximum){if(!submit()){print("semantic sample/style/width/mode/nowrap=");number(sample);print("/");number(style);print("/");real(width);print("/");number(mode);print("/");number(nowrap);print("\n");return false;}count=0;}}
                if(count&&!submit()){print("semantic sample/style/width/mode/nowrap=");number(sample);print("/");number(style);print("/");real(width);print("/");number(mode);print("/");number(nowrap);print("\n");return false;}
            }}
        }
    }return true;
}
inline bool semantic_long_range() noexcept {
    // More than 256 rectangles: one per line of each hit range, all observed, and a capacity one short is refused
    // without a partial output.
    static TextGlyph glyphs[1024];static TextRun runs[1024];static u16 clusters[1024];
    static TextHitRegion regions[1024];static char bytes[1024];ShapedText output{};
    CHECK(shaped_text_initialize(&output,{glyphs,1024},{runs,1024},{clusters,1024},{},{regions,1024}));
    struct Release {ShapedText& text;~Release() noexcept {shaped_text_reset(&text);}} release{output};
    const u32 lengths[]{255,256,257,400};for(u32 lines:lengths){
        const u32 units=lines*2-1;for(u32 i=0;i<units;++i)bytes[i]=i&1?'\n':'a';
        const TextHitRange hits[]{{0,units,65535,0},{2,units-4,17,0}};
        ++cases;shaped_text_reset(&output);
        CHECK(native_text_candidate_styled(owner,{{bytes,units},{},1000000,1000000,TextWrapping::wrap,TextOverflow::clip,{hits,2}},&output,nullptr));++native_cases;
        const u32 written=output.hit_region_count;
        CHECK(written==lines*2-2&&output.native_fonts());observe(output,{bytes,units},owner);
        shaped_text_reset(&output);output.hit_region_capacity=written-1;
        CHECK(!native_text_candidate_styled(owner,{{bytes,units},{},1000000,1000000,TextWrapping::wrap,TextOverflow::clip,{hits,2}},&output,nullptr));
        CHECK(!output.native_fonts()&&!output.run_count&&!output.hit_region_count);
        output.hit_region_capacity=written;
        CHECK(native_text_candidate_styled(owner,{{bytes,units},{},1000000,1000000,TextWrapping::wrap,TextOverflow::clip,{hits,2}},&output,nullptr));
        CHECK(output.hit_region_count==written);shaped_text_reset(&output);output.hit_region_capacity=1024;
    }return true;
}
inline bool semantic_options() noexcept {
    CHECK(semantic_long_range());
    const float stops[]{3.5f,91.25f};
    for(float stop:stops){auto system=owner;CHECK(set_format(system,L"Segoe UI",16,400,0,stop));CHECK(semantic_corpus(system,true));}
    const auto text=utf8_literal("AV office e\xcc\x81 fi AV\n\xc3\xa9 AV");
    const TextStyleSpan style{3,6,text_style_link};const TextHitSpan hit{0,text.bytes,65535,0};
    // A preceding draw with no requested geometry must not reuse an absent
    // line table. Then changing only hit ranges must reuse the prepared shape.
    CHECK(text_shape_utf8(&owner, {text,{&style,1},37,1000000}, &native_output.text).ok());
    CHECK(semantic_compare(owner,text,{&style,1},{&hit,1},true,37,TextWrapping::wrap));
    shaped_text_reset(&native_output.text);u32 units=0;CHECK(utf8_utf16_position_for_byte_offset(text,text.bytes,&units));const TextHitRange range{0,units,65535,0};
    const u32 capacity=native_output.text.hit_region_capacity;native_output.text.hit_region_capacity=1;
    CHECK(!native_text_candidate_styled(owner,{text,{&style,1},37,1000000,TextWrapping::wrap,TextOverflow::clip,{&range,1}},&native_output.text,nullptr));
    CHECK(!native_output.text.native_fonts()&&!native_output.text.run_count&&!native_output.text.hit_region_count&&!native_output.text.decoration_count);
    native_output.text.hit_region_capacity=capacity;
    CHECK(semantic_compare(owner,text,{&style,1},{&hit,1},true,37,TextWrapping::wrap));
    const TextHitSpan bads[]{{0,0,0,0},{text.bytes,1,0,0},{11,1,0,0}};
    for(const auto& bad:bads){const auto result=text_shape_utf8(&owner, {text,{&style,1},37,1000000,TextWrapping::wrap,TextOverflow::clip,{&bad,1}}, &native_output.text);
        CHECK(result.error==TextShapeError::invalid_input&&!native_output.text.native_fonts()&&!native_output.text.run_count&&!native_output.text.hit_region_count);}
    CHECK(semantic_compare(owner,text,{&style,1},{&hit,1},true,37,TextWrapping::wrap));return true;
}
