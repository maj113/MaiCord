u32 styled_hit_styles(Utf8View text,u32 style,TextStyleSpan* spans) noexcept {
    u32 count=1;
    if(style==0)spans[0]={3,6,text_style_link};
    if(style==1)spans[0]={3,6,text_style_bold};
    if(style==2)spans[0]={3,6,text_style_italic};
    if(style==3){spans[0]={0,3,text_style_link};spans[1]={3,text.bytes-3,text_style_link};count=2;}
    if(style==4){spans[0]={0,3,text_style_underline};spans[1]={3,6,text_style_heading_3|text_style_underline};spans[2]={9,text.bytes-9,text_style_bold|text_style_underline};count=3;}
    if(style==5)spans[0]={9,text.bytes-9,text_style_subtext};
    if(style==6)spans[0]={0,text.bytes,text_style_monospace};
    if(style==7){spans[0]={0,2,text_style_heading_1};spans[1]={2,1,text_style_subtext};spans[2]={3,text.bytes-3,text_style_mention};count=3;}
    return count;
}
const Utf8View styled_hit_texts[]{utf8_literal("AV office caf\xc3\xa9 fi AV"),utf8_literal("AV office e\xcc\x81 fi AV\n\xc3\xa9 AV"),utf8_literal("AV\toffice\r\n\xc3\xa9 AV\n"),utf8_literal("AV office\n\n"),utf8_literal("AV office\t\t   "),utf8_literal("AV office\r\n\t\t")};
bool styled_hit_corpus(DWriteTextSystem system,bool required) noexcept {
    const float widths[]{8.f,37.f,95.f,1000000.f};
    for(u32 sample=0;sample<6;++sample)for(u32 style=0;style<8;++style){const auto text=styled_hit_texts[sample];TextStyleSpan spans[3]{};const u32 count=styled_hit_styles(text,style,spans);
        for(u32 fixed=0;fixed<2;++fixed){system.fixed_weight=fixed!=0;
            if(!required){NativeParagraphHits hits{};SfntFontLineParts parts[513];NativeParagraphHitStorage storage{{hit_clusters,256},{hit_lines,257},{hit_advances,1280},&hits,{parts,513}};TextMeasureResult measure{};
                if(!native_text_candidate_styled(system,text,{spans,count},1000000,1000000,nullptr,&measure,TextWrapping::wrap,TextOverflow::clip,{},&storage))continue;}
            for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap)if(!compare_hits(system,text,width,nowrap?TextWrapping::no_wrap:TextWrapping::wrap,{spans,count})){
                print("styled hit sample/style/fixed/em/width/nowrap=");number(sample);print("/");number(style);print("/");number(fixed);print("/");real(system.format->GetFontSize());print("/");real(width);print("/");number(nowrap);print("\n");return false;}
        }
    }return true;
}
