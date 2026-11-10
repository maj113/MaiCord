#pragma once
// The Access, Enable Community / Community Overview and Server Template pages'
// shared state and requests. UI thread only.
#include "page_common.hpp"
#include "maicord/model/guild/community_settings.hpp"

namespace maicord::app_detail::server_settings {

// One request of these pages, in flight or queued under Op::server_access.
enum class AccessStep : u8 {
    none,
    // Access: the reads, then a save's parts in order.
    verification_get,
    requirements_get,
    categories_get,
    metadata_get,
    // The submitted applications' total, read while manual approval is on.
    join_requests_get,
    content_level,
    leave_discovery,
    verification_update,
    discovery_guild,
    metadata_update,
    profile_visibility,
    // Discovery subcategories, added and removed at once (not with the save).
    subcategory_add,
    subcategory_remove,
    // Enable Community and Community Overview.
    everyone_role,
    community_enable,
    overview_update,
    // Server Template.
    templates_get,
    template_create,
    template_sync,
    template_update,
    template_delete,
};

// Enable Community's steps; the safety and defaults steps show only when
// something falls short.
enum class SetupStep : u8 { none, intro, safety, basics, defaults, rules };

// The popup a page opened under Menu::server_access.
enum class AccessMenu : u8 { none, rules_channel, updates_channel, safety_channel, locale, category, subcategory };

inline constexpr u32 kDiscoveryCategoryCapacity = 64;
inline constexpr u32 kAccessBodyBytes = kGuildCommunityMaximumBodyBytes;

// Rows of these pages below kRowBase (control_server_access and up).
enum AccessControl : u64 {
    access_join = control_server_access,
    access_age,
    access_require_rules,
    access_rule_add,
    access_question_add,
    access_question_examples,
    access_discovery_category,
    access_discovery_tags,
    access_discovery_emoji,
    access_locale,
    community_enable_button,
    community_wizard,
    community_rules_channel,
    community_updates_channel,
    community_safety_channel,
    community_agree,
    community_disable,
    // The template fields' own error rows (the fields are control_name and control_description).
    template_title_error,
    template_description_error,
    template_create_button,
    template_link,
    template_actions,
    access_subcategory_add,
    access_apply_profile,
    access_discovery_banner,
    access_preview,
    // List rows: a rule, a question or a subcategory, by index.
    access_rule_row = control_server_access + 64,
    access_question_row = control_server_access + 96,
    access_subcategory_row = control_server_access + 120,
};

struct ServerAccessState {
    u64 lifetime;
    AccessStep step;
    AccessMenu menu;
    // Text form scratch (initial texts of the open form).
    char form_text[2048];
    // A request body, built as the request is sent.
    u8 body[kAccessBodyBytes];

    // Access.
    GuildVerificationForm form;
    GuildAccessForm form_base, form_draft;
    // The features and content level the drafts started from (the save's
    // expected result until the server update confirms it).
    u64 features_base;
    bool age_base;
    // A question being written: its slot, type and label until its answers are typed.
    u8 question_index;
    GuildVerificationFieldType question_type;
    u16 question_label_bytes;
    char question_label[kGuildAccessLabelBytes];
    bool form_requested, form_loaded, form_failed;
    GuildJoinType join_base, join_draft;
    bool require_rules_draft, age_draft;
    GuildVerificationBulkAction bulk_action;
    // Applications still waiting (Discord asks what becomes of them only when some do); 0 until read.
    u32 pending_requests;
    bool requests_requested;
    // The Preview dialog's text.
    char preview_text[8192];
    // What the save still has to send (AccessStep bits from content_level).
    u32 save_parts;
    GuildDiscoveryRequirements requirements;
    bool requirements_requested, requirements_loaded, requirements_failed;
    GuildDiscoveryCategory categories[kDiscoveryCategoryCapacity];
    u32 category_count;
    bool categories_requested, categories_loaded;
    // The metadata read is kept whole: its PATCH repeats it.
    GuildDiscoveryMetadata metadata_base, metadata_draft;
    char metadata_body[kGuildDiscoveryMetadataMaximumBytes];
    u32 metadata_body_bytes;
    bool metadata_requested, metadata_loaded, metadata_failed;
    // The subcategory being added or removed.
    u32 subcategory_target;
    // Apply from Profile: the profile visibility the toggle asks for, once touched.
    bool apply_from_profile, apply_from_profile_set;
    // The Discovery banner is to be removed (a new one waits in Settings::splash).
    bool discovery_splash_clear;
    // The discoverable server's description and language.
    GuildCommunityOverview discovery_base, discovery_draft;

    // Enable Community.
    SetupStep setup;
    GuildCommunitySetup community;
    bool policy_accepted;
    PermissionSet everyone_permissions;
    // Community Overview.
    GuildCommunityOverview overview_base, overview_draft;

    // Server Template.
    GuildTemplate guild_template;
    bool template_exists, templates_requested, templates_loaded, templates_failed;
};

// The pages' state, made on the first visit of one of them (null when
// memory ran out); it follows the open server.
ServerAccessState* server_access_state() noexcept;
// Queues one request of these pages.
void server_access_queue(AccessStep step) noexcept;

// Per page: build one step, read its reply.
bool access_build(AccessStep step, GuildSettingsPatch* patch, GuildCommunityRequest* community) noexcept;
void access_response(AccessStep step, const RestClientResponse& response, const Utf8View& body) noexcept;
bool community_build(AccessStep step, GuildSettingsPatch* patch, RolePatch* role) noexcept;
void community_response(AccessStep step, const RestClientResponse& response, const Utf8View& body) noexcept;
bool template_build(AccessStep step, GuildCommunityRequest* community) noexcept;
void template_response(AccessStep step, const RestClientResponse& response, const Utf8View& body) noexcept;

// Channel and language popups shared by Access and Community Overview: text
// channels in channel-list order after the optional None and "Create one for
// me" rows; menu_channel holds each row's id (0 for None, 1 to create).
void access_open_channels(AccessMenu menu, u64 current, bool none_row, bool create_row, const UiRect& anchor) noexcept;
void access_open_locales(Utf8View current, const UiRect& anchor) noexcept;
// A channel choice's label: None, Create one for me or #name.
Utf8View access_channel_label(Bank& bank, u64 channel_id) noexcept;
// A language's name; "Choose a language" for none.
Utf8View access_locale_label(Utf8View code) noexcept;
// The server's description as the Gateway last sent it.
Utf8View access_guild_description() noexcept;
// The server's complete features list for a PATCH; false while it is not kept.
bool access_feature_names(ConstSlice<u8>* names, u16* count) noexcept;
// Whether the features list is kept, for a page that only has to know it can write one.
bool access_features_known() noexcept;
// The description editor's text into `overview`; the language chosen from the language popup into it.
void access_sync_description(GuildCommunityOverview* overview) noexcept;
void access_choose_locale(GuildCommunityOverview* overview, u32 index) noexcept;
// A text form of these pages: its key, validity and the page that answers it.
enum AccessForm : u64 {
    form_access_rule = 0x41434301,
    form_access_question,
    form_access_choices,
    form_access_tags,
    form_access_bulk,
    form_access_preview,
    form_community_disable,
    form_template_delete,
};

} // namespace maicord::app_detail::server_settings
