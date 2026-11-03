#pragma once
// Shared by community.cpp and its parts (community_*.cpp); private to them.
#include "community.hpp"
#include "../settings/modal.hpp"
#include "../shell/state.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/model/chat/bounded_edit.hpp"
#include "maicord/ui/shell/navigation_view.hpp"
#include "../account/account_access.hpp"

namespace maicord::app_detail {
namespace community_detail {}
using namespace community_detail;
namespace community_detail {

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

constexpr u32 kDurationCount = 8;

constexpr Utf8View kDurationLabels[kDurationCount]{
    utf8_literal("No end time"), utf8_literal("30 minutes"), utf8_literal("1 hour"),   utf8_literal("2 hours"),
    utf8_literal("3 hours"),     utf8_literal("6 hours"),    utf8_literal("12 hours"), utf8_literal("1 day")};

struct Request {
    u64 guild_id, event_id;
    GuildCommunityOperation op;
};

struct Owner : SettingsDialog {
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

extern Owner* owner;
extern u64 lifetime_serial;

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

extern Background* background;
extern ScheduledEventStore* events;
extern u32 published_events_revision;
ChatStateStore& state() noexcept;
bool ensure_background() noexcept;
bool ensure_events() noexcept;
void events_changed() noexcept;
bool upsert_listed(void*, const ScheduledEvent& e) noexcept;
bool enqueue(GuildCommunityOperation op, u64 guild_id, u64 event_id = 0) noexcept;
const GuildRecord* guild(u64 id) noexcept;
bool self_member_known(const GuildRecord& g) noexcept;
bool rules_gated(const GuildRecord& g) noexcept;
bool can_manage_events(u64 guild_id) noexcept;

using Bank = TextBank;

bool modal_submission(GuildCommunityOperation op) noexcept;
bool modal_request_current(const Request& request) noexcept;
void discard_modal_submissions() noexcept;
void pop_request() noexcept;
bool rules_presentation_complete(const Owner& p) noexcept;
bool onboarding_presentation_complete(const Owner& p) noexcept;
bool modal_presentation_complete(const Owner& p) noexcept;
bool onboarding_required_complete(const GuildOnboarding& model) noexcept;
bool editable() noexcept;
using Editors = SettingsEditors<&owner, editable>;
void bind_editor(u32 slot, u64 control, const Utf8View& text, u32 limit, bool multiline) noexcept;
int editor_slot(u64 control) noexcept;
bool rules_answers(GuildVerificationAnswer* answers) noexcept;
void clear_editors() noexcept;
Utf8View guild_name(u64 guild_id) noexcept;
Utf8View failure_text(const RestClientResponse& response, const Utf8View& fallback) noexcept;
ScheduledEventCreate event_draft() noexcept;
bool project() noexcept;
bool repaint() noexcept;
void build_calendar() noexcept;
void build_channels() noexcept;

} // namespace community_detail
} // namespace maicord::app_detail
