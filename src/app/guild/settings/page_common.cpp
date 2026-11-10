#include "page_common.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/time.hpp"

namespace maicord::app_detail::server_settings {

u32 push_load_error(const Utf8View& heading) noexcept {
    return push_error(heading, owner->status, utf8_literal("Try again"));
}

void push_status_error() noexcept {
    if (owner->failed && owner->status.bytes)
        push_error({}, owner->status);
}

void push_no_memory() noexcept {
    push_error(utf8_literal("Couldn't open this page"), utf8_literal("MaiCord ran out of memory."));
}

void push_notices(const char* texts) noexcept {
    while (*texts) {
        const u32 description = c_string_length(texts);
        const char* heading = texts + description + 1;
        const u32 heading_bytes = c_string_length(heading);
        push_notice({texts, description}, {heading, heading_bytes});
        texts = heading + heading_bytes + 1;
    }
}

void animate_toggle(const SettingsLayerActivation& activation) noexcept {
    ui_settings_scene_animate_toggle(&owner->scene, activation.value.target, activation.value.checked,
                                     clock_monotonic_milliseconds());
}

bool next_part(const Utf8View& text, u32* at, char separator, Utf8View* part) noexcept {
    if (*at > text.bytes)
        return false;
    u32 end = *at;
    while (end < text.bytes && text.data[end] != separator)
        ++end;
    *part = ascii_trim({text.data + *at, end - *at});
    *at = end + 1;
    return true;
}

u32 sorted_channels(u32 types, u32 room) noexcept {
    auto& p = *owner;
    const auto& s = state();
    u32 count = 0;
    const u32 slots = chat_state_store_channel_slot_bound(s);
    for (u32 i = 0; i < slots && count < room; ++i) {
        const auto& channel = s.channels[i];
        if (channel.occupied && channel.guild_id == p.guild_id && channel.type < 32 && (types >> channel.type & 1))
            p.popup_rows[count++] = {{}, &channel};
    }
    using Row = Settings::PopupRow;
    heap_sort_shared(p.popup_rows, count, [&s](const Row& a, const Row& b) noexcept {
        return guild_settings_channel_before(s, GuildChannelPickerOrder::channel_list, *a.channel, *b.channel);
    });
    return count;
}

void open_confirmation(u64 kind, u64 item, const Utf8View& title, const Utf8View& context, const Utf8View& accept,
                       bool (*save)(TextFormOwnerKey key, Utf8View text) noexcept, const Utf8View& cancel) noexcept {
    ApplicationTextFormRequest request{};
    request.title = title;
    request.context = context;
    request.accept = accept;
    request.cancel = cancel;
    request.danger = request.confirmation_only = true;
    request.owner = form_key(kind, item);
    request.valid = form_valid;
    request.save = save;
    application_text_form_open(request);
}

void open_preview(u64 kind, u64 item, const Utf8View& title, const Utf8View& context, const Utf8View& label,
                  const Utf8View& detail) noexcept {
    ApplicationTextFormRequest request{};
    request.title = title;
    request.context = context;
    request.label = label;
    request.detail = detail;
    request.accept = utf8_literal("Close");
    request.owner = form_key(kind, item);
    request.valid = form_valid;
    request.save = [](TextFormOwnerKey, Utf8View) noexcept {
        application_text_form_close();
        return true;
    };
    request.confirmation_only = true;
    request.single_action = true;
    application_text_form_open(request);
}

} // namespace maicord::app_detail::server_settings
