#pragma once

#include "maicord/media/asset.hpp"
#include "maicord/model/message_rich_store.hpp"
#include "maicord/ui/geometry.hpp"

namespace maicord {
struct UiResolvedThemeStyles;

inline constexpr u32 kMessageRichAttachmentItemOffset = 0;
inline constexpr u32 kMessageRichEmbedItemOffset =
    kMessageRichAttachmentItemOffset + kGatewayMessageAttachmentMaximum;
inline constexpr u32 kMessageRichStickerItemOffset =
    kMessageRichEmbedItemOffset + kGatewayMessageEmbedMaximum;
inline constexpr u32 kMessageRichComponentItemOffset =
    kMessageRichStickerItemOffset + kGatewayMessageStickerMaximum;
inline constexpr u32 kMessageRichNoticeItemOffset =
    kMessageRichComponentItemOffset + kGatewayMessageComponentMaximum;
// Invite cards for discord.gg links in the content (coded links).
inline constexpr u32 kMessageInviteCardMaximum = 3;
inline constexpr u32 kMessageRichInviteItemOffset = kMessageRichNoticeItemOffset + 1u;
inline constexpr u32 kMessageRichItemMaximum = kMessageRichInviteItemOffset + kMessageInviteCardMaximum;
inline constexpr float kMessageInviteCardHeight = 106.0f;
inline constexpr float kMessageInviteCardMaximumWidth = 432.0f;
inline constexpr float kMessageInviteCardGap = 8.0f;
inline constexpr float kDefaultMessageRichBlockGapDips = 8.0f;
// Thread starter card below the reactions: name line and a metadata line.
inline constexpr float kMessageThreadCardGapDips = 6.0f;
inline constexpr float kMessageThreadCardHeightDips = 50.0f;
inline constexpr float kMessageThreadCardMaximumWidthDips = 432.0f;
inline constexpr float kMessageThreadCardPaddingDips = 12.0f;

struct MessageReactionLayoutMetrics {
    float button_width;
    float button_height;
    float gap;
    float content_left_padding;
    float content_top_padding;
    float content_right_padding;
    float unicode_text_inset;
    float emoji_extent;
    float emoji_gap;
};

inline constexpr MessageReactionLayoutMetrics kDefaultMessageReactionLayoutMetrics{
    80.0f, 24.0f, 4.0f, 6.0f, 2.0f, 6.0f, 0.0f, 16.0f, 4.0f};

struct MessageStickerLayoutMetrics {
    float preview_extent;
    float caption_gap;
    float minimum_caption_extent;

    constexpr float caption_offset() const noexcept {
        return preview_extent + caption_gap;
    }
    constexpr float minimum_height() const noexcept {
        return caption_offset() + minimum_caption_extent;
    }
};

inline constexpr MessageStickerLayoutMetrics kDefaultMessageStickerLayoutMetrics{64.0f, 8.0f, 24.0f};

struct MessageRichLayout {
    float attachments;
    float embeds;
    float stickers;
    float components;
    float reactions;
    float notice;
    float thread;
    float invites;

