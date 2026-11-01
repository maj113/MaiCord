const Utf8View directional_cut_holdouts[]{
    utf8_literal("\xd7\x90\xd7\x91" "A\xcc\x81\xcc\x88"),
    utf8_literal("\xd7\x90\xd7\x91\xe2\x9d\xa4\xef\xb8\x8eZ"),
    utf8_literal("\xd7\x90\xd7\x91\xe2\x9d\xa4\xef\xb8\x8fZ"),
    utf8_literal("\xd7\x90\xd7\x91\xce\xb1\xcc\x81"),
    utf8_literal("\xd7\x90\xd7\x91\xd0\x91\xcc\x88"),
    utf8_literal("A\xcc\x81\xcc\x88\xd7\x90\xd7\x91")};
bool directional_editor_hit_corpus(DWriteTextSystem system,bool cuts) noexcept {
    const u64 captured_identity=native_text_format_identity(system);
    CHECK(system.native_text&&system.native_format&&captured_identity);
    const auto& texts=directional_editor_samples;
    const u32 flags[]{0,text_style_bold,text_style_italic,text_style_underline,text_style_monospace,text_style_heading_3,text_style_subtext,text_style_link,text_style_strikethrough,text_style_bold|text_style_italic,text_style_underline|text_style_strikethrough,text_style_heading_1,text_style_heading_2};
    struct Scenario {float width;TextWrapping wrap;};
    const Scenario scenarios[]{{8,TextWrapping::wrap},{95,TextWrapping::wrap},{1000000,TextWrapping::no_wrap}};
    struct ScaleCase {u32 sample,scalar,flag;};
    const ScaleCase cut_scale[]{{6,3,text_style_underline},{11,6,text_style_heading_3},{20,3,text_style_bold}};
    const ScaleCase hit_scale[]{{6,3,text_style_underline},{10,5,text_style_heading_3},{16,1,text_style_bold}};
    const auto* scale_cases=cuts?cut_scale:hit_scale;
    const u32 base_count=sizeof(texts)/sizeof(texts[0]),sample_count=base_count+(cuts?sizeof(directional_cut_holdouts)/sizeof(directional_cut_holdouts[0]):0u);
    for(u32 sample=0;sample<sample_count;++sample){const auto text=sample<base_count?texts[sample]:directional_cut_holdouts[sample-base_count];u32 offsets[256],scalars=0;
        for(u32 byte=0;byte<text.bytes;){offsets[scalars++]=byte;const auto cp=utf8_decode_one(text.data+byte,text.bytes-byte);CHECK(cp.valid);byte+=cp.width;}offsets[scalars]=text.bytes;
        u32 middle=offsets[scalars/2];UnicodeGraphemeState graphemes{};
        for(u32 byte=0;!cuts&&byte<text.bytes;){const auto cp=utf8_decode_one(text.data+byte,text.bytes-byte);CHECK(cp.valid);const bool boundary=unicode_grapheme_boundary(graphemes,cp.codepoint);
            if(byte>=middle&&boundary){middle=byte;break;}byte+=cp.width;if(byte==text.bytes)middle=byte;}
        const auto execute=[&](u32 first,u32 end,u32 flag,const Scenario& scenario) noexcept {
            const TextStyleSpan style{first,end-first,flag};const float width=scenario.width==1000000?scenario.width:scenario.width*system.em_size/16;
                if(!compare_hits(system,text,width,scenario.wrap,{&style,flag&&style.byte_count?1u:0u},true,false,true)){
                    print("directional editor sample/first/end/flag/em/width/nowrap=");number(sample);print("/");number(first);print("/");number(end);print("/");number(flag);print("/");real(system.em_size);print("/");real(width);print("/");number(scenario.wrap==TextWrapping::no_wrap);print("\n");return false;}
            return true;
        };
        if(system.em_size==17.5f){
            for(const auto& scenario:scenarios)CHECK(execute(0,text.bytes,0,scenario));
            // Each scalar has a suffix and a single-scalar style cut. At the
            // final scalar those are one cut; no-style is tested once above.
            for(u32 pattern=0;pattern<(cuts?scalars*2-1:2u);++pattern)for(u32 style_index=1;style_index<(cuts?13u:8u);++style_index){
                const u32 first=cuts?offsets[pattern/2]:pattern?middle:0;
                const u32 end=cuts&&(pattern&1)?offsets[pattern/2+1]:text.bytes;
                for(const auto& scenario:scenarios)CHECK(execute(first,end,flags[style_index],scenario));
            }
        }
        else for(u32 i=0;i<3;++i)if(scale_cases[i].sample==sample){const auto& scale=scale_cases[i];CHECK(execute(offsets[scale.scalar],offsets[scale.scalar+1],scale.flag,scenarios[0]));}
    }
    // The corpus keeps its COM format alive for this complete font/em cell.
    // Re-read the real declaration once more before that owner releases it.
    auto control=system;
    CHECK(native_text_format_capture(&control)&&native_text_format_identity(control)==captured_identity);
    return true;
}
