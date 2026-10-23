#include "application_report.hpp"
#include "application_account_presentation.hpp"
#include "application_attachments.hpp"
#include "application_channel_overview.hpp"
#include "application_composer_text_input.hpp"
#include "application_content_input.hpp"
#include "application_media_picker.hpp"
#include "application_message_history.hpp"
#include "application_message_interaction.hpp"
#include "application_profile.hpp"
#include "application_settings.hpp"
#include "application_settings_layer.hpp"
#include "application_state.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/render/layer_presentation.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {
constexpr u64 kPageOwner = 0x4D41495245505254ull; // "MAIREPRT"
constexpr UiPageKey kPage{kPageOwner, 1};
constexpr u32 kControlCapacity = 64;
constexpr u32 kChildChoices = 16;
constexpr u32 kAnswerCapacity = 48;
constexpr u32 kPreviewBytes = 280;
enum : u64 {
    control_header = 1,
    control_info,
    control_preview,
    control_children,
    control_confirm,
    control_status,
    control_text_base = 100,
    control_link_base = 600,
    control_option_base = 1000,
};

enum class Phase : u8 { loading, failed, node, confirm, submitting, done };
enum class Op : u8 { none, menu, submit };

struct Answer {
    u32 element;
    u64 selected;
};

struct Owner {
    UiSettingsScene scene;
    SettingsLayer layer;
    UiPageRegistry registry;
    UiPage page;
    UiSettingsPageContent content;
    UiSettingsControl controls[kControlCapacity];
    UiSettingsControlPresentation presentation[kControlCapacity];
    u32 control_count;
    UiSettingsFooter footer;
    Utf8View child_labels[kChildChoices];
    u32 child_targets[kChildChoices];
    u32 child_count;
    ApplicationReportTarget target;
    u64 lifetime, account_scope, profile_id;
    Phase phase;
    ReportMenu menu;
    u32 path[kReportMaximumBreadcrumbs];
    u32 path_count;
    Answer answers[kAnswerCapacity];
    u32 answer_count;
    Op op;
    bool queued, failed;
    u64 request_id, retry_at;
    Utf8View status;
    char title[64];
    u32 title_bytes;
    char preview[kPreviewBytes];
    u32 preview_bytes;
    u32 structurals[16384];
    u8 body[kUserSocialMaximumBodyBytes];
};
Owner* owner;
u64 lifetime_serial;

bool connected() noexcept {
    return g_app.account.instance && g_app.account.status.gateway_ready && g_app.account.api.user_social &&
           g_app.account.status.required_action == AccountRequirement::none;
}
bool scope_current() noexcept {
    return owner && application_account_available() && owner->account_scope == g_app.plugin_account_scope_serial &&
           owner->profile_id == g_app.account.status.profile_id;
}
bool busy() noexcept {
    return owner && (owner->queued || owner->request_id);
}
const ReportNode* current_node() noexcept {
    return owner && owner->path_count ? report_menu_node(owner->menu, owner->path[owner->path_count - 1]) : nullptr;
}
Answer* answer(u32 element, bool create) noexcept {
    for (u32 i = 0; i < owner->answer_count; ++i)
        if (owner->answers[i].element == element)
            return owner->answers + i;
    if (!create || owner->answer_count == kAnswerCapacity)
        return nullptr;
    owner->answers[owner->answer_count] = {element, 0};
    return owner->answers + owner->answer_count++;
}
bool selection_satisfied(const ReportNode& node) noexcept {
    if (!node.multi_select_required)
        return true;
    for (u32 e = 0; e < node.element_count; ++e) {
        const u32 index = node.element_first + e;
        if (owner->menu.elements[index].kind != ReportElementKind::checkbox)
            continue;
        if (const auto* a = answer(index, false); a && a->selected)
            return true;
    }
    return false;
}

