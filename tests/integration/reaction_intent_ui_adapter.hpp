#pragma once
#include "application_message_reaction.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/model/message_rich_store.hpp"
#include "maicord/model/message_store.hpp"

namespace maicord {
// Offline transport admits exact production requests; the native probe owns
// the renderer and borrows these actual App model addresses without copying it.
bool reaction_intent_ui_adapter_begin(HWND window, bool (*refresh)(void*) noexcept, void* context) noexcept;
void reaction_intent_ui_adapter_end() noexcept;
bool reaction_intent_ui_adapter_active() noexcept;
bool reaction_intent_ui_adapter_refresh() noexcept;
MessageStore* reaction_intent_ui_adapter_store() noexcept;
MessageRichStore* reaction_intent_ui_adapter_rich_store() noexcept;
ChatStateStore* reaction_intent_ui_adapter_state() noexcept;
UserStore* reaction_intent_ui_adapter_users() noexcept;
// Adds a server emoji reaction through the same owner admission the shared
// emoji picker's reaction popout uses (its UI is covered without a GPU).
app_detail::ApplicationMessageReactionEnqueueResult reaction_intent_ui_adapter_add(u64 message,
                                                                               u64 emoji) noexcept;
app_detail::ApplicationMessageReactionEnqueueResult reaction_intent_ui_adapter_toggle(u64 message,
                                                                                  u32 index) noexcept;
bool reaction_intent_ui_adapter_submit(u64 request_id) noexcept;
bool reaction_intent_ui_adapter_response(
    u16 status, RestClientError error = RestClientError::none) noexcept;
void reaction_intent_ui_adapter_observe(const GatewayMessageReactionEvent&) noexcept;
bool reaction_intent_ui_adapter_synchronize() noexcept;
app_detail::ApplicationMessageReactionMetrics reaction_intent_ui_adapter_metrics() noexcept;
bool reaction_intent_ui_adapter_flight(u64* request_id, u64* message_id, u64* emoji_id, bool* add) noexcept;
} // namespace maicord
