#include "maicord/app/plugins/dispatch.hpp"
#include "maicord/ui/chat/messages/scene.hpp"

// With no plugin enabled, rendering a message scene reaches no plugin hook: the dispatcher is
// never entered, every hook has no subscriber, and it commits no memory.
namespace maicord {
namespace {
constexpr u32 widget_count = kMessageSceneWidgetsPerRow + 1;
struct Fixture {
    MessageRecord record[1];
    u32 order[1];
    u8 text[64];
    u32 heights[1];
    u64 fenwick[1];
    UiWidget widgets[widget_count];
    UiLayoutNode nodes[widget_count];
    MessageStore store;
    UiVariableList list;
    MessageTimeline timeline;
    MessageScene scene;
};
Fixture fixture;
} // namespace

void test_plugin_dispatch_scene(void (*check)(bool, const char*) noexcept) noexcept {
    Fixture& f = fixture;
    const bool ready = message_store_initialize(&f.store, {f.record, 1}, {f.order, 1}, {f.text, sizeof(f.text)}) &&
        message_store_upsert(&f.store, {1, 7, 9, 0, 0, utf8_literal("hello"), 0}).ok() &&
        ui_variable_list_initialize(&f.list, {f.heights, 1}, {f.fenwick, 1}, 1, ui_extent_units(57)) &&
        message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57), ui_extent_units(1000), false) &&
        message_scene_initialize_owned(&f.scene, &f.timeline, 1, 1, {f.widgets, widget_count}, {f.nodes, widget_count});
    check(ready, "plugin_dispatch_scene_fixture");
    if (!ready) return;
    const PluginDispatchStats before = plugin_dispatch_stats();
    bool rendered = true;
    const float widths[]{740.0f, 520.0f, 900.0f, 740.0f};
    for (const float width : widths) {
        rendered = rendered && message_scene_update(&f.scene, {0, 0, width, 1000}).ok() &&
                   message_scene_find_row(f.scene, 0) != nullptr;
        // The hook points a composer submit reaches cost one branch each.
        rendered = rendered && !plugin_hook_subscribed(PluginHook::pre_send) &&
                   !plugin_hook_subscribed(PluginHook::pre_edit);
    }
    const PluginDispatchStats after = plugin_dispatch_stats();
    check(rendered, "plugin_dispatch_scene_renders_without_subscribers");
    check(after.calls == before.calls && after.strikes == before.strikes && after.allocations == before.allocations &&
              after.allocations == 0,
          "plugin_dispatch_scene_render_calls_no_plugin_and_allocates_nothing");
    message_scene_release(&f.scene);
}

} // namespace maicord
