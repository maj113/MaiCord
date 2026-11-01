#pragma once
#include "maicord/core/hash.hpp"
#include "maicord/ui/widgets/badge.hpp"
#include "maicord/ui/chat/messages/text_presentation.hpp"
#include "maicord/model/user/store.hpp"

namespace maicord {

// The "<user> used <command>" header above an application's reply, one 18-DIP
// line to the right of the 16-DIP user avatar:
//   name (muted) · server tag chip (muted) · "used" · command
// The command of a slash command is a chip (grid icon, name without the
// slash); any other command is the name as plain link-coloured text.
inline constexpr float kMessageInteractionNameGap = 4.0f;    // after the name
inline constexpr float kMessageInteractionTagGap = 2.0f;     // before the server tag chip
inline constexpr float kMessageInteractionWordGap = 4.0f;    // a space beside "used"
inline constexpr float kMessageInteractionChipPadding = 2.0f;
inline constexpr float kMessageInteractionChipIcon = 10.0f;
inline constexpr float kMessageInteractionChipIconGap = 3.0f;
inline constexpr float kMessageInteractionChipRadius = 3.0f;
inline constexpr float kMessageInteractionAvatar = 16.0f;

struct MessageInteractionLayout {
    UiTextCommand name, used, command;
    // The interaction user's server tag chip; no label without one.
    UiBadgeWidget tag;
    const UserAppearanceRecord* appearance;
    // The slash command chip and its icon; empty for a plain command name.
    UiRect chip, chip_icon;
    bool chip_hovered;
    u64 user_id, guild_id, message_id, channel_id;
};

enum class MessageInteractionTarget : u8 { none, avatar, name, tag, command };
// MessageScene::header_hover_target values 1..4 are these targets.

struct MessageInteractionHit {
    MessageInteractionTarget target;
    u64 user_id, guild_id, message_id, channel_id;
    // The target's rectangle: popouts anchor to it.
    UiRect anchor;
};

// The row whose header line owns the command, when that line is an
// interaction header ("used" spans on a reply-region command).
const MessageSceneRowWidget* message_interaction_command_row(const MessageScene& scene,
                                                             const MessageTextCommand& c) noexcept;

bool message_interaction_layout(const MessageScene& scene, const MessageTextCommand& command,
                                const UserStore& users, UiTextMeasureCache* cache, const Theme& theme,
                                MessageInteractionLayout* out) noexcept;

// The chip's fill and icon (the server tag chip paints through ui_badge_paint).
bool message_interaction_paint(const MessageInteractionLayout& layout, const MessageTextCommand& command,
                               const Theme& theme, PaintStream* paint) noexcept;

// What lies under a point of an interaction header: the user's avatar and
// name (profile), the server tag chip (server tag popout) and the command.
MessageInteractionHit message_interaction_hit_test(const MessageScene& scene,
                                                   const MessageTextPresentation& text, const UserStore& users,
                                                   UiTextMeasureCache* cache, const Theme& theme, float x,
                                                   float y) noexcept;

// The command chip (a plain command name's rectangle) of one message's
// interaction header in the visible lines: the popout anchors to it. False
// when that header is not laid out.
bool message_interaction_command_anchor(const MessageScene& scene, const MessageTextPresentation& text,
                                        const UserStore& users, UiTextMeasureCache* cache, const Theme& theme,
                                        u64 message_id, UiRect* anchor) noexcept;

// A quoted reply's author: the 16-DIP avatar and the name at the start of the
// preview line open that user's profile (the rest of the line jumps to the
// quoted message). Quoted join and app notices show no author.
MessageInteractionHit message_reply_author_hit_test(const MessageScene& scene,
                                                    const MessageTextPresentation& text,
                                                    UiTextMeasureCache* cache, float x, float y) noexcept;

} // namespace maicord
