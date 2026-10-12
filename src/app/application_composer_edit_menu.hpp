#pragma once
#include "maicord/ui/text_edit_menu.hpp"
#include "maicord/ui/clipboard.hpp"
namespace maicord::app_detail {
struct ComposerEditClipboard {
    void* context;
    ClipboardTextResult (*read)(void*, Slice<char>) noexcept;
    bool (*write)(void*, Utf8View) noexcept;
    bool has_text;
};
bool application_composer_edit_command(TextEditCommand command, const ComposerEditClipboard* clipboard = nullptr) noexcept;
// spell_offset: model byte offset under the pointer for spelling suggestions;
// the caret when opened from the keyboard.
inline constexpr u32 kComposerEditMenuAtCaret = 0xFFFFFFFFu;
bool application_composer_edit_menu_open(UiRect anchor, u32 spell_offset = kComposerEditMenuAtCaret) noexcept;
bool application_composer_edit_menu_action(u32 action) noexcept;
bool application_composer_edit_menu_synchronize() noexcept;
}
