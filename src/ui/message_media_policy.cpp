#include "maicord/ui/message_media_policy.hpp"

#include "maicord/model/message_rich_store.hpp"

namespace maicord {

bool message_media_revealed(const MessageMediaReveals& reveals, u64 key) noexcept {
    for (u32 i = 0; i < reveals.count && i < kMessageMediaRevealMaximum; ++i)
        if (reveals.keys[i] == key)
            return true;
    return false;
}

bool message_media_reveal(MessageMediaReveals* reveals, u64 key) noexcept {
    if (!reveals || !key || message_media_revealed(*reveals, key))
        return false;
    reveals->keys[reveals->cursor % kMessageMediaRevealMaximum] = key;
    reveals->cursor = (reveals->cursor + 1u) % kMessageMediaRevealMaximum;
    if (reveals->count < kMessageMediaRevealMaximum)
        ++reveals->count;
    ++reveals->revision;
    return true;
}

MessageMediaObscure message_media_obscure(const MessageMediaReveals& reveals, u64 key, bool spoiler,
                                          bool explicit_media, MessageExplicitFilter filter,
                                          bool spoilers_always) noexcept {
    if (explicit_media && filter == MessageExplicitFilter::block)
        return MessageMediaObscure::blocked;
    if (message_media_revealed(reveals, key))
        return MessageMediaObscure::none;
    if (explicit_media && filter == MessageExplicitFilter::blur)
        return MessageMediaObscure::explicit_content;
    if (spoiler && !spoilers_always)
        return MessageMediaObscure::spoiler;
    return MessageMediaObscure::none;
}

bool message_rich_attachment_spoiler(const MessageRichStore& store, const MessageRichAttachment& attachment) noexcept {
    if (attachment.flags & kGatewayAttachmentSpoilerFlag)
        return true;
    const Utf8View name = message_rich_store_string(store, attachment.filename);
    constexpr char prefix[] = "SPOILER_";
    if (name.bytes < sizeof(prefix) - 1u)
        return false;
    for (u32 i = 0; i + 1u < sizeof(prefix); ++i)
        if (name.data[i] != prefix[i])
            return false;
    return true;
}

bool message_rich_attachment_explicit(const MessageRichAttachment& attachment) noexcept {
    return (attachment.flags & kGatewayMediaExplicitFlag) != 0;
}

bool message_rich_embed_explicit(const MessageRichEmbed& embed) noexcept {
    return (embed.flags & kGatewayMediaExplicitFlag) != 0;
}

bool message_rich_component_spoiler(const MessageRichComponent& component) noexcept {
    return (component.flags & gateway_message_component_spoiler) != 0;
}

Utf8View message_media_obscure_label(MessageMediaObscure obscure) noexcept {
    switch (obscure) {
    case MessageMediaObscure::spoiler:
        return utf8_literal("Spoiler");
    case MessageMediaObscure::explicit_content:
        return utf8_literal("Explicit content");
    case MessageMediaObscure::blocked:
        return utf8_literal("Explicit content blocked");
    case MessageMediaObscure::plugin_hidden:
        return utf8_literal("Media hidden by a plugin");
    default:
        return {};
    }
}

} // namespace maicord
