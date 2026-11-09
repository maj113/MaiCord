#include "maicord/ui/chat/messages/typing_presentation.hpp"

#include "maicord/core/text_bank.hpp"

namespace maicord {
namespace {

Utf8View display_name(const UserStore& users, u64 user_id) noexcept {
    const UserRecord* user = user_store_find(users, user_id);
    return user ? user_store_name(users, *user) : Utf8View{};
}

MessageTypingPresentationResult failure(MessageTypingPresentationError error) noexcept {
    return MessageTypingPresentationResult{.error = error};
}

} // namespace

MessageTypingPresentationResult message_typing_presentation_build(const TypingStore& typing,
                                                                  const UserStore& users,
                                                                  u64 channel_id, u64 self_user_id,
                                                                  Slice<char> output, MessageTypingName* names) noexcept {
    if (channel_id == 0 || !typing.records.data || typing.count > typing.records.count || !users.records ||
        !users.order || !users.names || (!output.data && output.count != 0)) {
        return failure(MessageTypingPresentationError::invalid_input);
    }
    const u32 count = typing_store_channel_count(typing, channel_id, self_user_id);
    if (count == 0) {
        return MessageTypingPresentationResult{};
    }
    TextWriter writer{output};
    u32 named = 0;
    if (count > 3) {
        writer.add(utf8_literal("Several people"));
        if (names && writer.ok)
            names[named++] = {0, static_cast<u16>(writer.used)};
        writer.add(utf8_literal(" are typing..."));
    } else {
        u64 ids[3]{};
        const u32 written =
            typing_store_channel_users(typing, channel_id, self_user_id, Slice<u64>{ids, 3});
        if (written != count) {
            return failure(MessageTypingPresentationError::invalid_input);
        }
        for (u32 index = 0; index < written; ++index) {
            const Utf8View name = display_name(users, ids[index]);
            if (index != 0) {
                writer.add(index + 1 == written ? utf8_literal(" and ") : utf8_literal(", "));
            }
            const u32 name_at = writer.used;
            writer.add(name.bytes != 0 ? name : utf8_literal("Someone"));
            if (names && writer.ok)
                names[named++] = {static_cast<u16>(name_at), static_cast<u16>(writer.used - name_at)};
        }
        writer.add(written == 1 ? utf8_literal(" is typing...") : utf8_literal(" are typing..."));
    }
    if (!writer.ok) {
        return failure(MessageTypingPresentationError::capacity);
    }
    return MessageTypingPresentationResult{writer.used, count, named, MessageTypingPresentationError::none};
}

} // namespace maicord
