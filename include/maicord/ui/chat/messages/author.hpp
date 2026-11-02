#pragma once
#include "maicord/core/hash.hpp"
#include "maicord/ui/widgets/badge.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/theme/background_presentation.hpp"
#include "maicord/ui/chat/messages/text_presentation.hpp"
#include "maicord/ui/plugins/slots.hpp"
#include "maicord/model/user/store.hpp"

namespace maicord {
struct MessageAuthorLayout {
    UiTextCommand name, timestamp;
    UiBadgeWidget badge;
    const UserAppearanceRecord* appearance;
    // Plugin message accessory, after the author badge.
    UiPluginSlotBadge plugin;
    // Plugin decorations (message decorations): muted text around the name and up
    // to four badges after the accessory, read from the host cache.
    UiTextCommand name_prefix, name_suffix;
    UiPluginSlotBadge decorations[kUiPluginRenderBadges];
    u32 decoration_count;
};

// Plugin decoration tones are theme roles; MC_TONE_ROLE keeps the author colour.
inline Color message_author_decoration_color(const Theme& theme, u8 tone, const Color& author) noexcept {
    if (tone == kUiPluginToneRole)
        return author;
    return tone ? ui_icon_role_color(theme, ui_plugin_slot_role(tone)) : ui_background_muted_text_color(theme);
}

struct MessageProfileHit {
    u64 user_id{};
    UiRect anchor{};
};

// One header geometry for text, badge paint and its server icon.
bool message_author_layout(const MessageScene& scene, const MessageTextCommand& command,
                           const ChatStateStore& state, const UserStore& users,
                           UiTextMeasureCache* cache, const Theme& theme,
                           MessageAuthorLayout* out,
                           const GuildMemberStore* members = nullptr) noexcept;

// Compact rows have no author line: the name is the bold role-colored prefix
// of the content flow's first line.
MessageProfileHit message_author_compact_hit(const MessageScene& scene, const MessageTextCommand& command,
                                             UiTextMeasureCache* cache, float x, float y) noexcept;

// The row whose author line owns the command; null for grouped rows and foreign commands.
const MessageSceneRowWidget* message_author_command_row(const MessageScene& scene,
                                                        const MessageTextCommand& c) noexcept;

MessageProfileHit message_author_hit_test(const MessageScene& scene,
                                          const MessageTextPresentation& text,
                                          const ChatStateStore& state,
                                          const UserStore& users,
                                          UiTextMeasureCache* cache, const Theme& theme,
                                          float x, float y) noexcept;

// The server tag chip on a message's author line: the tag's server and the
// chip (the server tag popout anchors to it). Bot "APP" badges are not tags.
struct MessageGuildTagHit {
    u64 guild_id;
    UiRect chip;
};
MessageGuildTagHit message_author_guild_tag_hit_test(const MessageScene& scene, const MessageTextPresentation& text,
                                                     const ChatStateStore& state, const UserStore& users,
                                                     UiTextMeasureCache* cache, const Theme& theme, float x,
                                                     float y) noexcept;

// The check leading a verified app's APP tag on a message's author line: its
// tooltip reads "Verified App" (the client wraps only the check).
struct MessageAppCheckHit {
    UiWidgetReference owner;
    u64 message_id;
    u32 revision;
    UiRect check;
};
MessageAppCheckHit message_author_app_check_hit_test(const MessageScene& scene, const MessageTextPresentation& text,
                                                     const ChatStateStore& state, const UserStore& users,
                                                     UiTextMeasureCache* cache, const Theme& theme, float x,
                                                     float y) noexcept;

// A plugin accessory button on a message's author line.
struct MessagePluginSlotHit {
    u64 owner_id, action_id, channel_id, message_id;
};
MessagePluginSlotHit message_author_plugin_hit_test(const MessageScene& scene, const MessageTextPresentation& text,
                                                    const ChatStateStore& state, const UserStore& users,
                                                    UiTextMeasureCache* cache, const Theme& theme, float x,
                                                    float y) noexcept;
} // namespace maicord
