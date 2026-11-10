#include "store_build.hpp"

namespace maicord::app_detail::store_build {
namespace {

// A footer rule sits above the band: the hairline flag draws below, so the body's last element carries it.
UiStoreNode& dialog_footer(Ctx& c) noexcept {
    auto& row = c.b.open(UiStoreKind::row);
    row.pad_x = 24;
    row.pad_y = 12;
    row.gap = 8;
    row.fill = static_cast<u8>(UiStoreFill::translucent);
    row.flags = ui_store_center | ui_store_hairline;
    return row;
}

UiStoreNode& dialog_body(Ctx& c) noexcept {
    auto& col = c.b.open(UiStoreKind::column);
    col.pad_x = col.pad_y = 24;
    col.gap = 12;
    return col;
}

void title_row(Ctx& c, const Utf8View& title, const Utf8View& sub, UiIcon icon, bool tile, bool chips, bool reviewed) noexcept {
    auto& row = c.b.open(UiStoreKind::row);
    row.gap = 16;
    if (tile)
        c.b.tile(icon, 56, UiStoreTone::subtle);
    auto& col = c.b.open(UiStoreKind::column);
    col.gap = 4;
    col.flags = ui_store_flex;
    c.b.text(title, UiStoreType::heading, UiStoreInk::strong, ui_store_wrap);
    if (sub.bytes || chips) {
        auto& meta = c.b.open(UiStoreKind::flow);
        meta.gap = 8;
        if (sub.bytes)
            c.b.text(sub, UiStoreType::small, UiStoreInk::subtle);
        if (reviewed)
            c.b.chip("Reviewed", UiStoreTone::positive, UiIcon::circle_check);
        c.b.close();
    }
    c.b.close();
    c.b.icon_button(UiIcon::close, "Close", store_action(StoreAct::dialog_close));
    c.b.close();
}

void consent_row(Ctx& c, const StorePermission& p, ConstSlice<Utf8View> hosts) noexcept {
    auto& row = c.b.open(UiStoreKind::row);
    row.gap = 12;
    c.b.tile(p.glyph, 28, UiStoreTone::neutral);
    auto& col = c.b.open(UiStoreKind::column);
    col.gap = 6;
    col.flags = ui_store_flex;
    center_row(c, 8);
    c.b.text(p.sentence, UiStoreType::body, p.critical ? UiStoreInk::critical : UiStoreInk::strong, ui_store_wrap | ui_store_flex);
    if (p.added)
        c.b.chip("New", UiStoreTone::brand);
    c.b.close();
    host_chips(c, p, hosts);
    if (p.note.bytes)
        c.b.text(p.note, UiStoreType::small, UiStoreInk::subtle, ui_store_wrap);
    c.b.close();
    c.b.close();
}

// A large footer button in its dialog's arrow-key group.
UiStoreNode& footer_button(Ctx& c, const char* label, UiStoreButton variant, u32 action, u32 group) noexcept {
    auto& b = c.b.button(label, variant, action, UiIcon::count, true);
    b.group = group;
    return b;
}

void build_consent(Ctx& c) noexcept {
    const StoreConsent& k = c.m.consent;
    const bool critical = k.sideload;
    dialog_body(c);
    const Utf8View title = k.sideload ? utf8_literal("Load an unreviewed plugin?")
                           : k.update ? compose(c.b, "{} {} needs new permissions", k.name, k.version)
                                      : compose(c.b, "Install {}?", k.name);
    Utf8View sub = k.author.bytes ? compose(c.b, "{} \xC2\xB7 v{}", k.author, k.version) : compose(c.b, "v{}", k.version);
    if (k.bytes)
        sub = compose(c.b, "{} \xC2\xB7 {}", sub, size_text(c.b, k.bytes));
    title_row(c, title, sub, UiIcon::puzzle, true, true, k.reviewed);
    c.b.text(compose(c.b, "{} will be able to:", k.name), UiStoreType::body, UiStoreInk::normal);
    const ConstSlice<Utf8View> hosts{k.hosts, k.host_count};
    u32 lows = 0;
    bool acknowledge = false;
    for (u32 i = 0; i < k.permission_count; ++i) {
        lows += k.permissions[i].risk == 0 ? 1 : 0;
        acknowledge = acknowledge || k.permissions[i].critical;
    }
    for (int risk = 2; risk >= 1; --risk) {
        bool any = false;
        for (u32 i = 0; i < k.permission_count; ++i)
            if (k.permissions[i].risk == risk) {
                if (!any)
                    access_eyebrow(c, static_cast<u8>(risk), 0);
                any = true;
                consent_row(c, k.permissions[i], hosts);
            }
    }
    if (lows) {
        const bool open = (c.st.low_expanded & 1) != 0;
        if (open) {
            access_eyebrow(c, 0, 0);
            for (u32 i = 0; i < k.permission_count; ++i)
                if (k.permissions[i].risk == 0)
                    consent_row(c, k.permissions[i], hosts);
        } else {
            Utf8View names{};
            u32 shown = 0;
            for (u32 i = 0; i < k.permission_count && shown < 3; ++i)
                if (k.permissions[i].risk == 0) {
                    names = shown ? compose(c.b, "{}, {}", names, k.permissions[i].name) : k.permissions[i].name;
                    ++shown;
                }
            auto& b = c.b.button(compose(c.b, lows == 1 ? "{} low-access permission: {}" : "{} low-access permissions: {}", c.b.number(lows), names),
                                 UiStoreButton::secondary, store_action(StoreAct::dialog_toggle_low), UiIcon::count, true);
            b.icon2 = static_cast<u8>(UiIcon::chevron_down);
            b.flags |= ui_store_stretch;
        }
    }
    if (!k.permission_count)
        c.b.text("Nothing beyond running its own code.", UiStoreType::body, UiStoreInk::subtle);
    notice_card(c, critical ? UiIcon::warning : UiIcon::info, {},
                critical ? utf8_literal("MaiCord hasn't reviewed this plugin. It runs with full access to your PC.")
                         : k.reviewed ? utf8_literal("Plugins run as native code with the same access to your PC as MaiCord. MaiCord reviewed this one and "
                                                     "built it from its public source.")
                                      : utf8_literal("Plugins run as native code with the same access to your PC as MaiCord."),
                critical ? UiStoreFill::critical_tint : UiStoreFill::subtle, critical ? UiStoreInk::critical : UiStoreInk::subtle);
    if (critical && acknowledge) {
        auto& box = c.b.leaf(UiStoreKind::check);
        box.text = utf8_literal("I understand this plugin can act for me without asking each time.");
        box.action = store_action(StoreAct::dialog_check, 1);
        box.value = c.st.dialog_acknowledged ? 1 : 0;
        box.group = 20;
    }
    c.b.close();
    dialog_footer(c);
    if (!k.update && !k.sideload) {
        auto& box = c.b.leaf(UiStoreKind::check);
        box.text = utf8_literal("Turn it on now");
        box.action = store_action(StoreAct::dialog_check, 0);
        box.value = c.st.dialog_check ? 1 : 0;
        box.flags = ui_store_flex;
        box.group = 20;
    } else if (k.update) {
        c.b.button("Skip this version", UiStoreButton::link, store_action(StoreAct::dialog_skip));
        c.b.spacer(0, 0, true);
    } else
        c.b.spacer(0, 0, true);
    footer_button(c, k.update ? "Not now" : "Cancel", UiStoreButton::secondary, store_action(StoreAct::dialog_cancel), 20);
    auto& ok = footer_button(c, k.update ? "Allow and update" : k.sideload ? "Load plugin" : c.st.dialog_check ? "Install and turn on" : "Install",
                             critical ? UiStoreButton::critical : UiStoreButton::primary, store_action(StoreAct::dialog_confirm), 20);
    if (critical && acknowledge && !c.st.dialog_acknowledged)
        ok.flags |= ui_store_disabled;
    c.b.close();
}

void build_link(Ctx& c) noexcept {
    const StoreLink& l = c.m.link;
    if (l.phase == 0) {
        dialog_body(c);
        title_row(c, utf8_literal("Install from link"), utf8_literal("A theme or effect from a link or a file, not reviewed by MaiCord."), UiIcon::link, false, false, false);
        auto& field = c.b.leaf(UiStoreKind::search);
        field.action = kStoreLinkField;
        field.group = 21;
        field.text = utf8_literal("Paste an https:// link to a .maicord-theme.json or .mcfx");
        field.value = c.st.link_text_focus ? 1 : 0;
        field.flags = ui_store_stretch;
        c.b.close();
        dialog_footer(c);
        footer_button(c, "Open file...", UiStoreButton::secondary, store_action(StoreAct::dialog_open_file), 21);
        c.b.spacer(0, 0, true);
        footer_button(c, "Cancel", UiStoreButton::secondary, store_action(StoreAct::dialog_cancel), 21);
        footer_button(c, "Continue", UiStoreButton::primary, store_action(StoreAct::dialog_confirm), 21);
        c.b.close();
        return;
    }
    if (l.phase == 1) {
        dialog_body(c);
        title_row(c, utf8_literal("Checking the link"), l.host, UiIcon::link, false, false, false);
        center_row(c, 12);
        c.b.leaf(UiStoreKind::spinner);
        c.b.text("Fetching and checking the file...", UiStoreType::small, UiStoreInk::subtle);
        c.b.close();
        c.b.close();
        dialog_footer(c);
        c.b.spacer(0, 0, true);
        footer_button(c, "Cancel", UiStoreButton::secondary, store_action(StoreAct::dialog_cancel), 21);
        c.b.close();
        return;
    }
    if (l.phase == 3) {
        dialog_body(c);
        title_row(c, utf8_literal("Can't install that"), {}, UiIcon::warning, false, false, false);
        notice_card(c, UiIcon::warning, {}, c.m.error_text.bytes ? c.m.error_text : utf8_literal("The link or file couldn't be used."), UiStoreFill::critical_tint, UiStoreInk::critical);
        c.b.close();
        dialog_footer(c);
        c.b.spacer(0, 0, true);
        footer_button(c, "Close", UiStoreButton::secondary, store_action(StoreAct::dialog_cancel), 21);
        footer_button(c, "Try another link", UiStoreButton::primary, store_action(StoreAct::dialog_open_file, 1), 21);
        c.b.close();
        return;
    }
    dialog_body(c);
    title_row(c, l.kind == CatalogKind::effect ? utf8_literal("Install an effect from a link?") : utf8_literal("Install a theme from a link?"),
              l.host.bytes ? compose(c.b, "From {}", l.host) : Utf8View{}, UiIcon::link, false, false, false);
    notice_card(c, UiIcon::warning, utf8_literal("Not reviewed"),
                l.kind == CatalogKind::effect
                    ? utf8_literal("MaiCord hasn't checked this effect, and it won't update on its own. Effects are data: they run on your GPU and never as code "
                                   "on your PC, and MaiCord limits how much GPU time they use.")
                    : utf8_literal("MaiCord hasn't checked this theme, and it won't update on its own. A theme is data: it changes how MaiCord looks and runs no code."),
                UiStoreFill::warning_tint, UiStoreInk::warning);
    auto& row = c.b.open(UiStoreKind::row);
    row.gap = 16;
    auto& view = c.b.open(UiStoreKind::preview);
    view.variant = static_cast<u8>(l.previewing && l.kind == CatalogKind::effect ? UiStoreAspect::live : UiStoreAspect::fixed);
    view.rw = 320;
    view.rh = 180;
    if (l.kind == CatalogKind::theme && l.swatches[0] + l.swatches[1] + l.swatches[2] + l.swatches[3]) {
        auto& sw = c.b.open(UiStoreKind::row);
        sw.gap = 4;
        sw.anchor = static_cast<u8>(UiStoreAnchor::center);
        for (u32 i = 0; i < 4; ++i) {
            auto& s = c.b.leaf(UiStoreKind::swatch);
            s.variant = 1;
            s.value = l.swatches[i];
            s.rw = s.rh = 32;
        }
        c.b.close();
    }
    auto& pill = c.b.chip(l.previewing ? "Live \xC2\xB7 Low budget" : "Preview", UiStoreTone::scrim, UiIcon::play);
    pill.anchor = static_cast<u8>(UiStoreAnchor::bottom_left);
    pill.flags = ui_store_foreground;
    c.b.close();
    auto& facts = c.b.open(UiStoreKind::column);
    facts.gap = 8;
    facts.flags = ui_store_flex;
    fact(c, "Name", l.name);
    fact(c, "Author", l.authors);
    fact(c, "License", l.license);
    fact(c, "Based on", l.based_on);
    fact(c, "Size", l.bytes ? size_text(c.b, l.bytes) : Utf8View{});
    if (l.kind == CatalogKind::effect) {
        // The dot is the measured class: green light, amber medium, red heavy or refused.
        const u8 dot = l.cost_refused || l.cost_level >= 2 ? 3 : l.cost_level == 1 ? 4 : 5;
        if (l.cost_known)
            fact(c, "Cost here", l.cost_text, 0, dot);
        else
            fact(c, "Cost here", utf8_literal("Measuring..."));
    }
    if (l.has_effect)
        fact(c, "Effect", l.effect_installed ? compose(c.b, "{} (installed)", l.effect_slug) : l.effect_slug);
    if (l.replaces)
        fact(c, "Replaces", utf8_literal("the one you installed before"));
    c.b.close();
    c.b.close();
    c.b.close();
    dialog_footer(c);
    c.b.spacer(0, 0, true);
    footer_button(c, "Cancel", UiStoreButton::secondary, store_action(StoreAct::dialog_cancel), 21);
    auto& ok = footer_button(c, l.kind == CatalogKind::effect ? "Install effect" : "Install theme", UiStoreButton::primary,
                             store_action(StoreAct::dialog_confirm), 21);
    if (l.cost_refused)
        ok.flags |= ui_store_disabled;
    c.b.close();
}

void build_remove(Ctx& c) noexcept {
    const StoreItem& item = c.m.items[c.st.remove_item < c.m.count ? c.st.remove_item : 0];
    dialog_body(c);
    title_row(c, compose(c.b, item.kind == CatalogKind::plugin ? "Uninstall {}?" : "Remove {}?", item.name), {}, UiIcon::trash, false, false, false);
    c.b.text(item.kind == CatalogKind::plugin ? "The plugin is turned off and its files are deleted. It can be installed again from the catalog."
                                              : "The file is deleted. Anything using it falls back to the built-in choice.",
             UiStoreType::body, UiStoreInk::normal, ui_store_wrap);
    if (item.kind == CatalogKind::plugin) {
        auto& box = c.b.leaf(UiStoreKind::check);
        box.text = utf8_literal("Also delete its settings and saved data");
        box.action = store_action(StoreAct::dialog_check, 2);
        box.value = c.st.dialog_check ? 1 : 0;
        box.group = 22;
    }
    c.b.close();
    dialog_footer(c);
    c.b.spacer(0, 0, true);
    footer_button(c, "Cancel", UiStoreButton::secondary, store_action(StoreAct::dialog_cancel), 22);
    footer_button(c, item.kind == CatalogKind::plugin ? "Uninstall" : "Remove", UiStoreButton::critical, store_action(StoreAct::dialog_confirm), 22);
    c.b.close();
}

} // namespace

void build_dialog(Ctx& c) noexcept {
    if (c.m.consent.open)
        build_consent(c);
    else if (c.st.remove_confirm)
        build_remove(c);
    else if (c.st.link_dialog || c.m.link.open)
        build_link(c);
}

void build_trial_bar(Ctx& c) noexcept {
    auto& row = center_row(c, 12);
    row.pad_x = 16;
    row.pad_y = 8;
    c.b.text(compose(c.b, "Previewing {}", c.m.trying_name), UiStoreType::body_bold, UiStoreInk::strong);
    c.b.button("Revert", UiStoreButton::secondary, store_action(StoreAct::revert_theme)).group = 23;
    c.b.button("Keep", UiStoreButton::primary, store_action(StoreAct::keep_theme)).group = 23;
    c.b.close();
}

} // namespace maicord::app_detail::store_build
