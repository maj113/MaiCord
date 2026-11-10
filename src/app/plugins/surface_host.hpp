#pragma once

#include "maicord/app/plugins/surfaces.hpp"
#include "maicord/ui/chat/messages/text_presentation.hpp"

namespace maicord { enum class RelationshipType : u8; } // maicord/model/user/relationship_store.hpp

// The application's side of plugin decorations and buttons (decorations.cpp and buttons.cpp are the runtime's):
// what the scenes show goes to the plugin runtime in a bounded prepare pass, and clicks on plugin
// controls go back to it as gestures. Everything here returns at once while no plugin decorates or declares a button
// (g_plugin_surfaces_active is false).
namespace maicord::app_detail {

// Asks the plugins about what the scenes show now: the message rows, their authors, member rows, channel and
// server rows and both headers, each kind only when some plugin decorates it. Runs before message text is rebuilt.
void application_plugin_surfaces_prepare() noexcept;
// What plugins changed (a push, an invalidation, a button state) since the UI last built from it.
bool application_plugin_surfaces_stale() noexcept;
void application_plugin_surfaces_rendered() noexcept;
// Polls the plugin side for changes that need a rebuild; true when it asked for one.
bool application_plugin_surfaces_poll() noexcept;
// The message under the pointer: runs the hover bar's `visible` predicates for it before the bar builds.
void application_plugin_surfaces_hover(u64 message_id) noexcept;
// A click on a plugin control: a button (action_id from its slot node) or a decoration button, with the slot
// surface and what it targets. message_id (the hovered or decorated message) is looked up for the plugin.
bool application_plugin_surfaces_activate(u64 owner_id, const PluginSurfaceTarget& target, u64 action_id) noexcept;
// A click that landed on text a plugin generated (a message accessory or card link or button, the "Show" link).
bool application_plugin_surfaces_text_click(const MessageTextHit& hit) noexcept;
// The open channel changed: buttons whose visibility depends on it are asked again.
void application_plugin_surfaces_context_changed() noexcept;
// The plugin ABI's relationship value (kPluginRelationship*, equal to MC_RELATIONSHIP_*) of a stored type, and
// of what the signed-in account has with a user.
u32 application_plugin_relationship(RelationshipType type) noexcept;
u32 application_plugin_relationship_of(u64 user_id) noexcept;

} // namespace maicord::app_detail
