#include "maicord/protocol/messages/reaction.hpp"

#include "maicord/protocol/http/rest_route.hpp"

namespace maicord {
namespace {

MessageReactionWriteResult error(
    MessageReactionWriteError value) noexcept {
    return MessageReactionWriteResult{0, value};
}

} // namespace

MessageReactionWriteResult write_message_reaction_target(
    u64 channel_id, u64 message_id, u64 emoji_id, Utf8View emoji_name,
    MessageReactionType type, bool add, Slice<u8> output) noexcept {
    const u8 type_value = static_cast<u8>(type);
    if (channel_id == 0 || message_id == 0 || !output.data ||
        !emoji_name.data || emoji_name.bytes == 0 ||
        emoji_name.bytes > kMessageReactionMaximumEmojiNameBytes ||
        type_value > static_cast<u8>(MessageReactionType::vote)) {
        return error(MessageReactionWriteError::invalid_input);
    }
    if (!utf8_validate(emoji_name).valid) {
        return error(MessageReactionWriteError::invalid_utf8);
    }
    RouteWriter writer{};
    if (!route_writer_initialize(&writer, output) ||
        !route_write(&writer, "/api/v10/channels/", channel_id, "/messages/", message_id, "/reactions/",
                     path_component(emoji_name)) ||
        (emoji_id != 0 && !route_write(&writer, ':', emoji_id)) || !route_write(&writer, '/')) {
        return error(MessageReactionWriteError::output_too_small);
    }
    const bool suffix = add ? route_write(&writer, "@me?location=Message&type=")
                            : route_write(&writer, static_cast<char>('0' + type_value),
                                          "/@me?location=Message&burst=");
    const bool burst_value =
        add || route_write(&writer, type == MessageReactionType::burst ? utf8_literal("true") : utf8_literal("false"));
    if (!suffix || (add && !route_write(&writer, static_cast<char>('0' + type_value))) || !burst_value) {
        return error(MessageReactionWriteError::output_too_small);
    }
    return MessageReactionWriteResult{
        writer.bytes, MessageReactionWriteError::none};
}

} // namespace maicord
