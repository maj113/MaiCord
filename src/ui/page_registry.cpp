#include "maicord/ui/page_registry.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/utf.hpp"

namespace maicord {
namespace {

bool valid_key(UiPageKey key) noexcept {
    return key.owner_id && key.page_id;
}

u32 find_index(const UiPageRegistry& registry, UiPageKey key) noexcept {
    for (u32 index = 0; index < registry.count; ++index) {
        if (ui_page_key_equal(registry.storage[index].key, key)) return index;
    }
    return kUiPageNoSelection;
}

// Retain selection by logical identity while compacting. If it is removed,
// select the first survivor after its original position, then the last before.
u32 remove_pages(UiPageRegistry* registry, UiPageKey key, bool whole_owner) noexcept {
    const u32 old_count = registry->count;
    const u32 old_selected = registry->selected_index;
    u32 count = 0;
    u32 selected = kUiPageNoSelection;
    u32 following = kUiPageNoSelection;
    for (u32 index = 0; index < old_count; ++index) {
        const UiPage& page = registry->storage[index];
        if (whole_owner ? page.key.owner_id == key.owner_id : ui_page_key_equal(page.key, key))
            continue;
        if (index == old_selected) selected = count;
        if (index > old_selected && following == kUiPageNoSelection) following = count;
        if (count != index) registry->storage[count] = page;
        ++count;
    }
    if (count == old_count) return 0;
    // Erase removed descriptors so dormant slots retain no plugin pointers.
    for (u32 index = count; index < old_count; ++index) reset_value(&registry->storage[index]);
    registry->count = count;
    registry->selected_index = !count ? kUiPageNoSelection :
        selected != kUiPageNoSelection ? selected :
        following != kUiPageNoSelection ? following : count - 1;
    saturating_increment(registry->revision);
    return old_count - count;
}

} // namespace

bool ui_page_registry_initialize(UiPageRegistry* registry, Slice<UiPage> storage) noexcept {
    if (!registry || !storage.data || !storage.count) return false;
    *registry = {storage, 0, kUiPageNoSelection, 0};
    return true;
}

UiPageResult ui_page_registry_register(UiPageRegistry* registry, const UiPage& page) noexcept {
    if (!registry || !registry->storage.data || !valid_key(page.key) || !page.label.data ||
        !page.label.bytes || !utf8_validate(page.label).valid)
        return {UiPageError::invalid_argument, false};
    if (find_index(*registry, page.key) != kUiPageNoSelection)
        return {UiPageError::duplicate_key, false};
    if (registry->count == registry->storage.count) return {UiPageError::capacity, false};
    registry->storage[registry->count++] = page;
    if (registry->selected_index == kUiPageNoSelection) registry->selected_index = 0;
    saturating_increment(registry->revision);
    return {UiPageError::none, true};
}

UiPageResult ui_page_registry_unregister(UiPageRegistry* registry, UiPageKey key) noexcept {
    if (!registry || !registry->storage.data || !valid_key(key))
        return {UiPageError::invalid_argument, false};
    return remove_pages(registry, key, false) ? UiPageResult{UiPageError::none, true} :
        UiPageResult{UiPageError::not_found, false};
}

u32 ui_page_registry_unregister_owner(UiPageRegistry* registry, u64 owner_id) noexcept {
    return registry && registry->storage.data && owner_id ?
        remove_pages(registry, {owner_id, 0}, true) : 0;
}

UiPageResult ui_page_registry_select(UiPageRegistry* registry, UiPageKey key) noexcept {
    if (!registry || !registry->storage.data || !valid_key(key))
        return {UiPageError::invalid_argument, false};
    const u32 index = find_index(*registry, key);
    if (index == kUiPageNoSelection) return {UiPageError::not_found, false};
    const bool changed = registry->selected_index != index;
    registry->selected_index = index;
    if (changed) saturating_increment(registry->revision);
    return {UiPageError::none, changed};
}

bool ui_page_registry_clear(UiPageRegistry* registry) noexcept {
    if (!registry || !registry->storage.data || !registry->count) return false;
    for (u32 index = 0; index < registry->count; ++index) reset_value(&registry->storage[index]);
    registry->count = 0;
    registry->selected_index = kUiPageNoSelection;
    saturating_increment(registry->revision);
    return true;
}

const UiPage* ui_page_registry_at(const UiPageRegistry& registry, u32 index) noexcept {
    return registry.storage.data && index < registry.count ? &registry.storage[index] : nullptr;
}

const UiPage* ui_page_registry_find(const UiPageRegistry& registry, UiPageKey key) noexcept {
    return valid_key(key) ? ui_page_registry_at(registry, find_index(registry, key)) : nullptr;
}

const UiPage* ui_page_registry_selected(const UiPageRegistry& registry) noexcept {
    return ui_page_registry_at(registry, registry.selected_index);
}

} // namespace maicord
