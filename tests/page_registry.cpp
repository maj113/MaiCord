#include "maicord/ui/page_registry.hpp"

namespace maicord {

void test_ui_page_registry(void (*check)(bool, const char*) noexcept) noexcept {
    UiPage storage[5]{};
    UiPageRegistry registry{};
    check(!ui_page_registry_initialize(nullptr, {storage, 5}) &&
              !ui_page_registry_initialize(&registry, {}) &&
              ui_page_registry_initialize(&registry, {storage, 5}) &&
              !ui_page_registry_selected(registry) && !ui_page_registry_at(registry, 0),
          "page registry initialization rejects missing storage and starts empty");

    u32 native_context = 0, plugin_context = 0;
    UiPage native{{1, 1}, utf8_literal("Account"), &native_context};
    const UiPage plugin{{2, 1}, utf8_literal("Plugin settings"), &plugin_context};
    check(ui_page_registry_register(&registry, native).ok() &&
              ui_page_registry_selected(registry) &&
              ui_page_key_equal(ui_page_registry_selected(registry)->key, native.key) &&
              ui_page_registry_register(&registry, plugin).ok() &&
              registry.count == 2 && ui_page_registry_at(registry, 1)->context == &plugin_context &&
              ui_page_registry_find(registry, {2, 1}) == ui_page_registry_at(registry, 1),
          "owner namespaces share local page IDs with copied descriptors and borrowed context");
    native.context = nullptr;
    native.label = utf8_literal("Changed local descriptor");
    check(ui_page_registry_at(registry, 0)->context == &native_context &&
              utf8_equal(ui_page_registry_at(registry, 0)->label, utf8_literal("Account")),
          "registration copies the descriptor without retaining its address");

    const u32 before_rejected = registry.revision;
    const char malformed[] = {'\xC0', '\xAF'};
    check(ui_page_registry_register(&registry, plugin).error == UiPageError::duplicate_key &&
              ui_page_registry_register(&registry, {{0, 1}, utf8_literal("Bad owner"), nullptr}).error ==
                  UiPageError::invalid_argument &&
              ui_page_registry_register(&registry, {{1, 0}, utf8_literal("Bad page"), nullptr}).error ==
                  UiPageError::invalid_argument &&
              ui_page_registry_register(&registry, {{3, 1}, {}, nullptr}).error == UiPageError::invalid_argument &&
              ui_page_registry_register(&registry, {{3, 1}, {malformed, 2}, nullptr}).error ==
                  UiPageError::invalid_argument &&
              ui_page_registry_select(&registry, {9, 9}).error == UiPageError::not_found &&
              ui_page_registry_unregister(&registry, {9, 9}).error == UiPageError::not_found &&
              registry.count == 2 && registry.revision == before_rejected,
          "duplicate invalid and missing page operations are atomic and preserve revision");

    check(ui_page_registry_register(&registry, {{1, 2}, utf8_literal("Appearance"), nullptr}).ok() &&
              ui_page_registry_register(&registry, {{2, 2}, utf8_literal("Plugin tools"), nullptr}).ok() &&
              ui_page_registry_register(&registry, {{3, 1}, utf8_literal("Other plugin"), nullptr}).ok(),
          "page order follows registration across native and multiple plugin owners");
    const u32 full_revision = registry.revision;
    check(ui_page_registry_register(&registry, {{4, 1}, utf8_literal("Overflow"), nullptr}).error ==
                  UiPageError::capacity && registry.count == 5 && registry.revision == full_revision &&
              !ui_page_registry_at(registry, 5),
          "full page registry rejects registration without allocating or changing existing pages");
    const auto selected = ui_page_registry_select(&registry, {2, 1});
    const u32 selected_revision = registry.revision;
    const auto same = ui_page_registry_select(&registry, {2, 1});
    check(selected.ok() && selected.changed && same.ok() && !same.changed &&
              registry.revision == selected_revision &&
              ui_page_key_equal(ui_page_registry_selected(registry)->key, {2, 1}),
          "page selection changes revision once and unchanged selection is a no-op");

    const u32 before_unload = registry.revision;
    check(ui_page_registry_unregister_owner(&registry, 2) == 2 && registry.count == 3 &&
              registry.revision == before_unload + 1 &&
              ui_page_key_equal(ui_page_registry_at(registry, 0)->key, {1, 1}) &&
              ui_page_key_equal(ui_page_registry_at(registry, 1)->key, {1, 2}) &&
              ui_page_key_equal(ui_page_registry_at(registry, 2)->key, {3, 1}) &&
              ui_page_key_equal(ui_page_registry_selected(registry)->key, {1, 2}) &&
              !storage[3].label.data && !storage[3].context && !storage[4].key.owner_id,
          "owner unload preserves survivor order selects the next page and erases removed pointers");
    const u32 after_unload = registry.revision;
    check(!ui_page_registry_unregister_owner(&registry, 2) &&
              !ui_page_registry_unregister_owner(&registry, 0) && registry.revision == after_unload &&
              ui_page_registry_register(&registry, plugin).ok() &&
              ui_page_key_equal(ui_page_registry_at(registry, 3)->key, plugin.key),
          "owner reload can reuse stable keys and appends without changing existing order");

    check(ui_page_registry_select(&registry, {3, 1}).ok() &&
              ui_page_registry_unregister(&registry, {1, 1}).ok() &&
              registry.selected_index == 1 &&
              ui_page_key_equal(ui_page_registry_selected(registry)->key, {3, 1}),
          "removing an earlier page preserves selected logical identity after compaction");
    check(ui_page_registry_select(&registry, plugin.key).ok() &&
              ui_page_registry_unregister(&registry, plugin.key).ok() &&
              ui_page_key_equal(ui_page_registry_selected(registry)->key, {3, 1}) &&
              ui_page_registry_unregister_owner(&registry, 3) == 1 &&
              ui_page_key_equal(ui_page_registry_selected(registry)->key, {1, 2}) &&
              ui_page_registry_unregister_owner(&registry, 1) == 1 && !registry.count &&
              !ui_page_registry_selected(registry) && registry.selected_index == kUiPageNoSelection,
          "last selected page falls back to previous and removing every page leaves no selection");

    check(ui_page_registry_register(&registry, plugin).ok() && registry.selected_index == 0,
          "first page after emptying restores automatic selection");
    registry.revision = ~0u;
    check(ui_page_registry_clear(&registry) && !registry.count && !storage[0].label.data &&
              !storage[0].context && !ui_page_registry_selected(registry) &&
              registry.revision == ~0u && !ui_page_registry_clear(&registry),
          "clear erases borrowed descriptors and revision saturates without wrapping");
}

} // namespace maicord
