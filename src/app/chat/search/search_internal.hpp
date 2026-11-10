#pragma once
// Shared by search.cpp and its parts (search_*.cpp); private to them.
#include "search.hpp"
#include "../../account/account_presentation.hpp"
#include "../threads/browser.hpp"
#include "../channel_authority.hpp"
#include "maicord/model/chat/message_search.hpp"
#include "maicord/model/chat/message_poll_store.hpp"
#include "maicord/ui/chat/search/filter_context.hpp"
#include "maicord/app/chat/search/recent.hpp"
#include <limits.h>

namespace maicord::app_detail {
namespace search_detail {}
using namespace search_detail;
namespace search_detail {

// Two fixed-address banks with independently committed prefixes. Switching a
// model text bank never commits the unused gap between the two prefixes.
struct SearchTextBanks {
    u8* data{};
    u32 committed[2]{};
};

bool text_reserve_both(SearchTextBanks& storage, u32 bytes) noexcept;

struct SearchTextOwner {
    MessageTextPresentation presentation;
    MessageTextCommand commands[kMaximumMessageTextCommands];
    char utf8[kMaximumMessageTextUtf8Bytes];
    TextStyleSpan styles[kMaximumMessageTextStyleSpans];
    MessageMarkdownSemantic semantics[kMaximumMessageTextSemantics];
    MessageTextMeasurement measurements[kMaximumMessageTextMeasurements];
    MessageTextSpoilerReveal reveals[kMaximumMessageTextSpoilerReveals];
    u32 byte_styles[kMessageTextByteStyleScratchCapacity];
    u64 input_signature;
};
struct SearchBank {
    MessageSearchPage page;
    MessageSearchGroup groups[kMessageSearchMaximumGroups];
    MessageSearchMessage messages[kMessageSearchMaximumMessages];
    char channels[kMessageSearchMaximumGroups][128];
    u8 channel_bytes[kMessageSearchMaximumGroups];
    char details[kMessageSearchMaximumMessages][32];
    u8 detail_bytes[kMessageSearchMaximumMessages];
    MessageStore store;
    MessageRecord records[kMessageSearchMaximumMessages];
    u32 order[kMessageSearchMaximumMessages];
    MessageHandle result_handles[kMessageSearchMaximumMessages];
    MessageRichStore rich;
    MessagePollStore staged_polls;
    MessageScenePollBinding polls;
    u8 poll_fields[kMessageSearchMaximumMessages]; // 0 omitted, 1 null, 2 present
    StableStorage content_storage, rich_storage, user_records, user_order;
    SearchTextBanks rich_text, user_names;
    u16 rich_order[kMessageSearchMaximumMessages];
    UserStore users;
    GuildMemberStore members;
    GuildMemberRecord member_records[kMessageSearchMaximumMessages];
    u32 member_order[kMessageSearchMaximumMessages];
    UiVariableList list;
    u32 heights[kMessageSearchMaximumMessages];
    u64 fenwick[kMessageSearchMaximumMessages];
    MessageTimeline timeline;
    char author_names[kMessageSearchMaximumMessages][128];
    char avatars[kMessageSearchMaximumMessages][34];
    u8 author_bytes[kMessageSearchMaximumMessages],
        avatar_bytes[kMessageSearchMaximumMessages];
    u16 channel_generations[kMessageSearchMaximumMessages];
    bool unavailable[kMessageSearchMaximumMessages];
    bool access_checks[kMessageSearchMaximumMessages];
    u64 action_epoch, account_scope, profile_id, self_id;
    const void* account_instance;
    const AccountUiRuntime* account_ui;
    u32 publication;
    bool initialized;
    float body_heights[kMessageSearchMaximumMessages];
    float measured_width, measured_em, measured_tracking;
    const void* measured_native_text;
    u32 measured_dpi, measured_native_format;
};
struct SearchFilterStamp {
    const void *actor{}, *ui{}, *relationships{}, *private_channels{};
    u64 account{}, profile{}, epoch{}, channel_epoch{}, active{}, document{};
    u32 text{}, anchor{}, caret{}, guilds{}, channels{}, permissions{}, names{}, users{}, members{},
        relationship_revision{}, private_revision{}, created{}, updated{}, deleted{}, compacted{},
        publication{};
    bool operator==(const SearchFilterStamp&) const noexcept = default;
};
struct SearchFilterChoices {
    UiSearchFilterChoiceView rows[kUiSearchFilterChoiceMaximum];
    char labels[kUiSearchFilterChoiceMaximum][128], details[kUiSearchFilterChoiceMaximum][64],
        values[kUiSearchFilterChoiceMaximum][24], avatars[kUiSearchFilterChoiceMaximum][kUserAvatarHashMaximumBytes];
    u8 ranks[kUiSearchFilterChoiceMaximum];
    UiSearchFilterContext filter;
    UiSearchFilterChoiceSource source;
    SearchFilterStamp stamp;
    u64 scans, copies, rebuilds;
    bool initialized, capacity_limited;
};

} // namespace search_detail

struct ApplicationSearch {
    MessageComposer edit;
    char query_text[kMessageSearchContentBytes];
    MessageSearchController controller;
    MessageSearchToken submitted;
    MessagePollResultTicket* submitted_poll_tickets;
    u32 submitted_poll_count;
    u64 submitted_poll_gateway_serial;
    SearchBank* bank;
    SearchBank* candidate;
    SearchFilterChoices* choices;
    SearchTextOwner* text_owner;
    UiSearchScene scene;
    UiSearchSource source;
    UiSearchContent content;
    UiSearchTarget pressed;
    u64 scope_id, blink_at, account_id, account_scope, profile_id, submitted_account_scope,
        submitted_profile_id, action_epoch;
    u32 held_key, pressed_revision, edit_revision, committed_edit_revision;
    MessageSearchScope scope;
    MessageSearchSort sort;
    Utf8View validation;
    char scope_heading[160];
    u16 heading_bytes;
    float thumb_grab;
    bool panel, focused, caret_visible, dragging, filters, suggestions, sort_menu, admission_busy, captured,
        scrollbar_dragging, submitted_cancelled, member_query_active;
    // Selected tab, the tab of the published bank, and per-tab totals from the tabs endpoint.
    MessageSearchTab tab, result_tab;
    u8 tab_known;
    bool tabs_unavailable;
    u64 tab_totals[kMessageSearchTabCount];
    SearchRecentList* recent;
    u64 recent_profile;
    Utf8View recent_views[kSearchRecentCapacity];
    u32 recent_published;
    // While the thread browser is open the field searches its thread names.
    // The browser session the user widened to the whole server, if any.
    u64 widened_thread_session;
};

namespace search_detail {

extern u32 filter_publication_counter;
Utf8View without_prefix(const Utf8View& value, char prefix) noexcept;
__declspec(noinline) void menus_close(ApplicationSearch& s) noexcept;
__declspec(noinline) void suggestions_open(ApplicationSearch& s) noexcept;
bool edit_replace(ApplicationSearch& s, u32 begin, u32 end, const Utf8View& text) noexcept;
void member_query_withdraw(ApplicationSearch& s, bool invalidate_choices = false) noexcept;
void choices_release(ApplicationSearch& s) noexcept;
MessagePollResultTicket poll_ticket(const ApplicationSearch& s, u64 id) noexcept;
void bank_release(SearchBank* bank) noexcept;
bool bank_initialize(SearchBank& b) noexcept;
bool bank_user(SearchBank& b, u64 id, const Utf8View& name, const Utf8View& avatar = {}) noexcept;
bool bank_mentions(SearchBank& b, ConstSlice<GatewayMessageNamedUser> users) noexcept;
bool bank_accept(void* opaque, u32 index, const MessageSearchMessage& item) noexcept;

struct SearchDecodeScratch {
    GatewayMessagePollAnswer poll_answers[kGatewayMessagePollMaximumAnswers];
    GatewayMessagePollAnswerCount poll_counts[kGatewayMessagePollMaximumAnswers];
    u64 mention_roles[kGatewayMessageMentionRoleMaximumIds],
        member_roles[kGatewayMessageMemberRoleMaximumIds];
    GatewayMessageNamedUser mention_users[kGatewayMessageMentionUserMaximum],
        snapshot_users[kGatewayMessageMentionUserMaximum];
    GatewayMessageReaction reactions[kGatewayMessageReactionMaximum];
    GatewayMessageAttachment attachments[2][kGatewayMessageAttachmentMaximum];
    GatewayMessageEmbed embeds[2][kGatewayMessageEmbedMaximum];
    GatewayMessageEmbedField fields[2][kGatewayMessageEmbedFieldMaximum];
    GatewayMessageSticker stickers[2][kGatewayMessageStickerMaximum];
    GatewayMessageComponent components[2][kGatewayMessageComponentMaximum];
};

bool publish_polls(ApplicationSearch& s, SearchBank& bank, u32 message_count) noexcept;
MessageSearchRichScratch decode_scratch(SearchDecodeScratch& s) noexcept;
bool available() noexcept;
bool front() noexcept;
bool thread_scope(const ApplicationSearch& s, ApplicationThreadBrowserSearchScope* out) noexcept;
bool resolve(void* context, u32 index, UiSearchRow* out) noexcept;
bool measure_rows(ApplicationSearch& s) noexcept;
void cancel_submitted(ApplicationSearch& s) noexcept;
void focus(ApplicationSearch& s, bool value) noexcept;
void release_pointer(ApplicationSearch& s) noexcept;
void focus_widget(ApplicationSearch& s, u32 index) noexcept;
bool focus_target(ApplicationSearch& s, UiSearchAction action, u32 source_index = ~0u) noexcept;
bool repaint() noexcept;
bool menu_focus(ApplicationSearch& s, const bool& opened, UiSearchAction first) noexcept;
UiScrollbarGeometry body_scrollbar(const ApplicationSearch& s) noexcept;
bool selected_scope(MessageSearchScope* kind, u64* id) noexcept;
bool context_current(const ApplicationSearch& s) noexcept;
bool source_current(void* opaque) noexcept;
void retire_actions(ApplicationSearch& s) noexcept;
void current_scope(ApplicationSearch& s) noexcept;
bool heading(ApplicationSearch& s) noexcept;
bool choices_current(void* opaque) noexcept;
bool channel_usable(const ChannelRecord& c) noexcept;
bool channel_view_denied(const ChatStateStore& state, const ChannelRecord& c) noexcept;
bool choices_synchronize(ApplicationSearch& s) noexcept;
bool choice_accept(ApplicationSearch& s, u32 revision, u32 index) noexcept;
bool choice_move(ApplicationSearch& s, bool previous) noexcept;
bool resolve_user(void*, Utf8View, u64*) noexcept;
bool resolve_channel(void*, Utf8View, u64*) noexcept;
bool local_date(void*, Utf8View date, u64* first, u64* last) noexcept;
void recent_load(ApplicationSearch& s) noexcept;
void recent_store(ApplicationSearch& s) noexcept;
bool commit(ApplicationSearch& s, bool tab_switch = false) noexcept;
void close_panel(ApplicationSearch& s) noexcept;
void decorate(SearchBank& bank) noexcept;
bool activate(ApplicationSearch& s, UiSearchTarget target) noexcept;
bool scroll_read(void* opaque, u64 key, UiScrollRange* out) noexcept;
bool scroll_write(void* opaque, u64 key, double offset, bool* changed) noexcept;
bool row_unavailable(const ApplicationSearch&, u32) noexcept;
bool row_access_check(const ApplicationSearch&, u32) noexcept;

} // namespace search_detail
} // namespace maicord::app_detail
