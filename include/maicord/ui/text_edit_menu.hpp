#pragma once
#include "maicord/ui/popup_scene.hpp"
namespace maicord {
enum class TextEditCommand : u8 { none, undo, redo, cut, copy, paste, erase, select_all };
inline constexpr u32 kTextEditMenuItems = 7;
struct TextEditMenuState {
    bool undo, redo, selection, text, clipboard, editable, copy_allowed, composing;
};
bool text_edit_command_enabled(TextEditCommand command, TextEditMenuState state) noexcept;
bool text_edit_menu_build(TextEditMenuState state, Slice<PopupItem> items) noexcept;
}
