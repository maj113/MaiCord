namespace {
#include "generated/system_fallback_data.inc"
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
SfntStatus system_coverage(SystemMapping& m,const NativeFontCatalog& catalog,u32 token,u32 cp,bool* output,u32 scope) noexcept {
    *output=false;auto& cache=m.owner->coverage;
    // End the borrow before probing another face, which may evict it.
    if(!m.bound&&m.coverage.font&&m.coverage.candidate==token){
        u16 glyph=0;const auto status=sfnt_cmap_glyph(m.coverage.font->cmap,cp,0,&glyph);
        if(status==SfntStatus::ok)*output=glyph!=0;return status;
    }
    if(!m.bound)m.coverage.font=nullptr;
    const NativeFontFace* borrowed=nullptr;
    const auto result=native_font_coverage_probe(&cache,catalog,cp,output,m.bound?nullptr:&borrowed);
    if(!m.bound)m.coverage={borrowed,token};
    if(result.ok())return SfntStatus::ok;acquisition_failed(*m.owner,scope,result);
    return result.error==NativeFontError::font?result.font_status:SfntStatus::invalid_input;
}
// The candidates of one face share its cmap, so one probe covers them all and the
// preferred candidate is the first. Tokens fit: load_face caps global candidate
// indices at 0x1fffff and append_scope caps scopes at 32.
u32 primary_token(const FamilyOrder& family) noexcept {return (family.scope<<21)|(family.base+family.order[0]);}
// Whether the family's face covers cp. ASCII answers are kept per family: [known, covered] bit sets.
SfntStatus primary_covers(SystemMapping& m,const FamilyOrder& family,u32 cp,bool* covered) noexcept {
    if(cp>=128)return system_coverage(m,*family.catalog,primary_token(family),cp,covered,family.scope);
    const u64 bit=u64{1}<<(cp&63);u64& known=family.ascii[cp>>6];u64& has=family.ascii[2+(cp>>6)];
    if(!(known&bit)){bool value=false;const auto status=system_coverage(m,*family.catalog,primary_token(family),cp,&value,family.scope);
        if(status!=SfntStatus::ok)return status;known|=bit;if(value)has|=bit;}
    *covered=(has&bit)!=0;return SfntStatus::ok;
}
SfntStatus system_family(SystemMapping& m,const FamilyOrder& family,u32 cp,u32* output) noexcept {
    *output=native_fallback_missing;if(!family.order.count)return SfntStatus::ok;
    // family_order validated the immutable catalog, request and order.
    bool covered=false;
    const auto status=primary_covers(m,family,cp,&covered);
    if(status!=SfntStatus::ok)return status;
    if(covered){*output=primary_token(family);return SfntStatus::ok;}
    if(family.scope)return SfntStatus::ok;
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
        const auto covered_status=system_coverage(m,*companion->catalog,companion->base+companion->order[0],cp,&covered,0);
        if(covered_status!=SfntStatus::ok)return covered_status;if(!covered)continue;
        for(u32 i=0;i<companion->order.count;++i){const u32 candidate=companion->order[i];const auto value=companion->catalog->properties[candidate];
            if(*output!=native_fallback_missing){const NativeFontProperties choices[]{best,value};
                if(!native_font_match({choices,2},m.properties).index)continue;
            }best=value;*output=companion->base+candidate;
        }
    }return SfntStatus::ok;
}
SfntStatus system_base(void* context,u32 cp,u32* output) noexcept {
    auto& m=*static_cast<SystemMapping*>(context);return system_family(m,*m.base,cp,output);
}
SfntStatus system_route(void* context,u32 cp,u32* output) noexcept {
    auto& m=*static_cast<SystemMapping*>(context);*output=native_fallback_cascade_find(system_fallback_policy,cp);
    if(!m.bound){m.targets={};m.route=*output;m.bound=true;}return SfntStatus::ok;
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
// Tokens come from system_family in this same query, so their scope and candidate exist.
SfntStatus system_cover(void* context,u32 font,u32 cp,bool* output) noexcept {
    auto& m=*static_cast<SystemMapping*>(context);const u32 scope=font>>21;
    u32 candidate=font&fallback_candidate_mask;const auto* catalog=candidate_catalog(*m.owner,scope,&candidate);
    return system_coverage(m,*catalog,font,cp,output,scope);
}
}
bool native_text_catalog_range(NativeTextCatalogs* owner,const NativeTextFontQuery& query,NativeFontSession* session,Arena* scratch,Utf8View text,NativeTextFontRange* output) noexcept {
    if(output)*output={{native_fallback_missing,{},0},0,0,native_fallback_missing};
    if(!owner||!output||!session||!scratch||!scratch->base||query.scope>=owner->count||!query.family.data||!query.family.count||query.family.count>32767||!text.data||!text.bytes)return false;
    const auto props=query.properties;
    if(props.width!=5||(props.weight!=400&&props.weight!=600)||(props.style!=0&&props.style!=2))return false;
    const FamilyOrder* family=nullptr;if(!coverage_ready(*owner,session)||!family_order(*owner,query,*scratch,&family))return false;
    SystemMapping mapping{owner,scratch,props,family};NativeFallbackRangeResult range{};
    // Leading ASCII that the base family's own face covers always maps to its first
    // candidate (no routing, attachment or companions), so the walker only needs the
    // last such scalar for context when a non-ASCII scalar follows.
    u32 skip=0;bool whole=false;
    if(family->order.count){u32 prefix=0;bool covered=true;
        for(;prefix<text.bytes&&static_cast<u8>(text.data[prefix])<128;++prefix){
            if(primary_covers(mapping,*family,static_cast<u8>(text.data[prefix]),&covered)!=SfntStatus::ok){prefix=0;break;}
            if(!covered)break;}
        if(prefix&&(prefix==text.bytes||static_cast<u8>(text.data[prefix])<128)){whole=true;range={{primary_token(*family),native_fallback_missing},prefix,prefix};}
        else if(prefix)skip=prefix-1;}
    if(!whole){if(native_font_fallback_map({text.data+skip,text.bytes-skip},system_base,system_route,system_font,system_cover,&mapping,&range)!=SfntStatus::ok)return false;
        range.bytes+=skip;range.units+=skip;}
    NativeTextFontRange result{{native_fallback_missing,{},0},range.bytes,range.units,native_fallback_missing};
    if(range.choice.font!=native_fallback_missing){const u32 scope=range.choice.font>>21,global=range.choice.font&fallback_candidate_mask;u32 candidate=global;const auto* selected=candidate_catalog(*owner,scope,&candidate);if(!selected)return false;const auto& catalog=*selected;
        const auto match=native_font_match({catalog.properties+candidate,1},props);
        result.match={global,match.properties,match.simulations};result.scope=scope;
    }*output=result;return true;
}
