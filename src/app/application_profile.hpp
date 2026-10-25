#pragma once
#include "maicord/model/message_composer.hpp"
#include "maicord/protocol/user_social.hpp"
#include "maicord/ui/profile_scene.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/ui/button.hpp"

namespace maicord {
struct RestClientResponse;
} // namespace maicord
namespace maicord::app_detail {
enum class ProfileAnchor : u8 {
    point,
    member,
    account
};
enum class ProfileRoleChange : u8 {
    none,
    add,
    remove
};
enum class ProfileRequest : u8 {
    profile,
    effect,
    roles,
    applications,
    metadata
};
inline constexpr u32 kProfileApplicationCapacity = 16;
struct ApplicationProfile {
    ProfileAnchor anchor_kind;
    MessageComposer message, pending_message;
    u64 pending_recipient;
    u32 pending_message_revision;
    u64 message_user, message_blink_at;
    bool message_dragging, message_pending;
    UserProfile data;
    ProfileEffect effect;
    // The open profile menu (Edit Profile, Friend Actions or the role picker).
    u64 menu_user, menu_guild;
    u64 menu_roles[100];
    u32 menu_role_count;
    PopupItem menu_items[100];
    u64 requested_effect;
    bool effect_pending;
    ProfileScene scene;
    u64 requested_user, requested_guild, loaded_at, retry_at;
    u64 mutation_role;
    ProfileRoleChange mutation;
    ProfileRequest submitted_request;
    bool pending;
    // Public applications whose icons stand in for missing activity art,
    // fetched once per session (oldest replaced first); the batch in flight.
    ProfileApplication applications[kProfileApplicationCapacity];
    u32 application_count, application_next;
    u64 requested_applications[kUserActivityMaximum];
    u32 requested_application_count;
    // A custom activity button waiting for its URL (activity metadata).
    bool metadata_pending;
    u8 metadata_button;
    u64 metadata_user, metadata_application;
    ProfileString<64> metadata_session;
    // The note field: its editor while editing and the text the card shows.
    MessageComposer note;
    u64 note_user;
    u32 note_text_revision;
    char note_display[kUserNoteMaximumBytes];
};
bool application_profile_open(u64 user, UiRect anchor, ProfileAnchor kind = ProfileAnchor::point) noexcept;
// Edit Profile for the signed-in user: the profile editor's Main Profile.
bool application_profile_open_edit(u64 user, UiRect anchor, ProfileAnchor kind = ProfileAnchor::account) noexcept;
// The open popout for user becomes the full profile; false when not shown.
bool application_profile_expand(u64 user) noexcept;
// Popup rows of the profile card's menus.
bool application_profile_menu_action(u32 action) noexcept;
bool application_profile_menu_activate(u32 action) noexcept;
bool application_profile_close() noexcept;
void application_profile_release() noexcept;
bool application_profile_pointer(UiButtonPointerEvent event, UiPoint point) noexcept;
bool application_profile_double_click(UiPoint point) noexcept;
bool application_profile_key(u32 key, bool down, bool repeated) noexcept;
bool application_profile_character(u32 character) noexcept;
void application_profile_tick(u64 now) noexcept;
bool application_profile_conversation_ready() noexcept;
bool application_profile_refresh_message() noexcept;
bool application_profile_submit(u64 now) noexcept;
bool application_profile_response(const RestClientResponse& response, Utf8View body) noexcept;
void application_profile_layout(UiRect viewport) noexcept;
u64 application_profile_next_wake() noexcept;
// The editor that owns keyboard and IME input: the note while it is edited,
// else the message field.
MessageComposer* application_profile_edit_model() noexcept;
bool application_profile_edit_focused() noexcept;
// A note load, save or USER_NOTE_UPDATE for the shown user.
void application_profile_note_changed() noexcept;
} // namespace maicord::app_detail
