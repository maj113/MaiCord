#pragma once

#include "plugins/plugins.hpp"
#include "chat/composer/text_form.hpp"
#include "settings/custom_theme.hpp"
#include "maicord/ui/chat/messages/author.hpp"

namespace maicord {
namespace settings_runtime_test {
using namespace app_detail;

extern ApplicationCustomThemeView custom_theme_projection;

extern ApplicationCustomThemeAction custom_theme_last_action;

extern u32 custom_theme_actions;

extern bool custom_theme_pointer_transition;

// The installed-themes dropdown the settings stubs report: a long list for the choice runtime test. selected is
// the last index the controller chose; dismissed says whether the popup and its borrowed labels were already
// gone when it did; invalidate makes the choice invalidate the account scope from inside the callback.
extern ConstSlice<Utf8View> installed_themes;
struct InstalledSelection {
    u32 calls, last;
    bool dismissed, invalidate;
};
extern InstalledSelection installed_selection;

struct Observations {
    UiRect viewport{0, 0, 1000, 700};
    u32 text_builds, layout_updates, account_text_updates, forced_account_text_updates, suspends,
        drop_updates, member_updates, navigation_updates;
    u32 resize_sync_presents, resize_finishes, resize_finish_failures;
    bool resize_finish_failed;
    bool fail_text;
    u32 fail_text_remaining;
    u32 profile_opens;
    u32 account_menu_opens;
    UiWidgetHandle account_menu_source;
    UiRect account_menu_anchor;
    bool friends_active, requests_active;
    u32 opened_urls;
    u32 notifications_refreshes, notifications_setups;
    u32 content_hits, text_hits, profile_hits;
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

} // namespace settings_runtime_test
} // namespace maicord
