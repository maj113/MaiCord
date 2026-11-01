bool native_editor_policy_probe() noexcept {
    IDWriteTextFormat* format=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,14,L"en-US",&format)));
    struct Release {IDWriteTextFormat*& f;~Release() noexcept{com_release(f);}} release{format};
    const Utf8View texts[]{utf8_literal("AV To"),utf8_literal("AV \xf0\x9f\x98\x80 To"),utf8_literal("AV ffi \xf0\x9f\x98\x80 To"),utf8_literal("AV \xce\xb1\xf0\x9f\x98\x80\xd0\x91 To")};
    const float widths[]{7,1000000};
    for(u32 t=0;t<4;++t)for(u32 independent=0;independent<2;++independent)for(u32 tracking=0;tracking<2;++tracking)for(float width:widths)for(u32 wrap=0;wrap<2;++wrap){
        const auto text=texts[t];const int units=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data,text.bytes,utf16,256);CHECK(units>0);
        IDWriteTextLayout* layout=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextLayout(utf16,units,format,width,1000000,&layout)));
        struct LayoutRelease {IDWriteTextLayout*& p;~LayoutRelease() noexcept{com_release(p);}} layout_release{layout};
        auto system=owner;system.native_text=nullptr;system.format=format;system.em_size=14;system.independent_letters=independent!=0;system.tracking=tracking?.03125f:0;
        CHECK(SUCCEEDED(layout->SetWordWrapping(wrap?DWRITE_WORD_WRAPPING_WRAP:DWRITE_WORD_WRAPPING_NO_WRAP))&&SUCCEEDED(text_apply_layout_styles(system,layout,text,{})));
        DWRITE_CLUSTER_METRICS clusters[256];u32 count=0;DWRITE_TEXT_METRICS metrics{};CHECK(SUCCEEDED(layout->GetClusterMetrics(clusters,256,&count))&&SUCCEEDED(layout->GetMetrics(&metrics)));
        print("{\"text\":");number(t);print(",\"independent\":");number(independent);print(",\"tracking\":");number(tracking);print(",\"limit\":");real(width);print(",\"wrap\":");number(wrap);print(",\"width\":");real(metrics.width);print(",\"clusters\":[");
        u32 at=0;for(u32 i=0;i<count;++i){if(i)print(",");DWRITE_HIT_TEST_METRICS hit{};float x=0,y=0;CHECK(SUCCEEDED(layout->HitTestTextPosition(at,FALSE,&x,&y,&hit)));
            print("[");number(clusters[i].length);print(",");real(clusters[i].width);print(",");real(x);print(",");real(y);print(",");real(hit.width);print("]");at+=clusters[i].length;}print("]}\n");
    }return true;
}
