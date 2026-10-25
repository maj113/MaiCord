#include "application_thread_header.hpp"
#include "maicord/core/memory.hpp"
#include "application_account_presentation.hpp"

namespace maicord::app_detail {
float application_thread_header_project(UiThreadHeader* header, UiScene* shell, u64 id,
    UiRect available, UiRect clip) noexcept {
    if (!header || !shell) return 0;
    const bool bound = header->manager == &shell->widgets &&
        ui_widget_manager_resolve(shell->widgets, header->widgets.root);
    if (!bound && !ui_thread_header_bind_shell(header, shell)) return 0;
    const auto style = ui_thread_header_style();
    auto view = application_thread_header_projection(application_thread_actions_view(id));
    const float height = ui_thread_header_height(view, style);
    if (view.visible && (ui_rect_width(available) < 320.f || ui_rect_height(available) < height)) reset_value(&view);
    ui_thread_header_update(header, view, available, clip, style);
    return header->active ? ui_rect_height(header->geometry.rectangle) : 0;
}
ApplicationThreadHeaderActivationSource application_thread_header_activation_source(
    UiThreadHeader* header, const ApplicationThreadActionTicket& ticket) noexcept {
    return {nullptr, header, nullptr, ticket, {},
        [](void*, const ApplicationThreadActionTicket& source) noexcept {
            return application_thread_actions_current(source);
        },
        [](void*, u64 id) noexcept { return application_thread_actions_view(id); },
        [](void*, const ApplicationThreadActionTicket& source, ThreadActionOperation op) noexcept {
            return application_thread_actions_activate(source, op);
        },
        [](void*, const ApplicationThreadActionTicket& source) noexcept { return application_thread_actions_retry(source); },
        [](void*, const ApplicationThreadActionTicket& source) noexcept { return application_thread_actions_cancel(source); },
        [](void*, const ApplicationThreadActionTicket& source, UiRect anchor) noexcept {
            return application_thread_actions_menu_open(source, anchor);
        },
        [](void*) noexcept {
            return application_refresh_thread_actions_menu() && refresh_account_frame(true);
        },
        [](void*, const ApplicationThreadActionTicket& source, ApplicationThreadActionEnqueueResult admission) noexcept {
            return application_thread_actions_admission_status(source, admission);
        }};
}
bool application_thread_header_activate_target(UiThreadHeader* header, const UiThreadHeaderTarget& target) noexcept {
    if (!target.thread_id) return false;
    const auto host = application_thread_actions_view(target.thread_id);
    return application_thread_header_activate(application_thread_header_activation_source(header, host.ticket), target).handled;
}
} // namespace maicord::app_detail
