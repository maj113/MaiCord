#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

// Owner namespaces isolate native and plugin page IDs. Both IDs must be nonzero.
// A key survives compaction and can be reused after its owner unloads.
struct UiPageKey {
    u64 owner_id;
    u64 page_id;
};

constexpr bool ui_page_key_equal(UiPageKey left, UiPageKey right) noexcept {
    return left.owner_id == right.owner_id && left.page_id == right.page_id;
}

struct UiPage {
    UiPageKey key;
    Utf8View label;
    void* context;
};

enum class UiPageError : u8 { none, invalid_argument, duplicate_key, capacity, not_found };

struct UiPageResult {
    UiPageError error;
    bool changed;
    constexpr bool ok() const noexcept { return error == UiPageError::none; }
};

inline constexpr u32 kUiPageNoSelection = ~0u;

// UI-thread only. Storage is caller-owned; descriptors are copied but labels
// and contexts are borrowed and must remain valid until unregister/clear.
// Labels must remain immutable. Unregister an owner before freeing its data or
// unloading its module. No code is invoked by the registry, including on removal.
// Pages enumerate in registration order; removals preserve survivor order.
// First registration selects itself. Removing the selected page chooses the
// next surviving page in order, or the previous page if none follows it.
struct UiPageRegistry {
    Slice<UiPage> storage;
    u32 count;
    u32 selected_index;
    u32 revision;
};

bool ui_page_registry_initialize(UiPageRegistry* registry, Slice<UiPage> storage) noexcept;
UiPageResult ui_page_registry_register(UiPageRegistry* registry, const UiPage& page) noexcept;
UiPageResult ui_page_registry_unregister(UiPageRegistry* registry, UiPageKey key) noexcept;
// Drops every borrowed descriptor for an owner in one bounded compaction pass.
// Returns the number removed; zero also covers invalid input.
u32 ui_page_registry_unregister_owner(UiPageRegistry* registry, u64 owner_id) noexcept;
UiPageResult ui_page_registry_select(UiPageRegistry* registry, UiPageKey key) noexcept;
bool ui_page_registry_clear(UiPageRegistry* registry) noexcept;
// Returned pointers borrow registry storage and must not be retained across
// mutations. Query selected once on revision change for external scene binding.
const UiPage* ui_page_registry_at(const UiPageRegistry& registry, u32 index) noexcept;
const UiPage* ui_page_registry_find(const UiPageRegistry& registry, UiPageKey key) noexcept;
const UiPage* ui_page_registry_selected(const UiPageRegistry& registry) noexcept;

} // namespace maicord
