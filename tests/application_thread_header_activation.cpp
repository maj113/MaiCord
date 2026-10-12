#include "application_thread_header_activation.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/ui/button_keyboard.hpp"

namespace maicord {
namespace {
using namespace app_detail;
struct ThreadActivationFixture {
    UiScene shell;
    UiThreadHeader header;
    PopupScene popup;
    PopupItem items[8];
    PopupRowWidget rows[8];
    UiWidget popup_widgets[18];
    UiLayoutNode popup_nodes[18];
    ApplicationThreadActionView host;
    ApplicationThreadActionEnqueueResult admission;
    u32 calls, retries, cancels, menus, refreshes, notices;
    ThreadActionOperation selected;
    bool current;
};
ThreadActivationFixture thread_activation;
bool ticket_same(const ApplicationThreadActionTicket& a, const ApplicationThreadActionTicket& b) noexcept {
    return a.serial == b.serial && a.account_scope == b.account_scope && a.profile_id == b.profile_id &&
        a.self_user_id == b.self_user_id && a.thread_id == b.thread_id && a.account_instance == b.account_instance &&
        a.channel_revision == b.channel_revision && a.publication_revision == b.publication_revision &&
        a.channel_generation == b.channel_generation;
}
bool current(void* opaque, const ApplicationThreadActionTicket& ticket) noexcept {
    auto& f = *static_cast<ThreadActivationFixture*>(opaque);
    return f.current && ticket_same(ticket, f.host.ticket);
}
ApplicationThreadActionView view(void* opaque, u64 id) noexcept {
    auto& f = *static_cast<ThreadActivationFixture*>(opaque);
    return id == f.host.ticket.thread_id ? f.host : ApplicationThreadActionView{};
}
ApplicationThreadActionEnqueueResult activate(void* opaque, const ApplicationThreadActionTicket& ticket,
    ThreadActionOperation op) noexcept {
    auto& f = *static_cast<ThreadActivationFixture*>(opaque);
    if (!current(opaque, ticket)) return ApplicationThreadActionEnqueueResult::stale;
    ++f.calls; f.selected = op;
    return f.admission;
}
bool retry(void* opaque, const ApplicationThreadActionTicket& ticket) noexcept {
    if (!current(opaque, ticket)) return false;
    ++static_cast<ThreadActivationFixture*>(opaque)->retries; return true;
}
bool cancel(void* opaque, const ApplicationThreadActionTicket& ticket) noexcept {
    if (!current(opaque, ticket)) return false;
    ++static_cast<ThreadActivationFixture*>(opaque)->cancels; return true;
}
bool open_menu(void* opaque, const ApplicationThreadActionTicket& ticket, UiRect) noexcept {
    if (!current(opaque, ticket)) return false;
    ++static_cast<ThreadActivationFixture*>(opaque)->menus; return true;
}
bool refresh(void* opaque) noexcept { ++static_cast<ThreadActivationFixture*>(opaque)->refreshes; return true; }
bool notice(void* opaque, const ApplicationThreadActionTicket& ticket, ApplicationThreadActionEnqueueResult) noexcept {
    auto& f = *static_cast<ThreadActivationFixture*>(opaque);
    if (!current(opaque, ticket)) return false;
    ++f.notices; return true;
}
ApplicationThreadHeaderActivationSource source() noexcept {
    auto& f = thread_activation;
    return {&f, &f.header, &f.popup, f.host.ticket, {801, 802, 803, 804, 805, 806},
        current, view, activate, retry, cancel, open_menu, refresh, notice};
}
bool project() noexcept {
    auto& f = thread_activation;
    return ui_thread_header_update(&f.header, application_thread_header_projection(f.host),
        {312, 48, 632, 128}, {0, 0, 632, 480}, ui_thread_header_style());
}
bool popup() noexcept {
    auto& f = thread_activation;
    PopupItem items[6]{};
    const auto source_view = application_thread_header_projection(f.host);
    const u32 count = ui_thread_menu_build(source_view, source().menu_actions, {items, 6});
    return count && popup_scene_open(&f.popup, PopupKind::context_menu, f.host.ticket.thread_id,
        {items, count}, {470, 88, 502, 120}, {0, 0, 632, 480}, 240.f, false, 0, 0,
        f.host.ticket.publication_revision).ok();
}
}
void test_application_thread_header_activation(void (*check)(bool, const char*) noexcept) noexcept {
    auto& f = thread_activation;
    f = {};
    f.host.ticket = {12, 3, 4, 5, 55, 6, 7, 8, 9};
    f.host.archived_known = true; f.host.locked_known = true;
    f.host.membership = ApplicationThreadMembershipStatus::unknown;
    f.host.actions = thread_action_fetch;
    f.current = true; f.admission = ApplicationThreadActionEnqueueResult::accepted;
    check(ui_scene_initialize_bootstrap(&f.shell) && ui_scene_layout(&f.shell, 632, 480) &&
        ui_thread_header_bind_shell(&f.header, &f.shell) &&
        popup_scene_initialize(&f.popup, {f.items, 8}, {f.rows, 8}, {f.popup_widgets, 18}, {f.popup_nodes, 18}) && project(),
        "thread activation: actual shell widgets and native popup bind the shared shipping dispatcher");
    if (!f.header.manager) { ui_scene_release(&f.shell); return; }
    const auto initial = ui_thread_header_widget_target(f.header, f.header.widgets.primary);
    const auto initial_source = source();
    const auto accepted = application_thread_header_activate(initial_source, initial);
    check(accepted.handled && accepted.accepted && f.calls == 1 && f.selected == ThreadActionOperation::fetch_self,
        "thread activation: Unknown Refresh dispatches exact fetch_self once through typed host admission");
    f.current = false;
    const auto stale = application_thread_header_activate(initial_source, initial);
    check(stale.handled && !stale.accepted && f.calls == 1 && !f.notices,
        "thread activation: retired actor ticket consumes the command without enqueue or attaching feedback to another scope");
    f.current = true;
    f.host.membership = ApplicationThreadMembershipStatus::not_joined;
    f.host.actions = thread_action_join | thread_action_fetch;
    ++f.host.ticket.publication_revision; project();
    const auto join = ui_thread_header_widget_target(f.header, f.header.widgets.primary);
    UiButtonKeyboard keyboard{};
    ui_widget_manager_focus(&f.shell.widgets, join.widget);
    const auto press = ui_button_keyboard_key(&keyboard, &f.shell.widgets, UiButtonKey::space, true);
    const auto keyboard_source = source();
    const auto release = ui_button_keyboard_key(&keyboard, &f.shell.widgets, UiButtonKey::space, false);
    const auto joined = release.activate ? application_thread_header_activate(keyboard_source, join)
        : ApplicationThreadHeaderActivationResult{};
    check(press.handled && release.activate && joined.accepted && f.selected == ThreadActionOperation::join && f.calls == 2,
        "thread activation: keyboard Space uses the native focused widget and captured exact Join source on release");
    ui_button_keyboard_key(&keyboard, &f.shell.widgets, UiButtonKey::space, true);
    const auto retired_keyboard = source();
    ++f.host.ticket.channel_revision;
    project();
    ui_button_keyboard_key(&keyboard, &f.shell.widgets, UiButtonKey::space, false);
    const auto no_stale_join = application_thread_header_activate(retired_keyboard, join);
    check(no_stale_join.handled && !no_stale_join.accepted && f.calls == 2,
        "thread activation: channel metadata change between keyboard press and release refuses the original Join");
    f.host.membership = ApplicationThreadMembershipStatus::joined;
    f.host.actions = thread_action_leave | thread_action_fetch;
    ++f.host.ticket.publication_revision; project();
    const auto overflow = ui_thread_header_widget_target(f.header, f.header.widgets.overflow);
    check(application_thread_header_activate(source(), overflow).accepted && f.menus == 1 && f.calls == 2,
        "thread activation: Joined overflow opens actions without implicitly leaving the thread");
    check(popup(), "thread activation: joined menu publishes native visible Leave row");
    const auto menu_source = source();
    check(application_thread_menu_activate(menu_source, menu_source.menu_actions.leave).accepted &&
        f.calls == 3 && f.selected == ThreadActionOperation::leave && f.popup.active,
        "thread activation: native visible menu Leave is scoped independently and retains the popup for queued feedback");
    f.host.actions = thread_action_fetch;
    const auto refused = application_thread_menu_activate(source(), source().menu_actions.leave);
    check(refused.handled && !refused.accepted && refused.admission == ApplicationThreadActionEnqueueResult::permission &&
        f.calls == 3 && f.notices == 1 && f.popup.active,
        "thread activation: changed host permission consumes native menu selection and publishes scoped refusal without falling through");
    ++f.host.ticket.publication_revision;
    check(!application_thread_menu_activate(menu_source, menu_source.menu_actions.leave).accepted && f.calls == 3,
        "thread activation: stale popup publication cannot enqueue a refreshed source action");
    check(!application_thread_menu_activate(source(), 9999).handled,
        "thread activation: unrelated menu action remains owned by ordinary dispatch");
    f.host.archived = true; f.host.actions = thread_action_unarchive | thread_action_fetch;
    project(); popup();
    check(application_thread_menu_activate(source(), source().menu_actions.unarchive).accepted &&
        f.selected == ThreadActionOperation::unarchive && f.calls == 4,
        "thread activation: Archived explicitly dispatches Unarchive rather than Join or an implicit read mutation");
    f.host.phase = ApplicationThreadActionPhase::retry_wait; f.host.operation = ThreadActionOperation::unarchive;
    f.host.actions = thread_action_cancel; f.host.status = utf8_literal("Waiting before retrying Unarchive.");
    ++f.host.ticket.publication_revision; project(); popup();
    const auto cancelled = application_thread_header_activate(source(), ui_thread_header_widget_target(f.header, f.header.widgets.cancel));
    check(cancelled.handled && cancelled.accepted && f.cancels == 1 && f.calls == 4,
        "thread activation: rate-wait Cancel targets the same operation without issuing a duplicate request");
    f.host.phase = ApplicationThreadActionPhase::failed; f.host.actions = thread_action_retry | thread_action_cancel;
    ++f.host.ticket.publication_revision; project();
    check(application_thread_header_activate(source(), ui_thread_header_widget_target(f.header, f.header.widgets.primary)).accepted &&
        f.retries == 1 && f.calls == 4,
        "thread activation: failed Retry is an exact-owner retry callback rather than a new guessed operation");
    f.host.ticket.account_scope++;
    const auto changed_actor = application_thread_header_projection(f.host);
    check(changed_actor.source_publication != f.header.view.source_publication,
        "thread activation: identical channel counters from another actor produce a distinct UI capture publication");
    popup_scene_release(&f.popup);
    ui_scene_release(&f.shell);
    f = {};
}
} // namespace maicord
