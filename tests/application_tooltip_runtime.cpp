#include "application_account_presentation.hpp"
#include "application_friends.hpp"
#include "application_settings.hpp"
#include "application_state.hpp"
#include "application_tooltip.hpp"
#include "application_video.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/ui/pinned_messages_scene.hpp"
#include "maicord/ui/profile_scene.hpp"
#include "test_output.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
void test_reaction_tooltip_owner(void (*)(bool, const char*) noexcept) noexcept;
}

namespace maicord::tooltip_runtime_test {
using namespace app_detail;
u32 failures, text_builds;
bool settings;
bool video_ready;
bool fail_text_build;
UiRect viewport{0, 0, 1000, 700};
UiTooltipTarget video_target;
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}
void metric(const char* label, u64 value) noexcept {
    test_write_metric(label, value);
}
bool measure(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* value) noexcept {
    value->size = {minimum(request.constraint.width, request.text.bytes * 7.f), 18};
    return true;
}
bool failed_measure(void*, const UiTextMeasureRequest&, UiTextMeasureValue*) noexcept {
    return false;
}
void run() noexcept {
    g_app.window = CreateWindowExW(0, L"STATIC", L"MaiCord tooltip fixture", WS_POPUP, 0, 0, 1000, 700,
                                   nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(g_app.window && g_app.account_ui, "tooltip runtime: bounded hidden native host initializes");
    if (!g_app.window || !g_app.account_ui)
        return;
    g_app.renderer_ready = true;
    g_app.renderer.dpi = 96;
    g_app.account.status.profile_id = 4;
    g_app.plugin_account_scope_serial = 7;
    g_app.account_ui->active_channel_id = 8;
    test_reaction_tooltip_owner(check);
    UiTextMeasureRecord measurements[4]{};
    check(ui_text_measure_cache_initialize(&g_app.account_ui->text_measure_cache, {measurements, 4},
                                           measure, nullptr) &&
              ui_scene_initialize_bootstrap(&g_app.shell_scene) &&
              ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::guild_channel,
                                           kGuildChannelShellLayoutMetrics, 1000, 700),
          "tooltip runtime: actual shell widgets and shared measurement cache initialize");
    const auto gear =
        ui_widget_manager_find(g_app.shell_scene.widgets, static_cast<u32>(UiSemantic::settings) + 1);
    const auto* node = ui_widget_manager_layout_node(g_app.shell_scene.widgets, gear);
    check(node && !g_app.tooltip && !application_tooltip_next_wake(),
          "tooltip runtime: dormant host allocates no tooltip owner or deadline");
    if (!node)
        return;
    video_target = {{{&g_app.shell_scene.widgets, {0, g_app.shell_scene.widgets.storage[0].generation}},
                     7,
                     9,
                     1,
                     0x401},
                    {400, 200, 440, 240},
                    utf8_literal("Play")};
    application_tooltip_input(WM_MOUSEMOVE, 0, MAKELPARAM(400, 200));
    check(g_app.tooltip && !application_tooltip_next_wake() && !g_app.renderer.tooltip_scene->visible,
          "tooltip runtime: first eligible pointer retains one lazy owner before a typed video target "
          "exists");
    video_ready = true;
    const u64 published_at = clock_monotonic_milliseconds();
    check(application_tooltip_synchronize(published_at) &&
              application_tooltip_next_wake() == published_at + kUiTooltipPointerDelayMs &&
              application_tooltip_synchronize(published_at + kUiTooltipPointerDelayMs) &&
              g_app.renderer.tooltip_scene->visible && g_app.renderer.tooltip_scene->key.action == 0x401,
          "tooltip runtime: postdispatch typed video publication admits delayed tooltip without another "
          "mousemove");
    video_ready = false;
    check(application_tooltip_synchronize(published_at + kUiTooltipPointerDelayMs + 1) &&
              !g_app.renderer.tooltip_scene->visible && !application_tooltip_next_wake(),
          "tooltip runtime: retired typed video target immediately removes its borrowed hint");
    application_tooltip_release();
    const LPARAM point =
        MAKELPARAM(static_cast<short>((node->rectangle.left + node->rectangle.right) * .5f),
                   static_cast<short>((node->rectangle.top + node->rectangle.bottom) * .5f));
    application_tooltip_input(WM_MOUSEMOVE, 0, point);
    const u64 deadline = application_tooltip_next_wake();
    MEMORY_BASIC_INFORMATION region{};
    const bool queried =
        g_app.tooltip && VirtualQuery(g_app.tooltip, &region, sizeof(region)) == sizeof(region);
    check(deadline && queried && region.State == MEM_COMMIT && region.RegionSize <= 4096 &&
              application_tooltip_owner_bytes() <= 4096 && !g_app.renderer.tooltip_scene->visible,
          "tooltip runtime: real Settings gear lazily owns at most one committed page and exact pending "
          "delay");
    metric("tooltip_scene_bytes=", sizeof(UiTooltipScene));
    metric("tooltip_owner_bytes=", application_tooltip_owner_bytes());
    metric("tooltip_owner_committed_bytes=", queried ? region.RegionSize : 0);
    metric("tooltip_application_bytes=", sizeof(Application));
    metric("tooltip_renderer_bytes=", sizeof(D3D11Renderer));
    auto* owner = g_app.tooltip;
    check(application_tooltip_synchronize(deadline - 1) && !g_app.renderer.tooltip_scene->visible &&
              application_tooltip_synchronize(deadline) &&
              ui_tooltip_visible(*g_app.renderer.tooltip_scene) && !application_tooltip_next_wake() &&
              !GetCapture(),
          "tooltip runtime: delayed actual label appears without capture and leaves no idle wake");
    const u32 builds = text_builds;
    check(application_tooltip_synchronize(deadline + 1) && text_builds == builds && g_app.tooltip == owner,
          "tooltip runtime: steady tooltip retains text publication and owner");
    application_tooltip_input(WM_MOUSELEAVE, 0, 0);
    check(application_tooltip_synchronize(deadline + 2) && !g_app.renderer.tooltip_scene->visible &&
              !application_tooltip_next_wake(),
          "tooltip runtime: leave hides immediately before borrowed source teardown");
    g_app.context_focus = &g_app.shell_scene.widgets;
    g_app.keyboard_focus_visible = true;
    ui_widget_manager_focus(&g_app.shell_scene.widgets, gear);
    application_tooltip_input(WM_KEYDOWN, VK_TAB, 0);
    const u64 keyboard_deadline = application_tooltip_next_wake();
    check(keyboard_deadline && application_tooltip_synchronize(keyboard_deadline) &&
              g_app.renderer.tooltip_scene->visible,
          "tooltip runtime: real keyboard-visible shell focus owns tooltip separately from pointer");
    settings = true;
    check(application_tooltip_synchronize(keyboard_deadline + 1) &&
              !g_app.renderer.tooltip_scene->visible && !application_tooltip_next_wake(),
          "tooltip runtime: opaque Settings excludes underlying focused tooltip and cancels delay");
    settings = false;
    application_tooltip_input(WM_MOUSEMOVE, 0, point);
    auto* mutable_gear = ui_widget_manager_resolve(&g_app.shell_scene.widgets, gear);
    mutable_gear->flags &= static_cast<u16>(~ui_widget_enabled);
    const auto fallback = ui_widget_manager_hit_test(g_app.shell_scene.widgets,
                                                     (node->rectangle.left + node->rectangle.right) * .5f,
                                                     (node->rectangle.top + node->rectangle.bottom) * .5f);
    const auto* fallback_widget = ui_widget_manager_resolve(g_app.shell_scene.widgets, fallback);
    check(fallback_widget && fallback_widget->id == static_cast<u32>(UiSemantic::channel_footer) + 1,
          "tooltip runtime: disabled gear fixture proves enabled footer fallback at unchanged pointer");
    check(application_tooltip_synchronize(application_tooltip_next_wake()) &&
              !g_app.renderer.tooltip_scene->visible && !application_tooltip_next_wake(),
          "tooltip runtime: disabled shipping control cannot show a tooltip");
    mutable_gear->flags |= ui_widget_enabled;
    application_tooltip_clear();
    UiWidget composer_widgets[2]{};
    UiLayoutNode composer_nodes[2]{};
    auto& composer = g_app.message_composer_scene;
    auto root_style = ui_layout_style();
    root_style.width = ui_layout_fill();
    root_style.height = ui_layout_fill();
    auto button_style = ui_layout_style();
    button_style.width = ui_layout_fixed(32);
    button_style.height = ui_layout_fixed(32);
    check(ui_widget_manager_initialize(&composer.widgets, {composer_widgets, 2}, {composer_nodes, 2}),
          "tooltip runtime: borrowed composer widgets initialize without owned bank");
    const auto composer_root = ui_widget_manager_add(
        &composer.widgets, ui_widget_invalid_handle(),
        {1, UiWidgetKind::container, ui_widget_none, {utf8_literal("Composer"), root_style}});
    const auto emoji =
        composer_root.ok()
            ? ui_widget_manager_add(&composer.widgets, composer_root.handle,
                                    {2,
                                     UiWidgetKind::button,
                                     ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable,
                                     {utf8_literal("Emoji"), button_style}})
            : UiWidgetResult{};
    check(emoji.ok() && ui_widget_manager_layout_rectangle(&composer.widgets, {500, 500, 900, 600}).ok(),
          "tooltip runtime: borrowed composer button has current clipped layout");
    composer.active = true;
    composer.emoji = emoji.handle;
    g_app.context_focus = &composer.widgets;
    g_app.keyboard_focus_visible = true;
    ui_widget_manager_focus(&composer.widgets, emoji.handle);
    application_tooltip_input(WM_KEYDOWN, VK_TAB, 0);
    const u64 composer_deadline = application_tooltip_next_wake();
    check(composer_deadline && application_tooltip_synchronize(composer_deadline) &&
              g_app.renderer.tooltip_scene->key.owner.manager == &composer.widgets &&
              g_app.renderer.tooltip_scene->visible,
          "tooltip runtime: typed composer Emoji focus resolves borrowed source and copied label");
    ++composer.reply_message_id;
    check(application_tooltip_synchronize(composer_deadline + 1) &&
              !g_app.renderer.tooltip_scene->visible &&
              application_tooltip_next_wake() == composer_deadline + 1 + kUiTooltipKeyboardDelayMs,
          "tooltip runtime: semantic composer revision requires a fresh delayed admission");
    check(application_tooltip_synchronize(application_tooltip_next_wake()),
          "tooltip runtime: revised source can show after its new deadline");
    application_tooltip_input(WM_KEYDOWN, VK_RETURN, 0);
    check(!g_app.renderer.tooltip_scene->visible && !application_tooltip_next_wake() &&
              application_tooltip_synchronize(composer_deadline + 1000) && !application_tooltip_next_wake(),
          "tooltip runtime: keyboard activation dismisses same semantic source without automatic rearm");
    application_tooltip_clear();
    composer.active = false;
    composer.widgets = {};
    UiSearchScene search{};
    UiSearchSource search_source{nullptr, 0, 1, [](void*, u32, UiSearchRow*) noexcept { return false; }};
    UiSearchContent search_content{};
    search_content.edit.active = true;
    search_content.panel = true;
    search_content.phase = UiSearchPhase::ready;
    search_content.revision = 1;
    check(ui_search_scene_update(&search, &search_source, &search_content, {0, 60, 700, 650},
                                 {300, 0, 700, 60}),
          "tooltip runtime: actual search scene initializes bounded current controls");
    g_app.renderer.search_scene = &search;
    const auto* filter_node = ui_widget_manager_layout_node(search.widgets, search.buttons[2]);
    check(filter_node != nullptr, "tooltip runtime: search filter has a real clipped target");
    if (filter_node) {
        application_tooltip_input(
            WM_MOUSEMOVE, 0,
            MAKELPARAM(
                static_cast<short>((filter_node->rectangle.left + filter_node->rectangle.right) * .5f),
                static_cast<short>((filter_node->rectangle.top + filter_node->rectangle.bottom) * .5f)));
        const u64 search_deadline = application_tooltip_next_wake();
        check(search_deadline && application_tooltip_synchronize(search_deadline) &&
                  g_app.renderer.tooltip_scene->visible &&
                  g_app.renderer.tooltip_scene->key.owner.manager == &search.widgets &&
                  g_app.renderer.tooltip_scene->key.action ==
                      0x400u + static_cast<u32>(UiSearchAction::filters),
              "tooltip runtime: delayed Search filters hint belongs to the exact search widget");
    }
    video_ready = true;
    application_tooltip_input(WM_MOUSEMOVE, 0, MAKELPARAM(400, 200));
    check(!g_app.renderer.tooltip_scene->visible && !application_tooltip_next_wake(),
          "tooltip runtime: opaque blank search panel cannot inherit a covered video hint");
    video_ready = false;
    g_app.context_focus = &search.widgets;
    g_app.keyboard_focus_visible = true;
    ui_widget_manager_focus(&search.widgets, search.buttons[0]);
    application_tooltip_input(WM_KEYDOWN, VK_TAB, 0);
    const u64 search_keyboard_deadline = application_tooltip_next_wake();
    check(search_keyboard_deadline && application_tooltip_synchronize(search_keyboard_deadline) &&
              g_app.renderer.tooltip_scene->visible &&
              g_app.renderer.tooltip_scene->key.owner.manager == &search.widgets &&
              g_app.renderer.tooltip_scene->key.action == 0x400u + static_cast<u32>(UiSearchAction::submit),
          "tooltip runtime: search keyboard owner resolves its own submit hint before shell controls");
    ++search_source.revision;
    check(ui_search_scene_update(&search, &search_source, &search_content, {0, 60, 700, 650},
                                 {300, 0, 700, 60}) &&
              application_tooltip_synchronize(search_keyboard_deadline + 1) &&
              !g_app.renderer.tooltip_scene->visible && application_tooltip_next_wake(),
          "tooltip runtime: changed search source retires the old hint and requires a new delay");
    application_tooltip_clear();
    g_app.renderer.search_scene = nullptr;
    ui_search_scene_release(&search);
    UiMediaPickerItem favorite_item{};
    favorite_item.id = 1;
    favorite_item.kind = UiMediaPickerItemKind::gif;
    favorite_item.label = utf8_literal("Fixture GIF");
    favorite_item.detail = utf8_literal("GIF details");
    favorite_item.enabled = favorite_item.favoritable = true;
    favorite_item.width = 160;
    favorite_item.height = 90;
    UiMediaPickerSource favorite_source{
        &favorite_item, 4, 1, 1, 1, {}, [](void* context, u32 index, UiMediaPickerItem* output) noexcept {
            if (index || !output)
                return false;
            *output = *static_cast<UiMediaPickerItem*>(context);
            return true;
        }};
    UiMediaPickerContent favorite_content{};
    favorite_content.account_scope = 4;
    favorite_content.query_generation = 1;
    favorite_content.revision = 1;
    favorite_content.edit.active = true;
    favorite_content.available_tabs = 7;
    favorite_content.kind = UiMediaPickerKind::gif;
    favorite_content.phase = UiMediaPickerPhase::ready;
    UiMediaPickerScene favorite_scene{};
    check(ui_media_picker_scene_update(&favorite_scene, &favorite_source, &favorite_content,
                                       {0, 0, 900, 700}, {800, 650, 840, 690}),
          "tooltip Favorites: native gallery fixture initializes");
    g_app.renderer.media_picker_scene = &favorite_scene;
    if (!favorite_scene.active || !favorite_scene.cell_count)
        return;
    const auto cell_rect = favorite_scene.projection->cells[0].rectangle;
    ui_media_picker_scene_hover(&favorite_scene, (cell_rect.left + cell_rect.right) * .5f,
                                (cell_rect.top + cell_rect.bottom) * .5f, clock_monotonic_milliseconds());
    UiRect star_rect{};
    check(ui_media_picker_scene_favorite(favorite_scene, 0, &star_rect),
          "tooltip Favorites: hover exposes exact clipped action subregion");
    const auto star_point = MAKELPARAM(static_cast<short>((star_rect.left + star_rect.right) * .5f),
                                       static_cast<short>((star_rect.top + star_rect.bottom) * .5f));
    const auto hint = [&](Utf8View expected) noexcept {
        ui_media_picker_scene_hover(&favorite_scene, (star_rect.left + star_rect.right) * .5f,
                                    (star_rect.top + star_rect.bottom) * .5f,
                                    clock_monotonic_milliseconds());
        application_tooltip_input(WM_MOUSEMOVE, 0, star_point);
        const u64 at = application_tooltip_next_wake();
        return at && application_tooltip_synchronize(at) && g_app.renderer.tooltip_scene->visible &&
               g_app.renderer.tooltip_scene->key.action ==
                   0x600u + static_cast<u32>(UiMediaPickerAction::favorite) &&
               utf8_equal({g_app.renderer.tooltip_scene->text, g_app.renderer.tooltip_scene->text_bytes},
                          expected);
    };
    check(hint(utf8_literal("Add to favorites (F)")),
          "tooltip Favorites: actual shared delayed hint describes Add action and keyboard shortcut");
    application_tooltip_clear();
    favorite_item.favorite = true;
    ++favorite_source.revision;
    check(ui_media_picker_scene_update(&favorite_scene, &favorite_source, &favorite_content,
                                       {0, 0, 900, 700}, {800, 650, 840, 690}) &&
              hint(utf8_literal("Remove from favorites (F)")),
          "tooltip Favorites: confirmed state changes exact action hint without borrowing old label");
    application_tooltip_clear();
    favorite_item.favorite_pending = true;
    ++favorite_source.revision;
    check(ui_media_picker_scene_update(&favorite_scene, &favorite_source, &favorite_content,
                                       {0, 0, 900, 700}, {800, 650, 840, 690}) &&
              hint(utf8_literal("Updating favorite")),
          "tooltip Favorites: pending state explains disabled action through shared hint");
    application_tooltip_clear();
    g_app.renderer.media_picker_scene = nullptr;
    ui_media_picker_scene_release(&favorite_scene);
    g_app.context_focus = &g_app.shell_scene.widgets;
    application_tooltip_input(WM_MOUSEMOVE, 0, point);
    const u64 publication_failure_at = application_tooltip_next_wake();
    const u32 builds_before_failure = text_builds;
    fail_text_build = true;
    check(publication_failure_at && !application_tooltip_synchronize(publication_failure_at) &&
              !application_tooltip_next_wake() && !g_app.renderer.tooltip_scene->visible &&
              text_builds == builds_before_failure + 1 && g_app.frame_pending &&
              !g_app.renderer.retained_presentation.valid,
          "tooltip runtime: failed native text publication retires visible revision and overdue wake");
    bool retired = true;
    g_app.frame_pending = false;
    for (u32 attempt = 0; attempt < 8; ++attempt) {
        application_tooltip_input(WM_MOUSEMOVE, 0, point);
        retired = retired && application_tooltip_synchronize(publication_failure_at + attempt + 1) &&
                  !application_tooltip_next_wake() && !g_app.renderer.tooltip_scene->visible &&
                  text_builds == builds_before_failure + 1 && !g_app.frame_pending;
    }
    check(retired, "tooltip runtime: identical failed source cannot request repeated native text builds "
                   "through synchronize or pointer observations");
    fail_text_build = false;
    application_tooltip_clear();
    application_tooltip_input(WM_MOUSEMOVE, 0, point);
    const u64 pre_resize_at = application_tooltip_next_wake();
    application_tooltip_input(WM_SIZE, 0, 0);
    check(pre_resize_at && application_tooltip_synchronize(pre_resize_at + 1) &&
              !application_tooltip_next_wake() && !g_app.renderer.tooltip_scene->visible,
          "tooltip runtime: resize dismisses pending old-anchor delay without later overdue revival");
    application_tooltip_input(WM_MOUSEMOVE, 0, point);
    const u64 visible_before_resize_at = application_tooltip_next_wake();
    check(visible_before_resize_at && application_tooltip_synchronize(visible_before_resize_at),
          "tooltip runtime: fresh input may recover after explicit failure and resize retirement");
    viewport = {0, 0, 900, 600};
    check(application_tooltip_synchronize(visible_before_resize_at + 1) &&
              !g_app.renderer.tooltip_scene->visible && !application_tooltip_next_wake() &&
              application_tooltip_synchronize(visible_before_resize_at + 1000) &&
              !application_tooltip_next_wake(),
          "tooltip runtime: changed viewport retires visible borrowed anchor until fresh observation");
    viewport = {0, 0, -1, 600};
    const u32 builds_before_bad_viewport = text_builds;
    check(!application_tooltip_synchronize(visible_before_resize_at + 1001) &&
              application_tooltip_synchronize(visible_before_resize_at + 1002) &&
              !application_tooltip_next_wake() && !g_app.renderer.tooltip_scene->visible &&
              text_builds == builds_before_bad_viewport,
          "tooltip runtime: invalid viewport failure is acknowledged once without text retry or wake");
    viewport = {0, 0, 1000, 700};
    check(application_tooltip_synchronize(visible_before_resize_at + 1003),
          "tooltip runtime: changed valid viewport recovers from failed layout admission");
    application_tooltip_clear();
    application_tooltip_input(WM_MOUSEMOVE, 0, point);
    const u64 failed_deadline = application_tooltip_next_wake();
    ui_text_measure_cache_invalidate(&g_app.account_ui->text_measure_cache);
    g_app.account_ui->text_measure_cache.backend = failed_measure;
    check(application_tooltip_synchronize(failed_deadline) && !application_tooltip_next_wake() &&
              application_tooltip_synchronize(failed_deadline + 1) && !application_tooltip_next_wake(),
          "tooltip runtime: measurement failure suppresses same source without a due wake loop");
    application_tooltip_clear();
    ui_scene_release(&g_app.shell_scene);
    check(application_tooltip_synchronize(failed_deadline + 2) && !application_tooltip_next_wake(),
          "tooltip runtime: guarded manager release never revives cached source authority");
    application_tooltip_release();
    check(!g_app.tooltip && !g_app.renderer.tooltip_scene && !application_tooltip_next_wake(),
          "tooltip runtime: final release unbinds owner before freeing its page");
    g_app.account_ui->text_measure_cache.backend = measure;
    ui_text_measure_cache_invalidate(&g_app.account_ui->text_measure_cache);
    auto pin_metrics = kGuildChannelShellLayoutMetrics;
    pin_metrics.flags |= ui_shell_layout_pins;
    ++pin_metrics.revision;
    check(ui_scene_initialize_bootstrap(&g_app.shell_scene) &&
              ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::guild_channel, pin_metrics, 1000, 700),
          "tooltip Pins: real retained header launcher initializes under selected-channel flag");
    const auto launcher = ui_scene_header_geometry(g_app.shell_scene).pins;
    application_tooltip_input(WM_MOUSEMOVE, 0,
        MAKELPARAM(static_cast<short>((launcher.left + launcher.right) * .5f),
                   static_cast<short>((launcher.top + launcher.bottom) * .5f)));
    const u64 pins_deadline = application_tooltip_next_wake();
    check(pins_deadline && application_tooltip_synchronize(pins_deadline) &&
              g_app.renderer.tooltip_scene->visible &&
              utf8_equal({g_app.renderer.tooltip_scene->text, g_app.renderer.tooltip_scene->text_bytes},
                         utf8_literal("Pinned messages")),
          "tooltip Pins: native header launcher owns delayed Pinned messages hint");
    application_tooltip_clear();
    UiPinnedMessagesScene pins_scene{};
    bool pin_scope_current = true;
    UiPinnedMessagesSource pins_source{&pin_scope_current, 1, 1,
        [](void*, u32, UiPinnedMessagesRow* row) noexcept {
            *row = {8, 99, 123, utf8_literal("Original author"), utf8_literal("Pinned today"),
                    utf8_literal("Original native pinned note"), {}, {}, 48, true, false};
            return true;
        }, [](void* context) noexcept { return *static_cast<bool*>(context); }};
    UiPinnedMessagesContent pins_content{};
    pins_content.channel_id = 8;
    pins_content.channel = utf8_literal("# native-pins");
    pins_content.revision = 1;
    pins_content.phase = UiPinnedMessagesPhase::ready;
    check(ui_pinned_messages_scene_update(&pins_scene, &pins_source, &pins_content, {0, 60, 700, 650}),
          "tooltip Pins: accepted native page owns retained jump and unpin controls");
    g_app.renderer.pinned_messages_scene = &pins_scene;
    g_app.context_focus = &pins_scene.widgets;
    g_app.keyboard_focus_visible = true;
    check(ui_pinned_messages_scene_focus(&pins_scene, {UiPinnedMessagesAction::unpin, 8, 99}),
          "tooltip Pins: permission-aware Unpin receives exact keyboard owner");
    application_tooltip_input(WM_KEYDOWN, VK_TAB, 0);
    const u64 unpin_deadline = application_tooltip_next_wake();
    check(unpin_deadline && application_tooltip_synchronize(unpin_deadline) &&
              g_app.renderer.tooltip_scene->visible &&
              g_app.renderer.tooltip_scene->key.owner.manager == &pins_scene.widgets &&
              utf8_equal({g_app.renderer.tooltip_scene->text, g_app.renderer.tooltip_scene->text_bytes},
                         utf8_literal("Unpin message")),
          "tooltip Pins: keyboard Unpin hint belongs to current accepted message action");
    pin_scope_current = false;
    check(application_tooltip_synchronize(unpin_deadline + 1) && !g_app.renderer.tooltip_scene->visible &&
              !application_tooltip_next_wake(),
          "tooltip Pins: retired account/channel page immediately hides borrowed row hint");
    pin_scope_current = true;
    pins_source.count = 0;
    ++pins_source.revision;
    ++pins_content.revision;
    check(ui_pinned_messages_scene_update(&pins_scene, &pins_source, &pins_content, pins_scene.viewport),
          "tooltip Pins: empty native page keeps its opaque panel input boundary");
    video_ready = true;
    application_tooltip_input(WM_MOUSEMOVE, 0, MAKELPARAM(400, 200));
    check(!g_app.renderer.tooltip_scene->visible && !application_tooltip_next_wake(),
          "tooltip Pins: empty opaque panel cannot inherit covered message or video hints");
    video_ready = false;
    g_app.renderer.pinned_messages_scene = nullptr;
    g_app.context_focus = nullptr;
    ui_pinned_messages_scene_release(&pins_scene);

    // Over the profile card only its own hints show: an activity's artwork
    // offers its hover text.
    {
        static UserStore users{};
        static UserProfile profile{};
        static ProfileScene card{};
        constexpr auto presence = utf8_literal(R"({"activities":[{"type":0,"name":"Mossrun","application_id":"5",
            "assets":{"large_image":"6","large_text":"High Ridge"}}]})");
        u32 structurals[256];
        UserActivitySet set{};
        bool has = false;
        profile.user_id = 42;
        const auto scan = json_scan_structurals_avx1(presence, {structurals, 256});
        const bool opened = scan.ok() &&
                            user_activities_decode({presence, {structurals, scan.count}},
                                                   {presence, 0, JsonValueKind::object}, 42, &set, &has) &&
                            user_activity_upsert(&users.activities, set) &&
                            profile_scene_open(&card, &profile, nullptr, &users, 1, {300, 100, 340, 140}, viewport);
        const UiLayoutNode* art = nullptr;
        for (u32 i = 0; opened && i < card.part_count; ++i)
            if (card.parts[i].media == ProfileMediaKind::activity_large)
                art = ui_widget_manager_layout_node(card.widgets, card.parts[i].widget);
        g_app.renderer.profile_scene = &card;
        bool shown = false;
        if (art) {
            application_tooltip_input(WM_MOUSEMOVE, 0,
                                      MAKELPARAM(static_cast<short>((art->rectangle.left + art->rectangle.right) * .5f),
                                                 static_cast<short>((art->rectangle.top + art->rectangle.bottom) * .5f)));
            const u64 at = application_tooltip_next_wake();
            shown = at && application_tooltip_synchronize(at) && g_app.renderer.tooltip_scene->visible &&
                    g_app.renderer.tooltip_scene->key.action == 0x5800u &&
                    utf8_equal({g_app.renderer.tooltip_scene->text, g_app.renderer.tooltip_scene->text_bytes},
                               utf8_literal("High Ridge"));
        }
        check(shown, "tooltip Profile: activity artwork shows its hover text over the card");
        application_tooltip_clear();
        g_app.renderer.profile_scene = nullptr;
        profile_scene_release(&card);
        user_activity_release(&users.activities);
    }
    ui_scene_release(&g_app.shell_scene);
    application_tooltip_release();
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
    DestroyWindow(g_app.window);
    g_app.window = nullptr;
}
} // namespace maicord::tooltip_runtime_test

