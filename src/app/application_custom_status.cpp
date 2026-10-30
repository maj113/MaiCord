#include "application_custom_status.hpp"
#include "application_account_presentation.hpp"
#include "application_attachments.hpp"
#include "application_channel_overview.hpp"
#include "application_composer_text_input.hpp"
#include "application_media_picker.hpp"
#include "application_message_interaction.hpp"
#include "application_profile.hpp"
#include "application_settings.hpp"
#include "application_settings_layer.hpp"
#include "application_state.hpp"
#include "maicord/render/layer_presentation.hpp"
#include "maicord/model/bounded_edit.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "account_ui_runtime.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_system_time.hpp"

namespace maicord::app_detail {
namespace {
struct Request {
    SelfCustomStatus desired;
    SelfCustomStatus projected;
    u64 account_scope, profile_id, self_user_id, request_id, retry_at, expired_created_at;
    u32 revision, submitted_revision;
    u8 attempts;
    bool queued, failed, projected_known;
} request;

GatewayClientSelfStatusSnapshot identity() noexcept {
    return g_app.account.instance && g_app.account.api.self_status_snapshot
               ? g_app.account.api.self_status_snapshot(g_app.account.instance)
               : GatewayClientSelfStatusSnapshot{};
}
bool saved(SelfCustomStatus* output) noexcept {
    return g_app.account.instance && g_app.account.api.custom_status_snapshot &&
           g_app.account.api.custom_status_snapshot(g_app.account.instance, output);
}
void scope() noexcept {
    if (request.account_scope == g_app.plugin_account_scope_serial)
        return;
    if (request.request_id)
        return; // The primary lane still owns this completion.
    reset_value(&request);
    request.account_scope = g_app.plugin_account_scope_serial;
}
u64 local_ms_of_day() noexcept {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return ((static_cast<u64>(now.wHour) * 60 + now.wMinute) * 60 + now.wSecond) * 1000 + now.wMilliseconds;
}
// The account's own rows (member list, profile card, account panel) read the
// global appearance; server presence for the same user replaces it.
void project_appearance(u64 unix_now) noexcept {
    if (!g_app.account_ui)
        return;
    SelfCustomStatus effective{};
    if (!application_custom_status_snapshot(&effective))
        return;
    if (!self_custom_status_active(effective, unix_now))
        reset_value(&effective);
    if (request.projected_known && self_custom_status_equal(request.projected, effective))
        return;
    auto& users = g_app.account_ui->user_store;
    const u64 self = g_app.account_ui->chat_state.self_user_id;
    if (!self)
        return;
    UserAppearance appearance{};
    appearance.user_id = self;
    appearance.fields = appearance_activity | appearance_custom_status;
    const u32 text = utf8_copy_truncated(appearance.activity.data, sizeof(appearance.activity.data),
                                         {effective.text, effective.text_bytes});
    appearance.activity.bytes = static_cast<u16>(text);
    const u32 emoji = minimum<u32>(effective.emoji_name_bytes, sizeof(appearance.status_emoji.data));
    copy_bytes(appearance.status_emoji.data, effective.emoji_name, emoji);
    appearance.status_emoji.bytes = static_cast<u16>(emoji);
    appearance.status_emoji_id = effective.emoji_id;
    if (!user_appearance_upsert(&users.appearance, appearance))
        return;
    saturating_increment(users.revision);
    copy_value(&request.projected, effective);
    request.projected_known = true;
    if (g_app.renderer_ready)
        refresh_account_text(true);
    application_request_frame();
}

constexpr u64 kPageOwner = 0x4d41494353544154ull;
constexpr UiPageKey kPage{kPageOwner, 1};
constexpr u32 kMenuChoiceBase = 1'000;
enum : u64 { control_emoji = 1, control_text, control_clear };
// The official order; each label maps to a clear-after choice.
constexpr Utf8View kClearLabels[]{utf8_literal("Today"), utf8_literal("4 hours"), utf8_literal("1 hour"),
                                  utf8_literal("30 minutes"), utf8_literal("Don't clear")};
constexpr SelfCustomStatusClear kClearChoices[]{SelfCustomStatusClear::today, SelfCustomStatusClear::hours_4,
                                                SelfCustomStatusClear::hour_1, SelfCustomStatusClear::minutes_30,
                                                SelfCustomStatusClear::never};
constexpr u32 kClearCount = sizeof(kClearLabels) / sizeof(kClearLabels[0]);
constexpr u32 kEmojiInputBytes = 96;

struct Form {
    UiSettingsScene scene;
    SettingsLayer layer;
    UiPageRegistry registry;
    UiPage page;
    UiSettingsPageContent content;
    UiSettingsControl controls[4];
    UiSettingsEditor editors[2];
    UiSettingsFooter footer;
    MessageComposer models[2];
    BoundedEdit bindings[2];
    PopupItem popup_items[kClearCount];
    char emoji[kEmojiInputBytes];
    char text[kSelfCustomStatusTextBytes];
    u64 lifetime, account_scope;
    u32 clear_index;
    Utf8View status;
    bool invalid, had_status;
};
Form* form;
u64 lifetime_serial;

bool binding_ready(void* model, u64 identity) noexcept {
    MessageComposer* focused = nullptr;
    return form && form->lifetime == identity && form->scene.active &&
           settings_layer_editor(&form->layer, nullptr, &focused, nullptr, nullptr) && focused == model;
}
void load_editor(u32 index, u32 bytes) noexcept {
    auto& model = form->models[index];
    message_composer_refresh_borrowed(&model, bytes);
    message_composer_set_selection(&model, bytes, bytes);
    bounded_edit_bind(&form->bindings[index], &model, index ? kSelfCustomStatusTextCodepoints : 16, false,
                      form->lifetime, binding_ready, &model);
}
u32 editor_of(u64 control) noexcept {
    return control == control_emoji ? 0 : control == control_text ? 1 : 2;
}
bool project() noexcept {
    if (!application_custom_status_active())
        return true;
    auto& f = *form;
    f.editors[0] = settings_layer_editor_view(&f.layer, control_emoji, f.models[0], utf8_literal("😀 or <:name:id>"), true);
    f.editors[1] = settings_layer_editor_view(&f.layer, control_text, f.models[1],
                                              utf8_literal("What's on your mind?"), true);
    f.controls[0] = {control_emoji, utf8_literal("Emoji"), {}, UiSettingsControlKind::edit, true, false, {}, 0, {},
                     &f.editors[0]};
    f.controls[1] = {control_text, utf8_literal("Status"), {}, UiSettingsControlKind::edit, true, false, {}, 0, {},
                     &f.editors[1]};
    f.controls[2] = {control_clear, utf8_literal("Clear after"), {}, UiSettingsControlKind::choice, true, false,
                     {kClearLabels, kClearCount}, f.clear_index, {}, nullptr};
    const bool connected = application_custom_status_available();
    f.footer = {f.invalid             ? utf8_literal("Use one emoji and up to 128 characters.")
                : !connected          ? utf8_literal("Your status can be set when MaiCord is connected.")
                : f.status.bytes      ? f.status
                                      : utf8_literal("Everyone who can see you sees your status."),
                utf8_literal("Save"),
                utf8_literal("Clear status"),
                true,
                connected,
                false,
                f.invalid};
    f.content.controls = {f.controls, 3};
    f.content.footer = &f.footer;
    ++f.content.revision;
    ui_settings_scene_set_focus_visible(&f.scene, g_app.keyboard_focus_visible);
    return ui_settings_scene_update(&f.scene, &f.registry, application_viewport_dips());
}
bool repaint() noexcept {
    if (!project())
        return false;
    application_request_frame();
    return !g_app.renderer_ready || renderer_rebuild_content_text(&g_app.renderer, application_theme());
}
void save() noexcept {
    auto& f = *form;
    SelfCustomStatus built{};
    if (!self_custom_status_build({f.emoji, static_cast<u32>(message_composer_text(f.models[0]).bytes)},
                                  {f.text, static_cast<u32>(message_composer_text(f.models[1]).bytes)},
                                  kClearChoices[f.clear_index], clock_unix_milliseconds(), local_ms_of_day(),
                                  &built)) {
        f.invalid = true;
        repaint();
        return;
    }
    if (!application_custom_status_request(&built)) {
        f.status = utf8_literal("Your status couldn't be saved. Try again.");
        repaint();
        return;
    }
    application_custom_status_close();
}
void activate(SettingsLayer*, const SettingsLayerActivation& activation) noexcept {
    if (!form)
        return;
    auto& f = *form;
    const auto target = activation.value.target;
    switch (target.kind) {
    case UiSettingsTargetKind::close:
        application_custom_status_close();
        return;
    case UiSettingsTargetKind::save:
        save();
        return;
    case UiSettingsTargetKind::reset:
        // A saved status is cleared; an unsaved one only empties the form.
        if (!f.had_status) {
            load_editor(0, 0);
            load_editor(1, 0);
            f.invalid = false;
            repaint();
        } else if (application_custom_status_request(nullptr))
            application_custom_status_close();
        return;
    case UiSettingsTargetKind::control:
        break;
    default:
        return;
    }
    if (editor_of(target.control_id) < 2) {
        settings_layer_focus(&f.layer, target);
        return;
    }
    if (target.control_id != control_clear)
        return;
    for (u32 i = 0; i < kClearCount; ++i) {
        reset_value(&f.popup_items[i]);
        f.popup_items[i].label = kClearLabels[i];
        f.popup_items[i].action = kMenuChoiceBase + i;
        f.popup_items[i].flags = static_cast<u16>(popup_item_enabled | (i == f.clear_index ? popup_item_checked : 0));
    }
    settings_layer_open_choice(&f.layer, kPageOwner, {f.popup_items, kClearCount}, activation.anchor, 200);
}
void choose(SettingsLayer*, u32 action) noexcept {
    if (!form || action < kMenuChoiceBase || action - kMenuChoiceBase >= kClearCount)
        return;
    form->clear_index = action - kMenuChoiceBase;
    repaint();
}
bool edited(SettingsLayer*, u64 control) noexcept {
    if (!form || editor_of(control) > 1)
        return false;
    form->invalid = false;
    return true;
}
bool editor_hook(SettingsLayer*, u64 control, MessageComposer** model, BoundedEdit** binding,
                 bool* multiline) noexcept {
    const u32 index = form ? editor_of(control) : 2;
    if (index > 1)
        return false;
    *model = &form->models[index];
    *binding = &form->bindings[index];
    *multiline = false;
    return true;
}
void close_hook(SettingsLayer*) noexcept {
    application_custom_status_close();
}
bool repaint_hook(SettingsLayer*) noexcept {
    return repaint();
}
bool editable_hook(SettingsLayer*) noexcept {
    return application_custom_status_active();
}
constexpr SettingsLayerHooks kHooks{editor_hook, edited, activate, choose, close_hook, repaint_hook, nullptr,
                                    editable_hook};
} // namespace

bool application_custom_status_snapshot(SelfCustomStatus* output) noexcept {
    if (!output)
        return false;
    scope();
    if (request.queued) {
        copy_value(output, request.desired);
        return true;
    }
    return saved(output);
}

bool application_custom_status_available() noexcept {
    SelfCustomStatus ignored{};
    const auto self = identity();
    return g_app.account.api.custom_status_submit && self.available && self.authoritative && saved(&ignored);
}

bool application_custom_status_request(const SelfCustomStatus* status) noexcept {
    scope();
    if (!application_custom_status_available())
        return false;
    if (status && (status->text_bytes || status->emoji_name_bytes || status->emoji_id))
        copy_value(&request.desired, *status);
    else
        reset_value(&request.desired);
    request.queued = true;
    request.failed = false;
    request.attempts = 0;
    if (++request.revision == 0)
        request.revision = 1;
    if (!request.request_id)
        request.retry_at = 1;
    project_appearance(clock_unix_milliseconds());
    application_request_frame();
    return true;
}

void application_custom_status_tick(u64) noexcept {
    scope();
    const u64 unix_now = clock_unix_milliseconds();
    SelfCustomStatus current{};
    // The client clears an expired status itself; do it once per status.
    if (!request.queued && !request.request_id && saved(&current) && current.present && current.expires_at_ms &&
        current.expires_at_ms <= unix_now && request.expired_created_at != current.created_at_ms + 1) {
        request.expired_created_at = current.created_at_ms + 1;
        application_custom_status_request(nullptr);
    }
    project_appearance(unix_now);
}

bool application_custom_status_pending(u64 now_ms) noexcept {
    scope();
    return request.queued && !request.request_id && request.retry_at && request.retry_at <= now_ms &&
           application_custom_status_available();
}

bool application_custom_status_submit(u64 request_id, u64 now_ms) noexcept {
    if (!request_id || !application_custom_status_pending(now_ms))
        return false;
    const auto self = identity();
    const bool clear = !request.desired.present;
    const auto submitted = g_app.account.api.custom_status_submit(g_app.account.instance, self.profile_id,
                                                                  self.self_user_id, clear ? nullptr : &request.desired,
                                                                  request_id);
    if (submitted.error == RestClientSubmitError::busy) {
        request.retry_at = deadline_after(now_ms, 50);
        return false;
    }
    if (!submitted.ok()) {
        log_u64(LogLevel::warning, "account", "custom_status_submit_error", static_cast<u64>(submitted.error));
        request.queued = false;
        request.failed = true;
        request.retry_at = 0;
        return false;
    }
    request.request_id = request_id;
    request.profile_id = self.profile_id;
    request.self_user_id = self.self_user_id;
    request.submitted_revision = request.revision;
    request.retry_at = 0;
    ++request.attempts;
    return true;
}

bool application_custom_status_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!request.request_id || response.request_id != request.request_id)
        return false;
    const bool consumed = g_app.account.instance && g_app.account.api.self_status_complete &&
                          g_app.account.api.self_status_complete(g_app.account.instance, request.profile_id,
                                                                 request.self_user_id, request.request_id, &response,
                                                                 body);
    request.request_id = 0;
    const bool ok = consumed && response.transport_ok() && response.status_code >= 200 && response.status_code < 300;
    if (!ok && response.transport_ok() && response.status_code == 429 && response.rate_limited &&
        response.retry_at_ms && !api_rejection_requires_action(response.rejection) && request.attempts < 3) {
        request.retry_at = response.retry_at_ms;
    } else if (!ok) {
        // An ambiguous settings write is never silently resent.
        log_u64(LogLevel::warning, "account", "custom_status_response_status", response.status_code);
        request.queued = false;
        request.failed = true;
    } else if (request.revision != request.submitted_revision) {
        request.retry_at = 1;
        request.attempts = 0;
    } else {
        request.queued = false;
    }
    project_appearance(clock_unix_milliseconds());
    application_request_frame();
    return true;
}

