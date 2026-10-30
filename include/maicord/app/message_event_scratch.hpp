#pragma once

#include "maicord/protocol/gateway_message_events.hpp"

namespace maicord {

// The decode arrays of one message event. On the stack they are ~30 KB, which
// costs a stack probe for every gateway message, so each applying path keeps
// one in static storage instead (message events apply on one thread and never
// nest within the same path).
struct MessageEventScratch {
    GatewayMessageReaction reactions[kGatewayMessageReactionMaximum];
    GatewayMessageAttachment attachments[kGatewayMessageAttachmentMaximum];
    GatewayMessageEmbed embeds[kGatewayMessageEmbedMaximum];
    GatewayMessageEmbedField embed_fields[kGatewayMessageEmbedFieldMaximum];
    GatewayMessageSticker stickers[kGatewayMessageStickerMaximum];
    GatewayMessageComponent components[kGatewayMessageComponentMaximum];
    GatewayMessageAttachment snapshot_attachments[kGatewayMessageAttachmentMaximum];
    GatewayMessageEmbed snapshot_embeds[kGatewayMessageEmbedMaximum];
    GatewayMessageEmbedField snapshot_embed_fields[kGatewayMessageEmbedFieldMaximum];
    GatewayMessageSticker snapshot_stickers[kGatewayMessageStickerMaximum];
    GatewayMessageComponent snapshot_components[kGatewayMessageComponentMaximum];
    GatewayMessageNamedUser snapshot_users[kGatewayMessageMentionUserMaximum];
    GatewayMessagePollAnswer poll_answers[kGatewayMessagePollMaximumAnswers];
    GatewayMessagePollAnswerCount poll_counts[kGatewayMessagePollMaximumAnswers];

    GatewayMessageRichScratch view(bool polls) noexcept {
        GatewayMessageRichScratch scratch{
            {reactions, kGatewayMessageReactionMaximum},
            {attachments, kGatewayMessageAttachmentMaximum},
            {embeds, kGatewayMessageEmbedMaximum},
            {embed_fields, kGatewayMessageEmbedFieldMaximum},
            {stickers, kGatewayMessageStickerMaximum},
            {components, kGatewayMessageComponentMaximum},
            {{snapshot_attachments, kGatewayMessageAttachmentMaximum},
             {snapshot_embeds, kGatewayMessageEmbedMaximum},
             {snapshot_embed_fields, kGatewayMessageEmbedFieldMaximum},
             {snapshot_stickers, kGatewayMessageStickerMaximum},
             {snapshot_components, kGatewayMessageComponentMaximum},
             {snapshot_users, kGatewayMessageMentionUserMaximum}},
            {}};
        if (polls)
            scratch.poll = {{poll_answers, kGatewayMessagePollMaximumAnswers},
                            {poll_counts, kGatewayMessagePollMaximumAnswers}};
        return scratch;
    }
};

// Everything one MESSAGE_CREATE/UPDATE route borrows: the rich arrays plus the mention user and member role
// lists. The gateway worker owns one per client, so routing a message costs no stack probe.
struct GatewayMessageDecodeScratch {
    MessageEventScratch rich;
    GatewayMessageNamedUser mention_users[kGatewayMessageMentionUserMaximum];
    u64 member_roles[kGatewayMessageMemberRoleMaximumIds];
};

} // namespace maicord
