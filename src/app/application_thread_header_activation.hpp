#pragma once
#include "application_thread_actions.hpp"
#include "maicord/ui/thread_header.hpp"

namespace maicord::app_detail {
// Shared by production native dispatch and its test/probe adapters. Callbacks
// borrow the UI-thread host only for this call. The host independently admits
// the exact source ticket before queueing work.
struct ApplicationThreadHeaderActivationSource {
    void* context;
    UiThreadHeader* header;
    PopupScene* popup;
    ApplicationThreadActionTicket ticket;
    UiThreadMenuActions menu_actions;
    bool (*current)(void*, const ApplicationThreadActionTicket&) noexcept;
    ApplicationThreadActionView (*view)(void*, u64) noexcept;
    ApplicationThreadActionEnqueueResult (*activate)(void*, const ApplicationThreadActionTicket&,
        ThreadActionOperation) noexcept;
    bool (*retry)(void*, const ApplicationThreadActionTicket&) noexcept;
    bool (*cancel)(void*, const ApplicationThreadActionTicket&) noexcept;
    bool (*open_menu)(void*, const ApplicationThreadActionTicket&, UiRect) noexcept;
    bool (*refresh)(void*) noexcept;
    bool (*notice)(void*, const ApplicationThreadActionTicket&, ApplicationThreadActionEnqueueResult) noexcept;
};
struct ApplicationThreadHeaderActivationResult {
    ApplicationThreadActionEnqueueResult admission;
    bool handled, accepted;
};
UiThreadHeaderView application_thread_header_projection(const ApplicationThreadActionView&) noexcept;
ApplicationThreadHeaderActivationResult application_thread_header_activate(
    const ApplicationThreadHeaderActivationSource&, const UiThreadHeaderTarget&) noexcept;
ApplicationThreadHeaderActivationResult application_thread_menu_activate(
    const ApplicationThreadHeaderActivationSource&, u32 action) noexcept;
} // namespace maicord::app_detail
