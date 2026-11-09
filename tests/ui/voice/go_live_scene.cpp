#include "maicord/ui/voice/go_live_scene.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/ui/theme/theme.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

bool near(float a, float b, float tolerance = .05f) noexcept {
    return a - b <= tolerance && b - a <= tolerance;
}
UiPoint middle(const UiRect& r) noexcept {
    return {(r.left + r.right) * .5f, (r.top + r.bottom) * .5f};
}
bool inside(const UiRect& inner, const UiRect& outer) noexcept {
    return inner.left >= outer.left - .01f && inner.top >= outer.top - .01f && inner.right <= outer.right + .01f &&
           inner.bottom <= outer.bottom + .01f;
}
// Text drawn by the dialog, for the copy checks.
struct Drawn {
    UiTextCommand commands[160];
    char owned[4096];
    UiTextCommandStream stream;
    bool collect(const UiGoLiveScene& s) noexcept {
        UiTextOutput output{&stream};
        return ui_text_command_stream_initialize(&stream, {commands, 160}, {owned, sizeof(owned)}) &&
               ui_go_live_scene_text_output(s, kDefaultTheme, &output);
    }
    bool has(Utf8View text) const noexcept {
        for (u32 i = 0; i < stream.count; ++i)
            if (stream.storage[i].text.bytes == text.bytes &&
                bytes_equal(stream.storage[i].text.data, text.data, text.bytes))
                return true;
        return false;
    }
};

UiGoLiveSource sources[kUiGoLiveMaximumSources];
UiGoLiveContent picker(u32 count, u32 revision) noexcept {
    static const char* const kNames[]{"Notes", "Browser", "Editor", "Terminal"};
    for (u32 i = 0; i < count; ++i) {
        const char* name = kNames[i % 4];
        u32 bytes = 0;
        while (name[bytes])
            ++bytes;
        sources[i] = {1000 + i, {name, bytes}, {}, 0, 0, false};
    }
    UiGoLiveContent c{};
    c.title = utf8_literal("Share a screen, window, or app");
    c.summary = utf8_literal("Gaming");
    c.detail = utf8_literal("Smoother video \xE2\x80\xA2 720p 30fps");
    c.sources = {sources, count};
    c.revision = revision;
    c.mode = UiGoLiveMode::picker;
    c.nitro = true;
    return c;
}

