#include "create.hpp"
#include "../../settings/layer.hpp"
#include "../../settings/modal.hpp"
#include "../../guild/channel_overview.hpp"
#include "maicord/render/d3d11/layer_presentation.hpp"
#include "../../shell/state.hpp"
#include "../composer/text_input.hpp"
#include "../threads/actions.hpp"
#include "maicord/app/chat/polls/draft.hpp"
#include "maicord/model/guild/permission_bits.hpp"
#include "maicord/model/chat/bounded_edit.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "../../account/account_access.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {
constexpr u64 kPageOwner = 0x4D4149504F4C4C43ull;
constexpr UiPageKey kPage{kPageOwner, 1};
constexpr u32 kAnswers = kPollCreateMaximumAnswers;
// Editor 0 is the question; then answer texts, then answer emoji.
constexpr u32 kEditorCount = 1 + 2 * kAnswers;
constexpr u32 kControlCapacity = 8 + 2 * kAnswers;
constexpr u32 kMenuChoiceBase = 1'000;
constexpr u32 kEmojiCodepoints = 16;
enum : u64 {
    control_question = 1,
    control_answers,
    control_add,
    control_remove,
    control_options,
    control_duration,
    control_multiselect,
    control_answer = 100,
    control_emoji = 200,
};
constexpr Utf8View kDurationLabels[]{utf8_literal("1 hour"),   utf8_literal("4 hours"), utf8_literal("8 hours"),
                                     utf8_literal("24 hours"), utf8_literal("3 days"),  utf8_literal("1 week")};
static_assert(sizeof(kDurationLabels) / sizeof(kDurationLabels[0]) == kPollDurationCount);

struct Form : SettingsDialog {
    UiSettingsControl controls[kControlCapacity];
    u32 control_count;
    UiSettingsEditor editors[kEditorCount];
    UiSettingsFooter footer;
    MessageComposer models[kEditorCount];
    BoundedEdit bindings[kEditorCount];
    PollDraft draft;
    // Built poll borrowing draft text; the draft is read-only while queued or sent.
    PollCreate poll;
    PopupItem popup_items[kPollDurationCount];
    UiWidgetManager* previous_focus;
    u64 lifetime, channel_id, account_scope, profile_id, nonce, request_id, retry_at;
    u16 channel_generation;
    Utf8View status;
    bool queued, failed;
    // Two banks so labels borrowed by the previous projection stay valid
    // until the scene has taken the next one.
    char banks[2][1024];
    u32 bank_index;
};
Form* owner;
u64 lifetime_serial, nonce_sequence;

bool available() noexcept {
    return g_app.account_ui && !g_app.login.visible && g_app.account.status.running;
}
bool connected() noexcept {
    return g_app.account.instance && g_app.account.status.gateway_ready && g_app.account.api.poll_create;
}
bool scope_current() noexcept {
    if (!owner || !available() || owner->account_scope != g_app.plugin_account_scope_serial ||
        owner->profile_id != g_app.account.status.profile_id)
        return false;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, owner->channel_id);
    return channel && channel->generation == owner->channel_generation &&
           application_poll_create_available(owner->channel_id);
}
bool busy() noexcept {
    return owner->queued || owner->request_id;
}
bool editable() noexcept {
    return owner && owner->scene.active && !busy();
}

// Editors ---------------------------------------------------------------

