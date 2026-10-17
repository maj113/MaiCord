#include "maicord/ui/private_channel_title.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/model/private_channel_store.hpp"

namespace maicord {
u64 private_channel_peer(const ChatStateStore& state, u64 channel_id) noexcept {
    if (!state.private_channels) return 0;
    const auto* record = private_channel_store_find(*state.private_channels, channel_id);
    if (!record) return 0;
    const auto recipients = private_channel_store_recipients(*state.private_channels, *record);
    for (u32 i = 0; i < recipients.count; ++i) if (recipients[i] != state.self_user_id) return recipients[i];
    return 0;
}
Utf8View private_channel_title(const ChatStateStore& state, const UserStore* users,
    const ChannelRecord& channel, Slice<char> storage) noexcept {
    const Utf8View explicit_name = chat_state_store_channel_name(state, channel);
    if (explicit_name.bytes) return explicit_name;
    const auto* record = state.private_channels ? private_channel_store_find(*state.private_channels, channel.id) : nullptr;
    const auto recipients = record ? private_channel_store_recipients(*state.private_channels, *record) : ConstSlice<u64>{};
    if (!storage.data || storage.count < 4) return {};
    u32 used = 0;
    bool truncated = false;
    const auto append = [&](Utf8View text) noexcept {
        const u32 take = minimum(text.bytes, storage.count - 3 - used);
        u32 bytes = take;
        // Do not split a UTF-8 sequence when reserving the ellipsis suffix.
        while (bytes && bytes < text.bytes && (static_cast<u8>(text.data[bytes]) & 0xc0u) == 0x80u) --bytes;
        copy_bytes(storage.data + used, text.data, bytes);
        used += bytes;
        truncated = truncated || bytes != text.bytes;
    };
    for (u32 i = 0; i < recipients.count && !truncated; ++i) {
        if (recipients[i] == state.self_user_id) continue;
        if (used) append(utf8_literal(", "));
        const auto* user = users ? user_store_find(*users, recipients[i]) : nullptr;
        const auto nick = state.private_channels ? private_channel_store_nickname(*state.private_channels, channel.id, recipients[i]) : Utf8View{};
        if (nick.bytes) append(nick);
        else if (user) append(user_store_name(*users, *user));
        else {
            char digits[20];
            u32 first = 20;
            u64 id = recipients[i];
            do { digits[--first] = static_cast<char>('0' + id % 10); id /= 10; } while (id);
            append({digits + first, 20 - first});
        }
        if (channel.type == 1) break;
    }
    if (!used) return channel.type == 1 ? utf8_literal("Direct message") : utf8_literal("Group message");
    if (truncated) { copy_bytes(storage.data + used, "\xe2\x80\xa6", 3); used += 3; }
    return {storage.data, used};
}
}
