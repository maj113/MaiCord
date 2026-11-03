#include "embed_parity_internal.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/text/unicode/unicode.hpp"
#include "maicord/ui/chat/messages/component_media.hpp"
#include "maicord/ui/chat/messages/interaction.hpp"
#include "maicord/ui/shell/navigation_view.hpp"
#include "../../../support/test_output.hpp"

namespace maicord {
namespace embed_parity_detail {

// An fxtwitter-style link embed, decoded from the Gateway JSON, through the
// text pass and paint: escapes and quotes in the description, invisible
// separators, author/footer icons through the media proxy, the footer
// timestamp, and the video shown without a corner thumbnail.
void test_fxtwitter_embed(Check check) noexcept {
    auto& f = fixture;
    const auto payload = utf8_literal(R"json({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"https://x.com/quill_example/status/1","timestamp":"2026-09-05T10:00:00Z","embeds":[{"type":"rich","url":"https://x.com/quill_example/status/1","color":6765239,"description":"[GOOD NEWS\\!] ⁣⁣Я Comet\\.\\.\\.\n\n> quoted tweet line\n> second line\n\n️️PREVIEW\\.\\.\\. IT SHIPS NOW\\! https://x.com/a_b_c","author":{"name":"quill (…) (@quill_example)","url":"https://x.com/quill_example","icon_url":"https://pbs.twimg.com/profile_images/1/a.jpg","proxy_icon_url":"https://images-ext-1.discordapp.net/external/abc/https/pbs.twimg.com/profile_images/1/a.jpg"},"footer":{"text":"EmbedX","icon_url":"https://embed.example.com/icon.png","proxy_icon_url":"https://images-ext-1.discordapp.net/external/def/https/embed.example.com/icon.png"},"timestamp":"2026-09-04T20:08:00.000000+00:00","provider":{"name":"EmbedX","url":"https://embed.example.com"},"thumbnail":{"url":"https://pbs.twimg.com/media/b.jpg","proxy_url":"https://images-ext-1.discordapp.net/external/ghi/https/pbs.twimg.com/media/b.jpg","width":1280,"height":720,"flags":0},"video":{"url":"https://video.twimg.com/c.mp4","proxy_url":"https://images-ext-1.discordapp.net/external/jkl/https/video.twimg.com/c.mp4","width":1280,"height":720,"flags":0}}]}})json");
    GatewayMessageEvent message{};
    bool ok = decode_message(payload, &message) && message.embeds.count == 1;
    const GatewayMessageEmbed& decoded = message.embeds[0];
    check(ok && utf8_equal(decoded.author_url, utf8_literal("https://x.com/quill_example")) &&
              utf8_starts_with(decoded.author_icon, utf8_literal("https://images-ext-1.discordapp.net/external/abc/")) &&
              utf8_starts_with(decoded.footer_icon, utf8_literal("https://images-ext-1.discordapp.net/external/def/")) &&
              utf8_equal(decoded.provider_name, utf8_literal("EmbedX")) &&
              utf8_equal(decoded.provider_url, utf8_literal("https://embed.example.com")) &&
              decoded.timestamp_ms == 1'788'552'480'000ull && decoded.video.width == 1280 &&
              utf8_starts_with(decoded.video.proxy_url, utf8_literal("https://images-ext-1.discordapp.net/")),
          "embed_decodes_author_footer_provider_timestamp_and_video_through_dispatch");

    ok = ok && initialize_fixture(message);
    const MessageRichRecord* rich = ok ? message_rich_store_find(f.rich, 1) : nullptr;
    const MessageRichEmbed* embed = rich && rich->embed_count == 1 ? &rich->embeds[0] : nullptr;
    MessageRichEmbedMediaSelection selected{};
    check(embed && !message_rich_embed_plan(f.rich, *embed).corner_thumbnail &&
              message_rich_embed_media_select(f.rich, *embed, &selected) &&
              selected.resource == MediaEmbedResource::video && selected.format == MediaAttachmentFormat::mp4,
          "video_embed_uses_thumbnail_only_as_poster");

    ChatStateStore state{};
    NavigationView navigation{};
    constexpr u64 now = 1'788'552'480 + 86'400; // the next day: "Yesterday at"
    const MessageTextCoordinatorRequest request{&f.scene, &f.presentation, &f.cache, &state, nullptr, &navigation,
                                                &kDefaultTheme, finish, nullptr, now, 1};
    ok = ok && embed && message_scene_update(&f.scene, {0, 0, 900, 1000}).ok() &&
         message_text_coordinator_converge(request).ok();
    const MessageSceneRowWidget* row = ok ? message_scene_find_row(f.scene, 0) : nullptr;
    check(ok && row, "fxtwitter_embed_converges");
    if (!row) {
        message_scene_release(&f.scene);
        return;
    }
    const auto& p = f.presentation;
    const auto* provider = command(p, 0x8008);
    const auto* author = command(p, 0x8002);
    const auto* description = command(p, 0x8004);
    const auto* footer = command(p, 0x8005);
    check(provider && utf8_equal(provider->text, utf8_literal("EmbedX")) &&
              utf8_equal(link_target(f, provider), utf8_literal("https://embed.example.com")) &&
              author && utf8_equal(link_target(f, author), utf8_literal("https://x.com/quill_example")) &&
              has_style(author, utf8_literal("quill"), text_style_bold),
          "embed_provider_and_author_link_to_their_urls");
    // The quote is its own block between the two plain runs.
    const auto* quote = command(p, 0x8004, 1);
    const auto* tail = command(p, 0x8004, 2);
    check(description && quote && tail && !command(p, 0x8004, 3) &&
              utf8_starts_with(description->text, utf8_literal("[GOOD NEWS!] ")) &&
              ends_with(description->text, utf8_literal(" Comet...\n")) &&
              utf8_equal(quote->text, utf8_literal("quoted tweet line\nsecond line")) &&
              utf8_starts_with(tail->text, utf8_literal("\n")) &&
              test_contains(tail->text, utf8_literal("PREVIEW... IT SHIPS NOW! ")) &&
              !test_contains(tail->text, utf8_literal("\\")) &&
              has_style(quote, utf8_literal("quoted tweet line"), text_style_quote) &&
              has_style(quote, utf8_literal("second"), text_style_quote) &&
              !has_style(tail, utf8_literal("PREVIEW"), text_style_quote) &&
              utf8_equal(link_target(f, tail), utf8_literal("https://x.com/a_b_c")),
          "embed_description_escapes_quotes_and_links");
    check(footer && utf8_starts_with(footer->text, utf8_literal("EmbedX \xE2\x80\xA2 Yesterday at ")) &&
              has_style(footer, utf8_literal("EmbedX"), text_style_subtext),
          "embed_footer_shows_text_bullet_and_calendar_timestamp");
    check(description && shape_styled(description->text).notdef == 0, "embed_description_invisible_separators_draw_nothing");
    // The official client's embed type scale: provider/footer text-xs (12), author and
    // description text-sm (14), each run shaped at that em.
    check(sized_throughout(provider, text_size_xs) && sized_throughout(author, text_size_sm) &&
              sized_throughout(description, text_size_sm) && sized_throughout(quote, text_size_sm) &&
              sized_throughout(tail, text_size_sm) && sized_throughout(footer, text_size_xs) &&
              shaped_em(footer) == 12 && shaped_em(provider) == 12 && shaped_em(author) == 14 &&
              shaped_em(quote) == 14,
          "embed_text_uses_reference_sizes_per_part");
    // The description quote is indented past its spine inside the card text.
    check(quote && description && quote->rectangle.left == description->rectangle.left + 16 &&
              quote->rectangle.top >= description->rectangle.top + description->measured_height + 4 &&
              tail && tail->rectangle.top >= quote->rectangle.top + quote->measured_height + 4,
          "embed_description_quote_is_an_indented_block");

    // Geometry: icons left of the author/footer text, video below the text,
    // footer last, no corner thumbnail; images keyed by proxied URL.
    const UiLayoutNode* item = message_scene_rich_item_layout_node(f.scene, *row, MessageSceneRichItemKind::embed, 0);
    const MessageRichItemLayout& layout = row->rich_item_layout;
    UiRect author_icon{}, footer_icon{};
    const bool icons = item &&
                       message_rich_embed_author_icon_rect(f.rich, *embed, layout, 0, item->content_rectangle,
                                                           &author_icon) &&
                       message_rich_embed_footer_icon_rect(f.rich, *embed, layout, 0, item->content_rectangle,
                                                           &footer_icon);
    check(icons && ui_rect_width(author_icon) == 24 && ui_rect_width(footer_icon) == 20 &&
              author_icon.left == item->content_rectangle.left + 16 && author && footer &&
              author->rectangle.left == author_icon.right + 8 && footer->rectangle.left == footer_icon.right + 8 &&
              layout.embed_media_widths[0] == 400 && layout.embed_media_heights[0] == 225 &&
              layout.embed_media_tops[0] > layout.embed_author_tops[0] + 24 &&
              layout.embed_footer_tops[0] >= layout.embed_media_tops[0] + 225 + 8,
          "embed_author_media_footer_geometry");
    paint_stream_reset(&f.paint, 900, 1000);
    MessageMediaCommandStream media{};
    MessagePresentationResult painted{};
    bool paint_ok = message_media_command_stream_initialize(&media, {f.media, 32}) &&
                    message_presentation_build(f.scene, kDefaultTheme, &f.paint, &media, &painted);
    u32 videos = 0, thumbnails = 0, round_icons = 0;
    const Utf8View author_url = message_rich_embed_author_icon(f.rich, *embed);
    const Utf8View footer_url = message_rich_embed_footer_icon(f.rich, *embed);
    for (u32 i = 0; paint_ok && i < media.count; ++i) {
        const MessageMediaCommand& c = media.storage[i];
        if (c.kind == MessageMediaCommandKind::embed) {
            videos += c.embed_resource == static_cast<u8>(MediaEmbedResource::video) &&
                              c.rectangle.left == item->content_rectangle.left + 16
                          ? 1u
                          : 0u;
            thumbnails += c.embed_resource == static_cast<u8>(MediaEmbedResource::thumbnail) ? 1u : 0u;
        } else if (c.kind == MessageMediaCommandKind::component) {
            const bool author_match =
                c.id == message_component_media_key(hash64_bytes(kHash64Seed, author_url.data, author_url.bytes),
                                                    MediaAttachmentFormat::jpeg)
                            .id &&
                c.rectangle.left == author_icon.left && c.rectangle.top == author_icon.top && c.tile_radius == 12;
            const bool footer_match =
                c.id == message_component_media_key(hash64_bytes(kHash64Seed, footer_url.data, footer_url.bytes),
                                                    MediaAttachmentFormat::png)
                            .id &&
                c.rectangle.left == footer_icon.left && c.rectangle.top == footer_icon.top && c.tile_radius == 10;
            round_icons += author_match || footer_match ? 1u : 0u;
        }
    }
    check(paint_ok && videos == 1 && thumbnails == 0 && round_icons == 2,
          "embed_paints_video_and_round_icons_without_corner_thumbnail");
    paint_stream_release(&f.paint);
    message_scene_release(&f.scene);
}

// The official client's thumbnail rules per embed type.
void test_thumbnail_rules(Check check) noexcept {
    static MessageRichRecord records[1];
    static u16 order[1];
    static u8 text[4096];
    MessageRichStore store{};
    GatewayMessageEmbed embeds[1]{};
    embeds[0].title = utf8_literal("Title");
    embeds[0].thumbnail.url = utf8_literal("https://e.test/t.png");
    embeds[0].thumbnail.width = embeds[0].thumbnail.height = 128;
    GatewayMessageEvent event{};
    event.id = 5;
    event.fields = gateway_message_field_embeds;
    event.embeds = {embeds, 1};
    const auto plan = [&](GatewayMessageEmbedType type, bool image, bool video,
                          MessageRichEmbedMediaSelection* media) noexcept {
        embeds[0].type = type;
        embeds[0].image = {};
        embeds[0].video = {};
        if (image) {
            embeds[0].image.url = utf8_literal("https://e.test/i.png");
            embeds[0].image.width = embeds[0].image.height = 64;
        }
        if (video) {
            embeds[0].video.url = utf8_literal("https://e.test/v.mp4");
            embeds[0].video.width = embeds[0].video.height = 64;
        }
        if (!message_rich_store_initialize(&store, {records, 1}, {order, 1}, {text, sizeof(text)}) ||
            !message_rich_store_apply(&store, 5, 1, event).ok())
            return MessageRichEmbedPlan{};
        const MessageRichRecord* rich = message_rich_store_find(store, 5);
        *media = {};
        if (rich)
            message_rich_embed_media_select(store, rich->embeds[0], media);
        return rich ? message_rich_embed_plan(store, rich->embeds[0]) : MessageRichEmbedPlan{};
    };
    MessageRichEmbedMediaSelection media{};
    const bool rich_corner = plan(GatewayMessageEmbedType::rich, true, false, &media).corner_thumbnail &&
                             media.resource == MediaEmbedResource::image;
    const bool link_corner = plan(GatewayMessageEmbedType::link, false, false, &media).corner_thumbnail;
    const bool link_media = !plan(GatewayMessageEmbedType::link, true, false, &media).corner_thumbnail;
    const MessageRichEmbedPlan article = plan(GatewayMessageEmbedType::article, false, false, &media);
    const bool article_large = !article.corner_thumbnail && media.resource == MediaEmbedResource::thumbnail;
    const MessageRichEmbedPlan video = plan(GatewayMessageEmbedType::video, false, true, &media);
    check(rich_corner && link_corner && link_media && article_large && !video.corner_thumbnail &&
              !video.description && !video.markdown && media.resource == MediaEmbedResource::video,
          "embed_thumbnail_image_video_selection_per_type");
    // Provider frames are never fed to the MP4 decoder, even if a supplied
    // MIME type claims they are video. A direct proxy still wins over a frame.
    embeds[0].video.url = utf8_literal("https://www.youtube.com/embed/aB_01234-xy");
    embeds[0].video.content_type = utf8_literal("video/mp4");
    message_rich_store_initialize(&store, {records, 1}, {order, 1}, {text, sizeof(text)});
    message_rich_store_apply(&store, 5, 1, event);
    auto* rich = message_rich_store_find(store, 5);
    check(rich && message_rich_embed_media_select(store, rich->embeds[0], &media) &&
          media.resource == MediaEmbedResource::thumbnail && media.format == MediaAttachmentFormat::png,
          "provider iframe remains a poster until media is resolved");
    check(rich && message_rich_embed_playback_select(store, rich->embeds[0], &media) &&
          media.provider && media.resource == MediaEmbedResource::video &&
          utf8_equal(media.source, embeds[0].video.url), "explicit play resolves the YouTube video behind its poster");
    const auto display = message_rich_display();
    auto hidden = display;
    hidden.embed_media = false;
    message_rich_set_display(hidden);
    check(!message_rich_embed_playback_select(store, rich->embeds[0], &media),
          "hidden embeds cannot start provider playback");
    message_rich_set_display(display);
    embeds[0].video.proxy_url = utf8_literal("https://images-ext-1.discordapp.net/external/test/https/e.test/v.mp4");
    message_rich_store_initialize(&store, {records, 1}, {order, 1}, {text, sizeof(text)});
    message_rich_store_apply(&store, 5, 1, event);
    rich = message_rich_store_find(store, 5);
    check(rich && message_rich_embed_media_select(store, rich->embeds[0], &media) &&
          media.resource == MediaEmbedResource::video && media.format == MediaAttachmentFormat::mp4,
          "direct video proxy takes precedence over provider iframe");
    check(rich && message_rich_embed_playback_select(store, rich->embeds[0], &media) && !media.provider,
          "direct proxy playback does not use the provider resolver");
    embeds[0].url = utf8_literal("https://www.pornhub.com/view_video.php?viewkey=683dbe0849d99");
    const Utf8View ph_sources[]{utf8_literal("https://www.pornhub.com/embed/683dbe0849d99"),
        utf8_literal("https://ev.phncdn.com/videos/old.mp4?validto=1")};
    for (const auto source : ph_sources) {
        embeds[0].video.url = source;
        message_rich_store_initialize(&store, {records, 1}, {order, 1}, {text, sizeof(text)});
        message_rich_store_apply(&store, 5, 1, event);
        rich = message_rich_store_find(store, 5);
        check(rich && message_rich_embed_media_select(store, rich->embeds[0], &media) &&
              media.resource == MediaEmbedResource::thumbnail && media.format == MediaAttachmentFormat::png &&
              utf8_equal(media.source, embeds[0].thumbnail.url),
              "PH proxy video never replaces the card thumbnail or starts background MP4 decoding");
        check(rich && message_rich_embed_playback_select(store, rich->embeds[0], &media) && media.provider &&
              utf8_equal(media.source, embeds[0].url) && !media.fallback_source.bytes,
              "PH click resolves fresh media from the canonical page despite a stale video proxy");
    }
    const auto saved_thumbnail = embeds[0].thumbnail;
    embeds[0].type = GatewayMessageEmbedType::link;
    embeds[0].url = utf8_literal("https://www.twitch.tv/redcs");
    embeds[0].video = {};
    for (u32 poster = 0; poster < 3; ++poster) {
        embeds[0].image = poster == 1 ? saved_thumbnail : GatewayMessageEmbedMedia{};
        embeds[0].thumbnail = poster == 0 ? saved_thumbnail : GatewayMessageEmbedMedia{};
        message_rich_store_initialize(&store, {records, 1}, {order, 1}, {text, sizeof(text)});
        message_rich_store_apply(&store, 5, 1, event);
        rich = message_rich_store_find(store, 5);
        float width{}, height{};
        check(rich && !message_rich_embed_plan(store, rich->embeds[0]).corner_thumbnail &&
              message_rich_embed_media_select(store, rich->embeds[0], &media) &&
              message_rich_embed_media_extent(media, 300, &width, &height) && width > 0 && height > 0 &&
              (poster != 2 || !media.source.bytes),
              "Twitch profile cards expose a play surface with thumbnail, image or no artwork");
        check(rich && message_rich_embed_playback_select(store, rich->embeds[0], &media) && media.provider &&
              utf8_equal(media.source, embeds[0].url),
              "Twitch channel URL plays the live stream without an iframe video field");
        bool changed = false;
        check(message_rich_store_set_embed_dimensions(&store, 5, 1, 0, MessageRichEmbedMediaSlot::video,
                                                      1920, 1080, &changed) && changed &&
              message_rich_embed_media_select(store, rich->embeds[0], &media) &&
              message_rich_embed_media_extent(media, 480, &width, &height) && width == 480 && height == 270 &&
              (poster == 2 || utf8_equal(media.source, saved_thumbnail.url)),
              "decoded Twitch video fills the card width at its own aspect ratio and preserves poster identity");
        check(message_rich_embed_media_extent(media, 240, &width, &height) && width == 240 && height == 135 &&
              message_rich_store_set_embed_dimensions(&store, 5, 1, 0, MessageRichEmbedMediaSlot::video,
                                                      1920, 1080, &changed) && !changed,
              "Twitch video resizes without stretching or invalidating unchanged frame dimensions");
    }
    embeds[0].image = {};
    embeds[0].thumbnail = saved_thumbnail;
    embeds[0].url = {};
    embeds[0].video = {};
    embeds[0].type = GatewayMessageEmbedType::rich;
    const Utf8View preview_urls[]{utf8_literal("https://open.spotify.com/track/11dFghVXANMlKmJXsNCbNl"),
        utf8_literal("https://music.apple.com/us/album/imagine/255144028?i=255145362"),
        utf8_literal("https://music.apple.com/us/music-video/name/1445828315"),
        utf8_literal("https://open.spotify.com/episode/7makk4oTQel546B0PZlDM5")};
    const Utf8View preview_labels[]{utf8_literal("Spotify · Preview"), utf8_literal("Apple Music · Preview"), utf8_literal("Apple Music · Preview"), utf8_literal("Spotify · Preview")};
    for (u32 preview = 0; preview < 4; ++preview) {
        embeds[0].url = preview_urls[preview];
        message_rich_store_initialize(&store, {records, 1}, {order, 1}, {text, sizeof(text)});
        message_rich_store_apply(&store, 5, 1, event);
        rich = message_rich_store_find(store, 5);
        check(rich && message_rich_embed_media_select(store, rich->embeds[0], &media) &&
              media.resource == MediaEmbedResource::thumbnail && !message_rich_embed_plan(store, rich->embeds[0]).corner_thumbnail &&
              utf8_equal(message_rich_embed_provider_label(store, rich->embeds[0]), preview_labels[preview]),
              "music track keeps large album art and labels preview playback before activation");
        check(rich && message_rich_embed_playback_select(store, rich->embeds[0], &media) &&
              media.provider && media.format == (preview == 2 ? MediaAttachmentFormat::mp4 : preview == 1 ? MediaAttachmentFormat::m4a : MediaAttachmentFormat::mp3) &&
              media.animated == (preview == 2) && utf8_equal(media.source, embeds[0].url),
              "music previews route to typed native audio or video without fetching the source during layout");
        auto& f = fixture;
        MessageTextCommand label{};
        label.message_id = 5; label.message_revision = 1; label.source_id = 0x8008;
        check(message_store_initialize(&f.store, {f.record, 2}, {f.order, 2}, {f.text, sizeof(f.text)}) &&
              message_store_upsert(&f.store, {5, 7, 9, 0, 0, utf8_literal(""), 0}).ok() &&
              utf8_equal(message_text_presentation_command_source(f.store, &store, label),
                         preview_labels[preview]),
              "preview label selection maps to displayed text instead of raw provider metadata");
    }
}
// As in the official client, embeds sharing a url fold into the first, whose
// media becomes a gallery of their images (a video wins over the images).
void test_embed_gallery(Check check) noexcept {
    static MessageRichRecord records[1];
    static u16 order[1];
    static u8 text[4096];
    MessageRichStore store{};
    GatewayMessageEmbed embeds[5]{};
    const char* images[]{"https://e.test/1.jpg", "https://e.test/2.jpg", "https://e.test/3.jpg",
                         "https://e.test/4.jpg", "https://e.test/5.jpg"};
    const auto reset = [&](u32 count) noexcept {
        for (u32 i = 0; i < 5; ++i) {
            embeds[i] = {};
            embeds[i].type = GatewayMessageEmbedType::rich;
            embeds[i].url = utf8_literal("https://x.test/status/2");
            if (i < count) {
                embeds[i].image.url = {images[i], 20};
                embeds[i].image.width = 1200;
                embeds[i].image.height = 900;
            }
        }
        embeds[0].description = utf8_literal("post");
    };
    const auto apply = [&](u32 count) noexcept -> const MessageRichRecord* {
        GatewayMessageEvent event{};
        event.id = 5;
        event.fields = gateway_message_field_embeds;
        event.embeds = {embeds, count};
        return message_rich_store_initialize(&store, {records, 1}, {order, 1}, {text, sizeof(text)}) &&
                       message_rich_store_apply(&store, 5, 1, event).ok()
                   ? message_rich_store_find(store, 5)
                   : nullptr;
    };
    const UiRect block{0, 0, 400, 300};
    const auto same_rect = [](UiRect a, UiRect b) noexcept {
        return close_to(a.left, b.left) && close_to(a.top, b.top) && close_to(a.right, b.right) &&
               close_to(a.bottom, b.bottom);
    };
    bool merges = true;
    for (u32 count = 1; count <= 4; ++count) {
        reset(count);
        const MessageRichRecord* rich = apply(count);
        MessageRichEmbedGallery gallery{};
        const bool shown = rich && message_rich_embed_gallery(store, *rich, 0, &gallery);
        merges = merges && rich && shown == (count > 1) && gallery.count == (count > 1 ? count : 0u);
        for (u32 i = 0; rich && i < count; ++i)
            merges = merges && message_rich_embed_merge_host(store, *rich, i) == 0 &&
                     (count == 1 || gallery.members[i] == i);
        const MessageRichItemLayout layout =
            rich ? message_rich_estimate_item_layout(&store, rich, 900) : MessageRichItemLayout{};
        merges = merges && layout.embed_media_heights[0] == 300 && layout.embed_media_widths[0] == 400;
        for (u32 i = 1; i < count; ++i)
            merges = merges && layout.heights[kMessageRichEmbedItemOffset + i] == 0;
    }
    // Two: side by side; three: one left, two stacked right; four: 2x2.
    merges = merges && same_rect(message_rich_embed_gallery_tile(block, 2, 0), {0, 0, 198, 300}) &&
             same_rect(message_rich_embed_gallery_tile(block, 2, 1), {202, 0, 400, 300}) &&
             same_rect(message_rich_embed_gallery_tile(block, 3, 0), {0, 0, 198, 300}) &&
             same_rect(message_rich_embed_gallery_tile(block, 3, 1), {202, 0, 400, 148}) &&
             same_rect(message_rich_embed_gallery_tile(block, 3, 2), {202, 152, 400, 300}) &&
             same_rect(message_rich_embed_gallery_tile(block, 4, 2), {0, 152, 198, 300}) &&
             same_rect(message_rich_embed_gallery_tile(block, 4, 3), {202, 152, 400, 300}) &&
             !ui_rect_has_area(message_rich_embed_gallery_tile(block, 5, 4));
    // One merged image under an imageless card fills the first column.
    reset(2);
    embeds[0].image = {};
    const MessageRichRecord* rich = apply(2);
    MessageRichEmbedGallery gallery{};
    MessageRichEmbedGalleryPlace place{};
    merges = merges && rich && message_rich_embed_gallery(store, *rich, 0, &gallery) && gallery.count == 1 &&
             gallery.members[0] == 1 && message_rich_embed_gallery_place(store, *rich, 1, &place) &&
             place.host == 0 && place.ordinal == 0 &&
             same_rect(message_rich_embed_gallery_tile(block, 1, 0), {0, 0, 198, 300}) &&
             message_rich_embed_gallery_width(rich->embeds[0], 1000) == 488;
    // A portrait first image narrower than 300 keeps the 520-wide card.
    reset(2);
    embeds[0].image.width = 900;
    embeds[0].image.height = 1200;
    rich = apply(2);
    merges = merges && rich && message_rich_embed_gallery_width(rich->embeds[0], 1000) == 488 &&
             message_rich_embed_gallery_width(rich->embeds[0], 300) == 300;
    check(merges, "embed_gallery_merges_one_to_four_images");

    // Different urls never merge; an image-less same-url embed is dropped.
    reset(3);
    embeds[1].url = utf8_literal("https://x.test/status/3");
    embeds[2].image = {};
    embeds[2].title = utf8_literal("dropped");
    rich = apply(3);
    const bool separate = rich && message_rich_embed_merge_host(store, *rich, 1) == 1 &&
                          message_rich_embed_merge_host(store, *rich, 2) == 0 &&
                          !message_rich_embed_gallery(store, *rich, 0, &gallery) &&
                          !message_rich_embed_gallery(store, *rich, 1, &gallery);
    check(separate, "embed_gallery_keeps_other_urls_separate");

    // A video (or gifv) card plays its video: the merged images are dropped.
    reset(3);
    embeds[0].video.url = utf8_literal("https://e.test/v.mp4");
    embeds[0].video.width = 1280;
    embeds[0].video.height = 720;
    rich = apply(3);
    MessageRichEmbedMediaSelection media{};
    const MessageRichItemLayout video_layout =
        rich ? message_rich_estimate_item_layout(&store, rich, 900) : MessageRichItemLayout{};
    bool video = rich && !message_rich_embed_gallery(store, *rich, 0, &gallery) &&
                 message_rich_embed_merge_host(store, *rich, 2) == 0 &&
                 !message_rich_embed_gallery_place(store, *rich, 1, &place) &&
                 message_rich_embed_media_select(store, rich->embeds[0], &media) &&
                 media.resource == MediaEmbedResource::video &&
                 video_layout.heights[kMessageRichEmbedItemOffset + 1] == 0 &&
                 video_layout.embed_media_heights[0] == 225;
    // A merged video embed adds nothing; the images still form the gallery.
    reset(2);
    embeds[2].video.url = utf8_literal("https://e.test/w.mp4");
    rich = apply(3);
    video = video && rich && message_rich_embed_gallery(store, *rich, 0, &gallery) && gallery.count == 2 &&
            message_rich_embed_merge_host(store, *rich, 2) == 0;
    check(video, "embed_gallery_video_wins_over_merged_images");
}

// Markup headings and subtext take the official client's rem sizes as explicit
// span sizes (24/20/16/14 DIP); a base size covers the plain text too. Each sized
// run shapes at its own em while unsized runs keep the system em.
void test_text_sizes(Check check) noexcept {
    static char output[1024];
    static u32 styles[1024];
    static TextStyleSpan spans[128];
    static MessageMarkdownSemantic semantics[32];
    MessageMarkdownOptions sized{};
    sized.text_sizes = true;
    auto size = [&](const MessageMarkdownResult& result, u32 byte) noexcept -> u8 {
        for (u32 i = 0; i < result.span_count; ++i)
            if (byte >= spans[i].byte_offset && byte < spans[i].byte_offset + spans[i].byte_count)
                return spans[i].size;
        return 0xFF;
    };
    auto result = parse(utf8_literal("# a\n## b\n### c\n-# d\ne **f**"), output, styles, spans, semantics, sized);
    // Display: "a\nb\nc\nd\ne f"
    check(result.ok() && size(result, 0) == text_size_xl && size(result, 2) == text_size_lg &&
              size(result, 4) == text_size_md && size(result, 6) == text_size_sm && size(result, 8) == 0xFF &&
              size(result, 10) == text_size_inherit,
          "markdown_headings_and_subtext_take_reference_rem_sizes");
    sized.base_size = text_size_sm;
    result = parse(utf8_literal("plain **bold** # not a heading"), output, styles, spans, semantics, sized);
    u32 covered = 0;
    bool all = result.ok();
    for (u32 i = 0; all && i < result.span_count; ++i) {
        all = spans[i].byte_offset == covered && spans[i].size == text_size_sm;
        covered += spans[i].byte_count;
    }
    check(all && covered == result.text.bytes && result.span_count == 3 && spans[0].flags == 0 &&
              spans[1].flags == text_style_bold,
          "markdown_base_size_covers_plain_text_with_size_only_spans");
    result = parse(utf8_literal("# a"), output, styles, spans, semantics);
    check(result.ok() && size(result, 0) == text_size_inherit, "markdown_without_text_sizes_keeps_flag_sizes");

    const auto em = [&](Utf8View text, TextStyleSpan span, float* width = nullptr) noexcept {
        ShapedText out{};
        float value = -1;
        if (shaped_text_initialize(&out, {glyphs, 1024}, {runs, 256}, {clusters, 1024}, {decorations, 64}) &&
            text_shape_utf8(&text_system, text, {&span, 1}, 1000, 1000, {utf16, 1024}, &out).ok() &&
            out.run_count != 0) {
            value = out.runs[0].em_size;
            if (width)
                *width = out.width;
        }
        shaped_text_reset(&out);
        return value;
    };
    const Utf8View hello = utf8_literal("Hello");
    float body = 0, xl = 0, xs = 0;
    check(em(hello, {0, 5, text_style_bold}, &body) == 16 && em(hello, {0, 5, text_style_bold, text_size_xl}, &xl) == 24 &&
              em(hello, {0, 5, 0, text_size_xs}, &xs) == 12 &&
              em(hello, {0, 5, text_style_heading_3}) == 16 * 1.1f &&
              em(hello, {0, 5, text_style_monospace, text_size_sm}) == 14 && xl > body * 1.3f &&
              xs < body * .85f,
          "sized_spans_shape_at_their_own_em");
    // Sized spans fold into the native engine's flags and keep their em.
    TextSystem native = text_system;
    bool native_sizes = native.native_text && native.native_format;
    const TextStyleSpan native_spans[]{{0, 5, text_style_bold, text_size_xl}, {0, 5, 0, text_size_sm},
                                       {0, 5, text_style_heading_3, text_size_md}, {0, 5, text_style_bold, text_size_xs}};
    const float native_ems[]{24, 14, 16, 12};
    for (u32 i = 0; native_sizes && i < 4; ++i) {
        ShapedText out{};
        native_sizes = shaped_text_initialize(&out, {glyphs, 1024}, {runs, 256}, {clusters, 1024}) &&
                       text_shape_utf8(&native, hello, {&native_spans[i], 1}, 1000, 1000, {utf16, 1024}, &out)
                           .ok() &&
                       out.run_count && out.runs[0].native_font && out.runs[0].em_size == native_ems[i] &&
                       text_measure_utf8(&native, hello, {&native_spans[i], 1}, 1000, 1000, {utf16, 1024})
                           .ok();
        shaped_text_reset(&out);
    }
    check(native_sizes, "sized_spans_shape_in_the_native_engine");
    // More sized spans than the front end folds inline: they fold into heap
    // storage and shape and measure exactly like one span over the same text.
    TextStyleSpan many[40]{};
    for (u32 i = 0; i < 40; ++i)
        many[i] = {i, 1, 0, text_size_xl};
    const Utf8View row = utf8_literal("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    const TextStyleSpan whole{0, 40, 0, text_size_xl};
    ShapedText long_out{};
    bool long_sizes = native.native_text && native.native_format &&
                      shaped_text_initialize(&long_out, {glyphs, 1024}, {runs, 256}, {clusters, 1024}) &&
                      text_shape_utf8(&native, row, {many, 40}, 1000, 1000, {utf16, 1024}, &long_out).ok() &&
                      long_out.glyph_count == 40;
    for (u32 i = 0; long_sizes && i < long_out.run_count; ++i)
        long_sizes = long_out.runs[i].em_size == 24;
    shaped_text_reset(&long_out);
    const auto many_measure = text_measure_utf8(&native, row, {many, 40}, 1000, 1000, {utf16, 1024});
    const auto whole_measure = text_measure_utf8(&native, row, {&whole, 1}, 1000, 1000, {utf16, 1024});
    check(long_sizes && many_measure.ok() && whole_measure.ok() && many_measure.width == whole_measure.width &&
              many_measure.height == whole_measure.height,
          "long_sized_span_lists_fold_beyond_the_inline_buffer");
    TextStyleSpan invalid{0, 5, 0, text_size_count};
    check(em(hello, invalid) < 0 && em(hello, {0, 5, 0, 0}) < 0, "size_only_spans_need_a_valid_size");
    const auto measured = [&](TextStyleSpan span) noexcept {
        return text_measure_utf8(&text_system, hello, {&span, 1}, 1000, 1000, {utf16, 1024}).height;
    };
    check(measured({0, 5, 0, text_size_xl}) > measured({0, 5, text_style_bold}) &&
              measured({0, 5, 0, text_size_xs}) < measured({0, 5, text_style_bold}),
          "sized_spans_measure_their_line_height");
}

// Quotes are blocks as in the official client: the body splits into plain and quoted
// runs, the quote text starts 16 DIP in (4-DIP spine + 12-DIP padding) and
// wraps at min(width - 4, 90%) - 20, every wrapped line keeps the indent, one
// spine spans the block and 4-DIP margins separate the blocks. Links inside
// keep their source, headings and subtext keep their sizes.
void test_quote_blocks(Check check) noexcept {
    auto& f = fixture;
    const auto payload = utf8_literal(R"json({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"# Title\nintro\n> quoted **bold** text long enough to wrap across several lines of this narrow pane, with https://q.test/x inside\n> second\n-# after","timestamp":"2026-09-05T10:00:00Z"}})json");
    GatewayMessageEvent message{};
    bool ok = decode_message(payload, &message);
    ok = ok && initialize_fixture(message);
    ChatStateStore state{};
    NavigationView navigation{};
    const MessageTextCoordinatorRequest request{&f.scene, &f.presentation, &f.cache, &state, nullptr, &navigation,
                                                &kDefaultTheme, finish, nullptr, 1'788'552'480, 1};
    ok = ok && message_scene_update(&f.scene, {0, 0, 520, 1000}).ok() && message_text_coordinator_converge(request).ok();
    const auto& p = f.presentation;
    const MessageTextCommand* body[4]{};
    u32 count = 0;
    for (u32 i = 0; ok && i < p.command_count; ++i)
        if (p.commands[i].measurement_kind == MessageTextMeasurementKind::content && count < 4)
            body[count++] = &p.commands[i];
    check(ok && count == 3 && utf8_equal(body[0]->text, utf8_literal("Title\nintro")) &&
              utf8_starts_with(body[1]->text, utf8_literal("quoted bold text")) &&
              ends_with(body[1]->text, utf8_literal("inside\nsecond")) && utf8_equal(body[2]->text, utf8_literal("after")),
          "quote_splits_the_body_into_blocks");
    if (count != 3) {
        message_scene_release(&f.scene);
        return;
    }
    const float width = ui_rect_width(body[0]->rectangle);
    const float quote_width = (width - 4 < width * .9f ? width - 4 : width * .9f) - 20;
    check(body[1]->rectangle.left == body[0]->rectangle.left + 16 && body[2]->rectangle.left == body[0]->rectangle.left &&
              ui_rect_width(body[1]->rectangle) > quote_width - .01f &&
              ui_rect_width(body[1]->rectangle) < quote_width + .01f &&
              close_to(body[1]->rectangle.top, body[0]->rectangle.top + body[0]->measured_height + 4) &&
              close_to(body[2]->rectangle.top, body[1]->rectangle.top + body[1]->measured_height + 4) &&
              close_to(p.measurements[0].content_height,
                   body[0]->measured_height + body[1]->measured_height + body[2]->measured_height + 8),
          "quote_block_is_indented_narrowed_and_stacked_with_margins");
    check(has_style(body[1], utf8_literal("bold"), text_style_bold | text_style_quote) &&
              has_style(body[1], utf8_literal("second"), text_style_quote) &&
              !has_style(body[0], utf8_literal("intro"), text_style_quote) &&
              utf8_equal(link_target(f, body[1]), utf8_literal("https://q.test/x")) &&
              size_at(body[0], utf8_literal("Title")) == text_size_xl &&
              size_at(body[2], utf8_literal("after")) == text_size_sm &&
              has_style(body[2], utf8_literal("after"), text_style_subtext),
          "quote_keeps_nested_formatting_links_and_heading_sizes");
    // Wrapped quote lines all start at the indented origin; one 4-DIP spine
    // spans every line, 16 DIP left of the text.
    ShapedText out{};
    bool shaped = shaped_text_initialize(&out, {glyphs, 1024}, {runs, 256}, {clusters, 1024}, {decorations, 64}) &&
                  text_shape_utf8(&text_system, body[1]->text, body[1]->styles, ui_rect_width(body[1]->rectangle),
                                  4096, {utf16, 1024}, &out)
                      .ok();
    u32 spines = 0;
    TextDecoration spine{};
    for (u32 i = 0; shaped && i < out.decoration_count; ++i)
        if (out.decorations[i].kind == TextDecorationKind::quote_bar) {
            spine = out.decorations[i];
            ++spines;
        }
    float left_most = 1e9f;
    u32 line_starts = 0;
    float previous_y = -1;
    for (u32 i = 0; shaped && i < out.run_count; ++i) {
        if (out.runs[i].baseline_y != previous_y) {
            left_most = out.runs[i].baseline_x < left_most ? out.runs[i].baseline_x : left_most;
            previous_y = out.runs[i].baseline_y;
            ++line_starts;
        }
    }
    check(shaped && out.line_count >= 3 && line_starts >= 3 && left_most >= 0 && spines == 1 && spine.left == -16 &&
              spine.right == -12 && spine.top == 0 && spine.bottom > out.height - .5f &&
              spine.bottom < out.height + .5f,
          "quote_wrapped_lines_keep_the_indent_under_one_spine");
    shaped_text_reset(&out);
    message_scene_release(&f.scene);
}

} // namespace embed_parity_detail

// The client shapes with the native engine alone (main.cpp). A text item it
// rejected used to fail the frame's whole content text: an fxtwitter embed
// author name with a Bamum Supplement scalar (U+169E7) or a description with
// U+2063 blanked the message list behind the shell fallback, and the full
// rebuild re-ran every 1.6 s, locking the UI up. These run on that engine.

// Every scalar shapes and measures, plain and styled, in a line of text.
void test_native_text_never_fails(Check check) noexcept {
    bool shaped = true, measured = true;
    // Past ASCII a step of 13 stays below the 16-scalar minimum Unicode block,
    // so every block (script, properties, fallback face) is still visited.
    for (u32 cp = 1; cp < 0xE01F0; cp += cp < 0x80 ? 1u : 13u) {
        if (cp >= 0xD800 && cp < 0xF900)
            cp = 0xF900; // surrogates and the private-use area
        if (cp >= 0x20000 && cp < 0x2FFF0)
            cp = 0x2FFF0;
        if (cp >= 0x30000 && cp < 0xE0000)
            cp = 0xE0000;
        char text[16];
        u32 n = 0;
        text[n++] = 'a';
        text[n++] = 'b';
        text[n++] = ' ';
        n += encode(cp, text + n);
        text[n++] = ' ';
        text[n++] = 'c';
        const TextStyleSpan bold{0, n, text_style_bold, text_size_sm};
        shaped = shaped && shape_styled({text, n}, {}).ok;
        measured = measured && text_measure_utf8(&text_system, {text, n}, {&bold, 1}, 1000, 1000,
                                                 {utf16, 1024}).ok();
    }
    check(shaped, "native_every_scalar_shapes_without_failing_the_item");
    check(measured, "native_every_scalar_measures_without_failing_the_item");

    // Names shaped like the ones that failed, other display-name decorations, and the
    // fxtwitter statistics line; measurement and drawing agree.
    const Utf8View names[]{
        utf8_literal("rowan \xF0\x96\xA7\xA7 (@rowan_example)"),
        utf8_literal("\xE1\xA1\xA3\xF0\x90\xAD\xA9 \xEA\x92\xB0\xE1\x90\xA2. .\xE1\x90\xA2\xEA\x92\xB1 \xE2\x8B\x86\xCB\x9A\xE2\x9C\xBF (@sample)"),
        utf8_literal("\xEA\x95\xA4 \xE2\xB5\xA3 \xF0\x90\x99\x9A \xEA\xA6\xBF \xE1\xA5\xAB\xE1\xAD\xA1 \xF0\x9D\x93\xAA\xF0\x9D\x93\xB7 (@x)"),
        utf8_literal("\xF0\x9F\x92\xAC 412   \xF0\x9F\x94\x81 377   \xE2\x9D\xA4\xEF\xB8\x8F 21.4K   \xF0\x9F\x91\x81\xEF\xB8\x8F 1.08M"),
        utf8_literal("ctl\x1B[0m bell\x07 \xC2\x90 soft\xC2\xADhyphen \xE2\x80\x8Frtl\xE2\x80\x8E \xEF\xBB\xBF \xF0\x9D\x85\xB3"),
    };
    bool agree = true;
    for (const Utf8View name : names) {
        const TextStyleSpan span{0, name.bytes, text_style_bold, text_size_sm};
        const auto m = text_measure_utf8(&text_system, name, {&span, 1}, 400, 4096, {utf16, 1024});
        const Shaped s = shape_styled(name, {&span, 1}, 400);
        agree = agree && m.ok() && s.ok && close_to(m.width, s.width) && m.width > 20;
    }
    check(agree, "native_decorated_names_measure_and_draw_alike");

    // A scalar no native item can express draws the missing glyph, not a
    // blank item; the placeholders are unassigned in the Unicode tables.
    const float ab = shape_styled(utf8_literal("ab"), {}).width;
    const Shaped javanese = shape_styled(utf8_literal("a\xEA\xA6\x84" "b"), {});
    check(javanese.ok && javanese.notdef == 1 && javanese.width > ab, "native_unsupported_scalar_draws_one_missing_glyph");
    // Simple scripts draw real glyphs from a covering system face (else one
    // missing glyph each), alone and between Latin letters, measured alike.
    bool simple = true;
    for (u32 i = 0; i < sizeof(kSimpleScripts) / sizeof(kSimpleScripts[0]); ++i) {
        char text[16];
        u32 n = 0;
        text[n++] = 'a';
        n += encode(kSimpleScripts[i], text + n);
        n += encode(kSimpleScripts[i], text + n);
        text[n++] = 'b';
        const Shaped s = shape_styled({text, n}, {});
        const auto m = text_measure_utf8(&text_system, {text, n}, {}, 1000, 1000, {utf16, 1024});
        simple = simple && s.ok && m.ok() && s.notdef == (simple_script_covered[i] ? 0u : 2u) && s.width > ab &&
                 close_to(m.width, s.width);
    }
    check(simple, "native_simple_scripts_draw_covering_system_glyphs");
    // Right-to-left text with an unmatched bracket keeps its letters; only the
    // bracket becomes a mirrored lookalike.
    const Shaped hebrew = shape_styled(utf8_literal("abc \xD7\x90\xD7\x91 ("), {});
    check(hebrew.ok && hebrew.notdef == 0 && hebrew.width > shape_styled(utf8_literal("abc"), {}).width,
          "native_unmatched_bracket_in_rtl_text_keeps_its_letters");
    check(unicode_properties(0x378)->general_category() == UnicodeGeneralCategory::cn &&
              unicode_properties(0x2B74)->general_category() == UnicodeGeneralCategory::cn &&
              unicode_properties(0x2FFFD)->general_category() == UnicodeGeneralCategory::cn,
          "native_missing_glyph_placeholders_are_unassigned");

    // Invisible format controls take no width natively, mid-line and at a
    // line start (fxtwitter prefixes lines with U+2063).
    const u32 hidden[]{0x0090, 0x00AD, 0x061C, 0x200E, 0x200F, 0x2060, 0x2063, 0x206A, 0xFEFF,
                       0xFFF9, 0x1BCA0, 0x1D173, 0xE0002, 0xE0100};
    const float plain = shape_styled(utf8_literal("abcd"), {}).width;
    const float line = shape_styled(utf8_literal("Hello"), {}).width;
    bool invisible = plain > 0 && line > 0;
    for (u32 cp : hidden) {
        for (u32 form = 0; form < 2; ++form) {
            char text[32]{};
            u32 n = 0;
            if (!form) {
                text[n++] = 'a';
                text[n++] = 'b';
            }
            n += encode(cp, text + n);
            n += encode(cp, text + n);
            const char* tail = form ? "Hello" : "cd";
            for (u32 i = 0; tail[i]; ++i)
                text[n++] = tail[i];
            const Shaped s = shape_styled({text, n}, {});
            const float expected = form ? line : plain;
            invisible = invisible && s.ok && s.notdef == 0 && close_to(s.width, expected);
        }
    }
    check(invisible, "native_invisible_format_controls_take_no_width");

    // A failing cluster is narrowed to itself, whatever the item's length: a
    // Cyrillic fxtwitter quote (lines led by U+FE00 U+FE00, emoji, CJK, escaped
    // markdown) stays real glyphs, plain and with an unshapable scalar
    // (Javanese) appended; only that cluster becomes the missing glyph.
    static const char tweet[] =
        "\xD0\x91\xD0\xB8\xD0\xB1\xD0\xBB\xD0\xB8\xD0\xBE\xD1\x82\xD0\xB5\xD0\xBA\xD0\xB0 \xD0\xA1\xD0\xB5\xD0\xB2\xD0\xB5\xD1\x80\xD0\xBD\xD0\xB0\xD1\x8F \xD0\x97\xD0\xB2\xD0\xB5\xD0\xB7\xD0\xB4\xD0\xB0 \xD0\xB4\xD0\xBB\xD1\x8F GitHub\n"
        "\xEF\xB8\x80\xEF\xB8\x80\xD1\x83\xD1\x81\xD0\xBF\xD0\xB5\xD1\x88\xD0\xBD\xD0\xBE \xD1\x81\xD0\xBE\xD0\xB1\xD1\x80\xD0\xB0\xD0\xBB\xD0\xB0 \xD1\x82\xD1\x8B\xD1\x81\xD1\x8F\xD1\x87\xD0\xB8\n"
        "\xEF\xB8\x80\xEF\xB8\x80\xD0\xBA\xD0\xBD\xD0\xB8\xD0\xB3 \xD1\x81\xD1\x82\xD0\xB0\xD1\x80\xD0\xB8\xD0\xBD\xD0\xBD\xD0\xBE\xD0\xB9 \xD0\xBF\xD0\xBE\xD1\x8D\xD0\xB7\xD0\xB8\xD0\xB8 \\[\xD0\x90\xD0\x90\xD0\x90\\] \n"
        "\xEF\xB8\x80\xEF\xB8\x80\xD1\x80\xD1\x83\xD0\xBA\xD0\xBE\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB5\xD0\xB9 \xD0\xB8 \xD1\x81\xD0\xBE\xD1\x85\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xBB\xD0\xB0 \xD1\x82\xD1\x8B\xD1\x81\xD1\x8F\xD1\x87\xD0\xB8 \n"
        "\xEF\xB8\x80\xEF\xB8\x80TLS cipher suites\n\xEF\xB8\x80\xEF\xB8\x80\xD0\xB2\xD0\xBC\xD0\xB5\xD1\x81\xD1\x82\xD0\xB5 \xD1\x81 make \xD0\xB8 cmake\n"
        "\xEF\xB8\x80\xEF\xB8\x80Quiet, quiet, \\[GOOD NEWS!\\]\n"
        "\xEF\xB8\x80\xEF\xB8\x80\xD0\xBF\xD1\x83\xD1\x81\xD1\x82\xD1\x8C \xD0\xB2\xD1\x81\xD1\x91 \xD0\xB8\xD0\xB4\xD1\x91\xD1\x82 \xD1\x81\xD0\xB2\xD0\xBE\xD0\xB8\xD0\xBC \xD0\xBF\xD1\x83\xD1\x82\xD1\x91\xD0\xBC\n"
        "\xEF\xB8\x80\xEF\xB8\x80\\-3^41 sample inputs \xF0\x9F\x9B\x8E\xEF\xB8\x8F\xF0\x9F\x94\xA8\xE2\x98\xA0\xEF\xB8\x8F\xE2\x98\xA3\xEF\xB8\x8F\n"
        "\xEF\xB8\x80\xEF\xB8\x80\xD0\xA2\xD1\x8B Vega, \xD0\xAF Comet...\n\n**[\xD0\xA6\xD0\xB8\xD1\x82\xD0\xB8\xD1\x80\xD1\x83\xD1\x8F](https://x.com/a/status/1)** "
        "\xE2\x81\xA3\xE2\x81\xA3\n> \\-\\-\\-\n> 2030: \xE6\x98\xA5\xE7\xA7\x8B \xD0\x9F\xD0\xA0\xD0\x90\xD0\x97\xD0\x94\xD0\x9D\xD0\x98\xD0\x9A TRULY BEGAN..\n"
        "**\xE2\x9D\xA4\xEF\xB8\x8F 27\xE2\x80\x82\xF0\x9F\x91\x81\xEF\xB8\x8F 358\xE2\x80\x82**";
    char forced[sizeof(tweet) + 8];
    copy_bytes(forced, tweet, sizeof(tweet) - 1);
    copy_bytes(forced + sizeof(tweet) - 1, " \xEA\xA6\x84", 4);
    // The quote's opening lines alone (no emoji or CJK): each line break is
    // followed by U+FE00 U+FE00, hidden as zero-width non-joiners that open the
    // Cyrillic run; that cut used to fail the item and box every letter.
    u32 lines = 0, opening = 0;
    while (opening < sizeof(tweet) - 1 && lines < 6)
        lines += tweet[opening++] == '\n' ? 1u : 0u;
    const Utf8View forms[]{{tweet, static_cast<u32>(sizeof(tweet) - 1)},
                           {tweet, opening},
                           {forced, static_cast<u32>(sizeof(tweet) + 3)}};
    bool cyrillic = true;
    for (u32 form = 0; form < 3; ++form) {
        const Utf8View text = forms[form];
        const TextStyleSpan span{0, text.bytes, 0, text_size_sm};
        const auto m = text_measure_utf8(&text_system, text, {&span, 1}, 520, 1'000'000, {utf16, 1024});
        const Shaped s = shape_styled(text, {&span, 1}, 520);
        cyrillic = cyrillic && m.ok() && s.ok && s.notdef <= (form == 2 ? 1u : 0u) && m.width > 100 &&
                   close_to(m.height, s.height);
    }
    check(cyrillic, "native_failing_cluster_keeps_cyrillic_glyphs_in_a_long_item");
    // Pathological text still measures: every scalar distinct (Yi syllables)
    // with invisible separators, as one 3 KB embed field.
    static char yi[2800];
    u32 bytes = 0;
    for (u32 cp = 0xA000; bytes + 6 < sizeof(yi) && cp < 0xA48D; ++cp) {
        bytes += encode(cp, yi + bytes);
        if (!(cp & 7))
            bytes += encode(0x2063, yi + bytes);
    }
    const TextStyleSpan span{0, bytes, 0, text_size_sm};
    const auto m = text_measure_utf8(&text_system, {yi, bytes}, {&span, 1}, 400, 1'000'000, {utf16, 1024});
    check(m.ok() && m.line_count > 1, "native_pathological_field_measures");
}

// Converges one MESSAGE_CREATE through the text pass with the native engine
// and paints it; returns the row or null.
const MessageSceneRowWidget* converge_native(Utf8View payload, GatewayMessageEvent* message,
                                            MessagePresentationResult* painted,
                                            MessageMediaCommandStream* media) noexcept {
    auto& f = fixture;
    bool ok = decode_message(payload, message);
    ok = ok && initialize_fixture(*message, 4000);
    ChatStateStore state{};
    NavigationView navigation{};
    const MessageTextCoordinatorRequest request{&f.scene, &f.presentation, &f.cache, &state, nullptr, &navigation,
                                                &kDefaultTheme, finish, nullptr, 1'791'600'000, 1};
    ok = ok && message_scene_update(&f.scene, {0, 0, 900, 4000}).ok() &&
         message_text_coordinator_converge(request).ok();
    const MessageSceneRowWidget* row = ok ? message_scene_find_row(f.scene, 0) : nullptr;
    paint_stream_reset(&f.paint, 900, 4000);
    // The renderer's temporary bank need not be zeroed before publication.
    for (auto& command : f.media)
        command.tile_radius = command.obscure = command.cover = command.gif = 255;
    if (!row || !message_media_command_stream_initialize(media, {f.media, 32}) ||
        !message_presentation_build(f.scene, kDefaultTheme, &f.paint, media, painted))
        row = nullptr;
    for (u32 i = 0; row && i < media->count; ++i)
        if (f.media[i].tile_radius == 255 || f.media[i].obscure == 255 ||
            f.media[i].cover == 255 || f.media[i].gif == 255)
            row = nullptr;
    paint_stream_release(&f.paint);
    return row;
}

// fxtwitter/vxtwitter embeds as the server delivers them (synthetic copies of
// observed shapes): the decorated author name that blanked the list, a
// description with U+2063 line prefixes, U+200B/U+3164/U+2800/U+FE0F, escaped
// markdown, a quote and a linked statistics line, a footer icon and timestamp,
// and a video; then a four-photo post as four embeds sharing one URL.
void test_vxtwitter_embeds(Check check) noexcept {
    auto& f = fixture;
    const auto video = utf8_literal(R"json({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"https://embed.example.com/i/status/1234567890123456789","timestamp":"2026-10-09T17:00:00Z","embeds":[{"type":"rich","url":"https://x.com/rowan_example/status/1234567890123456789","color":7193840,"description":"⁣Gardener tours the new seed library 🥺💔\n\n> \"Why does it smell like rain\\? It’s every spring\\.\\.\"\n\n⁣​ㅤ⠀ *not* a \\*joke\\* ❤️\n\n**[💬](https://x.com/intent/tweet?in_reply_to=1234567890123456789) 412 [🔁](https://x.com/intent/retweet?tweet_id=1234567890123456789) 377 [❤️](https://x.com/intent/like?tweet_id=1234567890123456789) 21.4K 👁️ 1.08M**","author":{"name":"rowan 𖧧 (@rowan_example)","url":"https://x.com/rowan_example","icon_url":"https://pbs.twimg.com/profile_images/1000000000000000001/avatar_200x200.jpg","proxy_icon_url":"https://images-ext-1.discordapp.net/external/a/https/pbs.twimg.com/profile_images/1000000000000000001/avatar_200x200.jpg"},"footer":{"text":"FxTwitter","icon_url":"https://assets.fxembed.com/logos/fxtwitter64.png","proxy_icon_url":"https://images-ext-1.discordapp.net/external/b/https/assets.fxembed.com/logos/fxtwitter64.png"},"timestamp":"2026-10-09T16:23:14+00:00","thumbnail":{"url":"https://pbs.twimg.com/amplify_video_thumb/1/img/a.jpg","proxy_url":"https://images-ext-1.discordapp.net/external/c/https/pbs.twimg.com/amplify_video_thumb/1/img/a.jpg","width":1920,"height":1080,"flags":0},"video":{"url":"https://video.twimg.com/amplify_video/1/vid/avc1/1920x1080/a.mp4","proxy_url":"https://images-ext-1.discordapp.net/external/d/https/video.twimg.com/amplify_video/1/vid/avc1/1920x1080/a.mp4","width":1920,"height":1080,"flags":0}}]}})json");
    GatewayMessageEvent message{};
    MessagePresentationResult painted{};
    MessageMediaCommandStream media{};
    const MessageSceneRowWidget* row = converge_native(video, &message, &painted, &media);
    check(row != nullptr, "vxtwitter_video_embed_converges_and_paints_natively");
    if (row) {
        const auto& p = f.presentation;
        const auto* author = command(p, 0x8002);
        const auto* description = command(p, 0x8004);
        check(author && utf8_equal(author->text, utf8_literal("rowan \xF0\x96\xA7\xA7 (@rowan_example)")) &&
                  author->measured_height > 0,
              "vxtwitter_decorated_author_is_measured");
        check(description && test_contains(description->text, utf8_literal("Gardener tours")) &&
                  description->measured_height > 0,
              "vxtwitter_description_text_is_shown");
        bool drawn = true;
        for (u32 i = 0; i < p.command_count; ++i) {
            const auto& c = p.commands[i];
            if (c.source_id < 0x8000 || !c.text.bytes)
                continue;
            const Shaped s = shape_styled(c.text, c.styles, ui_rect_width(c.rectangle));
            // The Bamum Supplement scalar draws one missing glyph unless a system face covers it.
            const bool decorated = &c == author && !simple_script_covered[1];
            drawn = drawn && s.ok && s.notdef == (decorated ? 1u : 0u);
        }
        check(drawn, "vxtwitter_embed_text_draws_without_tofu_for_invisibles");
    }
    message_scene_release(&f.scene);

    const auto photos = utf8_literal(R"json({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"https://fixvx.com/a/status/2","timestamp":"2026-10-09T17:00:00Z","embeds":[{"type":"rich","url":"https://x.com/a/status/2","description":"four ⁣photos⁣","author":{"name":"ꕤ a ⵣ (@a)","url":"https://x.com/a"},"image":{"url":"https://pbs.twimg.com/media/1.jpg","proxy_url":"https://images-ext-1.discordapp.net/external/1/https/pbs.twimg.com/media/1.jpg","width":1200,"height":900,"flags":0}},{"type":"rich","url":"https://x.com/a/status/2","image":{"url":"https://pbs.twimg.com/media/2.jpg","proxy_url":"https://images-ext-1.discordapp.net/external/2/https/pbs.twimg.com/media/2.jpg","width":1200,"height":900,"flags":0}},{"type":"rich","url":"https://x.com/a/status/2","image":{"url":"https://pbs.twimg.com/media/3.jpg","proxy_url":"https://images-ext-1.discordapp.net/external/3/https/pbs.twimg.com/media/3.jpg","width":900,"height":1200,"flags":0}},{"type":"rich","url":"https://x.com/a/status/2","image":{"url":"https://pbs.twimg.com/media/4.jpg","proxy_url":"https://images-ext-1.discordapp.net/external/4/https/pbs.twimg.com/media/4.jpg","width":1200,"height":1200,"flags":0}}]}})json");
    row = converge_native(photos, &message, &painted, &media);
    // One card: the merged embeds take no height and their images paint as
    // the four covering tiles of the card's 400x300 gallery.
    u32 images = 0;
    UiRect tiles[4]{};
    bool covered = true;
    for (u32 i = 0; row && i < media.count; ++i) {
        const auto& c = media.storage[i];
        if (c.kind != MessageMediaCommandKind::embed)
            continue;
        covered = covered && c.cover && c.embed_index < 4 &&
                  c.embed_resource == static_cast<u8>(MediaEmbedResource::image);
        if (c.embed_index < 4)
            tiles[c.embed_index] = c.rectangle;
        ++images;
    }
    const auto& items = row ? row->rich_item_layout : MessageRichItemLayout{};
    const float column = (400.0f - kMessageEmbedGalleryGap) * 0.5f;
    check(row && message.embeds.count == 4 && images == 4 && covered &&
              items.embed_media_widths[0] == 400 && items.embed_media_heights[0] == 300 &&
              items.heights[kMessageRichEmbedItemOffset + 1] == 0 &&
              items.heights[kMessageRichEmbedItemOffset + 3] == 0 &&
              close_to(ui_rect_width(tiles[0]), column) && close_to(ui_rect_height(tiles[0]), 148) &&
              close_to(tiles[1].left, tiles[0].right + 4) && close_to(tiles[2].top, tiles[0].bottom + 4) &&
              close_to(tiles[3].left, tiles[1].left) && close_to(tiles[3].top, tiles[2].top),
          "vxtwitter_four_photo_post_paints_one_gallery_card");
    if (row) {
        GatewayMessageEmbed members[4]{};
        for (u32 member = 0; member < 4; ++member) {
            members[member] = message.embeds[member];
            members[member].flags = member == 1 || member == 2 ? kGatewayMediaExplicitFlag : 0;
        }
        message.embeds = {members, 4};
        const auto publish_members = [&]() noexcept {
            const auto applied = message_rich_store_apply(&f.rich, 1, f.record[0].revision, message);
            return applied.ok() && !applied.storage_truncated;
        };
        ChatStateStore state{};
        NavigationView navigation{};
        const MessageTextCoordinatorRequest request{
            &f.scene,       &f.presentation, &f.cache, &state,        nullptr, &navigation,
            &kDefaultTheme, finish,          nullptr,  1'791'600'000, 1};
        const auto repaint_gallery = [&]() noexcept {
            message_scene_invalidate_geometry(&f.scene);
            return message_scene_update(&f.scene, {0, 0, 900, 4000}).ok() &&
                   message_text_coordinator_converge(request).ok() &&
                   paint_stream_reset(&f.paint, 900, 4000) &&
                   message_media_command_stream_initialize(&media, {f.media, 32}) &&
                   message_presentation_build(f.scene, kDefaultTheme, &f.paint, &media, &painted);
        };
        const auto media_members = [&](u32 clear, u32 blurred) noexcept {
            u32 seen = 0;
            for (u32 index = 0; index < media.count; ++index) {
                const auto& image = media.storage[index];
                if (image.kind != MessageMediaCommandKind::embed)
                    continue;
                if (image.embed_index >= 4)
                    return false;
                const u32 bit = 1u << image.embed_index;
                const auto expected =
                    blurred & bit ? MessageMediaObscure::explicit_content : MessageMediaObscure::none;
                if ((seen & bit) || !((clear | blurred) & bit) ||
                    image.obscure != static_cast<u8>(expected) || !image.cover ||
                    !ui_rect_equal(image.rectangle, tiles[image.embed_index]))
                    return false;
                seen |= bit;
            }
            return seen == (clear | blurred);
        };
        const auto labels = [&](MessageMediaObscure obscure, u32 expected) noexcept {
            u32 seen = 0, count = 0, wanted = 0;
            for (u32 member = 0; member < 4; ++member)
                wanted += (expected >> member) & 1u;
            for (u32 index = 0; index < f.presentation.command_count; ++index) {
                const auto& label = f.presentation.commands[index];
                if (!utf8_equal(label.text, message_media_obscure_label(obscure)))
                    continue;
                if (message_media_obscure_notice(obscure)
                        ? label.styles.count != 0
                        : label.styles.count != 1 || label.styles[0].flags != text_style_bold)
                    return false;
                ++count;
                for (u32 member = 0; member < 4; ++member) {
                    const auto cell = tiles[member];
                    const auto box = label.rectangle;
                    const bool fits = message_media_obscure_notice(obscure)
                                          ? box.left >= cell.left && box.right <= cell.right &&
                                                close_to(box.top, cell.top) &&
                                                close_to(box.bottom, cell.bottom) &&
                                                close_to(box.left + box.right, cell.left + cell.right)
                                          : ui_rect_equal(box, message_media_obscure_pill(cell, obscure));
                    if (fits)
                        seen |= 1u << member;
                }
            }
            return count == wanted && seen == expected;
        };
        const auto notices = [&]() noexcept {
            u32 seen = 0;
            for (u32 index = 0; index < f.paint.count; ++index) {
                const auto& notice = f.paint.quads[index];
                if (notice.radius != kDefaultTheme.scale.radius_sm || !notice.border_width)
                    continue;
                for (u32 member = 0; member < 4; ++member)
                    if (ui_rect_equal({notice.left, notice.top, notice.right, notice.bottom},
                                      tiles[member]))
                        seen |= 1u << member;
            }
            return seen;
        };
        static MessageMediaPolicyResult explicit_policy{MessageExplicitFilter::blur, true};
        constexpr auto explicit_query = +[](const void* context, const MessageMediaOwner&) noexcept {
            return *static_cast<const MessageMediaPolicyResult*>(context);
        };
        message_scene_set_media_policy(&f.scene, {&explicit_policy, explicit_query, 1});
        check(publish_members() && repaint_gallery() && media_members(0b1001, 0b0110) &&
                  labels(MessageMediaObscure::explicit_content, 0b0110),
              "gallery_filters_blur_only_flagged_members_and_place_each_label_in_its_cell");
        check(message_scene_reveal_media(&f.scene,
                                         message_media_reveal_key(1, MessageMediaSource::embed, 1)) &&
                  repaint_gallery() && media_members(0b1011, 0b0100) &&
                  labels(MessageMediaObscure::explicit_content, 0b0100),
              "gallery_reveal_key_opens_one_member_and_keeps_flagged_sibling_obscured");
        explicit_policy.filter = MessageExplicitFilter::block;
        message_scene_set_media_policy(&f.scene, {&explicit_policy, explicit_query, 2});
        const bool block_ready = repaint_gallery();
        check(block_ready, "gallery_block_converges_and_paints");
        if (block_ready) {
            check(media_members(0b1001, 0), "gallery_block_omits_only_flagged_media");
            check(notices() == 0b0110, "gallery_block_paints_notice_in_each_flagged_cell");
            check(labels(MessageMediaObscure::blocked, 0b0110),
                  "gallery_block_places_label_in_each_flagged_cell");
        }
        members[0].flags = kGatewayMediaExplicitFlag;
        members[1].flags = members[2].flags = 0;
        const bool host_ready = publish_members() && repaint_gallery();
        check(host_ready, "gallery_flagged_host_converges_and_paints");
        if (host_ready) {
            check(media_members(0b1110, 0), "gallery_flagged_host_does_not_block_clear_members");
            check(notices() == 0b0001, "gallery_flagged_host_paints_notice_only_in_host_cell");
            check(labels(MessageMediaObscure::blocked, 0b0001),
                  "gallery_flagged_host_places_label_only_in_host_cell");
        }
        paint_stream_release(&f.paint);
    }
    message_scene_release(&f.scene);
}

} // namespace maicord
