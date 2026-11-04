#include "maicord/ui/store/store_scene.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/text/presentation.hpp"
#include "maicord/ui/accessibility/accessibility.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/ui/theme/background_surface.hpp"
#include "maicord/ui/theme/styles.hpp"

// The store scene: layout of the retained element tree, grid columns by width, wrapping chips, hit testing,
// the keyboard model (roving focus in groups, sliders), scrolling and the centred dialog.
namespace maicord {
namespace {
UiStoreScene g_scene;

constexpr u32 kCardAction = 0x10000, kSlider = 0x20000, kChip = 0x30000;

void build(float width, u32 cards, bool dialog) noexcept {
    ui_store_scene_begin(&g_scene);
    UiStoreBuilder page(&g_scene);
    auto& chips = page.open(UiStoreKind::flow);
    chips.gap = 8;
    for (u32 i = 0; i < 14; ++i) {
        auto& chip = page.leaf(UiStoreKind::filter);
        chip.text = utf8_literal("Category");
        chip.text2 = utf8_literal("12");
        chip.action = kChip + i;
        chip.group = 3;
    }
    page.close();
    auto& grid = page.open(UiStoreKind::grid);
    grid.gap = 16;
    grid.rw = 232;
    for (u32 i = 0; i < cards; ++i) {
        auto& card = page.open(UiStoreKind::card);
        card.action = kCardAction + i;
        card.group = 2;
        card.pad_x = card.pad_y = 16;
        page.text(utf8_literal("A card with a body of text"), UiStoreType::body_bold, UiStoreInk::strong);
        page.spacer(0, 60);
        page.close();
    }
    page.close();
    auto& slider = page.leaf(UiStoreKind::slider);
    slider.action = kSlider;
    slider.count = 11;
    slider.value = 5;
    slider.group = 6;
    page.close();
    if (dialog) {
        UiStoreBuilder d(&g_scene, true);
        auto& body = d.open(UiStoreKind::column);
        body.pad_x = body.pad_y = 24;
        d.text(utf8_literal("Install it?"), UiStoreType::heading, UiStoreInk::strong);
        d.close();
        auto& footer = d.open(UiStoreKind::row);
        footer.pad_x = 24;
        footer.pad_y = 12;
        footer.fill = static_cast<u8>(UiStoreFill::translucent);
        d.button(utf8_literal("Cancel"), UiStoreButton::secondary, 0x40001, UiIcon::count, true);
        d.button(utf8_literal("Install"), UiStoreButton::primary, 0x40002, UiIcon::count, true);
        d.close();
        d.close();
    }
    ui_store_scene_commit(&g_scene, {0, 0, 900, 600}, {40, 30, 40 + width, 600}, {0, 0, 900, 600});
}

u32 columns() noexcept {
    u32 columns = 0;
    float first = -1;
    for (u32 i = 0; i < g_scene.count; ++i) {
        const auto& n = g_scene.nodes[i];
        if (n.kind != UiStoreKind::card)
            continue;
        if (first < 0)
            first = n.y;
        if (n.y == first)
            ++columns;
    }
    return columns;
}
} // namespace

void test_store_scene(void (*check)(bool, const char*) noexcept) noexcept {
    if (!ui_store_scene_open(&g_scene)) {
        check(false, "store scene: opens");
        return;
    }
    g_scene.active = true;
    build(792, 7, false);
    check(columns() == 3, "store scene: three card columns at 792 DIP");
    build(520, 7, false);
    check(columns() == 2, "store scene: two columns at 520 DIP");
    build(300, 7, false);
    check(columns() == 1, "store scene: one column below 480 DIP");
    build(792, 7, false);
    float widest = 0, chip_rows = 0, last_y = -1;
    for (u32 i = 0; i < g_scene.count; ++i) {
        const auto& n = g_scene.nodes[i];
        if (n.kind != UiStoreKind::filter)
            continue;
        widest = n.x + n.w > widest ? n.x + n.w : widest;
        if (n.y != last_y) {
            ++chip_rows;
            last_y = n.y;
        }
    }
    check(widest <= 40 + 792 + .5f && chip_rows >= 2, "store scene: chips wrap inside the column and never overflow it");
    // Hit testing and focus.
    UiRect card{};
    check(ui_store_scene_rect(g_scene, kCardAction, &card), "store scene: a node answers its rectangle by action");
    check(ui_store_scene_hover(&g_scene, card.left + 5, card.top + 5) && g_scene.hover_action == kCardAction,
          "store scene: the pointer hovers the card under it");
    UiStoreEvent event{};
    check(ui_store_scene_press(&g_scene, card.left + 5, card.top + 5, &event) &&
              ui_store_scene_release_pointer(&g_scene, card.left + 5, card.top + 5, &event) &&
              event.kind == UiStoreEventKind::activate && event.action == kCardAction,
          "store scene: press and release on a card activates it");
    check(!ui_store_scene_release_pointer(&g_scene, 0, 0, &event), "store scene: a release without a press does nothing");
    // Keyboard.
    ui_store_scene_focus(&g_scene, kCardAction, true);
    check(ui_store_scene_key(&g_scene, UiStoreKey::right, &event) && ui_store_scene_focused(g_scene) == kCardAction + 1,
          "store scene: Right moves to the next card of the group");
    check(ui_store_scene_key(&g_scene, UiStoreKey::down, &event) && ui_store_scene_focused(g_scene) == kCardAction + 4,
          "store scene: Down moves a whole row");
    check(ui_store_scene_key(&g_scene, UiStoreKey::home, &event) && ui_store_scene_focused(g_scene) == kCardAction + 3,
          "store scene: Home goes to the start of the row");
    check(ui_store_scene_key(&g_scene, UiStoreKey::last, &event) && ui_store_scene_focused(g_scene) == kCardAction + 6,
          "store scene: Ctrl+End goes to the last card");
    check(ui_store_scene_key(&g_scene, UiStoreKey::enter, &event) && event.kind == UiStoreEventKind::activate &&
              event.action == kCardAction + 6,
          "store scene: Enter activates the focused card");
    check(ui_store_scene_focus(&g_scene, kSlider, true) && ui_store_scene_key(&g_scene, UiStoreKey::right, &event) &&
              event.kind == UiStoreEventKind::value && event.value == 6 && ui_store_scene_key(&g_scene, UiStoreKey::end, &event) &&
              event.value == 10,
          "store scene: a slider steps with the arrows and jumps with End");
    // Tab walks every stop in order, then leaves the page.
    ui_store_scene_focus(&g_scene, 0, false);
    u32 stops = 0;
    while (ui_store_scene_key(&g_scene, UiStoreKey::tab, &event) && stops < 100)
        ++stops;
    check(stops == 14 + 7 + 1, "store scene: Tab visits each chip, card and slider once, then hands focus back");
    // Scrolling clamps to the content.
    check(!ui_store_scene_scroll(&g_scene, -50), "store scene: no scrolling above the top");
    build(792, 40, false);
    check(ui_store_scene_scroll(&g_scene, 100000) && g_scene.scroll == g_scene.content_height - (g_scene.panel.bottom - g_scene.column.top),
          "store scene: scrolling stops at the end of the content plus its tail");
    ui_store_scene_scroll_to(&g_scene, 0);
    // Accessibility: every interactive node is one node, a card with its title, a chip with its label.
    build(792, 7, false);
    static UiAccessNode access_nodes[128];
    static char access_text[8192];
    static u32 access_slots[256], access_keys[256];
    UiAccessTree tree{};
    check(ui_access_tree_initialize(&tree, {access_nodes, 128}, {access_text, sizeof(access_text)}, {access_slots, 256},
                                    {access_keys, 256}) &&
              ui_access_tree_reset(&tree, utf8_literal("Store"), {0, 0, 1000, 800}) &&
              ui_access_add_store(&tree, 0, g_scene, 9) != kUiAccessNone,
          "store scene: the page describes itself to accessibility");
    u32 filters = 0, cards = 0, sliders = 0;
    for (u32 i = 1; i < tree.count; ++i) {
        filters += tree.nodes[i].role == UiAccessRole::toggle_button && tree.nodes[i].surface == 9;
        cards += tree.nodes[i].role == UiAccessRole::button && tree.nodes[i].item >= kCardAction && tree.nodes[i].item < kSlider;
        sliders += tree.nodes[i].role == UiAccessRole::slider;
    }
    check(filters >= 1 && cards == 7 && sliders == 1, "store scene: chips are toggles, cards buttons and the knob a slider");
    // A dialog is centred in the modal and traps the pointer.
    build(792, 4, true);
    check(g_scene.dialog_open && g_scene.dialog_rect.left > 100 && g_scene.dialog_rect.right < 800 &&
              ui_rect_width(g_scene.dialog_rect) == 600 &&
              ui_store_scene_modal_hit(g_scene, 450, 300) && !ui_store_scene_modal_hit(g_scene, 10, 10),
          "store scene: a dialog is 600 wide and centred in the modal");
    check(!ui_store_scene_hover(&g_scene, card.left + 5, card.top + 5) || g_scene.hover_action == 0,
          "store scene: nothing behind a dialog is hot");
    // Preview tiles: a still asks the host for its picture at its size; a live tile is a canvas quad with a radius
    // (the renderer fills it with the running effect) and has no picture of its own.
    {
        static u32 asked_slot, asks;
        static float asked_width;
        ui_store_scene_begin(&g_scene);
        UiStoreBuilder page(&g_scene);
        auto& still = page.open(UiStoreKind::preview);
        still.variant = static_cast<u8>(UiStoreAspect::r16x9);
        still.value = 3;
        page.close();
        auto& live = page.open(UiStoreKind::preview);
        live.variant = static_cast<u8>(UiStoreAspect::live);
        live.rh = 220;
        live.value = 4;
        page.close();
        const bool laid = ui_store_scene_commit(&g_scene, {0, 0, 900, 700}, {40, 30, 832, 700}, {0, 0, 900, 700});
        g_scene.active = true;
        g_scene.media_key = [](const void*, u32 slot, const UiRect& rect, MediaAssetKey* key, u8* domain) noexcept {
            asked_slot = slot;
            asked_width = ui_rect_width(rect);
            ++asks;
            *key = media_asset_local_draft_preview(slot, 1, MediaAttachmentFormat::png);
            *domain = 1;
            return true;
        };
        UiStoreMedia media{};
        u32 cursor = 0, pictures = 0;
        while (ui_store_scene_next_media(g_scene, &cursor, &media))
            ++pictures;
        check(laid && pictures == 1 && asks == 1 && asked_slot == 3 && asked_width == 792 && media.slot == 3 && media.domain == 1 &&
                  media_asset_key_valid(media.key),
              "store scene: a still tile asks for its picture by slot and width; a live tile never does");
        PaintStream paint{};
        check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 900, 700) &&
                  ui_store_scene_paint(g_scene, kDefaultTheme, &paint, 0),
              "store scene: the tiles paint");
        u32 tiles = 0, canvases = 0;
        for (u32 i = 0; i < paint.count; ++i) {
            canvases += ui_background_canvas_quad(paint.quads[i]) ? 1 : 0;
            tiles += ui_background_canvas_quad(paint.quads[i]) && paint.quads[i].radius > 0 ? 1 : 0;
        }
        check(tiles == 1 && canvases == 1, "store scene: a live tile is one rounded canvas quad for the effect pass");
        paint_stream_release(&paint);
        // The live tile on screen lets its effect animate; under a dialog, or below the panel's edge, it does not.
        const bool shown = ui_store_scene_live_visible(g_scene);
        g_scene.dialog_open = true;
        const bool under_dialog = ui_store_scene_live_visible(g_scene);
        g_scene.dialog_open = false;
        const bool below = ui_store_scene_commit(&g_scene, {0, 0, 900, 300}, {40, 30, 832, 300}, {0, 0, 900, 300}) &&
                           !ui_store_scene_live_visible(g_scene);
        check(shown && !under_dialog && below, "store scene: a live tile animates only while it is on screen");
        g_scene.media_key = nullptr;
    }
    {
        // A search field with text tints its clear button while the pointer is over the button, not elsewhere in the field.
        constexpr u32 kSearch = 0x40000;
        ui_store_scene_begin(&g_scene);
        UiStoreBuilder page(&g_scene);
        auto& field = page.leaf(UiStoreKind::search);
        field.action = kSearch;
        field.value = 1;
        field.text = utf8_literal("Search");
        const bool laid = ui_store_scene_commit(&g_scene, {0, 0, 900, 700}, {40, 30, 832, 700}, {0, 0, 900, 700});
        g_scene.active = true;
        UiRect rect{};
        const bool found = ui_store_scene_rect(g_scene, kSearch, &rect);
        const float cy = (rect.top + rect.bottom) * .5f;
        // The fill of the 24-DIP clear disc.
        const auto clear_fill = [&](Color* fill) noexcept {
            PaintStream paint{};
            bool seen = false;
            if (paint_stream_initialize(&paint) && paint_stream_reset(&paint, 900, 700) &&
                ui_store_scene_paint(g_scene, kDefaultTheme, &paint, 0))
                for (u32 i = 0; i < paint.count && !seen; ++i) {
                    const PaintQuad& q = paint.quads[i];
                    if (q.radius == 12 && q.right - q.left > 23.99f && q.right - q.left < 24.01f &&
                        q.bottom - q.top > 23.99f && q.bottom - q.top < 24.01f) {
                        *fill = q.fill;
                        seen = true;
                    }
                }
            paint_stream_release(&paint);
            return seen;
        };
        Color away{}, over{};
        const bool left = laid && found && ui_store_scene_hover(&g_scene, rect.left + 40, cy) && clear_fill(&away);
        const bool moved = ui_store_scene_hover(&g_scene, rect.right - 20, cy) && clear_fill(&over);
        check(left && moved && !bytes_equal(&away, &over, sizeof(Color)),
              "store scene: the search clear button tints only under the pointer");
        check(ui_store_scene_hover(&g_scene, rect.left + 40, cy) && clear_fill(&over) && bytes_equal(&away, &over, sizeof(Color)),
              "store scene: the clear button's tint ends when the pointer leaves it for the field");
    }
    {
        // A busy button: the spinner sits where an icon goes, the label starts after it with the icon's gap, and
        // the button is wide enough for both (a card's Installing... and the detail page's stretched Updating...).
        // Busy buttons are disabled (no action to look them up by): they are found by their label.
        ui_store_scene_begin(&g_scene);
        UiStoreBuilder page(&g_scene);
        page.button(utf8_literal("Installing..."), UiStoreButton::secondary, 0).flags |= ui_store_busy | ui_store_disabled;
        page.button(utf8_literal("Updating..."), UiStoreButton::primary, 0).flags |= ui_store_busy | ui_store_disabled | ui_store_stretch;
        const bool laid = ui_store_scene_commit(&g_scene, {0, 0, 900, 700}, {40, 30, 832, 700}, {0, 0, 900, 700});
        g_scene.active = true;
        PaintStream paint{};
        UiTextCommand commands[16]{};
        UiTextCommandStream stream{};
        UiTextOutput output{&stream, nullptr};
        const bool drawn = laid && paint_stream_initialize(&paint) && paint_stream_reset(&paint, 900, 700) &&
                           ui_store_scene_paint(g_scene, kDefaultTheme, &paint, 0) &&
                           ui_text_command_stream_initialize(&stream, {commands, 16}) &&
                           ui_store_scene_text_output(g_scene, kDefaultTheme, &output, false);
        const auto separated = [&](const Utf8View& label) noexcept {
            const UiStoreNode* node = nullptr;
            for (u32 i = 0; drawn && i < g_scene.count; ++i)
                if (g_scene.nodes[i].kind == UiStoreKind::button && utf8_equal(g_scene.nodes[i].text, label))
                    node = &g_scene.nodes[i];
            if (!node)
                return false;
            // On screen: the page is laid out from the column's top and is not scrolled.
            const float top = node->y + g_scene.column.top;
            const UiRect button{node->x, top, node->x + node->w, top + node->h};
            // The spinner's strokes (line quads) inside the button.
            UiRect spinner{1e9f, 1e9f, -1e9f, -1e9f};
            for (u32 i = 0; i < paint.count; ++i) {
                const PaintQuad& q = paint.quads[i];
                if (q.border_width != -1 || q.top < button.top || q.bottom > button.bottom)
                    continue;
                spinner = {minimum(spinner.left, q.left), minimum(spinner.top, q.top), maximum(spinner.right, q.right),
                           maximum(spinner.bottom, q.bottom)};
            }
            const UiTextCommand* text = nullptr;
            for (u32 i = 0; i < stream.count; ++i)
                if (utf8_equal(stream.storage[i].text, label))
                    text = &stream.storage[i];
            return text && ui_rect_has_area(spinner) && spinner.left >= button.left && text->rectangle.right <= button.right &&
                   !ui_rect_intersects(spinner, text->rectangle) && text->rectangle.left - spinner.right >= 6;
        };
        check(separated(utf8_literal("Installing...")),
              "store scene: a busy button's label starts after its spinner and a gap, inside the button");
        check(separated(utf8_literal("Updating...")),
              "store scene: a stretched busy button centres the spinner and the label without overlap");
        paint_stream_release(&paint);
        // The busy spinners are the host's shared spinner clock's to drive; the pulse tick never spins for them.
        check(ui_store_scene_spinner_visible(g_scene) && !ui_store_scene_tick(&g_scene, 1000, false),
              "store scene: a busy button is a visible spinner and needs no pulse frames");
        g_scene.active = false;
        check(!ui_store_scene_spinner_visible(g_scene), "store scene: an inactive page has no visible spinner");
        g_scene.active = true;
        build(792, 7, false);
        check(!ui_store_scene_spinner_visible(g_scene), "store scene: a page without busy items has no visible spinner");
        // A skeleton pulses; a second tick in the same millisecond still asks for the next frame.
        ui_store_scene_begin(&g_scene);
        UiStoreBuilder skeleton(&g_scene);
        skeleton.leaf(UiStoreKind::bar).rw = 200;
        ui_store_scene_commit(&g_scene, {0, 0, 900, 700}, {40, 30, 832, 700}, {0, 0, 900, 700});
        check(ui_store_scene_tick(&g_scene, 2000, false) && ui_store_scene_tick(&g_scene, 2000, false) &&
                  !ui_store_scene_tick(&g_scene, 2001, true),
              "store scene: a skeleton keeps asking for frames, also on a repeated tick, and not under reduced motion");
    }
    {
        // The live preview's pill (the effect page's tile): its label holds 4.5:1 or better on its plate whether a
        // black or a white effect is behind it, in the dark and light themes and in a custom theme whose on-accent
        // text is dark (the plate is a scrim; its ink is the overlay text, not the accent's).
        Theme dark_on_accent = kDefaultTheme;
        dark_on_accent.tokens.text.on_accent = {.08f, .08f, .1f, 1};
        const Theme* themes[]{&kDefaultTheme, &kLightTheme, &dark_on_accent};
        const Utf8View label = utf8_literal("Live preview \xC2\xB7 24 fps \xC2\xB7 50 %");
        bool readable = true;
        for (const Theme* theme : themes) {
            ui_store_scene_begin(&g_scene);
            UiStoreBuilder page(&g_scene);
            auto& live = page.open(UiStoreKind::preview);
            live.variant = static_cast<u8>(UiStoreAspect::live);
            live.rh = 220;
            auto& pill = page.chip(label, UiStoreTone::scrim, UiIcon::play);
            pill.anchor = static_cast<u8>(UiStoreAnchor::bottom_left);
            pill.flags = ui_store_foreground;
            page.close();
            const bool laid = ui_store_scene_commit(&g_scene, {0, 0, 900, 700}, {40, 30, 832, 700}, {0, 0, 900, 700});
            g_scene.active = true;
            PaintStream paint{};
            UiTextCommand commands[16]{};
            UiTextCommandStream stream{};
            UiTextOutput output{&stream, nullptr};
            const bool drawn = laid && paint_stream_initialize(&paint) && paint_stream_reset(&paint, 900, 700) &&
                               ui_store_scene_paint(g_scene, *theme, &paint, 1) &&
                               ui_text_command_stream_initialize(&stream, {commands, 16}) &&
                               ui_store_scene_text_output(g_scene, *theme, &output, false);
            // The plate: the largest rounded fill of the foreground layer (the play icon sits inside it).
            const PaintQuad* plate = nullptr;
            for (u32 i = 0; drawn && i < paint.count; ++i) {
                const PaintQuad& q = paint.quads[i];
                if (q.radius > 0 && q.fill.alpha > 0 &&
                    (!plate || (q.right - q.left) * (q.bottom - q.top) > (plate->right - plate->left) * (plate->bottom - plate->top)))
                    plate = &q;
            }
            const UiTextCommand* text = nullptr;
            for (u32 i = 0; i < stream.count; ++i)
                if (utf8_equal(stream.storage[i].text, label))
                    text = &stream.storage[i];
            readable = readable && plate && text;
            for (u32 behind = 0; readable && behind < 2; ++behind) {
                const float effect = static_cast<float>(behind), a = plate->fill.alpha;
                const Color under{plate->fill.red * a + effect * (1 - a), plate->fill.green * a + effect * (1 - a),
                                  plate->fill.blue * a + effect * (1 - a), 1};
                readable = ui_theme_contrast_ratio(text->color, under) >= 4.5f;
            }
            paint_stream_release(&paint);
        }
        check(readable, "store scene: the live preview's pill reads 4.5:1 on its plate over a black or a white effect");
    }
    {
        // A choice: a segmented track of buttons as wide as their labels, and the dropdown standing in for it
        // (ui_store_fallback). The track shows while its buttons fit the width, the dropdown once they don't; each
        // layout decides again, and only the shown control takes the pointer.
        static constexpr Utf8View kLong[]{utf8_literal("Follow the clock"), utf8_literal("Clock, night when dark"),
                                          utf8_literal("Day (light themes)"), utf8_literal("Night")};
        static constexpr Utf8View kShort[]{utf8_literal("Clock"), utf8_literal("Night when dark"), utf8_literal("Day"),
                                           utf8_literal("Night")};
        u16 track = kUiStoreNone, menu = kUiStoreNone;
        const auto choice = [&](const Utf8View* labels, float width) noexcept {
            ui_store_scene_begin(&g_scene);
            UiStoreBuilder page(&g_scene);
            auto& row = page.open(UiStoreKind::row);
            row.variant = 1;
            row.pad_x = row.pad_y = 4;
            for (u32 i = 0; i < 4; ++i) {
                auto& s = page.leaf(UiStoreKind::segment);
                s.text = labels[i];
                s.action = 0x50000 + i;
            }
            page.close();
            track = static_cast<u16>(g_scene.count - 5);
            auto& dd = page.leaf(UiStoreKind::dropdown);
            dd.text2 = labels[1];
            dd.action = 0x50010;
            dd.flags = ui_store_stretch | ui_store_fallback;
            menu = static_cast<u16>(g_scene.count - 1);
            page.close();
            return ui_store_scene_commit(&g_scene, {0, 0, 1200, 700}, {40, 30, 40 + width, 700}, {0, 0, 1200, 700});
        };
        const auto shows_track = [&]() noexcept {
            return !(g_scene.nodes[track].flags & ui_store_hidden) && (g_scene.nodes[menu].flags & ui_store_hidden);
        };
        const auto shows_menu = [&]() noexcept {
            return (g_scene.nodes[track].flags & ui_store_hidden) && !(g_scene.nodes[menu].flags & ui_store_hidden);
        };
        const bool wide = choice(kShort, 600) && shows_track();
        const auto& first = g_scene.nodes[track + 1];
        const auto& second = g_scene.nodes[track + 2];
        const auto& last = g_scene.nodes[track + 4];
        const auto& plate = g_scene.nodes[track];
        const bool content = wide && first.w < second.w && second.x == first.x + first.w && plate.w < 600 &&
                             last.x + last.w <= plate.x + plate.w;
        UiStoreEvent event{};
        UiRect hit{};
        const auto click = [&](u32 action) noexcept {
            event = {};
            return ui_store_scene_rect(g_scene, action, &hit) &&
                   ui_store_scene_press(&g_scene, (hit.left + hit.right) * .5f, (hit.top + hit.bottom) * .5f, &event) &&
                   ui_store_scene_release_pointer(&g_scene, (hit.left + hit.right) * .5f, (hit.top + hit.bottom) * .5f,
                                                  &event) &&
                   event.action == action;
        };
        g_scene.active = true;
        check(content && click(0x50000), "store scene: a choice whose buttons fit shows them, each as wide as its label");
        check(choice(kShort, 200) && shows_menu() && click(0x50010) && !click(0x50000),
              "store scene: a choice whose buttons don't fit the width falls back to its dropdown");
        check(choice(kLong, 520) && shows_menu() && choice(kShort, 520) && shows_track() && choice(kLong, 1100) &&
                  shows_track(),
              "store scene: each layout decides again: long labels take the dropdown where short ones fit, and the "
              "track comes back when the width allows");
    }
    ui_store_scene_release(&g_scene);
}

} // namespace maicord
