#include "maicord/app/message_interaction_rules.hpp"
#include "maicord/app/message_media_controller.hpp"
#include "maicord/ui/message_rich_layout.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/ui/unread_divider.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

MessageRichRecord g_records[2];
u16 g_order[2];
u8 g_text[4 * 1024];

bool integral(float value) noexcept {
    const float nearest = static_cast<float>(static_cast<i64>(value + (value < 0 ? -.5f : .5f)));
    return value - nearest < 1e-3f && nearest - value < 1e-3f;
}

void test_rich_display(Check check) noexcept {
    MessageRichStore store{};
    GatewayMessageAttachment attachment{};
    attachment.id = 70;
    attachment.filename = utf8_literal("photo.png");
    attachment.content_type = utf8_literal("image/png");
    attachment.url = utf8_literal("https://cdn.discordapp.com/attachments/7/70/photo.png");
    attachment.width = 800;
    attachment.height = 600;
    attachment.has_width = attachment.has_height = true;
    GatewayMessageEmbed embeds[1]{};
    embeds[0].type = GatewayMessageEmbedType::rich;
    embeds[0].title = utf8_literal("Build");
    embeds[0].description = utf8_literal("green");
    embeds[0].image.url = utf8_literal("https://example.com/card.png");
    embeds[0].image.proxy_url = utf8_literal("https://media.discordapp.net/external/card.png");
    embeds[0].image.width = 640;
    embeds[0].image.height = 360;
    embeds[0].image.has_width = embeds[0].image.has_height = true;
    GatewayMessageReaction reactions[2]{};
    reactions[0].name = utf8_literal("\xF0\x9F\x91\x8D");
    reactions[0].count = 3;
    reactions[1].emoji_id = 44;
    reactions[1].name = utf8_literal("party");
    reactions[1].count = 1;
    GatewayMessageEvent rich{};
    rich.id = 1;
    rich.fields = gateway_message_field_attachments | gateway_message_field_embeds | gateway_message_field_reactions;
    rich.attachments = {&attachment, 1};
    rich.embeds = {embeds, 1};
    rich.reactions = {reactions, 2};
    // A bare image link: only the image shows and the URL text is hidden.
    const Utf8View link = utf8_literal("https://example.com/cat.gif");
    GatewayMessageEmbed image{};
    image.type = GatewayMessageEmbedType::image;
    image.url = link;
    image.thumbnail.url = link;
    image.thumbnail.proxy_url = utf8_literal("https://media.discordapp.net/external/cat.gif");
    image.thumbnail.width = 320;
    image.thumbnail.height = 240;
    image.thumbnail.has_width = image.thumbnail.has_height = true;
    GatewayMessageEvent bare{};
    bare.id = 2;
    bare.fields = gateway_message_field_embeds;
    bare.embeds = {&image, 1};
    const bool ready = message_rich_store_initialize(&store, {g_records, 2}, {g_order, 2}, {g_text, sizeof(g_text)}) &&
                       message_rich_store_apply(&store, 1, 1, rich).ok() &&
                       message_rich_store_apply(&store, 2, 1, bare).ok();
    const auto* record = message_rich_store_find(store, 1);
    const auto* link_record = message_rich_store_find(store, 2);
    check(ready && record && link_record, "chat_display_fixture");
    if (!ready || !record || !link_record)
        return;
    const auto measure = [&](const MessageRichRecord& r) noexcept {
        return message_rich_estimate_item_layout(&store, &r, 520);
    };
    auto items = measure(*record);
    auto layout = message_rich_aggregate_layout(*record, items, 520);
    const float preview = items.heights[kMessageRichAttachmentItemOffset];
    check(message_rich_display_equal(message_rich_display(), MessageRichDisplay{}) && preview > 42 &&
              items.embed_media_heights[0] > 0 && layout.embeds > 0 && layout.reactions == 24 &&
              measure(*link_record).heights[kMessageRichEmbedItemOffset] > 0 &&
              message_rich_embed_suppresses_content(store, *link_record, link),
          "chat_display_defaults_show_previews_embeds_and_reactions");

    MessageRichDisplay display{};
    display.attachment_media = false;
    const bool attachment_geometry = message_rich_set_display(display);
    float width = 0, height = 0;
    items = measure(*record);
    check(attachment_geometry && items.heights[kMessageRichAttachmentItemOffset] == 42 &&
              !message_rich_attachment_preview_extent(store, record->attachments[0], 520, &width, &height) &&
              items.embed_media_heights[0] > 0,
          "chat_display_inline_attachment_media_off_shows_file_cards_without_decoding");

    display = {};
    display.embed_media = false;
    MessageRichEmbedMediaSelection selection{};
    check(message_rich_set_display(display) && measure(*record).embed_media_heights[0] == 0 &&
              measure(*record).heights[kMessageRichEmbedItemOffset] > 0 &&
              !message_rich_embed_media_select(store, record->embeds[0], &selection) &&
              measure(*link_record).heights[kMessageRichEmbedItemOffset] == 0 &&
              !message_rich_embed_suppresses_content(store, *link_record, link),
          "chat_display_inline_embed_media_off_keeps_cards_drops_link_images_and_shows_the_url");

    display = {};
    display.embeds = false;
    const bool embeds_geometry = message_rich_set_display(display);
    items = measure(*record);
    layout = message_rich_aggregate_layout(*record, items, 520);
    check(embeds_geometry && items.heights[kMessageRichEmbedItemOffset] == 0 &&
              layout.embeds == 0 && measure(*link_record).heights[kMessageRichEmbedItemOffset] == 0 &&
              !message_rich_embed_suppresses_content(store, *link_record, link),
          "chat_display_render_embeds_off_collapses_every_embed");

    display = {};
    display.reactions = false;
    const bool reactions_geometry = message_rich_set_display(display);
    layout = message_rich_aggregate_layout(*record, measure(*record), 520);
    check(reactions_geometry && layout.reactions == 0 && message_rich_reaction_layout_height(*record, 520) == 0,
          "chat_display_render_reactions_off_collapses_the_reaction_row");

    display = {};
    const bool restored = message_rich_set_display(display);
    display.gifs = display.emoji = MessageMediaAnimation::interaction;
    display.spoilers = MessageSpoilerReveal::always;
    display.convert_emoticons = false;
    check(restored && !message_rich_set_display(display) &&
              message_rich_display().spoilers == MessageSpoilerReveal::always,
          "chat_display_animation_and_spoiler_modes_change_no_geometry");
    message_rich_set_display({});
}