void push(const UiSettingsControl& control,
          UiSettingsControlPresentation presentation = {UiSettingsVariant::standard, 0, kUiSettingsNoDefault}) noexcept {
    if (owner->control_count >= kControlCapacity)
        return;
    owner->presentation[owner->control_count] = presentation;
    owner->controls[owner->control_count++] = control;
}
UiSettingsControl notice(u64 id, Utf8View label, Utf8View description) noexcept {
    return {id, label, description, UiSettingsControlKind::notice, true, false, {}, 0, {}, nullptr};
}
// The reported message (author and an excerpt) or user, as the menu's
// preview elements show them.
void capture_preview() noexcept {
    auto& p = *owner;
    p.preview_bytes = 0;
    const auto& ui = *g_app.account_ui;
    const auto append = [&](Utf8View text) noexcept {
        const u32 room = kPreviewBytes - p.preview_bytes;
        p.preview_bytes += utf8_copy_truncated(p.preview + p.preview_bytes, room, text);
    };
    if (p.target.name == ReportMenuName::message) {
        const auto found = message_store_find(g_app.message_view.store, p.target.message_id);
        if (found.ok())
            append(message_store_content(g_app.message_view.store, found.handle));
        if (!p.preview_bytes)
            append(utf8_literal("Attachment or embed"));
    } else if (const auto* user = user_store_find(ui.user_store, p.target.user_id)) {
        append(utf8_literal("@"));
        append(user_store_username(ui.user_store, *user));
    }
}
Utf8View author_label() noexcept {
    const auto& ui = *g_app.account_ui;
    const auto* user = user_store_find(ui.user_store, owner->target.user_id);
    return user ? user_store_name(ui.user_store, *user) : utf8_literal("Unknown user");
}

void project_node(const ReportNode& node) noexcept {
    auto& p = *owner;
    const auto& menu = p.menu;
    push({control_header, node.header.bytes ? node.header : utf8_literal("Report"), node.subheader,
          UiSettingsControlKind::category, true, false, {}, 0, {}, nullptr});
    if (node.info.bytes)
        push(notice(control_info, {}, node.info));
    for (u32 e = 0; e < node.element_count; ++e) {
        const u32 index = node.element_first + e;
        const auto& element = menu.elements[index];
        switch (element.kind) {
        case ReportElementKind::message_preview:
        case ReportElementKind::user_preview:
            push(notice(control_preview, author_label(), {p.preview, p.preview_bytes}));
            break;
        case ReportElementKind::text:
            if (element.header.bytes || element.body.bytes)
                push(notice(control_text_base + e, element.header, element.body));
            break;
        case ReportElementKind::external_link:
            if (element.url.bytes)
                push({control_link_base + e, element.header.bytes ? element.header : utf8_literal("Learn more"),
                      element.body, UiSettingsControlKind::action, true, false, {}, 0, utf8_literal("Open"), nullptr});
            break;
        case ReportElementKind::checkbox: {
            const auto* a = answer(index, false);
            for (u32 o = 0; o < element.option_count && o < 64; ++o) {
                const auto& option = menu.options[element.option_first + o];
                push({control_option_base + (static_cast<u64>(index) << 6) + o,
                      option.label.bytes ? option.label : option.value, option.description,
                      UiSettingsControlKind::toggle, p.phase == Phase::node, a && (a->selected & (1ull << o)), {}, 0,
                      {}, nullptr});
            }
            break;
        }
        default:
            break;
        }
    }
    p.child_count = 0;
    for (u32 c = 0; c < node.child_count && p.child_count < kChildChoices; ++c) {
        const auto& child = menu.children[node.child_first + c];
        if (!child.label.bytes || !report_menu_node(menu, child.target))
            continue;
        p.child_labels[p.child_count] = child.label;
        p.child_targets[p.child_count++] = child.target;
    }
    if (p.child_count)
        push({control_children, {}, {}, UiSettingsControlKind::radio, p.phase == Phase::node, false,
              {p.child_labels, p.child_count}, kUiSettingsNoChoice, {}, nullptr});
}

Utf8View primary_label(const ReportNode* node) noexcept {
    switch (owner->phase) {
    case Phase::failed:
        return utf8_literal("Try again");
    case Phase::confirm:
    case Phase::submitting:
        return utf8_literal("Submit report");
    case Phase::done:
        return utf8_literal("Done");
    default:
        break;
    }
    if (!node)
        return utf8_literal("Next");
    switch (node->button) {
    case ReportButtonKind::submit:
        return utf8_literal("Submit report");
    case ReportButtonKind::done:
        return utf8_literal("Done");
    case ReportButtonKind::cancel:
        return utf8_literal("Close");
    default:
        return utf8_literal("Next");
    }
}

