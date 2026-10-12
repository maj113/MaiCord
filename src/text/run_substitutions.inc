struct GsubOwner:SfntGsubLookupSource {
    Arena* pool;
    usize used,limit;
    SfntLayout layout;
    SfntGsubProgram program;
    Slice<const SfntGsubExecutableLookup*> lookups;
    u32 work;
};
void release_substitutions(Arena*& pool) noexcept {
    if(!pool)return;auto memory=*pool;pool=nullptr;arena_release(&memory);
}
struct SubstitutionScope {
    Arena*& pool;ArenaMark mark;bool existed,keep=false;
    explicit SubstitutionScope(Arena*& value) noexcept:pool(value),mark(value?arena_mark(value):ArenaMark{}),existed(value!=nullptr) {}
    ~SubstitutionScope() noexcept {if(!keep&&pool){if(existed)arena_rewind(pool,mark);else release_substitutions(pool);}}
};
SfntStatus prepare_substitution(SfntGsubLookupSource* source,u16 index) noexcept {
    auto& owner=*static_cast<GsubOwner*>(source);const u32 work=owner.work;
    SfntGsubProgramRequirements need{};auto status=sfnt_gsub_lookup_requirements(owner.layout,index,&need,work);
    if(status!=SfntStatus::ok)return status;
    const usize payload=sizeof(SfntGsubExecutableLookup)+static_cast<usize>(need.subtables)*sizeof(SfntGsubExecutableSubtable)+static_cast<usize>(need.coverages)*sizeof(SfntCoverage)+static_cast<usize>(need.classes)*sizeof(SfntClassDef);
    const usize growth=payload+((0-owner.pool->offset)&7u);
    if(growth>owner.limit-owner.used||growth>owner.pool->reserved-owner.pool->offset)return SfntStatus::resource_limit;
    const auto mark=arena_mark(owner.pool);bool published=false;
    struct Rollback {GsubOwner& owner;ArenaMark mark;bool& published;~Rollback() noexcept {
        if(published)return;
        arena_rewind(owner.pool,mark);
    }} rollback{owner,mark,published};
    auto* prepared=arena_allocate<SfntGsubExecutableLookup>(owner.pool);if(!prepared)return SfntStatus::resource_limit;
    SfntGsubLookupStorage storage{take<SfntGsubExecutableSubtable>(*owner.pool,need.subtables),
        take<SfntCoverage>(*owner.pool,need.coverages),take<SfntClassDef>(*owner.pool,need.classes)};
    if(!filled(storage.subtables)||!filled(storage.coverages)||!filled(storage.classes))return SfntStatus::resource_limit;
    status=sfnt_gsub_prepare_lookup(owner.layout,index,owner.program.gdef,owner.program.glyph_limit,storage,prepared,work,work,true,true);
    if(status!=SfntStatus::ok)return status;
    owner.used+=owner.pool->offset-mark.offset;owner.lookups[index]=prepared;published=true;return SfntStatus::ok;
}
SfntStatus deferred_substitutions(const SfntLayout& layout,const SfntGdef& gdef,u32 limit,u32 work,usize bytes,Arena*& pool,SfntGsubProgram& program) noexcept {
    program={{},gdef,limit};if(!layout.lookup_count)return SfntStatus::ok;
    if(layout.lookup_count>work)return SfntStatus::resource_limit;
    if(!pool){Arena memory{};if(!arena_initialize(&memory,16*1024*1024,0,4096))return SfntStatus::resource_limit;
        auto* stored=arena_allocate<Arena>(&memory);if(!stored){arena_release(&memory);return SfntStatus::resource_limit;}*stored=memory;pool=stored;}
    const auto mark=arena_mark(pool);auto* owner=arena_allocate_zero<GsubOwner>(pool);if(!owner)return SfntStatus::resource_limit;
    owner->pool=pool;owner->limit=bytes;owner->prepare=prepare_substitution;owner->layout=layout;owner->work=work;
    owner->lookups={arena_allocate_zero<const SfntGsubExecutableLookup*>(pool,layout.lookup_count),layout.lookup_count};
    if(!filled(owner->lookups))return SfntStatus::resource_limit;
    owner->used=pool->offset-mark.offset;
    owner->entries=owner->lookups.data;
    owner->program={{nullptr,owner->lookups.count},gdef,limit,owner};program=owner->program;return SfntStatus::ok;
}
