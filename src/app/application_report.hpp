#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/user_social.hpp"
#include "maicord/ui/input_router.hpp"

namespace maicord::app_detail {
struct SettingsLayer;
// In-app reports for messages and users: the menu-driven reporting API.
// The menu is fetched, its node tree is walked as modal steps (options lead
// to child nodes, checkbox groups collect answers) and a final confirmation
// submits the path and answers. Hosted by the channel overview modal host.
struct ApplicationReportTarget {
    ReportMenuName name;
    u64 channel_id, message_id; // message reports
    u64 user_id, guild_id;      // the reported user; guild is the contextual server
};
bool application_report_open(const ApplicationReportTarget&) noexcept;
bool application_report_active() noexcept;
void application_report_close(bool restore_focus = true) noexcept;
void application_report_release() noexcept;
bool application_report_synchronize() noexcept;
bool application_report_input(UiInputKind, u32 key, bool repeated) noexcept;
bool application_report_window_message(UINT, WPARAM, LPARAM) noexcept;
SettingsLayer* application_report_settings_layer() noexcept;
u64 application_report_next_wake() noexcept;
u64 application_report_caret_wake() noexcept;
void application_report_tick(u64 now) noexcept;
bool application_report_submit(u64 now) noexcept;
bool application_report_owns_response(u64 request_id) noexcept;
bool application_report_response(const RestClientResponse&, Utf8View body) noexcept;
u32 application_report_owner_bytes() noexcept;
} // namespace maicord::app_detail