bool project() noexcept {
    if (!owner || !owner->scene.active)
        return true;
    auto& p = *owner;
    p.control_count = 0;
    const auto* node = current_node();
    bool can_primary = false;
    Utf8View status = p.status;
    switch (p.phase) {
    case Phase::loading:
        push(notice(control_status, utf8_literal("Loading"), utf8_literal("Getting the report form\xE2\x80\xA6")));
        break;
    case Phase::failed:
        push(notice(control_status, utf8_literal("Couldn't load the report form"),
                    utf8_literal("Check your connection and try again.")));
        can_primary = connected() && !busy();
        break;
    case Phase::confirm:
    case Phase::submitting:
        push({control_confirm, utf8_literal("Submit this report?"),
              utf8_literal("Discord's Trust & Safety team reviews reports. The person you report isn't told who "
                           "reported them."),
              UiSettingsControlKind::category, true, false, {}, 0, {}, nullptr});
        push(notice(control_preview, author_label(), {p.preview, p.preview_bytes}));
        can_primary = p.phase == Phase::confirm && connected();
        if (p.phase == Phase::submitting)
            status = utf8_literal("Submitting\xE2\x80\xA6");
        break;
    case Phase::node:
    case Phase::done:
        if (node) {
            project_node(*node);
            if (p.phase == Phase::done)
                can_primary = true;
            else if (node->button == ReportButtonKind::next)
                can_primary = report_menu_node(p.menu, node->button_target) && selection_satisfied(*node);
            else if (node->button == ReportButtonKind::submit)
                can_primary = selection_satisfied(*node) && connected();
            else
                can_primary = node->button == ReportButtonKind::done || node->button == ReportButtonKind::cancel;
        }
        break;
    }
    const bool back = (p.phase == Phase::node && p.path_count > 1) || p.phase == Phase::confirm;
    p.footer = {status, primary_label(node), back ? utf8_literal("Back") : utf8_literal("Cancel"), true, can_primary,
                p.phase == Phase::submitting || p.phase == Phase::loading, p.failed};
    if (p.phase == Phase::done)
        p.footer.reset_label = utf8_literal("Close");
    p.content.controls = {p.controls, p.control_count};
    p.content.presentation = p.presentation;
    p.content.footer = &p.footer;
    ++p.content.revision;
    ui_settings_scene_set_focus_visible(&p.scene, g_app.keyboard_focus_visible);
    return ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips());
}
bool repaint() noexcept {
    if (!project())
        return false;
    application_request_frame();
    return !g_app.renderer_ready || renderer_rebuild_content_text(&g_app.renderer, application_theme());
}

void queue(Op op) noexcept {
    owner->op = op;
    owner->queued = true;
    owner->failed = false;
    owner->status = {};
    owner->retry_at = 0;
    pump_message_history();
}
// Moving into a node: auto-submit nodes go straight to the confirmation.
void enter(u32 node_id) noexcept {
    auto& p = *owner;
    const auto* node = report_menu_node(p.menu, node_id);
    if (!node || p.path_count == kReportMaximumBreadcrumbs)
        return;
    p.path[p.path_count++] = node_id;
    p.status = {};
    p.failed = false;
    if (node->auto_submit)
        p.phase = Phase::confirm;
    p.scene.focus = {UiSettingsTargetKind::control, kPage, control_header};
}
void back() noexcept {
    auto& p = *owner;
    if (p.phase == Phase::confirm) {
        p.phase = Phase::node;
        const auto* node = current_node();
        // An auto-submit node has nothing to show; step back past it.
        if (node && node->auto_submit && p.path_count > 1)
            --p.path_count;
        return;
    }
    if (p.phase == Phase::node && p.path_count > 1) {
        --p.path_count;
        return;
    }
    application_report_close();
}
void primary() noexcept {
    auto& p = *owner;
    const auto* node = current_node();
    switch (p.phase) {
    case Phase::failed:
        p.phase = Phase::loading;
        queue(Op::menu);
        return;
    case Phase::confirm:
        if (connected())
            queue(Op::submit);
        if (p.queued)
            p.phase = Phase::submitting;
        return;
    case Phase::done:
        application_report_close();
        return;
    case Phase::node:
        if (!node)
            return;
        if (node->button == ReportButtonKind::next && selection_satisfied(*node))
            enter(node->button_target);
        else if (node->button == ReportButtonKind::submit && selection_satisfied(*node))
            p.phase = Phase::confirm;
        else if (node->button == ReportButtonKind::done || node->button == ReportButtonKind::cancel)
            application_report_close();
        return;
    default:
        return;
    }
}

