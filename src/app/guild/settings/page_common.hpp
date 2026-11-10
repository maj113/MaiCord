#pragma once
// What the Access, Community, Server Template, Integrations, AutoMod and Server Tag pages share beyond internal.hpp.
// UI thread only.
#include "internal.hpp"

namespace maicord::app_detail::server_settings {

inline bool community() noexcept {
    return (guild_features() & guild_feature_community) != 0;
}
inline bool administrator() noexcept {
    return permission_has(owner->access.permissions, kPermissionAdministrator);
}
// A run of fixed notices: each one a description, then its heading, both NUL-ended (an empty heading is a lone NUL);
// the string's own end ends the run.
void push_notices(const char* texts) noexcept;

// A failed read: its heading over the layer's status, with the notice action that reads again.
u32 push_load_error(Text heading) noexcept;
// The layer's status as an error row while a request of the page has failed.
void push_status_error() noexcept;
// The page's state could not be made.
void push_no_memory() noexcept;
// The switch of a toggle row moving to the state the activation set.
void animate_toggle(const SettingsLayerActivation& activation) noexcept;

// The next part of `text` from `*at` up to `separator`, without its surrounding white space; false once every part
// (an empty one included) has been read.
bool next_part(const Utf8View& text, u32* at, char separator, Utf8View* part) noexcept;

// The server's channels whose type bit is in `types` (a bit per channel type) in channel-list order, as
// Settings::popup_rows[i].channel; at most `room` of them, their count.
u32 sorted_channels(u32 types, u32 room) noexcept;

// A dangerous confirmation: `save` runs when it is accepted. `cancel` labels the way out when it is not "Cancel".
void open_confirmation(u64 kind, u64 item, Text title, Text context, Text accept,
                       bool (*save)(TextFormOwnerKey key, Utf8View text) noexcept, Text cancel = {}) noexcept;
// A read-only text (`detail`) under its title and `context`, closed by its one button.
void open_preview(u64 kind, u64 item, Text title, Text context, Text label, Text detail) noexcept;
// A text form's texts and size limits (the rest of the request stays with the caller).
void fill_form(ApplicationTextFormRequest& request, Text title, Text context, Text label, Text accept, u32 codepoints,
               u32 bytes) noexcept;

} // namespace maicord::app_detail::server_settings
