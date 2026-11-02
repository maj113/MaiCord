#include "store_build.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/sdk/maicord_plugin.h"

namespace maicord::app_detail {
using namespace store_build;

// ---- Permissions (the site's table: network, read_messages and act_messages are high; the rest that read or
// change Discord data are medium; the rest low) ---------------------------------------------------------------
namespace {
// The sentence and name (MC_PERM_* order) of each permission.
constexpr char kSentences[] =
    "Changes your messages before they're sent\0Reads the messages you receive\0Keeps deleted and edited messages visible\0"
    "Sees typing, presence, voice and server activity\0Connects to the hosts listed below\0Reads and changes what you're typing\0"
    "Sends, edits, reacts to and marks messages read for you\0Sets your custom status and activity\0Copies text to the clipboard\0"
    "Acts for you on its own, without you asking\0Saves deleted and edited messages on this computer\0";
constexpr char kNames[] = "message_send\0read_messages\0message_history\0read_activity\0network\0composer\0act_messages\0act_presence\0"
                          "clipboard\0automated_actions\0persistent_log\0";
struct PermissionLook {
    UiIcon glyph;
    u8 risk;
    bool critical;
};
constexpr PermissionLook kPermissions[]{
    {UiIcon::message_text, 1, false}, {UiIcon::eye, 2, false},     {UiIcon::history, 1, false},   {UiIcon::activity, 1, false},
    {UiIcon::link, 2, false},         {UiIcon::edit, 1, false},    {UiIcon::send, 2, false},      {UiIcon::user, 1, false},
    {UiIcon::copy, 0, false},         {UiIcon::warning, 2, true},  {UiIcon::file_text, 2, true},
};
constexpr u32 kPermissionCount = sizeof(kPermissions) / sizeof(kPermissions[0]);
static_assert(name_list_count(kSentences) == kPermissionCount && name_list_count(kNames) == kPermissionCount);
} // namespace

bool store_permission_text(u32 bit, StorePermission* out) noexcept {
    if (bit >= kPermissionCount)
        return false;
    const auto& row = kPermissions[bit];
    *out = {name_at(kSentences, bit), name_at(kNames, bit), {}, row.glyph, row.risk, false, row.critical};
    return true;
}

u8 store_permission_risk(u64 permissions, bool has_hosts) noexcept {
    u8 risk = 0;
    for (u32 bit = 0; bit < sizeof(kPermissions) / sizeof(kPermissions[0]); ++bit)
        if (permissions & (u64{1} << bit))
            risk = risk > kPermissions[bit].risk ? risk : kPermissions[bit].risk;
    if (has_hosts && risk < 2)
        risk = 2;
    return risk;
}

void store_page_reset(StorePageState* state, CatalogKind kind) noexcept {
    reset_value(state);
    state->kind = kind;
    state->sort = StoreSort::name;
}

void store_page_build(UiStoreScene* scene, const StoreLayoutInput& in) noexcept {
    const StoreModel& m = *in.model;
    StorePageState& st = *in.state;
    UiStoreBuilder page(scene);
    Ctx c{page, m, st, in};
    const u32 found = st.detail ? store_find_item(m, st.kind, Utf8View{st.slug, c_string_length(st.slug)}) : ~0u;
    if (st.detail && found == ~0u) {
        st.detail = st.knobs = false; // the item is gone
    }
    if (st.detail && found != ~0u) {
        if (st.knobs && st.kind == CatalogKind::effect && m.items[found].installed)
            build_knobs(c, m.items[found], found);
        else
            build_detail(c, m.items[found], found);
    } else {
        build_header(c);
        build_tabs(c);
        switch (st.tab) {
        case StoreTab::installed:
            build_installed(c);
            break;
        case StoreTab::updates:
            if (st.kind == CatalogKind::plugin)
                build_updates(c);
            break;
        case StoreTab::developer:
            build_developer(c);
            break;
        default:
            build_browse(c);
            break;
        }
    }
    page.close();
    const bool dialog = m.consent.open || st.remove_confirm || st.link_dialog || m.link.open;
    if (dialog) {
        UiStoreBuilder d(scene, true);
        Ctx dc{d, m, st, in};
        build_dialog(dc);
        d.close();
    } else if (m.trying) {
        UiStoreBuilder d(scene, true, true);
        Ctx dc{d, m, st, in};
        build_trial_bar(dc);
        d.close();
    }
    if (!page.ok())
        scene->count = 0, scene->page_root = scene->dialog_root = kUiStoreNone;
}

// ---- Actions ---------------------------------------------------------------------------------------------------
namespace {

StoreCommand cmd(StoreCommandKind kind, u32 item = 0, u32 arg = 0, u32 value = 0, bool flag = false) noexcept {
    StoreCommand c{};
    c.kind = kind;
    c.item = static_cast<u16>(item);
    c.arg = arg;
    c.value = value;
    c.flag = flag;
    return c;
}
StoreCommand menu(StoreMenu which, u32 item, const UiRect& anchor) noexcept {
    StoreCommand c = cmd(StoreCommandKind::menu, item);
    c.menu = which;
    c.anchor = anchor;
    return c;
}
u32 bits_of(float value) noexcept {
    u32 bits;
    copy_bytes(&bits, &value, 4);
    return bits;
}

void open_detail(StorePageState* st, const StoreItem& item, u32 scroll_value, bool knobs) noexcept {
    if (!st->detail)
        st->saved_scroll = static_cast<float>(scroll_value);
    st->detail_from = st->tab;
    st->detail = true;
    st->knobs = knobs;
    const u32 n = minimum<u32>(item.slug.bytes, kCatalogSlugMaximumBytes);
    copy_bytes(st->slug, item.slug.data, n);
    st->slug[n] = 0;
}

StoreCommand primary(const StoreModel& m, StorePageState* st, u32 index, u32 scroll) noexcept {
    if (index >= m.count)
        return cmd(StoreCommandKind::none);
    const StoreItem& item = m.items[index];
    if (item.revoked || item.busy)
        return cmd(StoreCommandKind::none);
    if (!item.installed)
        return cmd(StoreCommandKind::install, index);
    if (item.update)
        return cmd(item.needs_review ? StoreCommandKind::review : StoreCommandKind::update, index);
    if (item.kind == CatalogKind::theme)
        return cmd(StoreCommandKind::apply_theme, index);
    if (item.kind == CatalogKind::effect) {
        if (!item.active)
            return cmd(StoreCommandKind::use_effect, index);
        open_detail(st, item, scroll, true);
        return cmd(StoreCommandKind::none);
    }
    return cmd(item.enabled ? StoreCommandKind::disable : StoreCommandKind::enable, index);
}

} // namespace

StoreCommand store_page_act(const StoreModel& m, StorePageState* st, u32 action, u32 value, const UiRect& anchor) noexcept {
    const StoreAct act = store_act(action);
    const u32 arg = store_arg(action);
    const u32 item_index = arg < m.count ? arg : ~0u;
    switch (act) {
    case StoreAct::tab:
        st->tab = static_cast<StoreTab>(arg < 4 ? arg : 0);
        st->entered = true;
        st->detail = st->knobs = false;
        st->filter = StoreFilter::all;
        return cmd(StoreCommandKind::none);
    case StoreAct::header_link:
        st->link_dialog = true;
        st->link_text_focus = true;
        return cmd(StoreCommandKind::link_preview);
    case StoreAct::sort:
        return menu(StoreMenu::sort, 0, anchor);
    case StoreAct::chip: {
        StoreChips chips{};
        store_chips_build(m, st->kind, &chips);
        st->chips = store_chip_toggle(st->chips, chips, arg);
        return cmd(StoreCommandKind::none);
    }
    case StoreAct::clear_filters:
        st->chips = 0;
        st->query_bytes = 0;
        return cmd(StoreCommandKind::none, 0, 0, 0, true);
    case StoreAct::clear_search:
    case StoreAct::search_clear:
        st->query_bytes = 0;
        return cmd(StoreCommandKind::none, 0, 0, 0, true);
    case StoreAct::segment:
        st->filter = static_cast<StoreFilter>(arg < 4 ? arg : 0);
        return cmd(StoreCommandKind::none);
    case StoreAct::card:
        if (item_index == ~0u)
            break;
        open_detail(st, m.items[item_index], value, false);
        return cmd(StoreCommandKind::request_details, item_index);
    case StoreAct::card_primary:
    case StoreAct::row_primary: {
        const StoreCommand result = primary(m, st, arg, value);
        if (st->detail && st->knobs)
            return cmd(StoreCommandKind::request_details, arg);
        return result;
    }
    case StoreAct::card_switch:
    case StoreAct::row_switch:
        if (item_index == ~0u)
            break;
        return cmd(m.items[item_index].enabled ? StoreCommandKind::disable : StoreCommandKind::enable, item_index);
    case StoreAct::card_settings:
    case StoreAct::row_settings:
        return cmd(StoreCommandKind::open_plugin_settings, arg);
    case StoreAct::card_overflow:
    case StoreAct::row_overflow:
        if (item_index == ~0u)
            break;
        return menu(StoreMenu::overflow, item_index, anchor);
    case StoreAct::back:
        if (st->detail && st->knobs && st->detail_from == StoreTab::browse) {
            st->knobs = false;
            return cmd(StoreCommandKind::none);
        }
        st->detail = st->knobs = false;
        return cmd(StoreCommandKind::none, 0, 0, 0, true);
    case StoreAct::detail_primary: {
        if (item_index == ~0u)
            break;
        const StoreCommand result = primary(m, st, arg, value);
        return result;
    }
    case StoreAct::detail_secondary: {
        if (item_index == ~0u)
            break;
        const StoreItem& item = m.items[item_index];
        if (item.kind == CatalogKind::theme)
            return cmd(StoreCommandKind::try_theme, item_index);
        if (item.kind == CatalogKind::effect) {
            st->knobs = true;
            return cmd(StoreCommandKind::request_details, item_index);
        }
        return cmd(StoreCommandKind::open_plugin_settings, item_index);
    }
    case StoreAct::detail_remove:
        if (item_index == ~0u)
            break;
        st->remove_confirm = true;
        st->remove_item = static_cast<u16>(item_index);
        st->dialog_check = false;
        return cmd(StoreCommandKind::none);
    case StoreAct::detail_source:
        return cmd(arg == 0 ? StoreCommandKind::open_source : StoreCommandKind::open_homepage, 0, arg);
    case StoreAct::detail_copy_sha:
        return cmd(StoreCommandKind::copy_text, arg, 1);
    case StoreAct::update_all:
        return cmd(StoreCommandKind::update_all);
    case StoreAct::check_now:
    case StoreAct::retry:
        return cmd(StoreCommandKind::check_now);
    case StoreAct::show_saved:
        st->show_saved = true;
        return cmd(StoreCommandKind::saved_catalog);
    case StoreAct::copy_details:
        return cmd(StoreCommandKind::copy_text, 0, 2);
    case StoreAct::banner_update: {
        u32 first = ~0u, total = 0;
        for (u32 i = 0; i < m.count; ++i)
            if (m.items[i].kind == st->kind && m.items[i].installed && m.items[i].update) {
                if (first == ~0u)
                    first = i;
                ++total;
            }
        if (total == 1)
            return cmd(m.items[first].needs_review ? StoreCommandKind::review : StoreCommandKind::update, first);
        return cmd(StoreCommandKind::update_all);
    }
    case StoreAct::auto_update_all:
        return cmd(StoreCommandKind::set_auto_update_all, 0, 0, 0, !m.auto_update_all);
    case StoreAct::open_folder:
        return cmd(StoreCommandKind::open_folder);
    case StoreAct::protocol_on:
    case StoreAct::protocol_off:
        return cmd(StoreCommandKind::protocol_answer, 0, 0, 0, act == StoreAct::protocol_on);
    case StoreAct::knob: {
        const u32 index = arg & 0x7f;
        if (index >= m.knob_count)
            break;
        const StoreKnob& k = m.knobs[index];
        float natural;
        if (k.type == 2)
            natural = k.value > .5f ? 0.f : 1.f;
        else if (k.type == 4)
            natural = static_cast<float>((arg >> 8) & 0x7f);
        else {
            const u32 steps = k.type == 1 && static_cast<u32>(k.maximum - k.minimum) + 1 <= 101 ? static_cast<u32>(k.maximum - k.minimum) + 1 : 101;
            natural = k.minimum + (k.maximum - k.minimum) * static_cast<float>(value) / static_cast<float>(steps - 1);
            if (k.type == 1)
                natural = static_cast<float>(static_cast<i32>(natural + (natural < 0 ? -.5f : .5f)));
        }
        return cmd(StoreCommandKind::knob_set, index, 0, bits_of(natural));
    }
    case StoreAct::knob_menu:
        return menu(StoreMenu::knob_choice, arg, anchor);
    case StoreAct::knob_color: {
        StoreCommand c = menu(StoreMenu::knob_choice, arg, anchor);
        c.flag = true;
        return c;
    }
    case StoreAct::knobs_reset:
        return cmd(StoreCommandKind::knobs_reset);
    case StoreAct::universal: {
        const u32 row = (arg >> 8) & 0xff, option = arg & 0xff;
        const bool slider = row == static_cast<u32>(StoreUniversalRow::speed) || row == static_cast<u32>(StoreUniversalRow::opacity);
        const bool toggle = row == static_cast<u32>(StoreUniversalRow::pointer);
        return cmd(StoreCommandKind::universal_set, 0, row, toggle ? (m.universal.pointer ? 0 : 1) : slider ? value : option,
                   (st->kind == CatalogKind::effect) && st->detail);
    }
    case StoreAct::universal_reset:
        return cmd(StoreCommandKind::universal_reset, 0, arg);
    case StoreAct::keep_theme:
        return cmd(StoreCommandKind::keep_theme);
    case StoreAct::revert_theme:
        return cmd(StoreCommandKind::revert_theme);
    case StoreAct::dialog_check:
        if (arg == 1)
            st->dialog_acknowledged = !st->dialog_acknowledged;
        else
            st->dialog_check = !st->dialog_check;
        return cmd(StoreCommandKind::none);
    case StoreAct::dialog_toggle_low:
        st->low_expanded ^= 1;
        return cmd(StoreCommandKind::none);
    case StoreAct::dialog_skip:
        return cmd(StoreCommandKind::skip_update, 0, 0, 0, true);
    case StoreAct::dialog_open_file:
        if (arg == 1) {
            st->link_dialog = true;
            return cmd(StoreCommandKind::link_cancel, 0, 1);
        }
        return cmd(StoreCommandKind::link_open_file);
    case StoreAct::dialog_source:
        return cmd(StoreCommandKind::open_source, 0, 3);
    case StoreAct::dialog_confirm:
        if (m.consent.open) {
            const bool enable = m.consent.update ? true : st->dialog_check;
            st->dialog_acknowledged = false;
            return cmd(StoreCommandKind::consent_answer, 0, 1, 0, enable);
        }
        if (st->remove_confirm) {
            st->remove_confirm = false;
            return cmd(StoreCommandKind::remove, st->remove_item, 0, 0, st->dialog_check);
        }
        if (m.link.open && m.link.phase == 2) {
            st->link_dialog = false;
            return cmd(StoreCommandKind::link_commit);
        }
        return cmd(StoreCommandKind::link_begin);
    case StoreAct::dialog_cancel:
    case StoreAct::dialog_close:
        if (m.consent.open) {
            st->dialog_acknowledged = false;
            return cmd(StoreCommandKind::consent_answer, 0, 0);
        }
        if (st->remove_confirm) {
            st->remove_confirm = false;
            return cmd(StoreCommandKind::none);
        }
        st->link_dialog = false;
        return cmd(StoreCommandKind::link_cancel);
    case StoreAct::dev_load:
        return cmd(StoreCommandKind::dev_load);
    case StoreAct::dev_reload:
        return cmd(StoreCommandKind::dev_reload);
    case StoreAct::console_clear:
        return cmd(StoreCommandKind::console_clear);
    case StoreAct::updates_expand:
        if (arg < 8)
            st->updates_open[arg] = !st->updates_open[arg];
        return cmd(StoreCommandKind::none);
    default:
        break;
    }
    return cmd(StoreCommandKind::none);
}

StoreCommand store_page_escape(const StoreModel& m, StorePageState* st, bool* handled) noexcept {
    *handled = true;
    if (m.consent.open || st->remove_confirm || st->link_dialog || m.link.open)
        return store_page_act(m, st, store_action(StoreAct::dialog_cancel), 0, {});
    if (m.trying)
        return cmd(StoreCommandKind::revert_theme);
    if (st->detail)
        return store_page_act(m, st, store_action(StoreAct::back), 0, {});
    *handled = false;
    return cmd(StoreCommandKind::none);
}

// ---- Menus -----------------------------------------------------------------------------------------------------
namespace {

enum class Row : u8 { settings, catalog, rollback, auto_update, uninstall, apply, try_it, remove, use, knobs };

PopupItem row_item(const Utf8View& label, u32 action, UiIcon icon, u16 flags = 0) noexcept {
    PopupItem p{};
    p.label = label;
    p.action = action + 1; // an item's action is never 0: that is no selection
    p.flags = static_cast<u16>(popup_item_enabled | flags);
    p.presentation = ui_menu_row_presentation(icon);
    return p;
}

u32 overflow_rows(const StoreModel& m, u32 item, Row (&rows)[8]) noexcept {
    const StoreItem& it = m.items[item < m.count ? item : 0];
    u32 n = 0;
    if (it.kind == CatalogKind::plugin) {
        if (it.has_settings)
            rows[n++] = Row::settings;
        if (it.item != ~0u)
            rows[n++] = Row::catalog;
        if (it.can_roll_back)
            rows[n++] = Row::rollback;
        if (!it.sideloaded)
            rows[n++] = Row::auto_update;
        rows[n++] = Row::uninstall;
    } else if (it.kind == CatalogKind::theme) {
        if (!it.active)
            rows[n++] = Row::apply;
        if (!it.active)
            rows[n++] = Row::try_it;
        if (it.item != ~0u)
            rows[n++] = Row::catalog;
        rows[n++] = Row::remove;
    } else {
        if (!it.active)
            rows[n++] = Row::use;
        rows[n++] = Row::knobs;
        if (it.item != ~0u)
            rows[n++] = Row::catalog;
        rows[n++] = Row::remove;
    }
    return n;
}

constexpr u32 kColors[]{0xE5484D, 0xF5A524, 0xF5D90A, 0x30A46C, 0x12A594, 0x0091FF, 0x6E56CF, 0xE93D82, 0xFFFFFF, 0x8B8D98};
constexpr Utf8View kColorNames[]{utf8_literal("Red"),    utf8_literal("Orange"), utf8_literal("Yellow"), utf8_literal("Green"), utf8_literal("Teal"),
                                 utf8_literal("Blue"),   utf8_literal("Purple"), utf8_literal("Pink"),   utf8_literal("White"), utf8_literal("Grey")};

} // namespace

u32 store_menu_items(const StoreModel& m, const StorePageState& st, StoreMenu which, u32 item, PopupItem* out, u32 capacity,
                     Slice<char> labels) noexcept {
    u32 n = 0;
    const auto add = [&](const PopupItem& p) noexcept {
        if (n < capacity)
            out[n++] = p;
    };
    if (which == StoreMenu::sort) {
        static constexpr Utf8View kNames[]{utf8_literal("Name"), utf8_literal("Newest"), utf8_literal("Least access")};
        static constexpr Utf8View kLight[]{utf8_literal("Name"), utf8_literal("Newest"), utf8_literal("Lightest")};
        const u32 rows = st.kind == CatalogKind::theme ? 2 : 3;
        for (u32 i = 0; i < rows; ++i)
            add(row_item(st.kind == CatalogKind::effect ? kLight[i] : kNames[i], i, UiIcon::count,
                         static_cast<u16>(static_cast<u32>(st.sort) == i ? popup_item_checked : 0)));
        return n;
    }
    if (which == StoreMenu::knob_choice) {
        if (item >= m.knob_count)
            return 0;
        const StoreKnob& k = m.knobs[item];
        if (k.type == 3) {
            for (u32 i = 0; i < 10; ++i) {
                PopupItem p = row_item(kColorNames[i], i, UiIcon::palette, k.rgb == kColors[i] ? popup_item_checked : 0);
                p.media_flags = kPopupItemMediaIconTint;
                p.media_variant = kColors[i];
                add(p);
            }
            return n;
        }
        for (u32 i = 0; i < k.choice_count; ++i)
            add(row_item(k.choices[i], i, UiIcon::count, static_cast<u16>(static_cast<u32>(k.value) == i ? popup_item_checked : 0)));
        return n;
    }
    Row rows[8];
    const u32 count = overflow_rows(m, item, rows);
    const StoreItem& it = m.items[item < m.count ? item : 0];
    // Row order is the enum's: the label, glyph and flags of each row.
    static constexpr char kRowLabels[] = "Settings\0View in catalog\0Roll back to \0Update automatically\0Uninstall...\0Apply\0Try it\0"
                                         "Remove...\0Use as background\0Knobs\0";
    static constexpr UiIcon kRowIcons[]{UiIcon::settings, UiIcon::external_link, UiIcon::retry, UiIcon::refresh, UiIcon::trash,
                                        UiIcon::check,    UiIcon::eye,           UiIcon::trash, UiIcon::check,   UiIcon::filters};
    static_assert(sizeof(kRowIcons) / sizeof(kRowIcons[0]) == static_cast<u32>(Row::knobs) + 1 &&
                  name_list_count(kRowLabels) == static_cast<u32>(Row::knobs) + 1);
    for (u32 i = 0; i < count; ++i) {
        const Row row = rows[i];
        Utf8View label = name_at(kRowLabels, static_cast<u32>(row));
        if (row == Row::rollback) {
            const u32 bytes = label.bytes + it.previous_version.bytes;
            if (labels.count >= bytes) {
                copy_bytes(labels.data, label.data, label.bytes);
                copy_bytes(labels.data + label.bytes, it.previous_version.data, it.previous_version.bytes);
                label = {labels.data, bytes};
            }
        }
        const u16 flags = row == Row::auto_update ? (it.auto_update ? popup_item_checked : 0)
                          : row == Row::uninstall || row == Row::remove ? popup_item_danger | popup_item_separator_before
                                                                        : 0;
        add(row_item(label, i, kRowIcons[static_cast<u32>(row)], static_cast<u16>(flags)));
    }
    return n;
}

StoreCommand store_menu_choose(const StoreModel& m, StorePageState* st, StoreMenu which, u32 item, u32 row) noexcept {
    if (which == StoreMenu::sort) {
        st->sort = static_cast<StoreSort>(row < 3 ? row : 0);
        return cmd(StoreCommandKind::none);
    }
    if (which == StoreMenu::knob_choice) {
        if (item >= m.knob_count)
            return cmd(StoreCommandKind::none);
        const StoreKnob& k = m.knobs[item];
        if (k.type == 3)
            return cmd(StoreCommandKind::knob_color, item, 0, row < 10 ? kColors[row] : 0, true);
        return cmd(StoreCommandKind::knob_set, item, 0, bits_of(static_cast<float>(row)), true);
    }
    Row rows[8];
    const u32 count = overflow_rows(m, item, rows);
    if (row >= count)
        return cmd(StoreCommandKind::none);
    const StoreItem& it = m.items[item];
    switch (rows[row]) {
    case Row::settings:
        return cmd(StoreCommandKind::open_plugin_settings, item);
    case Row::catalog:
        return cmd(StoreCommandKind::open_homepage, item, 2);
    case Row::rollback:
        return cmd(StoreCommandKind::rollback, item);
    case Row::auto_update:
        return cmd(StoreCommandKind::set_auto_update, item, 0, 0, !it.auto_update);
    case Row::uninstall:
    case Row::remove:
        st->remove_confirm = true;
        st->remove_item = static_cast<u16>(item);
        st->dialog_check = false;
        return cmd(StoreCommandKind::none);
    case Row::apply:
        return cmd(StoreCommandKind::apply_theme, item);
    case Row::try_it:
        return cmd(StoreCommandKind::try_theme, item);
    case Row::use:
        return cmd(StoreCommandKind::use_effect, item);
    case Row::knobs:
        st->detail_from = StoreTab::installed;
        st->detail = true;
        st->knobs = true;
        copy_bytes(st->slug, it.slug.data, minimum<u32>(it.slug.bytes, kCatalogSlugMaximumBytes));
        st->slug[minimum<u32>(it.slug.bytes, kCatalogSlugMaximumBytes)] = 0;
        return cmd(StoreCommandKind::request_details, item);
    }
    return cmd(StoreCommandKind::none);
}

} // namespace maicord::app_detail
