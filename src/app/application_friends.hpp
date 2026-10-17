#pragma once
#include "maicord/ui/friends_scene.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/model/relationship_store.hpp"

namespace maicord::app_detail {
struct ApplicationFriends;
const RelationshipStore* application_friends_relationships() noexcept;
// Relationship presence as a UI dot; `unknown` is what an unreported presence shows.
constexpr UiPresence application_friends_presence(RelationshipPresence value,
                                                  UiPresence unknown = UiPresence::unknown) noexcept {
    return value == RelationshipPresence::online    ? UiPresence::online
           : value == RelationshipPresence::idle    ? UiPresence::idle
           : value == RelationshipPresence::dnd     ? UiPresence::busy
           : value == RelationshipPresence::offline ? UiPresence::offline
                                                    : unknown;
}
bool application_friends_active() noexcept;
bool application_friends_requests_active() noexcept;
void application_friends_close_requests() noexcept;
bool application_friends_show() noexcept;
void application_friends_hide() noexcept;
void application_friends_release() noexcept;
bool application_friends_synchronize() noexcept;
bool application_friends_input(UiInputKind, u32 code, bool repeated) noexcept;
bool application_friends_window_message(UINT, WPARAM, LPARAM) noexcept;
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
// Relationship changes from the user context menu; removing and blocking ask first.
enum class ApplicationFriendsUserAction : u8 { add, accept, cancel, remove, block, unblock };
bool application_friends_user_action(u64 user_id, ApplicationFriendsUserAction action) noexcept;
void application_friends_tick(u64 now_ms) noexcept;
u64 application_friends_search_next_wake() noexcept;
} // namespace maicord::app_detail
