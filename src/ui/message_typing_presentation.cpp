#include "maicord/ui/message_typing_presentation.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

struct Writer {
    Slice<char> output;
    u32 used;
};

bool append(Writer* writer, Utf8View value) noexcept {
    if (!writer || (!value.data && value.bytes != 0) || writer->used > writer->output.count ||
        value.bytes > writer->output.count - writer->used) {
        return false;
    }
    copy_bytes(writer->output.data + writer->used, value.data, value.bytes);
    writer->used += value.bytes;
    return true;
}

Utf8View display_name(const UserStore& users, u64 user_id) noexcept {
    const UserRecord* user = user_store_find(users, user_id);
    return user ? user_store_name(users, *user) : Utf8View{};
}

MessageTypingPresentationResult failure(MessageTypingPresentationError error) noexcept {
    return MessageTypingPresentationResult{0, 0, error};
}

} // namespace

MessageTypingPresentationResult message_typing_presentation_build(const TypingStore& typing,
                                                                  const UserStore& users,
                                                                  u64 channel_id, u64 self_user_id,
                                                                  Slice<char> output) noexcept {
    if (channel_id == 0 || !typing.records.data || typing.count > typing.records.count || !users.records ||
        !users.order || !users.names || (!output.data && output.count != 0)) {
        return failure(MessageTypingPresentationError::invalid_input);
    }
    const u32 count = typing_store_channel_count(typing, channel_id, self_user_id);
    if (count == 0) {
        return MessageTypingPresentationResult{0, 0, MessageTypingPresentationError::none};
    }
    Writer writer{output, 0};
    if (count > 3) {
        constexpr Utf8View several = utf8_literal("Several people are typing…");
        if (!append(&writer, several)) {
            return failure(MessageTypingPresentationError::capacity);
        }
        return MessageTypingPresentationResult{writer.used, count, MessageTypingPresentationError::none};
    }
    u64 ids[3]{};
    const u32 written =
        typing_store_channel_users(typing, channel_id, self_user_id, Slice<u64>{ids, 3});
    if (written != count) {
        return failure(MessageTypingPresentationError::invalid_input);
    }
    constexpr Utf8View someone = utf8_literal("Someone");
    for (u32 index = 0; index < written; ++index) {
        const Utf8View name = display_name(users, ids[index]);
        if (index != 0) {
            const Utf8View separator = written == 2           ? utf8_literal(" and ")
                                       : index + 1 == written ? utf8_literal(", and ")
                                                              : utf8_literal(", ");
            if (!append(&writer, separator)) {
                return failure(MessageTypingPresentationError::capacity);
            }
        }
        if (!append(&writer, name.bytes != 0 ? name : someone)) {
            return failure(MessageTypingPresentationError::capacity);
        }
    }
    if (!append(&writer, written == 1 ? utf8_literal(" is typing…") : utf8_literal(" are typing…"))) {
        return failure(MessageTypingPresentationError::capacity);
    }
    return MessageTypingPresentationResult{writer.used, count, MessageTypingPresentationError::none};
}

} // namespace maicord