void activate(SettingsLayer*, const SettingsLayerActivation& activation) noexcept {
    if (!owner)
        return;
    auto& p = *owner;
    const auto target = activation.value.target;
    switch (target.kind) {
    case UiSettingsTargetKind::close:
        application_report_close();
        return;
    case UiSettingsTargetKind::reset:
        if (p.phase == Phase::submitting)
            return;
        back();
        if (owner)
            repaint();
        return;
    case UiSettingsTargetKind::save:
        if (p.footer.can_save)
            primary();
        if (owner)
            repaint();
        return;
    case UiSettingsTargetKind::control:
        break;
    default:
        return;
    }
    const auto* node = current_node();
    if (p.phase != Phase::node || !node)
        return;
    const u64 id = target.control_id;
    if (id == control_children) {
        const u32 choice = activation.value.selected_choice;
        if (choice < p.child_count)
            enter(p.child_targets[choice]);
    } else if (id >= control_option_base) {
        const u32 element = static_cast<u32>((id - control_option_base) >> 6);
        const u32 option = static_cast<u32>((id - control_option_base) & 63);
        if (element < node->element_first || element >= node->element_first + node->element_count ||
            p.menu.elements[element].kind != ReportElementKind::checkbox ||
            option >= p.menu.elements[element].option_count)
            return;
        auto* a = answer(element, true);
        if (!a)
            return;
        if (activation.value.checked)
            a->selected |= 1ull << option;
        else
            a->selected &= ~(1ull << option);
        ui_settings_scene_animate_toggle(&p.scene, target, activation.value.checked, clock_monotonic_milliseconds());
    } else if (id >= control_link_base && id < control_option_base) {
        const u32 e = static_cast<u32>(id - control_link_base);
        if (e < node->element_count) {
            const auto url = p.menu.elements[node->element_first + e].url;
            if (url.bytes > 8 && bytes_equal(url.data, "https://", 8))
                application_open_content_url(url);
        }
        return;
    } else
        return;
    repaint();
}
void choose(SettingsLayer*, u32) noexcept {}
bool edited(SettingsLayer*, u64) noexcept {
    return false;
}
bool editor_hook(SettingsLayer*, u64, MessageComposer**, BoundedEdit**, bool*) noexcept {
    return false;
}
void close_hook(SettingsLayer*) noexcept {
    application_report_close();
}
bool repaint_hook(SettingsLayer*) noexcept {
    return repaint();
}
bool editable_hook(SettingsLayer*) noexcept {
    return false;
}
constexpr SettingsLayerHooks kHooks{editor_hook, edited, activate, choose, close_hook, repaint_hook, nullptr,
                                    editable_hook};

