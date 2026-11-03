#include "community_internal.hpp"
#include "../shell/toast.hpp"
#include "card.hpp"
#include "../account/login/action_captcha.hpp"
#include "../account/account_presentation.hpp"
#include "channel_overview.hpp"
#include "../chat/composer/text_input.hpp"
#include "../shell/content_input.hpp"
#include "maicord/app/gateway/client_event.hpp"
#include "maicord/core/log.hpp"

namespace maicord::app_detail {
namespace {

void activate(SettingsLayer*, const SettingsLayerActivation&) noexcept;
void choose(SettingsLayer*, u32 action) noexcept;
bool edited(SettingsLayer*, u64 control) noexcept {
    if (editor_slot(control) < 0)
        return false;
    owner->failed = false;
    reset_value(&owner->status);
    return true;
}
SettingsLayerEditor editor_hook(SettingsLayer*, u64 control) noexcept {
    const int slot = editor_slot(control);
    if (slot < 0)
        return {};
    return {&owner->models[slot], &owner->bindings[slot],
            control == control_description ||
                (control >= control_text_base && control < control_text_base + kGuildVerificationFieldMaximum &&
                 owner->form.fields[control - control_text_base].type == GuildVerificationFieldType::paragraph)};
}
constexpr SettingsLayerHooks kHooks{editor_hook, edited, activate, choose,
                                    [](SettingsLayer*) noexcept { application_guild_community_close(); },
                                    [](SettingsLayer*) noexcept { return repaint(); }, nullptr,
                                    [](SettingsLayer*) noexcept { return editable(); }};

bool ensure_owner() noexcept {
    if (owner)
        return true;
    owner = settings_modal_allocate<Owner>(kHooks);
    if (!owner)
        return false;
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
    settings_modal_yield();
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
    set_title(title);
    if (!settings_dialog_begin(p, kPage, p.content.title))
        return false;
    if (settings_dialog_show(p, kPage, focus, true, project) && repaint())
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
void open_menu(Menu menu, const UiRect& anchor) noexcept {
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
                                           const Slice<PopupItem>& output) noexcept {
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
    settings_modal_suspend(*owner);
    owner->menu = Menu::none;
    // A queued modal submit is dropped; one in flight finishes and is discarded.
    discard_modal_submissions();
    owner->mode = Mode::none;
    owner->lifetime = ++lifetime_serial;
    settings_modal_leave(*owner, restore_focus && application_account_available());
}

void application_guild_community_release() noexcept {
    application_guild_community_close(false);
    if (background) {
        application_cancel_request(&AccountModuleApi::guild_community, background->request_id);
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
    for (auto& model : owner->models)
        message_composer_release(&model);
    settings_modal_free(owner);
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
    if (settings_layer_choice_lapsed(p.layer)) {
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
    return settings_modal_refresh(p);
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
    if (submit_busy(submitted, b.retry_at, now, 50))
        return false;
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
    return earliest_deadline(settings_layer_next_wake(owner->layer), owner->copied_until);
}

} // namespace maicord::app_detail
