#include "maicord/ui/shell/page_registry.hpp"
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