bool ensure_owner() noexcept {
    if (owner)
        return true;
    owner = static_cast<Owner*>(VirtualAlloc(nullptr, sizeof(Owner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!owner)
        return false;
    owner->layer.scene = &owner->scene;
    owner->layer.hooks = &kHooks;
    return true;
}
} // namespace

bool application_report_open(const ApplicationReportTarget& target) noexcept {
    const bool message = target.name == ReportMenuName::message;
    if (!application_account_available() || !g_app.renderer_ready || g_app.text_form || application_channel_overview_active() ||
        target.name >= ReportMenuName::count || (message ? !target.channel_id || !target.message_id : !target.user_id) ||
        !ensure_owner() || busy())
        return false;
    application_settings_close(false);
    application_media_picker_close();
    application_profile_close();
    auto& p = *owner;
    report_menu_release(&p.menu);
    p.lifetime = ++lifetime_serial;
    p.target = target;
    p.account_scope = g_app.plugin_account_scope_serial;
    p.profile_id = g_app.account.status.profile_id;
    p.phase = Phase::loading;
    p.path_count = 0;
    p.answer_count = 0;
    p.status = {};
    p.failed = false;
    capture_preview();
    const auto title = message ? utf8_literal("Report message") : utf8_literal("Report user");
    copy_bytes(p.title, title.data, title.bytes);
    p.title_bytes = title.bytes;
    settings_layer_reset(&p.layer);
    if (!ui_page_registry_initialize(&p.registry, {&p.page, 1}))
        return false;
    p.content = {{p.title, p.title_bytes}, {}, {}, 1, kUiSettingsPageFormat, &kUiDefaultSettingsStyle, nullptr};
    if (!ui_page_registry_register(&p.registry, {kPage, p.content.title, &p.content}).ok())
        return false;
    application_composer_text_input_suspend();
    dismiss_popup();
    ui_scroll_host_cancel(&g_app.scroll_host);
    g_app.message_composer.focused = false;
    ui_settings_scene_set_presentation(&p.scene, UiSettingsPresentation::dialog);
    p.scene.focus = {UiSettingsTargetKind::control, kPage, control_status};
    if (!ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips()) || !project()) {
        application_report_close(false);
        return false;
    }
    renderer_bind_settings_scene(&g_app.renderer, &p.scene);
    settings_layer_begin_entrance(&p.layer, clock_monotonic_milliseconds(), g_app.preferences.reduced_motion);
    g_app.context_focus = &p.scene.widgets;
    application_sync_attachment_drop_target();
    queue(Op::menu);
    if (repaint())
        return true;
    application_report_close(false);
    return false;
}

bool application_report_active() noexcept {
    return owner && owner->scene.active;
}

void application_report_close(bool restore_focus) noexcept {
    if (!owner || !owner->scene.active)
        return;
    application_composer_text_input_suspend();
    settings_layer_close_choice(&owner->layer);
    settings_layer_cancel_pointer(&owner->layer);
    // A queued request is dropped; one in flight finishes and is discarded.
    owner->queued = false;
    renderer_bind_settings_scene(&g_app.renderer, nullptr);
    owner->layer.entrance_started = 0;
    renderer_set_settings_modal_progress(&g_app.renderer, 1);
    ui_settings_scene_close(&owner->scene);
    if (restore_focus && application_account_available())
        g_app.context_focus = &g_app.shell_scene.widgets;
    application_sync_attachment_drop_target();
    if (g_app.renderer_ready)
        refresh_account_text(true);
    application_request_frame();
}

void application_report_release() noexcept {
    application_report_close(false);
    if (!owner)
        return;
    report_menu_release(&owner->menu);
    ui_settings_scene_release(&owner->scene);
    secure_clear_bytes(owner, sizeof(*owner));
    VirtualFree(owner, 0, MEM_RELEASE);
    owner = nullptr;
}

bool application_report_synchronize() noexcept {
    if (!owner || !owner->scene.active)
        return true;
    auto& p = *owner;
    if (!scope_current()) {
        application_report_close(false);
        return true;
    }
    if (!ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips()))
        return false;
    return g_app.renderer.interaction_text_settings_revision == p.scene.revision ||
           renderer_rebuild_content_text(&g_app.renderer, application_theme());
}

bool application_report_input(UiInputKind kind, u32 key, bool repeated) noexcept {
    return application_report_active() && settings_layer_input(&owner->layer, kind, key, repeated);
}

bool application_report_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    return application_report_active() && settings_layer_window_message(&owner->layer, message, wparam, lparam);
}

SettingsLayer* application_report_settings_layer() noexcept {
    return owner ? &owner->layer : nullptr;
}

u64 application_report_next_wake() noexcept {
    if (!owner || !owner->queued || owner->request_id ||
        g_app.details_request_owner != MessageHistoryRequestOwner::none)
        return 0;
    return owner->retry_at ? owner->retry_at : 1;
}

u64 application_report_caret_wake() noexcept {
    return application_report_active() ? settings_layer_next_wake(owner->layer) : 0;
}

void application_report_tick(u64 now) noexcept {
    if (application_report_active())
        settings_layer_tick(&owner->layer, now, g_app.preferences.reduced_motion);
}

