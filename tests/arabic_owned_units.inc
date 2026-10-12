NativeFontFileSlot owned_files[2];NativeFontFaceSlot owned_faces[2];NativeFontInstanceSlot owned_instances[2];NativeRunPlanSlot owned_runs[2];NativeArabicPlanSlot owned_plans[2];
NativeFontStore owned_fonts{{owned_files,2},{owned_faces,2}};
NativeFontInstanceStore owned_instance_store{&owned_fonts,{owned_instances,2}};
NativeRunPlanStore owned_run_store{&owned_instance_store,{owned_runs,2}};
NativeArabicPlanStore owned_store{&owned_run_store,{owned_plans,2}};
NativeArabicPlanHandle owned_handle{};u32 owned_index=0xffffffffu;Arena owned_scratch{};wchar_t owned_path[MAX_PATH]{};
u64 owned_preparations=0,owned_hits=0,owned_committed=0,owned_used=0;
u8 owned_gate_bytes[65536];
bool owned_close() noexcept {
    if(owned_handle.slot){const auto old=owned_handle;if(!native_arabic_plan_release(&owned_store,old) || native_arabic_plan_get(owned_store,old).shape || native_arabic_plan_retain(&owned_store,old) || native_arabic_plan_release(&owned_store,old))return false;owned_handle={};}
    owned_index=0xffffffffu;return !native_font_stats(owned_fonts).files;
}
bool owned_start() noexcept {
    wchar_t directory[MAX_PATH];const DWORD count=GetTempPathW(MAX_PATH,directory);
    return count && count<MAX_PATH && GetTempFileNameW(directory,L"mca",0,owned_path) && arena_initialize(&owned_scratch,4*1024*1024,0,4096);
}
bool owned_gate_checks(ConstSlice<u8> original) noexcept {
    if(!owned_close() || original.count>sizeof(owned_gate_bytes))return false;
    for(u32 kind=0;kind<2;++kind){copy_bytes(owned_gate_bytes,original.data,original.count);SfntFace face{};ConstSlice<u8> bytes{};
        if(sfnt_open({owned_gate_bytes,original.count},0,&face)!=SfntStatus::ok || sfnt_table(face,sfnt_tag('G','S','U','B'),&bytes)!=SfntStatus::ok)return false;
        if(kind){SfntLayout layout{};if(sfnt_layout_open(bytes,SfntLayoutKind::substitution,&layout)!=SfntStatus::ok)return false;
            byte_store_be32(owned_gate_bytes+(layout.scripts.data-owned_gate_bytes)+2,sfnt_tag('c','y','r','l'));
        }else{bool changed=false;for(u32 at=0;at<face.directory.count;at+=16)if(byte_load_be32(face.directory.data+at)==sfnt_tag('G','S','U','B')){byte_store_be32(owned_gate_bytes+(face.directory.data-owned_gate_bytes)+at,sfnt_tag('G','S','U','X'));changed=true;break;}if(!changed)return false;}
        HANDLE file=CreateFileW(owned_path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);if(file==INVALID_HANDLE_VALUE)return false;DWORD written=0;
        const bool saved=WriteFile(file,owned_gate_bytes,original.count,&written,nullptr)&&written==original.count;CloseHandle(file);if(!saved)return false;
        NativeFontHandle font{};NativeFontInstanceHandle instance{};NativeRunPlanHandle run{};NativeArabicPlanHandle result{};AxisSegmentMap maps[16];double coordinates[16];NativeFontPrepareScratch prep{{maps,16},{coordinates,16},{},{}};
        if(!native_font_acquire(&owned_fonts,owned_path,0,&font).ok() || !native_font_instance_acquire(&owned_instance_store,font,{},false,prep,&instance).ok() || !native_run_plan_acquire(&owned_run_store,instance,&owned_scratch,&run).ok())return false;
        const bool rejected=native_arabic_plan_acquire(&owned_store,run,&owned_scratch,&result).font_status==SfntStatus::unsupported_sequence && !result.slot && owned_runs[run.slot-1].references==1;
        if(!native_run_plan_release(&owned_run_store,run) || !native_font_instance_release(&owned_instance_store,instance) || !native_font_release(&owned_fonts,font) || native_font_stats(owned_fonts).files || !rejected)return false;
    }
    return true;
}
bool owned_select(ConstSlice<u8> bytes,u32 index,bool independent,NativeArabicPlanView* view) noexcept {
    if(index!=owned_index){if(!owned_close())return false;
        HANDLE file=CreateFileW(owned_path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);if(file==INVALID_HANDLE_VALUE)return false;DWORD written=0;
        const bool saved=WriteFile(file,bytes.data,bytes.count,&written,nullptr)&&written==bytes.count;CloseHandle(file);if(!saved)return false;
        NativeFontHandle font{};NativeFontInstanceHandle instance{};NativeRunPlanHandle run{};
        AxisSegmentMap maps[16];double coordinates[16];NativeFontPrepareScratch prep{{maps,16},{coordinates,16},{},{}};
        if(!native_font_acquire(&owned_fonts,owned_path,0,&font).ok() || !native_font_instance_acquire(&owned_instance_store,font,{},false,prep,&instance).ok() || !native_run_plan_acquire(&owned_run_store,instance,&owned_scratch,&run).ok())return false;
        const u32 references=owned_runs[run.slot-1].references;NativeArabicPlanHandle invalid{};
        if(native_arabic_plan_acquire(&owned_store,run,&owned_scratch,&invalid,0).font_status!=SfntStatus::resource_limit || invalid.slot || owned_runs[run.slot-1].references!=references)return false;
        if(native_arabic_plan_acquire(&owned_store,run,&owned_scratch,&invalid,2).font_status!=SfntStatus::resource_limit || invalid.slot || owned_runs[run.slot-1].references!=references)return false;
        if(native_arabic_plan_acquire(&owned_store,run,&owned_scratch,&invalid,1000000,0).font_status!=SfntStatus::resource_limit || invalid.slot || owned_runs[run.slot-1].references!=references)return false;
        if(!native_arabic_plan_acquire(&owned_store,run,&owned_scratch,&owned_handle).ok())return false;
        const auto& state=owned_plans[owned_handle.slot-1].state;MEMORY_BASIC_INFORMATION info{};
        if(VirtualQuery(state.base,&info,sizeof(info))!=sizeof(info) || info.Protect!=PAGE_READONLY)return false;
        const auto normal=native_arabic_plan_get(owned_store,owned_handle),other=native_arabic_plan_get(owned_store,owned_handle,true);const auto* base=native_run_plan_get(owned_run_store,run);
        if(!normal.shape || normal.shape!=other.shape || normal.shape->substitutions!=base->substitutions || normal.shape->metric_context!=base->metric_context || other.shape->metric_context!=base->metric_context || normal.shape->positioning_roots.data!=other.shape->positioning_roots.data)return false;
        bool inherited=base->positioning!=nullptr;
        for(u32 i=0;i<normal.shape->positioning_roots.count;++i){const u16 root=normal.shape->positioning_roots[i];
            if(!normal.shape->positioning||root>=normal.shape->positioning->lookups.count||!sfnt_gpos_lookup(*normal.shape->positioning,root)||!sfnt_gpos_lookup(*normal.shape->positioning,root)->type)return false;
            inherited=inherited&&root<base->positioning->lookups.count&&sfnt_gpos_lookup(*base->positioning,root)&&sfnt_gpos_lookup(*base->positioning,root)->type!=0;}
        if(inherited&&normal.shape->positioning!=base->positioning)return false;
        NativeArabicPlanHandle alias{};if(!native_arabic_plan_acquire(&owned_store,run,&owned_scratch,&alias,1000000,0).ok() || alias.slot!=owned_handle.slot || alias.generation!=owned_handle.generation || !native_arabic_plan_release(&owned_store,alias))return false;
        auto& live=owned_plans[owned_handle.slot-1];const u32 saved_references=live.references;live.references=0xffffffffu;
        const bool overflow=native_arabic_plan_acquire(&owned_store,run,&owned_scratch,&alias).error==NativeFontError::capacity && !alias.slot && !native_arabic_plan_retain(&owned_store,owned_handle);live.references=saved_references;if(!overflow)return false;
        if(!native_run_plan_release(&owned_run_store,run) || !native_font_instance_release(&owned_instance_store,instance) || !native_font_release(&owned_fonts,font) || owned_runs[run.slot-1].references!=1)return false;
        ++owned_preparations;owned_committed+=state.committed;owned_used+=state.offset;owned_index=index;
    }else ++owned_hits;
    *view=native_arabic_plan_get(owned_store,owned_handle,independent);return view->shape!=nullptr;
}
