#pragma once
// Shared by channel_overview.cpp and its parts (channel_overview_*.cpp); private to them.
#include "channel_overview.hpp"
#include "settings/settings.hpp"
#include "../chat/polls/create.hpp"
#include "lifecycle.hpp"
#include "../account/profile_editor.hpp"
#include "../chat/report.hpp"
#include "community.hpp"
#include "../account/custom_status.hpp"
#include "../plugins/settings_dialog.hpp"
#include "../settings/modal.hpp"
#include "../shell/state.hpp"
#include "../account/account_presentation.hpp"
#include "../chat/composer/text_input.hpp"
#include "../chat/composer/text_form.hpp"
#include "maicord/app/guild/channel_overview_controller.hpp"
#include "maicord/model/guild/channel_settings_model.hpp"
#include "maicord/model/guild/permission_grid.hpp"
#include "maicord/model/chat/bounded_edit.hpp"
#include "maicord/core/text_bank.hpp"
#include "../account/account_access.hpp"

namespace maicord::app_detail {
namespace channel_overview_detail {}
using namespace channel_overview_detail;
namespace channel_overview_detail {

// Channel Settings: Overview (text channels through the overview controller,
// voice/stage/category through a channel PATCH), Permissions (overrides with
// allow / neutral / deny per permission) and Delete channel.
constexpr UiPageKey page_key{0x43484F5645525657ull, 1};
constexpr UiPageKey permissions_key{0x43484F5645525657ull, 2};

constexpr u32 kControlCapacity = 768;
constexpr u32 kBankBytes = 48 * 1024;
constexpr u32 kPopupCapacity = 320;

constexpr u64 kEchoMs = 8'000;

enum class Op : u8 {
    none,
    fetch,
    update,
    overwrites,
    remove
};
enum class Menu : u8 {
    none,
    slowmode,
    target
};
struct Overview : SettingsModal {
    ChannelOverviewController controller;
    UiPage pages[2];
    UiSettingsPageContent content, permissions_content;
    UiSettingsNavEntry nav[6];
    u32 nav_count, nav_revision;
    UiSettingsControl controls[kControlCapacity];
    UiSettingsControlPresentation presentation[kControlCapacity];
    UiSettingsAvatarMedia avatar_media[kControlCapacity];
    u32 control_count;
    UiSettingsEditor editors[2];
    UiSettingsFooter footer;
    MessageComposer models[2];
    BoundedEdit bindings[2];
    u64 lifetime, projected_revision;
    bool permission_observed;
    // Channel identity and capabilities.
    ChannelOverviewScope identity;
    u8 type;
    bool text, can_overview, can_permissions, can_delete;
    // Voice, stage and category overview.
    ChannelSettingsDraft base, draft;
    bool loaded, fetch_failed, conflict, echo;
    u64 echo_until;
    // Permission overrides.
    PermissionOverwriteDraft overwrite_base, overwrite_draft;
    u64 selected_target;
    bool overwrite_conflict, overwrite_echo;
    u64 overwrite_echo_until;
    // Transport for everything except the text overview.
    Op op;
    bool queued, failed;
    u64 request_id, retry_at;
    Utf8View status;
    Menu menu;
    u64 menu_ids[kPopupCapacity];
    u8 menu_types[kPopupCapacity];
    PopupItem popup_items[kPopupCapacity];
    char popup_text[24 * 1024];
    char header[kGuildAdminNameBytes + 4];
    char banks[2][kBankBytes];
    u32 bank_index;
    Utf8View user_limit_labels[kChannelSettingsUserLimitMaximum + 1];
    char user_limit_text[1'200];
    Utf8View bitrate_labels[16];
    // Packed RGBA accents of override rows; the last slot is the implicit @everyone.
    u32 row_accent[kPermissionOverwriteDraftMaximum + 1];
    u64 palette_revision;
    u32 bitrate_values[16];
    u32 bitrate_count;
    char bitrate_text[160];
};

extern Overview* owner;
extern u64 lifetime;

using Bank = TextBank;

const ChatStateStore& state() noexcept;
ChannelOverviewScope scope(u64 id) noexcept;
bool manage(u64 id) noexcept;
bool manage_channel(u64 id) noexcept;
bool manage_overrides(u64 id) noexcept;
bool available() noexcept;
bool editable() noexcept;
using Editors = SettingsEditors<&owner, editable>;
bool import_models() noexcept;
Utf8View status() noexcept;
bool project() noexcept;
bool repaint() noexcept;
void build_navigation() noexcept;
void load_overwrites() noexcept;
void queue(Op op) noexcept;
bool form_valid(TextFormOwnerKey key) noexcept;
void activate_hook(SettingsLayer*, const SettingsLayerActivation& activation) noexcept;
void choose_hook(SettingsLayer*, u32 action) noexcept;
SettingsLayerEditor editor_hook(SettingsLayer*, u64 control) noexcept;
bool edited_hook(SettingsLayer*, u64 control) noexcept;
void close_hook(SettingsLayer*) noexcept;

constexpr SettingsLayerHooks kHooks{editor_hook, edited_hook, activate_hook, choose_hook, close_hook,
                                    settings_hook<repaint>, nullptr, settings_hook<editable>};

bool reconcile() noexcept;

// The modals this host stacks, in the order they take precedence. The last four entries are null for a
// modal whose requests do not go through this host (Custom Status uses the PRIMARY lane).
struct HostedModal {
    bool (*active)() noexcept;
    void (*close)(bool restore_focus) noexcept;
    void (*release)() noexcept;
    bool (*synchronize)() noexcept;
    SettingsLayer* (*layer)() noexcept;
    u64 (*caret_wake)() noexcept;
    u64 (*next_wake)() noexcept;
    bool (*submit)(u64 now) noexcept;
    bool (*owns_response)(u64 request_id) noexcept;
    bool (*response)(const RestClientResponse&, Utf8View) noexcept;
};
#define MAICORD_HOSTED_MODAL(name) \
    {application_##name##_active, application_##name##_close, application_##name##_release, \
     application_##name##_synchronize, application_##name##_settings_layer, application_##name##_caret_wake, \
     application_##name##_next_wake, application_##name##_submit, application_##name##_owns_response, \
     application_##name##_response}
constexpr HostedModal kModals[] = {
    MAICORD_HOSTED_MODAL(guild_settings),
    MAICORD_HOSTED_MODAL(poll_create),
    MAICORD_HOSTED_MODAL(guild_lifecycle),
    MAICORD_HOSTED_MODAL(profile_editor),
    MAICORD_HOSTED_MODAL(guild_community),
    MAICORD_HOSTED_MODAL(report),
    {application_custom_status_active, application_custom_status_close, application_custom_status_release,
     application_custom_status_synchronize, application_custom_status_settings_layer,
     application_custom_status_caret_wake, nullptr, nullptr, nullptr, nullptr},
    {application_plugin_settings_active, application_plugin_settings_close, application_plugin_settings_release,
     application_plugin_settings_synchronize, application_plugin_settings_settings_layer,
     application_plugin_settings_caret_wake, nullptr, nullptr, nullptr, nullptr},
};
#undef MAICORD_HOSTED_MODAL

const HostedModal* active_modal() noexcept;

} // namespace channel_overview_detail
} // namespace maicord::app_detail
