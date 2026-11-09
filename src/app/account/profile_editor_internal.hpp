#pragma once
// Shared by profile_editor.cpp and its parts (profile_editor_*.cpp); private to them.
#include "account_presentation.hpp"
#include "../settings/modal.hpp"
#include "../shell/state.hpp"
#include "../chat/composer/text_form.hpp"
#include "../settings/catalog.hpp"
#include "maicord/model/user/profile_editor.hpp"
#include "maicord/platform/desktop/file_picker.hpp"
#include "maicord/model/chat/bounded_edit.hpp"
#include "maicord/ui/account/profile_dialog_scene.hpp"
#include "maicord/ui/account/profile_scene.hpp"


#include <stddef.h>
#include "maicord/app/account/account_profile_edit.hpp"
#include "maicord/app/media/local_draft_preview.hpp"

namespace maicord::app_detail {
namespace profile_editor_detail {}
using namespace profile_editor_detail;
namespace profile_editor_detail {

constexpr UiPageKey kProfilesPage{kSettingsNativeOwner, static_cast<u64>(SettingsPanel::profiles) + 1};
constexpr u32 kPageCapacity = 64;
constexpr u32 kControlCapacity = 40;
constexpr u32 kNameBytes = kProfileEditDisplayNameCodepoints * 4;
constexpr u32 kPronounsBytes = kProfileEditPronounsCodepoints * 4;
constexpr u32 kBioBytes = kProfileBioMaximumUtf8Bytes;
constexpr u32 kGuildChoices = 100;
constexpr u32 kOwnedCapacity = 96;

// The official client's bio box is 136 DIP tall; the rest is the settings default.
constexpr UiSettingsStyle kEditorStyle{12, 40, 136};

enum : u64 {
    control_tabs = 1,
    control_try,
    control_guild,
    control_name,
    control_name_error,
    control_pronouns,
    control_pronouns_error,
    control_nitro_note,
    control_name_style,
    control_name_effect,
    control_name_colors,
    control_name_style_error,
    control_avatar,
    control_avatar_error,
    control_decoration,
    control_effect,
    control_nameplate,
    control_nameplate_error,
    control_accent,
    control_banner,
    control_banner_error,
    control_theme,
    control_bio,
    control_bio_error,
    control_misc_error,
    control_status,
    control_status_note,
};
enum Editor : u32 { editor_name, editor_pronouns, editor_bio, editor_count };
enum class Op : u8 { none, fetch, collectibles, product, save_account, save_profile };
enum class Pick : u8 { none, avatar, banner };
enum class Dialog : u8 { none, crop, decoration, effect, nameplate };
// Settings popups this page opens; their actions are the choice's index + 1 above the popup's base.
enum : u32 { popup_guild = 0, popup_font = 1000, popup_effect = 2000 };

// A picked image: file bytes, what the module reported, its crop and its
// local preview (thumbnail in the preview atlas), and the data URI made
// when a save starts.
struct Picked {
    u8* data;
    u32 bytes;
    ProfileImageInfo info;
    ImageCropState crop;
    AccountMediaLocalPreviewRequest request;
    void* instance;
    u64 poll_at;
    bool admitted, ready, failed;
    char description[kProfileEditDescriptionBytes];
    u32 description_bytes;
    u8* uri;
    u32 uri_bytes;
    // MD5 of the original file the URI was cropped from (X-Discord-Original-MD5).
    bool has_md5;
    char md5[32];
    u32 path_units;
    // Last: moves copy only the used part of the path.
    wchar_t path[kNativeFilePickerMaximumPathUnits + 1];
};

struct Owner : SettingsModal {
    UiPage pages[kPageCapacity];
    UiSettingsPageContent content, other;
    UiSettingsControl controls[kControlCapacity];
    UiSettingsControlPresentation presentation[kControlCapacity];
    u32 control_count;
    UiSettingsEditor editors[editor_count];
    UiSettingsFooter footer;
    MessageComposer models[editor_count];
    BoundedEdit bindings[editor_count];
    char name[kNameBytes], pronouns[kPronounsBytes], bio[kBioBytes];
    Utf8View tab_choices[2];
    Utf8View guild_choices[kGuildChoices];
    u64 guild_ids[kGuildChoices];
    u32 guild_choice_count;
    Utf8View avatar_buttons[2], decoration_buttons[2], effect_buttons[2], banner_buttons[2], nameplate_buttons[2];
    Utf8View theme_choices[2], accent_choices[1], name_color_choices[kProfileNameStyleColors], try_buttons[2];
    Utf8View font_choices[kProfileNameFontCount], effect_choices[kProfileNameEffectCount];
    ProfileEditorTab tab;
    u64 guild_id;
    u16 guild_generation;
    // Saved values and the full profile of each tab (the preview's base).
    ProfileEditSaved saved;
    UserProfile base_profile, preview_profile;
    ProfileEditorDraft draft;
    ProfileEditorBase base;
    ProfileCollectibleRef base_collectibles[kProfileEditCollectibleMaximum];
    bool loaded, load_failed;
    // Try It Out (no Nitro): Nitro fields are editable for the preview only; Save needs them back to their saved values.
    bool trying;
    // Pending images and the one being cropped.
    Picked avatar, banner, staged;
    Pick staged_use;
    NativeFilePicker picker;
    Pick picking;
    bool picker_consumed;
    // Decorations/effects owned (from collectibles-purchases) and the asset
    // of the drafted decoration.
    ProfileCollectible owned[kOwnedCapacity];
    u32 owned_count;
    bool collectibles_loaded, collectibles_failed;
    ProfileString<64> decoration_asset;
    ProfileString<128> nameplate_asset;
    // The preview card and its profile effect.
    ProfileScene preview;
    ProfileEffect effect;
    u64 effect_loaded, effect_wanted;
    bool preview_open;
    // The modal dialog.
    UiProfileDialogScene dialog;
    Dialog dialog_kind;
    UiProfileDialogItem items[kUiProfileDialogItemCapacity];
    u32 item_count, item_selected;
    ImageCropState crop;
    bool crop_dragging, zoom_dragging;
    float drag_x, drag_y, drag_offset_x, drag_offset_y;
    // Requests: GET loads, then the two sequential PATCHes of a save.
    Op op;
    bool queued, failed, saving;
    u64 request_id, retry_at, lifetime, account_scope, profile_id, self_user_id;
    UserPatch user_patch;
    MemberEditPatch member_patch;
    ProfilePatch profile_patch;
    ProfileEditErrors errors;
    Utf8View status;
    // Backs `status` while it says a picked image is over the size limit.
    char size_error[192];
    u32 preview_serial;
    u32 structurals[32 * 1024];
    char banks[2][4096];
    u32 bank_index;
};

extern Owner* owner;
extern u64 lifetime_serial;
bool connected() noexcept;
ChatStateStore& state() noexcept;
bool scope_current() noexcept;
bool busy() noexcept;
bool saving() noexcept;
template <u32 N> void assign(ProfileString<N>* to, const Utf8View& from) noexcept {
    to->bytes = static_cast<u16>(utf8_copy_truncated(to->data, from));
}
bool editable() noexcept;
using Editors = SettingsEditors<&owner, editable>;
// What the account may save, and what the page lets it edit (Try It Out adds the Nitro fields).
ProfileEditorGate gate() noexcept;
ProfileEditorGate shown_gate() noexcept;
char* editor_buffer(u32 index) noexcept;
u32 editor_capacity(u32 index) noexcept;
void load_editor(u32 index, const Utf8View& text) noexcept;
u32 editor_of(u64 control) noexcept;
void release_picked(Picked& picked) noexcept;
ProfileEditorDraft current_draft() noexcept;
u16 changes() noexcept;
void discard() noexcept;
u32 effective_theme(u32* out) noexcept;
bool request_preview(Picked& picked, const FileSnapshot& snapshot) noexcept;
Picked* picked_for(MediaAssetKey key) noexcept;
bool pump_previews(u64 now) noexcept;
void build_items(Dialog kind) noexcept;
bool palette_resolve(void*, u64 control, u32 choice, BackgroundTheme* out) noexcept;
bool project() noexcept;
bool repaint() noexcept;
void queue(Op op) noexcept;
void queue_fetch() noexcept;
bool dialog_update() noexcept;
bool refresh_dialog() noexcept;
void close_dialog() noexcept;
void open_picker_dialog(Dialog kind) noexcept;
void apply_dialog() noexcept;
Utf8View image_error(ProfileImageError error) noexcept;
void describe(Picked& picked) noexcept;
void poll_picker() noexcept;
bool color_valid(TextFormOwnerKey key) noexcept;
bool guard() noexcept;
void select_guild(u32 index) noexcept;
void choose_option(u32 action) noexcept;
void collect_guilds() noexcept;
void activate(SettingsLayer*, const SettingsLayerActivation& activation) noexcept;

} // namespace profile_editor_detail
} // namespace maicord::app_detail
