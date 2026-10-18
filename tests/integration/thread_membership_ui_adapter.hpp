#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/model/message_rich_store.hpp"
#include "maicord/model/message_store.hpp"
#include "maicord/model/thread_member_store.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "application_thread_actions.hpp"
#include "application_thread_header_activation.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {
inline constexpr u64 kThreadMembershipProbeGuild = 96000;
inline constexpr u64 kThreadMembershipProbeParent = 96100;
inline constexpr u64 kThreadMembershipProbePublic = 96110;
inline constexpr u64 kThreadMembershipProbePrivate = 96120;
inline constexpr u64 kThreadMembershipProbeSelf = 96005;

// The adapter owns actual App model backing and an offline exact-request
// transport. The native renderer and its GPU resources always stay borrowed.
bool thread_membership_ui_adapter_begin(HWND window, bool (*refresh)(void*) noexcept,
                                        void* context) noexcept;
void thread_membership_ui_adapter_end() noexcept;
bool thread_membership_ui_adapter_active() noexcept;
bool thread_membership_ui_adapter_refresh() noexcept;
ChatStateStore* thread_membership_ui_adapter_state() noexcept;
UserStore* thread_membership_ui_adapter_users() noexcept;
MessageStore* thread_membership_ui_adapter_store() noexcept;
MessageRichStore* thread_membership_ui_adapter_rich_store() noexcept;
PopupScene* thread_membership_ui_adapter_popup() noexcept;
UiThreadHeader* thread_membership_ui_adapter_header() noexcept;
app_detail::ApplicationThreadActionView thread_membership_ui_adapter_view(u64 thread_id) noexcept;
void thread_membership_ui_adapter_bind_header(UiThreadHeader*, UiRect viewport) noexcept;
bool thread_membership_ui_adapter_open_menu(u64 thread_id, UiRect anchor, UiRect viewport) noexcept;
app_detail::ApplicationThreadHeaderActivationResult thread_membership_ui_adapter_activate(
    const UiThreadHeaderTarget&) noexcept;
app_detail::ApplicationThreadHeaderActivationResult thread_membership_ui_adapter_menu_activate(u32 action) noexcept;
UiThreadMenuActions thread_membership_ui_adapter_menu_actions() noexcept;
bool thread_membership_ui_adapter_submit(u64 request_id) noexcept;
bool thread_membership_ui_adapter_response(u16 status, Utf8View body = {},
    RestClientError error = RestClientError::none) noexcept;
bool thread_membership_ui_adapter_flight(u64* request_id, ThreadActionRequest* request) noexcept;
bool thread_membership_ui_adapter_synchronize() noexcept;
app_detail::ApplicationThreadActionMetrics thread_membership_ui_adapter_metrics() noexcept;
bool thread_membership_ui_adapter_observe_membership(u64 thread_id, bool joined,
                                                    bool known = true) noexcept;
bool thread_membership_ui_adapter_observe_archive(u64 thread_id, bool archived) noexcept;
} // namespace maicord
