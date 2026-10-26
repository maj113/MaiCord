#pragma once
// Server Settings internals shared by the layer (application_guild_settings.cpp)
// and its pages (application_guild_settings_<page>.cpp). UI thread only.
#include "application_guild_settings.hpp"
#include "application_settings_layer.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/model/guild_ban_store.hpp"
#include "maicord/model/guild_settings_model.hpp"
#include "maicord/model/guild_member_search.hpp"
#include "maicord/model/member_moderation.hpp"
#include "maicord/platform/file_picker.hpp"
#include "maicord/app/profile_image.hpp"
#include "maicord/protocol/guild_assets.hpp"
#include "application_soundboard.hpp"

namespace maicord::app_detail::server_settings {

constexpr u64 kPageOwner = 0x4D41494753455454ull;
// Values follow the sidebar order; the server menu's submenu offsets its
// actions by them.
enum class Page : u8 {
    none,
    profile,
    engagement,
    boost_perks,
    emoji,
    stickers,
    soundboard,
    members,
    roles,
    invites,
    safety,
    audit_log,
    bans,
    count
};
constexpr u32 kPageCount = static_cast<u32>(Page::count) - 1;
static_assert(kPageCount == kGuildSettingsPageCount);
constexpr UiPageKey page_key(Page page) noexcept {
    return {kPageOwner, static_cast<u64>(page)};
}
constexpr u32 kControlCapacity = 2'400;
constexpr u32 kRowCapacity = 2'000;
constexpr u32 kBankBytes = 224 * 1024;
constexpr u32 kBanCapacity = 1'000;
// One GET /bans?limit=1000; after= only continues.
constexpr u32 kBanPage = kGuildAdminPageMaximum;
constexpr u32 kScratchUnits = 48 * 1024;
constexpr u32 kPopupCapacity = 512;
constexpr u32 kSearchBytes = 400;
constexpr u32 kRowBase = 1'000;
constexpr u64 kEchoMs = 8'000;
constexpr u32 kInviteCapacity = 200;
constexpr u32 kAuditCapacity = 400;
constexpr u32 kAuditPage = 50;
constexpr u32 kAuditUsers = 400;
constexpr u32 kStickerCapacity = 120;
// The edited features list of an Engagement save.
constexpr u32 kFeatureNameBytes = 16 * 1024;
constexpr u64 kDeleteServerAction = 0x44454C5345525645ull;
constexpr u32 kMenuChoiceBase = 1'000;

// Inline editors: server or role name, role colour hex, list search, the
// server description and the custom invite link.
enum Editor : u32 {
    editor_name,
    editor_hex,
    editor_search,
    editor_description,
    editor_vanity,
    editor_count
};
// Control ids of the fixed rows; list rows start at kRowBase.
enum Control : u64 {
    control_header = 1,
    control_name,
    control_notice,
    control_create,
    control_search,
    control_list_category,
    control_color,
    control_hex,
    control_hoist,
    control_mentionable,
    control_display_category,
    control_delete,
    control_delete_category,
    control_filter_user,
    control_filter_action,
    control_more,
    control_icon,
    control_icon_remove,
    control_banner,
    control_banner_remove,
    control_role_icon,
    control_role_icon_remove,
    control_upload,
    control_verification,
    control_content_filter,
    control_mfa,
    control_banner_category,
    control_summaries_category,
    control_summaries,
    control_messages_category,
    control_system_channel,
    control_activity_category,
    control_activity_feed,
    control_notifications_category,
    control_notifications,
    control_afk_channel,
    control_afk_timeout,
    control_widget_category,
    control_widget_notice,
    control_widget,
    control_widget_channel,
    control_widget_id,
    control_widget_json,
    control_description,
    control_private,
    control_banner_colors,
    control_progress_bar,
    control_splash,
    control_splash_remove,
    control_vanity_category,
    control_vanity,
    control_vanity_remove,
    control_filter_joined,
    control_filter_created,
    control_filter_method,
    control_filter_roles,
    control_filter_signals,
    control_filter_sort,
    control_page_size,
    control_pages,
    control_select_page,
    control_bulk,
    // System message toggles: this base plus the suppress bit's index.
    control_message = 64,
    // Roles: the editor's tabs, Clear permissions, View Server as Role and
    // the @everyone card.
    control_role_tabs = 80,
    control_clear,
    control_view_as,
    control_everyone,
    control_permission_category = 100,
    control_permission = 200,
};
enum class Op : u8 {
    none,
    profile_update,
    boost_update,
    vanity_get,
    vanity_update,
    engagement_update,
    widget_get,
    widget_update,
    profile_get,
    role_counts,
    role_create,
    role_update,
    role_delete,
    role_reorder,
    ban_list,
    ban_remove,
    invite_list,
    invite_delete,
    audit_page,
    icon,
    banner,
    role_icon,
    emoji_create,
    emoji_rename,
    emoji_delete,
    sticker_list,
    sticker_create,
    sticker_update,
    sticker_delete,
    members_search,
    bulk_ban,
    ban_search,
    safety_update,
    mfa,
    guild_delete,
};
// Image picks waiting for the file dialog.
enum class Pick : u8 {
    none,
    icon,
    banner,
    role_icon,
    emoji,
    sticker,
    sound,
    splash,
};
// Popup menus owned by this layer; actions are offset by kind.
enum class Menu : u8 {
    none,
    system_channel,
    afk_channel,
    afk_timeout,
    widget_channel,
    member,
    timeout,
    ban,
    audit_user,
    audit_action,
    emoji,
    sticker,
    sound,
    role,
    member_since,
    joined_discord,
    join_method,
    member_roles,
    signals,
    sort,
};
enum ExpressionAction : u32 {
    expression_rename = 1,
    expression_tags,
    expression_description,
    expression_delete,
    // Soundboard sounds.
    expression_preview,
    expression_emoji,
    expression_volume,
};
// Text form kinds (TextFormOwnerKey::collection).
enum FormKind : u64 {
    form_role_delete = 1,
    form_unban,
    form_invite_revoke,
    form_emoji_create,
    form_emoji_rename,
    form_emoji_delete,
    form_sticker_name,
    form_sticker_tags,
    form_sticker_rename,
    form_sticker_retag,
    form_sticker_describe,
    form_sticker_delete,
    form_delete_server,
    form_sound_name,
    form_sound_emoji,
    form_sound_rename,
    form_sound_emoji_edit,
    form_sound_volume,
    form_sound_delete,
};

struct Request {
    Op op;
    u64 target;
    bool queued;
};

using Bank = TextBank;

// A picked image held until it is saved: the file bytes (prepared again by the
// account module at submit) and what preparation reported. `clear` removes.
struct Picked {
    u8* data;
    u32 bytes, capacity;
    GuildImageInfo info;
    bool set, clear;
};

struct Settings {
    UiSettingsScene scene;
    SettingsLayer layer;
    UiPageRegistry registry;
    UiPage pages[kPageCount];
    UiSettingsPageContent content[kPageCount];
    UiSettingsNavEntry nav[24];
    u32 nav_count, nav_revision;
    UiSettingsControl controls[kControlCapacity];
    UiSettingsControlPresentation presentation[kControlCapacity];
    UiSettingsAvatarMedia avatar_media[kControlCapacity];
    UiSettingsListItem list_items[kControlCapacity];
    u32 control_count;
    UiSettingsEditor editors[editor_count];
    UiSettingsFooter footer;
    MessageComposer models[editor_count];
    BoundedEdit bindings[editor_count];
    char name_text[kGuildAdminNameBytes], hex_text[16], search_text[kSearchBytes],
        description_text[kGuildAdminProfileDescriptionBytes], vanity_text[kGuildAdminVanityCodeBytes];
    u64 lifetime, guild_id, account_scope, profile_id, self_user_id;
    u16 guild_generation;
    GuildSettingsAccess access;
    Page page;
    u64 signature;
    // Server Profile: drafts over the last profile read. A replaced icon is
    // cropped into its data URI when the save starts.
    GuildServerProfileDraft profile_base, profile_draft;
    bool profile_loaded;
    ProfileImageInfo icon_info;
    u8* icon_uri;
    u32 icon_uri_bytes;
    // Boost Perks: the guild PATCH draft, the picked splash, the data URIs of
    // new images (made when the save starts) and the custom invite link.
    GuildBoostDraft boost_base, boost_draft;
    bool boost_echo;
    u64 boost_echo_until;
    Picked splash;
    u8 *banner_uri, *splash_uri;
    u32 banner_uri_bytes, splash_uri_bytes;
    char vanity_base[kGuildAdminVanityCodeBytes];
    u8 vanity_base_bytes;
    u32 vanity_uses;
    u64 vanity_error_code;
    bool vanity_requested, vanity_loaded, vanity_failed, vanity_error;
    // Engagement. The widget loads once per open; the guild PATCH of a save
    // is followed by the widget PATCH even when it failed.
    GuildEngagementDraft engagement_base, engagement_draft;
    bool engagement_conflict, engagement_echo, engagement_guild_sent;
    bool widget_requested, widget_loaded, widget_failed;
    // The server profile read of the current page visit.
    bool profile_requested;
    u64 engagement_echo_until;
    // Roles.
    u64 order[kGuildRoleMaximum];
    u32 order_count;
    bool order_dirty;
    u64 count_role[kGuildRoleMaximum];
    u32 count_value[kGuildRoleMaximum];
    u32 counts;
    bool counts_requested;
    u64 editing_role, open_after_create;
    // The role editor's tab (Display, Permissions).
    u8 role_tab;
    GuildRoleDraft role_base, role_draft;
    bool role_conflict, role_echo, role_missing, hex_invalid;
    u64 role_echo_until;
    // Members: the chunked search (its bank is committed on the first visit),
    // the filters, the debounced search text and the bulk-ban selection,
    // which survives paging.
    GuildMemberSearchStore search;
    GuildSearchMember* search_bank;
    u64 search_due_at;
    bool search_dirty, search_retry;
    // The planned request until it is sent, and the clock of the last sent.
    GuildMemberSearchQuery search_pending;
    u64 search_pending_hash, search_sent_now;
    u32 search_pending_bytes;
    u64 filter_roles[kGuildMemberSearchRoleMaximum];
    u32 filter_role_count;
    u64 filter_joined_after, filter_created_after;
    u8 filter_joined, filter_created, filter_signals, filter_sort, filter_join_type;
    bool filter_join_type_set;
    char filter_invite[kGuildMemberSearchInviteCodeBytes];
    u8 filter_invite_bytes;
    char search_applied[kSearchBytes];
    u32 search_applied_bytes;
    u64 selected[kGuildBulkBanMaximumUsers];
    u32 selected_count;
    const GuildSearchMember* page_rows[100];
    // The last bulk ban's outcome, shown above the list.
    Utf8View bulk_notice;
    bool bulk_failed;
    u32 page_row_count;
    Utf8View filter_choice[6][1];
    // Members and bans.
    u64 row_ids[kRowCapacity];
    u32 row_count;
    u64 menu_user;
    Menu menu;
    u64 menu_channel[kPopupCapacity];
    GuildBanStore bans;
    bool bans_requested, bans_failed;
    // The last server-side ban search: its text and the bans it found.
    GuildAdminBan ban_search[kGuildBanSearchLimit];
    u32 ban_search_count;
    char ban_search_text[kSearchBytes];
    u32 ban_search_bytes;
    // Invites and audit log.
    GuildAdminInvite invites[kInviteCapacity];
    u32 invite_count;
    bool invites_requested, invites_loaded, invites_failed;
    GuildAdminAuditEntry audit[kAuditCapacity];
    GuildAdminUser audit_users[kAuditUsers];
    u32 audit_count, audit_user_count;
    u64 audit_filter_user;
    u16 audit_filter_action;
    bool audit_requested, audit_complete, audit_filter_action_set, audit_failed;
    GuildAdminAuditEntry audit_page[kAuditPage];
    GuildAdminUser audit_page_users[kAuditPage * 2];
    u64 menu_users[kPopupCapacity];
    Utf8View audit_user_choice[1], audit_action_choice[1];
    // Picked images and the dialog that picks them.
    Picked icon, banner, role_icon, upload;
    NativeFilePicker picker;
    Pick pick;
    bool picker_consumed;
    u64 pick_role;
    char upload_filename[128];
    u32 upload_filename_bytes;
    // Safety.
    GuildSafetyDraft safety_base, safety_draft;
    bool safety_echo;
    u64 safety_echo_until;
    // Stickers.
    GuildStickerRecord stickers[kStickerCapacity];
    u32 sticker_count;
    bool stickers_requested, stickers_loaded, stickers_failed;
    // Text for the queued expression request and the open expression form.
    char asset_name[kGuildStickerNameBytes], asset_tags[kGuildStickerTagsBytes],
        asset_description[kGuildStickerDescriptionBytes];
    u16 asset_name_bytes, asset_tags_bytes, asset_description_bytes;
    u8 asset_fields;
    // Soundboard upload: the name between its name and emoji steps.
    char sound_name[kSoundboardNameBytes];
    u32 sound_name_bytes;
    u32 soundboard_revision;
    u64 menu_item;
    Utf8View form_status;
    u32 form_revision;
    // Transport.
    Request request;
    u64 request_id, retry_at;
    Utf8View status;
    bool failed;
    // Two banks so labels borrowed by the previous projection stay valid
    // until the scene has taken the next one.
    char banks[2][kBankBytes];
    u32 bank_index;
    char popup_text[32 * 1024];
    PopupItem popup_items[kPopupCapacity];
    // Projection scratch: choice labels, member chips and popup sorting.
    Utf8View system_choice[1], afk_choice[1], widget_choice[1], color_names[kGuildRoleColorCount + 1];
    u32 member_order[kGuildMemberSearchCapacity];
    // Packed RGBA for list-row accents, resolved through the palette source.
    u32 row_accent[kRowCapacity];
    u64 palette_revision;
    Utf8View chips[kRowCapacity][4];
    u32 chip_colors[kRowCapacity][4];
    struct PopupRow {
        PopupItem item;
        const ChannelRecord* channel;
    } popup_rows[kPopupCapacity];
    u8 feature_names[kFeatureNameBytes];
    u32 scratch[kScratchUnits];
    GuildAdminRoleCount role_counts[kGuildRoleMaximum];
    GuildBanRecord ban_records[kBanCapacity];
};

extern Settings* owner;
// The next expression form, opened on a later tick once menus have closed.
extern u64 next_form;
extern u64 next_form_item;

// One page's behaviour; null entries mean the page has none.
struct PageOps {
    // Loads the drafts when the page is selected, opened or reset.
    void (*load)() noexcept;
    void (*project)(Bank&) noexcept;
    // Control activations other than editors and notices.
    void (*activate)(const SettingsLayerActivation&) noexcept;
    // Choices of the popup menus the page opened.
    void (*choose)(Menu, u32 action) noexcept;
    bool (*dirty)() noexcept;
    bool (*can_save)() noexcept;
    Op (*next_save)() noexcept;
    // Follows the state store; true when the page changed.
    bool (*reconcile)(u64 now) noexcept;
    // Queues the page's next missing read; false when nothing was queued.
    bool (*ensure_data)() noexcept;
    // The page's notice action ("Load latest", "Try again").
    void (*notice)() noexcept;
    // Footer text when the draft cannot be saved; empty otherwise.
    Utf8View (*invalid)() noexcept;
    // Right-click or the menu key on a control.
    void (*context)(const SettingsLayerActivation&) noexcept;
};

extern const PageOps kProfilePage, kEngagementPage, kBoostPage, kExpressionPage, kMembersPage, kRolesPage,
    kInvitesPage, kSafetyPage, kAuditLogPage, kBansPage;

// Identity and transport.
bool available() noexcept;
const ChatStateStore& state() noexcept;
const GuildDetailStore& details() noexcept;
const GuildDetailRecord* guild_details() noexcept;
u8 premium_tier() noexcept;
u64 guild_features() noexcept;
bool scope_current() noexcept;
bool connected() noexcept;
bool busy() noexcept;
bool mutation_pending() noexcept;
bool page_allowed(Page page) noexcept;
bool can_manage_guild() noexcept;
bool editable() noexcept;
void queue(Op op, u64 target = 0) noexcept;
void show_failure(Utf8View text) noexcept;
bool repaint() noexcept;
bool guard_dirty() noexcept;
void select_page(Page page) noexcept;
void request_close() noexcept;

// Drafts and editors.
const RoleRecord* role(u64 id) noexcept;
bool pending(const Picked& picked) noexcept;
void release_picked(Picked* picked) noexcept;
void set_editor(Editor index, Utf8View text) noexcept;
Utf8View editor_text(Editor index) noexcept;
void start_pick(Pick pick) noexcept;

// Projection helpers.
u32 push(const UiSettingsControl& control, UiSettingsControlPresentation presentation = {}) noexcept;
// Fills popup row `index` (enabled plus `flags`); the action is kMenuChoiceBase + action.
void popup_row(u32 index, Utf8View label, u32 action, u32 flags = 0) noexcept;
// A control with no choices or editor: an optional action button label.
u32 push_row(u64 id, Utf8View label, Utf8View description, UiSettingsControlKind kind, bool enabled,
             bool checked = false, Utf8View action = {}, UiSettingsControlPresentation presentation = {}) noexcept;
u32 push_category(u64 id, Utf8View label, Utf8View description = {}) noexcept;
// A plain notice row.
u32 push_notice(Utf8View description, Utf8View label = {}) noexcept;
// A critical notice; `action` labels its retry button.
u32 push_error(Utf8View label, Utf8View description, Utf8View action = {}) noexcept;
// A notice with the loading spinner.
u32 push_loading(Utf8View description, u64 id = control_notice) noexcept;
// The list heading "<title><count>".
u32 push_count_heading(Bank& bank, Utf8View title, u32 count) noexcept;
Utf8View channel_label(Bank& bank, u64 channel_id, Utf8View none) noexcept;
UiSettingsEditor& editor_view(Editor index, u64 control, Utf8View placeholder, bool enabled) noexcept;
u32 person_color(u64 user_id) noexcept;
void image_summary(Bank& bank, const GuildImageInfo& info) noexcept;
Utf8View picked_text(Bank& bank, const Picked& picked, Utf8View current) noexcept;
void relative_time(Bank& bank, u64 then_ms, u64 now_ms, bool future) noexcept;
Utf8View user_label(u64 id) noexcept;
Utf8View user_name(const GuildAdminUser& user) noexcept;
// Channels of `types` (a bit per channel type) in channel-list order behind a
// first "none" row; menu_channel holds the matching ids.
void open_channel_menu(Menu menu, u32 types, u64 current, Utf8View none, PermissionSet required,
                       UiRect anchor) noexcept;

// Roles: a dragged row dropped before or after another, and the draft
// order with deleted roles skipped.
void roles_drop(u64 dragged, u64 target, bool after) noexcept;
u32 resolve_order(const RoleRecord** output) noexcept;

// Engagement: the widget read (null when it failed), the save's two PATCH
// bodies, and the outcome of each.
void engagement_widget_loaded(const GuildAdminWidget* widget) noexcept;
bool engagement_guild_patch(GuildSettingsPatch* patch) noexcept;
void engagement_widget_patch(GuildSettingsPatch* patch) noexcept;
void engagement_saved(Op op, bool ok, u64 now) noexcept;
// The server profile, kept across openings: Server Profile and Engagement
// read it on each visit when it is missing or older than a minute. Null
// until read for this guild or after a failed read; `settled` is true once
// a read finished either way.
const GuildAdminProfile* server_profile(bool* settled = nullptr) noexcept;
bool profile_fetch_due() noexcept;
void server_profile_loaded(const GuildAdminProfile* profile) noexcept;
// Server Profile: the save's body (cropping a new icon first) and its outcome.
bool profile_patch(GuildSettingsPatch* patch, GuildServerProfileWrite* write,
                   Slice<GuildServerProfileTraitWrite> traits) noexcept;
void profile_saved(bool ok, Utf8View body) noexcept;
// Boost Perks: the vanity read, the save's two requests and their outcomes.
void boost_vanity_loaded(const GuildAdminVanity* vanity) noexcept;
bool boost_vanity_patch(GuildSettingsPatch* patch) noexcept;
bool boost_guild_patch(GuildSettingsPatch* patch) noexcept;
void boost_saved(Op op, bool ok, Utf8View body, u64 now) noexcept;
void release_uri(u8** uri, u32* bytes) noexcept;

// Members: the search request of the current filters and page, its
// outcome, the debounced text and the bulk ban update.
bool members_search_request(GuildModerationRequest* request, GuildMemberSearchQuery* query) noexcept;
void members_search_sent() noexcept;
void members_search_response(const RestClientResponse& response, Utf8View body, u64 now) noexcept;
void members_tick(u64 now) noexcept;
void members_bulk_ban_update(const GuildBulkBanUpdate& update) noexcept;
void members_release() noexcept;
// Audit Log: queues older entries once their loading row is on screen.
bool audit_load_visible() noexcept;
// Bans: Enter in the search box, the ban search request and its reply.
void bans_editor_enter() noexcept;
bool bans_search_request(GuildModerationRequest* request) noexcept;
void bans_search_loaded(bool ok, Utf8View body) noexcept;

// Text forms.
TextFormOwnerKey form_key(u64 kind, u64 item) noexcept;
bool form_valid(TextFormOwnerKey key) noexcept;
bool form_save(TextFormOwnerKey key, Utf8View) noexcept;
void open_expression_form(u64 kind, u64 item) noexcept;

} // namespace maicord::app_detail::server_settings
