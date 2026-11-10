// Linux fallback routes (included by native_system_fallback.inc inside its anonymous namespace). A route is one
// interval of the generated table (generated/linux_fallback_data.inc); here it becomes the ordered list of families
// the catalog tries for its scalars: first the families fontconfig sorts for a sample scalar of the interval, in the
// user's language, when libfontconfig is available, then the table's own families that fontconfig did not name. The
// list is built at the first scalar of the route and kept in the catalog's storage.
constexpr u32 fallback_sorted_limit=6;
const NativeFallbackCascade& fallback_policy(const NativeTextCatalogs&) noexcept {return system_fallback_policy;}
const RouteFamilies* route_families(NativeTextCatalogs& owner,u32 route) noexcept {
    const auto& policy=fallback_policy(owner);
    if(route>=policy.ranges.count)return nullptr;
    if(!owner.routes){
        owner.routes=arena_allocate_zero<RouteFamilies*>(&owner.storage,system_fallback_range_count);
        if(!owner.routes){catalog_failed(owner,0,{NativeFontError::capacity,SfntStatus::output_too_small,0});return nullptr;}
    }
    if(owner.routes[route])return owner.routes[route];
    const auto& range=policy.ranges[route];
    SystemFontName sorted[fallback_sorted_limit];u32 sorted_count=0;
    if(range.count){
        auto* locator=system_locator(owner);
        // The interval's first scalar stands for it; the control characters at the start of the Latin interval
        // are represented by a letter.
        const u32 sample=range.first<0x41?0x41u:range.first;
        if(locator)sorted_count=system_font_locator_fallbacks(locator,sample,sorted,fallback_sorted_limit);
    }
    ConstSlice<u16> names[fallback_route_limit];u32 count=0,units=0;
    const auto push=[&](ConstSlice<u16> name) noexcept {
        for(u32 i=0;i<count;++i)if(same_name(names[i],name))return;
        if(count<fallback_route_limit){names[count++]=name;units+=name.count;}
    };
    for(u32 i=0;i<sorted_count;++i)push({sorted[i].units,sorted[i].count});
    // The table's names are ASCII bytes; widened here into the route's own storage below.
    u16 wide[system_fallback_max_families][64];
    for(u32 i=0;i<range.count;++i){
        const char* name=system_fallback_names+system_fallback_token(owner.fallback_locale,range.offset+i);
        u32 length=0;
        for(;name[length];++length)wide[i][length]=static_cast<u8>(name[length]);
        push({wide[i],length});
    }
    auto* route_storage=static_cast<RouteFamilies*>(arena_allocate(&owner.storage,sizeof(RouteFamilies)+static_cast<usize>(units)*2,alignof(RouteFamilies)));
    if(!route_storage){catalog_failed(owner,0,{NativeFontError::capacity,SfntStatus::output_too_small,0});return nullptr;}
    auto* saved=reinterpret_cast<u16*>(route_storage+1);
    route_storage->count=count;
    for(u32 i=0;i<count;++i){copy_bytes(saved,names[i].data,names[i].count*2);route_storage->names[i]={saved,names[i].count};saved+=names[i].count;}
    owner.routes[route]=route_storage;return route_storage;
}
