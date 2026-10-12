#include "maicord/ui/text_edit_menu.hpp"
namespace maicord {
bool text_edit_command_enabled(TextEditCommand command, TextEditMenuState state) noexcept {
    const bool edit = state.editable && !state.composing;
    switch (command) {
    case TextEditCommand::undo:
        return edit && state.undo;
    case TextEditCommand::redo:
        return edit && state.redo;
    case TextEditCommand::cut:
        return edit && state.selection && state.copy_allowed;
    case TextEditCommand::copy:
        return state.selection && state.copy_allowed;
    case TextEditCommand::paste:
        return edit && state.clipboard;
    case TextEditCommand::erase:
        return edit && state.selection;
    case TextEditCommand::select_all:
        return state.text && !state.composing;
    default:
        return false;
    }
}
bool text_edit_menu_build(TextEditMenuState state, Slice<PopupItem> items) noexcept {
    if (!items.data || items.count < kTextEditMenuItems)
        return false;
    const Utf8View labels[] = {utf8_literal("Undo"),      utf8_literal("Redo"),  utf8_literal("Cut"),
                               utf8_literal("Copy"),      utf8_literal("Paste"), utf8_literal("Delete"),
                               utf8_literal("Select all")};
    const u16 shortcuts[] = {popup_shortcut_undo,      popup_shortcut_redo,  popup_shortcut_cut,
                             popup_shortcut_copy,      popup_shortcut_paste, popup_shortcut_delete,
                             popup_shortcut_select_all};
    const UiIcon icons[]{UiIcon::undo,  UiIcon::redo,  UiIcon::cut,       UiIcon::copy,
                         UiIcon::paste, UiIcon::trash, UiIcon::select_all};
    for (u32 index = 0; index < kTextEditMenuItems; ++index) {
        const auto command = static_cast<TextEditCommand>(index + 1);
        u16 flags = text_edit_command_enabled(command, state) ? popup_item_enabled : 0;
        if (command == TextEditCommand::cut || command == TextEditCommand::select_all)
            flags |= popup_item_separator_before;
        items[index] = {labels[index], index + 1, flags, shortcuts[index]};
        items[index].presentation =
            ui_menu_row_presentation(icons[index]);
    }
    return true;
}
} // namespace maicord
