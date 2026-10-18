// Included inside font_store_test's namespace; fixtures remain QA-only.
#include "generated/gvar_outline_fixture.inc"
#include "maicord/core/win32_kernel.hpp"
NativeFontInstanceSlot instance_slots[4];NativeFontInstanceStore instance_store{&store,{instance_slots,4}};
u32 instance_fonts=0,instance_outlines=0,instance_budgets=0;bool trimmed_pages_checked=false;
AxisSegmentMap instance_maps[16];double instance_coordinates[16];i32 instance_stack[256];TtCallFrame instance_frames[32];
NativeFontPrepareScratch prepare_scratch{{instance_maps,16},{instance_coordinates,16},{instance_stack,256},{instance_frames,32}};
struct InstanceReader {
    ConstSlice<u8> bytes;u32 at=0;
    bool word(u32* v) noexcept {if(at>bytes.count||bytes.count-at<4)return false;*v=byte_load_le32(bytes.data+at);at+=4;return true;}
    bool take(u32 n,ConstSlice<u8>* v) noexcept {if(at>bytes.count||n>bytes.count-at)return false;*v={bytes.data+at,n};at+=n;return true;}
};
struct InstanceExpected {u32 glyph,count,fill,has_bounds,bounds[4];ConstSlice<u8> commands;};
bool instance_expected(InstanceReader& r,InstanceExpected& e) noexcept {
    CHECK(r.word(&e.glyph)&&r.word(&e.count)&&r.word(&e.fill)&&r.word(&e.has_bounds)&&e.count<=1024);
    for(auto& b:e.bounds)CHECK(r.word(&b));CHECK(r.take(e.count*28,&e.commands));return true;
}
bool instance_outline(NativeFontInstanceHandle h,const InstanceExpected& e) noexcept {
    const auto* view=native_font_instance_get(instance_store,h);CHECK(view);
    SfntOutlinePoint points[512];u32 ends[128];u16 active[64];GlyphPathCommand commands[1024];
    u32 shared_points[512],private_points[512];i32 dx[512],dy[512],storage[128],cvt[128];SfntGvarDelta deltas[1024];TtPoint twilight[64],vm_points[516];u32 vm_ends[128];
    TtOutlineContext context{};context.instance=view->instructions;context.scratch.stack={instance_stack,256};context.scratch.frames={instance_frames,32};context.scratch.storage={storage,128};context.scratch.cvt={cvt,128};context.scratch.twilight={twilight,64};context.scratch.glyph={vm_points,516};context.contour_ends={vm_ends,128};
    FontGeometryScratch scratch{{{points,512},{ends,128},{active,64},0,0,1000000},{{shared_points,512},{private_points,512},{dx,512},{dy,512}},{deltas,1024},view->instructions?&context:nullptr};
    GlyphOutline out{};out.commands=commands;out.capacity=1024;
    CHECK(font_geometry_outline(view->geometry,static_cast<u16>(e.glyph),&scratch,&out)==SfntStatus::ok);
    CHECK(out.count==e.count&&static_cast<u32>(out.fill_rule)==e.fill&&out.has_bounds==(e.has_bounds!=0)&&bytes_equal(commands,e.commands.data,e.commands.count));
    const float bounds[]{out.minimum_x,out.minimum_y,out.maximum_x,out.maximum_y};CHECK(bytes_equal(bounds,e.bounds,16));++instance_outlines;return true;
}
bool instance_readonly(NativeFontInstanceHandle h) noexcept {
    const auto& state=instance_slots[h.slot-1].state;MEMORY_BASIC_INFORMATION info{};
    CHECK(state.base&&VirtualQuery(state.base,&info,sizeof(info))==sizeof(info)&&info.Protect==PAGE_READONLY);return true;
}
bool coordinate_only_instances(ConstSlice<u8> original) noexcept {
    // Keep fvar and outlines, but remove outline variation from a first-party
    // font. COLR/GDEF/GSUB can consume coordinates without a gvar table.
    u8 bytes[16384];CHECK(original.count<=sizeof(bytes));copy_bytes(bytes,original.data,original.count);
    const u32 tables=byte_load_be16(bytes+4);bool removed=false;
    for(u32 i=0;i<tables;++i){auto* entry=bytes+12+i*16;if(byte_load_be32(entry)==sfnt_tag('g','v','a','r')){
        byte_store_be32(entry,sfnt_tag('g','v','a','s'));removed=true;}}
    CHECK(removed&&write_file(paths[1],{bytes,original.count}));NativeFontHandle face{};
    CHECK(native_font_acquire(&store,paths[1],0,&face).ok());const auto& geometry=native_font_get(store,face)->geometry;
    CHECK(geometry.axes.axis_count==1&&!geometry.gvar.table.data);FontAxis axis{};CHECK(font_axis(geometry.axes,0,&axis)==SfntStatus::ok);
    SfntOutlinePoint points[512];u32 ends[128];u16 active[64];GlyphPathCommand baseline_commands[1024],commands[1024];
    FontGeometryScratch scratch{{{points,512},{ends,128},{active,64},0,0,1000000},{},{}};
    GlyphOutline baseline{};baseline.commands=baseline_commands;baseline.capacity=1024;
    CHECK(font_geometry_outline({&geometry,nullptr},1,&scratch,&baseline)==SfntStatus::ok&&baseline.count);
    const double values[]{axis.minimum/65536.0,axis.normal/65536.0,axis.maximum/65536.0};
    NativeFontInstanceHandle handles[3]{};
    for(u32 i=0;i<3;++i){const FontAxisSetting setting{axis.tag,values[i]};u32 used=0;
        CHECK(native_font_instance_acquire(&instance_store,face,{&setting,1},false,prepare_scratch,handles+i,1000000,&used).ok());
        CHECK(instance_readonly(handles[i]));const auto* view=native_font_instance_get(instance_store,handles[i]);
        CHECK(view->geometry.variation&&view->geometry.variation->coordinates.count==1&&
            view->geometry.variation->coordinates[0]==static_cast<double>(static_cast<i32>(i)-1)&&
            !view->geometry.variation->font.table.data&&!view->geometry.variation->shared_scalars.count);
        GlyphOutline actual{};actual.commands=commands;actual.capacity=1024;
        CHECK(font_geometry_outline(view->geometry,1,&scratch,&actual)==SfntStatus::ok&&actual.count==baseline.count&&
            actual.has_bounds==baseline.has_bounds&&actual.minimum_x==baseline.minimum_x&&actual.minimum_y==baseline.minimum_y&&
            actual.maximum_x==baseline.maximum_x&&actual.maximum_y==baseline.maximum_y&&bytes_equal(commands,baseline_commands,actual.count*sizeof(GlyphPathCommand)));
        SfntGvarInstance coordinate_view{};FontGeometryInstance borrowed{};double coordinates[16];
        CHECK(font_geometry_instance(geometry,{&setting,1},{{instance_maps,16},{coordinates,16},{},&coordinate_view},&borrowed)==SfntStatus::ok&&
            borrowed.variation==&coordinate_view&&coordinates[0]==view->geometry.variation->coordinates[0]&&!coordinate_view.font.table.data);
        NativeFontInstanceHandle alias{};CHECK(native_font_instance_acquire(&instance_store,face,{&setting,1},false,prepare_scratch,&alias).ok()&&
            alias.slot==handles[i].slot&&alias.generation==handles[i].generation&&native_font_instance_release(&instance_store,alias));
        if(!i){CHECK(native_font_instance_release(&instance_store,handles[i]));const u32 work=used;
            for(u32 budget=0;budget<=work;++budget){NativeFontInstanceHandle bounded{};const auto result=native_font_instance_acquire(&instance_store,face,{&setting,1},false,prepare_scratch,&bounded,budget,&used);++instance_budgets;
                CHECK(used<=budget&&(budget<work?result.font_status==SfntStatus::resource_limit&&!bounded.slot:result.ok()));
                if(bounded.slot)CHECK(native_font_instance_release(&instance_store,bounded));}
            CHECK(native_font_instance_acquire(&instance_store,face,{&setting,1},false,prepare_scratch,handles+i).ok());}
    }
    CHECK(handles[0].slot!=handles[1].slot&&handles[1].slot!=handles[2].slot&&native_font_release(&store,face));
    for(auto handle:handles)CHECK(native_font_instance_release(&instance_store,handle));CHECK(stats(0,0,0));++instance_fonts;return true;
}
bool instance_cases() noexcept {
    InstanceReader r{{tt_outline_fixture,sizeof(tt_outline_fixture)},8};u32 fonts_count=0;CHECK(r.word(&fonts_count));
    for(u32 i=0;i<fonts_count;++i){u32 size=0,n=0;ConstSlice<u8> bytes{};CHECK(r.word(&size)&&r.word(&n)&&n==4&&r.take(size,&bytes));InstanceExpected expected[4]{};for(auto& e:expected)CHECK(instance_expected(r,e));
        CHECK(write_file(paths[0],bytes));NativeFontHandle face{};CHECK(native_font_acquire(&store,paths[0],0,&face).ok());NativeFontInstanceHandle h{},alias{},raw{};u32 used=0;
        CHECK(native_font_instance_acquire(&instance_store,face,{},true,prepare_scratch,&h,1000000,&used).ok());const u32 work=used;const usize committed=instance_slots[h.slot-1].state.committed;CHECK(instance_readonly(h));
        const auto* prepared=native_font_instance_get(instance_store,h)->instructions;
        if(prepared->graphics.instruction_control&1)CHECK(!prepared->storage.count&&!prepared->cvt.count&&!prepared->functions.count&&!prepared->twilight.count&&!prepared->stack_capacity&&committed==4096);
        if((prepared->graphics.instruction_control&1)&&!trimmed_pages_checked){
            // Expand unused storage in an already-disabled first-party font.
            // Its captured outlines stay valid; force multiple committed pages
            // so the test observes decommit, not just cleared array pointers.
            u8 changed[4096];CHECK(bytes.count<=sizeof(changed));copy_bytes(changed,bytes.data,bytes.count);SfntFace expanded{};ConstSlice<u8> maxp{};
            CHECK(sfnt_open({changed,bytes.count},0,&expanded)==SfntStatus::ok&&sfnt_table(expanded,sfnt_tag('m','a','x','p'),&maxp)==SfntStatus::ok&&maxp.count>=32);
            auto* profile=changed+(maxp.data-changed);profile[18]=8;profile[19]=0;CHECK(write_file(paths[1],{changed,bytes.count}));
            NativeFontHandle large_face{};NativeFontInstanceHandle large{};CHECK(native_font_acquire(&store,paths[1],0,&large_face).ok()&&native_font_instance_acquire(&instance_store,large_face,{},true,prepare_scratch,&large).ok());
            const auto& state=instance_slots[large.slot-1].state;MEMORY_BASIC_INFORMATION tail{};
            CHECK(state.reserved>state.committed&&state.committed==4096&&VirtualQuery(state.base+state.committed,&tail,sizeof(tail))==sizeof(tail)&&tail.State==MEM_RESERVE);
            for(const auto& e:expected)CHECK(instance_outline(large,e));
            CHECK(native_font_instance_release(&instance_store,large)&&native_font_release(&store,large_face));trimmed_pages_checked=true;++instance_fonts;
        }
        CHECK(native_font_instance_acquire(&instance_store,face,{},true,prepare_scratch,&alias).ok()&&h.slot==alias.slot&&h.generation==alias.generation&&native_font_stats(store).references==2);
        CHECK(native_font_instance_release(&instance_store,alias));
        if(!i){
            instance_store.slots.count=1;const auto full=native_font_instance_acquire(&instance_store,face,{},false,prepare_scratch,&raw);instance_store.slots.count=4;
            CHECK(full.error==NativeFontError::capacity&&!raw.slot&&native_font_stats(store).references==2);
            CHECK(native_font_instance_acquire(&instance_store,face,{},false,prepare_scratch,&raw).ok()&&raw.slot!=h.slot&&!instance_slots[raw.slot-1].state.base&&native_font_stats(store).references==3);
            CHECK(native_font_instance_release(&instance_store,raw));
            instance_slots[h.slot-1].references=0xffffffffu;const bool rejected=!native_font_instance_retain(&instance_store,h)&&native_font_instance_acquire(&instance_store,face,{},true,prepare_scratch,&alias).error==NativeFontError::capacity;instance_slots[h.slot-1].references=1;CHECK(rejected&&!alias.slot);
        }
        CHECK(native_font_instance_release(&instance_store,h));CHECK(!native_font_instance_get(instance_store,h)&&!native_font_instance_retain(&instance_store,h)&&!native_font_instance_release(&instance_store,h));
        CHECK(native_font_instance_acquire(&instance_store,face,{},true,prepare_scratch,&h,1000000,nullptr,committed-1).font_status==SfntStatus::resource_limit&&!h.slot&&native_font_stats(store).references==1);
        for(u32 budget=0;budget<=work;++budget){const auto status=native_font_instance_acquire(&instance_store,face,{},true,prepare_scratch,&h,budget,&used);++instance_budgets;
            CHECK(used<=budget&&(budget<work?status.error==NativeFontError::font&&status.font_status==SfntStatus::resource_limit&&!h.slot:status.ok()));
            if(h.slot)CHECK(native_font_instance_release(&instance_store,h));CHECK(native_font_stats(store).references==1);}
        CHECK(native_font_instance_acquire(&instance_store,face,{},true,prepare_scratch,&h,1000000,nullptr,0).font_status==SfntStatus::resource_limit&&!h.slot&&native_font_stats(store).references==1);
        auto short_scratch=prepare_scratch;short_scratch.stack.count=0;const auto short_result=native_font_instance_acquire(&instance_store,face,{},true,short_scratch,&h);
        const auto* source=native_font_get(store,face);TtOutlineRequirements needed{};CHECK(tt_outline_requirements(source->geometry.face,&needed)==SfntStatus::ok);
        if(needed.stack)CHECK(short_result.font_status==SfntStatus::output_too_small&&!h.slot);else {CHECK(short_result.ok());CHECK(native_font_instance_release(&instance_store,h));}
        CHECK(native_font_instance_acquire(&instance_store,face,{},true,prepare_scratch,&h).ok()&&native_font_release(&store,face)&&native_font_stats(store).references==1);
        for(u32 pass=0;pass<2;++pass)for(u32 g=0;g<4;++g)CHECK(instance_outline(h,expected[pass?g:3-g]));
        CHECK(native_font_instance_release(&instance_store,h)&&stats(0,0,0));++instance_fonts;
    }
    CHECK(r.at==r.bytes.count);r={{gvar_outline_fixture,sizeof(gvar_outline_fixture)},8};CHECK(r.word(&fonts_count));
    for(u32 i=0;i<fonts_count;++i){u32 size=0,n=0;ConstSlice<u8> bytes{};CHECK(r.word(&size)&&r.word(&n)&&n==2&&r.take(size,&bytes));
        if(!i)CHECK(coordinate_only_instances(bytes));
        CHECK(write_file(paths[0],bytes));NativeFontHandle face{};CHECK(native_font_acquire(&store,paths[0],0,&face).ok());NativeFontInstanceHandle h[2]{};InstanceExpected expected[2][4]{};
        for(u32 c=0;c<2;++c){u32 weight=0,glyphs=0;CHECK(r.word(&weight)&&r.word(&glyphs)&&glyphs==4);for(auto& e:expected[c])CHECK(instance_expected(r,e));
            const FontAxisSetting setting{sfnt_tag('w','g','h','t'),static_cast<double>(weight)};u32 used=0;
            CHECK(native_font_instance_acquire(&instance_store,face,{&setting,1},false,prepare_scratch,&h[c],1000000,&used).ok()&&instance_readonly(h[c]));
            const u32 work=used;CHECK(native_font_instance_release(&instance_store,h[c]));
            for(u32 budget=0;budget<=work;++budget){const auto status=native_font_instance_acquire(&instance_store,face,{&setting,1},false,prepare_scratch,&h[c],budget,&used);++instance_budgets;
                CHECK(used<=budget&&(budget<work?status.error==NativeFontError::font&&status.font_status==SfntStatus::resource_limit&&!h[c].slot:status.ok()));
                if(h[c].slot)CHECK(native_font_instance_release(&instance_store,h[c]));CHECK(native_font_stats(store).references==1+c);}
            CHECK(native_font_instance_acquire(&instance_store,face,{&setting,1},false,prepare_scratch,&h[c]).ok());
            const FontAxisSetting equivalent[]{{sfnt_tag('w','g','h','t'),0},{sfnt_tag('x','x','x','x'),42},setting};NativeFontInstanceHandle alias{};
            CHECK(native_font_instance_acquire(&instance_store,face,{equivalent,3},false,prepare_scratch,&alias).ok()&&alias.slot==h[c].slot&&alias.generation==h[c].generation&&native_font_instance_release(&instance_store,alias));
            const auto* view=native_font_instance_get(instance_store,h[c]);CHECK(view->geometry.variation&&view->geometry.variation->coordinates.data!=instance_coordinates);
            for(auto& value:instance_coordinates)value=999;for(auto& map:instance_maps)map={};
        }
        NativeFontInstanceHandle invalid{};CHECK(native_font_instance_acquire(&instance_store,face,{},true,prepare_scratch,&invalid).font_status==SfntStatus::unsupported&&!invalid.slot);
        CHECK(native_font_release(&store,face));for(u32 pass=0;pass<2;++pass)for(u32 c=0;c<2;++c)for(u32 g=0;g<4;++g)CHECK(instance_outline(h[c],expected[c][pass?g:3-g]));
        CHECK(native_font_instance_release(&instance_store,h[0])&&native_font_instance_release(&instance_store,h[1])&&stats(0,0,0));++instance_fonts;
    }
    CHECK(r.at==r.bytes.count&&trimmed_pages_checked);NativeFontHandle face{};CHECK(native_font_acquire(&store,paths[0],0,&face).ok());
    for(auto& p:instance_slots)p.generation=0xffffffffu;NativeFontInstanceHandle exhausted{};
    CHECK(native_font_instance_acquire(&instance_store,face,{},false,prepare_scratch,&exhausted).error==NativeFontError::capacity&&!exhausted.slot&&native_font_stats(store).references==1);
    CHECK(native_font_release(&store,face)&&stats(0,0,0));return true;
}
