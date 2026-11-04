#include "store_build.hpp"
#include "maicord/core/ascii.hpp"

namespace maicord::app_detail::store_build {
namespace {

void heading(Ctx& c, const char* value) noexcept {
    c.b.spacer(0, 24);
    c.b.text(value, UiStoreType::heading, UiStoreInk::strong);
    c.b.spacer(0, 8);
}

void markdown(Ctx& c, const void* block, const Utf8View& plain) noexcept {
    auto& n = c.b.leaf(UiStoreKind::markdown);
    n.data = block;
    n.text = plain;
    n.type = static_cast<u8>(UiStoreType::paragraph);
}

Utf8View tab_name(StoreTab t) noexcept {
    return t == StoreTab::installed ? utf8_literal("Installed") : t == StoreTab::updates ? utf8_literal("Updates") : utf8_literal("Browse");
}

void back_link(Ctx& c, const Utf8View& label) noexcept {
    auto& b = c.b.button(label, UiStoreButton::ghost, store_action(StoreAct::back), UiIcon::arrow_left);
    b.group = 7;
    c.b.spacer(0, 12);
}

void meta_row(Ctx& c, const StoreItem& item) noexcept {
    auto& row = c.b.open(UiStoreKind::flow);
    row.gap = 8;
    if (item.authors.bytes)
        c.b.text(join(c.b, utf8_literal("by "), item.authors, utf8_literal("  \xC2\xB7")), UiStoreType::small, UiStoreInk::subtle);
    if (item.released)
        c.b.text(join(c.b, utf8_literal("Updated "), date_text(c.b, item.released)), UiStoreType::small, UiStoreInk::subtle);
    if (item.item != ~0u && !item.unreviewed && !item.sideloaded)
        c.b.chip("Reviewed", UiStoreTone::positive, UiIcon::circle_check);
    if (item.unreviewed)
        c.b.chip("Not reviewed", UiStoreTone::warning);
    if (item.local)
        c.b.chip("Local file", UiStoreTone::subtle, UiIcon::file);
    if (item.kind == CatalogKind::theme) {
        c.b.chip(base_label(item.base), UiStoreTone::subtle);
    }
    for (u32 i = 0; i < item.category_count; ++i)
        c.b.chip(item.categories[i], UiStoreTone::subtle);
    c.b.close();
}

void rail(Ctx& c, const StoreItem& item, u32 index) noexcept {
    const auto kind = item.kind;
    auto& col = c.b.open(UiStoreKind::column);
    col.gap = 12;
    // Primary action card.
    card_open(c);
    c.b.node().pad_x = c.b.node().pad_y = 16;
    c.b.node().gap = 12;
    auto stretch = [&](UiStoreNode& n) noexcept -> UiStoreNode& {
        n.flags |= ui_store_stretch;
        n.type = 1;
        return n;
    };
    Utf8View caption{};
    const u32 primary = store_action(StoreAct::detail_primary, index), secondary = store_action(StoreAct::detail_secondary, index);
    if (item.revoked) {
        caption = utf8_literal("MaiCord turned this off because it was found harmful.");
    } else if (item.busy) {
        stretch(c.b.button(item.update ? utf8_literal("Updating...") : utf8_literal("Installing..."), UiStoreButton::primary, 0)).flags |=
            ui_store_busy | ui_store_disabled;
    } else if (!item.installed) {
        auto& b = stretch(c.b.button("Install", UiStoreButton::primary, primary, UiIcon::download));
        if (!item.installable || item.incompatible)
            b.flags |= ui_store_disabled;
        caption = item.incompatible ? utf8_literal("This needs a newer MaiCord.")
                  : kind == CatalogKind::plugin ? utf8_literal("You'll see what it can do and approve it before it runs.")
                  : kind == CatalogKind::theme  ? utf8_literal("A theme is data. It installs in one click.")
                                                : utf8_literal("An effect is data. It runs on your GPU, never as code on your PC.");
    } else if (item.update) {
        auto& b = stretch(c.b.button(join(c.b, item.needs_review ? utf8_literal("Review ") : utf8_literal("Update to "), item.update_version),
                                     UiStoreButton::primary, primary, UiIcon::download));
        if (!item.installable)
            b.flags |= ui_store_disabled;
        caption = item.needs_review ? utf8_literal("This version asks for more than you approved before.") : Utf8View{};
    } else if (kind == CatalogKind::plugin) {
        stretch(c.b.button(item.enabled ? utf8_literal("Turn off") : utf8_literal("Turn on"),
                           item.enabled ? UiStoreButton::secondary : UiStoreButton::primary, primary));
        if (item.has_settings)
            stretch(c.b.button("Settings", UiStoreButton::secondary, secondary, UiIcon::settings));
    } else if (kind == CatalogKind::theme) {
        auto& b = stretch(c.b.button(item.active ? utf8_literal("Applied") : utf8_literal("Apply theme"), UiStoreButton::primary, primary, UiIcon::check));
        if (item.active)
            b.flags |= ui_store_disabled;
        caption = utf8_literal("Installed.");
    } else {
        auto& b = stretch(c.b.button(item.active ? utf8_literal("Current background") : utf8_literal("Use as background"), UiStoreButton::primary, primary));
        if (item.active)
            b.flags |= ui_store_disabled;
        stretch(c.b.button("Knobs", UiStoreButton::secondary, secondary, UiIcon::filters));
    }
    // Try it is secondary: a moment's preview of an installed theme, never needed to apply one. The theme in use
    // offers the way back to the last built-in theme instead.
    if (kind == CatalogKind::theme && item.installed && !item.update && !item.revoked)
        stretch(c.b.button(item.active ? utf8_literal("Use built-in theme") : utf8_literal("Try it for a moment"),
                           UiStoreButton::secondary, secondary, item.active ? UiIcon::palette : UiIcon::eye));
    if (caption.bytes)
        c.b.text(caption, UiStoreType::small, UiStoreInk::subtle, ui_store_wrap);
    c.b.leaf(UiStoreKind::rule);
    auto& facts = c.b.open(UiStoreKind::column);
    facts.gap = 8;
    fact(c, "Version", item.installed ? item.installed_version : item.version);
    fact(c, "Released", date_text(c.b, item.released));
    fact(c, "Size", item.bytes ? size_text(c.b, item.bytes) : Utf8View{});
    fact(c, "License", item.license, item.license.bytes ? store_action(StoreAct::detail_source, 1) : 0);
    if (kind == CatalogKind::plugin && item.api_level)
        fact(c, "Plugin API", join(c.b, utf8_literal("Level "), c.b.number(item.api_level)));
    if (kind == CatalogKind::theme) {
        fact(c, "Base", base_label(item.base));
        fact(c, "Contrast", item.contrast);
        fact(c, "Effect", item.effect_slug.bytes ? item.effect_slug : utf8_literal("None"));
    }
    if (kind == CatalogKind::effect)
        fact(c, "Cost", cost_label(item.cost));
    if (item.sha.bytes) {
        auto& row = c.b.open(UiStoreKind::row);
        row.gap = 8;
        c.b.text("SHA-256", UiStoreType::small, UiStoreInk::subtle).rw = 80;
        c.b.text(item.sha, UiStoreType::mono, UiStoreInk::normal, ui_store_flex);
        c.b.icon_button(UiIcon::copy, "Copy SHA-256", store_action(StoreAct::detail_copy_sha, index), 24);
        c.b.close();
    }
    c.b.close();
    c.b.close();
    if (item.source.bytes) {
        auto& b = c.b.button("View source", UiStoreButton::secondary, store_action(StoreAct::detail_source, 0), UiIcon::code, true);
        b.icon2 = static_cast<u8>(UiIcon::external_link);
        b.flags |= ui_store_stretch;
    }
    if (item.installed) {
        auto& b = c.b.button(item.kind == CatalogKind::plugin ? utf8_literal("Uninstall...") : utf8_literal("Remove"),
                             UiStoreButton::ghost_critical, store_action(StoreAct::detail_remove, index), UiIcon::trash, true);
        b.flags |= ui_store_stretch;
    }
    if (item.commit.bytes) {
        card_open(c, UiStoreFill::none);
        c.b.node().pad_x = c.b.node().pad_y = 12;
        auto& row = c.b.open(UiStoreKind::row);
        row.gap = 8;
        auto& glyph = c.b.leaf(UiStoreKind::icon);
        glyph.icon = static_cast<u8>(UiIcon::shield);
        glyph.ink = static_cast<u8>(UiStoreInk::subtle);
        c.b.text(join(c.b, utf8_literal("Built by MaiCord's CI from the reviewed source at commit "), item.commit, utf8_literal(" and signed.")),
                 UiStoreType::small, UiStoreInk::normal, ui_store_flex | ui_store_wrap);
        c.b.close();
        c.b.close();
    }
    c.b.close();
}

void credit(Ctx& c, const StoreItem& item) noexcept {
    heading(c, "Credit");
    c.b.text(join(c.b, utf8_literal("Made by "), item.authors.bytes ? item.authors : utf8_literal("its author"),
                  item.license.bytes ? join(c.b, utf8_literal(", under the "), item.license, utf8_literal(" license.")) : utf8_literal(".")),
             UiStoreType::paragraph, UiStoreInk::normal, ui_store_wrap);
}

} // namespace

void access_eyebrow(Ctx& c, u8 risk, float pad_y) noexcept {
    auto& eyebrow = c.b.open(UiStoreKind::row);
    eyebrow.gap = 8;
    eyebrow.pad_y = pad_y;
    eyebrow.flags = ui_store_center;
    auto& dot = c.b.leaf(UiStoreKind::swatch);
    dot.variant = static_cast<u8>(risk >= 2 ? 3 : risk == 1 ? 4 : 5);
    dot.rw = dot.rh = 8;
    c.b.text(name_at("LOW ACCESS\0MEDIUM ACCESS\0HIGH ACCESS\0", risk), UiStoreType::eyebrow, UiStoreInk::header);
    c.b.close();
}

// The hosts a permission named `network` lists, as chips under its sentence.
void host_chips(Ctx& c, const StorePermission& p, ConstSlice<Utf8View> hosts) noexcept {
    if (p.name.bytes != 7 || p.name.data[0] != 'n' || !hosts.count)
        return;
    auto& flow = c.b.open(UiStoreKind::flow);
    flow.gap = 8;
    for (u32 h = 0; h < hosts.count; ++h)
        c.b.chip(hosts[h], UiStoreTone::mono);
    c.b.close();
}

void permission_group(Ctx& c, u8 risk, const StorePermission* rows, u32 count, ConstSlice<Utf8View> hosts) noexcept {
    u32 shown = 0;
    for (u32 i = 0; i < count; ++i)
        shown += rows[i].risk == risk ? 1 : 0;
    if (!shown)
        return;
    access_eyebrow(c, risk, 4);
    card_open(c);
    u32 seen = 0;
    for (u32 i = 0; i < count; ++i) {
        const auto& p = rows[i];
        if (p.risk != risk)
            continue;
        auto& row = hairline_row(c, ++seen == shown, 16, 10);
        row.rh = 48;
        c.b.tile(p.glyph, 28, UiStoreTone::neutral);
        auto& col = c.b.open(UiStoreKind::column);
        col.gap = 6;
        col.flags = ui_store_flex;
        c.b.text(p.sentence, UiStoreType::body, p.critical ? UiStoreInk::critical : UiStoreInk::strong, ui_store_wrap);
        host_chips(c, p, hosts);
        if (p.note.bytes)
            c.b.text(p.note, UiStoreType::small, UiStoreInk::subtle, ui_store_wrap);
        c.b.close();
        c.b.text(p.name, UiStoreType::mono, UiStoreInk::subtle);
        c.b.close();
    }
    c.b.close();
    c.b.spacer(0, 12);
}

void build_detail(Ctx& c, const StoreItem& item, u32 index) noexcept {
    const auto kind = item.kind;
    back_link(c, tab_name(c.st.detail_from));
    if (kind == CatalogKind::plugin) {
        auto& top = c.b.open(UiStoreKind::row);
        top.gap = 16;
        c.b.tile(category_icon(item), 56, UiStoreTone::subtle);
        auto& col = c.b.open(UiStoreKind::column);
        col.gap = 4;
        col.flags = ui_store_flex;
        c.b.text(item.name, UiStoreType::title, UiStoreInk::strong);
        c.b.text(item.summary, UiStoreType::paragraph, UiStoreInk::normal, ui_store_wrap);
        c.b.spacer(0, 4);
        meta_row(c, item);
        c.b.close();
        c.b.close();
    } else {
        c.b.text(item.name, UiStoreType::title, UiStoreInk::strong);
        c.b.spacer(0, 4);
        c.b.text(item.summary, UiStoreType::paragraph, UiStoreInk::normal, ui_store_wrap);
        c.b.spacer(0, 8);
        meta_row(c, item);
    }
    c.b.spacer(0, 24);
    auto& split = c.b.open(UiStoreKind::split);
    split.rw = 264;
    split.gap = 24;
    c.b.open(UiStoreKind::column);
    if (item.revoked)
        notice_card(c, UiIcon::warning, join(c.b, utf8_literal("Turned off: "), item.reason), {}, UiStoreFill::critical_tint, UiStoreInk::critical);
    else if (item.incompatible)
        notice_card(c, UiIcon::warning, utf8_literal("Needs a newer MaiCord"), utf8_literal("Update MaiCord to install this."), UiStoreFill::warning_tint,
                    UiStoreInk::warning);
    if (kind == CatalogKind::theme || kind == CatalogKind::effect) {
        auto& view = c.b.open(UiStoreKind::preview);
        view.variant = static_cast<u8>(kind == CatalogKind::theme ? UiStoreAspect::r16x10 : UiStoreAspect::r16x9);
        view.value = index + 1;
        view.data = item.has_swatches ? item.swatches : nullptr;
        auto& pill = c.b.chip(kind == CatalogKind::theme ? utf8_literal("Main chat view") : utf8_literal("Preview"), UiStoreTone::scrim,
                              kind == CatalogKind::theme ? UiIcon::image : UiIcon::play);
        pill.anchor = static_cast<u8>(UiStoreAnchor::bottom_left);
        pill.flags = ui_store_foreground;
        c.b.close();
    }
    if (kind == CatalogKind::theme && item.has_swatches) {
        heading(c, "Colours");
        auto& flow = c.b.open(UiStoreKind::flow);
        flow.gap = 16;
        static constexpr Utf8View kRoles[]{utf8_literal("Background"), utf8_literal("Surface"), utf8_literal("Text"), utf8_literal("Accent")};
        for (u32 i = 0; i < 4; ++i) {
            auto& cell = c.b.open(UiStoreKind::row);
            cell.gap = 8;
            cell.flags = ui_store_center;
            auto& sw = c.b.leaf(UiStoreKind::swatch);
            sw.variant = 1;
            sw.value = item.swatches[i];
            sw.rw = sw.rh = 40;
            c.b.open(UiStoreKind::column);
            c.b.text(kRoles[i], UiStoreType::small_bold, UiStoreInk::strong);
            char hex[8]{'#'};
            for (u32 k = 0; k < 6; ++k)
                hex[1 + k] = ascii_hex_digit(item.swatches[i] >> (20 - 4 * k));
            c.b.text(c.b.copy({hex, 7}), UiStoreType::mono, UiStoreInk::subtle);
            c.b.close();
            c.b.close();
        }
        c.b.close();
        if (item.effect_slug.bytes) {
            heading(c, "Uses an effect");
            c.b.text(join(c.b, item.effect_slug, utf8_literal(" is installed with this theme.")), UiStoreType::paragraph, UiStoreInk::normal, ui_store_wrap);
        }
    }
    heading(c, "About");
    markdown(c, c.in.about, c.m.details_loading ? utf8_literal("Loading...") : c.m.details_markdown.bytes ? c.m.details_markdown : item.summary);
    if (kind == CatalogKind::plugin && c.m.consent.permission_count) {
        // The facts of the shown version: permissions arrive through the consent model of the open item.
        heading(c, "Permissions");
        const ConstSlice<Utf8View> hosts{c.m.consent.hosts, c.m.consent.host_count};
        for (int risk = 2; risk >= 0; --risk)
            permission_group(c, static_cast<u8>(risk), c.m.consent.permissions, c.m.consent.permission_count, hosts);
    }
    if (kind == CatalogKind::effect && c.m.knob_count) {
        heading(c, "Knobs");
        card_open(c);
        for (u32 i = 0; i < c.m.knob_count; ++i) {
            hairline_row(c, i + 1 == c.m.knob_count, 16, 10);
            c.b.text(c.m.knobs[i].label, UiStoreType::body, UiStoreInk::strong, ui_store_flex);
            static constexpr Utf8View kTypes[]{utf8_literal("Slider"), utf8_literal("Whole number"), utf8_literal("Switch"), utf8_literal("Colour"), utf8_literal("Choice")};
            c.b.text(kTypes[c.m.knobs[i].type < 5 ? c.m.knobs[i].type : 0], UiStoreType::small, UiStoreInk::subtle);
            c.b.close();
        }
        c.b.close();
    }
    if (c.m.changelog_markdown.bytes && kind == CatalogKind::plugin) {
        heading(c, "What's new");
        card_open(c);
        c.b.node().pad_x = c.b.node().pad_y = 16;
        markdown(c, c.in.changes, c.m.changelog_markdown);
        c.b.close();
    }
    if (kind != CatalogKind::plugin || item.license.bytes)
        credit(c, item);
    c.b.close();
    rail(c, item, index);
    c.b.close();
}

// ---- Knobs --------------------------------------------------------------------------------------------------

u32 knob_steps(const StoreKnob& k) noexcept {
    constexpr u32 kSliderSteps = 101;
    // An integer knob steps by one across a span of 2..101 values; anything else, a span of one value included, takes the slider.
    const float range = k.maximum - k.minimum;
    return k.type == 1 && range >= 1 && range < static_cast<float>(kSliderSteps) ? static_cast<u32>(range) + 1 : kSliderSteps;
}

namespace {

u32 knob_index(const StoreKnob& k, float value, u32 steps) noexcept {
    const float range = k.maximum - k.minimum;
    if (!(range > 0))
        return 0;
    float t = (value - k.minimum) / range;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return static_cast<u32>(t * (steps - 1) + .5f);
}

Utf8View number_text(UiStoreBuilder& b, float value, u32 decimals) noexcept {
    const bool negative = value < 0;
    if (negative)
        value = -value;
    u32 scale = 1;
    for (u32 i = 0; i < decimals; ++i)
        scale *= 10;
    const u64 scaled = static_cast<u64>(value * scale + .5f);
    Utf8View whole = b.number(scaled / scale);
    if (!decimals)
        return negative ? join(b, utf8_literal("-"), whole) : whole;
    u64 frac = scaled % scale;
    char digits[4]{'0', '0', '0', '0'};
    for (u32 i = decimals; i; --i) {
        digits[i - 1] = static_cast<char>('0' + frac % 10);
        frac /= 10;
    }
    return join(b, negative ? utf8_literal("-") : Utf8View{}, whole, utf8_literal("."), b.copy({digits, decimals}));
}

void value_row(Ctx& c, const Utf8View& label, const Utf8View& value, bool overridden, u32 reset_action) noexcept {
    auto& row = c.b.open(UiStoreKind::row);
    row.gap = 8;
    row.flags = ui_store_center;
    c.b.text(label, UiStoreType::body, UiStoreInk::strong);
    if (overridden) {
        auto& dot = c.b.leaf(UiStoreKind::swatch);
        dot.variant = 2;
        dot.rw = dot.rh = 6;
    }
    c.b.spacer(0, 0, true);
    if (value.bytes)
        c.b.text(value, UiStoreType::small_bold, UiStoreInk::normal);
    if (overridden && reset_action)
        c.b.icon_button(UiIcon::reset, "Reset to the global default", reset_action, 24);
    c.b.close();
}

void knob_row(Ctx& c, u32 i, bool last) noexcept {
    const StoreKnob& k = c.m.knobs[i];
    auto& cell = hairline_row(c, last, 16, 12);
    cell.gap = 4;
    // A column row: re-open as a column inside the hairline row.
    auto& col = c.b.open(UiStoreKind::column);
    col.gap = 8;
    col.flags = ui_store_flex;
    switch (k.type) {
    case 2: {
        auto& row = c.b.open(UiStoreKind::row);
        row.gap = 8;
        row.flags = ui_store_center;
        c.b.text(k.label, UiStoreType::body, UiStoreInk::strong, ui_store_flex);
        auto& t = c.b.leaf(UiStoreKind::toggle);
        t.action = store_action(StoreAct::knob, i);
        t.value = k.value > .5f ? 1 : 0;
        t.group = 6;
        c.b.close();
        break;
    }
    case 3: {
        auto& row = c.b.open(UiStoreKind::row);
        row.gap = 8;
        row.flags = ui_store_center;
        c.b.text(k.label, UiStoreType::body, UiStoreInk::strong, ui_store_flex);
        auto& sw = c.b.leaf(UiStoreKind::swatch);
        sw.variant = 1;
        sw.value = k.rgb;
        sw.rw = sw.rh = 24;
        sw.action = store_action(StoreAct::knob_color, i);
        sw.group = 6;
        c.b.close();
        break;
    }
    case 4: {
        value_row(c, k.label, k.choice_count ? k.choices[static_cast<u32>(k.value) < k.choice_count ? static_cast<u32>(k.value) : 0] : Utf8View{}, false, 0);
        if (k.choice_count && k.choice_count <= 5) {
            auto& track = c.b.open(UiStoreKind::row);
            track.variant = 1;
            track.pad_x = track.pad_y = 4;
            for (u32 o = 0; o < k.choice_count; ++o) {
                auto& s = c.b.leaf(UiStoreKind::segment);
                s.text = k.choices[o];
                s.action = store_action(StoreAct::knob, i | (o << 8) | (1u << 15));
                s.group = 6;
                if (o == static_cast<u32>(k.value))
                    s.flags = ui_store_selected;
            }
            c.b.close();
        } else {
            auto& dd = c.b.leaf(UiStoreKind::dropdown);
            dd.text2 = k.choice_count ? k.choices[static_cast<u32>(k.value) < k.choice_count ? static_cast<u32>(k.value) : 0] : Utf8View{};
            dd.flags = ui_store_stretch;
            dd.action = store_action(StoreAct::knob_menu, i);
            dd.group = 6;
        }
        break;
    }
    default: {
        const u32 steps = knob_steps(k);
        value_row(c, k.label,
                  k.type == 1  ? number_text(c.b, k.value, 0)
                  : k.percent ? join(c.b, number_text(c.b, k.value * 100.f, 0), utf8_literal(" %"))
                              : number_text(c.b, k.value, 2),
                  false, 0);
        auto& s = c.b.leaf(UiStoreKind::slider);
        s.count = static_cast<u16>(steps);
        s.value = knob_index(k, k.value, steps);
        s.aux = static_cast<u16>(knob_index(k, k.fallback, steps) + 1);
        s.action = store_action(StoreAct::knob, i);
        s.group = 6;
        break;
    }
    }
    if (k.hint.bytes)
        subtle(c, k.hint, ui_store_wrap);
    c.b.close();
    c.b.close();
}

void segments(Ctx& c, StoreUniversalRow row, const Utf8View* labels, u32 count, u32 selected) noexcept {
    auto& track = c.b.open(UiStoreKind::row);
    track.variant = 1;
    track.pad_x = track.pad_y = 4;
    for (u32 o = 0; o < count; ++o) {
        auto& s = c.b.leaf(UiStoreKind::segment);
        s.text = labels[o];
        s.action = store_action(StoreAct::universal, (static_cast<u32>(row) << 8) | o);
        s.group = 8 + static_cast<u32>(row);
        if (o == selected)
            s.flags = ui_store_selected;
    }
    c.b.close();
}

// A universal block row: a label with its value, the control, an optional hint.
void universal_row(Ctx& c, StoreUniversalRow row, bool last, bool overridable, bool global_panel) noexcept {
    const StoreUniversal& u = global_panel ? c.m.global_universal : c.m.universal;
    static constexpr Utf8View kResolution[]{utf8_literal("25"), utf8_literal("35"), utf8_literal("50"), utf8_literal("75"), utf8_literal("100")};
    static constexpr Utf8View kFrames[]{utf8_literal("10"), utf8_literal("15"), utf8_literal("24"), utf8_literal("30"), utf8_literal("60")};
    static constexpr Utf8View kBlend[]{utf8_literal("Normal"), utf8_literal("Screen"), utf8_literal("Add")};
    static constexpr Utf8View kFocus[]{utf8_literal("Keep"), utf8_literal("10 fps"), utf8_literal("Pause")};
    static constexpr Utf8View kBudget[]{utf8_literal("Low"), utf8_literal("Balanced"), utf8_literal("High")};
    hairline_row(c, last, 16, 12);
    auto& col = c.b.open(UiStoreKind::column);
    col.gap = 8;
    col.flags = ui_store_flex;
    const u32 reset = store_action(StoreAct::universal_reset, static_cast<u32>(row));
    switch (row) {
    case StoreUniversalRow::resolution: {
        const bool over = overridable && u.resolution;
        const u32 effective = over ? u.resolution : global_panel ? u.resolution : u.global_resolution ? u.global_resolution : 3;
        value_row(c, utf8_literal("Resolution"), effective ? join(c.b, kResolution[effective - 1], utf8_literal(" %")) : utf8_literal("Automatic"), over, reset);
        segments(c, row, kResolution, 5, effective ? effective - 1 : ~0u);
        if (over && u.global_resolution)
            subtle(c, join(c.b, utf8_literal("Global default: "), kResolution[u.global_resolution - 1], utf8_literal(" %.")));
        break;
    }
    case StoreUniversalRow::frame_rate: {
        const bool over = overridable && u.frame_rate;
        const u32 requested = global_panel ? 0 : u.requested_frame_rate;
        const u32 effective = over ? u.frame_rate : global_panel ? u.frame_rate : u.global_frame_rate ? u.global_frame_rate : requested ? requested : 3;
        value_row(c, utf8_literal("Frame rate"), effective ? join(c.b, kFrames[effective - 1], utf8_literal(" fps")) : utf8_literal("Automatic"), over, reset);
        segments(c, row, kFrames, 5, effective ? effective - 1 : ~0u);
        if (over || requested) {
            Utf8View hint = u.global_frame_rate ? join(c.b, utf8_literal("Global default: "), kFrames[u.global_frame_rate - 1], utf8_literal(" fps.")) : Utf8View{};
            if (requested && requested != u.frame_rate)
                hint = join(c.b, hint, hint.bytes ? utf8_literal(" ") : Utf8View{}, utf8_literal("This effect asks for "), join(c.b, kFrames[requested - 1], utf8_literal(" fps.")));
            if (hint.bytes)
                subtle(c, hint, ui_store_wrap);
        }
        break;
    }
    case StoreUniversalRow::speed: {
        const float x = u.speed * .05f;
        value_row(c, utf8_literal("Speed"), join(c.b, number_text(c.b, x, 1), utf8_literal("\xC3\x97")), false, 0);
        auto& s = c.b.leaf(UiStoreKind::slider);
        s.count = 61;
        s.value = u.speed;
        s.aux = 21;
        s.action = store_action(StoreAct::universal, (static_cast<u32>(row) << 8));
        s.group = 8 + static_cast<u32>(row);
        break;
    }
    case StoreUniversalRow::opacity: {
        value_row(c, utf8_literal("Opacity"), join(c.b, c.b.number(u.opacity), utf8_literal(" %")), false, 0);
        auto& s = c.b.leaf(UiStoreKind::slider);
        s.count = 101;
        s.value = u.opacity;
        s.aux = 101;
        s.action = store_action(StoreAct::universal, (static_cast<u32>(row) << 8));
        s.group = 8 + static_cast<u32>(row);
        break;
    }
    case StoreUniversalRow::blend: {
        const u32 sel = u.blend ? u.blend - 1 : 0;
        value_row(c, utf8_literal("Blend"), kBlend[sel < 3 ? sel : 0], false, 0);
        segments(c, row, kBlend, 3, sel);
        break;
    }
    case StoreUniversalRow::unfocused:
        value_row(c, utf8_literal("When MaiCord isn't focused"), kFocus[u.unfocused < 3 ? u.unfocused : 1], false, 0);
        segments(c, row, kFocus, 3, u.unfocused);
        break;
    case StoreUniversalRow::budget:
        value_row(c, utf8_literal("GPU budget"), kBudget[u.budget < 3 ? u.budget : 1], false, 0);
        segments(c, row, kBudget, 3, u.budget);
        break;
    case StoreUniversalRow::light: {
        static constexpr Utf8View kLight[]{utf8_literal("Auto"), utf8_literal("Off")};
        if (!global_panel && u.appearance) {
            value_row(c, utf8_literal("Light themes"), utf8_literal("Its own"), false, 0);
            subtle(c, utf8_literal("This effect follows the light or dark theme itself."), ui_store_wrap);
            break;
        }
        const bool over = overridable && u.light;
        const u32 off = (over || global_panel ? u.light : u.global_light) == 2 ? 1 : 0;
        value_row(c, utf8_literal("Light themes"), kLight[off], over, reset);
        segments(c, row, kLight, 2, off);
        subtle(c, over ? join(c.b, utf8_literal("Global default: "), kLight[u.global_light == 2 ? 1 : 0], utf8_literal("."))
                       : utf8_literal("Auto turns the effect's light into shade on a light theme."),
               ui_store_wrap);
        break;
    }
    default: {
        auto& line = c.b.open(UiStoreKind::row);
        line.gap = 8;
        line.flags = ui_store_center;
        c.b.text("React to pointer", UiStoreType::body, UiStoreInk::strong, ui_store_flex);
        auto& t = c.b.leaf(UiStoreKind::toggle);
        t.action = store_action(StoreAct::universal, (static_cast<u32>(row) << 8));
        t.value = u.pointer ? 1 : 0;
        t.group = 8 + static_cast<u32>(row);
        c.b.close();
        break;
    }
    }
    c.b.close();
    c.b.close();
}

void build_universal(Ctx& c) noexcept {
    // Performance card of an effect: the shader's own rows, then the global ones.
    card_open(c);
    {
        auto& head = c.b.open(UiStoreKind::row);
        head.pad_x = 16;
        head.pad_y = 12;
        head.gap = 8;
        head.flags = ui_store_center | ui_store_hairline;
        c.b.text("Performance", UiStoreType::body_bold, UiStoreInk::strong);
        c.b.text("Your global defaults, except where marked", UiStoreType::small, UiStoreInk::subtle, ui_store_flex);
        c.b.close();
    }
    const StoreUniversalRow rows[]{StoreUniversalRow::resolution, StoreUniversalRow::frame_rate, StoreUniversalRow::speed,
                                   StoreUniversalRow::opacity,    StoreUniversalRow::blend,      StoreUniversalRow::light,
                                   StoreUniversalRow::unfocused,  StoreUniversalRow::budget,     StoreUniversalRow::pointer};
    const u32 count = c.m.universal.pointer_available ? 9 : 8;
    for (u32 i = 0; i < count; ++i)
        universal_row(c, rows[i], i + 1 == count, i < 6, false);
    c.b.close();
}

} // namespace

void build_perf_defaults(Ctx& c) noexcept {
    c.b.spacer(0, 0);
    card_open(c);
    auto& head = c.b.open(UiStoreKind::row);
    head.pad_x = 16;
    head.pad_y = 12;
    head.gap = 8;
    head.flags = ui_store_center | ui_store_hairline;
    c.b.text("Performance defaults for all effects", UiStoreType::body_bold, UiStoreInk::strong, ui_store_flex);
    c.b.close();
    const StoreUniversalRow rows[]{StoreUniversalRow::resolution, StoreUniversalRow::frame_rate, StoreUniversalRow::light,
                                   StoreUniversalRow::unfocused, StoreUniversalRow::budget};
    for (u32 i = 0; i < 5; ++i)
        universal_row(c, rows[i], i == 4, false, true);
    c.b.close();
    c.b.spacer(0, 16);
}

void build_knobs(Ctx& c, const StoreItem& item, u32 index) noexcept {
    back_link(c, tab_name(c.st.detail_from == StoreTab::browse ? StoreTab::browse : StoreTab::installed));
    auto& head = c.b.open(UiStoreKind::row);
    head.gap = 16;
    auto& names = c.b.open(UiStoreKind::column);
    names.gap = 4;
    names.flags = ui_store_flex;
    c.b.text(item.name, UiStoreType::title, UiStoreInk::strong);
    auto& meta = c.b.open(UiStoreKind::flow);
    meta.gap = 8;
    c.b.text(join(c.b, byline(c.b, item), item.license.bytes ? join(c.b, utf8_literal(" \xC2\xB7 "), item.license) : Utf8View{}), UiStoreType::small,
             UiStoreInk::subtle);
    if (item.active)
        c.b.chip("Current background", UiStoreTone::positive, UiIcon::check);
    if (c.m.effect_cost_text.bytes)
        c.b.chip(c.m.effect_cost_text, c.m.effect_cost_state == 2 ? UiStoreTone::risk_high : UiStoreTone::risk_low);
    c.b.close();
    c.b.close();
    if (item.item != ~0u)
        c.b.button("Catalog page", UiStoreButton::secondary, store_action(StoreAct::detail_source, 2), UiIcon::external_link, true);
    c.b.button("Remove", UiStoreButton::ghost_critical, store_action(StoreAct::detail_remove, index), UiIcon::trash, true);
    c.b.close();
    c.b.spacer(0, 16);
    // The live preview: the host draws the page's effect as the window background (in place of the window's own
    // when it is another one) and the renderer fills this plate with it once it is the effect drawing.
    static constexpr u32 kResolutions[]{25, 35, 50, 75, 100}, kFrames[]{10, 15, 24, 30, 60};
    const StoreUniversal& u = c.m.universal;
    const u32 resolution = u.resolution ? u.resolution : u.global_resolution ? u.global_resolution : 3;
    const u32 frames = u.frame_rate ? u.frame_rate : u.global_frame_rate ? u.global_frame_rate : u.requested_frame_rate ? u.requested_frame_rate : 3;
    const bool refused = c.m.effect_cost_state == 2, failed = c.m.effect_preview == 2;
    auto& view = c.b.open(UiStoreKind::preview);
    view.variant = static_cast<u8>(c.m.effect_preview == 1 && !refused ? UiStoreAspect::live : UiStoreAspect::fixed);
    view.rh = 220;
    if (refused || failed)
        view.flags = ui_store_critical;
    const Utf8View live_text = join(c.b, utf8_literal("Live preview \xC2\xB7 "), join(c.b, c.b.number(kFrames[(frames - 1) % 5]), utf8_literal(" fps \xC2\xB7 ")),
                                    join(c.b, c.b.number(kResolutions[(resolution - 1) % 5]), utf8_literal(" %")));
    auto& pill = c.b.chip(refused  ? utf8_literal("Too heavy for this GPU")
                          : failed ? utf8_literal("This effect could not be drawn")
                                   : live_text,
                          UiStoreTone::scrim, UiIcon::play);
    pill.anchor = static_cast<u8>(UiStoreAnchor::bottom_left);
    pill.flags = ui_store_foreground;
    auto& panels = c.b.open(UiStoreKind::row);
    panels.gap = 8;
    panels.anchor = static_cast<u8>(UiStoreAnchor::top_right);
    const float widths[]{44, 90, 150};
    for (u32 i = 0; i < 3; ++i) {
        auto& plate = c.b.open(UiStoreKind::card);
        plate.fill = static_cast<u8>(UiStoreFill::translucent);
        plate.rw = widths[i];
        plate.rh = 184;
        c.b.close();
    }
    c.b.close();
    c.b.close();
    c.b.spacer(0, 16);
    auto& grid = c.b.open(UiStoreKind::grid);
    grid.gap = 16;
    grid.rw = 320;
    card_open(c);
    {
        auto& row = c.b.open(UiStoreKind::row);
        row.pad_x = 16;
        row.pad_y = 12;
        row.gap = 8;
        row.flags = ui_store_center | ui_store_hairline;
        c.b.text(item.name, UiStoreType::body_bold, UiStoreInk::strong, ui_store_flex);
        c.b.button("Reset", UiStoreButton::ghost, store_action(StoreAct::knobs_reset), UiIcon::reset);
        c.b.close();
    }
    for (u32 i = 0; i < c.m.knob_count; ++i)
        knob_row(c, i, i + 1 == c.m.knob_count);
    if (!c.m.knob_count) {
        auto& col = c.b.open(UiStoreKind::column);
        col.pad_x = col.pad_y = 16;
        subtle(c, utf8_literal("This effect has no knobs of its own."), ui_store_wrap);
        c.b.close();
    }
    c.b.close();
    build_universal(c);
    c.b.close();
}

} // namespace maicord::app_detail::store_build
