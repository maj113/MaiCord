bool ltr_elision_cases() noexcept {
    // Exhaust all short combinations of every admitted bidi class, actual
    // paired brackets, CJK and emoji against both complete resolvers.
    const u32 input[]{'a','1','+','$',',',0x301,'!',32,10,9,'(',')',0x4e00,0x1f600};
    u32 tested=0,checks=0;
    for(u32 length=1,power=14;length<=4;++length,power*=14)for(u32 value=0;value<power;++value){
        u32 scalars[4],digits=value;for(u32 i=0;i<length;++i){scalars[i]=input[digits%14];digits/=14;
            if(!native_text_ltr_class(unicode_properties(scalars[i])->bidi_class()))return false;}
        for(u32 profile=0;profile<2;++profile){
            if(unicode_bidi_classify({scalars,length},{cells,length})!=UnicodeBidiStatus::ok)return false;
            for(u32 first=0;first<length;){u32 end=0,visible=0;UnicodeBidiParagraph paragraph{};
                if(unicode_bidi_paragraph_end({cells,length},first,&end)!=UnicodeBidiStatus::ok||end<=first)return false;
                const auto status=profile?bidi_resolve_tailored({cells+first,end-first},UnicodeParagraphDirection::ltr,{scratch,end-first},&paragraph):
                    unicode_bidi_resolve({cells+first,end-first},UnicodeParagraphDirection::ltr,{scratch,end-first},&paragraph);
                if(status!=UnicodeBidiStatus::ok||paragraph.level||unicode_bidi_line({cells+first,end-first},0,0,end-first,{levels,end-first},{visual,end-first},&visible)!=UnicodeBidiStatus::ok||visible!=end-first)return false;
                for(u32 i=0;i<visible;++i){if(levels[i]||visual[i]!=i)return false;++checks;}first=end;
            }
        }++tested;
    }
    // Every excluded class needs the ordinary path, including AN and BN even
    // when no strong RTL character appears. Invalid enum values also reject.
    const B rejected[]{B::al,B::an,B::bn,B::fsi,B::lre,B::lri,B::lro,B::pdf,B::pdi,B::r,B::rle,B::rli,B::rlo};
    for(auto type:rejected)if(native_text_ltr_class(type))return false;
    for(u32 i=static_cast<u8>(B::ws)+1;i<256;++i)if(native_text_ltr_class(static_cast<B>(i)))return false;
    print(utf8_literal("ltr_elision_cases="));number(tested);print(utf8_literal(" level_order_checks="));number(checks);print(utf8_literal("\n"));return true;
}
