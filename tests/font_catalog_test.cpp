// Native font catalog (MaiCordFontCatalogTests). No argument runs the suite. Manual modes:
// --file/--default <font> <query> (catalog dump), --open-proof-check <path> <0|1> (catalog
// freshness QA), --handle-stat-check (the default run already includes it) and
// --handle-stat-fallback-check, which asserts the fallback after a debugger (CDB) forces the
// handle query to fail and therefore fails without that injection.
#include "maicord/text/font_catalog_prepare.hpp"
#include "maicord/text/font_catalog_builder.hpp"
#include "maicord/core/byte_order.hpp"
#include "../src/text/font_metadata_read.hpp"
#include "../src/text/font_path_stat.hpp"
#include "../src/text/font_catalog_append.hpp"
namespace maicord {namespace {
#include "generated/font_family_fixture.inc"
u32 checks=0,output_count=0;bool print_ok=true;char print_buffer[65536];
void flush() noexcept {DWORD n=0;if(output_count&&(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),print_buffer,output_count,&n,nullptr)||n!=output_count))print_ok=false;output_count=0;}
void ch(char c) noexcept {if(output_count==sizeof(print_buffer))flush();print_buffer[output_count++]=c;}
void print(const char* s) noexcept {while(*s)ch(*s++);}
void number(u32 v) noexcept {char d[16];u32 n=0;do{d[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);while(n)ch(d[--n]);}
void string(ConstSlice<u16> s) noexcept {const char hex[]="0123456789abcdef";ch('"');for(u32 i=0;i<s.count;++i){print("\\u");for(int shift=12;shift>=0;shift-=4)ch(hex[(s[i]>>shift)&15]);}ch('"');}
void properties(NativeFontProperties p) noexcept {ch('[');number(p.weight);ch(',');number(p.width);ch(',');number(p.style);ch(']');}
#define CHECK(x) do{++checks;if(!(x)){print("FAIL line=");number(__LINE__);print("\n");return false;}}while(0)
u32 length(const wchar_t* p) noexcept {u32 n=0;while(p[n])++n;return n;}
ConstSlice<u16> wide(const wchar_t* p) noexcept {return {reinterpret_cast<const u16*>(p),length(p)};}
bool equal(ConstSlice<u16> a,ConstSlice<u16> b) noexcept {return a.count==b.count&&bytes_equal(a.data,b.data,a.count*2);}
struct Storage {
    NativeFontCatalogFile files[4]{};NativeFontCatalogFace faces[8]{};NativeFontCatalogCandidate candidates[16]{};FontAxisSetting settings[32]{};u16 strings[2048]{};
    NativeFontCatalog catalog{{files,4},{faces,8},{candidates,16},{settings,32},{strings,2048},{}};
};
struct Temporary {
    wchar_t reservation[MAX_PATH]{},directory[MAX_PATH]{},paths[4][MAX_PATH]{};bool reserved=false,created=false;
    bool open() noexcept {wchar_t temp[MAX_PATH];const u32 n=GetTempPathW(MAX_PATH,temp);if(!n||n>=MAX_PATH)return false;
        reserved=GetTempFileNameW(temp,L"mcc",0,reservation)!=0;if(!reserved)return false;const u32 size=length(reservation);if(size+5>=MAX_PATH)return false;
        copy_bytes(directory,reservation,size*2);copy_bytes(directory+size,L".dir",10);created=CreateDirectoryW(directory,nullptr)!=0;if(!created)return false;
        for(auto& path:paths)if(!GetTempFileNameW(directory,L"c",0,path))return false;return true;}
    ~Temporary() noexcept {for(auto& p:paths)if(*p)DeleteFileW(p);if(created)RemoveDirectoryW(directory);if(reserved)DeleteFileW(reservation);}
};
bool write(const wchar_t* path,ConstSlice<u8> bytes) noexcept {HANDLE h=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);if(h==INVALID_HANDLE_VALUE)return false;DWORD n=0;const bool ok=WriteFile(h,bytes.data,bytes.count,&n,nullptr)&&n==bytes.count;CloseHandle(h);return ok;}
struct Guard {
    u8* memory=nullptr;u8* data=nullptr;u32 page=0;
    bool open(u32 bytes) noexcept {SYSTEM_INFO info{};GetSystemInfo(&info);page=info.dwPageSize;if(!bytes||bytes>=page)return false;memory=static_cast<u8*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!memory)return false;data=memory+page-bytes;data[-1]=0xa5;DWORD old=0;return VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)!=0;}
    bool readonly() noexcept {DWORD old=0;return VirtualProtect(memory,page,PAGE_READONLY,&old)!=0;}
    bool intact() const noexcept {return data[-1]==0xa5;}
    ~Guard() noexcept {if(memory)VirtualFree(memory,0,MEM_RELEASE);}
};
bool guards(const wchar_t* path) noexcept {
    Guard file,face,candidate,strings,scratch,props,ids;const u32 string_units=length(path)+1+6;
    CHECK(file.open(sizeof(NativeFontCatalogFile))&&face.open(sizeof(NativeFontCatalogFace))&&candidate.open(sizeof(NativeFontCatalogCandidate))&&strings.open(string_units*2)&&scratch.open(128)&&props.open(4)&&ids.open(4));
    NativeFontCatalog c{{reinterpret_cast<NativeFontCatalogFile*>(file.data),1},{reinterpret_cast<NativeFontCatalogFace*>(face.data),1},{reinterpret_cast<NativeFontCatalogCandidate*>(candidate.data),1},{},{reinterpret_cast<u16*>(strings.data),string_units},{}};
    u32 index=0;CHECK(native_font_catalog_add(&c,path,{reinterpret_cast<u16*>(scratch.data),64},&index).ok()&&c.count.strings==string_units);
    CHECK(file.intact()&&face.intact()&&candidate.intact()&&strings.intact()&&scratch.intact());
    CHECK(file.readonly()&&face.readonly()&&candidate.readonly()&&strings.readonly());
    NativeFontCatalogMatch match{};CHECK(native_font_catalog_match(c,wide(L"probe"),{600,5,2},{reinterpret_cast<NativeFontProperties*>(props.data),1},{reinterpret_cast<u32*>(ids.data),1},&match)==SfntStatus::ok&&match.candidate==0&&match.simulations==2);
    CHECK(props.intact()&&ids.intact());CHECK(native_font_catalog_add(&c,path,{reinterpret_cast<u16*>(scratch.data),64},&index).ok()&&index==0);
    return true;
}
#include "font_metadata_read_units.inc"
#include "font_catalog_growth_units.inc"
#include "font_path_stat_units.inc"
bool builder_cases() noexcept {
    Temporary temp;CHECK(temp.open());const ConstSlice<u8> font{font_family_fixture+24,byte_load_le32(font_family_fixture+12)};CHECK(write(temp.paths[0],font)&&write(temp.paths[1],font));
    NativeFontCatalogBuilder builder{};Arena output{};struct Release {NativeFontCatalogBuilder& b;Arena& a;~Release() noexcept {native_font_catalog_builder_release(&b);arena_release(&a);}} release{builder,output};
    CHECK(!native_font_catalog_builder_initialize(nullptr));CHECK(!native_font_catalog_builder_initialize(&builder,{}));
    CHECK(!native_font_catalog_builder_initialize(&builder,{0xffffffffu,1,1,0,128})&&!builder.files.base);
    CHECK(native_font_catalog_builder_initialize(&builder,{2,2,2,0,2048}));CHECK(!native_font_catalog_builder_initialize(&builder));
    CHECK(!builder.files.committed&&!builder.faces.committed&&!builder.candidates.committed&&!builder.strings.committed&&!builder.settings.base);
    const Arena* pools[]{&builder.files,&builder.faces,&builder.candidates,&builder.strings};
    const auto* end=builder.files.base;
    for(const auto* pool:pools){MEMORY_BASIC_INFORMATION memory{};
        CHECK(pool->base==end&&VirtualQuery(pool->base,&memory,sizeof(memory))==sizeof(memory)&&
            memory.AllocationBase==builder.files.base&&memory.State==MEM_RESERVE);
        end=pool->base+pool->reserved;}
    u16 name[128];u32 index=0;CHECK(native_font_catalog_builder_add(&builder,temp.paths[0],{name,128},&index).ok()&&index==0);
    auto* saved=builder.catalog.files.data;const auto counts=builder.catalog.count;CHECK(counts.files==1&&counts.faces==1&&counts.candidates==1&&!counts.settings);
    CHECK(native_font_catalog_builder_add(&builder,temp.paths[1],{name,128},&index).ok()&&index==1&&builder.catalog.files.data==saved&&builder.catalog.count.files==2);
    CHECK(write(temp.paths[2],font));const auto full=builder.catalog.count;CHECK(native_font_catalog_builder_add(&builder,temp.paths[2],{name,128},&index).error==NativeFontError::capacity&&index==0xffffffffu&&bytes_equal(&full,&builder.catalog.count,sizeof(full)));
    CHECK(native_font_catalog_builder_add(&builder,temp.paths[0],{name,128},&index).ok()&&index==0);
    CHECK(arena_initialize(&output,65536,0,4096));auto* prefix=arena_allocate<u32>(&output);CHECK(prefix);*prefix=0x12345678;const auto mark=arena_mark(&output);NativeFontCatalog packed{};
    // Exhausted destination preserves the allocation offset and existing prefix.
    const auto saved_offset=output.offset;output.offset=output.reserved;CHECK(!native_font_catalog_publish(builder.catalog,&output,&packed).ok()&&!packed.count.files&&output.offset==output.reserved&&*prefix==0x12345678);output.offset=saved_offset;
    CHECK(native_font_catalog_publish(builder.catalog,&output,&packed).ok()&&packed.count.files==2&&*prefix==0x12345678);
    CHECK(packed.files.data!=builder.catalog.files.data&&packed.faces.data!=builder.catalog.faces.data&&packed.strings.data!=builder.catalog.strings.data);
    CHECK(packed.files.count==packed.count.files&&packed.faces.count==packed.count.faces&&packed.candidates.count==packed.count.candidates&&packed.strings.count==packed.count.strings);
    NativeFontCatalog second{};const auto* first_files=packed.files.data;CHECK(native_font_catalog_publish(builder.catalog,&output,&second).ok()&&second.files.data!=first_files&&packed.files.data==first_files&&equal(native_font_catalog_name(second,0),native_font_catalog_name(packed,0)));
    const auto* reservation=builder.files.base;
    native_font_catalog_builder_release(&builder);CHECK(!builder.files.base&&!builder.catalog.count.files);
    MEMORY_BASIC_INFORMATION released{};CHECK(VirtualQuery(reservation,&released,sizeof(released))==sizeof(released)&&released.State==MEM_FREE);
    DWORD old=0;CHECK(VirtualProtect(output.base,output.committed,PAGE_READONLY,&old));NativeFontProperties props[2];u32 ids[2];NativeFontCatalogMatch matched{};
    CHECK(native_font_catalog_match(packed,wide(L"probe"),{400,5,0},{props,2},{ids,2},&matched)==SfntStatus::ok&&matched.candidate==0&&matched.properties.weight==700);
    CHECK(equal(native_font_catalog_path(packed,1),wide(temp.paths[1]))&&equal(native_font_catalog_name(packed,1),wide(L"Probe")));
    CHECK(VirtualProtect(output.base,output.committed,PAGE_READWRITE,&old)&&arena_rewind(&output,mark));
    CHECK(native_font_catalog_publish({},&output,&packed).ok()&&!packed.count.files&&output.offset==mark.offset);
    CHECK(native_font_catalog_builder_add(&builder,temp.paths[0],{name,128},&index).error==NativeFontError::invalid_input);
    return true;
}
bool preparation() noexcept {
    const ConstSlice<u8> font{font_family_fixture+24,byte_load_le32(font_family_fixture+12)};Temporary temp;CHECK(temp.open());
    u8 collection[16384]{};byte_store_be32(collection,sfnt_tag('t','t','c','f'));byte_store_be32(collection+4,0x10000);byte_store_be32(collection+8,10);u32 size=52;
    for(u32 i=0;i<10;++i){const u32 offset=size;CHECK(offset+font.count+4<sizeof(collection));byte_store_be32(collection+12+i*4,offset);copy_bytes(collection+offset,font.data,font.count);
        for(u32 j=0;j<byte_load_be16(font.data+4);++j){auto* field=collection+offset+12+j*16+8;byte_store_be32(field,byte_load_be32(field)+offset);}size=(offset+font.count+3)&~3u;}
    CHECK(write(temp.paths[0],{collection,size}));NativeFontCatalogFile files[1]{};NativeFontCatalogFace faces[10]{};NativeFontCatalogCandidate candidates[10]{};u16 strings[1024],name[128];
    Storage private_storage;u32 private_index=0;CHECK(native_font_catalog_add(&private_storage.catalog,temp.paths[0],{name,128},&private_index,1,1000000,nullptr,true).ok()&&private_storage.catalog.count.faces==1&&private_storage.catalog.count.candidates==1&&private_storage.catalog.faces[0].index==0);
    NativeFontCatalog c{{files,1},{faces,10},{candidates,10},{},{strings,1024},{}};u32 index=0;CHECK(native_font_catalog_add(&c,temp.paths[0],{name,128},&index).ok());
    NativeFontSession* session=nullptr;Arena scratch{};NativeFontInstanceHandle handles[10]{},alias{};
    struct Release {NativeFontSession*& session;Arena& scratch;NativeFontInstanceHandle* handles;NativeFontInstanceHandle& alias;~Release() noexcept {if(session){auto* store=native_font_session_instances(session);if(alias.slot)native_font_instance_release(store,alias);for(u32 i=0;i<10;++i)if(handles[i].slot)native_font_instance_release(store,handles[i]);native_font_session_release(&session);}arena_release(&scratch);}} release{session,scratch,handles,alias};
    CHECK(native_font_session_create(&session)&&arena_initialize(&scratch,1024*1024,0,4096));const auto empty=native_font_session_stats(session);bool oblique=true,bold=true;
    CHECK(native_font_catalog_prepare(session,c,0,4,true,&scratch,&alias,&oblique,&bold).font_status==SfntStatus::unsupported&&!alias.slot&&!oblique&&!bold);
    CHECK(native_font_catalog_prepare(session,c,0,1,true,&scratch,&alias).font_status==SfntStatus::unsupported);
    CHECK(native_font_catalog_prepare(session,c,0,2,true,&scratch,&alias).font_status==SfntStatus::unsupported);
    CHECK(native_font_catalog_prepare(session,c,99,0,true,&scratch,&alias).error==NativeFontError::invalid_input);
    CHECK(native_font_catalog_prepare(nullptr,c,0,0,true,&scratch,&alias).error==NativeFontError::invalid_input);
    CHECK(native_font_catalog_prepare(session,c,0,0,true,nullptr,&alias).error==NativeFontError::invalid_input);
    CHECK(native_font_catalog_prepare(session,c,0,0,true,&scratch,nullptr).error==NativeFontError::invalid_input);
    CHECK(native_font_session_stats(session).metadata_committed==empty.metadata_committed&&!native_font_session_stats(session).live.files);
    const NativeFontInstance* first=nullptr;
    for(u32 i=0;i<10;++i){arena_reset(&scratch);CHECK(native_font_catalog_prepare(session,c,i,0,false,&scratch,handles+i).ok());const auto* instance=native_font_instance_get(*native_font_session_instances(session),handles[i]);
        CHECK(instance&&instance->geometry.font->face.face_index==i);if(!i)first=instance;CHECK(native_font_instance_get(*native_font_session_instances(session),handles[0])==first);}
    const auto grown=native_font_session_stats(session);CHECK(grown.live.files==1&&grown.live.faces==10&&grown.instances==10&&grown.live.references==10&&grown.peak_faces==10&&grown.peak_instances==10);
    CHECK(native_font_catalog_prepare(session,c,0,0,true,&scratch,&alias).ok()&&alias.slot==handles[0].slot&&alias.generation==handles[0].generation);CHECK(native_font_instance_release(native_font_session_instances(session),alias));alias={};
    for(u32 mode=1;mode<=3;++mode){CHECK(native_font_catalog_prepare(session,c,0,mode,true,&scratch,&alias,&oblique,&bold).ok()&&oblique==((mode&2)!=0)&&bold==((mode&1)!=0)&&alias.slot==handles[0].slot&&alias.generation==handles[0].generation);
        CHECK(native_font_instance_release(native_font_session_instances(session),alias));alias={};}
    NativeFontCatalogEntry entry{};CHECK(native_font_catalog_entry(c,1,&entry).ok()&&entry.face_index==1&&entry.path.count&&entry.file&&!entry.settings.count);
    CHECK(native_font_catalog_validate(entry,*native_font_session_fonts(session),first->face).native_error==ERROR_RETRY);
    CHECK(native_font_catalog_validate({},*native_font_session_fonts(session),first->face).error==NativeFontError::invalid_input);
    CHECK(native_font_catalog_entry(c,0,&entry).ok());
    CHECK(native_font_catalog_validate(entry,*native_font_session_fonts(session),first->face).ok());
    // Every part of the catalog version is required, even for an already live
    // face. A mismatched descriptor must not disturb the retained mapping.
    for(u32 field=0;field<5;++field){auto wrong_file=*entry.file;auto wrong=entry;wrong.file=&wrong_file;
        if(field==0)++wrong_file.bytes;else if(field==1)wrong_file.identity.VolumeSerialNumber^=1;
        else if(field==2)wrong_file.identity.FileId.Identifier[0]^=1;
        else if(field==3)wrong_file.last_write.dwLowDateTime^=1;else wrong_file.change_time.dwLowDateTime^=1;
        CHECK(native_font_catalog_validate(wrong,*native_font_session_fonts(session),first->face).native_error==ERROR_RETRY);
        CHECK(native_font_instance_get(*native_font_session_instances(session),handles[0])==first);
    }
    const u32 saved=candidates[0].first_setting;candidates[0].first_setting=1;
    CHECK(native_font_catalog_entry(c,0,&entry).error==NativeFontError::invalid_input&&!entry.file);
    candidates[0].first_setting=saved;CHECK(native_font_catalog_entry(c,0,nullptr).error==NativeFontError::invalid_input);
    // Destroy the catalog view before releasing instances: no instance borrows it.
    const auto counts=c.count;c.count={};for(u32 i=0;i<10;++i){const auto* instance=native_font_instance_get(*native_font_session_instances(session),handles[i]);CHECK(instance&&instance->geometry.font->face.face_index==i);CHECK(native_font_instance_release(native_font_session_instances(session),handles[i]));handles[i]={};}c.count=counts;
    CHECK(!native_font_session_stats(session).live.files&&!native_font_session_stats(session).instances);
    CHECK(write(temp.paths[0],{collection,size+1}));const auto stale=native_font_catalog_prepare(session,c,0,0,true,&scratch,&alias);
    CHECK(stale.error==NativeFontError::io&&stale.native_error==ERROR_RETRY&&!alias.slot&&!native_font_session_stats(session).live.files&&!native_font_session_stats(session).instances);
    CHECK(native_font_session_release(&session)&&!session);return true;
}
bool units() noexcept {
    const ConstSlice<u8> font{font_family_fixture+24,byte_load_le32(font_family_fixture+12)};CHECK(font.count+24<sizeof(font_family_fixture));
    Temporary temp;CHECK(temp.open());CHECK(write(temp.paths[0],font));CHECK(DeleteFileW(temp.paths[1])&&CreateHardLinkW(temp.paths[1],temp.paths[0],nullptr));
    CHECK(guards(temp.paths[0]));
    Storage storage;auto& c=storage.catalog;u16 name[512];u32 index=0;DWORD before=0,after=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    CHECK(native_font_catalog_add(&c,temp.paths[0],{name,512},&index).ok()&&index==0&&c.count.files==1&&c.count.faces==1&&c.count.candidates==1&&!c.count.settings);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);CHECK(equal(native_font_catalog_path(c,0),wide(temp.paths[0])));
    CHECK(equal(native_font_catalog_name(c,0),wide(L"Probe"))&&c.candidates[0].properties.weight==700);
    CHECK(!native_font_catalog_settings(c,0).count&&!native_font_catalog_path(c,1).count&&!native_font_catalog_name(c,1).count);
    const auto counts=c.count;CHECK(native_font_catalog_add(&c,temp.paths[1],{name,512},&index).ok()&&index==0&&bytes_equal(&counts,&c.count,sizeof(counts)));
    NativeFontProperties props[16];u32 ids[16];NativeFontCatalogMatch match{};
    CHECK(native_font_catalog_match(c,wide(L"pRoBe"),{400,5,0},{props,16},{ids,16},&match)==SfntStatus::ok&&match.candidate==0&&match.properties.weight==700&&!match.simulations);
    CHECK(native_font_catalog_match(c,wide(L"Probe Bold"),{400,5,0},{props,16},{ids,16},&match)==SfntStatus::unsupported_sequence);
    CHECK(native_font_catalog_match(c,wide(L"Probe"),{400,5,0},{},{ids,16},&match)==SfntStatus::output_too_small);
    CHECK(native_font_catalog_match(c,wide(L"Probe"),{400,5,0},{props,16},{},&match)==SfntStatus::output_too_small);
    CHECK(native_font_catalog_match(c,wide(L"Probe"),{0,5,0},{props,16},{ids,16},&match)==SfntStatus::invalid_input);
    CHECK(native_font_catalog_match(c,wide(L"Probe"),{400,5,0},{props,16},{ids,16},&match,0)==SfntStatus::resource_limit);
    CHECK(native_font_catalog_match(c,{}, {400,5,0},{props,16},{ids,16},&match)==SfntStatus::invalid_input);
    CHECK(native_font_catalog_match(c,wide(L"Probe"),{400,5,0},{props,16},{ids,16},nullptr)==SfntStatus::invalid_input);
    CHECK(native_font_catalog_add(nullptr,temp.paths[0],{name,512},&index).error==NativeFontError::invalid_input&&index==0xffffffffu);
    CHECK(native_font_catalog_add(&c,temp.paths[0],{},&index).error==NativeFontError::invalid_input);
    CHECK(native_font_catalog_add(&c,temp.paths[0],{name,512},nullptr).error==NativeFontError::invalid_input);
    // Build a two-face collection whose table offsets refer to the complete file.
    u8 collection[4096]{};const u32 offset[]{20,(20+font.count+3)&~3u};const u32 size=offset[1]+font.count;CHECK(size<sizeof(collection));
    byte_store_be32(collection,sfnt_tag('t','t','c','f'));byte_store_be32(collection+4,0x10000);byte_store_be32(collection+8,2);
    for(u32 i=0;i<2;++i){byte_store_be32(collection+12+i*4,offset[i]);copy_bytes(collection+offset[i],font.data,font.count);
        for(u32 j=0;j<byte_load_be16(font.data+4);++j){auto* field=collection+offset[i]+12+j*16+8;byte_store_be32(field,byte_load_be32(field)+offset[i]);}}
    CHECK(write(temp.paths[2],{collection,size}));CHECK(native_font_catalog_add(&c,temp.paths[2],{name,512},&index).ok()&&index==1&&c.count.faces==3&&c.count.candidates==3);
    CHECK(c.faces[1].index==0&&c.faces[2].index==1&&c.faces[1].name_offset==c.faces[0].name_offset&&c.faces[2].name_offset==c.faces[0].name_offset);
    CHECK(native_font_catalog_match(c,wide(L"Probe"),{400,5,0},{props,16},{ids,16},&match)==SfntStatus::ok&&match.candidate==0);
    NativeFontFileSlot store_files[2]{};NativeFontFaceSlot store_faces[3]{};NativeFontStore store{{store_files,2},{store_faces,3}};NativeFontHandle selected{};
    CHECK(native_font_catalog_acquire(c,2,&store,&selected).ok());const auto* selected_face=native_font_get(store,selected);CHECK(selected_face&&selected_face->geometry.face.face_index==1);
    CHECK(native_font_stats(store).files==1&&native_font_stats(store).references==1&&native_font_release(&store,selected)&&!native_font_stats(store).files);
    CHECK(native_font_catalog_acquire(c,0xffffffffu,&store,&selected).error==NativeFontError::invalid_input&&!selected.slot);
    CHECK(native_font_catalog_acquire(c,0,nullptr,&selected).error==NativeFontError::invalid_input);
    CHECK(native_font_catalog_acquire(c,0,&store,nullptr).error==NativeFontError::invalid_input);
    // Every capacity failure must preserve previously published prefixes.
    CHECK(write(temp.paths[3],font));const auto stable=c.count;NativeFontCatalogFile saved_files[4];NativeFontCatalogFace saved_faces[8];NativeFontCatalogCandidate saved_candidates[16];u16 saved_strings[2048];
    copy_bytes(saved_files,c.files.data,stable.files*sizeof(saved_files[0]));copy_bytes(saved_faces,c.faces.data,stable.faces*sizeof(saved_faces[0]));copy_bytes(saved_candidates,c.candidates.data,stable.candidates*sizeof(saved_candidates[0]));copy_bytes(saved_strings,c.strings.data,stable.strings*2);
    for(u32 which=0;which<5;++which){auto small=c;if(which==0)small.files.count=stable.files;if(which==1)small.faces.count=stable.faces;if(which==2)small.candidates.count=stable.candidates;if(which==3)small.strings.count=stable.strings;
        const auto error=native_font_catalog_add(&small,temp.paths[3],{name,which==4?1u:512u},&index);CHECK(!error.ok()&&index==0xffffffffu&&bytes_equal(&small.count,&stable,sizeof(stable)));
        CHECK(bytes_equal(saved_files,c.files.data,stable.files*sizeof(saved_files[0]))&&bytes_equal(saved_faces,c.faces.data,stable.faces*sizeof(saved_faces[0]))&&bytes_equal(saved_candidates,c.candidates.data,stable.candidates*sizeof(saved_candidates[0]))&&bytes_equal(saved_strings,c.strings.data,stable.strings*2));}
    Storage limited;CHECK(native_font_catalog_add(&limited.catalog,temp.paths[2],{name,512},&index,1).font_status==SfntStatus::resource_limit&&!limited.catalog.count.files);
    CHECK(native_font_catalog_add(&limited.catalog,temp.paths[0],{name,512},&index,65536,0).font_status==SfntStatus::resource_limit&&!limited.catalog.count.files);
    // An invalid later collection face rolls back an otherwise valid first face.
    byte_store_be32(collection+offset[1],0);CHECK(write(temp.paths[3],{collection,size}));CHECK(!native_font_catalog_add(&limited.catalog,temp.paths[3],{name,512},&index).ok()&&!limited.catalog.count.files&&!limited.catalog.count.faces&&!limited.catalog.count.strings);
    CHECK(write(temp.paths[3],font)&&native_font_catalog_add(&limited.catalog,temp.paths[3],{name,512},&index).ok());
    // The completed catalog owns metadata, so paths remain readable after deletion.
    CHECK(DeleteFileW(temp.paths[3]));CHECK(equal(native_font_catalog_name(limited.catalog,0),wide(L"Probe")));
    CHECK(native_font_catalog_add(&limited.catalog,temp.paths[3],{name,512},&index).error==NativeFontError::io);
    // A changed size under the same physical identity requires a fresh catalog.
    u8 longer[4096]{};copy_bytes(longer,font.data,font.count);CHECK(write(temp.paths[0],{longer,font.count+1}));const auto changed=native_font_catalog_add(&c,temp.paths[0],{name,512},&index);
    CHECK(changed.error==NativeFontError::io&&changed.native_error==ERROR_RETRY&&bytes_equal(&c.count,&stable,sizeof(stable)));
    const auto stale=native_font_catalog_acquire(c,0,&store,&selected);CHECK(stale.error==NativeFontError::io&&stale.native_error==ERROR_RETRY&&!selected.slot&&!native_font_stats(store).files);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);CHECK(preparation());CHECK(builder_cases());return true;
}
u16 name[32768];NativeFontCatalogBuilder builder{};NativeFontCatalog catalog{};Arena publication{};
NativeFontProperties query_properties[8192];u32 query_indices[8192];
bool acquired[8192];NativeFontFileSlot acquire_files[1];NativeFontFaceSlot acquire_faces[1];NativeFontStore acquire_store{{acquire_files,1},{acquire_faces,1}};
NativeFontSession* stress_session=nullptr;NativeFontInstanceHandle stress_handles[64];u32 stress_count=0,stress_failures=0;
bool stress_candidate(u32 candidate,Arena& scratch) noexcept {
    if(!stress_session)CHECK(native_font_session_create(&stress_session));const auto before=native_font_session_stats(stress_session);NativeFontInstanceHandle handle{};arena_reset(&scratch);
    const auto result=native_font_catalog_prepare(stress_session,catalog,candidate,0,false,&scratch,&handle);
    if(stress_count<64){CHECK(result.ok()&&handle.slot);stress_handles[stress_count++]=handle;}
    else{CHECK(result.error==NativeFontError::capacity&&!handle.slot);++stress_failures;const auto after=native_font_session_stats(stress_session);
        CHECK(after.instances==before.instances&&after.live.files==before.live.files&&after.live.faces==before.live.faces&&after.live.references==before.live.references&&after.live.mapped_bytes==before.live.mapped_bytes);}
    return true;
}
bool release_stress() noexcept {
    if(!stress_session)return true;for(u32 i=0;i<stress_count;++i)CHECK(native_font_instance_release(native_font_session_instances(stress_session),stress_handles[i]));
    CHECK(!native_font_session_stats(stress_session).instances&&!native_font_session_stats(stress_session).live.files);CHECK(native_font_session_release(&stress_session));
    print("{\"kind\":\"session_capacity\",\"retained\":");number(stress_count);print(",\"capacity_failures\":");number(stress_failures);print(",\"released\":true}\n");return true;
}
bool prepare_candidate(u32 candidate) noexcept {
    NativeFontSession* session=nullptr;Arena scratch{};NativeFontInstanceHandle handles[2]{};
    struct Release {NativeFontSession*& s;Arena& a;NativeFontInstanceHandle* h;~Release() noexcept {if(s){for(u32 i=0;i<2;++i)if(h[i].slot)native_font_instance_release(native_font_session_instances(s),h[i]);native_font_session_release(&s);}arena_release(&a);}} release{session,scratch,handles};
    CHECK(native_font_session_create(&session)&&arena_initialize(&scratch,4*1024*1024,0,4096));
    for(u32 mode=0;mode<2;++mode){arena_reset(&scratch);const auto result=native_font_catalog_prepare(session,catalog,candidate,0,mode!=0,&scratch,handles+mode);if(!result.ok()){print("prepare failure candidate=");number(candidate);print(" mode=");number(mode);print(" error=");number(static_cast<u32>(result.error));print(" font=");number(static_cast<u32>(result.font_status));print("\n");}CHECK(result.ok());
        const auto* instance=native_font_instance_get(*native_font_session_instances(session),handles[mode]);CHECK(instance&&instance->geometry.font->face.face_index==catalog.faces[catalog.candidates[candidate].face].index);
        print("{\"kind\":\"prepared\",\"candidate\":");number(candidate);print(",\"mode\":");number(mode);print(",\"instructions\":");number(instance->instructions?1u:0u);print(",\"coordinate_bits\":[");
        const auto coordinates=instance->geometry.variation?instance->geometry.variation->coordinates:ConstSlice<double>{};
        for(u32 i=0;i<coordinates.count;++i){if(i)ch(',');const float value=static_cast<float>(coordinates[i]);u32 bits=0;copy_bytes(&bits,&value,4);number(bits);}print("]}\n");}
    for(auto& handle:handles){CHECK(native_font_instance_release(native_font_session_instances(session),handle));handle={};}
    CHECK(!native_font_session_stats(session).live.files&&!native_font_session_stats(session).instances);CHECK(native_font_session_release(&session));CHECK(stress_candidate(candidate,scratch));return true;
}
bool add(const wchar_t* path,bool default_face=false) noexcept {u32 index=0;const auto status=native_font_catalog_builder_add(&builder,path,{name,32768},&index,65536,1000000,default_face);
    print("{\"kind\":\"add\",\"path\":");string(wide(path));print(",\"error\":");number(static_cast<u32>(status.error));print(",\"font_status\":");number(static_cast<u32>(status.font_status));print(",\"native_error\":");number(status.native_error);print(",\"file\":");number(index);print("}\n");return status.ok();}