bool application_report_submit(u64 now) noexcept {
    if (!owner || !owner->queued || owner->request_id || owner->retry_at > now ||
        g_app.details_request_owner != MessageHistoryRequestOwner::none || !connected() || !scope_current())
        return false;
    auto& p = *owner;
    UserSocialRequest request{};
    request.report = p.target.name;
    if (p.op == Op::menu)
        request.operation = UserSocialOperation::report_menu;
    else {
        // Only answers of nodes on the path are reported.
        ReportAnswer answers[kAnswerCapacity];
        u32 count = 0;
        for (u32 i = 0; i < p.answer_count; ++i) {
            bool on_path = false;
            for (u32 n = 0; n < p.path_count && !on_path; ++n) {
                const auto* node = report_menu_node(p.menu, p.path[n]);
                on_path = node && p.answers[i].element >= node->element_first &&
                          p.answers[i].element < node->element_first + node->element_count;
            }
            if (on_path && p.answers[i].selected)
                answers[count++] = {p.answers[i].element, p.answers[i].selected, {}};
        }
        const ReportSubmission submission{p.target.name,       p.target.channel_id, p.target.message_id,
                                          p.target.user_id,    p.target.guild_id,   {p.path, p.path_count},
                                          {answers, count}};
        const auto written = report_write_submit(p.menu, submission, {p.body, sizeof(p.body)});
        if (!written.ok()) {
            p.queued = false;
            p.phase = Phase::confirm;
            p.failed = true;
            p.status = utf8_literal("This report couldn't be prepared.");
            repaint();
            return false;
        }
        request.operation = UserSocialOperation::report_submit;
        request.text = {reinterpret_cast<const char*>(p.body), written.bytes};
    }
    const u64 id = application_next_request_id();
    const auto result = g_app.account.api.user_social(g_app.account.instance, &request, id);
    if (result.error == RestClientSubmitError::busy) {
        p.retry_at = now + 100;
        return false;
    }
    if (!result.ok()) {
        p.queued = false;
        p.failed = true;
        p.phase = p.op == Op::menu ? Phase::failed : Phase::confirm;
        p.status = utf8_literal("Couldn't reach Discord. Try again.");
        repaint();
        return false;
    }
    p.queued = false;
    p.request_id = id;
    g_app.details_request_owner = MessageHistoryRequestOwner::channel_overview;
    g_app.details_request_id = id;
    return true;
}

bool application_report_owns_response(u64 request_id) noexcept {
    return owner && request_id && owner->request_id == request_id;
}

bool application_report_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!application_report_owns_response(response.request_id))
        return true;
    auto& p = *owner;
    p.request_id = 0;
    const bool ok = response.error == RestClientError::none && response.status_code >= 200 &&
                    response.status_code < 300;
    if (!p.scene.active || !scope_current())
        return true;
    if (p.op == Op::menu) {
        if (ok && report_menu_decode(body, {p.structurals, 16384}, &p.menu)) {
            p.phase = Phase::node;
            p.path_count = 0;
            p.answer_count = 0;
            enter(p.menu.root_node_id);
        } else {
            log_u64(LogLevel::warning, "report", "menu_status", response.status_code);
            p.phase = Phase::failed;
        }
        return repaint();
    }
    if (ok) {
        p.phase = Phase::done;
        p.failed = false;
        p.status = {};
        if (report_menu_node(p.menu, p.menu.success_node_id) && p.path_count < kReportMaximumBreadcrumbs)
            p.path[p.path_count++] = p.menu.success_node_id;
        else
            p.status = utf8_literal("Thanks. Your report was sent.");
        return repaint();
    }
    log_u64(LogLevel::warning, "report", "submit_status", response.status_code);
    p.failed = true;
    if (response.error == RestClientError::none && response.status_code >= 400 && response.status_code < 500 &&
        response.status_code != 429 && report_menu_node(p.menu, p.menu.fail_node_id) &&
        p.path_count < kReportMaximumBreadcrumbs) {
        p.phase = Phase::done;
        p.path[p.path_count++] = p.menu.fail_node_id;
    } else {
        p.phase = Phase::confirm;
        p.status = response.status_code == 429 ? utf8_literal("Too many reports right now. Try again shortly.")
                                               : utf8_literal("Your report wasn't sent. Try again.");
    }
    return repaint();
}

u32 application_report_owner_bytes() noexcept {
    return owner ? sizeof(*owner) : 0;
}
} // namespace maicord::app_detail
