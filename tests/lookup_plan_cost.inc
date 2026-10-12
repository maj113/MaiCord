bool lookup_cost() noexcept {
    const wchar_t* names[]{L"segoeui.ttf",L"seguiemj.ttf",L"consola.ttf",L"seguisym.ttf",L"YuGothR.ttc"};
    const u32 scripts[]{0,sfnt_tag('l','a','t','n'),sfnt_tag('h','e','b','r'),sfnt_tag('k','a','n','a'),sfnt_tag('h','a','n','i')};
    static u64 bits[1024];static u16 indices[65536],expected[65536];
    u64 ticks[2]{},cycles[2]{},calls[2]{},operations[2]{};u32 cases=0;
    LARGE_INTEGER frequency{};if(!QueryPerformanceFrequency(&frequency))return false;
    for(const auto* name:names){wchar_t path[512];const u32 first=GetWindowsDirectoryW(path,480);if(!first||first>=480)return false;
        u32 used=first;for(const wchar_t* c=L"\\Fonts\\";*c;++c)path[used++]=*c;for(const auto* c=name;*c;++c)path[used++]=*c;path[used]=0;
        NativeFontFileSlot file{};NativeFontFaceSlot face{};NativeFontStore store{{&file,1},{&face,1}};NativeFontHandle font{};
        if(!native_font_acquire(&store,path,0,&font).ok())return false;
        struct Release {NativeFontStore& store;NativeFontHandle font;~Release() noexcept {native_font_release(&store,font);}} release{store,font};
        for(u32 kind=0;kind<2;++kind){ConstSlice<u8> table{};auto status=sfnt_table(face.face.geometry.face,kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&table);
            if(status==SfntStatus::missing_table)continue;SfntLayout layout{};
            if(status!=SfntStatus::ok||sfnt_layout_open(table,static_cast<SfntLayoutKind>(kind),&layout)!=SfntStatus::ok||layout.variations)return false;
            const u32 words=(layout.lookup_count+63u)/64;
            for(u32 script:scripts)for(u32 independent=0;independent<2;++independent){SfntLookupPlanResult result{};
                const auto select=[&](Slice<u16> output) noexcept {return script?sfnt_alphabetic_lookup_plan(layout,{},script,independent!=0,{bits,words},output,&result):
                    sfnt_common_lookup_plan(layout,{},{bits,words},output,&result,1000000,independent!=0);};
                if(select({expected,65536})!=SfntStatus::ok)return false;const u32 count=result.lookup_count;++cases;
                for(u32 batch=0;batch<4;++batch){const u32 side=(batch^(batch>>1))&1;LARGE_INTEGER start{},end{};ULONG64 before=0,after=0;u64 work=0;
                    if(!QueryThreadCycleTime(GetCurrentThread(),&before)||!QueryPerformanceCounter(&start))return false;
                    for(u32 iteration=0;iteration<128;++iteration){status=select({});
                        if(status!=SfntStatus::ok&&status!=SfntStatus::output_too_small)return false;const u64 selected=result.operations;
                        status=side?finish_lookup_plan({bits,words},{indices,count},selected,1000000):select({indices,count});
                        if(status!=SfntStatus::ok)return false;
                        work+=selected+(side?(count?words+count:0u):result.operations);
                    }
                    if(!QueryPerformanceCounter(&end)||!QueryThreadCycleTime(GetCurrentThread(),&after))return false;
                    if(result.lookup_count!=count||!bytes_equal(indices,expected,count*2))return false;
                    ticks[side]+=end.QuadPart-start.QuadPart;cycles[side]+=after-before;calls[side]+=128;operations[side]+=work;
                }
            }
        }
    }
    print(utf8_literal("{\"kind\":\"lookup_cost\",\"cases\":"));number(cases);print(utf8_literal(",\"frequency\":"));number(frequency.QuadPart);
    const u64* values[]{ticks,cycles,calls,operations};const Utf8View keys[]{utf8_literal(",\"ticks\":["),utf8_literal(",\"cycles\":["),utf8_literal(",\"calls\":["),utf8_literal(",\"operations\":[")};
    for(u32 i=0;i<4;++i){print(keys[i]);number(values[i][0]);print(utf8_literal(","));number(values[i][1]);print(utf8_literal("]"));}
    print(utf8_literal(",\"exact\":true}\n"));return true;
}
