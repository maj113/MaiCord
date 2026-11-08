#include "store_internal.hpp"
#include "maicord/ui/accessibility/accessibility.hpp"

namespace maicord {
using namespace store_detail;

namespace {

// The first two text leaves under a card or row name it and describe it.
void first_texts(const UiStoreScene& s, u16 index, Utf8View* name, Utf8View* help) noexcept {
    for (u16 child = s.nodes[index].first; child != kUiStoreNone; child = s.nodes[child].next) {
        const auto& n = s.nodes[child];
        if (n.kind == UiStoreKind::text && n.text.bytes) {
            if (!name->bytes)
                *name = n.text;
            else if (!help->bytes)
                *help = n.text;
        } else if (n.first != kUiStoreNone && n.kind != UiStoreKind::button)
            first_texts(s, child, name, help);
        if (help->bytes)
            return;
    }
}

bool describe(const UiStoreScene& s, u16 index, u8 surface, const Utf8View& context, UiAccessNodeDesc* out) noexcept {
    const auto& n = s.nodes[index];
    UiAccessRole role = UiAccessRole::button;
    u16 actions = ui_access_action_invoke, state = ui_access_focusable;
    Utf8View name = n.text, value = n.text2, help{};
    switch (n.kind) {
    case UiStoreKind::card:
        first_texts(s, index, &name, &help);
        break;
    case UiStoreKind::button:
        break;
    case UiStoreKind::icon_button:
        if (!name.bytes)
            name = context;
        break;
    case UiStoreKind::toggle:
        role = UiAccessRole::toggle_button;
        actions = ui_access_action_toggle;
        state |= static_cast<u16>(ui_access_toggleable | (n.value ? ui_access_checked : 0));
        name = context;
        break;
    case UiStoreKind::check:
        role = UiAccessRole::check_box;
        actions = ui_access_action_toggle;
        state |= static_cast<u16>(ui_access_toggleable | (n.value ? ui_access_checked : 0));
        break;
    case UiStoreKind::filter:
        role = UiAccessRole::toggle_button;
        actions = ui_access_action_toggle;
        state |= static_cast<u16>(ui_access_toggleable | ((n.flags & ui_store_selected) ? ui_access_checked : 0));
        break;
    case UiStoreKind::tab:
        role = UiAccessRole::tab;
        actions = ui_access_action_select;
        state |= static_cast<u16>(ui_access_selectable | ((n.flags & ui_store_selected) ? ui_access_selected : 0));
        break;
    case UiStoreKind::segment:
        role = UiAccessRole::radio_button;
        actions = ui_access_action_select;
        state |= static_cast<u16>(ui_access_selectable | ((n.flags & ui_store_selected) ? ui_access_selected : 0));
        value = {};
        break;
    case UiStoreKind::search:
        role = UiAccessRole::edit;
        actions = ui_access_action_focus;
        value = {};
        break;
    case UiStoreKind::dropdown:
        role = UiAccessRole::combo_box;
        actions = ui_access_action_invoke | ui_access_action_expand;
        state |= ui_access_collapsed;
        break;
    case UiStoreKind::slider:
        role = UiAccessRole::slider;
        actions = ui_access_action_focus;
        name = context;
        value = {};
        break;
    default:
        return false;
    }
    if (n.flags & ui_store_disabled)
        state = ui_access_disabled;
    UiAccessNodeDesc& d = *out = {};
    d.key = ui_access_key(surface, (static_cast<u64>(n.action) << 8) ^ static_cast<u64>(n.kind));
    d.item = n.action;
    d.surface = surface;
    d.rectangle = screen_rect(s, index);
    d.visible = ui_rect_intersection(d.rectangle, clip_of(s, index));
    d.name = name;
    d.value = value;
    d.help = help;
    d.role = role;
    d.state = state;
    d.actions = actions;
    return ui_rect_has_area(d.visible);
}

} // namespace

u32 ui_access_add_store(UiAccessTree* tree, u32 parent, const UiStoreScene& s, u8 surface) noexcept {
    if (!tree || !s.active || s.page_root == kUiStoreNone)
        return kUiAccessNone;
    u32 added = kUiAccessNone;
    Utf8View context{};
    // A dialog blocks the page: only its nodes are exposed then.
    const u32 first = s.dialog_open && !s.dialog_bar ? s.dialog_root : 0;
    for (u32 i = first; i < s.count; ++i) {
        const auto& n = s.nodes[i];
        if (n.kind == UiStoreKind::text && n.text.bytes && (n.type == static_cast<u8>(UiStoreType::body_bold) ||
                                                           n.type == static_cast<u8>(UiStoreType::heading)))
            context = n.text;
        if (hidden(n) || !n.action)
            continue;
        UiAccessNodeDesc d; // describe() writes all of it before it answers true
        if (!describe(s, static_cast<u16>(i), surface, context, &d))
            continue;
        if (const u32 node = ui_access_add(tree, parent, d); node != kUiAccessNone && added == kUiAccessNone)
            added = node;
    }
    return added;
}

} // namespace maicord
