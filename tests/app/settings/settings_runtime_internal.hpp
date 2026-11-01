#pragma once

#include "plugins/popover.hpp"
#include "chat/composer/text_form.hpp"
#include "settings/custom_theme.hpp"
#include "maicord/platform/desktop/file_picker.hpp"
#include "../../fixtures/native_plugin_fixture.hpp"
#include "maicord/ui/chat/messages/author.hpp"

namespace maicord {
namespace settings_runtime_test {
using namespace app_detail;

extern ApplicationCustomThemeView custom_theme_projection;

extern ApplicationCustomThemeAction custom_theme_last_action;

extern u32 custom_theme_actions;

extern bool custom_theme_pointer_transition;

constexpr u64 fixture_owner = 0x4D43504649585432ull;
using native_plugin_fixture::FixtureAudit;
using native_plugin_fixture::Configure;
struct Observations {
    UiRect viewport{0, 0, 1000, 700};
    u32 text_builds, layout_updates, account_text_updates, forced_account_text_updates, suspends,
        drop_updates, member_updates;
    u32 resize_sync_presents, resize_finishes, resize_finish_failures;
    bool resize_finish_failed;
    bool fail_text;
    u32 fail_text_remaining;
    NativePluginModule* detaching_module;
    FixtureAudit* audit;
    bool detached_live_code, detached_live_state, detached_active_scene, detached_live_menu_labels;
    u32 profile_opens;
    u32 account_menu_opens;
    UiWidgetHandle account_menu_source;
    UiRect account_menu_anchor;
    bool friends_active, requests_active;
    u32 opened_urls;
    u32 notifications_refreshes, notifications_setups;
    u32 content_hits, text_hits, profile_hits;
    bool detached_active_popup, detached_popup_labels;
};
extern Observations observed;

struct GradientFormBoundary {
    ApplicationTextFormRequest request;
    char initial[128];
    u32 opens;
    bool active;
};
extern GradientFormBoundary gradient_form;

struct MenuFixture {
    PopupItem items[16];
    PopupRowWidget rows[16];
    UiWidget widgets[34];
    UiLayoutNode nodes[34];
    UiFriendsScene friends, requests;
    UiFriendsRow row, requests_row;
    UiFriendsSource source, requests_source;
    MessageRecord records[4];
    u32 order[4];
    u8 text[256];
};
extern MenuFixture menu_fixture;

struct PickerRuntime {
    ConstSlice<wchar_t> path;
    NativeFilePickerResult result;
};
struct PickerBoundary {
    ConstSlice<wchar_t> next_path;
    u32 starts, releases, cancellations, path_reads, release_timeouts;
};
extern PickerBoundary picker_boundary;

} // namespace settings_runtime_test
} // namespace maicord
