#pragma once

// Scene-level pointer shapes. Every resolver follows one rule: an enabled
// target that activates on click is a hand, a text editor is an I-beam and
// anything else is an arrow. Header-only so each host links only the scenes
// it already uses.

#include "maicord/ui/cursor.hpp"
#include "maicord/ui/audio_controls.hpp"
#include "maicord/ui/download_scene.hpp"
#include "maicord/ui/friends_scene.hpp"
#include "maicord/ui/inbox_scene.hpp"
#include "maicord/ui/login_scene.hpp"
#include "maicord/ui/media_picker.hpp"
#include "maicord/ui/message_forward_scene.hpp"
#include "maicord/ui/pinned_messages_scene.hpp"
#include "maicord/ui/poll_controls.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/ui/quick_switcher_scene.hpp"
#include "maicord/ui/reaction_viewer.hpp"
#include "maicord/ui/search_scene.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/thread_browser_scene.hpp"
#include "maicord/ui/video_scene.hpp"

namespace maicord {

inline UiCursor ui_audio_scene_cursor(const UiAudioScene& scene, UiPoint point) noexcept {
    if (!scene.active)
        return UiCursor::arrow;
    if (scene.capture != UiAudioControl::none)
        return UiCursor::hand;
    const auto* descriptor = ui_audio_controls_find(scene.projection, ui_audio_scene_hit(scene, point));
    return ui_action_cursor(descriptor && descriptor->kind != UiAudioControlKind::label);
}

inline UiCursor ui_video_scene_cursor(const UiVideoScene& scene, UiPoint point) noexcept {
    if (!scene.active)
        return UiCursor::arrow;
    if (scene.capture != UiVideoControl::none)
        return UiCursor::hand;
    const auto id = ui_video_scene_hit(scene, point);
    if (id == UiVideoControl::none)
        return UiCursor::arrow;
    for (const auto& descriptor : scene.projection.controls)
        if (descriptor.id == id)
            return ui_action_cursor(descriptor.kind != UiVideoControlKind::label);
    return UiCursor::hand;
}

inline UiCursor ui_poll_controls_cursor(const UiPollProjection& projection, UiPoint point) noexcept {
    const auto target = ui_poll_controls_hit(projection, point);
    return ui_action_cursor(target.control != UiPollControl::none && ui_poll_target_enabled(projection, target));
}

inline UiCursor ui_poll_scene_cursor(const UiPollScene& scene, UiPoint point) noexcept {
    if (!scene.active)
        return UiCursor::arrow;
    const auto target = ui_poll_scene_hit(scene, point);
    return ui_action_cursor(target.control != UiPollControl::none &&
                            ui_poll_target_enabled(scene.projection, target));
}

inline UiCursor ui_poll_voters_scene_cursor(const UiPollVotersScene& scene, UiPoint point) noexcept {
    const auto target = ui_poll_voters_scene_hit(scene, point);
    return ui_action_cursor(target.control != UiPollVotersControl::none &&
                            ui_poll_voters_target_enabled(scene, target));
}

// Settings rows activate only on their actual control: action/notice rows on
// the button, option rows on an option, editors inside their field. Grips and
// an active list drag present the move shape.
inline UiCursor ui_settings_scene_cursor(const UiSettingsScene& scene, float x, float y,
                                         bool dragging = false) noexcept {
    if (dragging)
        return UiCursor::move;
    UiSettingsControlGeometry geometry{};
    if (ui_settings_scene_editor_hit(scene, x, y, &geometry))
        return UiCursor::ibeam;
    const auto target = ui_settings_scene_hit_test(scene, x, y);
    switch (target.kind) {
    case UiSettingsTargetKind::none:
        return UiCursor::arrow;
    case UiSettingsTargetKind::search:
        return UiCursor::ibeam;
    case UiSettingsTargetKind::control:
        break;
    default:
        return UiCursor::hand;
    }
    const auto* control = ui_settings_scene_control(scene, target);
    if (!control || !control->enabled)
        return UiCursor::arrow;
    if (ui_settings_scene_grip_hit(scene, target, x, y))
        return UiCursor::move;
    UiSettingsActivation activation{};
    switch (control->kind) {
    case UiSettingsControlKind::edit:
    case UiSettingsControlKind::multiline:
        return ui_action_cursor(ui_settings_scene_button_hit(scene, target, x, y));
    case UiSettingsControlKind::action:
    case UiSettingsControlKind::notice:
        return ui_action_cursor(ui_settings_scene_button_hit(scene, target, x, y));
    case UiSettingsControlKind::palette:
        return ui_action_cursor(ui_settings_scene_palette_pointer(scene, target, x, y, &activation));
    case UiSettingsControlKind::radio:
        return ui_action_cursor(ui_settings_scene_radio_pointer(scene, target, x, y, &activation));
    case UiSettingsControlKind::segments:
    case UiSettingsControlKind::swatches:
    case UiSettingsControlKind::tabs:
    case UiSettingsControlKind::buttons:
    case UiSettingsControlKind::colors:
        return ui_action_cursor(ui_settings_scene_option_pointer(scene, target, x, y, &activation));
    case UiSettingsControlKind::error:
        return UiCursor::arrow;
    default:
        return UiCursor::hand;
    }
}

inline UiCursor ui_popup_scene_cursor(const PopupScene& scene, float x, float y) noexcept {
    return ui_action_cursor(popup_scene_hit_test(scene, x, y).enabled);
}

inline UiCursor ui_login_scene_cursor(const LoginScene& scene, float x, float y) noexcept {
    if (login_password_reveal_hit_test(scene, x, y))
        return UiCursor::hand;
    const auto control = login_scene_hit_test(scene, x, y);
    return ui_action_cursor(control != LoginControl::none,
                            control == LoginControl::login || control == LoginControl::password ||
                                control == LoginControl::phone_code);
}

inline UiCursor ui_reaction_viewer_cursor(const ReactionViewer& viewer, float x, float y) noexcept {
    const auto kind = reaction_viewer_hit_test(viewer, x, y).kind;
    return ui_action_cursor(kind == ReactionViewerHitKind::close || kind == ReactionViewerHitKind::aggregate ||
                            kind == ReactionViewerHitKind::user);
}

inline UiCursor ui_search_scene_cursor(const UiSearchScene& scene, float x, float y) noexcept {
    const auto action = ui_search_scene_hit_test(scene, x, y).action;
    return ui_action_cursor(action != UiSearchAction::none, action == UiSearchAction::edit);
}

inline UiCursor ui_friends_scene_cursor(const UiFriendsScene& scene, float x, float y) noexcept {
    const auto action = ui_friends_scene_hit_test(scene, x, y).action;
    return ui_action_cursor(action != UiFriendsAction::none, action == UiFriendsAction::search);
}

inline UiCursor ui_inbox_scene_cursor(const UiInboxScene& scene, float x, float y) noexcept {
    return ui_action_cursor(ui_inbox_scene_hit_test(scene, x, y).action != UiInboxAction::none);
}

inline UiCursor ui_pinned_messages_scene_cursor(const UiPinnedMessagesScene& scene, float x, float y) noexcept {
    return ui_action_cursor(ui_pinned_messages_scene_hit_test(scene, x, y).action != UiPinnedMessagesAction::none);
}

inline UiCursor ui_thread_browser_scene_cursor(const UiThreadBrowserScene& scene, float x, float y) noexcept {
    return ui_action_cursor(ui_thread_browser_scene_hit_test(scene, x, y).action != UiThreadBrowserAction::none);
}

inline UiCursor ui_media_picker_scene_cursor(const UiMediaPickerScene& scene, float x, float y) noexcept {
    const auto action = ui_media_picker_scene_hit_test(scene, x, y).action;
    return ui_action_cursor(action != UiMediaPickerAction::none, action == UiMediaPickerAction::edit);
}

// Modal backdrops dismiss on click but stay an arrow.
inline UiCursor ui_message_forward_scene_cursor(const UiMessageForwardScene& scene, float x, float y) noexcept {
    if (!ui_rect_contains(scene.panel, x, y))
        return UiCursor::arrow;
    const auto action = ui_message_forward_scene_hit_test(scene, x, y).action;
    return ui_action_cursor(action != UiMessageForwardAction::none,
                            action == UiMessageForwardAction::edit || action == UiMessageForwardAction::caption);
}

inline UiCursor ui_quick_switcher_scene_cursor(const UiQuickSwitcherScene& scene, float x, float y) noexcept {
    if (!ui_rect_contains(scene.panel, x, y))
        return UiCursor::arrow;
    const auto action = ui_quick_switcher_scene_hit_test(scene, x, y).action;
    return ui_action_cursor(action != UiQuickSwitcherAction::none, action == UiQuickSwitcherAction::edit);
}

inline UiCursor ui_download_scene_cursor(const UiDownloadScene& scene, float x, float y) noexcept {
    if (!ui_rect_contains(scene.panel, x, y))
        return UiCursor::arrow;
    return ui_action_cursor(ui_download_scene_hit_test(scene, x, y).action != UiDownloadAction::none);
}

} // namespace maicord