void dump() noexcept {
    for(u32 i=0;i<catalog.count.files;++i){print("{\"kind\":\"file\",\"id\":");number(i);print(",\"path\":");string(native_font_catalog_path(catalog,i));print("}\n");}
    for(u32 i=0;i<catalog.count.faces;++i){const auto& f=catalog.faces[i];print("{\"kind\":\"face\",\"id\":");number(i);print(",\"file\":");number(f.file);print(",\"index\":");number(f.index);print(",\"family\":");string(native_font_catalog_name(catalog,i));print("}\n");}
    for(u32 i=0;i<catalog.count.candidates;++i){const auto& c=catalog.candidates[i];print("{\"kind\":\"candidate\",\"id\":");number(i);print(",\"face\":");number(c.face);print(",\"named_index\":");number(c.named_index);print(",\"properties\":");properties(c.properties);print(",\"settings\":[");const auto values=native_font_catalog_settings(catalog,i);
        for(u32 j=0;j<values.count;++j){if(j)ch(',');ch('[');number(values[j].tag);ch(',');const float value=static_cast<float>(values[j].value);u32 bits=0;copy_bytes(&bits,&value,4);number(bits);ch(']');}print("]}\n");}
    print("{\"kind\":\"catalog\",\"files\":");number(catalog.count.files);print(",\"faces\":");number(catalog.count.faces);print(",\"candidates\":");number(catalog.count.candidates);print(",\"settings\":");number(catalog.count.settings);print(",\"string_units\":");number(catalog.count.strings);
    print(",\"used_bytes\":");number(catalog.count.files*sizeof(NativeFontCatalogFile)+catalog.count.faces*sizeof(NativeFontCatalogFace)+catalog.count.candidates*sizeof(NativeFontCatalogCandidate)+catalog.count.settings*sizeof(FontAxisSetting)+catalog.count.strings*2);print("}\n");
}
bool queries(const wchar_t* path) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);const u8* data=nullptr;
    struct Cleanup {HANDLE f;const u8*& p;~Cleanup() noexcept {if(p)UnmapViewOfFile(p);CloseHandle(f);}} cleanup{file,data};LARGE_INTEGER size{};CHECK(GetFileSizeEx(file,&size)&&size.QuadPart>=8&&size.QuadPart<=0xffffffffll);
    HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);CHECK(mapping);data=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));CloseHandle(mapping);CHECK(data&&bytes_equal(data,"MCFQ",4));
    const u32 count=byte_load_le32(data+4);u32 at=8;CHECK(count<=65536);
    for(u32 i=0;i<count;++i){CHECK(size.QuadPart-at>=8);const u32 units=byte_load_le32(data+at);const NativeFontProperties request{byte_load_le16(data+at+4),data[at+6],data[at+7]};at+=8;CHECK(units&&units<=32767&&size.QuadPart-at>=units*2);
        NativeFontCatalogMatch match{};const auto status=native_font_catalog_match(catalog,{reinterpret_cast<const u16*>(data+at),units},request,{query_properties,8192},{query_indices,8192},&match);at+=units*2;
        if(status==SfntStatus::ok&&!acquired[match.candidate]){NativeFontHandle selected{};const auto result=native_font_catalog_acquire(catalog,match.candidate,&acquire_store,&selected);CHECK(result.ok());
            const auto* prepared=native_font_get(acquire_store,selected);CHECK(prepared&&prepared->geometry.face.face_index==catalog.faces[catalog.candidates[match.candidate].face].index);
            CHECK(native_font_release(&acquire_store,selected)&&!native_font_stats(acquire_store).files);acquired[match.candidate]=true;
            print("{\"kind\":\"acquire\",\"candidate\":");number(match.candidate);print(",\"released\":true}\n");CHECK(prepare_candidate(match.candidate));}
        print("{\"kind\":\"match\",\"id\":");number(i);print(",\"status\":");number(static_cast<u32>(status));print(",\"candidate\":");number(match.candidate);print(",\"properties\":");properties(match.properties);print(",\"simulations\":");number(match.simulations);print("}\n");}
    CHECK(at==size.QuadPart);return true;
}
bool argument(const wchar_t** command,wchar_t* out,u32 available) noexcept {const wchar_t* p=*command;while(*p==L' '||*p==L'\t')++p;const bool quoted=*p==L'"';if(quoted)++p;u32 n=0;
    while(*p&&!(quoted?*p==L'"':*p==L' '||*p==L'\t')){if(n+1>=available)return false;out[n++]=*p++;}if(quoted){if(*p!=L'"')return false;++p;}out[n]=0;*command=p;return n!=0;}
