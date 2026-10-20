bool thai_project_native_clusters(ConstSlice<u32> scalars,Slice<u8> boundaries) noexcept {
    char bytes[1024]{};u32 starts[257]{},count=0,units=0;CHECK(scalars.count<=256);
    for(u32 i=0;i<scalars.count;++i){starts[i]=units;count+=utf8_encode(scalars[i],bytes+count);units+=scalars[i]>65535?2u:1u;}
    CHECK(units<=256);if(!owner.native_format)CHECK(native_text_format_capture(&owner));
    auto detached=owner;detached.factory=nullptr;detached.format=nullptr;
    shaped_text_reset(&native_output.text);
    const auto result=text_shape_utf8(&detached,{bytes,count},1000000,1000000,{utf16,256},&native_output.text);
    if(!result.ok()){
        print("thai_native_shape error=");number(static_cast<u32>(result.error));
        print(" native_error=");number(static_cast<u32>(result.native_error));print(" units=");number(result.utf16_units);print("\n");
        return false;
    }
    if(!native_output.text.native_fonts()||native_output.text.cluster_count!=units){
        print("thai_native_output fonts=");number(native_output.text.native_fonts()?1u:0u);
        print(" clusters=");number(native_output.text.cluster_count);print(" expected=");number(units);
        print(" glyphs=");number(native_output.text.glyph_count);print(" runs=");number(native_output.text.run_count);print("\n");
        return false;
    }
    for(u32 i=1;i<scalars.count;++i)if(native_output.text.clusters[starts[i]]==native_output.text.clusters[starts[i]-1])boundaries[i]=0;
    shaped_text_reset(&native_output.text);return true;
}
bool thai_layout_break_probe(const wchar_t* argument,bool measure=false,bool analyzer_only=false,bool shaped=false) noexcept {
    while(*argument==' '||*argument=='\t')++argument;const bool quoted=*argument=='"';if(quoted)++argument;
    wchar_t path[1024]{};u32 length=0;while(*argument&&!(quoted?*argument=='"':*argument==' '||*argument=='\t')){CHECK(length+1<1024);path[length++]=*argument++;}
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);
    struct Close {HANDLE h;~Close() noexcept {CloseHandle(h);}} close{file};LARGE_INTEGER bytes{};CHECK(GetFileSizeEx(file,&bytes)&&bytes.QuadPart>=12&&bytes.QuadPart<32*1024*1024);
    auto* data=static_cast<u8*>(VirtualAlloc(nullptr,static_cast<SIZE_T>(bytes.QuadPart),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(data);
    struct Free {void* p;~Free() noexcept {VirtualFree(p,0,MEM_RELEASE);}} free{data};DWORD read=0;CHECK(ReadFile(file,data,static_cast<DWORD>(bytes.QuadPart),&read,nullptr)&&read==bytes.QuadPart&&bytes_equal(data,"MCLBPRB1",8));
    u32 at=12,total=0;copy_bytes(&total,data+8,4);
    if(measure){
        LARGE_INTEGER frequency{};CHECK(QueryPerformanceFrequency(&frequency));u64 checksum=0;
        print("{\"kind\":\"thai_word_benchmark\",\"cases\":");number(total);print(",\"iterations\":8,\"frequency\":");number(frequency.QuadPart);print(",\"ticks\":[");
        for(u32 batch=0;batch<5;++batch){u64 ticks=0;at=12;
            for(u32 i=0;i<total;++i){u32 id=0,count=0;CHECK(read-at>=8);copy_bytes(&id,data+at,4);copy_bytes(&count,data+at+4,4);at+=8;
                CHECK(id==i+1&&count&&count<=256&&read-at>=count*4);u32 scalars[256]{};copy_bytes(scalars,data+at,count*4);at+=count*4;u8 baseline[257]{},boundaries[257]{};
                CHECK(line_breaks_tailored({scalars,count},{baseline,257}));
                LARGE_INTEGER start{},end{};CHECK(QueryPerformanceCounter(&start));
                for(u32 iteration=0;iteration<8;++iteration){copy_bytes(boundaries,baseline,count+1);CHECK(native_thai_word_breaks({scalars,count},{boundaries,257}));
                    for(u32 c=0;c<=count;++c)checksum=checksum*33+boundaries[c];}
                CHECK(QueryPerformanceCounter(&end)&&end.QuadPart>=start.QuadPart);ticks+=static_cast<u64>(end.QuadPart-start.QuadPart);
            }
            CHECK(at==read);if(batch)print(",");number(ticks);
        }
        print("],\"checksum\":");number(checksum);print("}\n");return true;
    }
    IDWriteTextAnalyzer* analyzer=nullptr;
    struct AnalyzerRelease {IDWriteTextAnalyzer*& p;~AnalyzerRelease() noexcept {com_release(p);}} analyzer_release{analyzer};
    if(analyzer_only)CHECK(SUCCEEDED(owner.factory->CreateTextAnalyzer(&analyzer)));
    u32 unsupported=0;
    print(analyzer_only?"source_line,scalars,analysis,native\n":"source_line,scalars,layout,native\n");
    for(u32 i=0;i<total;++i){u32 id=0,count=0;CHECK(read-at>=8);copy_bytes(&id,data+at,4);copy_bytes(&count,data+at+4,4);at+=8;
        CHECK(id==i+1&&count&&count<=256&&read-at>=count*4);wchar_t text[513]{};u32 starts[257]{},scalars[256]{},units=0;
        for(u32 c=0;c<count;++c){u32 cp=0;copy_bytes(&cp,data+at+c*4,4);CHECK(cp<=0x10ffff&&!(cp>=0xd800&&cp<=0xdfff));scalars[c]=cp;starts[c]=units;
            if(cp>65535){text[units++]=static_cast<wchar_t>(0xd800+((cp-65536)>>10));text[units++]=static_cast<wchar_t>(0xdc00+((cp-65536)&1023));}else text[units++]=static_cast<wchar_t>(cp);}
        starts[count]=units;at+=count*4;IDWriteTextLayout* layout=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextLayout(text,units,owner.format,1000000,1000000,&layout)));
        struct Release {IDWriteTextLayout* p;~Release() noexcept {com_release(p);}} release{layout};DWRITE_CLUSTER_METRICS metrics[512]{};u32 clusters=0;
        CHECK(SUCCEEDED(layout->GetClusterMetrics(metrics,512,&clusters)));u8 breaks[513]{};u32 source=0;
        for(u32 c=0;c<clusters;++c){source+=metrics[c].length;CHECK(source<=units);breaks[source]=metrics[c].canWrapLineAfter?1u:0u;}
        CHECK(source==units);u8 native[257]{};CHECK(line_breaks_tailored({scalars,count},{native,257})&&native_thai_word_breaks({scalars,count},{native,257}));
        if(shaped&&!thai_project_native_clusters({scalars,count},{native,257})){
            print("unsupported_source=");number(id);print("\n");++unsupported;continue;
        }
        if(analyzer_only){
            CHECK(units<=256);AsciiBreakReference reference{};reference.count=units;copy_bytes(reference.units,text,units*sizeof(wchar_t));
            CHECK(SUCCEEDED(analyzer->AnalyzeLineBreakpoints(&reference,0,units,&reference)));
            for(u32 c=0;c<=units;++c)breaks[c]=reference.after(c)?1u:0u;
        }
        // Emit one complete CSV row per write. Per-number console writes cost
        // more than layout in large vocabulary probes. Even arbitrary byte
        // values fit three decimal digits; source length is bounded above.
        char row[2*257*4+32];u32 size=0;
        const auto append=[&](u32 value) noexcept {char digits[10];u32 n=0;do{digits[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);
            while(n)row[size++]=digits[--n];};
        append(id);row[size++]=',';append(count);row[size++]=',';
        for(u32 c=0;c<=count;++c){if(c)row[size++]=' ';append(breaks[starts[c]]);}row[size++]=',';
        for(u32 c=0;c<=count;++c){if(c)row[size++]=' ';append(native[c]);}row[size++]='\n';row[size]=0;print(row);
    }
    CHECK(at==read);if(shaped){print("thai_shaped_unsupported=");number(unsupported);print("\n");}return unsupported==0;
}
