#include "page_common.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/time.hpp"

namespace maicord::app_detail::server_settings {

u32 push_load_error(Text heading) noexcept {
    return push_error(heading, owner->status, "Try again");
}

void push_status_error() noexcept {
    if (owner->failed && owner->status.bytes)
        push_error({}, owner->status);
}

void push_no_memory() noexcept {
    push_error("Couldn't open this page", "MaiCord ran out of memory.");
}

void push_notices(const char* texts) noexcept {
    while (*texts) {
        const u32 description = c_string_length(texts);
        const char* heading = texts + description + 1;
        const u32 heading_bytes = c_string_length(heading);
        push_notice(Utf8View{texts, description}, Utf8View{heading, heading_bytes});
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

void fill_form(ApplicationTextFormRequest& request, Text title, Text context, Text label, Text accept, u32 codepoints,
               u32 bytes) noexcept {
    request.title = text_view(title);
    request.context = text_view(context);
    request.label = text_view(label);
    request.accept = text_view(accept);
    request.maximum_codepoints = codepoints;
    request.maximum_bytes = bytes;
}

void open_confirmation(u64 kind, u64 item, Text title, Text context, Text accept,
                       bool (*save)(TextFormOwnerKey key, Utf8View text) noexcept, Text cancel) noexcept {
    ApplicationTextFormRequest request{};
    fill_form(request, title, context, {}, accept, 0, 0);
    request.cancel = text_view(cancel);
    request.danger = request.confirmation_only = true;
    request.owner = form_key(kind, item);
    request.valid = form_valid;
    request.save = save;
    application_text_form_open(request);
}

void open_preview(u64 kind, u64 item, Text title, Text context, Text label, Text detail) noexcept {
    ApplicationTextFormRequest request{};
    fill_form(request, title, context, label, "Close", 0, 0);
    request.detail = text_view(detail);
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
