#include "application_emoji_tooltip.hpp"
#include "application_expression_card.hpp"
#include "application_state.hpp"
#include "account_ui_runtime.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/asset.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/ui/emoji_catalog.hpp"
#include "maicord/ui/message_text_presentation.hpp"

namespace maicord::app_detail {
namespace {
// The official client shows expression tooltips 300 ms after the pointer enters.
constexpr u32 kExpressionTooltipDelayMs = 300;
// Rich tooltip: at most 272 DIPs wide, the emoji's column beside the text.
constexpr float kEmojiTooltipMaximumWidth = 272;
constexpr float kStickerIconExtent = 16;
// Distinct from every other message-scene action: the high bit, the command
// index and the emoji's byte offset in its text; stickers by their slot.
constexpr u32 kEmojiSource = 0x80000000u;
constexpr u32 kStickerSource = 0x5b00u;
constexpr auto kLearnMore = utf8_literal("Click to learn more");

// The offered label; offer copies it.
char label[kUiTooltipMaximumTextBytes];
TextStyleSpan bold;

struct Writer {
    Slice<char> output;
    u32 at;
    bool ok;
    void add(Utf8View text) noexcept {
        if (!ok || at > output.count || text.bytes > output.count - at) {
            ok = false;
            return;
        }
        copy_bytes(output.data + at, text.data, text.bytes);
        at += text.bytes;
    }
};
// ":name:", or the bare name.
u32 compose(Utf8View name, bool colons, Slice<char> output) noexcept {
    Writer w{output, 0, name.bytes != 0};
    if (colons)
        w.add(utf8_literal(":"));
    w.add(name);
    if (colons)
        w.add(utf8_literal(":"));
    return w.ok ? w.at : 0;
}
// The name in a <:name:id> or <a:name:id> tag.
Utf8View tag_name(Utf8View tag) noexcept {
    u32 at = tag.bytes > 1 && tag.data[0] == '<' ? (tag.data[1] == 'a' ? 2u : 1u) : tag.bytes;
    if (at >= tag.bytes || tag.data[at] != ':')
        return {};
    const u32 first = ++at;
    while (at < tag.bytes && tag.data[at] != ':')
        ++at;
    return at < tag.bytes ? Utf8View{tag.data + first, at - first} : Utf8View{};
}
const MessageRecord* current_record(u64 message_id, u32 revision) noexcept {
    const auto* record = application_message_record(message_id);
    return record && record->revision == revision && record->channel_id == g_app.account_ui->active_channel_id
               ? record
               : nullptr;
}
bool emoji_at(UiPoint point, MessageExpression* out) noexcept {
    const auto hit = renderer_message_emoji_hit_test(g_app.renderer, point.x, point.y);
    const auto* text = g_app.renderer.message_text;
    if (hit.kind == MessageEmojiHitKind::none || !text || hit.command_index >= text->command_count ||
        !current_record(hit.message_id, hit.message_revision))
        return false;
    const auto& command = text->commands[hit.command_index];
    if (hit.byte_offset > command.text.bytes || hit.byte_count > command.text.bytes - hit.byte_offset)
        return false;
    MessageExpression e{};
    e.owner = command.owner;
    e.anchor = hit.rectangle;
    e.message_id = hit.message_id;
    e.message_revision = hit.message_revision;
    e.source = kEmojiSource | ((hit.command_index & 0x7fffu) << 16) | (hit.byte_offset & 0xffffu);
    e.glyph = {command.text.data + hit.byte_offset, hit.byte_count};
    if (hit.kind == MessageEmojiHitKind::unicode) {
        e.kind = MessageExpressionKind::unicode_emoji;
    } else {
        if (hit.semantic_index >= command.semantics.count)
            return false;
        const auto& semantic = command.semantics[hit.semantic_index];
        e.id = semantic.target;
        if (semantic.kind == MessageMarkdownSemanticKind::regional_flag) {
            e.kind = MessageExpressionKind::regional_flag;
        } else {
            e.kind = MessageExpressionKind::custom_emoji;
            e.animated = (semantic.reserved & 1u) != 0;
            e.source_offset = semantic.source_offset;
            e.source_bytes = semantic.source_bytes;
            e.glyph = {};
        }
    }
    *out = e;
    return true;
}
bool sticker_at(UiPoint point, MessageExpression* out) noexcept {
    MessageStickerHit hit{};
    if (!message_scene_sticker_hit(g_app.message_scene, point.x, point.y, &hit))
        return false;
    const auto* record = application_message_record(hit.message_id);
    if (!record || record->channel_id != g_app.account_ui->active_channel_id)
        return false;
    // A forwarded message's stickers are its snapshot's.
    const auto& store = g_app.account_ui->message_rich_store;
    const auto* rich =
        hit.snapshot ? message_rich_store_snapshot(store, record->id) : message_rich_store_find(store, record->id);
    if (!rich || rich->message_revision != record->revision || hit.index >= rich->sticker_count)
        return false;
    MessageExpression e{};
    e.owner = {&g_app.message_scene.widgets, hit.widget};
    e.anchor = hit.preview;
    e.message_id = record->id;
    e.message_revision = record->revision;
    e.source = kStickerSource + (hit.snapshot ? kGatewayMessageStickerMaximum : 0u) + hit.index;
    e.kind = MessageExpressionKind::sticker;
    e.id = rich->stickers[hit.index].id;
    e.sticker_format = rich->stickers[hit.index].format_type;
    e.sticker_index = static_cast<u8>(hit.index);
    e.snapshot = hit.snapshot;
    *out = e;
    return true;
}
} // namespace

bool application_message_expression_at(UiPoint point, MessageExpression* output) noexcept {
    if (!output || !g_app.account_ui)
        return false;
    return emoji_at(point, output) || sticker_at(point, output);
}

u32 application_message_expression_name(const MessageExpression& e, Slice<char> output) noexcept {
    if (!g_app.account_ui || !output.data)
        return 0;
    const auto& ui = *g_app.account_ui;
    switch (e.kind) {
    case MessageExpressionKind::unicode_emoji:
    case MessageExpressionKind::regional_flag: {
        char shortcode[kUnicodeEmojiMaximumNameBytes + 8];
        const u32 bytes = emoji_shortcode_key(e.glyph, {shortcode, sizeof(shortcode)});
        return bytes ? compose({shortcode, bytes}, true, output) : 0;
    }
    case MessageExpressionKind::custom_emoji: {
        // Known (its server's record): bare unless the server requires colons;
        // otherwise the message's tag, with colons.
        if (const auto* emoji = chat_state_store_find_emoji(ui.chat_state, e.id))
            return compose(chat_state_store_emoji_name(*emoji), (emoji->flags & custom_emoji_require_colons) != 0,
                           output);
        const MessageTextHit source{e.message_id, e.id, e.message_revision, e.source_offset, e.source_bytes,
                                    MessageMarkdownSemanticKind::custom_emoji, true};
        return compose(
            tag_name(message_text_presentation_hit_source(g_app.message_view.store, &ui.message_rich_store, source)),
            true, output);
    }
    case MessageExpressionKind::sticker: {
        const auto& store = ui.message_rich_store;
        const auto* rich = e.snapshot ? message_rich_store_snapshot(store, e.message_id)
                                      : message_rich_store_find(store, e.message_id);
        if (!rich || rich->message_revision != e.message_revision || e.sticker_index >= rich->sticker_count)
            return 0;
        return compose(message_rich_store_string(store, rich->stickers[e.sticker_index].name), false, output);
    }
    default:
        return 0;
    }
}

bool application_message_expression_same(const MessageExpression& a, const MessageExpression& b) noexcept {
    return a.kind != MessageExpressionKind::none && a.kind == b.kind && a.message_id == b.message_id &&
           a.message_revision == b.message_revision && a.source == b.source;
}

bool application_emoji_tooltip_target(UiPoint point, u64 scope, UiTooltipTarget* target,
                                      UiTooltipStyle* style) noexcept {
    MessageExpression e{};
    if (!target || !style || application_expression_card_contains(point) ||
        !application_message_expression_at(point, &e) || application_expression_card_shows(e))
        return false;
    const u32 bytes = application_message_expression_name(e, {label, sizeof(label)});
    if (!bytes)
        return false;
    bold = {0, bytes, text_style_bold};
    UiTooltipEmoji emoji{};
    switch (e.kind) {
    case MessageExpressionKind::unicode_emoji:
        if (e.glyph.bytes > kUiTooltipMaximumEmojiBytes)
            return false;
        emoji.glyph = e.glyph;
        break;
    case MessageExpressionKind::regional_flag:
        emoji.media = media_asset_regional_flag(static_cast<u16>(e.id));
        break;
    case MessageExpressionKind::custom_emoji:
        emoji.media = media_asset_custom_emoji(e.id, e.animated);
        break;
    case MessageExpressionKind::sticker:
        emoji.icon = UiIcon::sticker;
        break;
    default:
        return false;
    }
    if (e.kind != MessageExpressionKind::unicode_emoji && e.kind != MessageExpressionKind::sticker &&
        !media_asset_key_valid(emoji.media))
        return false;
    *target = {{e.owner, scope, e.message_id, e.message_revision, e.source},
               e.anchor,
               {label, bytes},
               UiTooltipPlacement::above,
               {&bold, 1},
               emoji,
               kLearnMore};
    const auto& scale = application_theme().scale;
    style->pointer_delay_ms = kExpressionTooltipDelayMs;
    if (e.kind == MessageExpressionKind::sticker) {
        // The official sticker tooltip: the icon leads the name, both lines centred.
        style->emoji_size = kStickerIconExtent;
        style->emoji_gap = scale.space_2 + scale.space_1;
        return true;
    }
    style->padding_x = scale.space_4 + scale.space_2;
    style->padding_y = scale.space_4;
    style->emoji_gap = scale.space_4;
    style->maximum_width = kEmojiTooltipMaximumWidth;
    return true;
}
} // namespace maicord::app_detail