    constexpr float total() const noexcept {
        return attachments + embeds + stickers + components + reactions + notice + thread + invites;
    }
};

enum class MessageRichItemKind : u8 {
    attachment,
    embed,
    sticker,
    component,
    notice,
    poll,
    invite,
};

// Invite card parts inside its item rectangle.
struct MessageInviteCardGeometry {
    UiRect card, header, icon, name, detail, button;
};
constexpr MessageInviteCardGeometry message_invite_card_geometry(UiRect item) noexcept {
    MessageInviteCardGeometry g{};
    const float right = item.left + kMessageInviteCardMaximumWidth < item.right
                            ? item.left + kMessageInviteCardMaximumWidth
                            : item.right;
    g.card = {item.left, item.top, right, item.top + kMessageInviteCardHeight};
    g.header = {g.card.left + 16.0f, g.card.top + 10.0f, g.card.right - 16.0f, g.card.top + 32.0f};
    g.icon = {g.card.left + 16.0f, g.card.top + 40.0f, g.card.left + 66.0f, g.card.top + 90.0f};
    g.button = {g.card.right - 96.0f, g.card.top + 49.0f, g.card.right - 16.0f, g.card.top + 81.0f};
    const float text_right = g.button.left - 12.0f > g.icon.right + 17.0f ? g.button.left - 12.0f
                                                                          : g.icon.right + 17.0f;
    g.name = {g.icon.right + 16.0f, g.card.top + 42.0f, text_right, g.card.top + 64.0f};
    g.detail = {g.icon.right + 16.0f, g.card.top + 66.0f, text_right, g.card.top + 86.0f};
    return g;
}

struct MessageRichItemLayout {
    float heights[kMessageRichItemMaximum];
    float component_widths[kGatewayMessageComponentMaximum];
    float embed_media_tops[kGatewayMessageEmbedMaximum];
    float embed_media_widths[kGatewayMessageEmbedMaximum];
    float embed_media_heights[kGatewayMessageEmbedMaximum];
    float embed_widths[kGatewayMessageEmbedMaximum];
    // Author and footer rows, relative to the item's content top (the text
    // pass measures them; paint and media requests place the icons there).
    float embed_author_tops[kGatewayMessageEmbedMaximum];
    float embed_footer_tops[kGatewayMessageEmbedMaximum];
    // Measured width of the reply preview line (avatar gap excluded); paint
    // puts the trailing attachment/sticker/command/voice icon after it.
    float reply_width;
};

// Reply preview: the text starts after the 16-DIP avatar and
// its 4-DIP margin; a trailing 20-DIP icon follows it 4 DIPs later.
inline constexpr float kMessageReplyTextInset = 20.0f;
inline constexpr float kMessageReplyIcon = 20.0f;
inline constexpr float kMessageReplyIconGap = 4.0f;

// Embed card: 4-DIP accent border, 12-DIP start and 16-DIP end padding, 2-DIP
// top padding plus an 8-DIP margin above every block, 16 DIPs above large media
// and 16 DIPs below the last block; a components embed (a container) pads 16
// DIPs above and below its content. The corner thumbnail (at most 80x80) sits
// 16 DIPs right of the text column; author icons are 24 DIPs and footer icons
// 20 DIPs, 8 DIPs before their text, both round.
inline constexpr float kMessageEmbedMaximumWidth = 520.0f;
inline constexpr float kMessageEmbedAccentWidth = 4.0f;
inline constexpr float kMessageEmbedContentInset = 16.0f; // card left to text (accent + 12)
inline constexpr float kMessageEmbedEndInset = 16.0f;
inline constexpr float kMessageEmbedTopInset = 2.0f;
inline constexpr float kMessageEmbedBlockGap = 8.0f;
inline constexpr float kMessageEmbedMediaGap = 16.0f;
inline constexpr float kMessageEmbedBottomInset = 16.0f;
inline constexpr float kMessageEmbedContainerInset = 16.0f;
inline constexpr float kMessageEmbedThumbnailExtent = 80.0f;
inline constexpr float kMessageEmbedThumbnailGap = 16.0f;
inline constexpr float kMessageEmbedAuthorIcon = 24.0f;
inline constexpr float kMessageEmbedFooterIcon = 20.0f;
inline constexpr float kMessageEmbedIconGap = 8.0f;
// The item rectangle includes this gap below the card.
inline constexpr float kMessageEmbedItemGap = 6.0f;
// Space above the first block's 8-DIP margin.
constexpr float message_rich_embed_top_inset(bool components) noexcept {
    return components ? kMessageEmbedContainerInset - kMessageEmbedBlockGap : kMessageEmbedTopInset;
}
constexpr float message_rich_embed_media_left(bool media_only) noexcept {
    return media_only ? 0.0f : kMessageEmbedContentInset;
}

// Which parts of one embed are shown.
struct MessageRichEmbedPlan {
    bool media_only;       // bare image/video/gifv without a card
    bool markdown;         // only rich embeds parse markdown; others show raw text
    bool description;      // image, video and gifv embeds hide their description
    bool fields;           // rich and poll_result embeds keep fields
    bool corner_thumbnail; // rich: no video; other types: no large media at all
    bool components;       // components embed: description at message size
};

struct MessageRichEmbedMediaSelection {
    const MessageRichEmbedMedia* media;
    Utf8View source;
    Utf8View fallback_source;
    MediaEmbedResource resource;
    MediaAttachmentFormat format;
    bool animated;
    bool provider = false;
    u32 start_seconds = 0;
    const MessageRichEmbedMedia* playback_extent = nullptr;
};

// "Chat" display settings (the text_and_images settings proto). Process
// wide like the hour cycle: layout, paint and media requests read one policy.
enum class MessageMediaAnimation : u8 { always, interaction, never };
enum class MessageSpoilerReveal : u8 { on_click, moderator, always };
struct MessageRichDisplay {
    bool attachment_media = true; // inline_attachment_media: previews, else file cards
    bool embed_media = true;      // inline_embed_media: link images and video
    bool embeds = true;           // render_embeds
    bool reactions = true;        // render_reactions
    MessageMediaAnimation gifs = MessageMediaAnimation::always;
    MessageMediaAnimation emoji = MessageMediaAnimation::always;
    MessageMediaAnimation stickers = MessageMediaAnimation::always;
    MessageSpoilerReveal spoilers = MessageSpoilerReveal::on_click;
    bool convert_emoticons = true;
};
constexpr bool message_rich_display_equal(const MessageRichDisplay& a, const MessageRichDisplay& b) noexcept {
    return a.attachment_media == b.attachment_media && a.embed_media == b.embed_media && a.embeds == b.embeds &&
           a.reactions == b.reactions && a.gifs == b.gifs && a.emoji == b.emoji && a.stickers == b.stickers &&
           a.spoilers == b.spoilers && a.convert_emoticons == b.convert_emoticons;
}
// True when an animation should play: always, or while its message is
// hovered or focused in interaction mode. Paused media shows its first frame.
constexpr bool message_media_animates(MessageMediaAnimation mode, bool interacting) noexcept {
    return mode == MessageMediaAnimation::always || (mode == MessageMediaAnimation::interaction && interacting);
}
const MessageRichDisplay& message_rich_display() noexcept;
// Returns true when anything that changes row geometry changed.
bool message_rich_set_display(const MessageRichDisplay&) noexcept;

bool message_rich_embed_resource_select(const MessageRichStore&, const MessageRichEmbed&,
                                        MediaEmbedResource, MessageRichEmbedMediaSelection*) noexcept;
bool message_rich_embed_thumbnail_extent(const MessageRichStore&, const MessageRichEmbed&,
                                         float available_width, float* width, float* height) noexcept;

// Card bounds inside the ThreadCard widget rectangle.
constexpr UiRect message_thread_card_rectangle(UiRect widget) noexcept {
    const float right = widget.left + kMessageThreadCardMaximumWidthDips;
    return {widget.left, widget.top + kMessageThreadCardGapDips, widget.right < right ? widget.right : right,
            widget.top + kMessageThreadCardGapDips + kMessageThreadCardHeightDips};
}
// "12 messages · 3 members · Archived"; returns bytes written (0 if nothing known).
u32 message_thread_card_detail(const MessageRichThread& thread, Slice<char> output) noexcept;

bool message_rich_layout_valid(const MessageRichLayout& layout) noexcept;
bool message_rich_layout_equal(const MessageRichLayout& left, const MessageRichLayout& right) noexcept;
bool message_rich_item_layout_valid(const MessageRichItemLayout& layout) noexcept;
bool message_rich_item_layout_equal(const MessageRichItemLayout& left,
                                    const MessageRichItemLayout& right) noexcept;
bool message_rich_item_slot(MessageRichItemKind kind, u32 index, u32* slot) noexcept;

MediaAttachmentFormat message_rich_attachment_format(const MessageRichStore& store,
                                                     const MessageRichAttachment& attachment) noexcept;
bool message_rich_attachment_preview_extent(const MessageRichStore& store,
                                            const MessageRichAttachment& attachment, float available_width,
                                            float* width, float* height) noexcept;
bool message_rich_embed_media_select(const MessageRichStore& store, const MessageRichEmbed& embed,
                                     MessageRichEmbedMediaSelection* selection) noexcept;
// Explicit play intent; ordinary layout and prefetch continue using the poster.
bool message_rich_embed_music_preview(const MessageRichStore&, const MessageRichEmbed&) noexcept;
Utf8View message_rich_embed_provider_label(const MessageRichStore&, const MessageRichEmbed&) noexcept;
bool message_rich_embed_playback_select(const MessageRichStore&, const MessageRichEmbed&,
                                       MessageRichEmbedMediaSelection*) noexcept;
bool message_rich_embed_media_only(const MessageRichEmbed& embed) noexcept;

// Embeds that share a url (multi-photo posts: one card plus image-only embeds)
// merge into the first: later ones render nothing and their images join the
// first's gallery. Presentation only; stored data is unchanged. The gallery is the
// media block, two equal columns with images covering their tiles: image 1 left,
// 2 right, then 3 below 2 (three images) or 3 below 1 and 4 below 2 (four). Only
// the first kMessageEmbedGalleryDrawn images are drawn.
inline constexpr u32 kMessageEmbedGalleryDrawn = 4;
inline constexpr float kMessageEmbedGalleryHeight = 300.0f;
inline constexpr float kMessageEmbedGalleryGap = 4.0f;
struct MessageRichEmbedGallery {
    u8 members[kGatewayMessageEmbedMaximum]; // embed indices, host first when it has an image
    u8 count;
};
// First embed sharing `index`'s url (index itself when it is first or has none).
u32 message_rich_embed_merge_host(const MessageRichStore&, const MessageRichRecord&, u32 index) noexcept;
// The images of a host's gallery; false (count 0) when the host shows none: no
// merged image, media hidden, or a video (the video plays instead).
bool message_rich_embed_gallery(const MessageRichStore&, const MessageRichRecord&, u32 host,
                                MessageRichEmbedGallery*) noexcept;
// A gallery image: the embed's image, else an article/image thumbnail.
bool message_rich_embed_gallery_select(const MessageRichStore&, const MessageRichEmbed&,
                                       MessageRichEmbedMediaSelection*) noexcept;
// Gallery width for the host: the first image's fitted width plus insets when
// that is at least 300 DIPs, else 520 wide.
float message_rich_embed_gallery_width(const MessageRichStore&, const MessageRichEmbed& host,
                                       float available_width) noexcept;
// Tile `ordinal` (< kMessageEmbedGalleryDrawn) of a gallery of `count` images.
UiRect message_rich_embed_gallery_tile(UiRect gallery, u32 count, u32 ordinal) noexcept;
// Where embed `index` draws as a gallery tile: its host and ordinal; false when
// it is not a drawn gallery image.
struct MessageRichEmbedGalleryPlace {
    u32 host;
    u32 ordinal;
    u32 count;
};
bool message_rich_embed_gallery_place(const MessageRichStore&, const MessageRichRecord&, u32 index,
                                      MessageRichEmbedGalleryPlace*) noexcept;
// The media block of a laid-out embed (its own media, or the whole gallery).
UiRect message_rich_embed_media_rect(const MessageRichItemLayout&, u32 index, UiRect content,
                                     bool media_only) noexcept;
MessageRichEmbedPlan message_rich_embed_plan(const MessageRichStore& store, const MessageRichEmbed& embed) noexcept;
// Proxied author/footer icon URLs (empty when the embed shows none).
Utf8View message_rich_embed_author_icon(const MessageRichStore& store, const MessageRichEmbed& embed) noexcept;
Utf8View message_rich_embed_footer_icon(const MessageRichStore& store, const MessageRichEmbed& embed) noexcept;
// Icon rectangles of a laid-out embed item; false (and an empty rectangle)
// when that icon is absent. `item` is the item node's rectangle and `content`
// its content rectangle.
bool message_rich_embed_author_icon_rect(const MessageRichStore& store, const MessageRichEmbed& embed,
                                         const MessageRichItemLayout& layout, u32 index, UiRect content,
                                         UiRect* output) noexcept;
bool message_rich_embed_footer_icon_rect(const MessageRichStore& store, const MessageRichEmbed& embed,
                                         const MessageRichItemLayout& layout, u32 index, UiRect content,
                                         UiRect* output) noexcept;
// Decoder for an author/footer icon URL (proxy URLs without an extension
// still serve PNG); part of the icon's URL-keyed media identity.
MediaAttachmentFormat message_rich_embed_icon_format(Utf8View url) noexcept;
// Corner thumbnail rectangle inside a card of the given bounds.
UiRect message_rich_embed_thumbnail_rect(UiRect card, float width, float height, bool components) noexcept;
bool message_rich_embed_suppresses_content(const MessageRichStore& store, const MessageRichRecord& rich,
                                           Utf8View content) noexcept;
bool message_rich_embed_media_extent(const MessageRichEmbedMediaSelection& selection, float available_width,
                                     float* width, float* height) noexcept;
float message_rich_embed_visual_height(const MessageRichStore& store, const MessageRichRecord& rich,
                                       const MessageRichEmbed& embed) noexcept;
float message_rich_component_visual_height(const MessageRichComponent& component) noexcept;
float message_rich_reaction_button_width(const MessageRichReaction& reaction) noexcept;
float message_rich_reaction_layout_height(const MessageRichRecord& rich, float available_width,
                                          ConstSlice<float> intrinsic_widths = {}) noexcept;
MessageRichItemLayout message_rich_estimate_item_layout(const MessageRichStore* store,
                                                        const MessageRichRecord* rich,
                                                        float available_width,
                                                        const UiResolvedThemeStyles* = nullptr) noexcept;
MessageRichLayout message_rich_aggregate_layout(const MessageRichRecord& rich,
                                                const MessageRichItemLayout& items, float available_width,
                                                ConstSlice<float> reaction_widths = {}) noexcept;
MessageRichLayout message_rich_measure_layout(const MessageRichStore* store, const MessageRichRecord* rich,
                                              float available_width,
                                              const UiResolvedThemeStyles* = nullptr) noexcept;

} // namespace maicord
