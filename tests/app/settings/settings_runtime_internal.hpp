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

// The installed themes the settings stubs report (Appearance's "Your themes" and the Themes page). calls/last/preview
// record what the controller applied; active is the installed theme in use (~0u none); builtin counts Use built-in.
extern ConstSlice<ClientThemeCatalogEntry> installed_themes;
struct InstalledSelection {
    u32 calls, last, active = ~0u, builtin;
    bool preview;
};
extern InstalledSelection installed_selection;
// Calls into the custom theme owner: built-in preference changes and the editor's document background edits.
extern u32 theme_preference_changes, theme_background_edits;

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
    // Accessibility: theme republications (Text size in chat, Saturation) and Preview's speech.
    u32 theme_refreshes, speeches;
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
