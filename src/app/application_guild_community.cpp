#include "application_guild_community.hpp"
#include "application_toast.hpp"
#include "application_guild_card.hpp"
#include "application_action_captcha.hpp"
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
#include "maicord/core/deadline.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/guild_settings_model.hpp"
#include "maicord/render/layer_presentation.hpp"
#include "maicord/model/bounded_edit.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/ui/navigation_view.hpp"
#include "maicord/ui/timestamp_format.hpp"
#include "application_account_access.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_system_time.hpp"

namespace maicord::app_detail {

namespace {
constexpr u64 kPageOwner = 0x4D4149434F4D4D55ull;
constexpr UiPageKey kPage{kPageOwner, 1};
constexpr u32 kControlCapacity = 96;
constexpr u32 kEditors = 4;
constexpr u32 kEditorBytes = 4000;
constexpr u32 kListedEvents = 24;
constexpr u32 kChannelChoices = 32;
constexpr u32 kDayChoices = 14;
constexpr u32 kTimeChoices = 48;
constexpr u32 kQueueCapacity = 8;
constexpr u64 kCopiedMs = 2'000;
constexpr u32 kMenuChoiceBase = 3'000;

enum class Mode : u8 { none, rules, onboarding, channels_roles, events, event, create };
enum class Phase : u8 { idle, loading, ready, submitting, failed, done };
enum class Menu : u8 { none, channel, day, time, duration };
enum : u64 {
    control_notice = 1,
    control_agree,
    control_where,
    control_channel,
    control_location,
    control_name,
    control_day,
    control_time,
    control_duration,
    control_description,
    control_create_event,
    control_interested,
    control_copy_link,
    control_when,
    control_place,
    control_about,
    control_category,
    control_text_base = 100,   // + field index
    control_choice_base = 200, // + field index
    control_rule_base = 300,   // + field * 32 + value
    control_option_base = 1000,
    control_event_base = 5000,
};

static constexpr Utf8View kWhereLabels[3]{utf8_literal("Stage channel"), utf8_literal("Voice channel"),
                                   utf8_literal("Somewhere else")};
constexpr u32 kDurationCount = 8;
constexpr u32 kDurationMinutes[kDurationCount]{0, 30, 60, 120, 180, 360, 720, 1440};
constexpr Utf8View kDurationLabels[kDurationCount]{
    utf8_literal("No end time"), utf8_literal("30 minutes"), utf8_literal("1 hour"),   utf8_literal("2 hours"),
    utf8_literal("3 hours"),     utf8_literal("6 hours"),    utf8_literal("12 hours"), utf8_literal("1 day")};

struct Request {
    u64 guild_id, event_id;
    GuildCommunityOperation op;
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
    UiSettingsEditor editors[kEditors];
    u64 editor_controls[kEditors];
    MessageComposer models[kEditors];
    BoundedEdit bindings[kEditors];
    char editor_text[kEditors][kEditorBytes];
    UiSettingsFooter footer;
    PopupItem popup_items[kTimeChoices];
    char title[160];
    Mode mode;
    Phase phase;
    Menu menu;
    bool from_list, failed;
    u64 lifetime, account_scope, profile_id;
    u64 guild_id, event_id;
    u16 guild_generation;
    Utf8View status;
    u64 copied_until;
    // Rules.
    GuildVerificationForm form;
    GuildVerificationAnswer answers[kGuildVerificationFieldMaximum];
    char form_body[kGuildCommunityMaximumBodyBytes];
    u32 form_body_bytes;
    // Onboarding.
    GuildOnboarding onboarding;
    u16 flow[kGuildOnboardingPromptMaximum];
    u32 flow_count, step;
    // Events.
    const ScheduledEvent* listed[kListedEvents];
    u32 listed_count;
    // Create.
    u32 where, day, time_slot, duration;
    u64 channel_choices[kChannelChoices];
    Utf8View channel_labels[kChannelChoices];
    u32 channel_count, channel_index;
    char day_labels[kDayChoices][32];
    char time_labels[kTimeChoices][24];
    Utf8View day_views[kDayChoices], time_views[kTimeChoices];
    u64 created_event_id;
    // Projection text.
    char banks[2][8192];
    u32 bank_index;
};
Owner* owner;
u64 lifetime_serial;

struct Background {
    u64 account_scope;
    Request queue[kQueueCapacity];
    u32 queue_count;
    Request flight;
    u64 request_id, retry_at;
    u64 modal_lifetime; // the modal a flight's response belongs to
    u64 wanted_events[8];
    u32 wanted_count;
    u64 missing_events[16]; // linked events the server could not return
    u32 missing_count;
    u64 interest_event;
    u64 gate_opened[16]; // servers whose rules opened on their own this session
    u32 gate_opened_count;
    u8 body[kGuildCommunityMaximumBodyBytes];
    u32 scratch[16384];
};
Background* background;
ScheduledEventStore* events;
u32 published_events_revision;

bool connected() noexcept {
    return g_app.account.instance && g_app.account.status.gateway_ready && g_app.account.api.guild_community;
}
ChatStateStore& state() noexcept {
    return g_app.account_ui->chat_state;
}
bool ensure_background() noexcept {
    if (!g_app.account_ui)
        return false;
    if (!background) {
        background = static_cast<Background*>(
            VirtualAlloc(nullptr, sizeof(Background), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!background)
            return false;
        background->account_scope = g_app.plugin_account_scope_serial;
    }
    if (background->account_scope != g_app.plugin_account_scope_serial) {
        const u64 request = background->request_id;
        SecureZeroMemory(background, sizeof(Background));
        background->account_scope = g_app.plugin_account_scope_serial;
        if (request && g_app.account.instance && g_app.account.api.guild_community)
            g_app.account.api.guild_community(g_app.account.instance, nullptr, request);
    }
    return true;
}
bool ensure_events() noexcept {
    if (events)
        return true;
    events = static_cast<ScheduledEventStore*>(
        VirtualAlloc(nullptr, sizeof(ScheduledEventStore), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    return events != nullptr;
}
void events_changed() noexcept {
    application_request_frame();
}
bool upsert_listed(void*, const ScheduledEvent& e) noexcept {
    scheduled_event_store_upsert(events, e);
    return true;
}
bool enqueue(GuildCommunityOperation op, u64 guild_id, u64 event_id = 0) noexcept {
    if (!ensure_background() || !guild_id)
        return false;
    auto& b = *background;
    for (u32 i = 0; i < b.queue_count; ++i)
        if (b.queue[i].op == op && b.queue[i].guild_id == guild_id && b.queue[i].event_id == event_id)
            return true;
    if (b.queue_count >= kQueueCapacity)
        return false;
    b.queue[b.queue_count++] = {guild_id, event_id, op};
    pump_message_history();
    return true;
}
const GuildRecord* guild(u64 id) noexcept {
    return g_app.account_ui ? chat_state_store_find_guild(state(), id) : nullptr;
}
bool self_member_known(const GuildRecord& g) noexcept {
    return (g.settings.known & guild_settings_self_member) != 0;
}
bool rules_gated(const GuildRecord& g) noexcept {
    return guild_rules_gate(g.upload_features, g.settings.self_member, self_member_known(g));
}
bool can_manage_events(u64 guild_id) noexcept {
    if (!g_app.account_ui)
        return false;
    const auto access = guild_settings_access(state(), guild_id);
    return access.resolved && (permission_has(access.permissions, kPermissionManageEvents) ||
                               permission_has(access.permissions, kPermissionCreateEvents));
}

using Bank = TextBank;

bool modal_submission(GuildCommunityOperation op) noexcept {
    return op == GuildCommunityOperation::verification_submit ||
           op == GuildCommunityOperation::onboarding_submit ||
           op == GuildCommunityOperation::onboarding_update || op == GuildCommunityOperation::event_create;
}
bool modal_request_current(const Request& request) noexcept {
    if (!owner || !owner->scene.active || owner->guild_id != request.guild_id ||
        (modal_submission(request.op) && owner->phase != Phase::submitting))
        return false;
    switch (request.op) {
    case GuildCommunityOperation::verification_get:
    case GuildCommunityOperation::verification_submit:
        return owner->mode == Mode::rules;
    case GuildCommunityOperation::onboarding_get:
        return owner->mode == Mode::onboarding || owner->mode == Mode::channels_roles;
    case GuildCommunityOperation::onboarding_submit:
        return owner->mode == Mode::onboarding;
    case GuildCommunityOperation::onboarding_update:
        return owner->mode == Mode::channels_roles;
    case GuildCommunityOperation::event_create:
        return owner->mode == Mode::create;
    default:
        return false;
    }
}
void discard_modal_submissions() noexcept {
    if (!background)
        return;
    u32 kept = 0;
    for (u32 i = 0; i < background->queue_count; ++i)
        if (!modal_submission(background->queue[i].op))
            background->queue[kept++] = background->queue[i];
    background->queue_count = kept;
}
void pop_request() noexcept {
    auto& b = *background;
    for (u32 i = 1; i < b.queue_count; ++i)
        b.queue[i - 1] = b.queue[i];
    --b.queue_count;
}

// These bounds describe what the modal can show, independently of the larger
// decoded models. Recompute before submission instead of trusting footer state.
bool rules_presentation_complete(const Owner& p) noexcept {
    if (p.form.truncated || p.form.field_count > kGuildVerificationFieldMaximum ||
        !guild_verification_supported(p.form))
        return false;
    u32 text_bytes = 0, controls = 0, editors = 0;
    bool agree = false;
    for (u32 f = 0; f < p.form.field_count; ++f) {
        const auto& field = p.form.fields[f];
        if (field.value_count > kGuildVerificationValueMaximum)
            return false;
        if (field.type == GuildVerificationFieldType::terms) {
            ++controls;
            if (field.value_count)
                ++controls;
            if (!agree)
                ++controls;
            agree = true;
            for (u32 v = 0; v < field.value_count; ++v)
                text_bytes += (v ? 1u : 0u) + decimal_digits(v + 1) + 2u + field.values[v].bytes;
        } else {
            ++controls;
            if (field.type == GuildVerificationFieldType::text_input ||
                field.type == GuildVerificationFieldType::paragraph)
                ++editors;
        }
    }
    return text_bytes <= sizeof(p.banks[0]) && controls <= kControlCapacity && editors <= kEditors;
}
u32 onboarding_option_bytes(const GuildOnboarding& model, const GuildOnboardingPrompt& prompt) noexcept {
    u32 bytes = 0;
    for (u32 o = 0; o < prompt.option_count; ++o) {
        const auto& option = model.options[prompt.first_option + o];
        bytes += option.title.bytes;
        if (option.emoji.bytes && !option.emoji_id)
            bytes += option.emoji.bytes + 1u;
    }
    return bytes;
}
bool onboarding_presentation_complete(const Owner& p) noexcept {
    const auto& model = p.onboarding;
    if (model.truncated || model.prompt_count > kGuildOnboardingPromptMaximum ||
        model.option_count > kGuildOnboardingOptionMaximum)
        return false;
    const u32 flow_count = guild_onboarding_flow_prompts(model, {});
    u32 text_bytes = 0, controls = 0, flow_step = 0;
    for (u32 i = 0; i < model.prompt_count; ++i) {
        const auto& prompt = model.prompts[i];
        if (prompt.first_option > model.option_count ||
            prompt.option_count > model.option_count - prompt.first_option)
            return false;
        if (!prompt.option_count || (p.mode == Mode::onboarding && !prompt.in_onboarding))
            continue;
        const u32 option_bytes = onboarding_option_bytes(model, prompt);
        if (p.mode == Mode::channels_roles) {
            text_bytes += option_bytes;
            controls += 1u + prompt.option_count;
        } else {
            // Reserve the largest footer used on this page, including the
            // submitting phase, so a queued write has the same admission rule.
            const u32 progress = utf8_literal("Question ").bytes + decimal_digits(++flow_step) +
                                 utf8_literal(" of ").bytes + decimal_digits(flow_count);
            const u32 status =
                maximum(p.status.bytes, maximum(progress, utf8_literal("Finishing\xE2\x80\xA6").bytes));
            const u32 instructions =
                (prompt.single_select ? utf8_literal("Pick one") : utf8_literal("Pick as many as you like"))
                    .bytes +
                (prompt.required ? utf8_literal(" (required).") : utf8_literal(".")).bytes;
            if (option_bytes + instructions + status > sizeof(p.banks[0]) ||
                1u + prompt.option_count > kControlCapacity)
                return false;
        }
    }
    return text_bytes <= sizeof(p.banks[0]) && controls <= kControlCapacity;
}
bool modal_presentation_complete(const Owner& p) noexcept {
    if (p.mode == Mode::rules)
        return rules_presentation_complete(p);
    if (p.mode == Mode::onboarding || p.mode == Mode::channels_roles)
        return onboarding_presentation_complete(p);
    return true;
}
bool onboarding_required_complete(const GuildOnboarding& model) noexcept {
    for (u32 i = 0; i < model.prompt_count; ++i)
        if (model.prompts[i].in_onboarding && model.prompts[i].required &&
            !guild_onboarding_prompt_answered(model, i))
            return false;
    return true;
}
constexpr Utf8View kIncompleteForm =
    utf8_literal("These rules or questions can't be shown completely here. Open Discord to continue.");

bool editable() noexcept {
    return owner && owner->scene.active && owner->phase != Phase::submitting && owner->phase != Phase::loading;
}
bool binding_ready(void* model, u64 identity) noexcept {
    MessageComposer* focused = nullptr;
    return owner && owner->lifetime == identity && editable() &&
           settings_layer_editor(&owner->layer, nullptr, &focused, nullptr, nullptr) && focused == model;
}
// Binds editor slot `slot` to `control` with initial text.
void bind_editor(u32 slot, u64 control, Utf8View text, u32 limit, bool multiline) noexcept {
    auto& p = *owner;
    const u32 bytes = minimum<u32>(text.bytes, kEditorBytes);
    copy_bytes(p.editor_text[slot], text.data, bytes);
    message_composer_refresh_borrowed(&p.models[slot], bytes);
    message_composer_set_selection(&p.models[slot], bytes, bytes);
    bounded_edit_bind(&p.bindings[slot], &p.models[slot], limit, multiline, p.lifetime, binding_ready,
                      &p.models[slot]);
    p.editor_controls[slot] = control;
}
int editor_slot(u64 control) noexcept {
    for (u32 i = 0; owner && i < kEditors; ++i)
        if (owner->editor_controls[i] == control)
            return static_cast<int>(i);
    return -1;
}
Utf8View editor_value(u64 control) noexcept {
    const int slot = editor_slot(control);
    return slot >= 0 ? message_composer_text(owner->models[slot]) : Utf8View{};
}
bool rules_answers(GuildVerificationAnswer* answers) noexcept {
    const auto& p = *owner;
    if (p.form.field_count > kGuildVerificationFieldMaximum)
        return false;
    for (u32 f = 0; f < p.form.field_count; ++f) {
        answers[f] = p.answers[f];
        const auto type = p.form.fields[f].type;
        if (type == GuildVerificationFieldType::text_input ||
            type == GuildVerificationFieldType::paragraph) {
            answers[f].text = editor_value(control_text_base + f);
            answers[f].answered = answers[f].text.bytes != 0;
        } else if (type == GuildVerificationFieldType::verification) {
            answers[f].agreed = true;
        }
    }
    return guild_verification_answers_complete(p.form, {answers, p.form.field_count});
}
void clear_editors() noexcept {
    for (u32 i = 0; i < kEditors; ++i) {
        owner->editor_controls[i] = 0;
        message_composer_refresh_borrowed(&owner->models[i], 0);
    }
}

// Appends a control and returns it for the optional fields; past capacity the row lands in a scratch sink.
UiSettingsControl& add_control(u64 id, const Utf8View& label, const Utf8View& description, UiSettingsControlKind kind,
                               bool enabled = true, bool checked = false,
                               UiSettingsVariant variant = UiSettingsVariant::standard) noexcept {
    static UiSettingsControl overflow;
    if (owner->control_count >= kControlCapacity)
        return overflow;
    const u32 index = owner->control_count++;
    owner->presentation[index] = {variant, 0, kUiSettingsNoDefault};
    owner->controls[index] = {id, label, description, kind, enabled, checked, {}, 0, {}, nullptr};
    return owner->controls[index];
}
void notice(u64 id, const Utf8View& label, const Utf8View& description, const Utf8View& action = {}) noexcept {
    add_control(id, label, description, UiSettingsControlKind::notice).action_label = action;
}
void set_footer(const Utf8View& status, const Utf8View& save, const Utf8View& reset, bool visible, bool can_save,
                bool saving, bool failed) noexcept {
    owner->footer = {status, save, reset, visible, can_save, saving, failed};
}
UiSettingsEditor* editor_view(u64 control, Utf8View placeholder, bool enabled) noexcept {
    const int slot = editor_slot(control);
    if (slot < 0)
        return nullptr;
    owner->editors[slot] =
        settings_layer_editor_view(&owner->layer, control, owner->models[slot], placeholder, enabled);
    return &owner->editors[slot];
}
Utf8View guild_name(u64 guild_id) noexcept {
    const auto* g = guild(guild_id);
    return g ? chat_state_store_guild_name(state(), *g) : Utf8View{};
}
Utf8View failure_text(const RestClientResponse& response, Utf8View fallback) noexcept {
    if (!response.transport_ok())
        return utf8_literal("Couldn't reach Discord. Try again.");
    switch (guild_community_classify(response.status_code)) {
    case GuildCommunityOutcome::rate_limited: return utf8_literal("Slow down. Try again in a moment.");
    case GuildCommunityOutcome::forbidden: return utf8_literal("You don't have permission to do this.");
    case GuildCommunityOutcome::not_found: return utf8_literal("This is no longer available.");
    default: return fallback;
    }
}

// Event time line: "Live now", or the start (and end) in local time.
Utf8View event_when(Bank& bank, const ScheduledEvent& e, bool full) noexcept {
    const u32 start = bank.used;
    const u64 now = clock_unix_milliseconds() / 1000;
    if (e.status == ScheduledEventStatus::active) {
        bank.add("Live now");
    } else {
        char text[96];
        const auto formatted = format_timestamp_local(e.start_ms / 1000, full ? 'F' : 'f', now, {text, sizeof(text)});
        if (formatted.ok)
            bank.add({text, formatted.bytes});
    }
    if (full && e.end_ms) {
        char text[96];
        const auto formatted = format_timestamp_local(e.end_ms / 1000, 'f', now, {text, sizeof(text)});
        if (formatted.ok) {
            bank.add(" \xE2\x80\x93 ");
            bank.add({text, formatted.bytes});
        }
    }
    return bank.since(start);
}
Utf8View event_place(Bank& bank, const ScheduledEvent& e) noexcept {
    const u32 start = bank.used;
    if (e.entity == ScheduledEventEntity::external) {
        bank.add({e.location, e.location_bytes});
    } else if (const auto* channel = chat_state_store_find_channel(state(), e.channel_id)) {
        bank.add(e.entity == ScheduledEventEntity::stage ? utf8_literal("Stage: ") : utf8_literal("Voice: "));
        bank.add(chat_state_store_channel_name(state(), *channel));
    }
    return bank.since(start);
}
Utf8View interested_text(Bank& bank, const ScheduledEvent& e) noexcept {
    const u32 start = bank.used;
    if (!e.user_count_known)
        return {};
    if (!e.user_count)
        return utf8_literal("No one's interested in this event yet.");
    bank.number(e.user_count);
    bank.add(" interested");
    return bank.since(start);
}

void project_rules(Bank& bank) noexcept {
    auto& p = *owner;
    const bool edit = editable();
    p.content.description = utf8_literal("Before you can talk here, read and agree to the rules.");
    if (p.phase == Phase::loading || p.phase == Phase::idle) {
        notice(control_notice, {}, utf8_literal("Loading the rules\xE2\x80\xA6"));
        set_footer({}, utf8_literal("Submit"), utf8_literal("Cancel"), true, false, true, false);
        return;
    }
    if (p.phase == Phase::failed && !p.form.field_count) {
        notice(control_notice, utf8_literal("Couldn't load the rules"), p.status, utf8_literal("Try again"));
        set_footer(p.status, utf8_literal("Submit"), utf8_literal("Cancel"), true, false, false, true);
        return;
    }
    if (!rules_presentation_complete(p)) {
        notice(control_notice, utf8_literal("This form can't be completed here"), kIncompleteForm);
        set_footer(kIncompleteForm, utf8_literal("Submit"), utf8_literal("Cancel"), true, false, false, true);
        return;
    }
    if (p.form.description.bytes)
        p.content.description = guild_verification_text(p.form, p.form.description);
    bool agree_shown = false; // one agreement covers every rules field
    for (u32 f = 0; f < p.form.field_count; ++f) {
        const auto& field = p.form.fields[f];
        const Utf8View label = guild_verification_text(p.form, field.label);
        const Utf8View description = guild_verification_text(p.form, field.description);
        switch (field.type) {
        case GuildVerificationFieldType::terms:
            add_control(control_category + f, label.bytes ? label : utf8_literal("Read and agree to the server rules"), {},
                        UiSettingsControlKind::category);
            // One numbered list in a single box, as the official client shows the rules.
            if (field.value_count) {
                const u32 start = bank.used;
                for (u32 v = 0; v < field.value_count; ++v) {
                    if (v)
                        bank.add("\n");
                    bank.number(v + 1);
                    bank.add(". ");
                    bank.add(guild_verification_text(p.form, field.values[v]));
                }
                notice(control_rule_base + f * kGuildVerificationValueMaximum, {}, bank.since(start));
            }
            if (!agree_shown)
                add_control(control_agree, utf8_literal("I have read and agree to the rules"), {},
                            UiSettingsControlKind::toggle, edit, p.answers[f].agreed);
            agree_shown = true;
            break;
        case GuildVerificationFieldType::text_input:
        case GuildVerificationFieldType::paragraph: {
            const bool paragraph = field.type == GuildVerificationFieldType::paragraph;
            const u64 id = control_text_base + f;
            const auto* view = editor_view(id, guild_verification_text(p.form, field.placeholder), edit);
            if (view)
                add_control(id, label, description,
                            paragraph ? UiSettingsControlKind::multiline : UiSettingsControlKind::edit, edit)
                    .editor = view;
            break;
        }
        case GuildVerificationFieldType::multiple_choice: {
            add_control(control_choice_base + f, label, description, UiSettingsControlKind::radio, edit)
                .selected_choice = p.answers[f].answered ? p.answers[f].choice : kUiSettingsNoChoice;
            break;
        }
        case GuildVerificationFieldType::verification:
            notice(control_category + f, label.bytes ? label : utf8_literal("Verified account"),
                   description.bytes ? description
                                     : utf8_literal("Your account meets this server's verification level."));
            break;
        default:
            notice(control_category + f, label.bytes ? label : utf8_literal("Unsupported question"),
                   utf8_literal("Answer this question in the Discord app or on the web."));
            break;
        }
    }
    GuildVerificationAnswer answers[kGuildVerificationFieldMaximum];
    const bool complete = rules_answers(answers);
    Utf8View status = p.status;
    if (!status.bytes && p.phase == Phase::submitting)
        status = utf8_literal("Submitting\xE2\x80\xA6");
    if (!status.bytes && !complete)
        status = utf8_literal("Agree to the rules to start talking.");
    set_footer(status, utf8_literal("Submit"), utf8_literal("Cancel"), true,
               complete && connected() && p.phase != Phase::submitting && p.phase != Phase::done,
               p.phase == Phase::submitting, p.failed);
}

// Multiple-choice labels live in the form bank; the radio borrows them.
Utf8View g_choice_views[kGuildVerificationFieldMaximum][kGuildVerificationValueMaximum];
void bind_choices() noexcept {
    auto& p = *owner;
    for (u32 i = 0; i < p.control_count; ++i) {
        auto& c = p.controls[i];
        if (c.id < control_choice_base || c.id >= control_choice_base + kGuildVerificationFieldMaximum)
            continue;
        const u32 f = static_cast<u32>(c.id - control_choice_base);
        const auto& field = p.form.fields[f];
        for (u32 v = 0; v < field.value_count; ++v)
            g_choice_views[f][v] = guild_verification_text(p.form, field.values[v]);
        c.choices = {g_choice_views[f], field.value_count};
    }
}

void option_row(Bank& bank, u32 option_index, bool enabled) noexcept {
    auto& p = *owner;
    const auto& option = p.onboarding.options[option_index];
    const u32 start = bank.used;
    if (option.emoji.bytes && !option.emoji_id) {
        bank.add(guild_onboarding_text(p.onboarding, option.emoji));
        bank.add(" ");
    }
    bank.add(guild_onboarding_text(p.onboarding, option.title));
    add_control(control_option_base + option_index, bank.since(start),
                guild_onboarding_text(p.onboarding, option.description), UiSettingsControlKind::toggle, enabled,
                option.selected);
}
void project_onboarding(Bank& bank) noexcept {
    auto& p = *owner;
    const bool edit = editable();
    if (p.phase == Phase::loading || p.phase == Phase::idle) {
        notice(control_notice, {}, utf8_literal("Loading\xE2\x80\xA6"));
        set_footer({}, utf8_literal("Next"), utf8_literal("Skip"), true, false, true, false);
        return;
    }
    if (p.phase == Phase::failed && !p.onboarding.prompt_count) {
        notice(control_notice, utf8_literal("Couldn't load this server's questions"), p.status,
               utf8_literal("Try again"));
        set_footer(p.status, utf8_literal("Next"), utf8_literal("Close"), true, false, false, true);
        return;
    }
    if (!onboarding_presentation_complete(p)) {
        notice(control_notice, utf8_literal("These questions can't be completed here"), kIncompleteForm);
        set_footer(kIncompleteForm, p.mode == Mode::channels_roles ? utf8_literal("Save") : utf8_literal("Finish"),
                   utf8_literal("Close"), true, false, false, true);
        return;
    }
    if (p.mode == Mode::channels_roles) {
        p.content.description =
            utf8_literal("Answer questions to get roles and see the channels you care about.");
        for (u32 i = 0; i < p.onboarding.prompt_count; ++i) {
            const auto& prompt = p.onboarding.prompts[i];
            if (!prompt.option_count)
                continue;
            add_control(control_category + i, guild_onboarding_text(p.onboarding, prompt.title),
                        prompt.single_select ? utf8_literal("Pick one.") : utf8_literal("Pick as many as you like."),
                        UiSettingsControlKind::category);
            for (u32 o = 0; o < prompt.option_count; ++o)
                option_row(bank, prompt.first_option + o, edit);
        }
        set_footer(p.status.bytes                     ? p.status
                   : p.phase == Phase::submitting ? utf8_literal("Saving\xE2\x80\xA6")
                                                  : Utf8View{},
                   utf8_literal("Save"), utf8_literal("Cancel"), true, connected() && p.phase != Phase::submitting,
                   p.phase == Phase::submitting, p.failed);
        return;
    }
    if (!p.flow_count) {
        notice(control_notice, utf8_literal("You're all set"), utf8_literal("This server has no questions for you."));
        set_footer({}, utf8_literal("Finish"), utf8_literal("Close"), true, connected(), false, false);
        return;
    }
    const u32 step = minimum(p.step, p.flow_count - 1);
    const u32 index = p.flow[step];
    const auto& prompt = p.onboarding.prompts[index];
    p.content.description = guild_onboarding_text(p.onboarding, prompt.title);
    const u32 start = bank.used;
    bank.add(prompt.single_select ? utf8_literal("Pick one") : utf8_literal("Pick as many as you like"));
    bank.add(prompt.required ? utf8_literal(" (required).") : utf8_literal("."));
    add_control(control_category + index, guild_onboarding_text(p.onboarding, prompt.title), bank.since(start),
                UiSettingsControlKind::category);
    for (u32 o = 0; o < prompt.option_count; ++o)
        option_row(bank, prompt.first_option + o, edit);
    const bool last = step + 1 >= p.flow_count;
    const bool answered = !prompt.required || guild_onboarding_prompt_answered(p.onboarding, index);
    const u32 status_start = bank.used;
    if (p.status.bytes)
        bank.add(p.status);
    else if (p.phase == Phase::submitting)
        bank.add("Finishing\xE2\x80\xA6");
    else {
        bank.add("Question ");
        bank.number(step + 1);
        bank.add(" of ");
        bank.number(p.flow_count);
    }
    set_footer(bank.since(status_start), last ? utf8_literal("Finish") : utf8_literal("Next"),
               step ? utf8_literal("Back") : utf8_literal("Skip"), true,
               answered && p.phase != Phase::submitting && (!last || connected()), p.phase == Phase::submitting,
               p.failed);
}

void project_events(Bank& bank) noexcept {
    auto& p = *owner;
    p.listed_count = events ? scheduled_event_store_guild_events(*events, p.guild_id, {p.listed, kListedEvents}) : 0;
    if (can_manage_events(p.guild_id))
        add_control(control_create_event, utf8_literal("Create event"),
                    utf8_literal("Schedule an event for any planned activity in your server."),
                    UiSettingsControlKind::action, connected(), false, UiSettingsVariant::primary)
            .action_label = utf8_literal("Create");
    if (!p.listed_count) {
        notice(control_notice, utf8_literal("There are no upcoming events."),
               p.phase == Phase::loading ? utf8_literal("Loading events\xE2\x80\xA6")
                                         : utf8_literal("Schedule an event for any planned activity in your server."));
    }
    for (u32 i = 0; i < p.listed_count; ++i) {
        const auto& e = *p.listed[i];
        // "When · where · N interested" on one description line.
        char place_text[192];
        Bank place_bank{place_text, sizeof(place_text), 0};
        const Utf8View where = event_place(place_bank, e);
        const u32 start = bank.used;
        event_when(bank, e, false);
        if (where.bytes) {
            bank.add(" \xC2\xB7 ");
            bank.add(where);
        }
        if (e.user_count_known) {
            bank.add(" \xC2\xB7 ");
            bank.number(e.user_count);
            bank.add(" interested");
        }
        add_control(control_event_base + i, {e.name, e.name_bytes}, bank.since(start), UiSettingsControlKind::action,
                    true, false, e.interested ? UiSettingsVariant::positive : UiSettingsVariant::standard)
            .action_label = e.interested ? utf8_literal("Interested") : utf8_literal("View");
    }
    set_footer(p.status, {}, utf8_literal("Close"), false, false, false, p.failed);
}

void project_event(Bank& bank) noexcept {
    auto& p = *owner;
    const auto* e = events ? scheduled_event_store_find(*events, p.event_id) : nullptr;
    if (!e) {
        notice(control_notice, p.phase == Phase::loading ? Utf8View{} : utf8_literal("Event unavailable"),
               p.phase == Phase::loading ? utf8_literal("Loading the event\xE2\x80\xA6")
                                         : utf8_literal("This event ended or was canceled."));
        set_footer({}, utf8_literal("Done"), p.from_list ? utf8_literal("Back") : utf8_literal("Close"), true, true,
                   false, false);
        return;
    }
    const u32 title_bytes = minimum<u32>(e->name_bytes, sizeof(p.title));
    copy_bytes(p.title, e->name, title_bytes);
    p.content.title = {p.title, title_bytes};
    p.content.description = e->status == ScheduledEventStatus::active ? utf8_literal("Happening now") : Utf8View{};
    notice(control_when, utf8_literal("When"), event_when(bank, *e, true));
    const Utf8View place = event_place(bank, *e);
    if (place.bytes)
        notice(control_place, utf8_literal("Where"), place);
    if (e->description_bytes)
        notice(control_about, utf8_literal("About"), {e->description, e->description_bytes});
    const Utf8View count = interested_text(bank, *e);
    add_control(control_interested, utf8_literal("Interested"),
                count.bytes ? count : utf8_literal("Get notified when this event starts."),
                UiSettingsControlKind::toggle, connected() && (!background || background->interest_event != e->id),
                e->interested);
    const bool copied = p.copied_until && clock_monotonic_milliseconds() < p.copied_until;
    add_control(control_copy_link, utf8_literal("Share"), utf8_literal("Copy a link to this event."),
                UiSettingsControlKind::action, true, false,
                copied ? UiSettingsVariant::positive : UiSettingsVariant::standard)
        .action_label = copied ? utf8_literal("Copied") : utf8_literal("Copy link");
    set_footer(p.status, utf8_literal("Done"), p.from_list ? utf8_literal("Back") : utf8_literal("Close"), true, true,
               false, p.failed);
}

u64 create_start_unix() noexcept;
// The create form as filled in so far.
ScheduledEventCreate event_draft() noexcept {
    const auto& p = *owner;
    ScheduledEventCreate create{};
    create.name = editor_value(control_name);
    create.description = editor_value(control_description);
    create.location = editor_value(control_location);
    create.entity = p.where == 0   ? ScheduledEventEntity::stage
                    : p.where == 1 ? ScheduledEventEntity::voice
                                   : ScheduledEventEntity::external;
    create.channel_id = p.where != 2 && p.channel_index < p.channel_count ? p.channel_choices[p.channel_index] : 0;
    create.start_unix_seconds = create_start_unix();
    const u32 minutes = kDurationMinutes[p.duration];
    create.end_unix_seconds = minutes ? create.start_unix_seconds + minutes * 60ull : 0;
    return create;
}
void project_create() noexcept {
    auto& p = *owner;
    const bool edit = editable();
    auto& where_row = add_control(control_where, utf8_literal("Where is your event?"), {}, UiSettingsControlKind::radio, edit);
    where_row.choices = {kWhereLabels, 3};
    where_row.selected_choice = p.where;
    const bool external = p.where == 2;
    if (external) {
        if (auto* view = editor_view(control_location, utf8_literal("Add a location, link or something."), edit))
            add_control(control_location, utf8_literal("Location"), {}, UiSettingsControlKind::edit, edit).editor = view;
    } else if (!p.channel_count) {
        // A choice row needs at least one option; without channels the row states why there is nothing to pick.
        notice(control_channel, utf8_literal("Channel"),
               p.where == 0 ? utf8_literal("This server has no stage channels you can use.")
                            : utf8_literal("This server has no voice channels you can use."));
    } else {
        auto& channel = add_control(control_channel, utf8_literal("Channel"), {}, UiSettingsControlKind::choice, edit);
        channel.choices = {p.channel_labels, p.channel_count};
        channel.selected_choice = p.channel_index;
    }
    if (auto* view = editor_view(control_name, utf8_literal("What's your event?"), edit))
        add_control(control_name, utf8_literal("Event topic"), {}, UiSettingsControlKind::edit, edit).editor = view;
    auto& day = add_control(control_day, utf8_literal("Start date"), {}, UiSettingsControlKind::choice, edit);
    day.choices = {p.day_views, kDayChoices};
    day.selected_choice = p.day;
    auto& time = add_control(control_time, utf8_literal("Start time"), {}, UiSettingsControlKind::choice, edit);
    time.choices = {p.time_views, kTimeChoices};
    time.selected_choice = p.time_slot;
    auto& duration = add_control(control_duration, utf8_literal("Duration"),
                                 external ? utf8_literal("Events somewhere else need an end time.") : Utf8View{},
                                 UiSettingsControlKind::choice, edit);
    duration.choices = {external ? kDurationLabels + 1 : kDurationLabels,
                        external ? kDurationCount - 1 : kDurationCount};
    duration.selected_choice = external ? (p.duration ? p.duration - 1 : 0) : p.duration;
    if (auto* view = editor_view(control_description, utf8_literal("Tell people a little more about your event."),
                                 edit))
        add_control(control_description, utf8_literal("Description"), {}, UiSettingsControlKind::multiline, edit)
            .editor = view;
    const auto error = scheduled_event_create_validate(event_draft(), clock_unix_milliseconds() / 1000);
    Utf8View hint{};
    switch (error) {
    case ScheduledEventCreateError::name: hint = utf8_literal("Give your event a topic."); break;
    case ScheduledEventCreateError::location: hint = utf8_literal("Add where your event takes place."); break;
    case ScheduledEventCreateError::channel: hint = utf8_literal("Pick a channel for your event."); break;
    case ScheduledEventCreateError::start: hint = utf8_literal("Pick a start time in the future."); break;
    case ScheduledEventCreateError::end: hint = utf8_literal("Pick an end time after the start."); break;
    case ScheduledEventCreateError::description: hint = utf8_literal("The description is too long."); break;
    default: break;
    }
    set_footer(p.status.bytes                     ? p.status
               : p.phase == Phase::submitting ? utf8_literal("Creating your event\xE2\x80\xA6")
                                              : hint,
               utf8_literal("Create event"), utf8_literal("Cancel"), true,
               error == ScheduledEventCreateError::none && connected() && p.phase != Phase::submitting,
               p.phase == Phase::submitting, p.failed);
}

bool project() noexcept {
    if (!owner || !owner->scene.active)
        return true;
    auto& p = *owner;
    p.bank_index ^= 1;
    Bank bank{p.banks[p.bank_index], sizeof(p.banks[0]), 0};
    p.control_count = 0;
    reset_value(&p.content.description);
    switch (p.mode) {
    case Mode::rules: project_rules(bank); break;
    case Mode::onboarding:
    case Mode::channels_roles: project_onboarding(bank); break;
    case Mode::events: project_events(bank); break;
    case Mode::event: project_event(bank); break;
    case Mode::create: project_create(); break;
    default: break;
    }
    if (p.mode == Mode::rules)
        bind_choices();
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

constexpr u64 kWindowsToUnixSeconds = 11'644'473'600ull;
u64 filetime_seconds(const FILETIME& time) noexcept {
    return ((static_cast<u64>(time.dwHighDateTime) << 32) | time.dwLowDateTime) / 10'000'000ull;
}
FILETIME seconds_filetime(u64 seconds) noexcept {
    const u64 ticks = seconds * 10'000'000ull;
    return {static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
}
// Local midnight of today plus `day` days, as a local (wall-clock) FILETIME second count.
u64 local_day_seconds(u32 day) noexcept {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    now.wHour = now.wMinute = now.wSecond = now.wMilliseconds = 0;
    FILETIME file{};
    SystemTimeToFileTime(&now, &file);
    return filetime_seconds(file) + day * 86'400ull;
}
u64 local_to_unix(u64 local_seconds) noexcept {
    const FILETIME local_file = seconds_filetime(local_seconds);
    SYSTEMTIME local{}, utc{};
    FILETIME utc_file{};
    if (!FileTimeToSystemTime(&local_file, &local) || !TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) ||
        !SystemTimeToFileTime(&utc, &utc_file))
        return 0;
    const u64 seconds = filetime_seconds(utc_file);
    return seconds > kWindowsToUnixSeconds ? seconds - kWindowsToUnixSeconds : 0;
}
u64 create_start_unix() noexcept {
    return local_to_unix(local_day_seconds(owner->day) + owner->time_slot * 1'800ull);
}
u32 wide_label(const wchar_t* wide, int units, char* out, u32 capacity) noexcept {
    const int written = units > 1 ? WideCharToMultiByte(CP_UTF8, 0, wide, units - 1, out, static_cast<int>(capacity),
                                                        nullptr, nullptr)
                                  : 0;
    return written > 0 ? static_cast<u32>(written) : 0;
}
void build_calendar() noexcept {
    auto& p = *owner;
    for (u32 d = 0; d < kDayChoices; ++d) {
        const FILETIME file = seconds_filetime(local_day_seconds(d));
        SYSTEMTIME day{};
        FileTimeToSystemTime(&file, &day);
        u32 bytes = 0;
        if (d < 2) {
            const Utf8View text = d ? utf8_literal("Tomorrow") : utf8_literal("Today");
            copy_bytes(p.day_labels[d], text.data, text.bytes);
            bytes = text.bytes;
        } else {
            wchar_t wide[32]{};
            bytes = wide_label(wide, GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &day, L"ddd, MMM d", wide, 32, nullptr),
                               p.day_labels[d], sizeof(p.day_labels[d]));
        }
        p.day_views[d] = {p.day_labels[d], bytes};
    }
    for (u32 t = 0; t < kTimeChoices; ++t) {
        SYSTEMTIME time{};
        time.wYear = 2026;
        time.wMonth = 1;
        time.wDay = 1;
        time.wHour = static_cast<WORD>(t / 2);
        time.wMinute = static_cast<WORD>(t % 2 * 30);
        wchar_t wide[24]{};
        const int units = GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &time, nullptr, wide, 24);
        p.time_views[t] = {p.time_labels[t], wide_label(wide, units, p.time_labels[t], sizeof(p.time_labels[t]))};
    }
    // The next half hour, an hour ahead, as the official client's default.
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const u32 slot = (now.wHour * 60u + now.wMinute + 89u) / 30u;
    p.day = slot >= kTimeChoices ? 1 : 0;
    p.time_slot = slot % kTimeChoices;
}
void build_channels() noexcept {
    auto& p = *owner;
    const u8 type = p.where == 0 ? 13 : 2;
    p.channel_count = 0;
    const auto& s = state();
    for (u32 order = 0; order < s.channel_count && p.channel_count < kChannelChoices; ++order) {
        const auto& channel = s.channels[s.channel_order[order]];
        if (channel.guild_id != p.guild_id || channel.type != type || !channel.permissions_resolved ||
            !(channel.effective_permissions & kPermissionViewChannel) || !chat_state_store_channel_visible(s, channel))
            continue;
        p.channel_choices[p.channel_count] = channel.id;
        p.channel_labels[p.channel_count] = chat_state_store_channel_name(s, channel);
        ++p.channel_count;
    }
    if (p.channel_index >= p.channel_count)
        p.channel_index = 0;
}

void activate(SettingsLayer*, const SettingsLayerActivation&) noexcept;
void choose(SettingsLayer*, u32 action) noexcept;
bool edited(SettingsLayer*, u64 control) noexcept {
    if (editor_slot(control) < 0)
        return false;
    owner->failed = false;
    reset_value(&owner->status);
    return true;
}
bool editor_hook(SettingsLayer*, u64 control, MessageComposer** model, BoundedEdit** binding,
                 bool* multiline) noexcept {
    const int slot = editor_slot(control);
    if (slot < 0)
        return false;
    *model = &owner->models[slot];
    *binding = &owner->bindings[slot];
    *multiline = control == control_description ||
                 (control >= control_text_base && control < control_text_base + kGuildVerificationFieldMaximum &&
                  owner->form.fields[control - control_text_base].type == GuildVerificationFieldType::paragraph);
    return true;
}
constexpr SettingsLayerHooks kHooks{editor_hook, edited, activate, choose,
                                    [](SettingsLayer*) noexcept { application_guild_community_close(); },
                                    [](SettingsLayer*) noexcept { return repaint(); }, nullptr,
                                    [](SettingsLayer*) noexcept { return editable(); }};

bool ensure_owner() noexcept {
    if (owner)
        return true;
    owner = static_cast<Owner*>(VirtualAlloc(nullptr, sizeof(Owner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!owner)
        return false;
    owner->layer.scene = &owner->scene;
    owner->layer.hooks = &kHooks;
    for (u32 i = 0; i < kEditors; ++i)
        message_composer_initialize(&owner->models[i], {owner->editor_text[i], kEditorBytes});
    return true;
}
bool can_open() noexcept {
    return application_account_available() && g_app.renderer_ready && !g_app.text_form &&
           (!application_channel_overview_active() || application_guild_community_active()) && ensure_owner();
}
bool openable(u64 guild_id) noexcept {
    return g_app.account.api.guild_community && guild(guild_id) && can_open();
}
void set_title(Utf8View title) noexcept {
    const u32 bytes = minimum<u32>(title.bytes, sizeof(owner->title));
    if (title.data != owner->title)
        copy_bytes(owner->title, title.data, bytes);
    owner->content.title = {owner->title, bytes};
}
// Switches the open modal to another page of this owner.
bool switch_mode(Mode mode, u64 focus) noexcept {
    auto& p = *owner;
    application_composer_text_input_suspend();
    settings_layer_close_choice(&p.layer);
    p.mode = mode;
    p.menu = Menu::none;
    p.failed = false;
    reset_value(&p.status);
    p.scene.focus = {UiSettingsTargetKind::control, kPage, focus};
    if (!repaint())
        return false;
    if (focus)
        settings_layer_focus(&p.layer, {UiSettingsTargetKind::control, kPage, focus});
    return true;
}
bool open_modal(Mode mode, u64 guild_id, Utf8View title, u64 focus) noexcept {
    if (application_guild_community_active()) {
        const auto* g = guild(guild_id);
        if (!g)
            return false;
        discard_modal_submissions();
        owner->lifetime = ++lifetime_serial;
        owner->guild_id = guild_id;
        owner->guild_generation = g->generation;
        set_title(title);
        return switch_mode(mode, focus);
    }
    application_settings_close(false);
    application_media_picker_close();
    application_profile_close();
    auto& p = *owner;
    const auto* g = guild(guild_id);
    if (!g)
        return false;
    p.lifetime = ++lifetime_serial;
    p.mode = mode;
    p.menu = Menu::none;
    p.account_scope = g_app.plugin_account_scope_serial;
    p.profile_id = g_app.account.status.profile_id;
    p.guild_id = guild_id;
    p.guild_generation = g->generation;
    p.failed = false;
    reset_value(&p.status);
    p.copied_until = 0;
    clear_editors();
    settings_layer_reset(&p.layer);
    if (!ui_page_registry_initialize(&p.registry, {&p.page, 1}))
        return false;
    set_title(title);
    p.content = {p.content.title, {}, {}, 1, kUiSettingsPageFormat, &kUiDefaultSettingsStyle, nullptr};
    if (!ui_page_registry_register(&p.registry, {kPage, p.content.title, &p.content}).ok())
        return false;
    application_composer_text_input_suspend();
    dismiss_popup();
    ui_scroll_host_cancel(&g_app.scroll_host);
    g_app.message_composer.focused = false;
    ui_settings_scene_set_presentation(&p.scene, UiSettingsPresentation::dialog);
    p.scene.focus = {UiSettingsTargetKind::control, kPage, focus};
    if (!ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips()) || !project()) {
        application_guild_community_close(false);
        return false;
    }
    renderer_bind_settings_scene(&g_app.renderer, &p.scene);
    settings_layer_begin_entrance(&p.layer, clock_monotonic_milliseconds(), g_app.preferences.reduced_motion);
    g_app.context_focus = &p.scene.widgets;
    if (focus)
        settings_layer_focus(&p.layer, {UiSettingsTargetKind::control, kPage, focus});
    application_sync_attachment_drop_target();
    if (repaint())
        return true;
    application_guild_community_close(false);
    return false;
}
bool scope_current() noexcept {
    if (!owner || !application_account_available() || owner->account_scope != g_app.plugin_account_scope_serial ||
        owner->profile_id != g_app.account.status.profile_id)
        return false;
    const auto* g = guild(owner->guild_id);
    return g && g->generation == owner->guild_generation && !g->unavailable;
}

void reload() noexcept {
    auto& p = *owner;
    p.phase = Phase::loading;
    p.failed = false;
    reset_value(&p.status);
    if (p.mode == Mode::rules)
        enqueue(GuildCommunityOperation::verification_get, p.guild_id);
    else if (p.mode == Mode::onboarding || p.mode == Mode::channels_roles)
        enqueue(GuildCommunityOperation::onboarding_get, p.guild_id);
}
bool submit_request(GuildCommunityOperation op) noexcept {
    auto& p = *owner;
    const Phase previous = p.phase;
    p.phase = Phase::submitting;
    if (enqueue(op, p.guild_id))
        return true;
    p.phase = previous;
    p.status = utf8_literal("This couldn't be queued. Try again.");
    p.failed = true;
    return false;
}
void submit_modal() noexcept {
    auto& p = *owner;
    if (!p.footer.can_save || p.phase == Phase::submitting)
        return;
    if (!p.scene.active || !modal_presentation_complete(p) ||
        ((p.mode == Mode::rules || p.mode == Mode::onboarding || p.mode == Mode::channels_roles) &&
         p.phase != Phase::ready && p.phase != Phase::failed))
        return;
    switch (p.mode) {
    case Mode::rules: {
        GuildVerificationAnswer answers[kGuildVerificationFieldMaximum];
        if (!rules_answers(answers))
            return;
        for (u32 f = 0; f < p.form.field_count; ++f) {
            const auto type = p.form.fields[f].type;
            if (type == GuildVerificationFieldType::verification)
                p.answers[f].agreed = true;
        }
        if (!submit_request(GuildCommunityOperation::verification_submit))
            return;
        break;
    }
    case Mode::onboarding:
        if (p.flow_count && (p.step >= p.flow_count || p.flow[p.step] >= p.onboarding.prompt_count ||
                             (p.onboarding.prompts[p.flow[p.step]].required &&
                              !guild_onboarding_prompt_answered(p.onboarding, p.flow[p.step]))))
            return;
        if (p.flow_count && p.step + 1 < p.flow_count) {
            ++p.step;
            p.scene.focus = {UiSettingsTargetKind::control, kPage, 0};
            break;
        }
        if (!onboarding_required_complete(p.onboarding))
            return;
        if (!submit_request(GuildCommunityOperation::onboarding_submit))
            return;
        break;
    case Mode::channels_roles:
        if (!submit_request(GuildCommunityOperation::onboarding_update))
            return;
        break;
    case Mode::create:
        if (!submit_request(GuildCommunityOperation::event_create))
            return;
        break;
    case Mode::event:
    case Mode::events:
        application_guild_community_close();
        return;
    default:
        break;
    }
    reset_value(&p.status);
    p.failed = false;
}
void reset_modal() noexcept {
    auto& p = *owner;
    if (p.mode == Mode::onboarding && p.step && p.phase != Phase::submitting) {
        --p.step;
        repaint();
        return;
    }
    if (p.mode == Mode::event && p.from_list) {
        p.from_list = false;
        set_title(utf8_literal("Events"));
        switch_mode(Mode::events, 0);
        return;
    }
    application_guild_community_close();
}
void toggle_interest(const ScheduledEvent& e, bool interested) noexcept {
    if (!ensure_background() || background->interest_event)
        return;
    background->interest_event = e.id;
    // Optimistic, like the official client; a refusal reverts it.
    scheduled_event_store_set_interest(events, e.id, interested, true);
    enqueue(interested ? GuildCommunityOperation::event_interested : GuildCommunityOperation::event_uninterested,
            e.guild_id, e.id);
}
void copy_event_link() noexcept {
    char link[96];
    const u32 bytes = scheduled_event_write_link(owner->guild_id, owner->event_id, {link, sizeof(link)});
    if (application_copy_text({link, bytes}, kToastLinkCopied, kToastLinkCopyFailed))
        owner->copied_until = deadline_after(clock_monotonic_milliseconds(), kCopiedMs);
}
void open_menu(Menu menu, UiRect anchor) noexcept {
    auto& p = *owner;
    u32 count = 0, selected = 0;
    const Utf8View* labels = nullptr;
    switch (menu) {
    case Menu::channel: labels = p.channel_labels, count = p.channel_count, selected = p.channel_index; break;
    case Menu::day: labels = p.day_views, count = kDayChoices, selected = p.day; break;
    case Menu::time: labels = p.time_views, count = kTimeChoices, selected = p.time_slot; break;
    case Menu::duration:
        labels = p.where == 2 ? kDurationLabels + 1 : kDurationLabels;
        count = p.where == 2 ? kDurationCount - 1 : kDurationCount;
        selected = p.where == 2 ? (p.duration ? p.duration - 1 : 0) : p.duration;
        break;
    default: return;
    }
    if (!count)
        return;
    count = minimum(count, kTimeChoices);
    for (u32 i = 0; i < count; ++i) {
        reset_value(&p.popup_items[i]);
        p.popup_items[i].label = labels[i];
        p.popup_items[i].action = kMenuChoiceBase + i;
        p.popup_items[i].flags = static_cast<u16>(popup_item_enabled | (i == selected ? popup_item_checked : 0));
    }
    p.menu = menu;
    settings_layer_open_choice(&p.layer, p.guild_id, {p.popup_items, count}, anchor, 220);
}
void activate(SettingsLayer*, const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    const auto target = activation.value.target;
    switch (target.kind) {
    case UiSettingsTargetKind::close: application_guild_community_close(); return;
    case UiSettingsTargetKind::reset: reset_modal(); return;
    case UiSettingsTargetKind::save:
        submit_modal();
        if (owner)
            repaint();
        return;
    case UiSettingsTargetKind::control: break;
    default: return;
    }
    const u64 id = target.control_id;
    if (editor_slot(id) >= 0) {
        settings_layer_focus(&p.layer, target);
        return;
    }
    if (id == control_notice) {
        if (p.phase == Phase::failed) {
            reload();
            repaint();
        }
        return;
    }
    if (id == control_copy_link) {
        copy_event_link();
        repaint();
        return;
    }
    if (id == control_create_event) {
        application_guild_community_open_create(p.guild_id);
        return;
    }
    if (id >= control_event_base && id < control_event_base + kListedEvents) {
        const u32 index = static_cast<u32>(id - control_event_base);
        if (index < p.listed_count) {
            p.event_id = p.listed[index]->id;
            p.from_list = true;
            p.phase = Phase::ready;
            switch_mode(Mode::event, control_interested);
        }
        return;
    }
    if (!editable() || !modal_presentation_complete(p))
        return;
    if (id == control_interested) {
        const auto* e = events ? scheduled_event_store_find(*events, p.event_id) : nullptr;
        if (e) {
            toggle_interest(*e, activation.value.checked);
            ui_settings_scene_animate_toggle(&p.scene, target, activation.value.checked,
                                             clock_monotonic_milliseconds());
        }
    } else if (id == control_agree) {
        for (u32 f = 0; f < p.form.field_count; ++f)
            if (p.form.fields[f].type == GuildVerificationFieldType::terms)
                p.answers[f].agreed = activation.value.checked;
        ui_settings_scene_animate_toggle(&p.scene, target, activation.value.checked, clock_monotonic_milliseconds());
    } else if (id >= control_choice_base && id < control_choice_base + kGuildVerificationFieldMaximum) {
        const u32 f = static_cast<u32>(id - control_choice_base);
        if (activation.value.selected_choice < p.form.fields[f].value_count) {
            p.answers[f].choice = activation.value.selected_choice;
            p.answers[f].answered = true;
        }
    } else if (id >= control_option_base && id < control_option_base + kGuildOnboardingOptionMaximum) {
        const u32 option = static_cast<u32>(id - control_option_base);
        for (u32 i = 0; i < p.onboarding.prompt_count; ++i) {
            const auto& prompt = p.onboarding.prompts[i];
            if (option >= prompt.first_option && option < static_cast<u32>(prompt.first_option) + prompt.option_count) {
                guild_onboarding_toggle(&p.onboarding, i, option - prompt.first_option);
                ui_settings_scene_animate_toggle(&p.scene, target, p.onboarding.options[option].selected,
                                                 clock_monotonic_milliseconds());
                break;
            }
        }
    } else if (id == control_where) {
        if (activation.value.selected_choice < 3 && activation.value.selected_choice != p.where) {
            p.where = activation.value.selected_choice;
            if (p.where == 2 && !p.duration)
                p.duration = 2; // an hour
            build_channels();
            // The location editor takes the slot only while it is shown.
            if (p.where == 2 && editor_slot(control_location) < 0)
                bind_editor(2, control_location, {}, kScheduledEventLocationMaximum, false);
        }
    } else if (id == control_channel) {
        open_menu(Menu::channel, activation.anchor);
        return;
    } else if (id == control_day) {
        open_menu(Menu::day, activation.anchor);
        return;
    } else if (id == control_time) {
        open_menu(Menu::time, activation.anchor);
        return;
    } else if (id == control_duration) {
        open_menu(Menu::duration, activation.anchor);
        return;
    } else {
        return;
    }
    p.failed = false;
    reset_value(&p.status);
    repaint();
}
void choose(SettingsLayer*, u32 action) noexcept {
    auto& p = *owner;
    const Menu menu = p.menu;
    p.menu = Menu::none;
    if (!editable() || action < kMenuChoiceBase)
        return;
    const u32 index = action - kMenuChoiceBase;
    switch (menu) {
    case Menu::channel:
        if (index < p.channel_count)
            p.channel_index = index;
        break;
    case Menu::day:
        if (index < kDayChoices)
            p.day = index;
        break;
    case Menu::time:
        if (index < kTimeChoices)
            p.time_slot = index;
        break;
    case Menu::duration:
        if (p.where == 2 && index + 1 < kDurationCount)
            p.duration = index + 1;
        else if (p.where != 2 && index < kDurationCount)
            p.duration = index;
        break;
    default: return;
    }
    repaint();
}

// Body for a modal submit, built from the modal's current state.
u32 build_body(const Request& request) noexcept {
    auto& b = *background;
    if (!owner || owner->guild_id != request.guild_id)
        return 0;
    auto& p = *owner;
    if (modal_submission(request.op) && !modal_request_current(request))
        return 0;
    switch (request.op) {
    case GuildCommunityOperation::verification_submit: {
        if (!rules_presentation_complete(p))
            return 0;
        GuildVerificationAnswer answers[kGuildVerificationFieldMaximum];
        if (!rules_answers(answers))
            return 0;
        return guild_verification_write_submit({p.form_body, p.form_body_bytes},
                                               {answers, p.form.field_count}, {b.scratch, 16384},
                                               {b.body, sizeof(b.body)});
    }
    case GuildCommunityOperation::onboarding_submit:
    case GuildCommunityOperation::onboarding_update:
        if (!onboarding_presentation_complete(p))
            return 0;
        if (request.op == GuildCommunityOperation::onboarding_submit &&
            !onboarding_required_complete(p.onboarding))
            return 0;
        return guild_onboarding_write_responses(p.onboarding, clock_unix_milliseconds(),
                                                {b.body, sizeof(b.body)},
                                                request.op == GuildCommunityOperation::onboarding_update);
    case GuildCommunityOperation::event_create:
        return scheduled_event_write_create(event_draft(), {b.body, sizeof(b.body)});
    default:
        return 0;
    }
}
void request_failed(const Request& request, Utf8View text) noexcept {
    if (request.op == GuildCommunityOperation::event_interested ||
        request.op == GuildCommunityOperation::event_uninterested) {
        if (events)
            scheduled_event_store_set_interest(events, request.event_id,
                                               request.op != GuildCommunityOperation::event_interested, true);
        background->interest_event = 0;
    }
    if (owner && owner->scene.active && owner->guild_id == request.guild_id) {
        if (owner->phase == Phase::submitting || owner->phase == Phase::loading)
            owner->phase = Phase::failed;
        owner->failed = true;
        owner->status = text;
        repaint();
    }
    events_changed();
}
} // namespace

void application_guild_community_gateway(const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    if (event.kind == GatewayClientEventKind::ready) {
        if (events)
            scheduled_event_store_reset(events);
        return;
    }
    if (event.kind != GatewayClientEventKind::dispatch)
        return;
    const auto kind = event.dispatch_kind;
    if (kind == GatewayClientDispatchKind::guild_delete) {
        if (events && scheduled_event_store_clear_guild(events, event.state.id))
            events_changed();
        return;
    }
    if (kind < GatewayClientDispatchKind::guild_scheduled_event_snapshot ||
        kind > GatewayClientDispatchKind::guild_scheduled_event_user_remove || !payload.data || !payload.count)
        return;
    const Utf8View json{reinterpret_cast<const char*>(payload.data), static_cast<u32>(payload.count)};
    static u32 scratch[16384];
    if (kind == GatewayClientDispatchKind::guild_scheduled_event_snapshot) {
        // Nothing allocated for servers without events.
        if (!events && json.bytes <= 2)
            return;
        if (!ensure_events())
            return;
        scheduled_event_store_clear_guild(events, event.voice_states.guild_id);
        scheduled_event_decode_list(json, {scratch, 16384}, upsert_listed, nullptr);
        events_changed();
        return;
    }
    if (kind == GatewayClientDispatchKind::guild_scheduled_event_user_add ||
        kind == GatewayClientDispatchKind::guild_scheduled_event_user_remove) {
        ScheduledEventUser user{};
        if (events && g_app.account_ui && scheduled_event_decode_user(json, {scratch, 16384}, &user) &&
            scheduled_event_store_user(events, user.event_id, user.user_id, state().self_user_id,
                                       kind == GatewayClientDispatchKind::guild_scheduled_event_user_add))
            events_changed();
        return;
    }
    ScheduledEvent decoded{};
    if (!scheduled_event_decode(json, {scratch, 16384}, &decoded) || !ensure_events())
        return;
    const bool changed = kind == GatewayClientDispatchKind::guild_scheduled_event_delete
                             ? scheduled_event_store_remove(events, decoded.id)
                             : scheduled_event_store_upsert(events, decoded);
    if (changed)
        events_changed();
}

const ScheduledEventStore* application_guild_community_events() noexcept {
    return events;
}

void application_guild_community_navigation(NavigationView* view) noexcept {
    if (!view || !g_app.account_ui)
        return;
    const auto& s = state();
    const u64 guild_id = s.home_selected ? 0 : s.selected_guild_id;
    const u32 count = guild_id && events ? scheduled_event_store_guild_count(*events, guild_id) : 0;
    // Shown when there is something to see or the account can schedule.
    const bool row = guild_id && (count || can_manage_events(guild_id));
    if (row != view->events_row || count != view->event_count) {
        view->events_row = row;
        view->event_count = count;
        ++view->events_revision;
    }
}

namespace {
const GuildRecord* composer_guild() noexcept {
    if (!g_app.account_ui)
        return nullptr;
    const auto& s = state();
    return s.home_selected ? nullptr : guild(s.selected_guild_id);
}
// A server previewed (lurked) from another client holds the composer too.
bool previewing(const GuildRecord& g) noexcept {
    return self_member_known(g) && (g.settings.self_member & guild_self_member_lurking);
}
} // namespace

bool application_guild_community_composer_gated() noexcept {
    const auto* g = composer_guild();
    return g && (rules_gated(*g) || previewing(*g));
}

Utf8View application_guild_community_composer_notice() noexcept {
    const auto* g = composer_guild();
    if (g && previewing(*g))
        return utf8_literal("You are currently in preview mode. Join this server to start chatting!");
    return g && rules_gated(*g) ? utf8_literal("Complete the server\xE2\x80\x99s rules to start talking")
                                : Utf8View{};
}

bool application_guild_community_composer_press() noexcept {
    const auto* g = composer_guild();
    if (!g || !rules_gated(*g))
        return false;
    application_guild_community_open_rules(state().selected_guild_id);
    return true;
}

void application_guild_community_joined(u64 guild_id) noexcept {
    const auto* g = guild(guild_id);
    if (!g || !g_app.account.api.guild_community)
        return;
    const bool known = self_member_known(*g);
    // Freshly joined: onboarding first when the server has it, then rules.
    if ((g->upload_features & guild_record_onboarding) &&
        (!known || !(g->settings.self_member & guild_self_member_onboarding_completed)) &&
        application_guild_community_open_onboarding(guild_id, false))
        return;
    if (rules_gated(*g))
        application_guild_community_open_rules(guild_id);
}

bool application_guild_community_open_rules(u64 guild_id) noexcept {
    if (!openable(guild_id))
        return false;
    reset_value(&owner->form);
    owner->form_body_bytes = 0;
    owner->phase = Phase::loading;
    reset_value(&owner->status);
    for (auto& answer : owner->answers)
        reset_value(&answer);
    char title[160];
    Bank bank{title, sizeof(title), 0};
    bank.add(guild_name(guild_id));
    if (!open_modal(Mode::rules, guild_id, bank.since(0), 0))
        return false;
    reload();
    return repaint();
}

bool application_guild_community_open_onboarding(u64 guild_id, bool revisit) noexcept {
    if (!openable(guild_id))
        return false;
    reset_value(&owner->onboarding);
    owner->flow_count = owner->step = 0;
    owner->phase = Phase::loading;
    reset_value(&owner->status);
    if (!open_modal(revisit ? Mode::channels_roles : Mode::onboarding, guild_id,
                    revisit ? utf8_literal("Channels & Roles") : utf8_literal("Customize your experience"), 0))
        return false;
    reload();
    return repaint();
}

bool application_guild_community_open_events(u64 guild_id) noexcept {
    if (!openable(guild_id))
        return false;
    owner->from_list = false;
    owner->phase = Phase::loading;
    if (!open_modal(Mode::events, guild_id, utf8_literal("Events"), 0))
        return false;
    enqueue(GuildCommunityOperation::events_list, guild_id);
    return repaint();
}

bool application_guild_community_open_event(u64 guild_id, u64 event_id) noexcept {
    if (!event_id || !openable(guild_id))
        return false;
    owner->event_id = event_id;
    owner->from_list = false;
    const bool known = events && scheduled_event_store_find(*events, event_id);
    owner->phase = known ? Phase::ready : Phase::loading;
    if (!open_modal(Mode::event, guild_id, utf8_literal("Event"), control_interested))
        return false;
    if (!known)
        enqueue(GuildCommunityOperation::event_get, guild_id, event_id);
    enqueue(GuildCommunityOperation::event_rsvps, guild_id);
    return repaint();
}

bool application_guild_community_open_create(u64 guild_id) noexcept {
    if (!g_app.account.api.guild_community || !can_manage_events(guild_id) || !can_open())
        return false;
    const bool switching = application_guild_community_active();
    auto& p = *owner;
    p.where = 1;
    p.duration = 0;
    p.channel_index = 0;
    p.created_event_id = 0;
    p.guild_id = guild_id;
    build_channels();
    if (!p.channel_count) {
        p.where = 2;
        p.duration = 2;
    }
    build_calendar();
    if (switching) {
        const auto* g = guild(guild_id);
        if (!g)
            return false;
        discard_modal_submissions();
        p.lifetime = ++lifetime_serial;
        p.guild_generation = g->generation;
        clear_editors();
        set_title(utf8_literal("Create event"));
    }
    if (!switching && !open_modal(Mode::create, guild_id, utf8_literal("Create event"), control_name))
        return false;
    bind_editor(0, control_name, {}, kScheduledEventNameMaximum, false);
    bind_editor(1, control_description, {}, kScheduledEventDescriptionMaximum, true);
    bind_editor(2, control_location, {}, kScheduledEventLocationMaximum, false);
    p.phase = Phase::ready;
    if (switching)
        return switch_mode(Mode::create, control_name);
    settings_layer_focus(&p.layer, {UiSettingsTargetKind::control, kPage, control_name});
    return repaint();
}

bool application_guild_community_can_create(u64 guild_id) noexcept {
    return g_app.account.api.guild_community && can_manage_events(guild_id);
}

u32 application_guild_community_menu_items(u64 guild_id, u32 onboarding_action, u32 create_event_action,
                                           Slice<PopupItem> output) noexcept {
    const auto* g = guild(guild_id);
    if (!g || g->unavailable || !output.data || !g_app.account.api.guild_community)
        return 0;
    u32 used = 0;
    if (onboarding_action && used < output.count && (g->upload_features & guild_record_onboarding)) {
        reset_value(&output[used]);
        output[used].label = utf8_literal("Channels & Roles");
        output[used].action = onboarding_action;
        output[used].flags = popup_item_enabled;
        ++used;
    }
    if (create_event_action && used < output.count && can_manage_events(guild_id)) {
        reset_value(&output[used]);
        output[used].label = utf8_literal("Create event");
        output[used].action = create_event_action;
        output[used].flags = popup_item_enabled;
        ++used;
    }
    return used;
}

void application_guild_community_want_event(u64 guild_id, u64 event_id) noexcept {
    if (!guild_id || !event_id || !guild(guild_id) || !g_app.account.api.guild_community || !ensure_background())
        return;
    if (events && scheduled_event_store_find(*events, event_id))
        return;
    auto& b = *background;
    for (u32 i = 0; i < b.wanted_count; ++i)
        if (b.wanted_events[i] == event_id)
            return;
    for (u32 i = 0; i < b.missing_count; ++i)
        if (b.missing_events[i] == event_id)
            return;
    if (b.wanted_count >= 8)
        return;
    b.wanted_events[b.wanted_count++] = event_id;
    enqueue(GuildCommunityOperation::event_get, guild_id, event_id);
}

void application_guild_community_frame() noexcept {
    if (!g_app.account_ui)
        return;
    // A gated server opens its rules on its own once per session.
    const auto& s = state();
    const u64 selected = s.home_selected ? 0 : s.selected_guild_id;
    if (selected && application_guild_community_composer_gated() && !g_app.text_form &&
        !application_channel_overview_active() && g_app.renderer_ready && ensure_background()) {
        auto& b = *background;
        bool opened = false;
        for (u32 i = 0; i < b.gate_opened_count && !opened; ++i)
            opened = b.gate_opened[i] == selected;
        if (!opened && b.gate_opened_count < 16) {
            b.gate_opened[b.gate_opened_count++] = selected;
            application_guild_community_open_rules(selected);
        }
    }
    if (owner && owner->scene.active && owner->mode == Mode::create && owner->created_event_id && events &&
        scheduled_event_store_find(*events, owner->created_event_id)) {
        owner->event_id = owner->created_event_id;
        owner->created_event_id = 0;
        owner->from_list = false;
        owner->phase = Phase::ready;
        clear_editors();
        switch_mode(Mode::event, control_interested);
    }
    if (events && events->revision != published_events_revision) {
        published_events_revision = events->revision;
        if (owner && owner->scene.active && (owner->mode == Mode::events || owner->mode == Mode::event))
            repaint();
        // The events row count and event cards follow the store.
        synchronize_navigation_view();
        refresh_account_text(true);
    }
}

bool application_guild_community_active() noexcept {
    return owner && owner->scene.active;
}

void application_guild_community_close(bool restore_focus) noexcept {
    if (!owner || !owner->scene.active)
        return;
    application_composer_text_input_suspend();
    settings_layer_close_choice(&owner->layer);
    settings_layer_cancel_pointer(&owner->layer);
    owner->menu = Menu::none;
    // A queued modal submit is dropped; one in flight finishes and is discarded.
    discard_modal_submissions();
    renderer_bind_settings_scene(&g_app.renderer, nullptr);
    owner->layer.entrance_started = 0;
    renderer_set_settings_modal_progress(&g_app.renderer, 1);
    ui_settings_scene_close(&owner->scene);
    owner->mode = Mode::none;
    owner->lifetime = ++lifetime_serial;
    if (restore_focus && application_account_available())
        g_app.context_focus = &g_app.shell_scene.widgets;
    application_sync_attachment_drop_target();
    if (g_app.renderer_ready)
        refresh_account_text(true);
    application_request_frame();
}

void application_guild_community_release() noexcept {
    application_guild_community_close(false);
    if (background) {
        if (background->request_id && g_app.account.instance && g_app.account.api.guild_community)
            g_app.account.api.guild_community(g_app.account.instance, nullptr, background->request_id);
        SecureZeroMemory(background, sizeof(Background));
        VirtualFree(background, 0, MEM_RELEASE);
        background = nullptr;
    }
    if (events) {
        message_scene_bind_scheduled_events(&g_app.message_scene, nullptr);
        VirtualFree(events, 0, MEM_RELEASE);
        events = nullptr;
    }
    published_events_revision = 0;
    if (!owner)
        return;
    ui_settings_scene_release(&owner->scene);
    for (auto& model : owner->models)
        message_composer_release(&model);
    SecureZeroMemory(owner, sizeof(*owner));
    VirtualFree(owner, 0, MEM_RELEASE);
    owner = nullptr;
}

bool application_guild_community_synchronize() noexcept {
    if (!owner || !owner->scene.active)
        return true;
    auto& p = *owner;
    if (!scope_current()) {
        application_guild_community_close(false);
        return true;
    }
    bool changed = false;
    if (p.copied_until && clock_monotonic_milliseconds() >= p.copied_until) {
        p.copied_until = 0;
        changed = true;
    }
    if (p.layer.choice && !settings_layer_choice_current(p.layer)) {
        p.layer.choice = false;
        p.menu = Menu::none;
        changed = true;
    }
    // The member's pending flag clears through GUILD_MEMBER_UPDATE.
    if (p.mode == Mode::rules && p.phase == Phase::done && !application_guild_community_composer_gated()) {
        application_guild_community_close();
        return true;
    }
    if (changed)
        return repaint();
    if (!ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips()))
        return false;
    return g_app.renderer.interaction_text_settings_revision == p.scene.revision ||
           renderer_rebuild_content_text(&g_app.renderer, application_theme());
}

SettingsLayer* application_guild_community_settings_layer() noexcept {
    return owner ? &owner->layer : nullptr;
}

u64 application_guild_community_next_wake() noexcept {
    if (!background || background->request_id || !background->queue_count)
        return 0;
    return background->retry_at ? background->retry_at : 1;
}

bool application_guild_community_submit(u64 now) noexcept {
    // The tag popout's join (its CAPTCHA retry) also advances per frame.
    application_guild_card_join_poll(now);
    if (!g_app.account_ui || !background || !ensure_background() || background->request_id ||
        !background->queue_count || g_app.details_request_owner != MessageHistoryRequestOwner::none ||
        !g_app.account.instance || !g_app.account.api.guild_community ||
        (background->retry_at && background->retry_at > now))
        return false;
    auto& b = *background;
    const Request request = b.queue[0];
    GuildCommunityRequest wire{};
    wire.guild_id = request.guild_id;
    wire.event_id = request.event_id;
    wire.operation = request.op;
    if (guild_community_operation_has_body(request.op)) {
        const u32 bytes = build_body(request);
        if (!bytes) {
            pop_request();
            request_failed(request, utf8_literal("This couldn't be sent. Try again."));
            return false;
        }
        wire.body = b.body;
        wire.body_bytes = bytes;
    }
    const u64 id = application_next_request_id();
    // A join after a solved CAPTCHA carries its token (one armed retry).
    if (request.op == GuildCommunityOperation::guild_join &&
        application_guild_card_join_take_captcha(request.guild_id) &&
        !application_action_captcha_arm(ApplicationActionCaptchaKind::guild_join, request.guild_id, id, true))
        log_write(LogLevel::warning, "community", "join_captcha_arm_failed");
    const auto submitted = g_app.account.api.guild_community(g_app.account.instance, &wire, id);
    if (submitted.error == RestClientSubmitError::busy) {
        b.retry_at = deadline_after(now, 50);
        return false;
    }
    pop_request();
    b.retry_at = 0;
    if (wire.body_bytes)
        SecureZeroMemory(b.body, wire.body_bytes);
    if (!submitted.ok()) {
        log_u64(LogLevel::warning, "community", "submit_error", static_cast<u64>(submitted.error));
        request_failed(request, utf8_literal("This couldn't be sent. Try again."));
        return false;
    }
    b.flight = request;
    b.request_id = id;
    b.modal_lifetime = owner ? owner->lifetime : 0;
    g_app.details_request_owner = MessageHistoryRequestOwner::channel_overview;
    g_app.details_request_id = id;
    if (owner && owner->scene.active)
        repaint();
    return true;
}

bool application_guild_community_profile(u64 guild_id) noexcept {
    return application_account_available() && enqueue(GuildCommunityOperation::guild_profile_get, guild_id);
}

bool application_guild_community_join(u64 guild_id) noexcept {
    return application_account_available() && enqueue(GuildCommunityOperation::guild_join, guild_id);
}

bool application_guild_community_owns_response(u64 request_id) noexcept {
    return request_id && background && background->request_id == request_id;
}

bool application_guild_community_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!application_guild_community_owns_response(response.request_id))
        return false;
    auto& b = *background;
    const Request request = b.flight;
    const bool same_modal = owner && owner->scene.active && owner->lifetime == b.modal_lifetime &&
                            owner->guild_id == request.guild_id;
    b.request_id = 0;
    if ((modal_submission(request.op) || request.op == GuildCommunityOperation::verification_get ||
         request.op == GuildCommunityOperation::onboarding_get) &&
        (!same_modal || !modal_request_current(request)))
        return true;
    const bool ok = response.transport_ok() &&
                    guild_community_classify(response.status_code) == GuildCommunityOutcome::ok;
    if (response.transport_ok() && response.status_code == 429 && response.rate_limited &&
        response.retry_at_ms && !api_rejection_requires_action(response.rejection)) {
        // Retry the same request once the server allows it.
        if (b.queue_count < kQueueCapacity) {
            for (u32 i = b.queue_count; i > 0; --i)
                b.queue[i] = b.queue[i - 1];
            b.queue[0] = request;
            ++b.queue_count;
            b.retry_at = response.retry_at_ms;
            return true;
        }
    }
    // The server tag popout reads every answer itself (50001: private).
    if (request.op == GuildCommunityOperation::guild_profile_get || request.op == GuildCommunityOperation::guild_join) {
        application_guild_card_response(request.guild_id, request.op == GuildCommunityOperation::guild_join, response,
                                        body);
        return true;
    }
    const auto forget_wanted = [&](bool missing) noexcept {
        for (u32 i = 0; i < b.wanted_count; ++i)
            if (b.wanted_events[i] == request.event_id) {
                b.wanted_events[i] = b.wanted_events[--b.wanted_count];
                break;
            }
        if (missing) {
            if (b.missing_count < 16)
                b.missing_events[b.missing_count++] = request.event_id;
            else
                b.missing_events[request.event_id % 16] = request.event_id;
        }
    };
    if (!ok) {
        log_u64(LogLevel::warning, "community", "response_status", response.status_code);
        if (request.op == GuildCommunityOperation::event_get)
            forget_wanted(true);
        if (request.op == GuildCommunityOperation::verification_submit)
            request_failed(request, failure_text(response, response.status_code == 403
                                                               ? utf8_literal("You don't have permission to join this server.")
                                                               : utf8_literal("Your answers couldn't be submitted. Try again.")));
        else if (request.op == GuildCommunityOperation::event_create)
            request_failed(request, failure_text(response, utf8_literal("Your event couldn't be created.")));
        else if (request.op == GuildCommunityOperation::event_rsvps || request.op == GuildCommunityOperation::event_get)
            events_changed();
        else
            request_failed(request, failure_text(response, utf8_literal("Something went wrong. Try again.")));
        if (request.op == GuildCommunityOperation::event_get && owner && owner->mode == Mode::event &&
            owner->event_id == request.event_id) {
            owner->phase = Phase::failed;
            repaint();
        }
        return true;
    }
    switch (request.op) {
    case GuildCommunityOperation::verification_get:
        if (!same_modal || owner->mode != Mode::rules)
            break;
        if (body.bytes <= sizeof(owner->form_body) &&
            guild_verification_decode(body, {b.scratch, 16384}, &owner->form)) {
            copy_bytes(owner->form_body, body.data, body.bytes);
            owner->form_body_bytes = body.bytes;
            owner->phase = Phase::ready;
            // Text questions take the editor slots in field order.
            u32 slot = 0;
            for (u32 f = 0; f < owner->form.field_count && slot < kEditors; ++f) {
                const auto type = owner->form.fields[f].type;
                if (type == GuildVerificationFieldType::text_input || type == GuildVerificationFieldType::paragraph)
                    bind_editor(slot++, control_text_base + f, {}, type == GuildVerificationFieldType::paragraph ? 1000 : 150,
                                type == GuildVerificationFieldType::paragraph);
            }
            if (!guild_verification_supported(owner->form))
                owner->status = utf8_literal("Some questions can only be answered in the Discord app.");
        } else {
            owner->phase = Phase::failed;
            owner->status = utf8_literal("The rules couldn't be read.");
        }
        repaint();
        break;
    case GuildCommunityOperation::verification_submit: {
        const auto status = guild_verification_decode_status(body, {b.scratch, 16384});
        if (same_modal && owner->mode == Mode::rules) {
            owner->phase = Phase::done;
            owner->status = status == GuildApplicationStatus::submitted
                                ? utf8_literal("Your application to join the server is awaiting review.")
                                : utf8_literal("You're all set.");
            repaint();
            if (status != GuildApplicationStatus::submitted && !application_guild_community_composer_gated())
                application_guild_community_close();
        }
        break;
    }
    case GuildCommunityOperation::onboarding_get:
        if (!same_modal || (owner->mode != Mode::onboarding && owner->mode != Mode::channels_roles))
            break;
        if (guild_onboarding_decode(body, {b.scratch, 16384}, &owner->onboarding)) {
            owner->flow_count = guild_onboarding_flow_prompts(owner->onboarding, {owner->flow, kGuildOnboardingPromptMaximum});
            owner->flow_count = minimum(owner->flow_count, kGuildOnboardingPromptMaximum);
            owner->step = 0;
            owner->phase = Phase::ready;
            // Nothing to ask on join: onboarding is not shown at all.
            if (owner->mode == Mode::onboarding && !owner->onboarding.truncated &&
                (!owner->onboarding.enabled || !owner->flow_count)) {
                const u64 guild_id = owner->guild_id;
                application_guild_community_close();
                const auto* g = guild(guild_id);
                if (g && rules_gated(*g))
                    application_guild_community_open_rules(guild_id);
                break;
            }
        } else {
            owner->phase = Phase::failed;
            owner->status = utf8_literal("The questions couldn't be read.");
        }
        repaint();
        break;
    case GuildCommunityOperation::onboarding_submit:
    case GuildCommunityOperation::onboarding_update:
        if (same_modal) {
            const u64 guild_id = owner->guild_id;
            u64 channel = 0;
            for (u32 i = 0; !channel && i < owner->onboarding.default_channel_count; ++i)
                if (const auto* c = chat_state_store_find_channel(state(), owner->onboarding.default_channels[i]))
                    if (c->guild_id == guild_id && chat_state_store_channel_visible(state(), *c))
                        channel = c->id;
            const bool finishing = request.op == GuildCommunityOperation::onboarding_submit;
            application_guild_community_close();
            if (finishing && channel)
                application_navigate_known_message(channel, 0);
            const auto* g = guild(guild_id);
            if (finishing && g && rules_gated(*g))
                application_guild_community_open_rules(guild_id);
        }
        break;
    case GuildCommunityOperation::events_list: {
        if (!ensure_events())
            break;
        scheduled_event_store_clear_guild(events, request.guild_id);
        scheduled_event_decode_list(body, {b.scratch, 16384}, upsert_listed, nullptr);
        if (same_modal && owner->phase == Phase::loading)
            owner->phase = Phase::ready;
        enqueue(GuildCommunityOperation::event_rsvps, request.guild_id);
        events_changed();
        if (same_modal)
            repaint();
        break;
    }
    case GuildCommunityOperation::event_get: {
        ScheduledEvent e{};
        const bool decoded = scheduled_event_decode(body, {b.scratch, 16384}, &e) && ensure_events();
        // Finished events are not kept; they stay missing for cards.
        forget_wanted(!decoded || !scheduled_event_listed(e));
        if (decoded)
            scheduled_event_store_upsert(events, e);
        if (same_modal && owner->mode == Mode::event)
            owner->phase = Phase::ready;
        events_changed();
        if (same_modal)
            repaint();
        break;
    }
    case GuildCommunityOperation::event_rsvps:
        if (events) {
            scheduled_event_store_reset_interest(events, request.guild_id);
            scheduled_event_decode_users(body, {b.scratch, 16384},
                                         +[](void*, const ScheduledEventUser& user) noexcept {
                                             scheduled_event_store_set_interest(events, user.event_id, true, false);
                                             return true;
                                         },
                                         nullptr);
            events_changed();
        }
        break;
    case GuildCommunityOperation::event_create: {
        ScheduledEvent e{};
        if (scheduled_event_decode(body, {b.scratch, 16384}, &e) && ensure_events()) {
            scheduled_event_store_upsert(events, e);
            if (same_modal && owner->mode == Mode::create) {
                owner->created_event_id = e.id;
                owner->phase = Phase::ready;
            }
        } else if (same_modal) {
            application_guild_community_close();
        }
        events_changed();
        break;
    }
    case GuildCommunityOperation::event_interested:
    case GuildCommunityOperation::event_uninterested:
        b.interest_event = 0;
        if (same_modal)
            repaint();
        events_changed();
        break;
    default:
        break;
    }
    return true;
}

u64 application_guild_community_caret_wake() noexcept {
    if (!application_guild_community_active())
        return 0;
    u64 wake = settings_layer_next_wake(owner->layer);
    if (owner->copied_until && (!wake || owner->copied_until < wake))
        wake = owner->copied_until;
    return wake;
}

} // namespace maicord::app_detail
