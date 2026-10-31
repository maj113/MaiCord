#pragma once

#include "maicord/ui/paint.hpp"

namespace maicord {
struct MessageScene;

struct MessageTextPresentation;

enum class MessageMediaCommandKind : u8 {
    attachment,
    embed,
    sticker,
    custom_emoji,
    regional_flag,
    component, // thumbnail/gallery image keyed by URL hash (message_component_media)
};

struct MessageMediaCommand {
    u64 id;
    UiRect rectangle;
    UiRect clip;
    UiWidgetReference owner;
    u32 message_revision;
    MessageMediaCommandKind kind;
    u8 format;
    u8 embed_index;
    u8 embed_resource;
    bool animated;
    bool ordinary_video; // Primary MP4 preview, excluding every GIFV embed.
    bool snapshot;
    bool search_result;
    // Non-zero: the rectangle covers a centred square tile (search media grid);
    // the renderer masks the media to that tile with this corner radius (DIP).
    u8 tile_radius;
    // MessageMediaObscure: spoiler/explicit previews draw blurred.
    u8 obscure;
    // Non-zero: the media covers the rectangle (an embed gallery tile), cropped
    // to it, instead of being stretched to it.
    u8 cover;
    // Non-zero: a GIF (a .gif or a looping GIFV) that offers a favourite star.
    u8 gif;
    // A provider video embed with a page: its play control is a pill that
    // also holds an open-in-browser control (ui_video_embed_actions).
    bool open_link;
};
static_assert(sizeof(MessageMediaCommand) == 80, "Keep media commands compact: they fill a per-frame scratch array.");

struct MessageMediaCommandStream {
    Slice<MessageMediaCommand> storage;
    u32 count;
    u32 rejected;
};

// A playing embed can have a different aspect ratio from its poster. Fit its
// actual decoded pixels inside the stable message rectangle without cropping.
UiRect message_media_picture_rectangle(const MessageMediaCommand&, u32 width, u32 height,
                                      bool playing_embed) noexcept;

struct MessagePresentationResult {
    u32 geometry_quads;
    u32 media_commands;
};

bool message_media_command_stream_initialize(
    MessageMediaCommandStream* stream,
    Slice<MessageMediaCommand> storage) noexcept;
// `text` (the scene's text presentation, optional) adds the boxes of framed
// code blocks.
bool message_presentation_build(
    const MessageScene& scene, const Theme& theme, PaintStream* paint,
    MessageMediaCommandStream* media,
    MessagePresentationResult* result, const MessageTextPresentation* text = nullptr) noexcept;

} // namespace maicord
