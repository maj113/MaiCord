// QA-only stream of single-script analyzer observations. The command-line seed
// determines the script; each record supplies UTF-16 text and typography/direction.
bool shape_batch_transfer(bool write,void* bytes,u32 count) noexcept {
    auto* p=static_cast<u8*>(bytes);const auto handle=GetStdHandle(write?STD_OUTPUT_HANDLE:STD_INPUT_HANDLE);
    while(count){DWORD done=0;const bool ok=(write?WriteFile(handle,p,count,&done,nullptr):ReadFile(handle,p,count,&done,nullptr))!=FALSE;
        if(!ok||!done)return false;p+=done;count-=done;}return true;
}
bool shape_batch_reference(IDWriteTextAnalyzer* analyzer,IDWriteFontFace* face,DWRITE_SCRIPT_ANALYSIS script,float em,const wchar_t* locale) noexcept {
    char magic[8];u32 count=0;if(!shape_batch_transfer(false,magic,8)||!bytes_equal(magic,"MCSHPB1\0",8)||!shape_batch_transfer(false,&count,4)||count>10000000)return false;
    if(!shape_batch_transfer(true,magic,8)||!shape_batch_transfer(true,&count,4))return false;
    const DWRITE_FONT_FEATURE disabled[]{{DWRITE_FONT_FEATURE_TAG_STANDARD_LIGATURES,0},{DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_LIGATURES,0},{DWRITE_FONT_FEATURE_TAG_KERNING,0}};
    DWRITE_TYPOGRAPHIC_FEATURES features{const_cast<DWRITE_FONT_FEATURE*>(disabled),3};const DWRITE_TYPOGRAPHIC_FEATURES* sets[]{&features};
    for(u32 row=0;row<count;++row){u32 n=0,mode=0;wchar_t text[text_capacity];u16 glyphs[glyph_capacity],clusters[text_capacity];
        DWRITE_SHAPING_TEXT_PROPERTIES text_properties[text_capacity]{};DWRITE_SHAPING_GLYPH_PROPERTIES glyph_properties[glyph_capacity]{};
        if(!shape_batch_transfer(false,&n,4)||!shape_batch_transfer(false,&mode,4)||!n||n>text_capacity||mode>3||!shape_batch_transfer(false,text,n*2))return false;
        u32 actual=0;const bool independent=(mode&1)!=0,rtl=(mode&2)!=0;
        if(FAILED(analyzer->GetGlyphs(text,n,face,FALSE,rtl,&script,locale,nullptr,independent?sets:nullptr,independent?&n:nullptr,independent?1:0,glyph_capacity,clusters,text_properties,glyphs,glyph_properties,&actual)))return false;
        float advances[glyph_capacity];DWRITE_GLYPH_OFFSET offsets[glyph_capacity];
        if(FAILED(analyzer->GetGlyphPlacements(text,clusters,text_properties,n,glyphs,glyph_properties,actual,face,em,FALSE,rtl,&script,locale,independent?sets:nullptr,independent?&n:nullptr,independent?1:0,advances,offsets)))return false;
        if(!shape_batch_transfer(true,&actual,4)||!shape_batch_transfer(true,glyphs,actual*2)||!shape_batch_transfer(true,clusters,n*2))return false;
        for(u32 i=0;i<actual;++i){float values[]{advances[i],offsets[i].advanceOffset,offsets[i].ascenderOffset};if(!shape_batch_transfer(true,values,12))return false;}
    }return true;
}