Slice<char> editor_storage(u32 index) noexcept {
    auto& d = owner->draft;
    if (!index) return {d.question, kPollDraftQuestionBytes};
    if (index <= kAnswers) return {d.answers[index - 1], kPollDraftAnswerBytes};
    return {d.emoji[index - 1 - kAnswers], kPollDraftEmojiBytes};
}
u32 editor_bytes(u32 index) noexcept {
    auto& d = owner->draft;
    return !index ? d.question_bytes : index <= kAnswers ? d.answer_bytes[index - 1] : d.emoji_bytes[index - 1 - kAnswers];
}
bool binding_ready(void* model, u64 identity) noexcept {
    MessageComposer* focused = nullptr;
    return owner && owner->lifetime == identity && editable() &&
           settings_layer_editor(&owner->layer, nullptr, &focused, nullptr) && focused == model;
}
// Adopts the draft's bytes after the draft changed underneath the editor.
void load_editor(u32 index) noexcept {
    auto& model = owner->models[index];
    const u32 bytes = editor_bytes(index);
    message_composer_refresh_borrowed(&model, bytes);
    message_composer_set_selection(&model, bytes, bytes);
    const u32 limit = !index               ? kPollQuestionMaximumCodepoints
                      : index <= kAnswers ? kPollAnswerMaximumCodepoints
                                          : kEmojiCodepoints;
    bounded_edit_bind(&owner->bindings[index], &model, limit, false, owner->lifetime, binding_ready, &model);
}
void load_editors() noexcept {
    application_composer_text_input_suspend();
    for (u32 i = 0; i < kEditorCount; ++i)
        load_editor(i);
}
u32 editor_of(u64 control) noexcept {
    if (control == control_question) return 0;
    if (control >= control_answer && control < control_answer + owner->draft.answer_count)
        return 1 + static_cast<u32>(control - control_answer);
    if (control >= control_emoji && control < control_emoji + owner->draft.answer_count)
        return 1 + kAnswers + static_cast<u32>(control - control_emoji);
    return kEditorCount;
}

// Projection --------------------------------------------------------------

struct Bank {
    char* data;
    u32 capacity, used;
    Utf8View add(const Utf8View& text) noexcept {
        const u32 take = minimum(text.bytes, capacity - used);
        copy_bytes(data + used, text.data, take);
        used += take;
        return {data + used - take, take};
    }
    Utf8View numbered(Utf8View prefix, u32 number) noexcept {
        const u32 start = used;
        add(prefix);
        if (number >= 10 && used < capacity) data[used++] = static_cast<char>('0' + number / 10);
        if (used < capacity) data[used++] = static_cast<char>('0' + number % 10);
        return {data + start, used - start};
    }
};
void push(const UiSettingsControl& control) noexcept {
    if (owner->control_count < kControlCapacity)
        owner->controls[owner->control_count++] = control;
}
UiSettingsEditor* editor_view(u32 index, u64 control, Utf8View placeholder) noexcept {
    owner->editors[index] =
        settings_layer_editor_view(&owner->layer, control, owner->models[index], placeholder, editable());
    return &owner->editors[index];
}
Utf8View footer_status(PollDraftIssue issue) noexcept {
    if (owner->request_id || owner->queued) return utf8_literal("Posting poll…");
    if (owner->failed && owner->status.bytes) return owner->status;
    if (!connected()) return utf8_literal("Polls can be posted when MaiCord is connected.");
    return issue == PollDraftIssue::none ? utf8_literal("Ready to post.") : poll_draft_issue_text(issue);
}
bool project() noexcept {
    if (!owner || !owner->scene.active)
        return true;
    auto& p = *owner;
    auto& d = p.draft;
    const bool edit = editable();
    p.bank_index ^= 1;
    Bank bank{p.banks[p.bank_index], sizeof(p.banks[0]), 0};
    p.control_count = 0;
    push({control_question, utf8_literal("Question"), {}, UiSettingsControlKind::edit, edit, false, {}, 0, {},
          editor_view(0, control_question, utf8_literal("What do you want to ask?"))});
    push({control_answers, utf8_literal("Answers"), {}, UiSettingsControlKind::category, true, false, {}, 0, {},
          nullptr});
    for (u32 i = 0; i < d.answer_count; ++i) {
        push({control_answer + i, bank.numbered(utf8_literal("Answer "), i + 1), {}, UiSettingsControlKind::edit,
              edit, false, {}, 0, {}, editor_view(1 + i, control_answer + i, utf8_literal("Type an answer"))});
        push({control_emoji + i, utf8_literal("Emoji"), {}, UiSettingsControlKind::edit, edit, false, {}, 0, {},
              editor_view(1 + kAnswers + i, control_emoji + i, utf8_literal("Optional emoji"))});
    }
    if (d.answer_count < kAnswers)
        push({control_add, utf8_literal("Add answer"), {}, UiSettingsControlKind::action, edit, false, {}, 0,
              utf8_literal("Add"), nullptr});
    if (d.answer_count > kPollCreateMinimumAnswers)
        push({control_remove, utf8_literal("Remove last answer"), {}, UiSettingsControlKind::action, edit, false, {},
              0, utf8_literal("Remove"), nullptr});
    push({control_options, utf8_literal("Options"), {}, UiSettingsControlKind::category, true, false, {}, 0, {},
          nullptr});
    push({control_duration, utf8_literal("Duration"), {}, UiSettingsControlKind::choice, edit, false,
          {kDurationLabels, kPollDurationCount}, d.duration_index, {}, nullptr});
    push({control_multiselect, utf8_literal("Allow multiple answers"), {}, UiSettingsControlKind::toggle, edit,
          d.allow_multiselect, {}, 0, {}, nullptr});
    const auto issue = poll_draft_check(d);
    p.footer = {footer_status(issue),
                utf8_literal("Post poll"),
                utf8_literal("Clear"),
                true,
                issue == PollDraftIssue::none && connected() && !busy(),
                busy(),
                p.failed};
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, p.channel_id);
    const u32 start = bank.used;
    if (channel && channel->guild_id) {
        bank.add(utf8_literal("Posts in #"));
        bank.add(chat_state_store_channel_name(g_app.account_ui->chat_state, *channel));
        bank.add(utf8_literal("."));
    }
    p.content.description = {bank.data + start, bank.used - start};
    p.content.controls = {p.controls, p.control_count};
    p.content.footer = &p.footer;
    ++p.content.revision;
    return settings_modal_update(p);
}
bool repaint() noexcept {
    return settings_modal_present(project());
}

