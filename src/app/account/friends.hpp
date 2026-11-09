#pragma once
#include "maicord/ui/core/input_router.hpp"
#include "maicord/model/chat/message_composer.hpp"
#include "maicord/model/user/relationship_store.hpp"
#include "maicord/ui/widgets/presence.hpp"
#include "maicord/platform/window/event.hpp"

namespace maicord {
struct RestClientResponse;

struct GatewayClientEvent;
} // namespace maicord

namespace maicord::app_detail {
const RelationshipStore* application_friends_relationships() noexcept;
bool application_friends_active() noexcept;
bool application_friends_requests_active() noexcept;
void application_friends_close_requests() noexcept;
bool application_friends_show() noexcept;
// Message Requests: the same page listing DMs from people the user may not know (Requests and Spam).
bool application_friends_show_requests() noexcept;
void application_friends_hide() noexcept;
void application_friends_release() noexcept;
bool application_friends_synchronize() noexcept;
bool application_friends_input(UiInputKind, u32 code, bool repeated) noexcept;
bool application_friends_window_event(const WindowEvent&) noexcept;
bool application_friends_pending(u64 now_ms) noexcept;
bool application_friends_submit(u64 request_id, u64 now_ms) noexcept;
bool application_friends_response(const RestClientResponse&, Utf8View body) noexcept;
u64 application_friends_next_wake() noexcept;
void application_friends_gateway(const GatewayClientEvent&, ConstSlice<u8> payload) noexcept;
bool application_friends_notification_gates(u64 user_id, u32* gates) noexcept;
MessageComposer* application_friends_search_model() noexcept;
bool application_friends_search_focused() noexcept;
bool application_friends_search_changed(bool content) noexcept;
TextInputTarget application_friends_search_target() noexcept;
void application_friends_search_cancel_native_group(const TextInputTarget&) noexcept;
bool application_friends_popup_action(u32 action) noexcept;
// The relationship for the user context menu: 0 none, 1 friend, 2 incoming,
// 3 outgoing, 4 blocked. False until the relationship store is ready.
bool application_friends_user_relation(u64 user_id, u8* relation) noexcept;
// The same relationship, 0 while the store is not ready.
inline u8 application_friends_relation(u64 user_id) noexcept {
    u8 relation = 0;
    application_friends_user_relation(user_id, &relation);
    return relation;
}
// Relationship changes from the user context menu and settings; removing and blocking ask first,
// unblocking and unignoring do not.
enum class ApplicationFriendsUserAction : u8 { add, accept, cancel, remove, block, unblock, unignore };
bool application_friends_user_action(u64 user_id, ApplicationFriendsUserAction action) noexcept;
void application_friends_tick(u64 now_ms) noexcept;
u64 application_friends_search_next_wake() noexcept;
} // namespace maicord::app_detail