void picker_cases(Check check) noexcept {
    static UiGoLiveScene s;
    static Drawn drawn;
    const UiRect viewport{0, 0, 1280, 800};
    auto c = picker(3, 1);
    check(ui_go_live_scene_update(&s, c, viewport) && s.active && s.source_count == 3 && s.opens == 1,
          "go live dialog: the picker lays out its sources");
    check(inside(s.panel, viewport) && near(s.panel.left - viewport.left, viewport.right - s.panel.right) &&
              ui_rect_width(s.panel) <= 860 && ui_rect_has_area(s.upsell) && s.upsell.bottom == s.panel.bottom,
          "go live dialog: centred, at most 860 DIP wide, the HD line at its foot without Nitro");
    bool tiles = true;
    for (u32 i = 0; i < 3; ++i) {
        const UiPoint p = middle(ui_widget_manager_rectangle(s.widgets, s.tiles[i]));
        const auto target = ui_go_live_scene_hit_test(s, p.x, p.y);
        tiles = tiles && target.action == UiGoLiveAction::source && target.index == i && target.id == 1000 + i;
    }
    check(tiles, "go live dialog: each source tile starts its source");
    const auto outside = ui_go_live_scene_hit_test(s, 4, 4);
    const UiPoint tab = middle(s.tab_rectangles[1]);
    const auto screens = ui_go_live_scene_hit_test(s, tab.x, tab.y);
    const UiPoint options = middle(s.options_rectangle);
    const UiPoint upsell = middle(s.upsell);
    check(outside.action == UiGoLiveAction::close && screens.action == UiGoLiveAction::tab && screens.index == 1 &&
              ui_go_live_scene_hit_test(s, options.x, options.y).action == UiGoLiveAction::options &&
              ui_go_live_scene_hit_test(s, upsell.x, upsell.y).action == UiGoLiveAction::nitro,
          "go live dialog: the scrim closes; tabs, Options and the HD line are targets");
    check(drawn.collect(s) && drawn.has(utf8_literal("Applications")) && drawn.has(utf8_literal("Entire Screen")) &&
              drawn.has(utf8_literal("Notes")) && drawn.has(utf8_literal("Gaming")) &&
              drawn.has(utf8_literal("Stream in HD resolution with Nitro")) && !drawn.has(utf8_literal("Share Screen")),
          "go live dialog: tabs, source names, the Stream Mode summary and the HD line are drawn");

    // Thumbnails: letterboxed into the 16:9 box, dimmed under the hover call to action.
    UiRect picture{}, clip{};
    bool dim = true;
    check(!ui_go_live_scene_thumbnail(s, 0, &picture, &clip, &dim), "go live dialog: no thumbnail before the first");
    sources[0].thumbnail = media_asset_local_draft_preview(77, 1, MediaAttachmentFormat::png);
    sources[0].width = 300;
    sources[0].height = 300;
    c.revision = 2;
    ui_go_live_scene_update(&s, c, viewport);
    const UiRect tile = ui_widget_manager_rectangle(s.widgets, s.tiles[0]);
    const bool shown = ui_go_live_scene_thumbnail(s, 0, &picture, &clip, &dim);
    check(shown && near(ui_rect_width(picture), ui_rect_height(picture)) && inside(picture, tile) && !dim &&
              near(middle(picture).x, middle(tile).x),
          "go live dialog: a square window's thumbnail is letterboxed in its 16:9 tile");
    bool changed = false;
    const UiPoint hover = middle(tile);
    check(ui_go_live_scene_set_hover(&s, hover.x, hover.y, &changed) && changed &&
              ui_go_live_scene_thumbnail(s, 0, &picture, &clip, &dim) && dim && drawn.collect(s) &&
              drawn.has(utf8_literal("Share Screen")),
          "go live dialog: a hovered source dims under \"Share Screen\"");

    // Keyboard: the tab row, then the grid by arrows; Up from the first row returns to the tabs.
    check(ui_widget_handle_matches(s.widgets.focused, s.tabs[0]), "go live dialog: focus starts on Applications");
    check(ui_go_live_scene_focus_arrow(&s, 1, 0) && ui_widget_handle_matches(s.widgets.focused, s.tabs[1]) &&
              ui_go_live_scene_focus_arrow(&s, -1, 0) && ui_go_live_scene_focus_arrow(&s, 0, 1) &&
              ui_widget_handle_matches(s.widgets.focused, s.tiles[0]) && ui_go_live_scene_focus_arrow(&s, 1, 0) &&
              ui_widget_handle_matches(s.widgets.focused, s.tiles[1]) &&
              ui_go_live_scene_target(s, s.widgets.focused).id == 1001 && ui_go_live_scene_focus_arrow(&s, 0, -1) &&
              ui_widget_handle_matches(s.widgets.focused, s.tabs[0]),
          "go live dialog: arrows move across the tabs and the source grid");
    bool cycled = true;
    for (u32 i = 0; i < 16 && cycled; ++i)
        cycled = ui_go_live_scene_focus_next(&s, false);
    check(cycled && s.widgets.focused.generation && ui_go_live_scene_focus_next(&s, true),
          "go live dialog: Tab cycles through the dialog's controls");

    // Many sources scroll; a focused tile below the fold scrolls into view, and the refresh keeps its focus.
    c = picker(20, 3);
    check(ui_go_live_scene_update(&s, c, viewport) && s.content_height > ui_rect_height(s.body) &&
              ui_go_live_scene_scroll(&s, 200) && s.scroll > 0 && ui_go_live_scene_scroll(&s, -1e6f) && s.scroll == 0,
          "go live dialog: a long source list scrolls within the body");
    ui_widget_manager_focus(&s.widgets, s.tabs[0]);
    ui_go_live_scene_focus_arrow(&s, 0, 1);
    for (u32 i = 0; i < 6; ++i)
        ui_go_live_scene_focus_arrow(&s, 0, 1);
    u32 focused = kUiGoLiveMaximumSources;
    for (u32 i = 0; i < s.source_count; ++i)
        if (ui_widget_handle_matches(s.widgets.focused, s.tiles[i]))
            focused = i;
    const UiRect focus_rect = ui_widget_manager_rectangle(s.widgets, s.tiles[focused < 20 ? focused : 0]);
    c.revision = 4;
    check(focused < 20 && focused >= s.columns && s.scroll > 0 && inside(focus_rect, s.body) &&
              ui_go_live_scene_update(&s, c, viewport) && ui_widget_handle_matches(s.widgets.focused, s.tiles[focused]),
          "go live dialog: a focused tile scrolls into view and keeps focus across refreshes");
    ui_go_live_scene_release(&s);
}

