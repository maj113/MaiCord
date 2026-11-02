#include "../../../src/app/store/store_model.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/text/presentation.hpp"

// The store pages' projection and actions over hand-made models: chips and counts, search, filters and sorts,
// the Installed segments, the Updates list, what each activation asks the host to do, the menus, the dialogs'
// answers and the page tree the builder produces.
namespace maicord {
namespace {
using namespace app_detail;

StoreModel g_model;
UiStoreScene g_scene;

StoreItem& add(Utf8View slug, Utf8View name, Utf8View summary, Utf8View authors, Utf8View category, u8 risk,
               bool installed = false, bool enabled = false) noexcept {
    StoreItem& item = g_model.items[g_model.count++];
    reset_value(&item);
    item.kind = CatalogKind::plugin;
    item.slug = slug;
    item.name = name;
    item.summary = summary;
    item.authors = authors;
    item.version = utf8_literal("1.0.0");
    item.installed_version = item.version;
    item.categories[0] = category;
    item.category_count = 1;
    item.item = g_model.count - 1;
    item.risk = risk;
    item.installed = installed;
    item.enabled = enabled;
    item.installable = true;
    item.library = ~0u;
    return item;
}

void fixture() noexcept {
    reset_value(&g_model);
    g_model.catalog = StoreCatalogState::ready;
    g_model.installable = true;
    add(utf8_literal("clear-urls"), utf8_literal("Clear URLs"), utf8_literal("Removes tracking parameters from links you send."),
        utf8_literal("Maj"), utf8_literal("chat"), 1, true, true);
    StoreItem& tr = add(utf8_literal("translate"), utf8_literal("Translate"), utf8_literal("Translate messages with Google or DeepL."),
                        utf8_literal("MaiCord"), utf8_literal("chat"), 2);
    tr.released = 300;
    StoreItem& names = add(utf8_literal("show-me-your-name"), utf8_literal("Show Me Your Name"),
                           utf8_literal("Shows usernames next to nicknames."), utf8_literal("MaiCord"), utf8_literal("appearance"), 1,
                           true, true);
    names.update = true;
    names.update_version = utf8_literal("1.2.0");
    names.released = 200;
    add(utf8_literal("reverse-image-search"), utf8_literal("Reverse Image Search"), utf8_literal("Adds a Search image submenu."),
        utf8_literal("MaiCord"), utf8_literal("utility"), 0);
    StoreItem& silent = add(utf8_literal("silent-toggle"), utf8_literal("Silent Toggle"), utf8_literal("Sends your next message silently."),
                            utf8_literal("MaiCord"), utf8_literal("notifications"), 1, true, false);
    silent.released = 100;
    StoreItem& review = add(utf8_literal("pin-counter"), utf8_literal("Pin Counter"), utf8_literal("Counts pins."), utf8_literal("Lumen"),
                            utf8_literal("utility"), 0, true, true);
    review.item = ~0u;
    review.sideloaded = true;
    review.update = false;
}

bool same(Utf8View a, Utf8View b) noexcept {
    return a.bytes == b.bytes && bytes_equal(a.data, b.data, a.bytes);
}

void set_query(StorePageState* st, const char* text) noexcept {
    st->query_bytes = c_string_length(text);
    copy_bytes(st->query, text, st->query_bytes);
}
} // namespace

void test_store_page(void (*check)(bool, const char*) noexcept) noexcept {
    fixture();
    StorePageState st{};
    store_page_reset(&st, CatalogKind::plugin);
    // Chips: All, categories sorted by name, then Low access only; counts cover the listed items.
    StoreChips chips{};
    store_chips_build(g_model, CatalogKind::plugin, &chips);
    check(chips.count == 6 && same(chips.chips[0].label, utf8_literal("All")) && chips.chips[0].count == 5 &&
              same(chips.chips[1].label, utf8_literal("appearance")) && same(chips.chips[2].label, utf8_literal("chat")) &&
              chips.chips[2].count == 2 && same(chips.chips[4].label, utf8_literal("utility")) &&
              same(chips.chips[5].label, utf8_literal("Low access only")) && chips.chips[5].count == 1 &&
              chips.chips[5].group == 2,
          "store: chips are All, the categories by name, then Low access only, with counts of listed items");
    StoreList list{};
    store_browse_list(g_model, st, chips, &list);
    check(list.count == 5 && list.total == 5 && same(g_model.items[list.order[0]].name, utf8_literal("Clear URLs")) &&
              same(g_model.items[list.order[4]].name, utf8_literal("Translate")),
          "store: Browse lists the catalog's items by name and leaves out unlisted installs");
    st.sort = StoreSort::newest;
    store_browse_list(g_model, st, chips, &list);
    check(same(g_model.items[list.order[0]].name, utf8_literal("Translate")) && same(g_model.items[list.order[1]].name, utf8_literal("Show Me Your Name")),
          "store: Newest sorts by release date");
    st.sort = StoreSort::lightest;
    store_browse_list(g_model, st, chips, &list);
    check(g_model.items[list.order[0]].risk == 0 && g_model.items[list.order[4]].risk == 2,
          "store: Least access sorts by risk, then name");
    st.sort = StoreSort::name;
    // Search: every word must prefix a word of the name, summary, authors or categories; no keystroke allocates.
    set_query(&st, "transl");
    store_browse_list(g_model, st, chips, &list);
    check(list.count == 1 && same(g_model.items[list.order[0]].slug, utf8_literal("translate")), "store: search matches a word prefix");
    set_query(&st, "google deepl");
    store_browse_list(g_model, st, chips, &list);
    check(list.count == 1, "store: every search word must match, in any field");
    set_query(&st, "TRACKING");
    store_browse_list(g_model, st, chips, &list);
    check(list.count == 1 && same(g_model.items[list.order[0]].slug, utf8_literal("clear-urls")), "store: search is case-insensitive and reads summaries");
    set_query(&st, "lumen");
    store_browse_list(g_model, st, chips, &list);
    check(list.count == 0, "store: unlisted installs never appear in Browse");
    set_query(&st, "maicord utility");
    store_browse_list(g_model, st, chips, &list);
    check(list.count == 1 && same(g_model.items[list.order[0]].slug, utf8_literal("reverse-image-search")), "store: authors and categories are searched");
    st.query_bytes = 0;
    // Chips: OR within a group, AND across groups.
    st.chips = store_chip_toggle(0, chips, 2); // chat
    store_browse_list(g_model, st, chips, &list);
    check(list.count == 2, "store: one category chip filters");
    st.chips = store_chip_toggle(st.chips, chips, 1); // + appearance (same group: OR)
    store_browse_list(g_model, st, chips, &list);
    check(list.count == 3, "store: chips of one group add up");
    st.chips = store_chip_toggle(st.chips, chips, 4); // + utility (same group)
    st.chips = store_chip_toggle(st.chips, chips, 5); // + low access only (another group: AND)
    store_browse_list(g_model, st, chips, &list);
    check(list.count == 1 && g_model.items[list.order[0]].risk == 0, "store: chips of different groups intersect");
    st.chips = store_chip_toggle(st.chips, chips, 0);
    check(st.chips == 0, "store: choosing All clears the filters");
    // Installed.
    store_installed_list(g_model, st, &list);
    check(list.counts[0] == 4 && list.counts[1] == 3 && list.counts[2] == 1 && list.counts[3] == 1 && list.count == 4 &&
              store_installed_count(g_model, CatalogKind::plugin) == 4 && store_update_count(g_model, CatalogKind::plugin) == 1,
          "store: Installed counts all, on, off and updates");
    st.filter = StoreFilter::off;
    store_installed_list(g_model, st, &list);
    check(list.count == 1 && same(g_model.items[list.order[0]].slug, utf8_literal("silent-toggle")) && list.counts[0] == 4,
          "store: the Off segment lists what is turned off and keeps the counts");
    st.filter = StoreFilter::updates;
    store_installed_list(g_model, st, &list);
    check(list.count == 1 && g_model.items[list.order[0]].update, "store: the Updates segment lists items with updates");
    store_updates_list(g_model, &list);
    check(list.count == 1 && same(g_model.items[list.order[0]].update_version, utf8_literal("1.2.0")), "store: the Updates tab lists installed plugins with an update");
    st.filter = StoreFilter::all;
    // Installed lists in the order things were installed in, not by name.
    g_model.items[4].order = 0; // Silent Toggle
    g_model.items[2].order = 1; // Show Me Your Name
    g_model.items[5].order = 2; // Pin Counter
    g_model.items[0].order = 3; // Clear URLs
    store_installed_list(g_model, st, &list);
    check(list.count == 4 && list.order[0] == 4 && list.order[1] == 2 && list.order[2] == 5 && list.order[3] == 0,
          "store: the Installed list keeps the order things were installed in");
    for (auto& item : g_model.items)
        item.order = 0;
    check(store_initial_tab(g_model, CatalogKind::plugin) == StoreTab::installed &&
              store_initial_tab(g_model, CatalogKind::theme) == StoreTab::browse,
          "store: a page opens on Installed when something is installed");

}

static void store_tree(void (*check)(bool, const char*) noexcept) noexcept;

void test_store_actions(void (*check)(bool, const char*) noexcept) noexcept {
    fixture();
    StorePageState st{};
    store_page_reset(&st, CatalogKind::plugin);
    // Actions.
    const UiRect anchor{10, 10, 20, 20};
    StoreCommand c = store_page_act(g_model, &st, store_action(StoreAct::card, 1), 40, anchor);
    check(st.detail && same({st.slug, c_string_length(st.slug)}, utf8_literal("translate")) && st.saved_scroll == 40 &&
              c.kind == StoreCommandKind::request_details,
          "store: a card opens its detail view and remembers the scroll");
    c = store_page_act(g_model, &st, store_action(StoreAct::back), 0, anchor);
    check(!st.detail && c.flag, "store: back leaves the detail view");
    c = store_page_act(g_model, &st, store_action(StoreAct::card_primary, 1), 0, anchor);
    check(c.kind == StoreCommandKind::install && c.item == 1, "store: Install on a card installs");
    g_model.items[2].needs_review = false;
    c = store_page_act(g_model, &st, store_action(StoreAct::card_primary, 2), 0, anchor);
    check(c.kind == StoreCommandKind::update && c.item == 2, "store: an update without new permissions is one click");
    g_model.items[2].needs_review = true;
    c = store_page_act(g_model, &st, store_action(StoreAct::card_primary, 2), 0, anchor);
    check(c.kind == StoreCommandKind::review, "store: an update that needs review asks for review");
    g_model.items[2].needs_review = false;
    c = store_page_act(g_model, &st, store_action(StoreAct::card_switch, 0), 0, anchor);
    check(c.kind == StoreCommandKind::disable && c.item == 0, "store: the switch of an enabled plugin turns it off");
    c = store_page_act(g_model, &st, store_action(StoreAct::card_switch, 4), 0, anchor);
    check(c.kind == StoreCommandKind::enable, "store: the switch of a disabled plugin turns it on");
    c = store_page_act(g_model, &st, store_action(StoreAct::sort), 0, anchor);
    check(c.kind == StoreCommandKind::menu && c.menu == StoreMenu::sort && c.anchor.right == 20, "store: Sort opens a menu at its field");
    PopupItem rows[16]{};
    char labels[48];
    u32 n = store_menu_items(g_model, st, StoreMenu::sort, 0, rows, 16, {labels, sizeof(labels)});
    check(n == 3 && (rows[0].flags & popup_item_checked) && !(rows[1].flags & popup_item_checked), "store: the sort menu checks the current sort");
    store_menu_choose(g_model, &st, StoreMenu::sort, 0, 2);
    check(st.sort == StoreSort::lightest, "store: choosing a sort row sets it");
    st.sort = StoreSort::name;
    g_model.items[0].can_roll_back = true;
    g_model.items[0].previous_version = utf8_literal("0.9.2");
    g_model.items[0].has_settings = true;
    n = store_menu_items(g_model, st, StoreMenu::overflow, 0, rows, 16, {labels, sizeof(labels)});
    check(n == 5 && same(rows[2].label, utf8_literal("Roll back to 0.9.2")) && (rows[4].flags & popup_item_danger) &&
              (rows[4].flags & popup_item_separator_before),
          "store: the overflow menu lists Settings, catalog, roll back, automatic updates and Uninstall (critical)");
    c = store_menu_choose(g_model, &st, StoreMenu::overflow, 0, 2);
    check(c.kind == StoreCommandKind::rollback, "store: Roll back asks the host to roll back");
    c = store_menu_choose(g_model, &st, StoreMenu::overflow, 0, 4);
    check(st.remove_confirm && st.remove_item == 0, "store: Uninstall asks for confirmation first");
    c = store_page_act(g_model, &st, store_action(StoreAct::dialog_confirm), 0, anchor);
    check(c.kind == StoreCommandKind::remove && !st.remove_confirm && !c.flag, "store: confirming removes (keeping the plugin's data unless asked)");
    // A consent dialog answers once, with "turn it on now" as the user left it.
    g_model.consent.open = true;
    st.dialog_check = true;
    c = store_page_act(g_model, &st, store_action(StoreAct::dialog_confirm), 0, anchor);
    check(c.kind == StoreCommandKind::consent_answer && c.arg == 1 && c.flag, "store: Install and turn on answers yes and enables");
    c = store_page_act(g_model, &st, store_action(StoreAct::dialog_cancel), 0, anchor);
    check(c.kind == StoreCommandKind::consent_answer && c.arg == 0, "store: Cancel answers no");
    bool handled = false;
    c = store_page_escape(g_model, &st, &handled);
    check(handled && c.kind == StoreCommandKind::consent_answer && c.arg == 0, "store: Esc cancels a dialog first");
    g_model.consent.open = false;
    st.detail = true;
    c = store_page_escape(g_model, &st, &handled);
    check(handled && !st.detail, "store: Esc leaves a detail view");
    c = store_page_escape(g_model, &st, &handled);
    check(!handled, "store: Esc at the top level is the settings' to handle");
    check(store_permission_risk(u64{1} << 4, false) == 2, "store: network is high access");

    store_tree(check);
}

static void store_tree(void (*check)(bool, const char*) noexcept) noexcept {
    fixture();
    StorePageState st{};
    store_page_reset(&st, CatalogKind::plugin);
    check(ui_store_scene_open(&g_scene), "store: scene opens");
    st.tab = StoreTab::browse;
    st.chips = 0;
    ui_store_scene_begin(&g_scene);
    StoreLayoutInput in{&g_model, &st, nullptr, 792, false, nullptr, nullptr};
    store_page_build(&g_scene, in);
    const bool laid = ui_store_scene_commit(&g_scene, {0, 0, 900, 700}, {40, 28, 832, 700}, {0, 0, 900, 700});
    u32 cards = 0, switches = 0, installs = 0;
    for (u32 i = 0; i < g_scene.count; ++i) {
        cards += g_scene.nodes[i].kind == UiStoreKind::card && store_act(g_scene.nodes[i].action) == StoreAct::card;
        switches += g_scene.nodes[i].kind == UiStoreKind::toggle;
        installs += g_scene.nodes[i].kind == UiStoreKind::button && store_act(g_scene.nodes[i].action) == StoreAct::card_primary;
    }
            check(laid && cards == 5 && switches == 2 && installs == 3 && g_scene.page_root != kUiStoreNone,
          "store: Browse builds a card per listed plugin, a switch on installed ones, Install or Update on the rest");
    {
        PaintStream paint{};
        UiTextCommand commands[1024]{};
        UiTextCommandStream stream{};
        UiTextOutput output{&stream, nullptr};
        g_scene.active = true;
        const bool ready = paint_stream_initialize(&paint) && paint_stream_reset(&paint, 900, 700) &&
                           ui_text_command_stream_initialize(&stream, {commands, 1024});
        check(ready, "store: paint and text streams");
        check(ui_store_scene_paint(g_scene, kDefaultTheme, &paint, 0, 96), "store: the page paints plates");
        check(ui_store_scene_paint(g_scene, kDefaultTheme, &paint, 1, 96), "store: the page paints its foreground");
        check(ui_store_scene_text_output(g_scene, kDefaultTheme, &output, false) && stream.count > 20, "store: the page emits its text");
        paint_stream_release(&paint);
    }
    // Three columns at the store column's width, two when it narrows.
    float first_x = -1, third_x = -1;
    for (u32 i = 0, seen = 0; i < g_scene.count; ++i)
        if (g_scene.nodes[i].kind == UiStoreKind::card && store_act(g_scene.nodes[i].action) == StoreAct::card) {
            if (seen == 0)
                first_x = g_scene.nodes[i].x;
            if (seen == 2)
                third_x = g_scene.nodes[i].x;
            ++seen;
        }
    check(third_x > first_x + 400, "store: cards flow into three columns at 792 DIP");
    ui_store_scene_begin(&g_scene);
    store_page_build(&g_scene, in);
    ui_store_scene_commit(&g_scene, {0, 0, 700, 700}, {40, 28, 560, 700}, {0, 0, 700, 700});
    float second_x = -1;
    first_x = -1;
    for (u32 i = 0, seen = 0; i < g_scene.count; ++i)
        if (g_scene.nodes[i].kind == UiStoreKind::card && store_act(g_scene.nodes[i].action) == StoreAct::card) {
            if (seen == 0)
                first_x = g_scene.nodes[i].x;
            if (seen == 1)
                second_x = g_scene.nodes[i].x;
            ++seen;
        }
    check(second_x > first_x + 100 && second_x < first_x + 300, "store: the grid drops to two columns at 520 DIP");
    // Empty catalog: the unreachable state offers a retry.
    g_model.catalog = StoreCatalogState::unreachable;
    g_model.count = 0;
    ui_store_scene_begin(&g_scene);
    store_page_build(&g_scene, in);
    bool retry = false;
    for (u32 i = 0; i < g_scene.count; ++i)
        retry = retry || store_act(g_scene.nodes[i].action) == StoreAct::retry;
    check(ui_store_scene_commit(&g_scene, {0, 0, 900, 700}, {40, 28, 832, 700}, {0, 0, 900, 700}) && retry,
          "store: an unreachable catalog shows Try again");
    // The Developer tab: the console's lines are on the page, and Clear only when there is something to clear.
    g_model.catalog = StoreCatalogState::ready;
    g_model.developer_mode = true;
    g_model.console_count = 0;
    st.tab = StoreTab::developer;
    const auto console_page = [&](u32* lines, u32* clears) noexcept {
        ui_store_scene_begin(&g_scene);
        store_page_build(&g_scene, in);
        const bool laid = ui_store_scene_commit(&g_scene, {0, 0, 900, 700}, {40, 28, 832, 700}, {0, 0, 900, 700});
        *lines = *clears = 0;
        for (u32 i = 0; i < g_scene.count; ++i) {
            const auto& node = g_scene.nodes[i];
            *clears += store_act(node.action) == StoreAct::console_clear;
            for (u32 k = 0; k < g_model.console_count; ++k)
                *lines += node.text.bytes && node.text.bytes == g_model.console[k].text.bytes &&
                          bytes_equal(node.text.data, g_model.console[k].text.data, node.text.bytes);
        }
        return laid;
    };
    u32 lines = 0, clears = 0;
    check(console_page(&lines, &clears) && lines == 0 && clears == 0, "store: an empty console has no lines and nothing to clear");
    g_model.console[0] = {static_cast<u8>(1), utf8_literal("12:00:01"), utf8_literal("Hello"), utf8_literal("started")};
    g_model.console[1] = {static_cast<u8>(5), utf8_literal("12:00:02"), utf8_literal("Hello"), utf8_literal("Reloaded in 12 ms. State kept.")};
    g_model.console[2] = {static_cast<u8>(6), utf8_literal("12:00:03"), utf8_literal("Hello"), utf8_literal("Not reloaded: It isn't an x64 DLL.")};
    g_model.console_count = g_model.console_total = 3;
    check(console_page(&lines, &clears) && lines == 3 && clears == 1, "store: the Developer tab lists the console's lines and offers Clear");
    for (u32 i = 0; i < kStoreConsoleLines; ++i)
        g_model.console[i] = g_model.console[i % 3];
    g_model.console_count = kStoreConsoleLines;
    g_model.console_total = 40;
    check(console_page(&lines, &clears) && clears == 1 && lines >= kStoreConsoleLines, "store: a full console still lays out");
    ui_store_scene_release(&g_scene);
}

} // namespace maicord