void test_animation_policy(Check check) noexcept {
    using A = MessageMediaAnimation;
    check(message_media_animates(A::always, false) && !message_media_animates(A::interaction, false) &&
              message_media_animates(A::interaction, true) && !message_media_animates(A::never, true),
          "chat_animation_modes_always_hover_or_focus_and_never");
    MessageMediaController controller{};
    if (!message_media_controller_initialize_owned(&controller, 8, 32)) {
        check(false, "chat_animation_controller_fixture");
        return;
    }
    MessageRichDisplay display{};
    display.emoji = A::interaction;
    display.gifs = A::interaction;
    display.stickers = A::never;
    message_rich_set_display(display);
    const bool idle = !message_media_controller_animation_plays(controller, MediaAssetKind::custom_emoji, 5) &&
                      !message_media_controller_animation_plays(controller, MediaAssetKind::attachment, 5) &&
                      !message_media_controller_animation_plays(controller, MediaAssetKind::embed, 5) &&
                      message_media_controller_animation_plays(controller, MediaAssetKind::user_avatar, 5);
    controller.scan_pending = false;
    const bool entered = message_media_controller_set_interaction(&controller, 5) && controller.scan_pending &&
                         !message_media_controller_set_interaction(&controller, 5);
    const bool hovered = message_media_controller_animation_plays(controller, MediaAssetKind::custom_emoji, 5) &&
                         message_media_controller_animation_plays(controller, MediaAssetKind::attachment, 5) &&
                         !message_media_controller_animation_plays(controller, MediaAssetKind::custom_emoji, 6) &&
                         !message_media_controller_animation_plays(controller, MediaAssetKind::sticker, 5);
    const bool left = message_media_controller_set_interaction(&controller, 0) &&
                      !message_media_controller_animation_plays(controller, MediaAssetKind::custom_emoji, 5) &&
                      !message_media_controller_animation_plays(controller, MediaAssetKind::custom_emoji, 0);
    message_rich_set_display({});
    check(idle && entered && hovered && left &&
              message_media_controller_animation_plays(controller, MediaAssetKind::sticker, 0),
          "chat_animation_interaction_plays_only_the_hovered_message_media_of_each_kind");
    message_media_controller_release(&controller);
}

