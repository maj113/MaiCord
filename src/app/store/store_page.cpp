#include "store_build.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/core/os_text.hpp"

namespace maicord::app_detail {
namespace store_build {
namespace {

struct Date {
    u32 year, month, day;
};
Date civil(u64 unix) noexcept {
    const i64 z = static_cast<i64>(unix / 86400) + 719468;
    const i64 era = (z >= 0 ? z : z - 146096) / 146097;
    const u64 doe = static_cast<u64>(z - era * 146097);
    const u64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const i64 y = static_cast<i64>(yoe) + era * 400;
    const u64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const u64 mp = (5 * doy + 2) / 153;
    const u32 d = static_cast<u32>(doy - (153 * mp + 2) / 5 + 1);
    const u32 m = static_cast<u32>(mp < 10 ? mp + 3 : mp - 9);
    return {static_cast<u32>(y + (m <= 2 ? 1 : 0)), m, d};
}
constexpr char kMonths[] = "Jan\0Feb\0Mar\0Apr\0May\0Jun\0Jul\0Aug\0Sep\0Oct\0Nov\0Dec\0";

} // namespace

Utf8View kind_plural(CatalogKind k) noexcept {
    return name_at("plugins\0themes\0effects\0", static_cast<u32>(k));
}
Utf8View kind_title(CatalogKind k) noexcept {
    return name_at("Plugins\0Themes\0Effects\0", static_cast<u32>(k));
}
Utf8View kind_noun(CatalogKind k) noexcept {
    return name_at("plugin\0theme\0effect\0", static_cast<u32>(k));
}
UiIcon kind_icon(CatalogKind k) noexcept {
    return k == CatalogKind::plugin ? UiIcon::puzzle : k == CatalogKind::theme ? UiIcon::palette : UiIcon::sparkles;
}
UiIcon category_icon(const StoreItem& item) noexcept {
    // The first category's glyph: each name of the list has the icon at its position.
    static constexpr UiIcon kIcons[]{UiIcon::message_text, UiIcon::palette,       UiIcon::bell,  UiIcon::shield, UiIcon::command,
                                     UiIcon::headphones,   UiIcon::accessibility, UiIcon::gavel, UiIcon::code,   UiIcon::emoji};
    if (!item.category_count)
        return UiIcon::puzzle;
    const Utf8View first = item.categories[0];
    u32 at = 0;
    for (NameList list{"chat\0appearance\0notifications\0privacy\0utility\0voice\0accessibility\0moderation\0developer\0fun\0"}; list;
         list.next(), ++at) {
        if (list.bytes != first.bytes)
            continue;
        u32 i = 0;
        while (i < first.bytes && (first.data[i] | 32) == list.at[i])
            ++i;
        if (i == first.bytes)
            return kIcons[at];
    }
    return UiIcon::puzzle;
}
Utf8View risk_label(u8 risk) noexcept {
    return risk >= 2 ? utf8_literal("High access") : risk == 1 ? utf8_literal("Medium access") : utf8_literal("Low access");
}
__declspec(noinline) Utf8View name_at(const char* names, u32 index) noexcept {
    return name_list_at(names, index);
}
Utf8View base_label(u32 base) noexcept {
    return name_at("Dark\0Light\0AMOLED\0", base < 3 ? base : 0);
}
UiStoreTone risk_tone(u8 risk) noexcept {
    return risk >= 2 ? UiStoreTone::risk_high : risk == 1 ? UiStoreTone::risk_medium : UiStoreTone::risk_low;
}
Utf8View cost_label(u8 cost) noexcept {
    return cost >= 2 ? utf8_literal("Heavy") : cost == 1 ? utf8_literal("Medium") : utf8_literal("Light");
}

Utf8View date_text(UiStoreBuilder& b, u64 unix) noexcept {
    if (!unix)
        return {};
    const Date d = civil(unix);
    return join(b, name_at(kMonths, d.month - 1), utf8_literal(" "), b.number(d.day), join(b, utf8_literal(", "), b.number(d.year)));
}
Utf8View size_text(UiStoreBuilder& b, u32 bytes) noexcept {
    if (bytes < 1024)
        return join(b, b.number(bytes), utf8_literal(" B"));
    const u32 unit = bytes < 1024 * 1024 ? 1024 : 1024 * 1024;
    const u64 tenths = (static_cast<u64>(bytes) * 10 + unit / 2) / unit;
    return join(b, b.number(tenths / 10), utf8_literal("."), b.number(tenths % 10),
                unit == 1024 ? utf8_literal(" KB") : utf8_literal(" MB"));
}
Utf8View byline(UiStoreBuilder& b, const StoreItem& item, bool listed) noexcept {
    // A Browse card names what the catalog offers; an installed item's own pages name what is installed.
    const Utf8View version = item.installed && item.installed_version.bytes && !(listed && item.version.bytes) ? item.installed_version : item.version;
    if (!item.authors.bytes)
        return join(b, utf8_literal("v"), version);
    return join(b, item.authors, utf8_literal(" \xC2\xB7 v"), version);
}

UiStoreNode& subtle(Ctx& c, const Utf8View& value, u16 flags) noexcept {
    return c.b.text(value, UiStoreType::small, UiStoreInk::subtle, flags);
}
void card_open(Ctx& c, UiStoreFill fill, u32 action) noexcept {
    auto& n = c.b.open(UiStoreKind::card);
    n.fill = static_cast<u8>(fill);
    n.action = action;
}
UiStoreNode& hairline_row(Ctx& c, bool last, float pad_x, float pad_y) noexcept {
    auto& n = c.b.open(UiStoreKind::row);
    n.pad_x = pad_x;
    n.pad_y = pad_y;
    n.gap = 12;
    n.flags = static_cast<u16>(ui_store_center | (last ? 0 : ui_store_hairline));
    return n;
}

void fact(Ctx& c, const char* label, const Utf8View& value, u32 action, u8 dot) noexcept {
    if (!value.bytes)
        return;
    auto& row = c.b.open(UiStoreKind::row);
    row.gap = 8;
    row.flags = dot ? ui_store_center : 0;
    c.b.text(label, UiStoreType::small, UiStoreInk::subtle).rw = 80;
    if (dot) {
        auto& mark = c.b.leaf(UiStoreKind::swatch);
        mark.variant = dot;
        mark.rw = mark.rh = 8;
    }
    if (action)
        c.b.button(value, UiStoreButton::link, action);
    else
        c.b.text(value, UiStoreType::small, UiStoreInk::normal, ui_store_flex);
    c.b.close();
}

void notice_card(Ctx& c, UiIcon icon, const Utf8View& title, const Utf8View& body, UiStoreFill fill, UiStoreInk ink) noexcept {
    card_open(c, fill);
    c.b.node().pad_x = 16;
    c.b.node().pad_y = 12;
    auto& row = c.b.open(UiStoreKind::row);
    row.gap = 12;
    auto& glyph = c.b.leaf(UiStoreKind::icon);
    glyph.icon = static_cast<u8>(icon);
    glyph.ink = static_cast<u8>(ink);
    auto& col = c.b.open(UiStoreKind::column);
    col.gap = 2;
    col.flags = ui_store_flex;
    if (title.bytes)
        c.b.text(title, UiStoreType::body_bold, ink);
    c.b.text(body, UiStoreType::small, UiStoreInk::normal, ui_store_wrap);
    c.b.close();
    c.b.close();
    c.b.close();
}

void build_empty(Ctx& c, UiIcon icon, const Utf8View& title, const Utf8View& body) noexcept {
    auto& col = c.b.open(UiStoreKind::column);
    col.pad_y = 72;
    auto& disc = c.b.leaf(UiStoreKind::disc);
    disc.icon = static_cast<u8>(icon);
    disc.flags = ui_store_center;
    c.b.spacer(0, 16);
    c.b.text(title, UiStoreType::body_bold, UiStoreInk::strong, ui_store_center | ui_store_wrap);
    c.b.spacer(0, 6);
    auto& txt = c.b.text(body, UiStoreType::small, UiStoreInk::subtle, ui_store_center | ui_store_wrap);
    txt.rw = 420;
}

// Centred buttons under an empty state; the caller closes the column afterwards.
void actions_row(Ctx& c) noexcept {
    c.b.spacer(0, 20);
    auto& row = c.b.open(UiStoreKind::row);
    row.gap = 8;
    row.flags = ui_store_center | ui_store_justify; // centred children
}

namespace {
void build_toolbar(Ctx& c, const Utf8View& placeholder) noexcept;
} // namespace

void build_skeleton(Ctx& c) noexcept {
    const auto kind = c.st.kind;
    build_toolbar(c, join(c.b, utf8_literal("Search "), kind_plural(kind)));
    c.b.spacer(0, 12);
    auto& bar = c.b.open(UiStoreKind::row);
    bar.gap = 8;
    for (u32 i = 0; i < 7; ++i) {
        auto& chip = c.b.leaf(UiStoreKind::bar);
        chip.rw = 56 + (i * 23) % 40;
        chip.rh = 32;
        chip.variant = 1;
    }
    c.b.close();
    c.b.spacer(0, 12);
    auto& line = c.b.leaf(UiStoreKind::bar);
    line.rw = 64;
    line.rh = 12;
    c.b.spacer(0, 12);
    auto& grid = c.b.open(UiStoreKind::grid);
    grid.gap = 16;
    grid.rw = 232;
    for (u32 i = 0; i < 6; ++i) {
        card_open(c, UiStoreFill::subtle);
        if (kind != CatalogKind::plugin) {
            auto& plate = c.b.open(UiStoreKind::preview);
            plate.variant = static_cast<u8>(kind == CatalogKind::theme ? UiStoreAspect::r16x10 : UiStoreAspect::r16x9);
            plate.flags = ui_store_clip | ui_store_busy;
            c.b.close();
        }
        auto& body = c.b.open(UiStoreKind::column);
        body.pad_x = body.pad_y = 16;
        body.gap = 8;
        const u32 widths[]{55, 35, 92, 70};
        const float heights[]{16, 12, 12, 12};
        for (u32 k = 0; k < 4; ++k) {
            auto& b = c.b.leaf(UiStoreKind::bar);
            b.count = static_cast<u16>(widths[k]);
            b.rw = 1;
            b.rh = heights[k];
        }
        c.b.spacer(0, 4);
        auto& foot = c.b.open(UiStoreKind::row);
        foot.gap = 8;
        for (u32 k = 0; k < 2; ++k) {
            auto& pill = c.b.leaf(UiStoreKind::bar);
            pill.rw = 56;
            pill.rh = 24;
            pill.variant = 1;
        }
        c.b.spacer(0, 0, true);
        auto& action = c.b.leaf(UiStoreKind::bar);
        action.rw = 64;
        action.rh = 32;
        action.variant = 2;
        c.b.close();
        c.b.close();
        c.b.close();
    }
    c.b.close();
}

void build_header(Ctx& c) noexcept {
    const auto kind = c.st.kind;
    auto& row = c.b.open(UiStoreKind::row);
    row.gap = 16;
    auto& col = c.b.open(UiStoreKind::column);
    col.gap = 8;
    col.flags = ui_store_flex;
    c.b.text(kind_title(kind), UiStoreType::title, UiStoreInk::strong);
    const Utf8View subtitle =
        kind == CatalogKind::plugin
            ? utf8_literal("Add features to MaiCord. Every catalog plugin is reviewed, built from its public source and signed, "
                           "and you approve what it may do before it runs.")
        : kind == CatalogKind::theme
            ? utf8_literal("Colours, type and motion for MaiCord's interface. A theme is plain data: it runs no code.")
            : utf8_literal("Animated backgrounds drawn by your GPU. An effect is a small shader with a few knobs; it runs "
                           "no code on your PC.");
    auto& sub = c.b.text(subtitle, UiStoreType::small, UiStoreInk::subtle, ui_store_wrap);
    sub.rw = 560;
    sub.count = 2;
    // Themes has two header actions: they sit under the subtitle and wrap, so a narrow column keeps both whole.
    if (kind == CatalogKind::theme) {
        auto& actions = c.b.open(UiStoreKind::flow);
        actions.gap = 8;
        // Import adds a theme file to the installed themes and applies it.
        auto& import = c.b.button("Import theme...", UiStoreButton::secondary, store_action(StoreAct::import_theme),
                                  UiIcon::upload, true);
        import.group = 1;
        if (c.m.theme_busy)
            import.flags |= ui_store_busy | ui_store_disabled;
        c.b.button("Install from link...", UiStoreButton::secondary, store_action(StoreAct::header_link), UiIcon::link,
                   true)
            .group = 1;
        c.b.close();
    }
    c.b.close();
    if (kind == CatalogKind::effect) {
        auto& b = c.b.button("Install from link...", UiStoreButton::secondary, store_action(StoreAct::header_link),
                             UiIcon::link, true);
        b.group = 1;
    }
    c.b.close();
    c.b.spacer(0, 16);
    // A theme that couldn't be read or saved says so above the tabs, with its retry.
    if (kind == CatalogKind::theme && c.m.theme_error) {
        card_open(c, UiStoreFill::critical_tint);
        c.b.node().pad_x = 16;
        c.b.node().pad_y = 12;
        auto& line = c.b.open(UiStoreKind::row);
        line.gap = 12;
        line.flags = ui_store_center;
        auto& glyph = c.b.leaf(UiStoreKind::icon);
        glyph.icon = static_cast<u8>(UiIcon::warning);
        glyph.ink = static_cast<u8>(UiStoreInk::critical);
        auto& text = c.b.open(UiStoreKind::column);
        text.gap = 2;
        text.flags = ui_store_flex;
        c.b.text(c.m.theme_error == 1 ? utf8_literal("Couldn't read theme file") : utf8_literal("Couldn't save theme"),
                 UiStoreType::body_bold, UiStoreInk::strong);
        subtle(c, c.m.theme_status, ui_store_wrap);
        c.b.close();
        if (c.m.theme_error == 1)
            c.b.button("Choose another file...", UiStoreButton::ghost, store_action(StoreAct::import_theme));
        c.b.button("Try again", UiStoreButton::secondary, store_action(StoreAct::import_theme, c.m.theme_error));
        c.b.close();
        c.b.close();
        c.b.spacer(0, 16);
    }
}

void build_tabs(Ctx& c) noexcept {
    const auto kind = c.st.kind;
    auto& row = c.b.open(UiStoreKind::row);
    row.gap = 24;
    row.flags = ui_store_hairline;
    // Tab slots: browse, installed, updates (plugins), developer (developer mode).
    for (u32 t = 0; t < 4; ++t) {
        if (t == 2 && kind != CatalogKind::plugin)
            continue;
        if (t == 3 && !(c.m.developer_mode && kind != CatalogKind::theme))
            continue;
        const StoreTab tab = static_cast<StoreTab>(t);
        auto& n = c.b.leaf(UiStoreKind::tab);
        n.text = name_at("Browse\0Installed\0Updates\0Developer\0", t);
        n.action = store_action(StoreAct::tab, t);
        n.group = 1;
        n.flags = tab == c.st.tab ? ui_store_selected : 0;
        const u32 count = tab == StoreTab::installed ? store_installed_count(c.m, kind)
                          : tab == StoreTab::updates ? store_update_count(c.m, kind)
                                                     : 0;
        if (count) {
            n.text2 = c.b.number(count);
            n.variant = tab == StoreTab::updates ? 1 : 0;
        }
    }
    c.b.close();
    c.b.spacer(0, 20);
}

namespace {

// The control at the end of a card or row footer for an item's state.
void card_control(Ctx& c, const StoreItem& item, u32 index, bool row) noexcept {
    const u32 primary = store_action(row ? StoreAct::row_primary : StoreAct::card_primary, index);
    const u32 toggle = store_action(row ? StoreAct::row_switch : StoreAct::card_switch, index);
    const u32 settings = store_action(row ? StoreAct::row_settings : StoreAct::card_settings, index);
    const u32 overflow = store_action(row ? StoreAct::row_overflow : StoreAct::card_overflow, index);
    const auto kind = item.kind;
    if (item.revoked) {
        c.b.chip("Turned off", UiStoreTone::dev);
        return;
    }
    if (item.busy) {
        auto& b = c.b.button(item.update ? utf8_literal("Updating...") : utf8_literal("Installing..."), UiStoreButton::secondary, 0);
        b.flags |= ui_store_busy | ui_store_disabled;
        return;
    }
    if (!item.installed) {
        if (item.incompatible) {
            c.b.chip("Needs a newer MaiCord", UiStoreTone::subtle);
            return;
        }
        auto& b = c.b.button("Install", UiStoreButton::secondary, primary);
        if (!item.installable)
            b.flags |= ui_store_disabled;
        return;
    }
    if (item.update) {
        auto& b = item.needs_review ? c.b.button("Review", UiStoreButton::secondary, primary)
                                    : c.b.button("Update", UiStoreButton::primary, primary, UiIcon::download);
        if (!item.installable)
            b.flags |= ui_store_disabled;
        if (!row)
            return;
    }
    if (kind == CatalogKind::plugin) {
        if (item.has_settings)
            c.b.icon_button(UiIcon::settings, "Settings", settings);
        if (row)
            c.b.icon_button(UiIcon::ellipsis, "More", overflow);
        auto& t = c.b.leaf(UiStoreKind::toggle);
        t.action = toggle;
        t.value = item.enabled ? 1 : 0;
        t.text = item.enabled ? utf8_literal("On") : utf8_literal("Off");
        return;
    }
    if (item.active && kind == CatalogKind::theme) {
        // The theme in use: a checked, disabled Applied; Remove and the catalog page stay in More.
        auto& applied = c.b.button("Applied", UiStoreButton::secondary, 0, UiIcon::check);
        applied.flags |= ui_store_disabled;
        c.b.icon_button(UiIcon::ellipsis, "More", overflow);
        return;
    }
    if (item.active) {
        if (row)
            c.b.chip(utf8_literal("Current"), UiStoreTone::positive, UiIcon::check);
        c.b.icon_button(UiIcon::filters, "Knobs", primary);
        return;
    }
    c.b.button(kind == CatalogKind::theme ? utf8_literal("Apply") : utf8_literal("Use"), UiStoreButton::primary, primary);
    if (row)
        c.b.icon_button(UiIcon::ellipsis, "More", overflow);
}

void build_card(Ctx& c, u32 index) noexcept {
    const StoreItem& item = c.m.items[index];
    const auto kind = item.kind;
    auto& card = c.b.open(UiStoreKind::card);
    card.action = store_action(StoreAct::card, index);
    card.fill = static_cast<u8>(UiStoreFill::subtle);
    card.group = 2;
    if (kind != CatalogKind::plugin) {
        auto& view = c.b.open(UiStoreKind::preview);
        view.variant = static_cast<u8>(kind == CatalogKind::theme ? UiStoreAspect::r16x10 : UiStoreAspect::r16x9);
        view.flags = ui_store_clip;
        view.value = index + 1;
        view.data = item.has_swatches ? item.swatches : nullptr;
        if (item.active) {
            auto& tag = c.b.chip(kind == CatalogKind::theme ? utf8_literal("Applied") : utf8_literal("Current"),
                                 UiStoreTone::positive, UiIcon::check);
            tag.anchor = static_cast<u8>(UiStoreAnchor::top_left);
            tag.flags = ui_store_foreground;
        }
        if (kind == CatalogKind::effect) {
            auto& pill = c.b.icon_button(UiIcon::play, "Play preview", 0, 28);
            pill.variant = 2;
            pill.anchor = static_cast<u8>(UiStoreAnchor::bottom_right);
            pill.flags = ui_store_foreground;
        }
        c.b.close();
    }
    auto& body = c.b.open(UiStoreKind::column);
    body.pad_x = body.pad_y = 16;
    body.gap = kind == CatalogKind::plugin ? 12 : 6;
    if (kind == CatalogKind::plugin) {
        auto& top = c.b.open(UiStoreKind::row);
        top.gap = 12;
        top.flags = ui_store_center;
        c.b.tile(category_icon(item), 40, UiStoreTone::subtle);
        auto& names = c.b.open(UiStoreKind::column);
        names.flags = ui_store_flex;
        c.b.text(item.name, UiStoreType::body_bold, UiStoreInk::strong);
        c.b.text(byline(c.b, item, true), UiStoreType::small, UiStoreInk::subtle);
        c.b.close();
        c.b.close();
    } else {
        c.b.text(item.name, UiStoreType::body_bold, UiStoreInk::strong);
        c.b.text(byline(c.b, item, true), UiStoreType::small, UiStoreInk::subtle);
    }
    auto& summary = c.b.text(item.summary, UiStoreType::small, UiStoreInk::normal, ui_store_wrap);
    summary.count = 2;
    summary.value = 2;
    auto& foot = c.b.open(UiStoreKind::row);
    foot.gap = kind == CatalogKind::theme ? 0 : 8;
    foot.rh = 32;
    foot.flags = ui_store_center;
    if (kind == CatalogKind::plugin)
        c.b.chip(risk_label(item.risk), risk_tone(item.risk));
    else if (kind == CatalogKind::theme) {
        if (item.has_swatches) {
            auto& stack = c.b.open(UiStoreKind::row);
            stack.gap = -4;
            for (u32 i = 0; i < 4; ++i) {
                auto& sw = c.b.leaf(UiStoreKind::swatch);
                sw.value = item.swatches[i];
                sw.rw = sw.rh = 16;
            }
            c.b.close();
        }
        c.b.spacer(8, 0);
        c.b.chip(base_label(item.base), UiStoreTone::subtle);
    } else {
        c.b.chip(cost_label(item.cost), risk_tone(item.cost));
        if (item.category_count)
            c.b.chip(item.categories[0], UiStoreTone::subtle);
    }
    c.b.spacer(0, 0, true);
    card_control(c, item, index, false);
    c.b.close();
    c.b.close();
    c.b.close();
}

void build_chips(Ctx& c, const StoreChips& chips) noexcept {
    auto& flow = c.b.open(UiStoreKind::flow);
    flow.gap = 8;
    u8 previous = 0;
    for (u32 i = 0; i < chips.count; ++i) {
        const auto& chip = chips.chips[i];
        if (i > 1 && chip.group != previous)
            c.b.leaf(UiStoreKind::rule).variant = 1;
        if (i)
            previous = chip.group;
        auto& n = c.b.leaf(UiStoreKind::filter);
        n.text = chip.label;
        if (chip.count)
            n.text2 = c.b.number(chip.count);
        n.action = store_action(StoreAct::chip, i);
        n.group = 3;
        if (i == 0 ? !c.st.chips : (c.st.chips >> i) & 1)
            n.flags = ui_store_selected;
        if (chip.type == 4)
            n.icon = static_cast<u8>(UiIcon::shield);
    }
    c.b.close();
}

// The page's search field, in the open row.
void search_field(Ctx& c, const Utf8View& placeholder) noexcept {
    auto& field = c.b.leaf(UiStoreKind::search);
    field.flags = ui_store_flex;
    field.action = store_action(StoreAct::search);
    field.group = 4;
    field.text = placeholder;
    field.text2 = utf8_literal("Ctrl F");
    field.value = c.st.query_bytes ? 1 : 0;
}

void build_toolbar(Ctx& c, const Utf8View& placeholder) noexcept {
    auto& row = c.b.open(UiStoreKind::row);
    row.gap = 8;
    search_field(c, placeholder);
    auto& dd = c.b.leaf(UiStoreKind::dropdown);
    dd.rw = 184;
    dd.text = utf8_literal("Sort");
    static constexpr char kSort[] = "Name\0Newest\0Least access\0", kSortLight[] = "Name\0Newest\0Lightest\0";
    const u32 s = static_cast<u32>(c.st.sort);
    // Themes sort by name or newest: a third choice reads as name.
    dd.text2 = name_at(c.st.kind == CatalogKind::effect ? kSortLight : kSort, c.st.kind == CatalogKind::theme && s >= 2 ? 0 : s);
    dd.action = store_action(StoreAct::sort);
    dd.group = 4;
    c.b.close();
}

Utf8View count_text(Ctx& c, u32 count) noexcept {
    return join(c.b, c.b.number(count), utf8_literal(" "),
                count == 1 ? kind_noun(c.st.kind) : kind_plural(c.st.kind));
}

void footnote(Ctx& c, const Utf8View& value) noexcept {
    c.b.spacer(0, 20);
    auto& row = c.b.open(UiStoreKind::row);
    row.gap = 8;
    row.flags = ui_store_center;
    auto& glyph = c.b.leaf(UiStoreKind::icon);
    glyph.icon = static_cast<u8>(UiIcon::info);
    glyph.ink = static_cast<u8>(UiStoreInk::subtle);
    c.b.text(value, UiStoreType::small, UiStoreInk::subtle, ui_store_flex | ui_store_wrap);
    c.b.close();
}

void protocol_notice(Ctx& c) noexcept {
    card_open(c);
    c.b.node().pad_x = 16;
    c.b.node().pad_y = 12;
    auto& row = c.b.open(UiStoreKind::row);
    row.gap = 12;
    row.flags = ui_store_center;
    c.b.text("Open maicord:// install links in MaiCord?", UiStoreType::body_bold, UiStoreInk::strong, ui_store_flex);
    c.b.button("Not now", UiStoreButton::secondary, store_action(StoreAct::protocol_off));
    c.b.button("Turn on", UiStoreButton::primary, store_action(StoreAct::protocol_on));
    c.b.close();
    c.b.close();
    c.b.spacer(0, 16);
}

} // namespace

void build_browse(Ctx& c) noexcept {
    const auto kind = c.st.kind;
    const bool listed = [&]() noexcept {
        for (u32 i = 0; i < c.m.count; ++i)
            if (c.m.items[i].kind == kind && c.m.items[i].item != ~0u)
                return true;
        return false;
    }();
    if (c.m.catalog == StoreCatalogState::not_configured) {
        build_empty(c, UiIcon::info, utf8_literal("The catalog isn't available in this build"),
                    utf8_literal("Developer builds don't include the catalog key."));
        c.b.close();
        return;
    }
    if (c.m.catalog == StoreCatalogState::loading && !listed) {
        build_skeleton(c);
        return;
    }
    if (c.m.catalog == StoreCatalogState::unreachable && (!listed || !c.st.show_saved)) {
        build_empty(c, UiIcon::cloud_off, join(c.b, utf8_literal("Can't reach the "), kind_noun(kind), utf8_literal(" catalog")),
                    join(c.b, utf8_literal("Check your connection and try again. "), kind_title(kind),
                         utf8_literal(" you've installed keep working, and MaiCord retries on its own.")));
        actions_row(c);
        c.b.button("Try again", UiStoreButton::primary, store_action(StoreAct::retry), UiIcon::refresh, true);
        if (listed)
            c.b.button(join(c.b, utf8_literal("Show the saved catalog ("), date_text(c.b, c.m.fetched_unix), utf8_literal(")")),
                       UiStoreButton::secondary, store_action(StoreAct::show_saved), UiIcon::count, true);
        c.b.close();
        if (c.m.error_text.bytes) {
            c.b.spacer(0, 16);
            auto& detail = c.b.open(UiStoreKind::row);
            detail.gap = 4;
            detail.flags = ui_store_center | ui_store_justify;
            c.b.text(join(c.b, utf8_literal("Error: "), c.m.error_text, utf8_literal(" \xC2\xB7 ")), UiStoreType::small, UiStoreInk::subtle);
            c.b.button("Copy details", UiStoreButton::link, store_action(StoreAct::copy_details));
            c.b.close();
        }
        c.b.close();
        return;
    }
    if (c.m.protocol_state == 1)
        protocol_notice(c);
    if (c.m.catalog == StoreCatalogState::stale || c.m.catalog == StoreCatalogState::unreachable) {
        notice_card(c, UiIcon::warning, utf8_literal("The catalog is out of date"),
                    join(c.b, utf8_literal("Showing the saved catalog from "), date_text(c.b, c.m.fetched_unix),
                         utf8_literal(". Installs need a fresh catalog.")),
                    UiStoreFill::warning_tint, UiStoreInk::warning);
        c.b.spacer(0, 16);
    }
    StoreChips chips{};
    store_chips_build(c.m, kind, &chips);
    StoreList list{};
    store_browse_list(c.m, c.st, chips, &list);
    build_toolbar(c, join(c.b, utf8_literal("Search "), c.b.number(list.total), utf8_literal(" "), kind_plural(kind)));
    c.b.spacer(0, 12);
    build_chips(c, chips);
    c.b.spacer(0, 16);
    auto& result = c.b.open(UiStoreKind::row);
    result.gap = 4;
    result.flags = ui_store_center;
    c.b.text(count_text(c, list.count), UiStoreType::small, UiStoreInk::subtle);
    if (c.st.chips || c.st.query_bytes) {
        c.b.text("\xC2\xB7", UiStoreType::small, UiStoreInk::subtle);
        c.b.button("Clear filters", UiStoreButton::link, store_action(StoreAct::clear_filters));
    }
    c.b.close();
    c.b.spacer(0, 12);
    if (!list.count) {
        const Utf8View query{c.st.query, c.st.query_bytes};
        build_empty(c, UiIcon::search,
                    query.bytes ? join(c.b, utf8_literal("No "), kind_plural(kind), utf8_literal(" match \""),
                                       join(c.b, query, utf8_literal("\"")))
                                : join(c.b, utf8_literal("No "), kind_plural(kind), utf8_literal(" match")),
                    utf8_literal("Try another word, or clear the filters."));
        actions_row(c);
        if (c.st.query_bytes)
            c.b.button("Clear search", UiStoreButton::secondary, store_action(StoreAct::clear_search), UiIcon::count, true);
        if (c.st.chips)
            c.b.button("Clear filters", UiStoreButton::ghost, store_action(StoreAct::clear_filters), UiIcon::count, true);
        c.b.close();
        c.b.close();
    } else {
        auto& grid = c.b.open(UiStoreKind::grid);
        grid.gap = 16;
        grid.rw = 232;
        for (u32 i = 0; i < list.count; ++i)
            build_card(c, list.order[i]);
        c.b.close();
    }
    footnote(c, kind == CatalogKind::plugin
                    ? utf8_literal("Plugins come from the reviewed MaiCord catalog. Developer Mode adds a Developer tab for "
                                   "loading your own builds.")
                    : join(c.b, kind_title(kind), utf8_literal(" come from the reviewed MaiCord catalog. \"Install from link\" "
                                                              "adds one that hasn't been reviewed.")));
}

namespace {

void installed_row(Ctx& c, const StoreItem& item, u32 index, bool last) noexcept {
    const auto kind = item.kind;
    auto& row = hairline_row(c, last);
    row.rh = 72;
    if (kind == CatalogKind::plugin)
        c.b.tile(item.sideloaded ? UiIcon::code : category_icon(item), 40, item.sideloaded ? UiStoreTone::dev : UiStoreTone::subtle);
    else {
        auto& thumb = c.b.open(UiStoreKind::preview);
        thumb.variant = static_cast<u8>(UiStoreAspect::fixed);
        thumb.rw = 64;
        thumb.rh = 40;
        thumb.value = index + 1;
        thumb.data = item.has_swatches ? item.swatches : nullptr;
        c.b.close();
    }
    auto& col = c.b.open(UiStoreKind::column);
    col.gap = 2;
    col.flags = ui_store_flex;
    auto& names = c.b.open(UiStoreKind::row);
    names.gap = 8;
    names.flags = ui_store_center;
    c.b.text(item.name, UiStoreType::body, UiStoreInk::strong);
    if (item.installed_version.bytes)
        c.b.text(join(c.b, utf8_literal("v"), item.installed_version), UiStoreType::small, UiStoreInk::subtle);
    if (item.local)
        c.b.chip("Local file", UiStoreTone::subtle, UiIcon::file);
    if (item.update)
        c.b.chip(join(c.b, utf8_literal("Update "), item.update_version), item.needs_review ? UiStoreTone::warning : UiStoreTone::brand);
    if (item.update && item.needs_review)
        c.b.chip("Needs review", UiStoreTone::warning);
    if (item.sideloaded)
        c.b.chip("Sideloaded", UiStoreTone::dev);
    else if (item.unreviewed)
        c.b.chip("Not reviewed", UiStoreTone::warning);
    if (item.revoked)
        c.b.chip("Turned off", UiStoreTone::dev);
    c.b.close();
    if (item.sideloaded && item.folder.bytes)
        subtle(c, join(c.b, utf8_literal("Loaded from "), item.folder, utf8_literal(" \xC2\xB7 reloads when you rebuild")));
    else
        subtle(c, item.summary);
    c.b.close();
    card_control(c, item, index, true);
    c.b.close();
}

void build_item_list(Ctx& c, const StoreList& list) noexcept {
    card_open(c);
    for (u32 i = 0; i < list.count; ++i)
        installed_row(c, c.m.items[list.order[i]], list.order[i], i + 1 == list.count);
    c.b.close();
}

} // namespace

void build_installed(Ctx& c) noexcept {
    const auto kind = c.st.kind;
    StoreList list{};
    store_installed_list(c.m, c.st, &list);
    if (kind == CatalogKind::effect)
        build_perf_defaults(c);
    if (!list.counts[0] && kind != CatalogKind::effect) {
        build_empty(c, kind_icon(kind),
                    join(c.b, utf8_literal("No "), kind_plural(kind), kind == CatalogKind::theme ? utf8_literal(" installed") : utf8_literal(" yet")),
                    kind == CatalogKind::plugin ? utf8_literal("Plugins add features to MaiCord. Each one tells you what it can do before it runs.")
                    : kind == CatalogKind::theme ? utf8_literal("The built-in themes live in Appearance.")
                                                 : utf8_literal("Stars and Comets are built in (Appearance)."));
        actions_row(c);
        c.b.button(join(c.b, utf8_literal("Browse "), kind_plural(kind)), UiStoreButton::primary, store_action(StoreAct::tab, static_cast<u32>(StoreTab::browse)),
                   UiIcon::count, true);
        if (kind == CatalogKind::theme) {
            c.b.button("Import theme", UiStoreButton::secondary, store_action(StoreAct::import_theme), UiIcon::count, true);
            c.b.button("Install from link", UiStoreButton::secondary, store_action(StoreAct::header_link), UiIcon::count, true);
        }
        c.b.close();
        c.b.close();
        return;
    }
    // Revoked items first: the page says why they are off.
    for (u32 i = 0; i < c.m.count; ++i) {
        const auto& item = c.m.items[i];
        if (item.kind != kind || !item.installed || !item.revoked)
            continue;
        notice_card(c, UiIcon::warning, {}, join(c.b, utf8_literal("MaiCord turned off "), item.name, utf8_literal(" because it was found harmful. "),
                                                  item.reason),
                    UiStoreFill::critical_tint, UiStoreInk::critical);
        c.b.spacer(0, 12);
    }
    const bool searchable = list.counts[0] > 8;
    if (kind == CatalogKind::plugin || searchable) {
        auto& bar = c.b.open(UiStoreKind::row);
        bar.gap = 8;
        search_field(c, join(c.b, utf8_literal("Search installed "), kind_plural(kind)));
        if (kind == CatalogKind::plugin) {
            auto& track = c.b.open(UiStoreKind::row);
            track.variant = 1;
            track.rw = 330;
            track.pad_x = track.pad_y = 4;
            static constexpr Utf8View kNames[]{utf8_literal("All "), utf8_literal("On "), utf8_literal("Off "), utf8_literal("Updates ")};
            for (u32 i = 0; i < 4; ++i) {
                auto& s = c.b.leaf(UiStoreKind::segment);
                s.text = join(c.b, kNames[i], c.b.number(list.counts[i]));
                s.action = store_action(StoreAct::segment, i);
                s.group = 5;
                if (static_cast<u32>(c.st.filter) == i)
                    s.flags = ui_store_selected;
            }
            c.b.close();
        }
        c.b.close();
        c.b.spacer(0, 16);
    }
    if (kind == CatalogKind::plugin && list.counts[3]) {
        card_open(c);
        c.b.node().pad_x = 16;
        c.b.node().pad_y = 12;
        auto& row = c.b.open(UiStoreKind::row);
        row.gap = 12;
        row.flags = ui_store_center;
        auto& glyph = c.b.leaf(UiStoreKind::icon);
        glyph.icon = static_cast<u8>(UiIcon::download);
        glyph.ink = static_cast<u8>(UiStoreInk::brand);
        auto& col = c.b.open(UiStoreKind::column);
        col.gap = 2;
        col.flags = ui_store_flex;
        c.b.text(list.counts[3] == 1 ? utf8_literal("1 update is ready") : join(c.b, c.b.number(list.counts[3]), utf8_literal(" updates are ready")),
                 UiStoreType::body_bold, UiStoreInk::strong);
        u32 first = ~0u, review = 0;
        for (u32 i = 0; i < c.m.count; ++i)
            if (c.m.items[i].kind == kind && c.m.items[i].installed && c.m.items[i].update) {
                if (first == ~0u)
                    first = i;
                review += c.m.items[i].needs_review ? 1 : 0;
            }
        if (list.counts[3] == 1 && first != ~0u)
            subtle(c, join(c.b, c.m.items[first].name, utf8_literal(" "), c.m.items[first].update_version,
                           c.m.items[first].needs_review ? utf8_literal(" asks for new permissions.") : utf8_literal(" asks for no new permissions.")));
        else
            subtle(c, review ? join(c.b, c.b.number(review), utf8_literal(" of them need your review.")) : utf8_literal("None of them asks for new permissions."));
        c.b.close();
        c.b.button(list.counts[3] == 1 ? utf8_literal("Update") : utf8_literal("Update all"), UiStoreButton::primary, store_action(StoreAct::banner_update));
        c.b.close();
        c.b.close();
        c.b.spacer(0, 16);
    }
    if (kind == CatalogKind::plugin) {
        u32 off = list.counts[2];
        subtle(c, off ? join(c.b, c.b.number(list.counts[0]), utf8_literal(" plugins \xC2\xB7 "), c.b.number(off), utf8_literal(" turned off"))
                      : join(c.b, c.b.number(list.counts[0]), list.counts[0] == 1 ? utf8_literal(" plugin") : utf8_literal(" plugins")));
        c.b.spacer(0, 12);
    }
    if (list.count)
        build_item_list(c, list);
    else if (list.counts[0]) {
        build_empty(c, UiIcon::search, utf8_literal("Nothing matches"), utf8_literal("Try another word, or pick another filter."));
        c.b.close();
    }
    if (kind == CatalogKind::plugin) {
        c.b.spacer(0, 16);
        auto& row = c.b.open(UiStoreKind::row);
        row.gap = 8;
        row.flags = ui_store_center;
        auto& glyph = c.b.leaf(UiStoreKind::icon);
        glyph.icon = static_cast<u8>(UiIcon::folder);
        glyph.ink = static_cast<u8>(UiStoreInk::subtle);
        c.b.text(MAICORD_OS_TEXT("Plugin files live in %LOCALAPPDATA%\\MaiCord\\plugins.",
                                 "Plugin files live in ~/.local/share/maicord/plugins."),
                 UiStoreType::small, UiStoreInk::subtle);
        c.b.button("Open folder", UiStoreButton::link, store_action(StoreAct::open_folder));
        c.b.close();
    }
}

void build_updates(Ctx& c) noexcept {
    StoreList list{};
    store_updates_list(c.m, &list);
    card_open(c);
    c.b.node().pad_x = 20;
    c.b.node().pad_y = 20;
    auto& head = c.b.open(UiStoreKind::row);
    head.gap = 16;
    head.flags = ui_store_center;
    c.b.tile(list.count ? UiIcon::download : UiIcon::circle_check, 48, list.count ? UiStoreTone::subtle : UiStoreTone::neutral);
    auto& col = c.b.open(UiStoreKind::column);
    col.gap = 2;
    col.flags = ui_store_flex;
    c.b.text(list.count ? (list.count == 1 ? utf8_literal("1 update available") : join(c.b, c.b.number(list.count), utf8_literal(" updates available")))
                        : utf8_literal("Your plugins are up to date"),
             UiStoreType::heading, UiStoreInk::strong);
    subtle(c, c.m.checked_unix ? join(c.b, utf8_literal("Last checked "), date_text(c.b, c.m.checked_unix)) : utf8_literal("Not checked yet"));
    c.b.close();
    if (list.count)
        c.b.button("Update all", UiStoreButton::primary, store_action(StoreAct::update_all), UiIcon::download, true);
    c.b.button("Check now", UiStoreButton::secondary, store_action(StoreAct::check_now), UiIcon::refresh, true);
    c.b.close();
    c.b.close();
    c.b.spacer(0, 16);
    card_open(c);
    c.b.node().pad_x = 16;
    c.b.node().pad_y = 14;
    auto& auto_row = c.b.open(UiStoreKind::row);
    auto_row.gap = 16;
    auto_row.flags = ui_store_center;
    auto& names = c.b.open(UiStoreKind::column);
    names.gap = 2;
    names.flags = ui_store_flex;
    c.b.text("Update official plugins automatically", UiStoreType::body, UiStoreInk::strong);
    subtle(c, utf8_literal("Plugins that ask for no new permissions update on their own. Others wait for your review."), ui_store_wrap);
    c.b.close();
    auto& t = c.b.leaf(UiStoreKind::toggle);
    t.action = store_action(StoreAct::auto_update_all);
    t.value = c.m.auto_update_all ? 1 : 0;
    c.b.close();
    c.b.close();
    if (!list.count)
        return;
    c.b.spacer(0, 16);
    card_open(c);
    for (u32 i = 0; i < list.count; ++i) {
        const StoreItem& item = c.m.items[list.order[i]];
        const bool open = i < 8 && c.st.updates_open[i];
        auto& row = hairline_row(c, i + 1 == list.count && !open);
        row.rh = 72;
        c.b.tile(category_icon(item), 40, UiStoreTone::subtle);
        auto& body = c.b.open(UiStoreKind::column);
        body.gap = 2;
        body.flags = ui_store_flex;
        auto& line = c.b.open(UiStoreKind::row);
        line.gap = 8;
        line.flags = ui_store_center;
        c.b.text(item.name, UiStoreType::body, UiStoreInk::strong);
        if (item.needs_review)
            c.b.chip("Needs review", UiStoreTone::warning);
        c.b.close();
        subtle(c, join(c.b, item.installed_version, utf8_literal(" \xE2\x86\x92 "), item.update_version));
        c.b.close();
        c.b.icon_button(open ? UiIcon::chevron_up : UiIcon::chevron_down, "Changelog", store_action(StoreAct::updates_expand, i));
        auto& b = item.needs_review ? c.b.button("Review", UiStoreButton::secondary, store_action(StoreAct::row_primary, list.order[i]))
                                    : c.b.button("Update", UiStoreButton::primary, store_action(StoreAct::row_primary, list.order[i]));
        if (!item.installable || item.busy)
            b.flags |= ui_store_disabled;
        c.b.close();
        if (open) {
            auto& note = c.b.open(UiStoreKind::column);
            note.pad_x = 84;
            note.pad_y = 12;
            note.flags = i + 1 == list.count ? 0 : ui_store_hairline;
            subtle(c, item.changelog.bytes ? item.changelog : utf8_literal("No release notes."), ui_store_wrap);
            c.b.close();
        }
    }
    c.b.close();
}

void build_developer(Ctx& c) noexcept {
    const auto kind = c.st.kind;
    card_open(c);
    {
        auto& row = hairline_row(c, false);
        row.rh = 64;
        auto& col = c.b.open(UiStoreKind::column);
        col.gap = 2;
        col.flags = ui_store_flex;
        c.b.text("Dev folder", UiStoreType::body, UiStoreInk::strong);
        c.b.text(c.m.dev_folder, UiStoreType::mono, UiStoreInk::subtle);
        c.b.close();
        c.b.button("Open", UiStoreButton::ghost, store_action(StoreAct::open_folder));
        c.b.close();
    }
    {
        auto& row = hairline_row(c, false);
        row.rh = 56;
        auto& col = c.b.open(UiStoreKind::column);
        col.gap = 2;
        col.flags = ui_store_flex;
        c.b.text(kind == CatalogKind::plugin ? MAICORD_OS_TEXT("Load a plugin DLL...", "Load a plugin...") : utf8_literal("Load a .mcfx..."), UiStoreType::body, UiStoreInk::strong);
        subtle(c, kind == CatalogKind::plugin ? utf8_literal("Asks for your consent as an unreviewed plugin with full access to your PC.")
                                              : utf8_literal("Shows the not-reviewed card with a preview before it is installed."),
               ui_store_wrap);
        c.b.close();
        c.b.button("Choose file...", UiStoreButton::secondary, store_action(StoreAct::dev_load));
        c.b.close();
    }
    {
        auto& row = hairline_row(c, true);
        row.rh = 56;
        auto& col = c.b.open(UiStoreKind::column);
        col.gap = 2;
        col.flags = ui_store_flex;
        c.b.text("Reload", UiStoreType::body, UiStoreInk::strong);
        subtle(c, utf8_literal("Scan the dev folder again now."));
        c.b.close();
        c.b.button("Reload", UiStoreButton::secondary, store_action(StoreAct::dev_reload));
        c.b.close();
    }
    c.b.close();
    u32 loaded = 0;
    for (u32 i = 0; i < c.m.count; ++i)
        loaded += c.m.items[i].kind == kind && c.m.items[i].installed && c.m.items[i].sideloaded ? 1 : 0;
    if (loaded) {
        c.b.spacer(0, 16);
        card_open(c);
        u32 seen = 0;
        for (u32 i = 0; i < c.m.count; ++i) {
            const auto& item = c.m.items[i];
            if (item.kind == kind && item.installed && item.sideloaded)
                installed_row(c, item, i, ++seen == loaded);
        }
        c.b.close();
    }
    if (kind != CatalogKind::plugin)
        return;
    // The console: what the plugins log, the time-budget strikes and the reloads (src/app/plugins/dev_console.hpp).
    c.b.spacer(0, 16);
    card_open(c);
    {
        hairline_row(c, !c.m.console_count);
        auto& col = c.b.open(UiStoreKind::column);
        col.gap = 2;
        col.flags = ui_store_flex;
        c.b.text("Console", UiStoreType::body, UiStoreInk::strong);
        subtle(c, c.m.console_total > c.m.console_count
                      ? join(c.b, utf8_literal("The newest "), c.b.number(c.m.console_count), utf8_literal(" of "), c.b.number(c.m.console_total))
                      : utf8_literal("Plugin logs, time-budget strikes and reloads."));
        c.b.close();
        if (c.m.console_count)
            c.b.button("Clear", UiStoreButton::ghost, store_action(StoreAct::console_clear));
        c.b.close();
    }
    for (u32 i = 0; i < c.m.console_count; ++i) {
        const StoreConsoleLine& line = c.m.console[i];
        // debug, info, warning, error, strike, reload, problem
        static constexpr UiStoreInk kInk[]{UiStoreInk::subtle, UiStoreInk::normal, UiStoreInk::warning, UiStoreInk::critical,
                                           UiStoreInk::warning, UiStoreInk::positive, UiStoreInk::critical};
        hairline_row(c, i + 1 == c.m.console_count, 16, 6);
        c.b.text(join(c.b, line.time, utf8_literal("  "), line.name, utf8_literal("  ")), UiStoreType::mono, UiStoreInk::subtle);
        c.b.text(line.text, UiStoreType::mono, kInk[line.kind < 7 ? line.kind : 1], ui_store_flex);
        c.b.close();
    }
    if (!c.m.console_count) {
        hairline_row(c, true);
        subtle(c, utf8_literal("Nothing yet. Build a plugin with maicord-plugin dev and its ctx.log lines show here."), ui_store_wrap);
        c.b.close();
    }
    c.b.close();
}

} // namespace store_build

} // namespace maicord::app_detail
