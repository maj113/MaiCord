#pragma once

#include "maicord/ui/poll_controls.hpp"
#include "maicord/ui/paint.hpp"

namespace maicord {
bool ui_poll_controls_paint(const UiPollProjection&, const UiPollStyle&, UiRect clip, const Theme&,
                            UiWidgetReference, PaintStream*, UiPollTarget focus = {},
                            UiPollTarget hover = {}, bool keyboard_focus = false) noexcept;
bool ui_poll_controls_text_output(const UiPollProjection&, const UiPollStyle&, UiRect clip,
                                  UiWidgetReference, UiTextOutput*, UiPollTarget hover = {}) noexcept;
bool ui_poll_scene_paint(const UiPollScene&, const Theme&, PaintStream*) noexcept;
bool ui_poll_scene_text_output(const UiPollScene&, UiTextOutput*) noexcept;
u64 ui_poll_scene_paint_revision(const UiPollScene&) noexcept;
u64 ui_poll_scene_text_revision(const UiPollScene&) noexcept;
bool ui_poll_voters_scene_paint(const UiPollVotersScene&, const Theme&, PaintStream*) noexcept;
bool ui_poll_voters_scene_text_output(const UiPollVotersScene&, UiTextOutput*) noexcept;
u64 ui_poll_voters_scene_paint_revision(const UiPollVotersScene&) noexcept;
u64 ui_poll_voters_scene_text_revision(const UiPollVotersScene&) noexcept;
} // namespace maicord
