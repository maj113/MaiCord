#pragma once
// Server Settings internals shared by the layer (settings.cpp)
// and its pages (application_guild_settings_<page>.cpp). UI thread only.
#include "settings.hpp"
#include "../../settings/layer.hpp"
#include "../../settings/modal.hpp"
#include "../../chat/composer/text_form.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/model/guild/ban_store.hpp"
#include "maicord/model/guild/settings_model.hpp"
#include "maicord/model/guild/server_tag.hpp"
#include "maicord/model/guild/member_search.hpp"
#include "maicord/platform/desktop/file_picker.hpp"
#include "maicord/app/media/profile_image.hpp"
#include "maicord/protocol/guild/assets.hpp"
#include "maicord/protocol/guild/community.hpp"
#include "maicord/protocol/guild/automation.hpp"
#include "../../voice/soundboard.hpp"
#include "maicord/model/chat/chat_state.hpp"
#include "maicord/model/guild/detail_store.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/ui/chat/game_media.hpp"
#include "maicord/ui/guild/banner.hpp"
#include "maicord/ui/guild/card_scene.hpp"

namespace maicord::app_detail::server_settings {

constexpr u64 kPageOwner = 0x4D41494753455454ull;
// Values follow the sidebar order; the server menu's submenu offsets its
// actions by them.
enum class Page : u8 {
    none,
    profile,
    server_tag,
    engagement,
    boost_perks,
    emoji,
    stickers,
    soundboard,
    members,
    roles,
    invites,
    access,
    integrations,
    safety,
    audit_log,
    bans,
    automod,
    community,
    guild_template,
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
constexpr u32 kChoicePool = 512;
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
// The App Directory sidebar link (open_app_directory).
constexpr u64 kAppDirectoryAction = 0x4150504449524543ull;
constexpr u32 kMenuChoiceBase = 1'000;

// Inline editors: server or role name, list search, the
// server description and the custom invite link; AutoMod's word lists:
// blocked words, regex patterns (one per line) and allowed words.
enum Editor : u32 {
    editor_name,
    editor_search,
    editor_description,
    editor_vanity,
    editor_words,
    editor_patterns,
    editor_allowed,
    // Server Tag: the tag text.
    editor_tag,
    // Server Profile: the five trait labels (editor_trait + slot) and the game search.
    editor_trait,
    editor_trait_last = editor_trait + kGuildAdminProfileTraits - 1,
    editor_game_search,
    editor_count
};
// AutoMod's word boxes: 1,000 words of up to 60 characters with their separators.
constexpr u32 kWordsCodepoints = 62'000;
constexpr u32 kWordsBytes = 64 * 1024;
constexpr u32 kPatternsCodepoints = kAutoModRegexMaximum * (kAutoModRegexCodepoints + 1);
constexpr u32 kPatternsBytes = kPatternsCodepoints * 4;
// Control ids of the fixed rows; list rows start at kRowBase.
enum Control : u64 {
    control_header = 1,
    control_name,
    control_notice,
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
    // Integrations and AutoMod: their own fixed rows from here (below kRowBase).
    control_automation = 600,
    control_words = control_automation,
    control_patterns,
    control_allowed,
    // Server Tag: its fixed rows (700-799; the preview's sample lines follow control_tag_preview).
    control_tag_intro = 700,
    control_tag_private,
    control_tag_locked,
    control_tag_enable,
    control_tag_text,
    control_tag_text_error,
    control_tag_name_category,
    control_tag_badges,
    control_tag_show_locked,
    control_tag_locked_badges,
    control_tag_colors,
    control_tag_custom,
    control_tag_adopt_category,
    control_tag_adopt,
    control_tag_preview_category,
    control_tag_preview,
    // Access, Community and Server Template: their own fixed rows from here (server_access.hpp).
    control_server_access = 800,
    // Server Profile: its fixed rows (900-999); game tiles use their application id as control id.
    control_icon_buttons = 900,
    control_traits_category,
    control_games_search,
    control_games_suggested,
    control_private_access,
    control_trait = 910,
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
    role_icon,
    emoji_create,
    emoji_rename,
    emoji_delete,
    sticker_list,
    sticker_create,
    sticker_update,
    sticker_delete,
    members_search,
    ban_search,
    safety_update,
    mfa,
    guild_delete,
    // The Integrations and AutoMod pages' requests (apps_request, automod_request).
    apps,
    automod,
    // The Access, Community and Server Template pages' requests (server_access_request).
    server_access,
    // Server Tag: the tag fields of the profile, and adopting the tag.
    tag_update,
    tag_adopt,
    // Server Profile: the server's most played games (the "Your Members Play" suggestions).
    top_games,
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
    webhook_avatar,
    // Access: the Discovery banner (the guild's discovery_splash), held in Settings::splash.
    discovery_splash,
    // Server Profile: a Discoverable server's custom banner, held in Settings::custom_banner.
    custom_banner,
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
    // The Integrations and AutoMod pages' menus (their own kinds).
    automation,
    // The Access and Community pages' menus (their own kinds).
    server_access,
    // Server Profile: a trait's emoji picker.
    trait_emoji,
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
    // Server Tag: the colour picker on the badge's colours (item: 0 primary, 1 secondary).
    form_tag_color,
    // Roles: the colour picker on the edited role's colour (item: the role).
    form_role_color,
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

struct Settings : SettingsModal {
    UiPage pages[kPageCount];
    UiSettingsPageContent content[kPageCount];
    UiSettingsNavEntry nav[40];
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
    char name_text[kGuildAdminNameBytes], search_text[kSearchBytes],
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
    // A new custom banner (the Discoverable upload tile): the file, sent as its own data URI as the official
    // image input does.
    Picked custom_banner;
    u8* custom_banner_uri;
    u32 custom_banner_uri_bytes;
    // Server Profile: the trait labels and game search as typed, the server's most played games (the "Your
    // Members Play" tiles; top-games, read once a visit), the trait an emoji pick goes to, the covers of the
    // game tiles on screen, and the live preview card with its game icons and banner.
    char trait_text[kGuildAdminProfileTraits][kGuildAdminProfileTraitBytes];
    char game_search_text[kGuildAdminGameQueryCodepoints * 4];
    GuildAdminGameActivity top_games[kGuildAdminProfileGameMaximum];
    u32 top_game_count, emoji_slot;
    bool top_games_requested, top_games_failed;
    GameImageMedia covers[kGuildAdminProfileGames + kGuildAdminProfileGameMaximum];
    GameImageMedia preview_games[kUiGuildCardGames];
    // The custom banner on the card and on the Banner row's upload tile.
    UiGuildImageMedia preview_banner, banner_tile;
    UiGuildCardScene preview;
    float preview_height;
    u32 icon_color;
    bool has_icon_color;
    u32 lookup_revision;
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
    // Server Tag: the draft over the profile's tag fields, whether the
    // custom colour rows and the locked badges show, and whether the tag
    // was adopted this visit. The glyph tiles back its badge grids and name
    // previews (UiSettingsPageContent::glyph_tiles); the badge grids list
    // badge types by choice. tag_images is whether the saved badge's CDN image
    // (clan-badges) is known for this draft base, at the size of a tile and of
    // the preview chip; a tile showing exactly that badge draws it.
    GuildTagDraft tag_base, tag_draft;
    bool tag_images;
    UiGuildImageMedia tag_tile_image, tag_chip_image;
    bool tag_loaded, tag_custom_open, tag_show_locked, tag_adopted;
    char tag_text[kGuildAdminTagBytes], tag_error[192];
    UiSettingsGlyphTile glyph_tiles[96];
    u32 glyph_tile_count;
    u8 tag_unlocked[kGuildTagBadgeCount], tag_locked[kGuildTagBadgeCount];
    Utf8View tag_unlocked_names[kGuildTagBadgeCount], tag_locked_names[kGuildTagBadgeCount],
        tag_palette_names[kGuildTagPaletteCount + 2];
    u64 engagement_echo_until;
    // Roles.
    u64 order[kGuildRoleMaximum];
    u32 order_count;
    bool order_dirty;
    // The reorder request's roles and rows, built as the save is sent.
    const RoleRecord* order_rows[kGuildRoleMaximum];
    GuildAdminPosition positions[kGuildRoleMaximum];
    u32 counts;
    bool counts_requested;
    u64 editing_role, open_after_create;
    // The role editor's tab (Display, Permissions).
    u8 role_tab;
    GuildRoleDraft role_base, role_draft;
    bool role_conflict, role_echo, role_missing;
    u64 role_echo_until;
    // Members: the chunked search (its bank is reserved on the first visit
    // and committed as replies add members), the filters, the debounced
    // search text and the bulk-ban selection, which survives paging.
    GuildMemberSearchStore search;
    StableStorage search_bank;
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
    // Backs `status` while it says a picked file is over its size limit.
    char size_error[192];
    bool failed;
    // Two banks so labels borrowed by the previous projection stay valid
    // until the scene has taken the next one.
    char banks[2][kBankBytes];
    u32 bank_index;
    // The choice lists of the buttons and drop-downs of the projection, per bank like the labels.
    Utf8View choice_pool[2][kChoicePool];
    u32 choice_used;
    char popup_text[32 * 1024];
    PopupItem popup_items[kPopupCapacity];
    // Projection scratch: the role colour names, member chips and popup sorting.
    Utf8View color_names[kGuildRoleColorCount + 1];
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
    // AutoMod's word boxes (editor_words, editor_patterns, editor_allowed).
    char words_text[kWordsBytes], patterns_text[kPatternsBytes], allowed_text[kWordsBytes];
    // The Integrations and AutoMod pages' state, made on their first visit.
    struct AppsState* apps;
    struct AutoModState* automod;
    // The Access, Community and Server Template pages' state, made on their first visit.
    struct ServerAccessState* server_access;
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
    // Footer text when the draft cannot be saved; null otherwise.
    const char* (*invalid)() noexcept;
    // Right-click or the menu key on a control.
    void (*context)(const SettingsLayerActivation&) noexcept;
};

extern const PageOps kProfilePage, kServerTagPage, kEngagementPage, kBoostPage, kExpressionPage, kMembersPage,
    kRolesPage, kInvitesPage, kAccessPage, kIntegrationsPage, kSafetyPage, kAuditLogPage, kBansPage, kAutoModPage,
    kCommunityPage, kTemplatePage;
// The Community entry: "Enable Community", or "Community Overview" once the
// guild has the COMMUNITY feature.
Utf8View community_page_label(u64 guild_id) noexcept;

// Identity and transport.
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
using Editors = SettingsEditors<&owner, editable>;
void queue(Op op, u64 target = 0) noexcept;
// A page's read, queued once per visit. Clears `failed` with the new request; false when already requested.
bool request_read(bool& requested, bool& failed, Op op) noexcept;
// A failed read's retry notice: reads again unless a request is in flight or MaiCord is offline.
void retry_read(bool& requested, bool& failed, Op op) noexcept;
void show_failure(Utf8View text) noexcept;
bool repaint() noexcept;
bool guard_dirty() noexcept;
void select_page(Page page) noexcept;
void request_close() noexcept;

// Drafts and editors.
const RoleRecord* role(u64 id) noexcept;
bool pending(const Picked& picked) noexcept;
void release_picked(Picked* picked) noexcept;
void set_editor(Editor index, const Utf8View& text) noexcept;
Utf8View editor_text(Editor index) noexcept;
void start_pick(Pick pick) noexcept;

// Projection helpers.
// A row's text. A string literal costs its address at the call site (the engine measures it); text made at run time
// is a Utf8View that outlives the call. Null is empty. A parameter type only: never kept.
struct Text {
    static constexpr usize kViewBit = 1ull << 63;
    usize bits;
    constexpr Text() noexcept : bits(0) {}
    Text(const char* literal) noexcept : bits(reinterpret_cast<usize>(literal)) {}
    Text(const Utf8View& view) noexcept : bits(reinterpret_cast<usize>(&view) | kViewBit) {}
};
Utf8View text_view(Text text) noexcept;
// Fills popup row `index` (enabled plus `flags`); the action is kMenuChoiceBase + action.
void popup_row(u32 index, Text label, u32 action, u32 flags = 0) noexcept;
// Opens the popup of the first `count` rows as menu `menu`, owned by `subject` (a guild, user or role id).
void open_popup(Menu menu, u64 subject, u32 count, const UiRect& anchor, u32 width) noexcept;
// A control with no choices or editor: an optional action button label.
u32 push_row(u64 id, Text label, Text description, UiSettingsControlKind kind, bool enabled, bool checked = false,
             Text action = {}, UiSettingsControlPresentation presentation = {}) noexcept;
// A switch row.
u32 push_toggle(u64 id, Text label, Text description, bool enabled, bool checked) noexcept;
// A row with a button labelled `action`.
u32 push_action(u64 id, Text label, Text description, Text action, bool enabled = true,
                UiSettingsControlPresentation presentation = {}) noexcept;
// A row with a choice list (choice, radio, swatches, tabs, buttons, segments) and the selected choice.
u32 push_choices(u64 id, Text label, Text description, UiSettingsControlKind kind, bool enabled,
                 const ConstSlice<Utf8View>& choices, u32 selected = 0, Text action = {},
                 UiSettingsControlPresentation presentation = {}) noexcept;
// The same over the first `shown` of `names` (each name followed by "\0", the literal's own end ending the list).
u32 push_names(u64 id, Text label, Text description, UiSettingsControlKind kind, bool enabled, const char* names,
               u32 shown, u32 selected = 0, UiSettingsControlPresentation presentation = {}) noexcept;
// A row of buttons over `names`.
u32 push_buttons(u64 id, Text label, Text description, bool enabled, const char* names, u32 shown,
                 UiSettingsControlPresentation presentation = {}) noexcept;
// A drop-down showing `current` (its one entry).
u32 push_choice(u64 id, Text label, Text description, bool enabled, Text current) noexcept;
// An edit or multiline row over the editor `index`; `action` labels the button beside the field.
u32 push_editor(u64 id, Text label, Text description, UiSettingsControlKind kind, bool enabled, Editor index,
                Text placeholder, Text action = {}) noexcept;
u32 push_category(u64 id, Text label, Text description = {}) noexcept;
// A plain notice row.
u32 push_notice(Text description, Text label = {}) noexcept;
// A critical notice; `action` labels its retry button.
u32 push_error(Text label, Text description, Text action = {}) noexcept;
// A notice with the loading spinner.
u32 push_loading(Text description, u64 id = control_notice) noexcept;
// The list heading "<title><count>".
u32 push_count_heading(Bank& bank, Text title, u32 count) noexcept;
Utf8View channel_label(Bank& bank, u64 channel_id, Text none) noexcept;
void image_summary(Bank& bank, const GuildImageInfo& info) noexcept;
Utf8View picked_text(Bank& bank, const Picked& picked, Text current) noexcept;
void relative_time(Bank& bank, u64 then_ms, u64 now_ms, bool future) noexcept;
Utf8View user_label(u64 id) noexcept;
Utf8View user_name(const GuildAdminUser& user) noexcept;
// Channels of `types` (a bit per channel type) in channel-list order behind a first row labelled `none` (null: no
// such row) and, with `create_row`, the "Create one for me" row; menu_channel holds the matching ids.
void open_channel_menu(Menu menu, u32 types, u64 current, Text none, PermissionSet required, const UiRect& anchor,
                       bool create_row = false) noexcept;

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
void profile_saved(bool ok, const Utf8View& body) noexcept;
// Server Profile: an edit of a trait label or the game search, the top-games
// read and its reply, the per-tick work (game search, the icon's colour), the
// page's content (sections beside the preview), the preview card placed at the
// aside (true when its height changed) and closed, and the first banner tile's
// colour (the icon's dominant one, 0xRRGGBB; 0 until read back).
void profile_edited(Editor editor, Utf8View text) noexcept;
bool top_games_request(GuildAdminRequest* request) noexcept;
void top_games_loaded(bool ok, const Utf8View& body) noexcept;
void profile_tick(u64 now) noexcept;
void profile_page_content(UiSettingsPageContent* content) noexcept;
bool profile_place_preview() noexcept;
void profile_close_preview() noexcept;
u32 profile_icon_color() noexcept;
// Server Tag: the save's body and the adoption, their outcomes, and the edits
// of the tag text.
bool tag_patch(GuildSettingsPatch* patch, GuildTagWrite* write, GuildTagColorText* colors) noexcept;
void tag_saved(Op op, bool ok, const Utf8View& body) noexcept;
// A refused save or adoption: Discord's own reason (a field error, the rate
// limit's wording) as the status.
void tag_failed(const RestClientResponse& response, const Utf8View& body) noexcept;
// A server profile read finished: the Server Tag page takes it unless it holds unsaved edits.
void tag_profile_loaded() noexcept;
void tag_edited(const Utf8View& text) noexcept;
// Host colours of the page's swatches: the sample names' and the custom colour pair's.
bool tag_color(u64 id, u32 choice, u32* rgb) noexcept;
// Boost Perks: the vanity read, the save's two requests and their outcomes.
void boost_vanity_loaded(const GuildAdminVanity* vanity) noexcept;
bool boost_vanity_patch(GuildSettingsPatch* patch) noexcept;
bool boost_guild_patch(GuildSettingsPatch* patch) noexcept;
void boost_saved(Op op, bool ok, const Utf8View& body, u64 now) noexcept;
void release_uri(u8** uri, u32* bytes) noexcept;

// Members: the search request of the current filters and page, its
// outcome, the debounced text and the bulk ban update.
bool members_search_request(GuildModerationRequest* request, GuildMemberSearchQuery* query) noexcept;
void members_search_sent() noexcept;
void members_search_response(const RestClientResponse& response, const Utf8View& body, u64 now) noexcept;
void members_tick(u64 now) noexcept;
void members_bulk_ban_update(const GuildBulkBanUpdate& update) noexcept;
void members_release() noexcept;
// Audit Log: queues older entries once their loading row is on screen.
bool audit_load_visible() noexcept;
// Bans: Enter in the search box, the ban search request and its reply.
void bans_editor_enter() noexcept;
bool bans_search_request(GuildModerationRequest* request) noexcept;
void bans_search_loaded(bool ok, const Utf8View& body) noexcept;
// Integrations (integrations.cpp) and AutoMod (automod.cpp): the request of
// their queued Op::apps or Op::automod, built as it is sent (false: nothing
// to send); whether it only reads; its reply. Their state is released with
// the layer.
bool apps_request(GuildAutomationRequest* request) noexcept;
bool apps_reading() noexcept;
void apps_response(const RestClientResponse& response, const Utf8View& body) noexcept;
void apps_release() noexcept;
bool automod_request(GuildAutomationRequest* request) noexcept;
bool automod_reading() noexcept;
void automod_response(const RestClientResponse& response, const Utf8View& body) noexcept;
void automod_release() noexcept;
// Their Gateway events (AutoMod rules; webhooks, integrations and command
// permissions): the open page reads its lists again.
void apps_gateway(const GatewayClientEvent& event, const Utf8View& body) noexcept;
void automod_gateway(const GatewayClientEvent& event, const Utf8View& body) noexcept;
// Safety Setup in community servers holds AutoMod (as the official client
// routes it): its entry row, and that row's activation (true when handled).
void automod_safety_entry(Bank& bank) noexcept;
bool automod_safety_activate(const SettingsLayerActivation& activation) noexcept;
// The App Directory sidebar link: Discord's app directory in the browser.
void open_app_directory() noexcept;
// Access, Community and Server Template (server_access.cpp): the queued
// Op::server_access request, built as it is sent on the community lane
// (*community_lane) or the guild admin one (false: nothing to send); whether
// it only reads; its reply. Their state is released with the layer.
bool server_access_request(GuildAdminRequest* admin, GuildSettingsPatch* patch, RolePatch* role,
                           GuildCommunityRequest* community, bool* community_lane) noexcept;
bool server_access_reading() noexcept;
void server_access_response(const RestClientResponse& response, const Utf8View& body) noexcept;
void server_access_release() noexcept;
// The Server Template sidebar entry's dot: kUiSettingsNavDot while the template has unsynced changes.
u16 server_template_nav_badge(Page page) noexcept;
// Reads the template once per visit to show that dot; false when nothing was queued.
bool server_template_ensure_status() noexcept;

// Text forms.
TextFormOwnerKey form_key(u64 kind, u64 item) noexcept;
bool form_valid(TextFormOwnerKey key) noexcept;
bool form_save(TextFormOwnerKey key, Utf8View) noexcept;
void open_expression_form(u64 kind, u64 item) noexcept;

} // namespace maicord::app_detail::server_settings

namespace maicord::app_detail::server_settings {
namespace guild_settings_detail {}
using namespace guild_settings_detail;
namespace guild_settings_detail {

bool read_op(Op op) noexcept;
Op next_save_op() noexcept;
Utf8View failure_text(const RestClientResponse& response) noexcept;
Utf8View failure_text(const RestClientResponse& response, const Utf8View& body) noexcept;
void ensure_page_data() noexcept;

// Channel creation from menus; independent of the open layer.
struct ChannelCreate {
    u64 serial, guild_id, parent_id, request_id, retry_at, account_scope;
    u16 name_bytes;
    u8 type;
    bool queued, done;
    Utf8View status;
    u32 revision;
    char name[kGuildAdminNameBytes];
};

extern ChannelCreate creation;
void poll_picker() noexcept;

} // namespace guild_settings_detail
} // namespace maicord::app_detail::server_settings
namespace maicord::app_detail {

using namespace server_settings;

} // namespace maicord::app_detail
