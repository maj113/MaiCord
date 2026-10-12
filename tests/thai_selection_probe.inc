bool native_thai_selection_probe(const wchar_t* argument,bool lexical=false,bool syllable=false,bool forward=false) noexcept {
    while(*argument==' '||*argument=='\t')++argument;
    const bool quoted=*argument=='"';if(quoted)++argument;
    wchar_t path[1024]{};u32 length=0;
    while(*argument&&!(quoted?*argument=='"':*argument==' '||*argument=='\t')){
        CHECK(length+1<1024);path[length++]=*argument++;
    }
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(file!=INVALID_HANDLE_VALUE);
    struct Close {HANDLE value;~Close() noexcept {CloseHandle(value);}} close{file};
    LARGE_INTEGER size{};CHECK(GetFileSizeEx(file,&size)&&size.QuadPart>=12&&size.QuadPart<128*1024*1024);
    auto* data=static_cast<u8*>(VirtualAlloc(nullptr,static_cast<SIZE_T>(size.QuadPart),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(data);
    struct Free {void* value;~Free() noexcept {VirtualFree(value,0,MEM_RELEASE);}} free{data};
    DWORD bytes=0;CHECK(ReadFile(file,data,static_cast<DWORD>(size.QuadPart),&bytes,nullptr)&&bytes==size.QuadPart);
    CHECK(bytes_equal(data,forward?"MCTHSEG1":syllable?"MCTHSYL1":lexical?"MCTHLEX2":"MCTHSEL1",8));u32 total=0,at=12;copy_bytes(&total,data+8,4);
    if(forward){
        auto* guarded=static_cast<u8*>(VirtualAlloc(nullptr,12288,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS));CHECK(guarded);
        Free guarded_free{guarded};DWORD old=0;CHECK(VirtualProtect(guarded+4096,4096,PAGE_READWRITE,&old));u32 segments=0;
        struct Output {u8 lengths[257];u32 count;bool overflow;};
        const auto sink=[](void* opaque,u8 length) noexcept {auto& out=*static_cast<Output*>(opaque);
            if(out.count<257)out.lengths[out.count++]=length;else out.overflow=true;};
        for(u32 row=0;row<total;++row){
            CHECK(bytes-at>=8);u32 count=0;copy_bytes(&count,data+at,4);at+=4;CHECK(count&&count<=256&&count<=(bytes-at-4)/4);
            auto* scalars=reinterpret_cast<u32*>(guarded+8192-count*4);copy_bytes(scalars,data+at,count*4);at+=count*4;
            u32 expected=0;copy_bytes(&expected,data+at,4);at+=4;CHECK(expected<=count&&expected<=bytes-at);
            Output actual{};CHECK(native_thai_forward_segments({scalars,count},&actual,sink));
            if(actual.overflow||actual.count!=expected||!bytes_equal(actual.lengths,data+at,expected)){
                print("Thai forward mismatch row=");number(row+1);print(" expected=");for(u32 i=0;i<expected;++i){number(data[at+i]);print(" ");}
                print(" actual=");for(u32 i=0;i<actual.count;++i){number(actual.lengths[i]);print(" ");}print("\n");
            }
            CHECK(!actual.overflow&&actual.count==expected&&bytes_equal(actual.lengths,data+at,expected));at+=expected;segments+=expected;
        }
        CHECK(at==bytes);print("thai_forward_spans=");number(total);print(" segments=");number(segments);print("\n");return true;
    }
    if(syllable){
        CHECK(native_thai_unknown_recovery({},0)==0&&native_thai_syllable_result({},0)==0);
        auto* guarded=static_cast<u8*>(VirtualAlloc(nullptr,12288,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS));CHECK(guarded);
        Free guarded_free{guarded};DWORD old_protect=0;CHECK(VirtualProtect(guarded+4096,4096,PAGE_READWRITE,&old_protect));
        u32 queries=0;
        for(u32 row=0;row<total;++row){
            CHECK(bytes-at>=12);u32 count=0;copy_bytes(&count,data+at,4);at+=4;
            CHECK(count&&count<=256&&count<=(bytes-at-8)/12);
            auto* scalars=reinterpret_cast<u32*>(guarded+8192-count*4);copy_bytes(scalars,data+at,count*4);at+=count*4;
            CHECK(native_thai_syllable_result({scalars,count},count+1)==0&&native_thai_unknown_recovery({scalars,count},count+1)==0);
            for(u32 position=0;position<=count;++position){
                i32 expected_raw=0;u32 expected_recovery=0;
                copy_bytes(&expected_raw,data+at,4);copy_bytes(&expected_recovery,data+at+4,4);at+=8;
                const i32 raw=native_thai_syllable_result({scalars,count},position);
                const u32 recovery=native_thai_unknown_recovery({scalars,count},position);
                if(raw!=expected_raw||recovery!=expected_recovery){
                    print("Thai syllable mismatch row=");number(row+1);print(" position=");number(position);print("\n");
                }
                CHECK(raw==expected_raw&&recovery==expected_recovery);++queries;
            }
        }
        CHECK(at==bytes);print("thai_syllable_queries=");number(queries);print("\n");return true;
    }
    if(lexical){
        u32 queries=0,selections=0;
        for(u32 row=0;row<total;++row){
            CHECK(bytes-at>=4);u32 count=0;copy_bytes(&count,data+at,4);at+=4;
            CHECK(count&&count<=256&&count<=(bytes-at)/36);u32 scalars[256]{};copy_bytes(scalars,data+at,count*4);at+=count*4;
            for(u32 position=0;position<count;++position){
                NativeThaiWordCandidates expected{};copy_bytes(&expected,data+at,32);at+=32;
                const auto actual=native_thai_lexical_candidates({scalars,count},position);
                if(!bytes_equal(&actual,&expected,32)){
                    print("Thai lexical mismatch row=");number(row+1);print(" position=");number(position);print(" expected=");
                    for(u32 i=0;i<8;++i){number(static_cast<u32>(expected.lengths[i]));print(" ");}print(" actual=");
                    for(u32 i=0;i<8;++i){number(static_cast<u32>(actual.lengths[i]));print(" ");}print("\n");
                }
                CHECK(bytes_equal(&actual,&expected,32));++queries;
            }
            CHECK(bytes-at>=4);u32 decisions=0;copy_bytes(&decisions,data+at,4);at+=4;
            CHECK(decisions&&decisions<=count&&decisions<=(bytes-at)/72);
            ConstSlice<u32> text{scalars,count};NativeThaiWordCandidates retained{};
            const auto native_lookup=[](void* opaque,u32 position) noexcept {
                return native_thai_lexical_candidates(*static_cast<ConstSlice<u32>*>(opaque),position);
            };
            for(u32 step=0;step<decisions;++step){
                u32 position=0,expected_count=0;copy_bytes(&position,data+at,4);copy_bytes(&expected_count,data+at+4,4);
                NativeThaiWordCandidates expected{},expected_retained{},actual{};
                copy_bytes(&expected,data+at+8,32);copy_bytes(&expected_retained,data+at+40,32);at+=72;
                CHECK(position<count&&expected_count<=8);
                const u32 actual_count=native_thai_word_select(position,count,&text,native_lookup,retained,actual);
                if(actual_count!=expected_count||!bytes_equal(&actual,&expected,32)||!bytes_equal(&retained,&expected_retained,32)){
                    print("Thai composed selection mismatch row=");number(row+1);print(" position=");number(position);print("\n");
                }
                CHECK(actual_count==expected_count&&bytes_equal(&actual,&expected,32)&&bytes_equal(&retained,&expected_retained,32));++selections;
            }
        }
        CHECK(at==bytes);print("thai_lexical_queries=");number(queries);print(" thai_composed_selections=");number(selections);print("\n");return true;
    }
    struct Queries {const u8* data;u32 count;bool missing;};
    const auto lookup=[](void* opaque,u32 position) noexcept {
        auto& q=*static_cast<Queries*>(opaque);NativeThaiWordCandidates result{};
        for(u32 i=0;i<q.count;++i){u32 key=0;copy_bytes(&key,q.data+i*36,4);
            if(key==position){copy_bytes(&result,q.data+i*36+4,32);return result;}}
        q.missing=true;return result;
    };
    for(u32 row=0;row<total;++row){
        CHECK(bytes-at>=112);u32 position=0,limit=0,count=0,queries=0;
        copy_bytes(&position,data+at,4);copy_bytes(&limit,data+at+4,4);copy_bytes(&count,data+at+8,4);
        NativeThaiWordCandidates retained{},expected{},expected_retained{},output{};
        copy_bytes(&retained,data+at+12,32);copy_bytes(&expected,data+at+44,32);copy_bytes(&expected_retained,data+at+76,32);
        copy_bytes(&queries,data+at+108,4);at+=112;
        CHECK(position<limit&&limit<=256&&count<=8&&queries<=256&&queries<=(bytes-at)/36);
        Queries source{data+at,queries,false};at+=queries*36;
        const u32 actual=native_thai_word_select(position,limit,&source,lookup,retained,output);
        if(actual!=count||!bytes_equal(&retained,&expected_retained,32)||!bytes_equal(&output,&expected,32)||source.missing){
            print("Thai selection mismatch row=");number(row+1);print(" position=");number(position);print("\n");
        }
        CHECK(!source.missing&&actual==count&&bytes_equal(&retained,&expected_retained,32)&&bytes_equal(&output,&expected,32));
    }
    CHECK(at==bytes);print("thai_selection_cases=");number(total);print(" state_bytes=");number(sizeof(NativeThaiWordCandidates));print("\n");
    return true;
}
