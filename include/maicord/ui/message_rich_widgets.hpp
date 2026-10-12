#pragma once

#include "maicord/ui/message_rich_layout.hpp"
#include "maicord/ui/widget_manager.hpp"

namespace maicord {

struct MessageSceneRowWidget;

void message_rich_widgets_reset_handles(MessageSceneRowWidget* row) noexcept;
bool message_rich_widgets_structure_matches(const MessageSceneRowWidget& row, const MessageRichRecord* rich,
                                            bool poll = false, u32 invites = 0) noexcept;
bool message_rich_widgets_build(UiWidgetManager* manager, UiWidgetHandle parent, u32 row_widget_id,
                                const MessageRichRecord* rich, MessageSceneRowWidget* row,
                                u32 invites = 0) noexcept;
bool message_rich_widgets_apply(UiWidgetManager* manager, const MessageRichRecord* rich,
                                MessageSceneRowWidget* row) noexcept;

} // namespace maicord
