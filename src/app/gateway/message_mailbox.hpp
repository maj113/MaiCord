#pragma once
// The message mailbox's descriptor block (client_event.hpp) against the decoded structures it carries, shared by its packer
// (dispatch.cpp) and unpacker (dispatch_payload.cpp). Both move each run of adjacent text members as one run of
// views <-> ranges and the six rich counts as one array, so these assertions are the contract that makes that exact.
#include "maicord/app/gateway/client_event.hpp"

namespace maicord {
namespace mailbox_layout {

template <typename... Offsets> consteval bool run(u32 stride, u32 first, Offsets... rest) noexcept {
    u32 index = 1;
    return ((static_cast<u32>(rest) == first + stride * index++) && ...);
}

#define MAICORD_MEMBER_OFFSET(Type, member) static_cast<u32>(__builtin_offsetof(Type, member))

static_assert(sizeof(Utf8View) == 16 && sizeof(GatewayClientMessageTextRange) == 8);

// GatewayMessageReference and GatewayMessageInteraction against the header's six ranges.
static_assert(run(16, MAICORD_MEMBER_OFFSET(GatewayMessageReference, author_name),
                  MAICORD_MEMBER_OFFSET(GatewayMessageReference, author_avatar_hash),
                  MAICORD_MEMBER_OFFSET(GatewayMessageReference, content)));
static_assert(run(8, MAICORD_MEMBER_OFFSET(GatewayClientMessageRichHeader, reference_author_name),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageRichHeader, reference_author_avatar_hash),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageRichHeader, reference_content)));
static_assert(run(16, MAICORD_MEMBER_OFFSET(GatewayMessageInteraction, user_name),
                  MAICORD_MEMBER_OFFSET(GatewayMessageInteraction, user_avatar_hash),
                  MAICORD_MEMBER_OFFSET(GatewayMessageInteraction, command_name)));
static_assert(run(8, MAICORD_MEMBER_OFFSET(GatewayClientMessageRichHeader, interaction_user_name),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageRichHeader, interaction_user_avatar_hash),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageRichHeader, interaction_command_name)));

// The six counts, in the order of the descriptor arrays that follow the header.
static_assert(run(2, MAICORD_MEMBER_OFFSET(GatewayClientMessageRichHeader, reaction_count),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageRichHeader, attachment_count),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageRichHeader, embed_count),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageRichHeader, embed_field_count),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageRichHeader, sticker_count),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageRichHeader, component_count)));

static_assert(run(16, MAICORD_MEMBER_OFFSET(GatewayMessageAttachment, filename),
                  MAICORD_MEMBER_OFFSET(GatewayMessageAttachment, title),
                  MAICORD_MEMBER_OFFSET(GatewayMessageAttachment, description),
                  MAICORD_MEMBER_OFFSET(GatewayMessageAttachment, content_type),
                  MAICORD_MEMBER_OFFSET(GatewayMessageAttachment, url)));
static_assert(run(8, MAICORD_MEMBER_OFFSET(GatewayClientMessageAttachmentMutation, filename),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageAttachmentMutation, title),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageAttachmentMutation, description),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageAttachmentMutation, content_type),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageAttachmentMutation, url)));

static_assert(run(16, MAICORD_MEMBER_OFFSET(GatewayMessageEmbed, title),
                  MAICORD_MEMBER_OFFSET(GatewayMessageEmbed, description),
                  MAICORD_MEMBER_OFFSET(GatewayMessageEmbed, url),
                  MAICORD_MEMBER_OFFSET(GatewayMessageEmbed, author_name),
                  MAICORD_MEMBER_OFFSET(GatewayMessageEmbed, footer_text),
                  MAICORD_MEMBER_OFFSET(GatewayMessageEmbed, author_url),
                  MAICORD_MEMBER_OFFSET(GatewayMessageEmbed, author_icon),
                  MAICORD_MEMBER_OFFSET(GatewayMessageEmbed, footer_icon),
                  MAICORD_MEMBER_OFFSET(GatewayMessageEmbed, provider_name),
                  MAICORD_MEMBER_OFFSET(GatewayMessageEmbed, provider_url)));
static_assert(run(8, MAICORD_MEMBER_OFFSET(GatewayClientMessageEmbedMutation, title),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageEmbedMutation, description),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageEmbedMutation, url),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageEmbedMutation, author_name),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageEmbedMutation, footer_text),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageEmbedMutation, author_url),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageEmbedMutation, author_icon),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageEmbedMutation, footer_icon),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageEmbedMutation, provider_name),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageEmbedMutation, provider_url)));
static_assert(run(16, MAICORD_MEMBER_OFFSET(GatewayMessageEmbedMedia, url),
                  MAICORD_MEMBER_OFFSET(GatewayMessageEmbedMedia, proxy_url),
                  MAICORD_MEMBER_OFFSET(GatewayMessageEmbedMedia, content_type),
                  MAICORD_MEMBER_OFFSET(GatewayMessageEmbedMedia, description)));
static_assert(run(8, MAICORD_MEMBER_OFFSET(GatewayClientMessageEmbedMediaMutation, url),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageEmbedMediaMutation, proxy_url),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageEmbedMediaMutation, content_type),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageEmbedMediaMutation, description)));

static_assert(run(16, MAICORD_MEMBER_OFFSET(GatewayMessageEmbedField, name),
                  MAICORD_MEMBER_OFFSET(GatewayMessageEmbedField, value)));
static_assert(run(8, MAICORD_MEMBER_OFFSET(GatewayClientMessageEmbedFieldMutation, name),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageEmbedFieldMutation, value)));

static_assert(run(16, MAICORD_MEMBER_OFFSET(GatewayMessageComponent, label),
                  MAICORD_MEMBER_OFFSET(GatewayMessageComponent, content),
                  MAICORD_MEMBER_OFFSET(GatewayMessageComponent, custom_id),
                  MAICORD_MEMBER_OFFSET(GatewayMessageComponent, url),
                  MAICORD_MEMBER_OFFSET(GatewayMessageComponent, emoji_name)));
static_assert(run(8, MAICORD_MEMBER_OFFSET(GatewayClientMessageComponentMutation, label),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageComponentMutation, content),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageComponentMutation, custom_id),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageComponentMutation, url),
                  MAICORD_MEMBER_OFFSET(GatewayClientMessageComponentMutation, emoji_name)));

#undef MAICORD_MEMBER_OFFSET

} // namespace mailbox_layout
} // namespace maicord
