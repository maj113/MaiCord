bool position_flag_units() noexcept {
    const u32 boundaries[]{0,2};u64 bits[1];SfntClusterMap clusters{};if(sfnt_cluster_map_open(4,{boundaries,2},{bits,1},&clusters)!=SfntStatus::ok)return false;
    SfntComponentGlyph records[5]{{{1,SfntGlyphClass::base,0},0,0,0},{{2,SfntGlyphClass::base,0},2,0,0},{{3,SfntGlyphClass::base,0},2,0,0},{{4,SfntGlyphClass::mark,0},1,0,0},{{5,SfntGlyphClass::mark,0},3,0,0}};
    const u8 wanted[]{2,2,0,3,3},policy[]{1,0,0,0,0},overridden[]{3,2,0,2,2};u8 flags[5];
    for(u32 split=0;split<=5;++split){SfntComponentGlyph cells[9];for(auto& cell:cells)cell={{65535,static_cast<SfntGlyphClass>(255),255},0xffffffffu,0xffffffffu,0xffffffffu};
        for(u32 i=0;i<5;++i)cells[i<split?i:i+3]=records[i];const SfntGlyphView view{&cells[0].glyph,&cells[split+3].glyph,split,5-split,sizeof(SfntComponentGlyph)};
        if(sfnt_position_flags_from_clusters(clusters,view,{flags,5})!=SfntStatus::ok || !bytes_equal(flags,wanted,5))return false;
        if(sfnt_position_flags_from_clusters(clusters,view,{flags,5},{policy,5})!=SfntStatus::ok || !bytes_equal(flags,overridden,5))return false;
        copy_bytes(flags,policy,5);if(sfnt_position_flags_from_clusters(clusters,view,{flags,5},{flags,5})!=SfntStatus::ok || !bytes_equal(flags,overridden,5))return false;
        for(u8& flag:flags)flag=0xcc;
        if(sfnt_position_flags_from_clusters(clusters,view,{flags,5},{},29)!=SfntStatus::resource_limit)return false;for(u8 flag:flags)if(flag!=0xcc)return false;
        if(sfnt_position_flags_from_clusters(clusters,view,{flags,5},{},30)!=SfntStatus::ok || !bytes_equal(flags,wanted,5))return false;
    }
    const SfntGlyphView view{&records[0].glyph,nullptr,5,0,sizeof(SfntComponentGlyph)};u8 invalid[]{0,0,0,0,2};
    u8 overlap[6]{};for(u32 direction=0;direction<2;++direction){if(sfnt_position_flags_from_clusters(clusters,view,{overlap+direction,5},{overlap+1-direction,5})!=SfntStatus::invalid_input)return false;for(u8 value:overlap)if(value)return false;}
    if(sfnt_position_flags_from_clusters(clusters,view,{flags,4})!=SfntStatus::output_too_small || sfnt_position_flags_from_clusters(clusters,view,{flags,5},{policy,4})!=SfntStatus::output_too_small || sfnt_position_flags_from_clusters(clusters,view,{flags,5},{invalid,5})!=SfntStatus::invalid_input)return false;
    records[4].source=4;if(sfnt_position_flags_from_clusters(clusters,view,{flags,5})!=SfntStatus::invalid_input)return false;records[4].source=3;
    records[4].glyph.glyph_class=static_cast<SfntGlyphClass>(5);if(sfnt_position_flags_from_clusters(clusters,view,{flags,5})!=SfntStatus::invalid_input)return false;records[4].glyph.glyph_class=SfntGlyphClass::mark;
    SfntClusterMap empty{};if(sfnt_cluster_map_open(0,{}, {},&empty)!=SfntStatus::ok || sfnt_position_flags_from_clusters(empty,{nullptr,nullptr,0,0,sizeof(SfntComponentGlyph)},{})!=SfntStatus::ok)return false;
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*6,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!memory)return false;DWORD old=0;
    for(u32 i=1;i<6;i+=2)if(!VirtualProtect(memory+page*i,page,PAGE_NOACCESS,&old)){VirtualFree(memory,0,MEM_RELEASE);return false;}
    auto* guarded_records=reinterpret_cast<SfntComponentGlyph*>(memory+page-sizeof(records));auto* guarded_policy=memory+page*3-5;auto* guarded_flags=memory+page*5-5;
    copy_bytes(guarded_records,records,sizeof(records));copy_bytes(guarded_policy,policy,5);const SfntGlyphView guarded{&guarded_records[0].glyph,nullptr,5,0,sizeof(SfntComponentGlyph)};
    const bool ok=sfnt_position_flags_from_clusters(clusters,guarded,{guarded_flags,5})==SfntStatus::ok && bytes_equal(guarded_flags,wanted,5) && sfnt_position_flags_from_clusters(clusters,guarded,{guarded_flags,5},{guarded_policy,5})==SfntStatus::ok && bytes_equal(guarded_flags,overridden,5);
    VirtualFree(memory,0,MEM_RELEASE);return ok;
}
