#pragma once
// Shared by settings.cpp and its parts (settings_*.cpp); private to them.
#include "settings.hpp"
#include "../shell/state.hpp"
#include "custom_theme.hpp"
#include "catalog.hpp"
#include "../voice/settings.hpp"
#include "maicord/app/account/login/oauth_sign_in.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/render/d3d11/layer_presentation.hpp"
#include "maicord/render/text/edit_layout.hpp"
#include "../account/account_ui_runtime.hpp"

namespace maicord::app_detail {
namespace settings_detail {}
using namespace settings_detail;

// The page a choice popup was opened on and the revisions it was projected under: any change retires it.
struct ApplicationSettingsToken {
    UiPageKey page;
    u32 registry_revision, content_revision;
};
struct ApplicationSettingsChoice {
    ApplicationSettingsToken token;
    u64 control_id, account_serial, profile_id;
    UiRect anchor;
    u32 first, control_index, choice_count, popup_opens, pressed_action;
    u16 popup_generation;
    bool active;
};

// Native panels and search share one registry.
inline constexpr u32 kSettingsPageCapacity = 24;
// One bank serves any catalog panel and search results. Keybinds, which lists every default shortcut, projects
// into its own bank (application_keybinds_settings_bank).
inline constexpr u32 kSettingsControlCapacity = 40;

// Only short pages carry avatar media (the Account card); longer ones have none.
inline constexpr u32 kSettingsAvatarRows = 12;

inline constexpr u32 kSettingsSearchBytes = 40;
inline constexpr u32 kSettingsSearchPage = kSettingsPanelCount + 1;
inline constexpr u64 kSettingsPanelLinkFirst = 0x7F000000u;
struct ApplicationSettings {
    UiSettingsScene scene;
    UiPageRegistry registry;
    UiPage pages[kSettingsPageCapacity];
    // One selected native page borrows this descriptor bank. Native panels and
    // search results never borrow it at the same time.
    UiSettingsControl controls[kSettingsControlCapacity];
    UiSettingsControlPresentation presentation[kSettingsControlCapacity];
    UiSettingsAvatarMedia avatar_media[kSettingsAvatarRows];
    u64 custom_theme_revision;
    u32 native_page;
    u64 pressed_account_serial, pressed_profile;
    UiSettingsPageContent content[kSettingsSearchPage];
    // The search field edits search_text through the shared text model, like
    // every other single-line field; search_bytes is the applied query.
    char search_text[kSettingsSearchBytes];
    u32 search_bytes, search_revision;
    MessageComposer search_edit;
    bool search_selecting;
    // A press on the scrim around the modal; releasing it there closes.
    bool backdrop_pressed;
    u64 search_return, flash_started_ms, entrance_started_ms, entrance_opens;
    UiWidgetManager* previous_focus;
    UiSettingsTarget pressed;
    u32 pressed_revision;
    bool previous_composer_focus, save_failed;
    bool notifications_active, notifications_settings_failed;
    bool scrollbar_dragging, scrollbar_sidebar;
    bool slider_dragging;
    u16 slider_generation;
    u32 preference_key;
    ClientPreferences slider_baseline;
    UiSettingsTarget slider_target;
    bool slider_preview;
    UiRect slider_viewport;
    // Pointer offset from the grabber's centre when the drag started on it.
    float slider_grab;
    float scrollbar_grab;
    ApplicationSettingsChoice choice;
};

namespace settings_detail {

struct MediaAccount {
    OAuthSignIn sign_in{};
    u64 generation{};
    char client[kOAuthClientBytes]{};
    u32 client_bytes{};
    bool launched{}, stopping{}, checked{}, saved{};
    Utf8View message{};
    Utf8View client_id() const noexcept { return {client, client_bytes}; }
};

extern MediaAccount g_spotify_account;

constexpr auto provider = OAuthProvider::spotify;

Utf8View saved_account_message() noexcept;
void media_close(MediaAccount& account) noexcept;
bool media_account_action() noexcept;

// Settings opened before sign-in: it borrows an account UI runtime for its
// text and measurement (released again unless an account started meanwhile)
// and covers the sign-in form until it closes.
struct SignedOutSession {
    bool active;
    bool created_runtime;
    // Closed: the next frame returns the runtime and the sign-in form, outside
    // any caller still holding the settings owner.
    bool ending;
};

extern SignedOutSession g_signed_out;
bool settings_allowed() noexcept;
bool page_allowed(const UiPageKey& page) noexcept;

constexpr u64 kNativeSettingsOwner = kSettingsNativeOwner;
constexpr u64 kPluginsPage = static_cast<u64>(SettingsPanel::plugins) + 1;
constexpr u64 kThemesPage = static_cast<u64>(SettingsPanel::themes) + 1;
constexpr u64 kEffectsPage = static_cast<u64>(SettingsPanel::effects) + 1;
// Appearance's "Your themes" palette shows up to this many installed themes, then its Browse themes tile
// (the palette control holds 12 options).
constexpr u32 kInstalledThemeTiles = 11;
// The installed theme a tile shows: the first ones by file name, with the active theme always among them.
u32 installed_theme_at(u32 tile) noexcept;

bool theme_panel(const UiPageKey& page) noexcept;
bool option_row(UiSettingsControlKind kind) noexcept;
bool theme_binding(SettingBinding binding) noexcept;
const UiSettingsSchemaNode* control_node(u64 id, SettingBinding* binding = nullptr) noexcept;
BackgroundTheme gradient_edit_base() noexcept;
bool persist() noexcept;
void project(ApplicationSettings& settings) noexcept;
bool begin_signed_out() noexcept;
void end_signed_out() noexcept;
void finish_signed_out() noexcept;
ApplicationSettings* ensure() noexcept;
bool recover_rejected_page(ApplicationSettings& settings) noexcept;
bool refresh() noexcept;
bool update_scene(ApplicationSettings& settings, const UiRect& viewport) noexcept;
bool resolve_palette(void*, u64 control, u32 choice, BackgroundTheme* output) noexcept;
void close_choice() noexcept;
const UiSettingsControl* choice_control(const ApplicationSettingsChoice& choice) noexcept;
bool choice_popup_current(const ApplicationSettingsChoice& choice) noexcept;
bool project_choice(ApplicationSettingsChoice choice) noexcept;
void apply_search(ApplicationSettings& settings) noexcept;
void set_search(ApplicationSettings& settings, const Utf8View& text) noexcept;
bool navigate(ApplicationSettings& settings, const UiPageKey& page, u64 control_id) noexcept;
bool open_result(ApplicationSettings& settings, u64 control_id) noexcept;
bool searching(const ApplicationSettings& settings) noexcept;
bool settled(ApplicationSettings& settings, bool result) noexcept;
bool activate(const UiSettingsTarget& target, bool reverse, bool picker = false, u32 selected = ~0u,
              bool save = true) noexcept;
bool choice_action(u32 action, const ApplicationSettingsChoice* captured = nullptr) noexcept;
bool search_character(ApplicationSettings& settings, u32 code) noexcept;
bool search_key(ApplicationSettings& settings, u32 key, bool control, bool shift) noexcept;
bool search_point(ApplicationSettings& settings, float x, float y, bool extend) noexcept;
void settings_glide_to(bool sidebar, float target) noexcept;
void settings_glide_reveal(float before) noexcept;

} // namespace settings_detail
} // namespace maicord::app_detail
