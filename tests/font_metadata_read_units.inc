bool metadata_projection_cases() noexcept {
    Temporary temp;CHECK(temp.open());const ConstSlice<u8> font{font_family_fixture+24,byte_load_le32(font_family_fixture+12)};
    u8 bytes[16384],projected[16384];CHECK(font.count<=sizeof(bytes));copy_bytes(bytes,font.data,font.count);
    const auto compare=[&](u32 count,u32 face_index) noexcept -> bool {
        CHECK(write(temp.paths[0],{bytes,count}));HANDLE file=CreateFileW(temp.paths[0],GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);
        SfntFace reference{},actual{};const auto expected=sfnt_open({bytes,count},face_index,&reference);
        const auto result=font_metadata::read(file,count,face_index,{projected,sizeof(projected)},&actual);CloseHandle(file);
        CHECK(result.font_status==expected&&((expected==SfntStatus::ok)==result.ok()));
        if(result.ok()){
            CHECK(actual.face_index==reference.face_index&&actual.face_count==reference.face_count&&actual.version==reference.version);
            const u32 tags[]{sfnt_tag('O','S','/','2'),sfnt_tag('S','T','A','T'),sfnt_tag('f','v','a','r'),sfnt_tag('n','a','m','e')};
            for(u32 tag:tags){ConstSlice<u8> a{},b{};CHECK(sfnt_table(actual,tag,&a)==sfnt_table(reference,tag,&b));CHECK(a.count==b.count&&(!a.count||bytes_equal(a.data,b.data,a.count)));}
        }return true;
    };
    CHECK(compare(font.count,0)&&compare(font.count,1));
    // Truncation boundaries and damaged directories, including unneeded tables.
    const u32 sizes[]{0,1,11,12,27,font.count-1};for(u32 size:sizes)CHECK(compare(size,0));
    for(u32 at=0;at<minimum<u32>(font.count,140);++at){const u8 saved=bytes[at];bytes[at]^=0xff;CHECK(compare(font.count,0));bytes[at]=saved;}
    // Empty/missing metadata, zero-size tables, unsorted tags, and invalid spans.
    const u32 tags[]{sfnt_tag('O','S','/','2'),sfnt_tag('S','T','A','T'),sfnt_tag('f','v','a','r'),sfnt_tag('n','a','m','e')};
    for(u32 count=1;count<=4;++count){for(auto& byte:bytes)byte=0;byte_store_be32(bytes,0x10000);bytes[5]=static_cast<u8>(count);
        for(u32 i=0;i<count;++i){byte_store_be32(bytes+12+i*16,tags[i]);byte_store_be32(bytes+20+i*16,12+count*16);}CHECK(compare(12+count*16,0));}
    byte_store_be32(bytes+12,sfnt_tag('A','A','A','A'));CHECK(compare(76,0));
    byte_store_be32(bytes+20,77);CHECK(compare(76,0));byte_store_be32(bytes+20,76);byte_store_be32(bytes+28,sfnt_tag('A','A','A','A'));CHECK(compare(76,0));
    // TTC offsets are absolute, and v2 has mandatory trailing DSIG fields.
    for(auto& byte:bytes)byte=0;byte_store_be32(bytes,sfnt_tag('t','t','c','f'));byte_store_be32(bytes+4,0x10000);byte_store_be32(bytes+8,2);byte_store_be32(bytes+12,64);byte_store_be32(bytes+16,64);
    copy_bytes(bytes+64,font.data,font.count);for(u32 i=0;i<byte_load_be16(bytes+68);++i){auto* p=bytes+64+12+i*16+8;byte_store_be32(p,byte_load_be32(p)+64);}
    CHECK(compare(font.count+64,0)&&compare(font.count+64,1)&&compare(font.count+64,2));
    {
        HANDLE source=CreateFileW(temp.paths[0],GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        HANDLE locker=CreateFileW(temp.paths[0],GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        CHECK(source!=INVALID_HANDLE_VALUE&&locker!=INVALID_HANDLE_VALUE);
        font_metadata::Reader reader(source,font.count+64);SfntFace first{},second{};
        CHECK(reader.read(2,{projected,sizeof(projected)},&first).font_status==SfntStatus::face_out_of_range);
        CHECK(reader.read(0,{projected,sizeof(projected)},&first).ok());
        OVERLAPPED lock{};CHECK(LockFileEx(locker,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,64,0,&lock));
        CHECK(reader.read(1,{projected,sizeof(projected)},&second).ok()&&second.face_index==1&&second.face_count==2);
        font_metadata::Reader retry(source,font.count+64);
        CHECK(retry.read(0,{projected,sizeof(projected)},&first).native_error==ERROR_LOCK_VIOLATION);
        CHECK(UnlockFileEx(locker,0,64,0,&lock));
        CHECK(retry.read(0,{projected,sizeof(projected)},&first).ok());
        CHECK(first.file.count==second.file.count&&first.version==second.version);
        CloseHandle(locker);CloseHandle(source);
    }
    byte_store_be32(bytes+4,0x20000);CHECK(compare(font.count+64,1)&&compare(20,0)&&compare(31,0));byte_store_be32(bytes+8,0xffffffff);CHECK(compare(font.count+64,0));byte_store_be32(bytes+8,0);CHECK(compare(font.count+64,0));
    // Metadata may be physically unsorted, adjacent, overlapping, or separated
    // by small gaps. The projection must retain every original table byte and
    // keep the exact-capacity fallback valid without additional storage.
    for(u32 variant=0;variant<5;++variant){
        for(auto& byte:bytes)byte=0;byte_store_be32(bytes,0x10000);bytes[5]=4;
        const u32 starts[]{2120,2048,2100,2080};
        for(u32 i=0;i<4;++i){const u32 at=variant==1?2048+i*80:variant==2?2048+i*8:variant==3?2048+i*200:starts[i];
            const u32 size=variant==4?0u:16u;auto* record=bytes+12+i*16;
            byte_store_be32(record,tags[i]);byte_store_be32(record+8,at);byte_store_be32(record+12,size);}
        for(u32 i=2048;i<2800;++i)bytes[i]=static_cast<u8>(i*37);
        CHECK(compare(2800,0));CHECK(write(temp.paths[0],{bytes,2800}));
        HANDLE source=CreateFileW(temp.paths[0],GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);CHECK(source!=INVALID_HANDLE_VALUE);
        const u32 exact=64+(variant==4?0u:64u);Guard compact;CHECK(compact.open(exact));SfntFace a{};
        CHECK(font_metadata::read(source,2800,0,{compact.data,exact},&a).ok()&&compact.intact());
        SfntFace b{};CHECK(sfnt_open({bytes,2800},0,&b)==SfntStatus::ok);
        for(u32 tag:tags){ConstSlice<u8> x{},y{};CHECK(sfnt_table(a,tag,&x)==sfnt_table(b,tag,&y)&&x.count==y.count&&(!x.count||bytes_equal(x.data,y.data,x.count)));}
        CloseHandle(source);
    }
    // A locked gap must not turn two individually readable tables into failure.
    for(auto& byte:bytes)byte=0;byte_store_be32(bytes,0x10000);bytes[5]=2;
    for(u32 i=0;i<2;++i){auto* record=bytes+12+i*16;byte_store_be32(record,tags[i]);byte_store_be32(record+8,2048+i*32);byte_store_be32(record+12,16);}
    CHECK(write(temp.paths[0],{bytes,2112}));HANDLE locked=CreateFileW(temp.paths[0],GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    HANDLE reader=CreateFileW(temp.paths[0],GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);CHECK(locked!=INVALID_HANDLE_VALUE&&reader!=INVALID_HANDLE_VALUE);
    OVERLAPPED lock{};lock.Offset=2064;CHECK(LockFileEx(locked,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,16,0,&lock));
    SfntFace locked_projection{};CHECK(font_metadata::read(reader,2112,0,{projected,sizeof(projected)},&locked_projection).ok());
    ConstSlice<u8> locked_table{};CHECK(sfnt_table(locked_projection,tags[0],&locked_table)==SfntStatus::ok&&locked_table.count==16);
    CHECK(sfnt_table(locked_projection,tags[1],&locked_table)==SfntStatus::ok&&locked_table.count==16);
    CHECK(UnlockFileEx(locked,0,16,0,&lock));CloseHandle(reader);CloseHandle(locked);
    // Exact writable extent, and catalog fallback with intentionally tiny scratch.
    CHECK(write(temp.paths[0],font));HANDLE file=CreateFileW(temp.paths[0],GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);
    SfntFace image{};const auto read=font_metadata::read(file,font.count,0,{projected,sizeof(projected)},&image);CHECK(read.ok());Guard guarded;CHECK(guarded.open(image.file.count));
    CHECK(font_metadata::read(file,font.count,0,{guarded.data,image.file.count},&image).ok()&&guarded.intact());
    CHECK(font_metadata::read(file,font.count,0,{projected,1},&image).font_status==SfntStatus::output_too_small);CloseHandle(file);
    Storage mapped,compact,fallback;u16 name[128];u32 a=0,b=0,c=0;u8 small[1];
    CHECK(native_font_catalog_add(&mapped.catalog,temp.paths[0],{name,128},&a).ok());
    CHECK(native_font_catalog_add(&compact.catalog,temp.paths[0],{name,128},&b,65536,1000000,nullptr,false,{projected,sizeof(projected)}).ok());
    CHECK(native_font_catalog_add(&fallback.catalog,temp.paths[0],{name,128},&c,65536,1000000,nullptr,false,{small,1}).ok());
    CHECK(a==b&&a==c&&bytes_equal(&mapped.catalog.count,&compact.catalog.count,sizeof(NativeFontCatalogCounts))&&bytes_equal(&mapped.catalog.count,&fallback.catalog.count,sizeof(NativeFontCatalogCounts)));
    CHECK(equal(native_font_catalog_name(mapped.catalog,0),native_font_catalog_name(compact.catalog,0))&&equal(native_font_catalog_name(mapped.catalog,0),native_font_catalog_name(fallback.catalog,0)));
    // Borrowing the scratch tail must not lower the accepted family-name limit.
    // This leaves only one name unit on the fast attempt, forcing the full-size
    // mapped retry to produce exactly the same catalog entry.
    static u16 shared_scratch[8193];NativeFontCatalogBuilder builder{};
    struct ReleaseBuilder {NativeFontCatalogBuilder& b;~ReleaseBuilder() noexcept {native_font_catalog_builder_release(&b);}} release_builder{builder};
    CHECK(native_font_catalog_builder_initialize(&builder));
    CHECK(native_font_catalog_builder_add(&builder,temp.paths[0],{shared_scratch,8193},&a).ok());
    CHECK(equal(native_font_catalog_name(mapped.catalog,0),native_font_catalog_name(builder.catalog,0)));
    return true;
}
