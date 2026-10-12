// QA-only Windows ownership/cache observations; never linked into the app.
volatile u32 collection_phase=0;
__declspec(noinline) void collection_marker(u32 phase) noexcept {collection_phase=phase;}
usize canonical(IUnknown* object) noexcept {
    IUnknown* token=nullptr;if(!object||FAILED(object->QueryInterface(__uuidof(IUnknown),reinterpret_cast<void**>(&token))))return 0;
    const auto value=reinterpret_cast<usize>(token);release(token);return value;
}
bool collection_family_name(IDWriteFontCollection* collection,u32 index,wchar_t (&output)[256]) noexcept {
    IDWriteFontFamily* family=nullptr;IDWriteLocalizedStrings* names=nullptr;
    struct Cleanup {IDWriteFontFamily*& f;IDWriteLocalizedStrings*& n;~Cleanup() noexcept {release(n);release(f);}} cleanup{family,names};
    CHECK(SUCCEEDED(collection->GetFontFamily(index,&family))&&SUCCEEDED(family->GetFamilyNames(&names)));
    UINT32 language=0,units=0;BOOL exists=FALSE;
    CHECK(SUCCEEDED(names->FindLocaleName(L"en-us",&language,&exists)));if(!exists)language=0;
    CHECK(SUCCEEDED(names->GetStringLength(language,&units))&&units<256&&SUCCEEDED(names->GetString(language,output,256)));return true;
}
bool collection_lifetime(IDWriteFactory3* factory,const wchar_t* first,const wchar_t* second) noexcept {
    IDWriteFontCollection *system[3]{},*old=nullptr,*next=nullptr,*reverse=nullptr,*retained=nullptr;
    IDWriteFontCollection3* expiration=nullptr;IDWriteFontSetBuilder *builder=nullptr,*reversed=nullptr;
    IDWriteFontSet *sets[3]{};IDWriteFontFaceReference *references[2]{},*ordered[3]{};
    IDWriteTextFormat* format=nullptr;IDWriteTextLayout* layout=nullptr;
    struct Cleanup {
        IDWriteFontCollection **system,*&old,*&next,*&reverse,*&retained;IDWriteFontCollection3*& expiration;
        IDWriteFontSetBuilder *&builder,*&reversed;IDWriteFontSet** sets;IDWriteFontFaceReference **references,**ordered;
        IDWriteTextFormat*& format;IDWriteTextLayout*& layout;
        ~Cleanup() noexcept {release(layout);release(format);release(retained);release(reverse);release(next);release(old);
            for(u32 i=0;i<3;++i){release(system[i]);release(sets[i]);release(ordered[i]);}
            for(u32 i=0;i<2;++i)release(references[i]);release(reversed);release(builder);release(expiration);}
    } cleanup{system,old,next,reverse,retained,expiration,builder,reversed,sets,references,ordered,format,layout};
    collection_marker(1);
    auto* base=static_cast<IDWriteFactory*>(factory);
    CHECK(SUCCEEDED(base->GetSystemFontCollection(system,FALSE))&&SUCCEEDED(base->GetSystemFontCollection(system+1,FALSE))&&
        SUCCEEDED(base->GetSystemFontCollection(system+2,TRUE)));
    const bool same_cached=canonical(system[0])==canonical(system[1]),same_checked=canonical(system[0])==canonical(system[2]);
    CHECK(SUCCEEDED(system[0]->QueryInterface(__uuidof(IDWriteFontCollection3),reinterpret_cast<void**>(&expiration))));
    const HANDLE event=expiration->GetExpirationEvent();CHECK(event);const u32 event_state=WaitForSingleObject(event,0);CHECK(event_state!=WAIT_FAILED);
    collection_marker(2);
    LARGE_INTEGER frequency{},begin{},end{};ULONG64 cycles_begin=0,cycles_end=0;
    CHECK(QueryPerformanceFrequency(&frequency));collection_marker(3);
    CHECK(QueryThreadCycleTime(GetCurrentThread(),&cycles_begin)&&QueryPerformanceCounter(&begin));
    for(u32 i=0;i<32;++i){IDWriteFontCollection* hit=nullptr;CHECK(SUCCEEDED(base->GetSystemFontCollection(&hit,FALSE)));release(hit);}
    CHECK(QueryPerformanceCounter(&end)&&QueryThreadCycleTime(GetCurrentThread(),&cycles_end));collection_marker(4);
    // Each set is a snapshot of its builder. A new collection may assign the
    // same numeric index to a different file; the old format retains its owner.
    CHECK(SUCCEEDED(factory->CreateFontFaceReference(first,nullptr,0,DWRITE_FONT_SIMULATIONS_NONE,references))&&
        SUCCEEDED(factory->CreateFontFaceReference(second,nullptr,0,DWRITE_FONT_SIMULATIONS_NONE,references+1)));
    CHECK(!references[0]->Equals(references[1]));
    CHECK(SUCCEEDED(factory->CreateFontSetBuilder(&builder))&&SUCCEEDED(builder->AddFontFaceReference(references[0]))&&SUCCEEDED(builder->CreateFontSet(sets)));
    CHECK(SUCCEEDED(builder->AddFontFaceReference(references[1]))&&SUCCEEDED(builder->CreateFontSet(sets+1)));
    CHECK(SUCCEEDED(factory->CreateFontSetBuilder(&reversed))&&SUCCEEDED(reversed->AddFontFaceReference(references[1]))&&
        SUCCEEDED(reversed->AddFontFaceReference(references[0]))&&SUCCEEDED(reversed->CreateFontSet(sets+2)));
    CHECK(sets[0]->GetFontCount()==1&&sets[1]->GetFontCount()==2&&sets[2]->GetFontCount()==2);
    for(u32 i=0;i<3;++i)CHECK(SUCCEEDED(sets[i]->GetFontFaceReference(0,ordered+i)));
    CHECK(ordered[0]->Equals(ordered[1])&&!ordered[0]->Equals(ordered[2]));
    IDWriteFontFaceReference* other=nullptr;CHECK(SUCCEEDED(sets[1]->GetFontFaceReference(1,&other)));
    const bool same_materialized=ordered[2]->Equals(other)!=FALSE;release(other);CHECK(same_materialized);
    text("{\"kind\":\"reference_equalities\",\"old_new\":");number(ordered[0]->Equals(ordered[1]));
    text(",\"old_reverse\":");number(ordered[0]->Equals(ordered[2]));text(",\"reverse_input_b\":");number(ordered[2]->Equals(references[1]));text("}\n");
    IDWriteFontCollection1* made=nullptr;
    CHECK(SUCCEEDED(factory->CreateFontCollectionFromFontSet(sets[0],&made)));old=made;made=nullptr;
    CHECK(SUCCEEDED(factory->CreateFontCollectionFromFontSet(sets[1],&made)));next=made;made=nullptr;
    CHECK(SUCCEEDED(factory->CreateFontCollectionFromFontSet(sets[2],&made)));reverse=made;
    wchar_t old_family[256]{},second_family[256]{};CHECK(collection_family_name(old,0,old_family)&&collection_family_name(reverse,0,second_family));
    CHECK(old->GetFontFamilyCount()==1&&next->GetFontFamilyCount()==2&&reverse->GetFontFamilyCount()==2);
    UINT32 index=0;BOOL exists=FALSE;CHECK(SUCCEEDED(old->FindFamilyName(second_family,&index,&exists))&&!exists);
    CHECK(SUCCEEDED(next->FindFamilyName(second_family,&index,&exists))&&exists);
    CHECK(canonical(old)!=canonical(next)&&canonical(next)!=canonical(reverse));
    const usize old_identity=canonical(old);CHECK(old_identity);collection_marker(5);
    CHECK(SUCCEEDED(factory->CreateTextFormat(old_family,old,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,16,L"en-us",&format)));
    release(old);release(next);release(reverse);release(builder);release(reversed);
    for(u32 i=0;i<3;++i){release(sets[i]);release(ordered[i]);}for(u32 i=0;i<2;++i)release(references[i]);
    collection_marker(6);
    CHECK(SUCCEEDED(format->GetFontCollection(&retained))&&canonical(retained)==old_identity&&retained->GetFontFamilyCount()==1);
    CHECK(SUCCEEDED(retained->FindFamilyName(second_family,&index,&exists))&&!exists);
    CHECK(SUCCEEDED(factory->CreateTextLayout(L"A",1,format,256,64,&layout)));release(format);release(retained);
    CHECK(SUCCEEDED(layout->GetFontCollection(0,&retained))&&canonical(retained)==old_identity&&retained->GetFontFamilyCount()==1);
    DWRITE_TEXT_METRICS metrics{};CHECK(SUCCEEDED(layout->GetMetrics(&metrics))&&metrics.width>0&&metrics.height>0);collection_marker(7);
    const auto print64=[](u64 value) noexcept {char digits[32];u32 n=0;do{digits[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);while(n)ch(digits[--n]);};
    text("{\"kind\":\"collection_lifetime\",\"exact\":true,\"same_cached_system\":");text(same_cached?"true":"false");
    text(",\"same_checked_system\":");text(same_checked?"true":"false");text(",\"expiration_state\":");number(event_state);
    text(",\"old_set_count\":1,\"new_set_count\":2,\"same_index_different_face\":true,\"format_retained_collection\":true,\"layout_retained_collection\":true,\"cache_get_calls\":32,\"frequency\":");print64(frequency.QuadPart);
    text(",\"cached_ticks\":");print64(end.QuadPart-begin.QuadPart);text(",\"cached_cycles\":");print64(cycles_end-cycles_begin);
    text(",\"old_family\":");wide(old_family);text(",\"added_family\":");wide(second_family);text("}\n");return true;
}
