#include "maicord/ui/chat/messages/media_policy.hpp"

#include "maicord/model/chat/message_rich_store.hpp"
#include "maicord/model/chat/content_policy.hpp"
#include "maicord/model/chat/message_store.hpp"

namespace maicord {

MessageExplicitFilter message_explicit_filter_resolve(MessageMediaContext context, u64 explicit_value,
                                                      u64 legacy_value,
                                                      const AccountContentPolicy& account) noexcept {
    if (explicit_value >= 1 && explicit_value <= 3)
        return explicit_value == 1   ? MessageExplicitFilter::show
               : explicit_value == 2 ? MessageExplicitFilter::blur
                                     : MessageExplicitFilter::block;
    const bool regional_teen = (account.flags & account_content_teen_defaults) != 0;
    const bool teen = regional_teen || account.eligibility == AccountContentEligibility::restricted;
    if (regional_teen)
        return context == MessageMediaContext::other_dm ? MessageExplicitFilter::block
                                                        : MessageExplicitFilter::blur;
    if (context == MessageMediaContext::guild)
        return teen ? MessageExplicitFilter::blur : MessageExplicitFilter::show;
    if (legacy_value == 2 || (legacy_value != 0 && context == MessageMediaContext::other_dm))
        return MessageExplicitFilter::block;
    return teen ? MessageExplicitFilter::blur : MessageExplicitFilter::show;
}

MessageMediaOwner message_media_owner(const MessageRecord& record,
                                      const MessageRichRecord* outer_rich) noexcept {
    MessageMediaOwner owner{};
    owner.channel_id = record.channel_id;
    owner.author_id = record.author_id;
    if (outer_rich && !outer_rich->forwarded_snapshot_child && !message_type_is_system(record.type) &&
        !message_type_is_system(outer_rich->system.message_type) && outer_rich->reference.type == 1) {
        owner.forwarded = true;
        owner.author_id = 0;
        owner.source_channel_id = outer_rich->reference.channel_id;
        owner.source_message_id = outer_rich->reference.message_id;
    }
    return owner;
}

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
                                          bool spoilers_always, bool explicit_reveal_allowed) noexcept {
    if (explicit_media && filter == MessageExplicitFilter::block)
        return MessageMediaObscure::blocked;
    if (explicit_media && filter == MessageExplicitFilter::blur && !explicit_reveal_allowed)
        return MessageMediaObscure::explicit_content;
    if (message_media_revealed(reveals, key))
        return MessageMediaObscure::none;
    if (explicit_media && filter == MessageExplicitFilter::blur)
        return MessageMediaObscure::explicit_content;
    if (spoiler && !spoilers_always)
        return MessageMediaObscure::spoiler;
    return MessageMediaObscure::none;
}

bool message_rich_attachment_spoiler(const MessageRichStore& store,
                                     const MessageRichAttachment& attachment) noexcept {
    if (attachment.flags & kGatewayAttachmentSpoilerFlag)
        return true;
    const Utf8View name = message_rich_store_string(store, attachment.filename);
    static constexpr char prefix[] = "SPOILER_";
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

__declspec(noinline) u64 message_media_reveal_key(u64 message_id, MessageMediaSource source,
                                                  u32 index, bool snapshot) noexcept {
    u64 value = message_id * 0x9E3779B97F4A7C15ull;
    value ^= (static_cast<u64>(source) + 1u) << 56u;
    value ^= static_cast<u64>(index + 1u) * 0xBF58476D1CE4E5B9ull;
    if (snapshot)
        value ^= 0x94D049BB133111EBull;
    return value ? value : 1u;
}

UiRect message_media_obscure_pill(const UiRect& media, MessageMediaObscure obscure) noexcept {
    const float wanted = obscure == MessageMediaObscure::spoiler ? 84.0f : 148.0f;
    const float available = media.right - media.left - 16.0f;
    const float width = wanted < available ? wanted : available;
    const float height = 28.0f;
    if (width < 40.0f || media.bottom - media.top < height + 8.0f)
        return {};
    const float cx = (media.left + media.right) * 0.5f, cy = (media.top + media.bottom) * 0.5f;
    return {cx - width * 0.5f, cy - height * 0.5f, cx + width * 0.5f, cy + height * 0.5f};
}

} // namespace maicord