#include "font_handle_stat_units.inc"
bool run() noexcept {
    static wchar_t arg[32768],query[32768];const wchar_t* command=GetCommandLineW();CHECK(argument(&command,arg,32768));while(*command==L' '||*command==L'\t')++command;
    if(!*command)return units()&&metadata_projection_cases()&&catalog_growth_cases()&&path_stat_tests::run()&&handle_stat_tests::run();
    CHECK(argument(&command,arg,32768));
    if(equal(wide(arg),wide(L"--handle-stat-check")))return handle_stat_tests::run();
    if(equal(wide(arg),wide(L"--open-proof-check"))){CHECK(argument(&command,arg,32768)&&argument(&command,query,32768)&&query[0]>=L'0'&&query[0]<=L'1'&&!query[1]);return path_stat_tests::proof(arg,query[0]==L'1');}
    if(equal(wide(arg),wide(L"--handle-stat-fallback-check")))return handle_stat_tests::run(true);
    CHECK(native_font_catalog_builder_initialize(&builder)&&arena_initialize(&publication,2*1024*1024,0,4096));
    {const bool default_face=equal(wide(arg),wide(L"--default"));CHECK((default_face||equal(wide(arg),wide(L"--file")))&&argument(&command,arg,32768));CHECK(add(arg,default_face));}
    CHECK(native_font_catalog_publish(builder.catalog,&publication,&catalog).ok());
    const u64 build_commit=builder.files.committed+builder.faces.committed+builder.candidates.committed+builder.settings.committed+builder.strings.committed;
    print("{\"kind\":\"publication\",\"builder_commit\":");number(static_cast<u32>(build_commit));print(",\"used_bytes\":");number(static_cast<u32>(publication.offset));print(",\"committed_bytes\":");number(static_cast<u32>(publication.committed));print("}\n");
    native_font_catalog_builder_release(&builder);DWORD old=0;CHECK(VirtualProtect(publication.base,publication.committed,PAGE_READONLY,&old));
    dump();CHECK(argument(&command,query,32768));while(*command==L' '||*command==L'\t')++command;CHECK(!*command);return queries(query);
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_font_catalog_entry() noexcept {
    using namespace maicord;bool ok=run();ok=release_stress()&&ok;native_font_catalog_builder_release(&builder);arena_release(&publication);print("checks=");number(checks);print(ok?"\nPASS native font catalog\n":"\nFAIL native font catalog\n");flush();ExitProcess(ok&&print_ok?0:1);
}