// Actions -----------------------------------------------------------------

void open_duration_menu(UiRect anchor) noexcept {
    auto& p = *owner;
    for (u32 i = 0; i < kPollDurationCount; ++i) {
        reset_value(&p.popup_items[i]);
        p.popup_items[i].label = kDurationLabels[i];
        p.popup_items[i].action = kMenuChoiceBase + i;
        p.popup_items[i].flags =
            static_cast<u16>(popup_item_enabled | (i == p.draft.duration_index ? popup_item_checked : 0));
    }
    settings_layer_open_choice(&p.layer, p.channel_id, {p.popup_items, kPollDurationCount}, anchor, 200);
}
void post() noexcept {
    auto& p = *owner;
    if (busy() || !connected() || !poll_draft_build(p.draft, &p.poll))
        return;
    const u64 now_ms = clock_unix_milliseconds();
    // A snowflake-shaped nonce, like the official client's; the server deduplicates by it.
    p.nonce = ((now_ms > kSnowflakeEpochMilliseconds ? now_ms - kSnowflakeEpochMilliseconds : now_ms) << 22) |
              (++nonce_sequence & 0x3FFFFFu);
    p.queued = true;
    p.failed = false;
    reset_value(&p.status);
    p.retry_at = 0;
    application_composer_text_input_suspend();
}
void activate(SettingsLayer*, const SettingsLayerActivation& activation) noexcept {
    if (!owner)
        return;
    auto& p = *owner;
    const auto target = activation.value.target;
    switch (target.kind) {
    case UiSettingsTargetKind::close:
        application_poll_create_close();
        return;
    case UiSettingsTargetKind::save:
        post();
        repaint();
        return;
    case UiSettingsTargetKind::reset:
        if (busy())
            return;
        poll_draft_reset(&p.draft);
        p.failed = false;
        reset_value(&p.status);
        load_editors();
        repaint();
        return;
    case UiSettingsTargetKind::control:
        break;
    default:
        return;
    }
    const u64 id = target.control_id;
    if (editor_of(id) != kEditorCount) {
        settings_layer_focus(&p.layer, target);
        return;
    }
    if (!editable())
        return;
    if (id == control_add && poll_draft_add_answer(&p.draft)) {
        const u32 added = p.draft.answer_count - 1;
        load_editor(1 + added);
        load_editor(1 + kAnswers + added);
    } else if (id == control_remove && poll_draft_remove_answer(&p.draft, p.draft.answer_count - 1)) {
        load_editors();
    } else if (id == control_duration) {
        open_duration_menu(activation.anchor);
        return;
    } else if (id == control_multiselect) {
        p.draft.allow_multiselect = activation.value.checked;
        ui_settings_scene_animate_toggle(&p.scene, target, activation.value.checked, clock_monotonic_milliseconds());
    } else {
        return;
    }
    p.failed = false;
    repaint();
}
void choose(SettingsLayer*, u32 action) noexcept {
    if (!owner || !editable() || action < kMenuChoiceBase || action - kMenuChoiceBase >= kPollDurationCount)
        return;
    owner->draft.duration_index = static_cast<u8>(action - kMenuChoiceBase);
    repaint();
}
bool edited(SettingsLayer*, u64 control) noexcept {
    if (!owner)
        return false;
    const u32 index = editor_of(control);
    if (index == kEditorCount)
        return false;
    // The models edit the draft's own storage; only the lengths follow.
    const u32 bytes = message_composer_text(owner->models[index]).bytes;
    auto& d = owner->draft;
    if (!index)
        d.question_bytes = static_cast<u16>(bytes);
    else if (index <= kAnswers)
        d.answer_bytes[index - 1] = static_cast<u16>(bytes);
    else
        d.emoji_bytes[index - 1 - kAnswers] = static_cast<u8>(bytes);
    owner->failed = false;
    return true;
}
bool editor_hook(SettingsLayer*, u64 control, MessageComposer** model, BoundedEdit** binding,
                 bool* multiline) noexcept {
    const u32 index = owner ? editor_of(control) : kEditorCount;
    if (index == kEditorCount)
        return false;
    *model = &owner->models[index];
    *binding = &owner->bindings[index];
    *multiline = false;
    return true;
}
constexpr SettingsLayerHooks kHooks{editor_hook,
                                    edited,
                                    activate,
                                    choose,
                                    settings_hook_close<application_poll_create_close>,
                                    settings_hook<repaint>,
                                    nullptr,
                                    settings_hook<editable>};
} // namespace

