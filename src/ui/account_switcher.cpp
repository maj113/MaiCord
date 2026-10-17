#include "maicord/ui/account_switcher.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {

PopupItem action(Utf8View label, UiAccountSwitcherAction value, UiIcon icon, u16 flags = 0) noexcept {
    PopupItem result{label, static_cast<u32>(value), static_cast<u16>(popup_item_enabled | flags), 0};
    result.presentation = ui_menu_row_presentation(icon);
    return result;
}

// Whole code points only; the caller's view may be longer than the slot.
u32 copy_prefix(char* output, u32 capacity, Utf8View value) noexcept {
    if (!value.data)
        return 0;
    u32 bytes = minimum(value.bytes, capacity);
    while (bytes < value.bytes && bytes && (static_cast<u8>(value.data[bytes]) & 0xc0) == 0x80)
        --bytes;
    copy_bytes(output, value.data, bytes);
    return bytes;
}

bool label_valid(Utf8View value) noexcept {
    if (!value.bytes)
        return true;
    if (!value.data)
        return false;
    for (u32 i = 0; i < value.bytes; ++i)
        if (static_cast<u8>(value.data[i]) < 0x20)
            return false;
    return true;
}

} // namespace

bool ui_account_switcher_build(UiAccountSwitcherModel* model, u64 owner_id, UiAccountSwitcherPanel panel,
                               ConstSlice<UiAccountSwitcherAccount> accounts, bool can_add) noexcept {
    if (!model || !owner_id || accounts.count > kUiAccountSwitcherAccounts || (!accounts.data && accounts.count))
        return false;
    for (u32 i = 0; i < accounts.count; ++i)
        if (!accounts[i].user_id || !label_valid(accounts[i].display_name) || !label_valid(accounts[i].username))
            return false;
    *model = {};
    model->owner_id = owner_id;
    model->panel = panel;
    const bool manage = panel == UiAccountSwitcherPanel::manage_accounts;
    for (u32 i = 0; i < accounts.count; ++i) {
        const auto& account = accounts[i];
        const Utf8View name = account.display_name.bytes ? account.display_name
                              : account.username.bytes   ? account.username
                                                         : utf8_literal("Discord account");
        u32 label = 0;
        if (manage) {
            constexpr Utf8View prefix = utf8_literal("Log out of ");
            copy_bytes(model->labels[i], prefix.data, prefix.bytes);
            label = prefix.bytes;
        }
        label += copy_prefix(model->labels[i] + label, kUiAccountSwitcherLabelBytes - label, name);
        Utf8View detail{};
        if (account.needs_login)
            detail = utf8_literal("Log in again");
        else if (manage && account.active)
            detail = utf8_literal("Active account");
        else if (account.username.bytes && !utf8_equal(account.username, name))
            detail = {model->details[i], copy_prefix(model->details[i], kUiAccountSwitcherNameBytes, account.username)};
        auto& item = model->items[model->count++];
        u16 flags = popup_item_enabled | popup_item_avatar;
        if (!manage && account.active)
            flags |= popup_item_checked;
        if (manage)
            flags |= popup_item_danger;
        item = {{model->labels[i], label}, static_cast<u32>(UiAccountSwitcherAction::account_first) + i, flags, 0};
        item.detail = detail;
        item.presentation = {};
        item.presentation.secondary_line = detail.bytes != 0;
        item.media_id = account.user_id;
        item.media_variant = account.avatar_variant;
        item.media_flags = account.avatar_animated ? 1 : 0;
    }
    auto& add = model->items[model->count++];
    add = action(utf8_literal("Add an account"), UiAccountSwitcherAction::add_account, UiIcon::user,
                 accounts.count ? popup_item_separator_before : 0);
    if (!can_add) {
        add.flags = static_cast<u16>(add.flags & ~popup_item_enabled);
        add.detail = utf8_literal("You can switch between up to 5 accounts.");
        add.presentation.secondary_line = true;
    }
    model->items[model->count++] =
        manage ? action(utf8_literal("Back"), UiAccountSwitcherAction::back, UiIcon::chevron_left)
               : action(utf8_literal("Manage accounts"), UiAccountSwitcherAction::manage_accounts, UiIcon::settings);
    return true;
}

ConstSlice<PopupItem> ui_account_switcher_items(const UiAccountSwitcherModel& model) noexcept {
    return {model.items, model.count <= kUiAccountSwitcherItemCapacity ? model.count : 0};
}

bool ui_account_switcher_action_enabled(const UiAccountSwitcherModel& model, u32 value) noexcept {
    const auto items = ui_account_switcher_items(model);
    for (u32 i = 0; i < items.count; ++i)
        if (items[i].action == value && (items[i].flags & popup_item_enabled))
            return true;
    return false;
}

u32 ui_account_switcher_account_index(u32 action) noexcept {
    const u32 first = static_cast<u32>(UiAccountSwitcherAction::account_first);
    return action >= first && action < first + kUiAccountSwitcherAccounts ? action - first : ~0u;
}

} // namespace maicord