void test_emoticons(Check check) noexcept {
    char out[128];
    u32 bytes = 0;
    const auto converts = [&](Utf8View input, Utf8View expected) noexcept {
        return composer_emoticons_convert(input, {out, sizeof(out)}, &bytes) && utf8_equal({out, bytes}, expected);
    };
    const auto keeps = [&](Utf8View input) noexcept {
        return !composer_emoticons_convert(input, {out, sizeof(out)}, &bytes) && utf8_equal({out, bytes}, input);
    };
    check(converts(utf8_literal(":) hi <3"), utf8_literal("\xF0\x9F\x99\x82 hi \xE2\x9D\xA4\xEF\xB8\x8F")) &&
              converts(utf8_literal("ok >:(\n;)"), utf8_literal("ok \xF0\x9F\x98\xA0\n\xF0\x9F\x98\x89")) &&
              converts(utf8_literal(":-D"), utf8_literal("\xF0\x9F\x98\x84")),
          "chat_emoticons_convert_whole_words_longest_first");
    check(keeps(utf8_literal("a:) b")) && keeps(utf8_literal(":)x")) && keeps(utf8_literal("`:)`")) &&
              keeps(utf8_literal("```\n:) <3\n```")) && keeps(utf8_literal("https://x.y/a")) &&
              keeps(utf8_literal("plain text")),
          "chat_emoticons_skip_words_code_and_urls");
    check(!composer_emoticons_convert(utf8_literal(":) :) :)"), {out, 6}, &bytes),
          "chat_emoticons_report_overflow");
}

void test_hairlines_and_spine(Check check) noexcept {
    const UiRect divider{10.3f, 100.3f, 610.7f, 124.3f};
    bool crisp = true;
    constexpr float scales[]{1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
    for (const float scale : scales) {
        const auto line = ui_unread_divider_geometry(divider, kDefaultUiUnreadDividerStyle, scale).line;
        const float pixels = (line.bottom - line.top) * scale;
        crisp &= integral(line.top * scale) && integral(line.left * scale) && integral(line.right * scale) &&
                 pixels > .999f && pixels < scale + .501f && integral(pixels) &&
                 line.top <= 112.3f && line.bottom >= 112.3f - 1;
    }
    const float unsnapped = ui_unread_divider_geometry(divider).line.top;
    check(crisp && unsnapped > 111.79f && unsnapped < 111.81f,
          "chat_unread_divider_hairline_snaps_to_whole_device_pixels");

    const UiRect row{0, 100, 240, 132}, icon{36, 108, 52, 124};
    const auto first = shell_navigation_thread_spine(row, 32, icon, true, false);
    const auto middle = shell_navigation_thread_spine(row, 32, icon, false, true);
    check(first.rail_top.x == 32 && first.rail_top.y == 94 && first.arc_start.x == 32 && first.arc_start.y == 108 &&
              first.arc_end.x == 40 && first.arc_end.y == 116 && first.arm_end.x == 48 &&
              first.continue_bottom == first.arc_start.y && middle.rail_top.y == 100 &&
              middle.continue_bottom == 132 && kShellThreadSpineRadius == first.arc_end.x - first.arc_start.x,
          "chat_thread_spine_runs_under_the_parent_icon_and_curves_into_the_row");
}
} // namespace

void test_chat_display(Check check) noexcept {
    test_rich_display(check);
    test_animation_policy(check);
    test_emoticons(check);
    test_hairlines_and_spine(check);
}
} // namespace maicord
