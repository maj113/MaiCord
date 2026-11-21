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
    // Keyboard, from the card the press focused.
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
    check(ui_store_scene_key(&g_scene, UiStoreKey::tab, &event) && ui_store_scene_focused(g_scene) == kSlider &&
              ui_store_scene_key(&g_scene, UiStoreKey::right, &event) &&
              event.kind == UiStoreEventKind::value && event.value == 6 && ui_store_scene_key(&g_scene, UiStoreKey::end, &event) &&
              event.value == 10,
          "store scene: a slider steps with the arrows and jumps with End");
    // Tab walks every stop in order, then leaves the page.
    g_scene.focus_action = 0;
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
        // A preview's own radius (aux): a 32 DIP avatar past half its side is a disc, a game icon keeps its 6.
        ui_store_scene_begin(&g_scene);
        UiStoreBuilder round(&g_scene);
        static constexpr u16 kRadii[]{999, 6};
        for (const u16 radius : kRadii) {
            auto& picture = round.open(UiStoreKind::preview);
            picture.variant = static_cast<u8>(UiStoreAspect::fixed);
            picture.rw = picture.rh = 32;
            picture.aux = radius;
            picture.value = radius;
            round.close();
        }
        round.close();
        UiStoreMedia disc{}, icon{};
        u32 at = 0;
        const bool rounded = ui_store_scene_commit(&g_scene, {0, 0, 900, 700}, {40, 30, 832, 700}, {0, 0, 900, 700}) &&
                             ui_store_scene_next_media(g_scene, &at, &disc) &&
                             ui_store_scene_next_media(g_scene, &at, &icon);
        check(rounded && disc.mask_radius == 16 && icon.mask_radius == 6 && ui_rect_width(disc.rect) == 32,
              "store scene: a preview's own radius rounds its picture, a disc at most");
        g_scene.media_key = nullptr;
    }
    {
        // A card's corner badge overhangs its top end corner and takes no room; a row's revealed button is hit only
        // once its row is hovered (Channels & Roles' answer tile and Browse Channels' View).
        constexpr u32 kTile = 0x5000, kRow = 0x5001, kView = 0x5002;
        ui_store_scene_begin(&g_scene);
        UiStoreBuilder page(&g_scene);
        auto& tile = page.open(UiStoreKind::card);
        tile.variant = static_cast<u8>(UiStoreCard::answer);
        tile.action = kTile;
        tile.pad_x = tile.pad_y = 16;
        page.text("Answer");
        auto& badge = page.leaf(UiStoreKind::swatch);
        badge.rw = badge.rh = 20;
        badge.variant = 2;
        badge.anchor = static_cast<u8>(UiStoreAnchor::top_right);
        badge.pad_x = badge.pad_y = 10;
        page.close();
        auto& line = page.open(UiStoreKind::card);
        line.variant = static_cast<u8>(UiStoreCard::row);
        line.action = kRow;
        page.open(UiStoreKind::row);
        page.text("general").flags = ui_store_flex;
        auto& view = page.button("View", UiStoreButton::secondary, kView);
        view.aux = kRow;
        page.close();
        page.close();
        const bool laid = ui_store_scene_commit(&g_scene, {0, 0, 900, 700}, {40, 30, 832, 700}, {0, 0, 900, 700});
        g_scene.active = true;
        const UiStoreNode* card = nullptr;
        const UiStoreNode* disc = nullptr;
        for (u32 i = 0; i < g_scene.count; ++i) {
            const auto& n = g_scene.nodes[i];
            if (n.kind == UiStoreKind::card && n.action == kTile)
                card = &n;
            if (n.kind == UiStoreKind::swatch)
                disc = &n;
        }
        check(laid && card && disc && card->h == 52 && disc->x + disc->w == card->x + card->w + 10 &&
                  disc->y == card->y - 10,
              "store scene: a card's corner badge overhangs its top end corner and takes no room");
        UiRect button{};
        const bool found = ui_store_scene_rect(g_scene, kView, &button);
        const float x = (button.left + button.right) * .5f, y = (button.top + button.bottom) * .5f;
        ui_store_scene_hover(&g_scene, x, y);
        const u32 first = g_scene.hover_action;
        ui_store_scene_hover(&g_scene, x, y);
        const u32 second = g_scene.hover_action;
        check(found && first == kRow && second == kView,
              "store scene: a revealed button is hit only once its row is hovered");
        ui_store_scene_hover(&g_scene, -1, -1);
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
        check(ui_store_scene_spinner_visible(g_scene) && !ui_store_scene_tick(&g_scene, 1000, false, 300),
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
        check(ui_store_scene_tick(&g_scene, 2000, false, 300) && ui_store_scene_tick(&g_scene, 2000, false, 300) &&
                  !ui_store_scene_tick(&g_scene, 2001, true, 200),
              "store scene: a skeleton keeps asking for frames, also on a repeated tick, and not under reduced motion");
        // The switch last activated slides: the rebuilt switch starts on its old look, is between the two states
        // midway, settles exactly on the new one and then asks for no frame; reduced motion keeps the shorter slide.
        constexpr u32 kToggle = 0x50000;
        const auto build_toggle = [&](u32 value) noexcept {
            ui_store_scene_begin(&g_scene);
            UiStoreBuilder sw(&g_scene);
            auto& node = sw.leaf(UiStoreKind::toggle);
            node.action = kToggle;
            node.value = value;
            return ui_store_scene_commit(&g_scene, {0, 0, 900, 700}, {40, 30, 832, 700}, {0, 0, 900, 700});
        };
        // The switch's track fill and its thumb: the first two quads inside its rectangle.
        const auto look = [&](Color* track, UiRect* thumb) noexcept {
            UiRect rect{};
            PaintStream paint{};
            bool ok = ui_store_scene_rect(g_scene, kToggle, &rect) && paint_stream_initialize(&paint) &&
                      paint_stream_reset(&paint, 900, 700) && ui_store_scene_paint(g_scene, kDefaultTheme, &paint, 0);
            u32 found = 0;
            for (u32 i = 0; ok && i < paint.count && found < 2; ++i) {
                const auto& q = paint.quads[i];
                if (q.left < rect.left - .5f || q.top < rect.top - .5f || q.right > rect.right + .5f ||
                    q.bottom > rect.bottom + .5f)
                    continue;
                if (!found++)
                    *track = q.fill;
                else
                    *thumb = {q.left, q.top, q.right, q.bottom};
            }
            paint_stream_release(&paint);
            return ok && found == 2;
        };
        const auto near_rect = [](const UiRect& a, const UiRect& b) noexcept {
            return ui_rect_width(a) - ui_rect_width(b) < .01f && ui_rect_width(b) - ui_rect_width(a) < .01f &&
                   a.left - b.left < .01f && b.left - a.left < .01f && a.top - b.top < .01f && b.top - a.top < .01f;
        };
        UiStoreEvent event{};
        Color off_track{}, on_track{}, mid_track{};
        UiRect off_thumb{}, on_thumb{}, mid_thumb{}, start_thumb{};
        g_scene.active = true;
        g_scene.focus_action = 0;
        check(build_toggle(0) && look(&off_track, &off_thumb) && !ui_store_scene_animating(g_scene) &&
                  ui_store_scene_key(&g_scene, UiStoreKey::tab, &event) && ui_store_scene_focused(g_scene) == kToggle &&
                  ui_store_scene_key(&g_scene, UiStoreKey::space, &event) && event.kind == UiStoreEventKind::activate &&
                  event.action == kToggle && ui_store_scene_animating(g_scene) && build_toggle(1) &&
                  ui_store_scene_tick(&g_scene, 3000, false, 300) && look(&mid_track, &start_thumb) &&
                  near_rect(start_thumb, off_thumb),
              "store scene: an activated switch starts its slide on its old look");
        const bool midway = ui_store_scene_tick(&g_scene, 3150, false, 300) && look(&mid_track, &mid_thumb);
        const bool settled = !ui_store_scene_tick(&g_scene, 3400, false, 300) && !ui_store_scene_animating(g_scene) &&
                             look(&on_track, &on_thumb);
        check(midway && settled && mid_thumb.left > off_thumb.left + .5f && mid_thumb.left < on_thumb.left - .5f &&
                  !bytes_equal(&mid_track, &off_track, sizeof(Color)) && !bytes_equal(&mid_track, &on_track, sizeof(Color)),
              "store scene: midway the switch's thumb and track are between the off and on looks, then it settles");
        const u32 motion = g_scene.motion;
        check(!ui_store_scene_tick(&g_scene, 3500, false, 300) && g_scene.motion == motion &&
                  !bytes_equal(&on_track, &off_track, sizeof(Color)),
              "store scene: a settled switch asks for no frame and no repaint");
        UiRect reduced_thumb{};
        Color reduced_track{};
        check(ui_store_scene_key(&g_scene, UiStoreKey::space, &event) && build_toggle(0) &&
                  ui_store_scene_tick(&g_scene, 4000, true, 200) && ui_store_scene_tick(&g_scene, 4100, true, 200) &&
                  look(&reduced_track, &reduced_thumb) && reduced_thumb.left > off_thumb.left + .5f &&
                  reduced_thumb.left < on_thumb.left - .5f && !ui_store_scene_tick(&g_scene, 4300, true, 200) &&
                  look(&reduced_track, &reduced_thumb) && near_rect(reduced_thumb, off_thumb),
              "store scene: under reduced motion the switch slides over the shorter duration and settles on the new state");
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
    {
        // A pane: a list 120 DIPs high whose ten rows of 40 scroll inside it, clipped to it; the page goes on below.
        constexpr u32 kRow = 0x60000, kAfter = 0x60100;
        static constexpr const char* kNames[]{"Row 0", "Row 1", "Row 2", "Row 3", "Row 4",
                                              "Row 5", "Row 6", "Row 7", "Row 8", "Row 9"};
        const auto pane = [&]() noexcept {
            ui_store_scene_begin(&g_scene);
            UiStoreBuilder page(&g_scene);
            auto& list = page.open(UiStoreKind::column);
            list.flags = ui_store_pane;
            list.rh = 120;
            for (u32 i = 0; i < 10; ++i) {
                auto& row = page.open(UiStoreKind::row);
                row.action = kRow + i;
                row.text2 = Utf8View{kNames[i], 5};
                page.spacer(0, 40);
                page.close();
            }
            page.close();
            page.button(utf8_literal("After"), UiStoreButton::secondary, kAfter);
            page.close();
            return ui_store_scene_commit(&g_scene, {0, 0, 900, 600}, {40, 30, 832, 600}, {0, 0, 900, 600});
        };
        g_scene.active = true;
        UiRect view{}, after{};
        float offset = -1, most = -1;
        const bool laid = pane() && ui_store_scene_pane_range(g_scene, &view, &offset, &most);
        check(laid && view.bottom - view.top == 120 && offset == 0 && most == 280 &&
                  ui_store_scene_rect(g_scene, kAfter, &after) && after.top >= view.bottom,
              "store scene: a pane keeps its own height and what follows it starts below it");
        const float x = 100;
        check(ui_store_scene_action_at(g_scene, x, view.top + 5) == kRow &&
                  ui_store_scene_action_at(g_scene, x, view.top + 85) == kRow + 2 &&
                  ui_store_scene_action_at(g_scene, (after.left + after.right) * .5f, after.top + 5) == kAfter,
              "store scene: the hand finds the pane's rows inside it and the page's buttons below it");
        Utf8View tip{};
        UiRect anchor{};
        u32 index = 0;
        check(ui_store_scene_pane_scroll_to(&g_scene, 80) && ui_store_scene_pane_range(g_scene, &view, &offset, &most) &&
                  offset == 80 && ui_store_scene_action_at(g_scene, x, view.top + 5) == kRow + 2 &&
                  ui_store_scene_tooltip_at(g_scene, x, view.top + 5, &tip, &anchor, &index) &&
                  utf8_equal(tip, utf8_literal("Row 2")) && anchor.top >= view.top &&
                  !ui_store_scene_tooltip_at(g_scene, x, view.bottom + 2, &tip, &anchor, &index),
              "store scene: the pane scrolls its rows; hits and tooltips follow them and stop at its edge");
        check(ui_store_scene_pane_scroll_to(&g_scene, 10000) && ui_store_scene_pane_range(g_scene, &view, &offset, &most) &&
                  offset == 280 && pane() && ui_store_scene_pane_range(g_scene, &view, &offset, &most) && offset == 280 &&
                  !ui_store_scene_pane_scroll_to(&g_scene, 400),
              "store scene: the pane stops at its end and keeps its offset when the page is built again");
        ui_store_scene_pane_scroll_to(&g_scene, 0);
        g_scene.focus_action = 0;
        check(ui_store_scene_key(&g_scene, UiStoreKey::back_tab, nullptr) &&
                  ui_store_scene_key(&g_scene, UiStoreKey::back_tab, nullptr) &&
                  ui_store_scene_focused(g_scene) == kRow + 9 &&
                  ui_store_scene_pane_range(g_scene, &view, &offset, &most) && offset == 280,
              "store scene: Shift+Tab to a row out of the pane's view scrolls the pane to it");
    }
    {
        // A card narrower than its content (Channels & Roles' question card and the Server Hub's banner): the wrapped
        // title gives up the row's room and grows in lines, the required mark keeps its width, a long chip ellipsizes
        // in its pill, a flow's long text wraps on its own line and a banner's caption stays inside the banner.
        const auto card = [&](float width) noexcept {
            ui_store_scene_begin(&g_scene);
            UiStoreBuilder page(&g_scene);
            auto& plate = page.open(UiStoreKind::card);
            plate.pad_x = plate.pad_y = plate.gap = 16;
            page.open(UiStoreKind::row).gap = 4;
            page.text(utf8_literal("What do you want to do in this community?"), UiStoreType::body_bold,
                      UiStoreInk::strong, ui_store_wrap);
            page.text(utf8_literal("*"), UiStoreType::body_bold, UiStoreInk::critical);
            page.close();
            page.open(UiStoreKind::row).gap = 8;
            page.chip(utf8_literal("A chip label much wider than the card it sits in"), UiStoreTone::neutral);
            page.close();
            page.open(UiStoreKind::flow).gap = 4;
            page.text(utf8_literal("Tip:"), UiStoreType::body_bold, UiStoreInk::normal);
            page.text(utf8_literal("Channels with mentions will always appear on your channel list, even if you hide them."),
                      UiStoreType::body, UiStoreInk::normal, ui_store_wrap);
            page.close();
            auto& banner = page.open(UiStoreKind::preview);
            banner.variant = static_cast<u8>(UiStoreAspect::fixed);
            banner.rh = 120;
            banner.pad_x = banner.pad_y = 16;
            auto& caption = page.open(UiStoreKind::row);
            caption.anchor = static_cast<u8>(UiStoreAnchor::bottom_left);
            caption.gap = 12;
            page.leaf(UiStoreKind::swatch).rw = 32;
            page.text(utf8_literal("A server name much longer than its banner is wide"), UiStoreType::heading,
                      UiStoreInk::strong);
            page.close();
            page.close();
            page.close();
            page.close();
            return ui_store_scene_commit(&g_scene, {0, 0, 900, 600}, {40, 30, 40 + width, 600}, {0, 0, 900, 600});
        };
        const auto node = [&](Utf8View text) noexcept -> const UiStoreNode& {
            u32 i = 0;
            while (i + 1 < g_scene.count && !utf8_equal(g_scene.nodes[i].text, text))
                ++i;
            return g_scene.nodes[i];
        };
        // Every text and chip lies inside the card's padding, and every child of the banner inside its insets.
        const auto inside = [&]() noexcept {
            const UiStoreNode* plate = nullptr;
            const UiStoreNode* banner = nullptr;
            bool in = true;
            for (u32 i = 0; i < g_scene.count; ++i) {
                const auto& n = g_scene.nodes[i];
                if (n.kind == UiStoreKind::card)
                    plate = &n;
                if (n.kind == UiStoreKind::preview)
                    banner = &n;
                if (!plate || (n.kind != UiStoreKind::text && n.kind != UiStoreKind::chip && n.kind != UiStoreKind::swatch))
                    continue;
                const float left = banner ? banner->x + banner->pad_x : plate->x + plate->pad_x;
                const float right = banner ? banner->x + banner->w - banner->pad_x : plate->x + plate->w - plate->pad_x;
                in = in && n.w > 0 && n.x >= left - .01f && n.x + n.w <= right + .01f && n.y >= plate->y + plate->pad_y &&
                     n.y + n.h <= plate->y + plate->h - plate->pad_y + .01f;
            }
            return in;
        };
        const Utf8View title = utf8_literal("What do you want to do in this community?");
        const Utf8View chip = utf8_literal("A chip label much wider than the card it sits in");
        const bool wide = card(800);
        const float title_width = node(title).w, chip_width = node(chip).w;
        check(wide && inside() && node(title).h == 20 && node(utf8_literal("*")).x == node(title).x + title_width + 4,
              "store scene: a wide card keeps the title on one line with the required mark beside it");
        const bool narrow = card(256);
        const auto& mark = node(utf8_literal("*"));
        check(narrow && inside() && node(title).h >= 40 && node(title).w < title_width && mark.w > 4 &&
                  node(chip).w < chip_width && node(chip).w <= 256 - 32,
              "store scene: a narrow card wraps its title, keeps the mark and ellipsizes a long chip, all inside the card");
    }
    {
        // The Server Hub banner (a red glow without a picture) under its scrim: the glow in the plates' layer, then in
        // the top layer (drawn after the pictures) a shade from 13.5% of the height and a foot from its middle, from
        // transparent to near black at its bottom. The foot is the dark theme's background, and the light theme keeps
        // it, as the official client does.
        ui_store_scene_begin(&g_scene);
        UiStoreBuilder page(&g_scene);
        auto& banner = page.open(UiStoreKind::preview);
        banner.variant = static_cast<u8>(UiStoreAspect::fixed);
        banner.rh = 240;
        banner.ink = static_cast<u8>(UiStoreInk::custom);
        banner.group = 0xE81E1E;
        banner.fill = static_cast<u8>(UiStoreFill::scrim);
        page.close();
        page.close();
        const bool laid = ui_store_scene_commit(&g_scene, {0, 0, 900, 600}, {40, 30, 840, 600}, {0, 0, 900, 600});
        const auto& node = g_scene.nodes[1];
        const float top = node.y + g_scene.column.top, bottom = top + 240;
        const auto scrim = [&](const Theme& theme) noexcept {
            PaintStream paint{};
            bool ok = laid && paint_stream_initialize(&paint) && paint_stream_reset(&paint, 900, 700) &&
                      ui_store_scene_paint(g_scene, theme, &paint, 0);
            const u32 plates = paint.count;
            ok = ok && ui_store_scene_paint(g_scene, theme, &paint, 1);
            u32 glow = paint.count, shades = 0;
            bool dark = false;
            for (u32 i = 0; ok && i < paint.count; ++i) {
                const auto& q = paint.quads[i];
                if (q.border_width == kPaintBannerGlowMode)
                    glow = i;
                if (q.border_width != -2 || i < glow || i < plates || q.bottom != bottom)
                    continue;
                ++shades;
                const bool middle = q.top > top + 119.9f && q.top < top + 120.1f;
                const auto near = [](float a, float b) noexcept { return a - b < .06f && b - a < .06f; };
                const Color& base = kDefaultTheme.base_lowest;
                dark = dark || (middle && q.fill.alpha == 0 && q.border.alpha == 1 && near(q.border.red, base.red) &&
                                near(q.border.green, base.green) && near(q.border.blue, base.blue));
            }
            bool none_below = true;
            for (u32 i = 0; i < plates; ++i)
                none_below = none_below && paint.quads[i].border_width != -2;
            const bool found = ok && glow < plates && none_below && shades == 2 && dark;
            paint_stream_release(&paint);
            return found;
        };
        check(scrim(kDefaultTheme) && scrim(kLightTheme),
              "store scene: the hub banner darkens toward near black at its bottom over its glow, in both themes");
    }
    ui_store_scene_release(&g_scene);
}

} // namespace maicord
