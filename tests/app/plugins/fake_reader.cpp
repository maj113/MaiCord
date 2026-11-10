#include "maicord/app/plugins/surfaces.hpp"

// The cache plugins read through mc_api.read_* (the plugin_host_read_* functions of maicord/app/plugins/surfaces.hpp)
// for the tests that do not read: an application whose cache holds nothing. They link this file in place of
// src/app/plugins/surface_host.cpp.
namespace maicord {

bool plugin_host_read_user(u64, u64, PluginUserInput*) noexcept {
    return false;
}

bool plugin_host_read_channel(u64, PluginChannelInput*) noexcept {
    return false;
}

bool plugin_host_read_guild(u64, PluginGuildInput*) noexcept {
    return false;
}

bool plugin_host_read_permissions(u64, u64, u64, u64*) noexcept {
    return false;
}

bool plugin_host_read_voice_channel(u64, u64, u64*) noexcept {
    return false;
}

u32 plugin_host_read_shared_group_dms(u64, Slice<u64>) noexcept {
    return 0;
}

} // namespace maicord
