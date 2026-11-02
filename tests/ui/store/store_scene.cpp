#include "maicord/ui/store/store_scene.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/text/presentation.hpp"
#include "maicord/ui/accessibility/accessibility.hpp"

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
    ui_store_scene_release(&g_scene);
}

} // namespace maicord
