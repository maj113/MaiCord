#pragma once
namespace maicord {
// Prove that a native profile run resolves to the shipped private file, without
// requiring the production renderer to manufacture a Windows font collection.
inline bool probe_native_profile_asset(TextSystem system,u16 id) noexcept {
    const wchar_t* files[]{L"Bangers.ttf",L"BioRhyme.ttf",L"CherryBomb.ttf",L"Chicle.ttf",L"Compagnon.otf",L"MuseoModerno.ttf",L"NeoCastel.otf",L"Pixelify.ttf",L"Ribes.otf",L"Sinistre.otf",nullptr,L"ZillaSlab.ttf",L"Playpen.ttf",L"Orbitron.ttf",L"NewRocker.ttf",L"Kalam.ttf"};
    if(!id||id>16||!files[id-1]||!system.native_format||!system.native_fonts)return false;
    wchar_t path[1024]{};const DWORD length=GetModuleFileNameW(nullptr,path,1024);if(!length||length>=900)return false;
    u32 at=length;while(at&&path[at-1]!=L'\\')--at;const wchar_t prefix[]=L"fonts\\";
    for(u32 i=0;i<6;++i)path[at++]=prefix[i];for(u32 i=0;files[id-1][i];++i)path[at++]=files[id-1][i];path[at]=0;
    auto* store=native_font_session_fonts(system.native_fonts);NativeFontHandle expected{};
    if(!native_font_acquire(store,path,0,&expected).ok())return false;
    ShapedText output{};TextGlyph glyphs[8]{};TextRun runs[8]{};u16 clusters[8]{};wchar_t utf16[8]{};
    struct Release {NativeFontStore* store;NativeFontHandle expected;ShapedText& output;~Release() noexcept {shaped_text_reset(&output);native_font_release(store,expected);}} release{store,expected,output};
    if(!shaped_text_initialize(&output,{glyphs,8},{runs,8},{clusters,8})||!text_shape_utf8(&system,utf8_literal("AV"),1000,1000,{utf16,8},&output).ok()||!output.run_count||!output.glyph_count)return false;
    for(u32 i=0;i<output.run_count;++i){if(!output.runs[i].native_font)return false;
        const auto* run=native_font_run_get(*native_font_session_runs(output.native_fonts()),output.runs[i].native_face);
        if(!run||run->source.face.slot!=expected.slot||run->source.face.generation!=expected.generation)return false;}
    return true;
}
}
