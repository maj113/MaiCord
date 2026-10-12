// Recovery is cold: normal formatting keeps the existing one-arena layout.
#pragma optimize("s",on)
bool native_text_context_rebuild(const TextSystem& system,NativeTextRebuild attempt,void* context) noexcept {
    auto* owner=system.native_text;if(!owner||!attempt||owner->catalogs.operation_depth)return false;
    // Renderer font IDs and externally retained runs belong to this stable
    // session. Staging owns its references; rejection releases only those refs.
    auto* fonts=owner->fonts;NativeTextContext* next=nullptr;
    struct Release {NativeTextContext*& n;~Release() noexcept {native_text_context_release(&n);}} release{next};
    if(!native_text_context_initialize(&next,fonts))return false;
    // Durable handles retain their ordering. The new generation gets fresh
    // identities so message/layout caches cannot confuse old and new fonts.
    // Private scopes keep their indices; scope zero is the system catalog.
    for(u32 i=1;i<owner->catalogs.count;++i){const auto& from=owner->catalogs.scopes[i];
        if(!from.path)return false;u32 units=0;while(units<32768&&from.path[units])++units;if(!units||units==32768)return false;
        auto* saved=static_cast<NativeTextCatalogScope*>(arena_allocate(&next->catalogs.storage,
            sizeof(NativeTextCatalogScope)+(static_cast<usize>(units)+1)*2,alignof(NativeTextCatalogScope)));
        if(!saved)return false;*saved={};auto* path=reinterpret_cast<wchar_t*>(saved+1);copy_bytes(path,from.path,(units+1)*2);saved->path=path;
        next->catalogs.scopes.entries[i]=saved;next->catalogs.count=i+1;
    }
    NativeFormatIndex* tail=nullptr;
    for(u32 i=1;i<=owner->format_count;++i){const auto* format=format_at(*owner,i);
        if(!format||!format->family_handle||format->family_handle>i)return false;
        // Source handles are already unique. Reuse the canonical family's new
        // pointer, rather than interning every descriptor against its prefix.
        const auto* family=format->family_handle<i?format_at(*next,format->family_handle):nullptr;
        if(append_format(*next,*format,tail,family?family->font.family.data:nullptr,format->family_handle)!=i)return false;
    }
    // This guard spans every nested shape/measure/ellipsis attempt. A second
    // mismatch cannot recurse, and rejected staging cannot flush its prefix.
    next->code_format=owner->code_format;
    next->catalogs.operation_depth=1;
    auto staged=system;staged.native_text=next;staged.native_fonts=fonts;
    if(!attempt(staged,context)||next->catalogs.stale_scopes)return false;
    next->catalogs.operation_depth=0;
    auto* old_backing=owner->backing;
    // Retiring metadata must not overwrite a cache just published by staging.
    owner->catalogs.operation_depth=1;release_context_contents(owner);
    *owner=*next;
    // Rebase only embedded control data. Bindings/plans and their arena-backed
    // payloads keep their original allocation addresses through retirement.
    owner->arabic_store.runs=&owner->store;
    owner->catalogs.scopes.entries[0]=&owner->catalogs.scopes.system;
    if(owner->catalogs.coverage.session)owner->catalogs.coverage.slots.data=owner->catalogs.coverage_slots;
    for(auto& slot:owner->catalogs.coverage_slots)
        if(slot.catalog==&next->catalogs.scopes.system.catalog)slot.catalog=&owner->catalogs.scopes.system.catalog;
    for(u32 i=1;i<=owner->format_count;++i){auto* format=const_cast<NativeFormat*>(format_at(*owner,i));
        if(format->font.family.data==next->first_family)format->font.family.data=owner->first_family;}
    for(u32 i=0;i<owner->binding_count;++i)if(owner->bindings[i].format==&next->first_format)owner->bindings[i].format=&owner->first_format;
    if(owner->last_format==&next->first_format)owner->last_format=&owner->first_format;
    for(auto* sign=owner->omission_signs;sign;sign=sign->next)if(sign->format==&next->first_format)sign->format=&owner->first_format;
    next=nullptr;
    if(old_backing!=owner)VirtualFree(old_backing,0,MEM_RELEASE);
    else VirtualFree(reinterpret_cast<u8*>(owner)+4096,context_reserved_bytes-4096,MEM_DECOMMIT);
    return true;
}
namespace {
void discard_recovery_output(ShapedText& output) noexcept {
    auto* fonts=output.native_fonts();if(!fonts)return;
    for(u32 i=0;i<output.run_count;++i)native_font_run_release(native_font_session_runs(fonts),output.runs[i].native_face);
    native_font_session_release(&fonts);
}
using RetryAttempt=bool (*)(const TextSystem&,ShapedText*,void*) noexcept;
bool retry_stale_erased(const TextSystem& system,ShapedText* output,RetryAttempt attempt,void* context) noexcept {
    const auto* owner=system.native_text;
    if(!owner||!owner->catalogs.stale_scopes||owner->catalogs.operation_depth||(output&&(output->native_fonts()||output->run_count)))return false;
    ShapedText draft=output?*output:ShapedText{};
    struct Retry {RetryAttempt attempt;ShapedText* output;void* context;} retry{attempt,output?&draft:nullptr,context};
    const bool okay=native_text_context_rebuild(system,[](const TextSystem& staged,void* opaque) noexcept {
        auto& retry=*static_cast<Retry*>(opaque);return retry.attempt(staged,retry.output,retry.context);
    },&retry);
    if(okay){if(output)*output=draft;}else discard_recovery_output(draft);
    return okay;
}
template<class Attempt> bool retry_stale(const TextSystem& system,ShapedText* output,Attempt attempt) noexcept {
    return retry_stale_erased(system,output,[](const TextSystem& staged,ShapedText* draft,void* context) noexcept {
        return (*static_cast<Attempt*>(context))(staged,draft);
    },&attempt);
}
}
#pragma optimize("",on)
