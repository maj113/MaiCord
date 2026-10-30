#include "maicord/protocol/media_catalog.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {
namespace {
u32 index[8192];
char text[32768];
MediaCatalogItem items[128];
u8 generated[32768];
bool same(Utf8View a, Utf8View b) noexcept {
    return a.bytes == b.bytes && bytes_equal(a.data, b.data, a.bytes);
}
MediaCatalogDecodeResult decode(MediaCatalogOperation operation, Utf8View body, u32 capacity = 50,
                                u32 bytes = sizeof(text)) noexcept {
    return media_catalog_decode(operation, body, {index, 8192}, 16, {items, capacity}, {text, bytes});
}
void sentinel() noexcept {
    for (auto& item : items)
        item = {};
    items[0].width = 777;
    for (u32 i = 0; i < sizeof(text); ++i)
        text[i] = '~';
}
bool untouched() noexcept {
    if (items[0].width != 777)
        return false;
    for (u32 i = 0; i < sizeof(text); ++i)
        if (text[i] != '~')
            return false;
    return true;
}
constexpr auto gif = utf8_literal(
    R"({"id":"fixture-provider-id","title":"Hello \uD83D\uDC4B","width":200,"height":100,"src":"https://static.klipy.com/ii/00000000000000000000000000000000/00/00/preview.webp","gif_src":"https://static.klipy.com/ii/00000000000000000000000000000000/00/00/animated.webp","url":"https://klipy.com/gifs/fixture","provider_metadata":{"unknown":[1,2]}})");
Utf8View array_fixture(u32 count, Utf8View entry, bool packs = false) noexcept {
    ProtocolWriter w{{generated, sizeof(generated)}, 0};
    if (packs)
        protocol_writer_append_literal(
            &w, "{\"sticker_packs\":[{\"id\":\"42\",\"name\":\"Fixture pack\",\"stickers\":[");
    else
        protocol_writer_append_literal(&w, "[");
    for (u32 i = 0; i < count; ++i) {
        if (i)
            protocol_writer_append_literal(&w, ",");
        protocol_writer_append_bytes(&w, {reinterpret_cast<const u8*>(entry.data), entry.bytes});
    }
    if (packs)
        protocol_writer_append_literal(&w, "]}]}");
    else
        protocol_writer_append_literal(&w, "]");
    return {reinterpret_cast<const char*>(generated), w.used};
}
} // namespace

void test_media_catalog(void (*check)(bool, const char*) noexcept) noexcept {
    MediaCatalogQuery query{};
    query.operation = MediaCatalogOperation::gif_search;
    query.query = utf8_literal("hello &+\xC3\xA9");
    query.locale = utf8_literal("en-US");
    query.limit = 20;
    u8 target[kMediaCatalogMaximumTargetBytes]{};
    auto written = media_catalog_write_target(query, {target, sizeof(target)});
    check(
        written.ok() &&
            same({reinterpret_cast<const char*>(target), written.bytes},
                 utf8_literal("/api/v10/gifs/"
                              "search?q=hello%20%26%2B%C3%A9&media_format=tinywebp&locale=en-US&limit=20")),
        "media catalog: GIF search query encodes UTF8 and delimiters, in the client's {q, media_format, "
        "locale, limit} order with the minimum provider limit");
    query.limit = 0;
    written = media_catalog_write_target(query, {target, sizeof(target)});
    check(written.ok() && same({reinterpret_cast<const char*>(target), written.bytes},
                               utf8_literal("/api/v10/gifs/"
                                            "search?q=hello%20%26%2B%C3%A9&media_format=tinywebp&locale=en-US")),
          "media catalog: zero limit omits the parameter like the official client");
    constexpr u32 rejected_limits[]{1u, 19u, kMediaCatalogMaximumPageItems + 1};
    for (u32 rejected : rejected_limits) {
        query.limit = rejected;
        target[0] = 0x55;
        check(
            !media_catalog_write_target(query, {target, sizeof(target)}).ok() && target[0] == 0x55,
            "media catalog: unsupported provider result counts fail atomically before request submission");
    }
    // The composer's inline /gif strip asks for animated WebP and 20 results.
    query.limit = kMediaCatalogMinimumGifPageItems;
    query.format = MediaCatalogFormat::webp;
    query.query = utf8_literal("dap");
    written = media_catalog_write_target(query, {target, sizeof(target)});
    check(written.ok() && same({reinterpret_cast<const char*>(target), written.bytes},
                               utf8_literal("/api/v10/gifs/search?q=dap&media_format=webp&locale=en-US&limit=20")),
          "media catalog: inline /gif search target is the client's integration request");
    target[0] = 0x55;
    query.limit = 50;
    query.format = MediaCatalogFormat::webm;
    written = media_catalog_write_target(query, {target, 1});
    check(written.error == MediaCatalogError::capacity && target[0] == 0x55,
          "media catalog: target capacity failure preserves destination");
    query.query = {reinterpret_cast<const char*>(target), 1};
    check(!media_catalog_write_target(query, {target, sizeof(target)}).ok(),
          "media catalog: overlapping target/query is refused");
    query.query = utf8_literal("cat");
    query.operation = MediaCatalogOperation::gif_suggestions;
    query.limit = 5;
    written = media_catalog_write_target(query, {target, sizeof(target)});
    check(written.ok() && same({reinterpret_cast<const char*>(target), written.bytes},
                               utf8_literal("/api/v10/gifs/suggest?q=cat&limit=5&locale=en-US")),
          "media catalog: suggestions use their separate endpoint without media_format, {q, limit, locale}");
    query.operation = MediaCatalogOperation::gif_categories;
    query.query = {};
    query.format = MediaCatalogFormat::tiny_webp;
    written = media_catalog_write_target(query, {target, sizeof(target)});
    check(written.ok() && same({reinterpret_cast<const char*>(target), written.bytes},
                               utf8_literal("/api/v10/gifs/trending?locale=en-US&media_format=tinywebp")),
          "media catalog: category landing uses the preview catalog route");
    query.operation = MediaCatalogOperation::gif_trending;
    query.limit = 50;
    written = media_catalog_write_target(query, {target, sizeof(target)});
    check(written.ok() &&
              same({reinterpret_cast<const char*>(target), written.bytes},
                   utf8_literal("/api/v10/gifs/trending-gifs?media_format=tinywebp&locale=en-US&limit=50")),
          "media catalog: Trending results use their separate provider route, {media_format, locale, limit}");
    query.operation = MediaCatalogOperation::sticker_packs;
    query.query = {};
    query.limit = 4096;
    written = media_catalog_write_target(query, {target, sizeof(target)});
    check(written.ok() && same({reinterpret_cast<const char*>(target), written.bytes},
                               utf8_literal("/api/v10/sticker-packs?locale=en-US")),
          "media catalog: complete sticker packs request has locale and no invented pagination parameter");
    query.operation = MediaCatalogOperation::guild_stickers;
    query.guild_id = 42;
    query.locale = {};
    written = media_catalog_write_target(query, {target, sizeof(target)});
    check(written.ok() && same({reinterpret_cast<const char*>(target), written.bytes},
                               utf8_literal("/api/v10/guilds/42/stickers")),
          "media catalog: guild sticker request uses authoritative guild route only");
    query.guild_id = 0;
    check(!media_catalog_write_target(query, {target, sizeof(target)}).ok(),
          "media catalog: absent guild identity never routes to a foreign catalog");
    // The emoji/sticker popout's lookups: one sticker, one pack, each by ID
    // without a locale.
    query.operation = MediaCatalogOperation::sticker;
    query.object_id = 749054660769218631;
    written = media_catalog_write_target(query, {target, sizeof(target)});
    check(written.ok() && same({reinterpret_cast<const char*>(target), written.bytes},
                               utf8_literal("/api/v10/stickers/749054660769218631")),
          "media catalog: one sticker by ID");
    query.operation = MediaCatalogOperation::sticker_pack;
    query.object_id = 847199849233514549;
    written = media_catalog_write_target(query, {target, sizeof(target)});
    check(written.ok() && same({reinterpret_cast<const char*>(target), written.bytes},
                               utf8_literal("/api/v10/sticker-packs/847199849233514549")),
          "media catalog: one sticker pack by ID");
    query.object_id = 0;
    const bool missing = !media_catalog_write_target(query, {target, sizeof(target)}).ok();
    query.operation = MediaCatalogOperation::sticker_packs;
    query.object_id = 7;
    query.locale = utf8_literal("en-US");
    check(missing && !media_catalog_write_target(query, {target, sizeof(target)}).ok(),
          "media catalog: an object ID belongs to exactly the sticker and pack lookups");
    query.object_id = 0;

    auto input = array_fixture(1, gif);
    auto result = decode(MediaCatalogOperation::gif_search, input);
    check(result.ok() && result.item_count == 1 && result.source_count == 1 && !result.truncated &&
              items[0].kind == MediaCatalogItemKind::gif && items[0].width == 200 &&
              items[0].height == 100 && same(items[0].provider_id, utf8_literal("fixture-provider-id")) &&
              same(items[0].name, utf8_literal("Hello \xF0\x9F\x91\x8B")) &&
              same(items[0].send_url, utf8_literal("https://klipy.com/gifs/fixture")) &&
              byte_ranges_overlap(
                  {reinterpret_cast<const u8*>(items[0].preview_url.data), items[0].preview_url.bytes},
                  {reinterpret_cast<const u8*>(text), sizeof(text)}) &&
              !byte_ranges_overlap(
                  {reinterpret_cast<const u8*>(items[0].send_url.data), items[0].send_url.bytes},
                  {reinterpret_cast<const u8*>(input.data), input.bytes}),
          "media catalog: real field family decodes into owned page text with opaque provider identity and "
          "separate send/preview URLs");
    for (u32 i = 0; i < input.bytes; ++i)
        generated[i] = '?';
    check(same(items[0].send_url, utf8_literal("https://klipy.com/gifs/fixture")),
          "media catalog: released response bank cannot change selected GIF send URL");
    const auto categories = utf8_literal(
        R"({"gifs":[],"categories":[{"name":"Happy \u263a","src":"//images-ext-1.discordapp.net/external/fixture/https/example.invalid/category.gif"}]})");
    result = decode(MediaCatalogOperation::gif_categories, categories);
    check(result.ok() && result.item_count == 1 && items[0].kind == MediaCatalogItemKind::category &&
              same(items[0].name, utf8_literal("Happy \xE2\x98\xBA")) && items[0].preview_url.bytes > 6 &&
              same({items[0].preview_url.data, 6}, utf8_literal("https:")),
          "media catalog: category query names and scheme-relative official preview URLs normalize into "
          "owned HTTPS views");
    ProtocolWriter landing{{generated, sizeof(generated)}, 0};
    protocol_writer_append_literal(
        &landing, "{\"categories\":[{\"name\":\"hello\",\"src\":\"https://images-ext-1.discordapp.net/"
                  "external/fixture/https/example.invalid/"
                  "category.gif?format=webp&animated=true&width=220&height=220\"}],\"gifs\":[");
    protocol_writer_append_bytes(&landing, {reinterpret_cast<const u8*>(gif.data), gif.bytes});
    protocol_writer_append_literal(&landing, ",");
    protocol_writer_append_bytes(&landing, {reinterpret_cast<const u8*>(gif.data), gif.bytes});
    protocol_writer_append_literal(&landing, "]}");
    const Utf8View landing_input{reinterpret_cast<const char*>(generated), landing.used};
    const auto landing_measure =
        media_catalog_measure(MediaCatalogOperation::gif_categories, landing_input, {index, 8192}, 16);
    result = decode(MediaCatalogOperation::gif_categories, landing_input);
    check(landing_measure.ok() && result.ok() && result.item_count == 2 && result.source_count == 2 &&
              result.text_bytes == landing_measure.text_bytes &&
              items[0].kind == MediaCatalogItemKind::gif &&
              same(items[0].provider_id, utf8_literal("fixture-provider-id")) &&
              items[1].kind == MediaCatalogItemKind::category && same(items[1].name, utf8_literal("hello")),
          "media catalog: one supplied Trending preview precedes categories independently of wire member "
          "order");
    result = decode(MediaCatalogOperation::gif_categories, landing_input, 1);
    check(result.ok() && result.item_count == 1 && result.source_count == 2 && result.truncated &&
              items[0].kind == MediaCatalogItemKind::gif,
          "media catalog: mixed landing catalog retains bounded local publication");
    const Utf8View invalid_landing[]{
        utf8_literal(R"({"categories":[],"gifs":{},"ignored":[]})"),
        utf8_literal(R"({"categories":[],"gifs":[],"gifs":[]})"),
        utf8_literal(R"({"categories":[],"categories":[],"gifs":[]})"), utf8_literal(R"({"gifs":[]})"),
        utf8_literal(R"({"categories":[],"gifs":[{"id":"missing-preview"}]})")};
    for (auto body : invalid_landing) {
        sentinel();
        result = decode(MediaCatalogOperation::gif_categories, body);
        check(!result.ok() && untouched(),
              "media catalog: malformed or duplicate landing members cannot partially publish Trending");
    }
    landing = {{generated, sizeof(generated)}, 0};
    protocol_writer_append_literal(&landing, "{\"categories\":[],\"gifs\":[");
    protocol_writer_append_bytes(&landing, {reinterpret_cast<const u8*>(gif.data), gif.bytes});
    protocol_writer_append_literal(&landing, ",{\"id\":\"invalid-second-preview\"}]}");
    sentinel();
    result = decode(MediaCatalogOperation::gif_categories,
                    {reinterpret_cast<const char*>(generated), landing.used});
    check(!result.ok() && untouched(),
          "media catalog: unused Trending previews are validated before publishing the first tile");
    result = decode(MediaCatalogOperation::gif_suggestions, utf8_literal(R"(["cats","cat\u00e9"])"), 5);
    check(result.ok() && result.item_count == 2 && items[1].kind == MediaCatalogItemKind::suggestion &&
              same(items[1].name, utf8_literal("cat\xC3\xA9")),
          "media catalog: actual string suggestions retain UTF8 words");
    const Utf8View invalid[]{
        utf8_literal(
            R"([{"id":"a","width":1,"height":1,"src":"https://static.klipy.com/x.webp","url":"https://klipy.com/gifs/x","width":2}])"),
        utf8_literal(
            R"([{"id":"a","width":0,"height":1,"src":"https://static.klipy.com/x.webp","url":"https://klipy.com/gifs/x"}])"),
        utf8_literal(
            R"([{"id":"a","width":1,"height":1,"src":"http://static.klipy.com/x.webp","url":"https://klipy.com/gifs/x"}])"),
        utf8_literal(
            R"([{"id":"a","width":1,"height":1,"src":"https://user@static.klipy.com/x.webp","url":"https://klipy.com/gifs/x"}])"),
        utf8_literal(
            R"([{"id":"a","width":1,"height":1,"src":"https://static.klipy.com/x.webp","url":"javascript:fixture"}])"),
        utf8_literal(
            R"([{"id":"a","width":1,"height":1,"src":"https://static.klipy.com/x.webp","url":"https://klipy.com/gifs/x\n"}])"),
        utf8_literal(R"([{"id":"a","width":1,"height":1,"src":"https://static.klipy.com/x.webp"}])")};
    for (auto body : invalid) {
        sentinel();
        result = decode(MediaCatalogOperation::gif_search, body);
        check(!result.ok() && untouched(), "media catalog: malformed, duplicate, unsafe URI and incomplete "
                                           "items leave prior owned page untouched");
    }
    sentinel();
    input = array_fixture(1, gif);
    result = decode(MediaCatalogOperation::gif_search, input, 50, 1);
    check(result.error == MediaCatalogError::text_capacity && untouched(),
          "media catalog: first item text capacity failure publishes no partial record");
    result =
        media_catalog_decode(MediaCatalogOperation::gif_search, input, {index, 8192}, 16,
                             {reinterpret_cast<MediaCatalogItem*>(generated), 1}, {text, sizeof(text)});
    check(result.error == MediaCatalogError::invalid_input,
          "media catalog: input/output alias is rejected before scan");
    constexpr auto sticker = utf8_literal(
        R"({"id":"7","name":"Wave","format_type":2,"tags":"hello,wave","description":null,"available":false,"user":{"private_extra":null}})");
    input = array_fixture(60, sticker, true);
    const auto measure =
        media_catalog_measure(MediaCatalogOperation::sticker_packs, input, {index, 8192}, 16);
    result = decode(MediaCatalogOperation::sticker_packs, input, 128);
    check(measure.ok() && measure.item_count == 60 && measure.source_count == 60 && !measure.truncated &&
              result.ok() && result.item_count == 60 && result.text_bytes == measure.text_bytes &&
              !result.truncated && items[59].sticker_id == 7 && items[59].pack_id == 42 &&
              items[59].sticker_format == 2 && !items[59].available &&
              same(items[59].pack_name, utf8_literal("Fixture pack")) &&
              items[0].pack_name.data == items[59].pack_name.data &&
              same(items[59].tags, utf8_literal("hello,wave")),
          "media catalog: measure-first full sticker packs flatten beyond visible page size and preserve "
          "format/tags/server availability");
    input = array_fixture(2, sticker);
    result = decode(MediaCatalogOperation::guild_stickers, input, 128);
    check(result.ok() && result.item_count == 2 && items[0].kind == MediaCatalogItemKind::sticker,
          "media catalog: guild sticker array shares exact typed gallery representation");
    constexpr auto whitespace_sticker = utf8_literal(
        R"({"id":"749046696482439188","name":"Think","format_type":2,"tags":"thinking, curious, ?\thuh, what","description":"Think\r\nabout\tthis"})");
    input = array_fixture(2, whitespace_sticker, true);
    const auto whitespace_measure =
        media_catalog_measure(MediaCatalogOperation::sticker_packs, input, {index, 8192}, 16);
    result = decode(MediaCatalogOperation::sticker_packs, input, 128);
    check(whitespace_measure.ok() && !whitespace_measure.truncated && result.ok() &&
              result.item_count == 2 && result.text_bytes == whitespace_measure.text_bytes &&
              same(items[0].tags, utf8_literal("thinking, curious, ?\thuh, what")) &&
              same(items[0].description, utf8_literal("Think\r\nabout\tthis")),
          "media catalog: official sticker tag tab and description whitespace preserve full pack bank");
    input = array_fixture(1, whitespace_sticker);
    result = decode(MediaCatalogOperation::guild_stickers, input, 128);
    check(result.ok() && result.item_count == 1 &&
              same(items[0].tags, utf8_literal("thinking, curious, ?\thuh, what")),
          "media catalog: guild sticker metadata admits ordinary whitespace as well");
    constexpr auto nul_tag = utf8_literal(
        R"({"id":"7","name":"Wave","format_type":2,"tags":"wave\u0000hidden"})");
    sentinel();
    result = decode(MediaCatalogOperation::sticker_packs, array_fixture(1, nul_tag, true), 128);
    check(result.error == MediaCatalogError::schema && untouched(),
          "media catalog: accepting metadata whitespace does not admit NUL or partially publish records");
    result = media_catalog_decode(MediaCatalogOperation::gif_search, utf8_literal("[]"), {index, 8192}, 16,
                                  {}, {});
    check(result.ok() && !result.item_count && !result.source_count && !result.truncated,
          "media catalog: authoritative empty result requires no output allocation");
    constexpr auto covered = utf8_literal(
        R"({"sticker_packs":[{"id":"42","name":"Pack","cover_sticker_id":"8","stickers":[)"
        R"({"id":"7","name":"First","format_type":1},{"id":"8","name":"Cover","format_type":3}]},)"
        R"({"id":"43","name":"Plain","cover_sticker_id":null,"stickers":[{"id":"9","name":"Only","format_type":2}]}]})");
    result = decode(MediaCatalogOperation::sticker_packs, covered, 128);
    check(result.ok() && result.item_count == 3 && !items[0].cover && items[1].cover && !items[2].cover,
          "media catalog: a pack's cover_sticker_id marks its rail icon sticker");
    constexpr auto standard = utf8_literal(
        R"({"id":"749054660769218631","name":"Wave","tags":"wave","type":1,"format_type":3,)"
        R"("description":"Wumpus waves","asset":"","pack_id":"847199849233514549","sort_value":12})");
    result = decode(MediaCatalogOperation::sticker, standard, 128);
    check(result.ok() && result.item_count == 1 && items[0].sticker_id == 749054660769218631 &&
              items[0].pack_id == 847199849233514549 && !items[0].guild_id && items[0].sticker_format == 3 &&
              same(items[0].name, utf8_literal("Wave")),
          "media catalog: a standard sticker names its pack");
    constexpr auto guild = utf8_literal(
        " \r\n{\"id\":\"9\",\"name\":\"Vibing\",\"tags\":\"dance\",\"type\":2,\"format_type\":1,\"description\":null,"
        "\"available\":true,\"guild_id\":\"77\",\"user\":{\"id\":\"5\"}}\n");
    result = decode(MediaCatalogOperation::sticker, guild, 128);
    check(result.ok() && result.item_count == 1 && items[0].guild_id == 77 && !items[0].pack_id &&
              items[0].available,
          "media catalog: a server sticker names its server");
    constexpr auto retired = utf8_literal(
        R"({"id":"42","name":"Old Pack","cover_sticker_id":"8","stickers":[)"
        R"({"id":"7","name":"First","format_type":1},{"id":"8","name":"Cover","format_type":1}]})");
    result = decode(MediaCatalogOperation::sticker_pack, retired, 128);
    check(result.ok() && result.item_count == 2 && items[0].pack_id == 42 && items[1].cover &&
              same(items[0].pack_name, utf8_literal("Old Pack")),
          "media catalog: one pack decodes like a pack of the list");
    sentinel();
    result = decode(MediaCatalogOperation::sticker, utf8_literal(R"([{"id":"9","name":"x","format_type":1}])"), 128);
    check(result.error == MediaCatalogError::schema && untouched(),
          "media catalog: a sticker lookup answered with a list is rejected whole");
    using S = StickerSendability;
    const bool own = guild_sticker_sendability(5, 5, true, false, 0) == S::sendable;
    const bool dm = guild_sticker_sendability(5, 0, true, false, 0) == S::premium &&
                    guild_sticker_sendability(5, 0, true, false, 2) == S::sendable &&
                    guild_sticker_sendability(5, 0, true, false, 3) == S::sendable &&
                    guild_sticker_sendability(5, 0, true, false, 1) == S::premium;
    const bool external = guild_sticker_sendability(5, 6, true, false, 2) == S::hidden &&
                          guild_sticker_sendability(5, 6, true, true, 2) == S::sendable &&
                          guild_sticker_sendability(5, 6, true, true, 0) == S::premium;
    const bool lost = guild_sticker_sendability(5, 5, false, true, 2) == S::unavailable &&
                      guild_sticker_sendability(5, 6, false, false, 2) == S::hidden &&
                      guild_sticker_sendability(0, 5, true, true, 2) == S::hidden;
    check(own && dm && external && lost,
          "media catalog: server stickers send in their server; elsewhere they need Use External Stickers and "
          "Nitro or Nitro Basic, else show locked; lost boosts show unavailable");
}
} // namespace maicord