bool application_poll_create_available(u64 channel_id) noexcept {
    if (!g_app.account_ui || !application_thread_actions_channel_sendable(channel_id))
        return false;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id);
    return channel && (!channel->guild_id || permission_has(channel->effective_permissions, kPermissionSendPolls));
}

bool application_poll_create_active() noexcept {
    return owner && owner->scene.active;
}

bool application_poll_create_open(u64 channel_id) noexcept {
    if (!available() || !g_app.renderer_ready || g_app.text_form || application_channel_overview_active() ||
        (owner && owner->request_id) || !application_poll_create_available(channel_id))
        return false;
    settings_modal_yield();
    if (!owner) {
        owner = settings_modal_allocate<Form>(kHooks);
        if (!owner)
            return false;
        for (u32 i = 0; i < kEditorCount; ++i)
            message_composer_initialize(&owner->models[i], editor_storage(i));
        nonce_sequence = GetTickCount64();
    }
    auto& p = *owner;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id);
    p.lifetime = ++lifetime_serial;
    p.channel_id = channel_id;
    p.channel_generation = channel->generation;
    p.account_scope = g_app.plugin_account_scope_serial;
    p.profile_id = g_app.account.status.profile_id;
    p.previous_focus = g_app.context_focus;
    p.queued = p.failed = false;
    reset_value(&p.status);
    poll_draft_reset(&p.draft);
    if (!settings_dialog_begin(p, kPage, utf8_literal("Create poll")))
        return false;
    load_editors();
    if (settings_dialog_show(p, kPage, control_question, true, project) && repaint())
        return true;
    application_poll_create_close(false);
    return false;
}

