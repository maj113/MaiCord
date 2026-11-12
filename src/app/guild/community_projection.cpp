#include "community_internal.hpp"
#include "control_rows.hpp"
#include "maicord/os/text.hpp"
#include "../chat/messages/history.hpp"
#include "maicord/model/guild/settings_model.hpp"
#include "maicord/model/chat/sorted_search.hpp"
#include "maicord/ui/text/timestamp_format.hpp"
#include "maicord/os/local_time.hpp"
#include "maicord/os/date_format.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/core/scratch_pages.hpp"

namespace maicord::app_detail {
namespace {

constexpr char kWhereLabels[] = "Stage channel\0Voice channel\0Somewhere else\0";
static_assert(name_list_count(kWhereLabels) == 3);
constexpr char kDurationLabels[] =
    "No end time\0" "30 minutes\0" "1 hour\0" "2 hours\0" "3 hours\0" "6 hours\0" "12 hours\0" "1 day\0";
static_assert(name_list_count(kDurationLabels) == kDurationCount);

constexpr u32 kDurationMinutes[kDurationCount]{0, 30, 60, 120, 180, 360, 720, 1440};

} // namespace
namespace community_detail {

Owner* owner;
u64 lifetime_serial;

Background* background;
ScheduledEventStore* events;
u32 published_events_revision;

} // namespace community_detail
namespace {

bool connected() noexcept {
    return g_app.account.instance && g_app.account.status.gateway_ready;
}

} // namespace
namespace community_detail {

ChatStateStore& state() noexcept {
    return g_app.account_ui->chat_state;
}
Slice<u32> json_scratch() noexcept {
    return {g_app.account_ui->message_history_page_structurals, 16384};
}
bool ensure_background() noexcept {
    if (!g_app.account_ui)
        return false;
    if (!background) {
        background = commit_pages<Background>();
        if (!background)
            return false;
        background->account_scope = g_app.plugin_account_scope_serial;
    }
    if (background->account_scope != g_app.plugin_account_scope_serial) {
        const u64 request = background->request_id;
        SecureZeroMemory(background, sizeof(Background));
        background->account_scope = g_app.plugin_account_scope_serial;
        application_cancel_request(account_module_guild_community, request);
    }
    return true;
}
bool ensure_events() noexcept {
    if (events)
        return true;
    events = commit_pages<ScheduledEventStore>();
    return events != nullptr;
}
void events_changed() noexcept {
    application_request_frame();
}
bool upsert_listed(void*, const ScheduledEvent& e) noexcept {
    scheduled_event_store_upsert(events, e);
    return true;
}
bool enqueue(GuildCommunityOperation op, u64 guild_id, u64 event_id) noexcept {
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
    sorted_erase_gap(b.queue, b.queue_count, 0);
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

} // namespace community_detail
namespace {

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

} // namespace
namespace community_detail {

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

} // namespace community_detail
namespace {

constexpr Utf8View kIncompleteForm =
    utf8_literal("These rules or questions can't be shown completely here. Open Discord to continue.");

} // namespace
namespace community_detail {

bool editable() noexcept {
    return owner && owner->scene.active && owner->phase != Phase::submitting && owner->phase != Phase::loading;
}

// Binds editor slot `slot` to `control` with initial text.
void bind_editor(u32 slot, u64 control, const Utf8View& text, u32 limit, bool multiline) noexcept {
    const u32 bytes = minimum<u32>(text.bytes, kEditorBytes);
    copy_bytes(owner->editor_text[slot], text.data, bytes);
    Editors::load(slot, bytes, limit, multiline);
    owner->editor_controls[slot] = control;
}
int editor_slot(u64 control) noexcept {
    for (u32 i = 0; owner && i < kEditors; ++i)
        if (owner->editor_controls[i] == control)
            return static_cast<int>(i);
    return -1;
}

} // namespace community_detail
namespace {

Utf8View editor_value(u64 control) noexcept {
    const int slot = editor_slot(control);
    return slot >= 0 ? message_composer_text(owner->models[slot]) : Utf8View{};
}

} // namespace
namespace community_detail {

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

} // namespace community_detail
namespace {

// Appends a control and returns it for the optional fields.
UiSettingsControl& add_control(u64 id, const Utf8View& label, const Utf8View& description, UiSettingsControlKind kind,
                               bool enabled = true, bool checked = false,
                               UiSettingsVariant variant = UiSettingsVariant::standard) noexcept {
    return control_row_add({owner->controls, owner->presentation, nullptr, &owner->control_count, kControlCapacity},
                           id, label, description, kind, enabled, checked, {variant, 0, kUiSettingsNoDefault});
}
void notice(u64 id, const Utf8View& label, const Utf8View& description, const Utf8View& action = {}) noexcept {
    add_control(id, label, description, UiSettingsControlKind::notice).action_label = action;
}
void set_footer(const Utf8View& status, const Utf8View& save, const Utf8View& reset, bool visible, bool can_save,
                bool saving, bool failed) noexcept {
    owner->footer = {status, save, reset, visible, can_save, saving, failed};
}
const UiSettingsEditor* editor_view(u64 control, Utf8View placeholder, bool enabled) noexcept {
    const int slot = editor_slot(control);
    return slot < 0 ? nullptr : Editors::view(slot, control, placeholder, enabled);
}

} // namespace
namespace community_detail {

Utf8View guild_name(u64 guild_id) noexcept {
    const auto* g = guild(guild_id);
    return g ? chat_state_store_guild_name(state(), *g) : Utf8View{};
}
Utf8View failure_text(const RestClientResponse& response, const Utf8View& fallback) noexcept {
    if (!response.transport_ok())
        return utf8_literal("Couldn't reach Discord. Try again.");
    switch (guild_community_classify(response.status_code)) {
    case GuildCommunityOutcome::rate_limited: return utf8_literal("Slow down. Try again in a moment.");
    case GuildCommunityOutcome::forbidden: return utf8_literal("You don't have permission to do this.");
    case GuildCommunityOutcome::not_found: return utf8_literal("This is no longer available.");
    default: return fallback;
    }
}

} // namespace community_detail
namespace {

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

} // namespace
namespace community_detail {

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

} // namespace community_detail
namespace {

void project_create() noexcept {
    auto& p = *owner;
    const bool edit = editable();
    auto& where_row = add_control(control_where, utf8_literal("Where is your event?"), {}, UiSettingsControlKind::radio, edit);
    static Utf8View where_views[3];
    name_list_views(where_views, kWhereLabels, 3);
    where_row.choices = {where_views, 3};
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
    // An event somewhere else needs an end time, so it has no "No end time".
    const u32 durations = kDurationCount - (external ? 1 : 0);
    name_list_views(p.duration_views, kDurationLabels + (external ? sizeof("No end time") : 0), durations);
    duration.choices = {p.duration_views, durations};
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

} // namespace
namespace community_detail {

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
    return settings_modal_update(p);
}
bool repaint() noexcept {
    return settings_modal_present(project());
}

} // namespace community_detail
namespace {

// Local midnight of today plus `day` days as a calendar reading. The days are added at local noon, which a daylight
// shift never moves across midnight.
CalendarTime local_day(u32 day) noexcept {
    CalendarTime now{};
    local_time_now(&now);
    now.wHour = 12;
    now.wMinute = now.wSecond = now.wMilliseconds = 0;
    CalendarTime result = now;
    i64 unix = 0;
    if (local_time_to_unix(now, &unix))
        local_time_from_unix(unix + static_cast<i64>(day) * 86'400, &result);
    result.wHour = result.wMinute = result.wSecond = result.wMilliseconds = 0;
    return result;
}
u64 create_start_unix() noexcept {
    CalendarTime start = local_day(owner->day);
    start.wHour = static_cast<u16>(owner->time_slot / 2);
    start.wMinute = static_cast<u16>(owner->time_slot % 2 * 30);
    i64 unix = 0;
    return local_time_to_unix(start, &unix) && unix > 0 ? static_cast<u64>(unix) : 0;
}
u32 wide_label(const wchar_t* wide, int units, char* out, u32 capacity) noexcept {
    const int written = units > 1 ? utf16_to_utf8(wide, units - 1, out, capacity, false)
                                  : 0;
    return written > 0 ? static_cast<u32>(written) : 0;
}

} // namespace
namespace community_detail {

void build_calendar() noexcept {
    auto& p = *owner;
    for (u32 d = 0; d < kDayChoices; ++d) {
        const CalendarTime day = local_day(d);
        u32 bytes = 0;
        if (d < 2) {
            const Utf8View text = d ? utf8_literal("Tomorrow") : utf8_literal("Today");
            copy_bytes(p.day_labels[d], text.data, text.bytes);
            bytes = text.bytes;
        } else {
            wchar_t wide[32]{};
            bytes = wide_label(wide, format_date(LOCALE_NAME_USER_DEFAULT, 0, &day, L"ddd, MMM d", wide, 32),
                               p.day_labels[d], sizeof(p.day_labels[d]));
        }
        p.day_views[d] = {p.day_labels[d], bytes};
    }
    for (u32 t = 0; t < kTimeChoices; ++t) {
        CalendarTime time{};
        time.wYear = 2026;
        time.wMonth = 1;
        time.wDay = 1;
        time.wHour = static_cast<u16>(t / 2);
        time.wMinute = static_cast<u16>(t % 2 * 30);
        wchar_t wide[24]{};
        const int units = format_time(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &time, nullptr, wide, 24);
        p.time_views[t] = {p.time_labels[t], wide_label(wide, units, p.time_labels[t], sizeof(p.time_labels[t]))};
    }
    // The next half hour, an hour ahead, as the official client's default.
    CalendarTime now{};
    local_time_now(&now);
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

} // namespace community_detail
} // namespace maicord::app_detail