void system_cases(Check check) noexcept {
    static UiGoLiveScene s;
    static Drawn drawn;
    const UiRect viewport{0, 0, 1280, 800};
    UiGoLiveContent c{};
    c.title = utf8_literal("Share a screen, window, or app");
    c.summary = utf8_literal("Gaming");
    c.mode = UiGoLiveMode::system_picker;
    c.kind = UiGoLiveSystemKind::screen;
    c.revision = 1;
    check(ui_go_live_scene_update(&s, c, viewport) && !s.source_count &&
              ui_widget_handle_matches(s.widgets.focused, s.kinds[0]),
          "go live dialog (system picker): the two cards, focus on the first");
    const UiPoint window = middle(s.kind_rectangles[1]);
    const UiPoint select = middle(s.select_rectangle);
    const auto kind = ui_go_live_scene_hit_test(s, window.x, window.y);
    check(kind.action == UiGoLiveAction::kind && kind.index == 2 &&
              ui_go_live_scene_hit_test(s, select.x, select.y).action == UiGoLiveAction::select,
          "go live dialog (system picker): \"Share a window\" and Make Selection are targets");
    check(drawn.collect(s) && drawn.has(utf8_literal("Share entire screen")) && drawn.has(utf8_literal("Share a window")) &&
              drawn.has(utf8_literal("Make Selection")) &&
              drawn.has(utf8_literal("Click the Make Selection button, then choose a window, application, or screen to "
                                     "Go Live using your system menu.")),
          "go live dialog (system picker): the official copy");
    c.busy = true;
    c.revision = 2;
    check(ui_go_live_scene_update(&s, c, viewport) &&
              ui_go_live_scene_hit_test(s, select.x, select.y).action != UiGoLiveAction::select && drawn.collect(s) &&
              drawn.has(utf8_literal("Waiting for your selection\xE2\x80\xA6")),
          "go live dialog (system picker): waiting for the system's picker hides Make Selection");
    ui_go_live_scene_release(&s);
}

void unavailable_cases(Check check) noexcept {
    static UiGoLiveScene s;
    static Drawn drawn;
    UiGoLiveContent c{};
    c.title = utf8_literal("Screen Sharing Unavailable");
    c.body = utf8_literal("Your graphics card has no hardware H.264 video encoder, which streaming needs.");
    c.mode = UiGoLiveMode::unavailable;
    c.nitro = true;
    c.revision = 1;
    check(ui_go_live_scene_update(&s, c, {0, 0, 1280, 800}) && !ui_rect_has_area(s.upsell) &&
              ui_widget_handle_matches(s.widgets.focused, s.close) &&
              ui_go_live_scene_hit_test(s, middle(s.options_rectangle).x, middle(s.options_rectangle).y).action ==
                  UiGoLiveAction::none &&
              drawn.collect(s) && drawn.has(c.title) && drawn.has(c.body),
          "go live dialog (unavailable): only the reason and Close, no settings or HD line");
    check(!ui_go_live_scene_update(&s, c, {0, 0, 200, 200}) && !ui_go_live_scene_update(nullptr, c, {0, 0, 900, 700}),
          "go live dialog: refuses a viewport too small for it");
    ui_go_live_scene_release(&s);
}
} // namespace

void test_go_live_scene(Check check) noexcept {
    picker_cases(check);
    system_cases(check);
    unavailable_cases(check);
}
} // namespace maicord
