#pragma once
namespace maicord {
inline bool probe_empty_decoration_contract() noexcept {
    const auto contract_failure=[](u32 check) noexcept {log_u64(LogLevel::error,"probe","empty_decoration_contract_check",check);return false;};
    GlyphQuad quad{};quad.left=73;const auto original=quad;const Color color{1,1,1,1};
    const TextDecoration rectangles[]{
        {3,4,3,9,0,TextDecorationKind::underline},
        {3,4,9,4,0,TextDecorationKind::strikethrough},
        {9,4,3,9,0,TextDecorationKind::background},
        {3,9,9,4,0,TextDecorationKind::background},
        {0,0,0,0,0,TextDecorationKind::underline}};
    for(const auto& rectangle:rectangles)for(u32 capacity=0;capacity<2;++capacity){u32 count=capacity;
        if(!d3d11_glyph_text_append_solid(rectangle,0,0,color,&quad,capacity,&count)||count!=capacity||!bytes_equal(&quad,&original,sizeof(quad)))return contract_failure(1);}
    const TextDecoration visible{3,4,9,10,0,TextDecorationKind::underline};u32 count=0;
    if(d3d11_glyph_text_append_solid(visible,0,0,color,&quad,0,&count)||count||!bytes_equal(&quad,&original,sizeof(quad)))return contract_failure(2);
    count=2;if(d3d11_glyph_text_append_solid(rectangles[0],0,0,color,&quad,1,&count)||count!=2)return contract_failure(3);
    count=0;if(d3d11_glyph_text_append_solid(rectangles[0],0,0,color,nullptr,0,&count)||
        d3d11_glyph_text_append_solid(rectangles[0],0,0,color,&quad,1,nullptr))return contract_failure(4);
    // Invalid non-finite bounds must not become successful empty geometry.
    for(u32 index=0;index<4;++index){auto invalid=rectangles[0];const u32 nan=0x7fc00000u;
        copy_bytes(reinterpret_cast<u8*>(&invalid)+index*4,&nan,4);
        if(d3d11_glyph_text_append_solid(invalid,0,0,color,&quad,1,&count)||count||!bytes_equal(&quad,&original,sizeof(quad)))return contract_failure(5);}
    if(!d3d11_glyph_text_append_solid(visible,10,20,color,&quad,1,&count)||count!=1||quad.left!=13||quad.top!=24||quad.right!=19||quad.bottom!=30)return contract_failure(6);
    return true;
}
inline bool probe_empty_decorations(D3D11Renderer& renderer) noexcept {
    u32 stage=0, current_case=0;bool passed=false;
    struct Diagnostic {u32& stage;u32& current_case;bool& passed;~Diagnostic() noexcept {
        if(!passed){log_u64(LogLevel::error,"probe","empty_decoration_stage",stage);
            log_u64(LogLevel::error,"probe","empty_decoration_case",current_case);}}} diagnostic{stage,current_case,passed};
    D3D11GlyphTextOwner owner{};
    if(!d3d11_glyph_text_initialize(&owner,renderer.graphics.device,renderer.graphics.context,renderer.dpi).ok())return false;
    struct ReleaseOwner {D3D11GlyphTextOwner& value;~ReleaseOwner() noexcept {d3d11_glyph_text_release(&value);}} release_owner{owner};
    stage=2;auto system=owner.system;system.em_size=14;
    if(!native_text_format_create(system.native_text,{{reinterpret_cast<const u16*>(L"Segoe UI"),8},0,{400,5,0},14,56,true},&system.native_format))return false;
    stage=3;TextGlyph glyphs[256]{};TextRun runs[64]{};u16 clusters[256]{};TextDecoration decorations[256]{},filtered_decorations[256]{};
    ShapedText shaped{};wchar_t utf16[256]{};GlyphQuad actual[512]{},expected[512]{};
    if(!shaped_text_initialize(&shaped,{glyphs,256},{runs,64},{clusters,256})||!shaped_text_bind_decorations(&shaped,{decorations,256}))return false;
    struct ReleaseShape {ShapedText& value;~ReleaseShape() noexcept {shaped_text_reset(&value);}} release_shape{shaped};
    const Utf8View inputs[]{utf8_literal("AV \xd7\x90\xd7\x91\xd7\x92 XYZ"),
        utf8_literal("AV \xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 XYZ"),utf8_literal("AV abcdefghijklmnopqrstuvwxyz")};
    const u32 styles[]{text_style_underline,text_style_underline|text_style_strikethrough,text_style_monospace|text_style_code_block};
    const float widths[]{1,8,45,95};u32 cases=0,empty=0;
    for(auto input:inputs)for(u32 style:styles)for(float width:widths){
        stage=4;current_case=cases;auto selected=system;
        const u32 first=style&text_style_code_block?3u:0u;const TextStyleSpan span{first,input.bytes-first,style};
        if(!text_shape_utf8_styled(&selected,input,{&span,1},width,1000000,{utf16,256},&shaped,TextWrapping::no_wrap,TextOverflow::ellipsis).ok())return false;
        stage=5;if(!shaped.native_fonts())return false;
        stage=6;auto filtered=shaped;filtered.decorations=filtered_decorations;filtered.decoration_count=0;
        for(u32 i=0;i<shaped.decoration_count;++i){const auto& d=shaped.decorations[i];
            if(d.right>d.left&&d.bottom>d.top)filtered_decorations[filtered.decoration_count++]=d;else ++empty;}
        u32 expected_count=0,actual_count=0;bool invalidated=false;
        D3D11GlyphTextAppendRequest request{};request.text=&filtered;request.output=expected;request.count=&expected_count;request.atlas_invalidated=&invalidated;
        request.source=input;request.theme=&kDefaultTheme;request.color=kDefaultTheme.text_primary;request.capacity=512;request.dpi=renderer.dpi;
        if(!d3d11_glyph_text_append(&owner,renderer.graphics.context,request))return false;
        const auto builds=owner.cache_builds;request.text=&shaped;request.output=actual;request.count=&actual_count;invalidated=false;
        if(!d3d11_glyph_text_append(&owner,renderer.graphics.context,request)){
            log_u64(LogLevel::error,"probe","empty_decoration_append_failed_case",cases);
            log_u64(LogLevel::error,"probe","empty_decorations_observed",empty);return false;}
        stage=7;if(invalidated||owner.cache_builds!=builds||actual_count!=expected_count||!bytes_equal(actual,expected,actual_count*sizeof(GlyphQuad))){
            log_u64(LogLevel::error,"probe","empty_decoration_invalidated",invalidated);
            log_u64(LogLevel::error,"probe","empty_decoration_new_builds",owner.cache_builds-builds);
            log_u64(LogLevel::error,"probe","empty_decoration_actual_quads",actual_count);
            log_u64(LogLevel::error,"probe","empty_decoration_expected_quads",expected_count);return false;}
        ++cases;
    }
    stage=8;if(!empty||!probe_empty_decoration_contract())return false;
    passed=true;
    log_u64(LogLevel::info,"probe","empty_decoration_cases",cases);
    log_u64(LogLevel::info,"probe","empty_decorations_skipped",empty);
    log_write(LogLevel::info,"probe","empty_decorations_preserve_glyph_quads_and_atlas_cache");return true;
}
}
