#pragma once
// Shared by interaction.cpp and its parts (interaction_*.cpp); private to them.
#include "../../shell/overlay_input.hpp"
#include "../../guild/community.hpp"
#include "maicord/core/memory.hpp"
#include "../../shell/toast.hpp"
#include "../../shell/content_input.hpp"
#include "../../account/user_actions.hpp"
#include "maicord/ui/account/user_context_menu.hpp"
#include "components.hpp"
#include "../../guild/lifecycle.hpp"
#include "../../guild/menu.hpp"
#include "../../guild/folders.hpp"
#include "../../shell/click_count.hpp"
#include "../../account/login/action_captcha.hpp"
#include "../composer/reply_draft.hpp"
#include "../../voice/voice.hpp"
#include "../../voice/stream.hpp"
#include "../../voice/soundboard.hpp"
#include "../composer/commands.hpp"
#include "../../account/friends.hpp"
#include "../../account/profile.hpp"
#include "interaction.hpp"
#include "forward.hpp"
#include "../../media/downloads.hpp"
#include "../../guild/member_moderation.hpp"
#include "../composer/edit_menu.hpp"
#include "../composer/attachments.hpp"
#include "../../plugins/plugins.hpp"

#include "../../account/account_presentation.hpp"
#include "../composer/composer.hpp"
#include "feedback.hpp"
#include "delete.hpp"
#include "edit.hpp"
#include "history.hpp"
#include "reaction.hpp"
#include "../threads/header.hpp"
#include "pinned_messages.hpp"
#include "../application_private_conversation.hpp"
#include "../private_channel.hpp"
#include "../../shell/state.hpp"
#include "../read_state.hpp"
#include "../scoped_read_menu.hpp"
#include "../../notifications/controls.hpp"
#include "../../account/account_menu.hpp"
#include "../../media/picker.hpp"
#include "../../shell/retained_projection.hpp"
#include "../../shell/scrolling.hpp"

#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/text/clipboard.hpp"
#include "maicord/ui/guild/channel_context_policy.hpp"
#include "maicord/ui/shell/navigation_context.hpp"
#include "../../guild/channel_overview.hpp"
#include "../../guild/settings/settings.hpp"
#include "../threads/browser.hpp"
#include "../threads/creation.hpp"
#include "maicord/ui/guild/context_policy.hpp"
#include "maicord/ui/chat/messages/text_presentation.hpp"
#include "maicord/ui/chat/unread_context.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/ui/chat/messages/received_attachment_menu.hpp"
#include "maicord/ui/chat/messages/author.hpp"

namespace maicord::app_detail {
namespace interaction_detail {}
using namespace interaction_detail;
namespace interaction_detail {

extern UnreadContextTarget unread_popup;

struct ThreadMenuScope {
    ApplicationThreadActionTicket ticket;
    bool active, from_header;
};

extern ThreadMenuScope thread_menu_scope;

struct ThreadBrowserMenuScope {
    u64 account, profile, self, parent;
    u16 generation;
};

extern ThreadBrowserMenuScope browser_menu_scope;

struct MessageThreadCreationMenuScope {
    u64 account, profile, self, parent, message;
    u32 message_revision;
    u16 channel_generation, message_generation;
    bool available;
};

extern MessageThreadCreationMenuScope creation_menu_scope;
bool creation_menu_current(const MessageThreadCreationMenuScope& source) noexcept;
bool thread_menu_action(u32 action) noexcept;
ApplicationThreadHeaderActivationSource thread_menu_activation_source() noexcept;
UnreadContextSource unread_source(UnreadContextKind kind, u64 id, bool retry = false) noexcept;
bool retry_failed_message(u64 nonce) noexcept;
bool discard_failed_message(u64 nonce) noexcept;
bool activate_message_link_action(u32 action) noexcept;
bool copy_message_link(u64 message_id) noexcept;
bool copy_message_id(u64 message_id, bool with_channel) noexcept;
bool open_reaction_viewer(u64 message_id) noexcept;

} // namespace interaction_detail
} // namespace maicord::app_detail
