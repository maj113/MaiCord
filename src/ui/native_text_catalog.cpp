#include "maicord/ui/native_text_catalog.hpp"
#include "maicord/core/memory.hpp"
#include "native_text_catalog_owner.hpp"
#include "maicord/text/font_fallback_catalog.hpp"
#include "maicord/core/log.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/gdi.hpp"
#include "maicord/core/win32_file_info.hpp"

namespace maicord {
struct FamilyOrder {FamilyOrder* next;const NativeFontCatalog* catalog;u32 scope,base;NativeFontProperties properties;ConstSlice<u16> name;ConstSlice<u32> order;mutable const FamilyOrder** companions;};
namespace {
bool acquisition_failed(NativeTextCatalogs& owner,u32 scope,NativeFontResult result) noexcept {
    if(result.error==NativeFontError::io&&result.native_error==ERROR_RETRY)owner.stale_scopes|=1u<<scope;
    else if(result.error==NativeFontError::capacity)++owner.retryable_failures;
    return false;
}
bool catalog_failed(NativeTextCatalogs& owner,u32 scope,NativeFontResult result) noexcept {
    ++owner.failures;return acquisition_failed(owner,scope,result);
}
bool same_name(ConstSlice<u16> a,ConstSlice<u16> b) noexcept {
    return a.count==b.count&&CompareStringOrdinal(reinterpret_cast<const wchar_t*>(a.data),a.count,reinterpret_cast<const wchar_t*>(b.data),b.count,TRUE)==CSTR_EQUAL;
}
bool contains_family(const NativeFontCatalog& catalog,const NativeTextFontQuery& query,bool exact=false) noexcept {
    for(u32 i=0;i<catalog.count.faces;++i)if(same_name(native_font_catalog_name(catalog,i),query.family)){
        if(!exact)return true;const auto& face=catalog.faces[i];
        for(u32 j=0;j<face.candidate_count;++j)if(bytes_equal(&catalog.candidates[face.first_candidate+j].properties,&query.properties,4))return true;
    }return false;
}
const NativeFontCatalog* candidate_catalog(NativeTextCatalogs& owner,u32 scope,u32* candidate) noexcept {
    if(scope)return *candidate<owner.scopes[scope].catalog.count.candidates?&owner.scopes[scope].catalog:nullptr;
    for(auto* file=owner.files;file;file=file->next)if(*candidate>=file->first_candidate&&*candidate-file->first_candidate<file->catalog.count.candidates){
        *candidate-=file->first_candidate;return &file->catalog;
    }return nullptr;
}
#include "native_text_catalog_face.inc"
#include "native_text_catalog_locator.inc"
bool build_private(NativeTextCatalogs& owner,const NativeTextFontQuery& query,Arena& scratch,bool* fresh) noexcept {
    const u32 scope_index=query.scope;
    auto& scope=owner.scopes[scope_index];const u32 bit=1u<<scope_index;
    if(owner.attempted&bit)return scope.catalog.count.candidates!=0;owner.attempted|=bit;
    const auto result=load_face(owner,scope.path,0,true,query,false,scratch,&scope.catalog);
    if(!result.ok())return catalog_failed(owner,scope_index,result);*fresh=true;return true;
}
bool family_order(NativeTextCatalogs& owner,const NativeTextFontQuery& query,Arena& scratch,const FamilyOrder** output,bool* loaded=nullptr) noexcept {
    *output=nullptr;if(loaded)*loaded=false;
    const auto properties=query.properties;if(!properties.weight||properties.weight>1000||!properties.width||properties.width>9||properties.style>2)return false;
    for(auto* family=owner.families;family;family=family->next)if(family->scope==query.scope&&bytes_equal(&family->properties,&query.properties,4)&&same_name(family->name,query.family)){*output=family;return true;}
    if(query.scope&&(owner.attempted&(1u<<query.scope))&&!owner.scopes[query.scope].catalog.count.candidates)return false;
    ++owner.builds;LARGE_INTEGER first{},last{};QueryPerformanceCounter(&first);
    struct Elapsed {NativeTextCatalogs& owner;LARGE_INTEGER& first;LARGE_INTEGER& last;
        ~Elapsed() noexcept {if(QueryPerformanceCounter(&last)&&last.QuadPart>=first.QuadPart)owner.build_ticks+=last.QuadPart-first.QuadPart;}} elapsed{owner,first,last};
    const NativeFontCatalog* catalog=nullptr;u32 base=0;bool fresh=false;
    if(query.scope){if(!build_private(owner,query,scratch,&fresh))return false;catalog=&owner.scopes[query.scope].catalog;}
    else if(!system_family_file(owner,query,scratch,&catalog,&base,&fresh))return false;
    arena_reset(&scratch);const u32 n=catalog?catalog->count.candidates:0;u32 count=0;
    auto* props=arena_allocate<NativeFontProperties>(&scratch,n);auto* ids=arena_allocate<u32>(&scratch,n);auto* order=arena_allocate<u32>(&scratch,n);
    if(n&&(!props||!ids||!order))return catalog_failed(owner,query.scope,{NativeFontError::capacity,SfntStatus::output_too_small,0});
    if(catalog){const auto status=native_font_catalog_fallback_order(*catalog,query.family,query.properties,{props,n},{ids,n},{order,n},&count);
        if(status!=SfntStatus::ok&&status!=SfntStatus::unsupported_sequence)return catalog_failed(owner,query.scope,{NativeFontError::font,status,0});}
    auto* family=static_cast<FamilyOrder*>(arena_allocate(&owner.storage,sizeof(FamilyOrder)+static_cast<usize>(count)*4+static_cast<usize>(query.family.count)*2,alignof(FamilyOrder)));
    if(!family)return catalog_failed(owner,query.scope,{NativeFontError::capacity,SfntStatus::output_too_small,0});
    auto* saved=reinterpret_cast<u32*>(family+1);if(count)copy_bytes(saved,order,count*4);
    auto* name=reinterpret_cast<u16*>(saved+count);copy_bytes(name,query.family.data,query.family.count*2);
    *family={owner.families,catalog,query.scope,base,query.properties,{name,query.family.count},{saved,count},nullptr};owner.families=family;*output=family;if(loaded)*loaded=fresh;return true;
}
bool coverage_ready(NativeTextCatalogs& owner,NativeFontSession* session) noexcept {
    return owner.coverage.session?owner.coverage.session==session:native_font_coverage_initialize(session,{owner.coverage_slots,16},&owner.coverage);
}
}
bool native_text_catalogs_initialize(NativeTextCatalogs* owner) noexcept {
    if(!owner||owner->storage.base||owner->count)return false;
    owner->scopes.entries[0]=&owner->scopes.system;
    if(!arena_initialize(&owner->storage,2*1024*1024,0,4096)){native_text_catalogs_release(owner);return false;}
    owner->count=1;return true;
}
void native_text_catalogs_release(NativeTextCatalogs* owner) noexcept {
    if(!owner)return;
    log_u64(LogLevel::info,"text","native_catalog_builds",owner->builds);log_u64(LogLevel::info,"text","native_catalog_selections",owner->selections);
    log_u64(LogLevel::info,"text","native_catalog_unsupported",owner->unsupported);log_u64(LogLevel::info,"text","native_catalog_failures",owner->failures);
    log_u64(LogLevel::info,"text","native_catalog_build_ticks",owner->build_ticks);log_u64(LogLevel::info,"text","native_catalog_commit",owner->storage.committed);
    native_font_coverage_release(&owner->coverage);
    if(owner->locator)owner->locator->release();
    arena_release(&owner->storage);clear_bytes(owner,sizeof(*owner));
}
namespace {
NativeTextCatalogScope* append_scope(NativeTextCatalogs& owner,const wchar_t* path,u32 units) noexcept {
    if(owner.count==32)return nullptr;
    auto* scope=static_cast<NativeTextCatalogScope*>(arena_allocate(&owner.storage,
        sizeof(NativeTextCatalogScope)+(static_cast<usize>(units)+1)*sizeof(wchar_t),alignof(NativeTextCatalogScope)));
    if(!scope)return nullptr;
    reset_value(scope);auto* saved=reinterpret_cast<wchar_t*>(scope+1);copy_bytes(saved,path,(units+1)*2);scope->path=saved;
    owner.scopes.entries[owner.count++]=scope;return scope;
}
}
bool native_text_catalog_register_path(NativeTextCatalogs* owner,const wchar_t* path,u32* scope) noexcept {
    if(!scope)return false;*scope=0xffffffffu;
    if(!owner||!owner->count||!path)return false;u32 units=0;while(units<32768&&path[units])++units;if(!units||units==32768)return false;
    for(u32 i=1;i<owner->count;++i){const auto* p=owner->scopes[i].path;if(!p)continue;u32 n=0;while(p[n])++n;
        if(n==units&&bytes_equal(p,path,(units+1)*2)){*scope=i;return true;}}
    const u32 index=owner->count;if(!append_scope(*owner,path,units))return false;
    *scope=index;return true;
}
bool native_text_catalog_select_query(NativeTextCatalogs* owner,const NativeTextFontQuery& query,NativeFontSession* session,Arena* scratch,
    NativeFontInstanceHandle* output,NativeFontSelection* source,u32* candidate,u32* simulations,u32* selected_scope,bool* missing_family) noexcept {
    if(missing_family)*missing_family=false;
    if(output)reset_value(output);if(source)reset_value(source);if(candidate)*candidate=0xffffffffu;if(simulations)*simulations=0;if(selected_scope)*selected_scope=0xffffffffu;
    if(!owner||query.scope>=owner->count||!query.family.count||!query.family.data||query.family.count>32767||!session||!scratch||!scratch->base||!output||!source)return false;
    const FamilyOrder* family=nullptr;bool loaded=false;if(!coverage_ready(*owner,session)||!family_order(*owner,query,*scratch,&family,&loaded))return false;
    if(!family->order.count){
        if(!selected_scope)return false;
        NativeTextFontRange range{};if(!native_text_catalog_range(owner,query,session,scratch,{" ",1},&range)||range.match.candidate==native_fallback_missing||(range.match.simulations&&!simulations)||
            !native_text_catalog_select_face(owner,range.scope,range.match.candidate,range.match.simulations,session,scratch,output,source))return false;
        if(candidate)*candidate=range.match.candidate;if(simulations)*simulations=range.match.simulations;*selected_scope=range.scope;if(missing_family)*missing_family=true;return true;
    }
    const auto& catalog=*family->catalog;const u32 index=family->order[0];NativeFontMatch match{};
    if(native_font_match({&catalog.candidates[index].properties,1},query.properties,&match)!=SfntStatus::ok||(match.simulations&&!simulations))return false;
    if(!coverage_ready(*owner,session)||!native_text_catalog_select_face(owner,query.scope,family->base+index,match.simulations,session,scratch,output,source,!loaded))return false;
    if(candidate)*candidate=family->base+index;if(simulations)*simulations=match.simulations;if(selected_scope)*selected_scope=query.scope;return true;
}
#include "native_system_fallback.inc"
bool native_text_catalog_select_face(NativeTextCatalogs* owner,u32 scope,u32 candidate,u32 simulations,NativeFontSession* session,Arena* scratch,NativeFontInstanceHandle* output,NativeFontSelection* source,bool refresh) noexcept {
    if(output)reset_value(output);if(source)reset_value(source);
    if(!owner||scope>=owner->count||simulations>3||!session||!scratch||!scratch->base||!output||!source||owner->coverage.session!=session)return false;
    const auto* selected=candidate_catalog(*owner,scope,&candidate);if(!selected)return false;const auto& catalog=*selected;NativeFontCatalogEntry entry{};
    if(!native_font_catalog_entry(catalog,candidate,&entry).ok()||entry.settings.count>2)return false;
    NativeFontHandle face{};const auto acquired=native_font_coverage_acquire(&owner->coverage,catalog,candidate,&face,refresh);
    if(!acquired.ok())return acquisition_failed(*owner,scope,acquired);
    struct Release {NativeFontStore* store;NativeFontHandle face;~Release() noexcept{native_font_release(store,face);}} release{native_font_session_fonts(session),face};
    arena_reset(scratch);NativeFontInstanceHandle instance{};bool oblique=false,bold=false;const auto prepared=native_font_prepare(session,face,entry.settings,simulations,false,scratch,&instance,&oblique,&bold);
    if(!prepared.ok())return acquisition_failed(*owner,scope,prepared);
    source->face=face;source->count=entry.settings.count;for(u32 i=0;i<source->count;++i)source->settings[i]=entry.settings[i];*output=instance;++owner->selections;return true;
}
}