void application_poll_create_close(bool restore_focus) noexcept {
    if (!owner || !owner->scene.active)
        return;
    settings_modal_suspend(*owner);
    // A queued poll is dropped; one in flight finishes and is discarded.
    owner->queued = false;
    settings_modal_leave(*owner, restore_focus && available());
}

void application_poll_create_release() noexcept {
    application_poll_create_close(false);
    if (!owner)
        return;
    if (owner->request_id && g_app.account.api.poll_create && g_app.account.instance)
        g_app.account.api.poll_create(g_app.account.instance, 0, 0, nullptr, owner->request_id);
    for (auto& model : owner->models)
        message_composer_release(&model);
    settings_modal_free(owner);
}

bool application_poll_create_synchronize() noexcept {
    if (!owner || !owner->scene.active)
        return true;
    auto& p = *owner;
    if (!scope_current()) {
        application_poll_create_close(false);
        return true;
    }
    if (p.layer.choice && !settings_layer_choice_current(p.layer)) {
        p.layer.choice = false;
        return repaint();
    }
    return settings_modal_refresh(p);
}

SettingsLayer* application_poll_create_settings_layer() noexcept {
    return owner ? &owner->layer : nullptr;
}

u64 application_poll_create_next_wake() noexcept {
    if (!owner || !owner->queued || owner->request_id)
        return 0;
    return owner->retry_at ? owner->retry_at : 1;
}

bool application_poll_create_submit(u64 now) noexcept {
    if (!owner || !owner->queued || owner->request_id ||
        g_app.details_request_owner != MessageHistoryRequestOwner::none || (owner->retry_at && owner->retry_at > now))
        return false;
    auto& p = *owner;
    if (!scope_current() || !connected()) {
        p.queued = false;
        p.failed = true;
        p.status = utf8_literal("This poll couldn't be sent.");
        repaint();
        return false;
    }
    const u64 id = application_next_request_id();
    const auto submitted = g_app.account.api.poll_create(g_app.account.instance, p.channel_id, p.nonce, &p.poll, id);
    if (submit_busy(submitted, p.retry_at, now, 50))
        return true;
    p.queued = false;
    if (!submitted.ok()) {
        p.failed = true;
        p.status = utf8_literal("This poll couldn't be sent.");
        repaint();
        return false;
    }
    p.request_id = id;
    g_app.details_request_owner = MessageHistoryRequestOwner::channel_overview;
    g_app.details_request_id = id;
    repaint();
    return true;
}

bool application_poll_create_owns_response(u64 request_id) noexcept {
    return owner && owner->request_id && owner->request_id == request_id;
}

bool application_poll_create_response(const RestClientResponse& response, Utf8View) noexcept {
    if (!application_poll_create_owns_response(response.request_id))
        return false;
    auto& p = *owner;
    p.request_id = 0;
    if (!p.scene.active)
        return true;
    if (response.succeeded()) {
        // The message itself arrives through the gateway like any other.
        application_poll_create_close();
        return true;
    }
    p.failed = true;
    p.status = response.error != RestClientError::none ? utf8_literal("Couldn't reach Discord. Try again.")
               : response.status_code == 403 ? utf8_literal("You can't post polls in this channel.")
               : response.status_code == 429 ? utf8_literal("Slow down. Try again in a moment.")
                                             : utf8_literal("Discord rejected this poll.");
    return repaint();
}

u64 application_poll_create_caret_wake() noexcept {
    return application_poll_create_active() ? settings_layer_next_wake(owner->layer) : 0;
}
} // namespace maicord::app_detail
