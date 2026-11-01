#pragma once
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
inline constexpr u32 kMessageCardExcerptMaximumBytes = 1024;
struct MessageCardExcerptInput {
    Utf8View outer{}, quoted{};
    bool forwarded{}, summarize_media{};
    u32 attachments{}, embeds{}, stickers{}, components{};
};
struct MessageCardExcerptResult { u16 bytes{}; bool truncated{}; u16 quoted_offset{}, quoted_bytes{}; };
// A card summary is plain text, never a conversation event or mention parser.
// Reserve room for the quoted portion when an outer caption is also present.
__declspec(noinline) inline MessageCardExcerptResult message_card_excerpt(MessageCardExcerptInput input,
                                                                          Slice<char> output) noexcept {
    if (!output.data || !output.count || !utf8_validate(input.outer).valid ||
        !utf8_validate(input.quoted).valid) return {};
    const u32 capacity = minimum(output.count, kMessageCardExcerptMaximumBytes);
    u32 used = 0;
    u16 quoted_offset = 0, quoted_bytes = 0;
    bool truncated = false;
    const auto append = [&](Utf8View value, u32 limit) noexcept {
        u32 bytes = minimum(value.bytes, minimum(limit, capacity - used));
        while (bytes && bytes < value.bytes && (static_cast<u8>(value.data[bytes]) & 0xc0u) == 0x80u) --bytes;
        if (bytes) copy_bytes(output.data + used, value.data, bytes);
        used += bytes; truncated |= bytes != value.bytes;
    };
    const bool media = input.summarize_media && (input.attachments || input.embeds || input.stickers || input.components);
    const u32 reserve = media ? minimum(112u, capacity / 3) : 0;
    if (input.outer.bytes) {
        const u32 forward_header = input.forwarded ? minimum(18u, capacity - reserve) : 0;
        append(input.outer, input.forwarded && input.quoted.bytes ? capacity / 3 : capacity - reserve - forward_header);
    }
    if (input.forwarded) {
        if (used) append(utf8_literal("\n"), capacity - used);
        append(utf8_literal("Forwarded message"), capacity - used);
        if (input.quoted.bytes) {
            append(utf8_literal("\n> "), capacity - used);
            quoted_offset = static_cast<u16>(used);
            append(input.quoted, capacity > used + reserve ? capacity - used - reserve : 0);
            quoted_bytes = static_cast<u16>(used - quoted_offset);
        }
    }
    if (media) {
        if (used) append(utf8_literal("\n"), capacity - used);
        const u32 counts[]{input.attachments, input.embeds, input.stickers, input.components};
        const Utf8View singular[]{utf8_literal("attachment"), utf8_literal("embed"), utf8_literal("sticker"), utf8_literal("component")};
        const Utf8View plural[]{utf8_literal("attachments"), utf8_literal("embeds"), utf8_literal("stickers"), utf8_literal("components")};
        bool first = true;
        for (u32 at = 0; at < 4; ++at) {
            if (!counts[at]) continue;
            if (!first) append(utf8_literal(" · "), capacity - used);
            first = false;
            char digits[10];
            append({digits, decimal_write(digits, counts[at])}, capacity - used);
            append(utf8_literal(" "), capacity - used);
            append(counts[at] == 1 ? singular[at] : plural[at], capacity - used);
        }
    }
    return {static_cast<u16>(used), truncated, quoted_offset, quoted_bytes};
}
} // namespace maicord
