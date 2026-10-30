#pragma once
#include "application_commands.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/user_profile.hpp"
#include "maicord/ui/profile_scene.hpp"

namespace maicord::app_detail {
// A bot's popular commands for one profile surface (the DM panel, the popout
// and the full profile), copied because the command index may change under
// its views, and what they were resolved from.
struct ProfileCommandSet {
    ProfileCommand commands[kProfileCommandMaximum];
    ProfileString<132> names[kProfileCommandMaximum];
    ProfileString<404> descriptions[kProfileCommandMaximum];
    u64 user, loaded_at, channel, application;
    u32 count, revision;
    // The index answered for this channel: an inactive channel (a selection
    // still settling) or an index on its way is asked again next time.
    bool settled;
};
// Resolves the Commands section: the profile's popular
// command IDs, in that order, among the app's root commands loaded for the
// selected channel (the conversation or server channel), without NSFW
// commands or commands with subcommands. Asking starts loading a missing
// index; its landing changes the catalog revision. Until the index answers
// (it is loading, or channel is not yet the active_channel the command index
// binds to) every call asks again. True when the list changed (the surface
// lays out again). loaded_at is when the profile arrived (zero: not yet).
inline bool profile_commands_resolve(ProfileCommandSet* set, const UserProfile& profile, u64 loaded_at,
                                     u64 channel, u64 active_channel) noexcept {
    const u32 revision = application_commands_catalog_revision();
    const bool same_source = set->user == profile.user_id && set->loaded_at == loaded_at && set->channel == channel;
    if (same_source && set->settled && set->revision == revision)
        return false;
    u64 before[kProfileCommandMaximum];
    const u32 before_count = same_source ? set->count : ~0u;
    for (u32 i = 0; same_source && i < set->count; ++i)
        before[i] = set->commands[i].id;
    set->user = profile.user_id;
    set->loaded_at = loaded_at;
    set->channel = channel;
    set->revision = revision;
    set->application = profile.application_id;
    set->count = 0;
    // An app may register 100 global commands, more in servers. Surfaces
    // resolve one at a time on the UI thread, so they share this scratch.
    static ApplicationRootCommand roots[128];
    bool loading = false;
    const bool wanted =
        loaded_at && profile.user_appearance.bot && profile.application_id && profile.popular_command_count && channel;
    const u32 found =
        wanted ? application_commands_for_application(channel, profile.application_id, {roots, 128}, &loading) : 0;
    set->settled = !wanted || (!loading && active_channel == channel);
    for (u32 i = 0; i < profile.popular_command_count && set->count < kProfileCommandMaximum; ++i)
        for (u32 k = 0; k < found; ++k) {
            const auto& root = roots[k];
            if (root.command_id != profile.popular_commands[i])
                continue;
            if (!root.nsfw && !root.subcommands && root.name.bytes) {
                const u32 n = set->count++;
                auto& name = set->names[n];
                auto& description = set->descriptions[n];
                name.bytes = static_cast<u16>(utf8_copy_truncated(name.data, root.name));
                description.bytes = static_cast<u16>(utf8_copy_truncated(description.data, root.description));
                set->commands[n] = {root.command_id, name.view(), description.view()};
            }
            break;
        }
    // Only a different list lays the surface out again (an index still on its
    // way asks each frame without moving anything).
    bool changed = set->count != before_count;
    for (u32 i = 0; !changed && i < set->count; ++i)
        changed = set->commands[i].id != before[i];
    return changed;
}
inline ConstSlice<ProfileCommand> profile_commands_view(const ProfileCommandSet& set) noexcept {
    return {set.commands, set.count};
}
// A command chip's click: the command (source: its index) goes to the composer.
inline bool profile_commands_insert(const ProfileCommandSet& set, u16 index) noexcept {
    return index < set.count &&
           application_commands_insert(set.application, set.commands[index].id, set.commands[index].name);
}
} // namespace maicord::app_detail
