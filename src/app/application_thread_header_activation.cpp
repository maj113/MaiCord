#include "application_thread_header_activation.hpp"

namespace maicord::app_detail {
namespace {
u64 publication(const ApplicationThreadActionTicket& t) noexcept {
    // UI capture identity includes actor and channel publication, not merely
    // the request owner's revision. Exact host currentness still compares the
    // complete ticket, so this fingerprint is never access authority.
    u64 hash = 14695981039346656037ull;
    const u64 fields[]{t.serial, t.account_scope, t.profile_id, t.self_user_id, t.thread_id,
        static_cast<u64>(t.account_instance), t.channel_revision, t.publication_revision, t.channel_generation};
    for (u64 value : fields) { hash ^= value; hash *= 1099511628211ull; }
    return hash ? hash : 1;
}
UiThreadAction operation(ThreadActionOperation op) noexcept {
    switch (op) {
    case ThreadActionOperation::fetch_self: return UiThreadAction::check_status;
    case ThreadActionOperation::join: return UiThreadAction::join;
    case ThreadActionOperation::leave: return UiThreadAction::leave;
    case ThreadActionOperation::unarchive: return UiThreadAction::unarchive;
    }
    return UiThreadAction::none;
}
bool same_ticket(const ApplicationThreadActionTicket& a, const ApplicationThreadActionTicket& b) noexcept {
    return a.serial == b.serial && a.account_scope == b.account_scope && a.profile_id == b.profile_id &&
        a.self_user_id == b.self_user_id && a.thread_id == b.thread_id && a.account_instance == b.account_instance &&
        a.channel_revision == b.channel_revision && a.publication_revision == b.publication_revision &&
        a.channel_generation == b.channel_generation;
}
UiThreadAction menu_action(UiThreadMenuActions ids, u32 action) noexcept {
    if (!action) return UiThreadAction::none;
    if (action == ids.check_status) return UiThreadAction::check_status;
    if (action == ids.join) return UiThreadAction::join;
    if (action == ids.leave) return UiThreadAction::leave;
    if (action == ids.unarchive) return UiThreadAction::unarchive;
    if (action == ids.retry) return UiThreadAction::retry;
    if (action == ids.cancel) return UiThreadAction::cancel;
    return UiThreadAction::none;
}
ApplicationThreadHeaderActivationResult refuse(const ApplicationThreadHeaderActivationSource& s,
    ApplicationThreadActionEnqueueResult why) noexcept {
    // A refused scoped command is consumed. It never falls through to channel
    // selection, another context action, or a second mutation.
    if (s.notice) s.notice(s.context, s.ticket, why);
    if (s.refresh) s.refresh(s.context);
    return {why, true, false};
}
ApplicationThreadHeaderActivationResult dispatch(const ApplicationThreadHeaderActivationSource& s,
    UiThreadAction action, UiRect anchor = {}) noexcept {
    if (!s.current || !s.view || !s.ticket.thread_id || !s.current(s.context, s.ticket))
        return refuse(s, ApplicationThreadActionEnqueueResult::stale);
    const auto host = s.view(s.context, s.ticket.thread_id);
    if (!same_ticket(host.ticket, s.ticket)) return refuse(s, ApplicationThreadActionEnqueueResult::stale);
    const auto ui = application_thread_header_projection(host);
    if (action == UiThreadAction::menu) {
        const bool opened = s.open_menu && s.open_menu(s.context, s.ticket, anchor);
        return {opened ? ApplicationThreadActionEnqueueResult::accepted : ApplicationThreadActionEnqueueResult::invalid,
            true, opened};
    }
    if (!(ui.enabled_actions & ui_thread_action_bit(action)) || (ui.busy && action != UiThreadAction::cancel))
        return refuse(s, ui.busy ? ApplicationThreadActionEnqueueResult::busy : ApplicationThreadActionEnqueueResult::permission);
    ApplicationThreadActionEnqueueResult admission = ApplicationThreadActionEnqueueResult::invalid;
    if (action == UiThreadAction::retry) {
        admission = s.retry && s.retry(s.context, s.ticket) ? ApplicationThreadActionEnqueueResult::accepted
            : ApplicationThreadActionEnqueueResult::stale;
    } else if (action == UiThreadAction::cancel) {
        admission = s.cancel && s.cancel(s.context, s.ticket) ? ApplicationThreadActionEnqueueResult::accepted
            : ApplicationThreadActionEnqueueResult::stale;
    } else if (s.activate) {
        ThreadActionOperation op{};
        switch (action) {
        case UiThreadAction::check_status: op = ThreadActionOperation::fetch_self; break;
        case UiThreadAction::join: op = ThreadActionOperation::join; break;
        case UiThreadAction::leave: op = ThreadActionOperation::leave; break;
        case UiThreadAction::unarchive: op = ThreadActionOperation::unarchive; break;
        default: return refuse(s, ApplicationThreadActionEnqueueResult::invalid);
        }
        admission = s.activate(s.context, s.ticket, op);
    }
    if (s.refresh) s.refresh(s.context);
    return {admission, true, admission == ApplicationThreadActionEnqueueResult::accepted ||
        admission == ApplicationThreadActionEnqueueResult::coalesced};
}
} // namespace
UiThreadHeaderView application_thread_header_projection(const ApplicationThreadActionView& h) noexcept {
    UiThreadHeaderView ui{};
    ui.thread_id = h.ticket.thread_id;
    ui.visible = ui.thread_id && h.ticket.account_instance && h.ticket.self_user_id;
    ui.source_publication = ui.visible ? publication(h.ticket) : 0;
    ui.membership = h.membership == ApplicationThreadMembershipStatus::joined ? UiThreadMembership::joined
        : h.membership == ApplicationThreadMembershipStatus::not_joined ? UiThreadMembership::not_joined
        : UiThreadMembership::unknown;
    static_assert(thread_action_fetch == ui_thread_action_bit(UiThreadAction::check_status) &&
        thread_action_join == ui_thread_action_bit(UiThreadAction::join) &&
        thread_action_leave == ui_thread_action_bit(UiThreadAction::leave) &&
        thread_action_unarchive == ui_thread_action_bit(UiThreadAction::unarchive) &&
        thread_action_retry == ui_thread_action_bit(UiThreadAction::retry) &&
        thread_action_cancel == ui_thread_action_bit(UiThreadAction::cancel));
    ui.enabled_actions = h.actions;
    ui.operation = operation(h.operation);
    ui.archive_known = h.archived_known; ui.archived = h.archived;
    ui.locked_known = h.locked_known; ui.locked = h.locked;
    ui.busy = h.phase == ApplicationThreadActionPhase::queued || h.phase == ApplicationThreadActionPhase::submitting ||
        h.phase == ApplicationThreadActionPhase::retry_wait;
    ui.status = h.status; ui.status_error = h.error;
    return ui;
}
ApplicationThreadHeaderActivationResult application_thread_header_activate(
    const ApplicationThreadHeaderActivationSource& s, const UiThreadHeaderTarget& target) noexcept {
    if (!target.thread_id || target.action == UiThreadAction::none) return {};
    if (!s.header || target.thread_id != s.ticket.thread_id || target.source_publication != publication(s.ticket) ||
        !ui_thread_header_current(*s.header, target)) return refuse(s, ApplicationThreadActionEnqueueResult::stale);
    const auto* n = s.header->manager ? ui_widget_manager_layout_node(*s.header->manager, target.widget) : nullptr;
    return n ? dispatch(s, target.action, ui_rect_intersection(n->rectangle, n->clip_rectangle))
        : refuse(s, ApplicationThreadActionEnqueueResult::stale);
}
ApplicationThreadHeaderActivationResult application_thread_menu_activate(
    const ApplicationThreadHeaderActivationSource& s, u32 action) noexcept {
    const auto semantic = menu_action(s.menu_actions, action);
    if (semantic == UiThreadAction::none) return {};
    if (!s.popup || !s.popup->active || s.popup->owner_id != s.ticket.thread_id ||
        s.popup->source_revision != s.ticket.publication_revision || !s.popup->items.data || !s.popup->rows.data ||
        s.popup->item_count > s.popup->items.count || s.popup->item_count > s.popup->rows.count)
        return refuse(s, ApplicationThreadActionEnqueueResult::stale);
    bool current_row = false;
    for (u32 i = 0; i < s.popup->item_count; ++i) {
        const auto& row = s.popup->rows[i];
        if (row.source_index >= s.popup->item_count || s.popup->items[row.source_index].action != action) continue;
        const auto& item = s.popup->items[row.source_index];
        const auto* n = ui_widget_manager_layout_node(s.popup->widgets, row.widget);
        current_row = (item.flags & popup_item_enabled) && n && n->visible &&
            ui_rect_has_area(ui_rect_intersection(n->rectangle, n->clip_rectangle)) &&
            ui_widget_manager_input_enabled(s.popup->widgets, row.widget);
        break;
    }
    if (!current_row) return refuse(s, ApplicationThreadActionEnqueueResult::stale);
    return dispatch(s, semantic);
}
} // namespace maicord::app_detail
