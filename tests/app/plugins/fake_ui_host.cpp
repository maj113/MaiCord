#include "fake_ui_host.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace plugin_fake_ui {
Recorded recorded;
} // namespace plugin_fake_ui
namespace {

using plugin_fake_ui::recorded;

template <u32 N> void keep(char (&storage)[N], Utf8View text) noexcept {
    const u32 bytes = text.bytes < N - 1 ? text.bytes : N - 1;
    copy_bytes(storage, text.data, bytes);
    storage[bytes] = 0;
}

} // namespace

bool plugin_host_open_modal(const PluginModalRequest& request) noexcept {
    if (!recorded.accept_modal) return false;
    ++recorded.modal_opens;
    recorded.modal = request;
    keep(recorded.title, request.title);
    keep(recorded.context, request.context);
    keep(recorded.accept, request.accept);
    keep(recorded.cancel, request.cancel);
    keep(recorded.input_label, request.input_label);
    keep(recorded.input_initial, request.input_initial);
    keep(recorded.hint, request.input_hint);
    keep(recorded.toggle, request.toggle_label);
    keep(recorded.select_label, request.select_label);
    return true;
}

void plugin_host_banner_changed() noexcept {
    ++recorded.banner_changes;
}

void plugin_host_close_modal() noexcept {
    ++recorded.modal_closes;
}

PluginUiStatus plugin_host_open_url(Utf8View url) noexcept {
    ++recorded.urls;
    keep(recorded.url, url);
    return recorded.status;
}

PluginUiStatus plugin_host_copy_text(Utf8View text) noexcept {
    ++recorded.copies;
    keep(recorded.copy, text);
    return recorded.status;
}

PluginUiStatus plugin_host_open_channel(u64 channel_id) noexcept {
    ++recorded.channels;
    recorded.channel = channel_id;
    return recorded.status;
}

PluginUiStatus plugin_host_open_profile(u64 user_id, u64 guild_id) noexcept {
    ++recorded.profiles;
    recorded.user = user_id;
    recorded.guild = guild_id;
    return recorded.status;
}

bool plugin_host_local_notice(u64 channel_id, Utf8View text, u64*) noexcept {
    ++recorded.notices;
    recorded.notice_channel = channel_id;
    keep(recorded.notice, text);
    return true;
}

void plugin_host_toast(u64 owner_id, Utf8View text, u32 tone) noexcept {
    ++recorded.toasts;
    recorded.toast_owner = owner_id;
    keep(recorded.toast, text);
    recorded.toast_tone = tone;
}

} // namespace maicord