namespace maicord {
bool renderer_rebuild_content_text(D3D11Renderer*, const Theme&) noexcept {
    ++tooltip_runtime_test::text_builds;
    return !tooltip_runtime_test::fail_text_build;
}
namespace app_detail {
bool application_settings_active() noexcept {
    return tooltip_runtime_test::settings;
}
bool application_friends_active() noexcept {
    return false;
}
bool application_thread_browser_read_blocked() noexcept { return false; }
UiThreadBrowserScene* application_thread_browser_scene() noexcept { return nullptr; }
bool application_friends_requests_active() noexcept {
    return false;
}
UiRect application_viewport_dips() noexcept {
    return tooltip_runtime_test::viewport;
}
// The caption's own tooltip has no pending deadline here.
u64 application_caption_tooltip_wake() noexcept {
    return 0;
}
bool application_video_tooltip_target(UiPoint point, bool keyboard, u64 scope,
                                      UiTooltipTarget* target) noexcept {
    if (!tooltip_runtime_test::video_ready || keyboard || point.x != 400 || point.y != 200 || !target)
        return false;
    *target = tooltip_runtime_test::video_target;
    target->key.scope = scope;
    return true;
}
Utf8View application_guild_rail_label(const GuildViewRow&, Slice<char>) noexcept {
    return {};
}
Utf8View application_voice_tooltip(float, float, UiRect*, u32*) noexcept {
    return {};
}
u64 application_plugins_slot_revision() noexcept {
    return 0;
}
struct ApplicationPluginSurfaceHit;
bool application_plugins_surface_hit(float, float, ApplicationPluginSurfaceHit*) noexcept {
    return false;
}
Utf8View application_plugins_hover_label(u32) noexcept {
    return {};
}
} // namespace app_detail
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_tooltip_runtime_entry() noexcept {
    maicord::tooltip_runtime_test::run();
    constexpr char passed[] = "PASS tooltip runtime\n", failed[] = "FAIL tooltip runtime\n";
    const bool failure = maicord::tooltip_runtime_test::failures != 0;
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failure ? failed : passed, sizeof(passed) - 1, &written,
              nullptr);
    ExitProcess(failure ? 1 : 0);
}
