namespace {
#include "../text/generated/system_fallback_data.inc"
constexpr u32 fallback_candidate_mask=0x1fffffu;
struct BaseCoverage {const NativeFontFace* font;u32 candidate;};
struct RoutingTargets {const FamilyOrder* values[system_fallback_max_families];};
static_assert(sizeof(BaseCoverage)<=sizeof(RoutingTargets));
struct SystemMapping {
    NativeTextCatalogs* owner;Arena* scratch;NativeFontProperties properties;const FamilyOrder* base;
    u32 route=native_fallback_missing;bool bound=false;
    // The base-family phase and routed-target phase never overlap. The latter
    // starts its trivial union member in system_route.
    union {BaseCoverage coverage{};RoutingTargets targets;};
};
static_assert(sizeof(SystemMapping)==40+sizeof(RoutingTargets));
SfntStatus system_coverage(SystemMapping& m,const NativeFontCatalog& catalog,u32 candidate,u32 token,u32 cp,bool* output,u32 scope) noexcept {
    *output=false;auto& cache=m.owner->coverage;
    // End the borrow before probing another face, which may evict it.
    if(!m.bound&&m.coverage.font&&m.coverage.candidate==token){
        u16 glyph=0;const auto status=sfnt_cmap_glyph(m.coverage.font->cmap,cp,0,&glyph);
        if(status==SfntStatus::ok)*output=glyph!=0;return status;
    }
    if(!m.bound)m.coverage.font=nullptr;
    const NativeFontFace* borrowed=nullptr;
    const auto result=native_font_coverage_probe(&cache,catalog,candidate,cp,output,m.bound?nullptr:&borrowed);
    if(!m.bound)m.coverage={borrowed,token};
    if(result.ok())return SfntStatus::ok;acquisition_failed(*m.owner,scope,result);
    return result.error==NativeFontError::font?result.font_status:SfntStatus::invalid_input;
}
SfntStatus system_family_faces(SystemMapping& m,const FamilyOrder& family,u32 cp,u32* output) noexcept {
    *output=native_fallback_missing;if(!family.order.count)return SfntStatus::ok;
    // family_order already validated the immutable catalog, request and order.
    // Range scanning needs only the first covered candidate; catalog_range
    // resolves its properties/simulations once, after the map completes.
    const auto& catalog=*family.catalog;
    for(u32 i=0;i<family.order.count;++i){
        if(i==1000000)return SfntStatus::resource_limit;
        const u32 candidate=family.order[i];bool covered=false;
        if(candidate+family.base>fallback_candidate_mask||family.scope>=32)return SfntStatus::resource_limit;
        const u32 token=(family.scope<<21)|(candidate+family.base);
        const auto status=system_coverage(m,catalog,candidate,token,cp,&covered,family.scope);if(status!=SfntStatus::ok)return status;
        if(covered){*output=token;return SfntStatus::ok;}
    }return SfntStatus::ok;
}
SfntStatus system_family(SystemMapping& m,const FamilyOrder& family,u32 cp,u32* output) noexcept {
    const auto status=system_family_faces(m,family,cp,output);
    if(status!=SfntStatus::ok||*output!=native_fallback_missing||family.scope||!family.order.count)return status;
    if(!family.companions){family.companions=arena_allocate_zero<const FamilyOrder*>(&m.owner->storage,4);
        if(!family.companions){catalog_failed(*m.owner,0,{NativeFontError::capacity,SfntStatus::output_too_small,0});return SfntStatus::invalid_input;}}
    NativeFontProperties best{};
    for(u32 variant=1;variant<5;++variant){auto properties=family.properties;
        if(variant==4){if(*output!=native_fallback_missing)break;properties.weight=300;properties.style=0;}
        if(variant&1)properties.weight=properties.weight==600?400:600;
        if(variant&2)properties.style=properties.style?0:2;
        auto*& companion=family.companions[variant-1];
        if(!companion&&!family_order(*m.owner,{0,family.name,properties},*m.scratch,&companion))return SfntStatus::invalid_input;
        if(companion->catalog==family.catalog||!companion->order.count)continue;
        for(u32 i=0;i<companion->order.count;++i){const u32 candidate=companion->order[i],token=companion->base+candidate;bool covered=false;
            const auto covered_status=system_coverage(m,*companion->catalog,candidate,token,cp,&covered,0);
            if(covered_status!=SfntStatus::ok)return covered_status;if(!covered)continue;
            const auto value=companion->catalog->candidates[candidate].properties;
            if(*output!=native_fallback_missing){const NativeFontProperties choices[]{best,value};NativeFontMatch match{};
                if(native_font_match({choices,2},m.properties,&match)!=SfntStatus::ok)return SfntStatus::invalid_input;
                if(!match.index)continue;
            }best=value;*output=token;
        }
    }return SfntStatus::ok;
}
SfntStatus system_base(void* context,u32 cp,u32* output) noexcept {
    auto& m=*static_cast<SystemMapping*>(context);return system_family(m,*m.base,cp,output);
}
SfntStatus system_route(void* context,u32 cp,u32* output) noexcept {
    auto& m=*static_cast<SystemMapping*>(context);const auto status=native_fallback_cascade_find(system_fallback_policy,cp,output);
    if(status==SfntStatus::ok&&!m.bound){m.targets={};m.route=*output;m.bound=true;}return status;
}
SfntStatus system_font(void* context,u32 cp,u32* output) noexcept {
    auto& m=*static_cast<SystemMapping*>(context);*output=native_fallback_missing;if(!m.bound)return SfntStatus::invalid_input;
    if(m.route==native_fallback_missing)return SfntStatus::ok;const auto& range=system_fallback_policy.ranges[m.route];
    for(u32 i=0;i<range.count;++i){auto*& family=m.targets.values[i];if(!family){const auto& name=system_fallback_families[system_fallback_policy.tokens[range.offset+i]];u16 wide[64];
            for(u32 j=0;j<name.count;++j)wide[j]=static_cast<u8>(system_fallback_names[name.offset+j]);
            if(!family_order(*m.owner,{0,{wide,name.count},m.properties},*m.scratch,&family))return SfntStatus::invalid_input;}
        const auto status=system_family(m,*family,cp,output);if(status!=SfntStatus::ok||*output!=native_fallback_missing)return status;
    }return SfntStatus::ok;
}
SfntStatus system_cover(void* context,u32 font,u32 cp,bool* output) noexcept {
    auto& m=*static_cast<SystemMapping*>(context);const u32 scope=font>>21;if(scope>=m.owner->count)return SfntStatus::invalid_input;
    u32 candidate=font&fallback_candidate_mask;const auto* catalog=candidate_catalog(*m.owner,scope,&candidate);
    return catalog?system_coverage(m,*catalog,candidate,font,cp,output,scope):SfntStatus::invalid_input;
}
}
namespace {
bool catalog_range(bool routed,NativeTextCatalogs* owner,const NativeTextFontQuery& query,NativeFontSession* session,Arena* scratch,Utf8View text,NativeTextFontRange* output) noexcept {
    if(output)*output={{native_fallback_missing,{},0},0,0,native_fallback_missing};
    if(!owner||!output||!session||!scratch||!scratch->base||query.scope>=owner->count||!query.family.data||!query.family.count||query.family.count>32767||!text.data||!text.bytes)return false;
    const auto props=query.properties;
    if(routed&&(props.width!=5||(props.weight!=400&&props.weight!=600)||(props.style!=0&&props.style!=2)))return false;
    const FamilyOrder* family=nullptr;if(!coverage_ready(*owner,session)||!family_order(*owner,query,*scratch,&family))return false;
    if(!routed&&!family->order.count)return false;
    SystemMapping mapping{owner,scratch,props,family};NativeFallbackRangeResult range{};
    const auto status=routed?native_font_fallback_map(text,system_base,system_route,system_font,system_cover,&mapping,&range):
        native_font_fallback_range(text,[](void* context,u32 cp,NativeFallbackChoice* choice) noexcept {
            choice->policy=0;return system_base(context,cp,&choice->font);},system_cover,&mapping,&range);
    if(status!=SfntStatus::ok)return false;
    NativeTextFontRange result{{native_fallback_missing,{},0},range.bytes,range.units,native_fallback_missing};
    if(range.choice.font!=native_fallback_missing){const u32 scope=range.choice.font>>21,global=range.choice.font&fallback_candidate_mask;u32 candidate=global;const auto* selected=candidate_catalog(*owner,scope,&candidate);if(!selected)return false;const auto& catalog=*selected;
        NativeFontMatch match{};if(native_font_match({&catalog.candidates[candidate].properties,1},props,&match)!=SfntStatus::ok)return false;
        result.match={global,match.properties,match.simulations};result.scope=scope;
    }*output=result;return true;
}
}
bool native_text_catalog_range(NativeTextCatalogs* owner,const NativeTextFontQuery& query,NativeFontSession* session,Arena* scratch,Utf8View text,NativeTextFontRange* output) noexcept {
    return catalog_range(true,owner,query,session,scratch,text,output);
}
bool native_text_catalog_family_range(NativeTextCatalogs* owner,const NativeTextFontQuery& query,NativeFontSession* session,Arena* scratch,Utf8View text,NativeTextFontRange* output) noexcept {
    return catalog_range(false,owner,query,session,scratch,text,output);
}