u64 application_custom_status_next_wake() noexcept {
    u64 wake = 0;
    if (request.queued && !request.request_id && request.retry_at &&
        g_app.message_history_request_owner == MessageHistoryRequestOwner::none)
        wake = request.retry_at;
    SelfCustomStatus current{};
    const u64 unix_now = clock_unix_milliseconds();
    // A past expiry is handled once by the tick; only a future one schedules.
    if (application_custom_status_snapshot(&current) && current.present && current.expires_at_ms > unix_now)
        wake = earliest_deadline(wake, clock_monotonic_milliseconds() + (current.expires_at_ms - unix_now));
    return wake;
}

void application_custom_status_clear() noexcept {
    if (request.request_id)
        return;
    reset_value(&request);
}

bool application_custom_status_open() noexcept {
    if (!g_app.account_ui || g_app.login.visible || !g_app.renderer_ready || g_app.text_form ||
        application_channel_overview_active() || !application_custom_status_available())
        return false;
    application_settings_close(false);
    application_media_picker_close();
    application_profile_close();
    if (!form) {
        form = static_cast<Form*>(VirtualAlloc(nullptr, sizeof(Form), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!form)
            return false;
        form->layer.scene = &form->scene;
        form->layer.hooks = &kHooks;
        message_composer_initialize(&form->models[0], {form->emoji, sizeof(form->emoji)});
        message_composer_initialize(&form->models[1], {form->text, sizeof(form->text)});
    }
    auto& f = *form;
    f.lifetime = ++lifetime_serial;
    f.account_scope = g_app.plugin_account_scope_serial;
    f.invalid = false;
    reset_value(&f.status);
    SelfCustomStatus current{};
    const bool known = application_custom_status_snapshot(&current) &&
                       self_custom_status_active(current, clock_unix_milliseconds());
    f.had_status = known;
    u32 emoji = 0;
    if (known && current.emoji_id) {
        char digits[20];
        const u32 count = decimal_write(digits, current.emoji_id);
        if (current.emoji_name_bytes + count + 3 <= sizeof(f.emoji)) {
            f.emoji[emoji++] = '<';
            f.emoji[emoji++] = ':';
            copy_bytes(f.emoji + emoji, current.emoji_name, current.emoji_name_bytes);
            emoji += current.emoji_name_bytes;
            f.emoji[emoji++] = ':';
            copy_bytes(f.emoji + emoji, digits, count);
            emoji += count;
            f.emoji[emoji++] = '>';
        }
    } else if (known) {
        copy_bytes(f.emoji, current.emoji_name, current.emoji_name_bytes);
        emoji = current.emoji_name_bytes;
    }
    const u32 text = known ? current.text_bytes : 0;
    if (text)
        copy_bytes(f.text, current.text, text);
    f.clear_index = known && !current.expires_at_ms ? kClearCount - 1 : 0;
    settings_layer_reset(&f.layer);
    if (!ui_page_registry_initialize(&f.registry, {&f.page, 1}))
        return false;
    f.content = {utf8_literal("Set a custom status"), {}, {}, 1, kUiSettingsPageFormat, &kUiDefaultSettingsStyle,
                 nullptr};
    if (!ui_page_registry_register(&f.registry, {kPage, f.content.title, &f.content}).ok())
        return false;
    application_composer_text_input_suspend();
    dismiss_popup();
    ui_scroll_host_cancel(&g_app.scroll_host);
    g_app.message_composer.focused = false;
    ui_settings_scene_set_presentation(&f.scene, UiSettingsPresentation::dialog);
    load_editor(0, emoji);
    load_editor(1, text);
    f.scene.focus = {UiSettingsTargetKind::control, kPage, control_text};
    if (!ui_settings_scene_update(&f.scene, &f.registry, application_viewport_dips()) || !project()) {
        application_custom_status_close(false);
        return false;
    }
    renderer_bind_settings_scene(&g_app.renderer, &f.scene);
    settings_layer_begin_entrance(&f.layer, clock_monotonic_milliseconds(), g_app.preferences.reduced_motion);
    g_app.context_focus = &f.scene.widgets;
    settings_layer_focus(&f.layer, {UiSettingsTargetKind::control, kPage, control_text});
    application_sync_attachment_drop_target();
    if (repaint())
        return true;
    application_custom_status_close(false);
    return false;
}

bool application_custom_status_active() noexcept {
    return form && form->scene.active;
}

void application_custom_status_close(bool restore_focus) noexcept {
    if (!application_custom_status_active())
        return;
    application_composer_text_input_suspend();
    settings_layer_close_choice(&form->layer);
    settings_layer_cancel_pointer(&form->layer);
    renderer_bind_settings_scene(&g_app.renderer, nullptr);
    form->layer.entrance_started = 0;
    renderer_set_settings_modal_progress(&g_app.renderer, 1);
    ui_settings_scene_close(&form->scene);
    if (restore_focus && g_app.account_ui && !g_app.login.visible)
        g_app.context_focus = &g_app.shell_scene.widgets;
    application_sync_attachment_drop_target();
    if (g_app.renderer_ready)
        refresh_account_text(true);
    application_request_frame();
}

void application_custom_status_release() noexcept {
    application_custom_status_close(false);
    if (!form)
        return;
    ui_settings_scene_release(&form->scene);
    for (auto& model : form->models)
        message_composer_release(&model);
    SecureZeroMemory(form, sizeof(*form));
    VirtualFree(form, 0, MEM_RELEASE);
    form = nullptr;
}

bool application_custom_status_synchronize() noexcept {
    if (!application_custom_status_active())
        return true;
    auto& f = *form;
    if (!g_app.account_ui || g_app.login.visible || f.account_scope != g_app.plugin_account_scope_serial) {
        application_custom_status_close(false);
        return true;
    }
    if (f.layer.choice && !settings_layer_choice_current(f.layer)) {
        f.layer.choice = false;
        return repaint();
    }
    if (!ui_settings_scene_update(&f.scene, &f.registry, application_viewport_dips()))
        return false;
    return g_app.renderer.interaction_text_settings_revision == f.scene.revision ||
           renderer_rebuild_content_text(&g_app.renderer, application_theme());
}

SettingsLayer* application_custom_status_settings_layer() noexcept {
    return form ? &form->layer : nullptr;
}
u64 application_custom_status_caret_wake() noexcept {
    return application_custom_status_active() ? settings_layer_next_wake(form->layer) : 0;
}
u32 application_custom_status_owner_bytes() noexcept {
    return form ? sizeof(*form) : 0;
}
} // namespace maicord::app_detail
