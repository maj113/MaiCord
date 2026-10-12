#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/ui/geometry.hpp"

namespace maicord {

struct MessageRichStore;
struct MessageRichAttachment;
struct MessageRichEmbed;
struct MessageRichComponent;

// Explicit content redaction (text_and_images.explicit_content_settings):
// 1 SHOW, 2 BLUR, 3 BLOCK; unset resolves per context in the settings host.
enum class MessageExplicitFilter : u8 { show, blur, block };

// Why a received media item is not shown as-is.
enum class MessageMediaObscure : u8 {
    none,
    spoiler,          // blurred behind "Spoiler" until clicked
    explicit_content, // blurred behind "Explicit content" until clicked
    blocked,          // replaced by a notice; never revealed
    plugin_hidden,    // a plugin hid the media: a notice like blocked
};
// Notice boxes replace the preview entirely.
constexpr bool message_media_obscure_notice(MessageMediaObscure obscure) noexcept {
    return obscure == MessageMediaObscure::blocked || obscure == MessageMediaObscure::plugin_hidden;
}

enum class MessageMediaSource : u8 { attachment, embed, component };

inline constexpr u32 kMessageMediaRevealMaximum = 64;
// Blur deviation (DIP) of obscured previews; the media shader caps it to the
// preview size.
inline constexpr float kMessageMediaObscureBlur = 28.0f;

// Session-only reveal memory: a bounded ring of item keys. Revealing never
// persists, and a channel switch keeps earlier reveals until the ring wraps.
struct MessageMediaReveals {
    u64 keys[kMessageMediaRevealMaximum];
    u32 count;
    u32 cursor;
    u32 revision;
};

constexpr u64 message_media_reveal_key(u64 message_id, MessageMediaSource source, u32 index) noexcept {
    u64 value = message_id * 0x9E3779B97F4A7C15ull;
    value ^= (static_cast<u64>(source) + 1u) << 56u;
    value ^= static_cast<u64>(index + 1u) * 0xBF58476D1CE4E5B9ull;
    return value ? value : 1u;
}

bool message_media_revealed(const MessageMediaReveals& reveals, u64 key) noexcept;
// Returns true when the key was newly revealed (and bumps revision).
bool message_media_reveal(MessageMediaReveals* reveals, u64 key) noexcept;

// spoilers_always mirrors the "show spoilers" chat setting (render_spoilers
// ALWAYS). Explicit filtering wins over a spoiler: blocked media stays hidden.
MessageMediaObscure message_media_obscure(const MessageMediaReveals& reveals, u64 key, bool spoiler,
                                          bool explicit_media, MessageExplicitFilter filter,
                                          bool spoilers_always) noexcept;

// Spoiler bit: attachment flags IS_SPOILER or the SPOILER_ filename prefix.
bool message_rich_attachment_spoiler(const MessageRichStore& store, const MessageRichAttachment& attachment) noexcept;
bool message_rich_attachment_explicit(const MessageRichAttachment& attachment) noexcept;
bool message_rich_embed_explicit(const MessageRichEmbed& embed) noexcept;
bool message_rich_component_spoiler(const MessageRichComponent& component) noexcept;

// Label shown over an obscured preview.
Utf8View message_media_obscure_label(MessageMediaObscure obscure) noexcept;

// Centred label pill over an obscured preview (empty when it cannot fit).
constexpr UiRect message_media_obscure_pill(UiRect media, MessageMediaObscure obscure = MessageMediaObscure::spoiler) noexcept {
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
