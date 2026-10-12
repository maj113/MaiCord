SfntComponentGlyph sequence_glyphs[4096],sequence_inputs[64],sequence_outputs[64];SfntComponentNode sequence_nodes[4096],sequence_snapshot[4096];SfntComponentUndo sequence_journal[65536];
u32 sequence_order[4096],sequence_copy[4096];u8 sequence_seen[4096];
u64 sequence_queries=0,sequence_edits=0,sequence_operations=0,sequence_rollbacks=0;u32 sequence_max_height=0,sequence_max_journal=0;
SfntMarkAssociation sequence_associations[4096];u64 affinity_first_work=0,affinity_repeat_work=0;
bool audit_node(const SfntComponentForest& forest,u32 at,u32 expected_parent,u32 depth,u32* cursor,u32* height,u32* count) noexcept {
    *height=*count=0;if(at==0xffffffffu)return true;if(at>=forest.nodes.count || depth>64 || sequence_seen[at])return false;sequence_seen[at]=1;const auto& n=forest.nodes.data[at];
    if(n.parent!=expected_parent || (n.flags&1))return false;u32 lh=0,rh=0,lc=0,rc=0;
    if(!audit_node(forest,n.left,at,depth+1,cursor,&lh,&lc) || *cursor>=forest.nodes.count || n.source!=sequence_order[(*cursor)++])return false;
    if(!audit_node(forest,n.right,at,depth+1,cursor,&rh,&rc) || lh>rh+1 || rh>lh+1)return false;
    *height=1+maximum(lh,rh);*count=1+lc+rc;return n.height==*height && n.count==*count;
}
bool sequence_audit(const SfntComponentForest& forest,u32 root,u32 count) noexcept {
    clear_bytes(sequence_seen,sizeof(sequence_seen));u32 cursor=0,height=0,visited=0;if(!audit_node(forest,root,0xffffffffu,0,&cursor,&height,&visited) || cursor!=count || visited!=count)return false;
    sequence_max_height=maximum(sequence_max_height,height);
    for(u32 i=0;i<count;++i){u32 found=0,rank=0;if(sfnt_component_location(forest,sequence_order[i],&found,&rank)!=SfntStatus::ok || found!=root || rank!=i)return false;++sequence_queries;}
    return true;
}
SfntStatus sequence_edit(SfntComponentForest* forest,u32 count,u32 outputs,bool ligature,SfntComponentEdit* result,u64 work=1000000,u32 capacity=65536) noexcept {
    u32 indices[64];for(u32 i=0;i<count;++i)indices[i]=i;for(u32 i=0;i<outputs;++i)sequence_outputs[i]=sequence_inputs[0];
    const SfntGlyphView view{&sequence_inputs[0].glyph,nullptr,count,0,sizeof(SfntComponentGlyph)};
    return sfnt_components_edit(forest,view,{indices,count},ligature,{reinterpret_cast<u8*>(sequence_outputs),outputs*sizeof(SfntComponentGlyph)},{sequence_journal,capacity},result,work);
}
bool sequence_units() noexcept {
    constexpr u32 count=4096;for(u32 i=0;i<count;++i){sequence_glyphs[i]={{1,SfntGlyphClass::base,0},i,0,0};sequence_order[i]=i;}
    SfntComponentForest forest{};if(sfnt_components_open({sequence_glyphs,count},{sequence_nodes,count},&forest)!=SfntStatus::ok)return false;u32 root=0;
    for(u32 i=1;i<count;++i){sequence_inputs[0]=sequence_glyphs[0];sequence_inputs[0].component=root;sequence_inputs[1]=sequence_glyphs[i];SfntComponentEdit result{};
        if(sequence_edit(&forest,2,1,true,&result)!=SfntStatus::ok)return false;root=sequence_outputs[0].component;sequence_max_journal=maximum(sequence_max_journal,result.undo_count);sequence_operations+=result.operations;++sequence_edits;
        if((i&127)==127 && !sequence_audit(forest,root,i+1))return false;}
    for(u32 pass=0;pass<32;++pass){sequence_inputs[0]=sequence_glyphs[0];sequence_inputs[0].component=root;SfntComponentEdit result{};
        if(sequence_edit(&forest,1,64,false,&result)!=SfntStatus::ok)return false;sequence_max_journal=maximum(sequence_max_journal,result.undo_count);sequence_operations+=result.operations;++sequence_edits;
        for(u32 i=0;i<64;++i)sequence_inputs[i]=sequence_outputs[63-i];
        if(sequence_edit(&forest,64,1,true,&result)!=SfntStatus::ok)return false;root=sequence_outputs[0].component;sequence_max_journal=maximum(sequence_max_journal,result.undo_count);sequence_operations+=result.operations;++sequence_edits;
        copy_bytes(sequence_copy,sequence_order,sizeof(sequence_copy));for(u32 i=0;i<count-63;++i)sequence_order[i]=sequence_copy[i+63];for(u32 i=0;i<63;++i)sequence_order[count-63+i]=sequence_copy[62-i];
        if(!sequence_audit(forest,root,count))return false;
    }
    sequence_inputs[0]=sequence_glyphs[0];sequence_inputs[0].component=root;SfntComponentEdit result{};if(sequence_edit(&forest,1,2,false,&result)!=SfntStatus::ok)return false;
    sequence_inputs[0]=sequence_outputs[0];sequence_inputs[1]=sequence_outputs[1];copy_bytes(sequence_snapshot,sequence_nodes,sizeof(sequence_nodes));
    if(sequence_edit(&forest,2,1,true,&result)!=SfntStatus::ok)return false;const u64 complete_work=result.operations;const u32 complete_journal=result.undo_count;
    sfnt_components_rollback(&forest,{sequence_journal,result.undo_count});if(!bytes_equal(sequence_snapshot,sequence_nodes,sizeof(sequence_nodes)))return false;++sequence_rollbacks;
    for(u64 budget=0;budget<=complete_work;++budget){const auto status=sequence_edit(&forest,2,1,true,&result,budget);if(status!=(budget<complete_work?SfntStatus::resource_limit:SfntStatus::ok) || result.operations>budget)return false;
        if(status==SfntStatus::ok)sfnt_components_rollback(&forest,{sequence_journal,result.undo_count});if(!bytes_equal(sequence_snapshot,sequence_nodes,sizeof(sequence_nodes)))return false;++sequence_rollbacks;}
    for(u32 capacity=0;capacity<=complete_journal;++capacity){const auto status=sequence_edit(&forest,2,1,true,&result,1000000,capacity);if(status!=(capacity<complete_journal?SfntStatus::output_too_small:SfntStatus::ok))return false;
        if(status==SfntStatus::ok)sfnt_components_rollback(&forest,{sequence_journal,result.undo_count});if(!bytes_equal(sequence_snapshot,sequence_nodes,sizeof(sequence_nodes)))return false;++sequence_rollbacks;}
    // Guard exactly the required mutation journal, including its failing edge.
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;const usize bytes=static_cast<usize>(complete_journal)*sizeof(SfntComponentUndo);if(bytes>page*4)return false;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*5,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!memory)return false;DWORD old=0;if(!VirtualProtect(memory+page*4,page,PAGE_NOACCESS,&old)){VirtualFree(memory,0,MEM_RELEASE);return false;}
    auto* guarded=reinterpret_cast<SfntComponentUndo*>(memory+page*4-bytes);const u32 indices[]{0,1};SfntComponentGlyph output=sequence_inputs[0];const SfntGlyphView view{&sequence_inputs[0].glyph,nullptr,2,0,sizeof(SfntComponentGlyph)};
    bool ok=sfnt_components_edit(&forest,view,{indices,2},true,{reinterpret_cast<u8*>(&output),sizeof(output)},{guarded,complete_journal-1},&result)==SfntStatus::output_too_small && bytes_equal(sequence_snapshot,sequence_nodes,sizeof(sequence_nodes));
    ok=ok && sfnt_components_edit(&forest,view,{indices,2},true,{reinterpret_cast<u8*>(&output),sizeof(output)},{guarded,complete_journal},&result)==SfntStatus::ok;
    if(ok)sfnt_components_rollback(&forest,{guarded,result.undo_count});ok=ok && bytes_equal(sequence_snapshot,sequence_nodes,sizeof(sequence_nodes));VirtualFree(memory,0,MEM_RELEASE);return ok;
}
bool affinity_units() noexcept {
    constexpr u32 count=4096;for(u32 i=0;i<count;++i)sequence_glyphs[i]={{static_cast<u16>(i?3:1),i?SfntGlyphClass::mark:SfntGlyphClass::base,0},i,0,0};
    SfntComponentForest forest{};if(sfnt_components_open({sequence_glyphs,count},{sequence_nodes,count},&forest)!=SfntStatus::ok)return false;
    const SfntGlyphView view{&sequence_glyphs[0].glyph,nullptr,count,0,sizeof(SfntComponentGlyph)};u32 target=0,target_count=0;
    if(sfnt_components_export(&forest,view,{}, {sequence_associations,count},&target_count)!=SfntStatus::output_too_small || !forest.sealed)return false;
    if(sfnt_components_export(&forest,view,{&target,1},{sequence_associations,count},&target_count,1000000,&affinity_first_work)!=SfntStatus::ok || target_count!=1 || target || affinity_first_work>count*8)return false;
    for(u32 i=0;i<count;++i)if(sequence_associations[i].target!=(i?0:0xffffffffu) || sequence_associations[i].component!=sfnt_component_unknown)return false;
    if(sfnt_components_export(&forest,view,{&target,1},{sequence_associations,count},&target_count,1000000,&affinity_repeat_work)!=SfntStatus::ok || affinity_repeat_work>affinity_first_work)return false;
    for(u32 i=0;i<count;++i)if(sequence_associations[i].target!=(i?0:0xffffffffu) || sequence_associations[i].component!=sfnt_component_unknown)return false;
    copy_bytes(sequence_snapshot,sequence_nodes,sizeof(sequence_nodes));SfntComponentEdit result{};const u32 indices[]{0,1};sequence_outputs[0]=sequence_glyphs[0];
    return sfnt_components_edit(&forest,view,{indices,2},true,{reinterpret_cast<u8*>(sequence_outputs),sizeof(SfntComponentGlyph)},{sequence_journal,65536},&result)==SfntStatus::invalid_input && bytes_equal(sequence_snapshot,sequence_nodes,sizeof(sequence_nodes));
}
