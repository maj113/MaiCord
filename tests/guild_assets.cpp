#include "maicord/app/account_guild_assets.hpp"
#include "maicord/app/guild_image.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/media/png.hpp"
#include "maicord/platform/clipboard_image.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/guild_admin.hpp"
#include "test_output.hpp"

namespace maicord {
namespace guild_assets_test {

u32 failures;
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}

// Capture-the-submission: the last request handed to the transport.
struct Capture {
    RestRouteKey route;
    u64 id;
    u32 submissions, large_submissions, cancellations, target_bytes, body_bytes, reason_bytes, boundary_bytes,
        timeout_ms;
    u8 target[kRestClientMaximumTargetBytes], reason[kRestAuditReasonMaximumEncodedBytes], boundary[80];
    u8* body;
    bool json, large;
    RestClientSubmitError result;
} capture;
constexpr u32 kCaptureBodyBytes = 4 * 1024 * 1024;

bool same(const u8* data, u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(data), bytes}, expected);
}

u8* scratch(u64 bytes) noexcept {
    return static_cast<u8*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(bytes), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}

// Builders -------------------------------------------------------------------

// Stored RGBA PNG: left half red, right half blue, top row transparent.
ConstSlice<u8> make_png(u32 width, u32 height) noexcept {
    const u64 bytes = clipboard_image_png_bytes(width, height);
    u8* rgba = scratch(u64{width} * height * 4);
    u8* png = scratch(bytes);
    for (u32 y = 0; y < height; ++y)
        for (u32 x = 0; x < width; ++x) {
            u8* p = rgba + (u64{y} * width + x) * 4;
            const bool left = x < width / 2;
            p[0] = left ? 255 : 0;
            p[1] = 0;
            p[2] = left ? 0 : 255;
            p[3] = y ? 255 : 0;
        }
    const u64 written = clipboard_image_encode_png(rgba, width, height, width * 4, {png, static_cast<u32>(bytes)});
    VirtualFree(rgba, 0, MEM_RELEASE);
    return {png, static_cast<u32>(written)};
}

// Structurally valid GIF with `frames` images and `padding` bytes of data.
ConstSlice<u8> make_gif(u32 width, u32 height, u32 frames, u32 padding) noexcept {
    u8* gif = scratch(64 + frames * 32 + padding + padding / 255 + 16);
    u32 at = 0;
    const auto put = [&](u8 value) noexcept { gif[at++] = value; };
    for (const char c : "GIF89a")
        if (c)
            put(static_cast<u8>(c));
    put(static_cast<u8>(width));
    put(static_cast<u8>(width >> 8));
    put(static_cast<u8>(height));
    put(static_cast<u8>(height >> 8));
    put(0);
    put(0);
    put(0);
    for (u32 f = 0; f < frames; ++f) {
        put(0x21); // graphic control extension
        put(0xF9);
        put(4);
        put(0);
        put(10);
        put(0);
        put(0);
        put(0);
        put(0x2C);
        for (u32 i = 0; i < 4; ++i)
            put(0);
        put(static_cast<u8>(width));
        put(static_cast<u8>(width >> 8));
        put(static_cast<u8>(height));
        put(static_cast<u8>(height >> 8));
        put(0);
        put(2);
        u32 left = f ? 1 : maximum(padding, 1u);
        while (left) {
            const u32 block = minimum(left, 255u);
            put(static_cast<u8>(block));
            for (u32 i = 0; i < block; ++i)
                put(0x55);
            left -= block;
        }
        put(0);
    }
    put(0x3B);
    return {gif, at};
}

ConstSlice<u8> make_jpeg(u32 width, u32 height) noexcept {
    static u8 jpeg[64];
    const u8 bytes[]{0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0,
                     0xFF, 0xC0, 0x00, 0x11, 8, static_cast<u8>(height >> 8), static_cast<u8>(height),
                     static_cast<u8>(width >> 8), static_cast<u8>(width), 3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1,
                     0xFF, 0xD9};
    copy_bytes(jpeg, bytes, sizeof(bytes));
    return {jpeg, sizeof(bytes)};
}

ConstSlice<u8> make_webp(u32 width, u32 height, bool animated) noexcept {
    static u8 webp[30];
    const u8 bytes[]{'R', 'I', 'F', 'F', 22, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'X', 10, 0, 0, 0,
                     static_cast<u8>(animated ? 0x02 : 0), 0, 0, 0, static_cast<u8>(width - 1),
                     static_cast<u8>((width - 1) >> 8), 0, static_cast<u8>(height - 1),
                     static_cast<u8>((height - 1) >> 8), 0};
    copy_bytes(webp, bytes, sizeof(bytes));
    return {webp, sizeof(bytes)};
}

ConstSlice<u8> text_bytes(Utf8View text) noexcept {
    return {reinterpret_cast<const u8*>(text.data), text.bytes};
}

// Expected "data:<mime>;base64,..." for raw bytes.
Utf8View data_uri(Utf8View mime, ConstSlice<u8> raw) noexcept {
    static char* buffer = nullptr;
    if (!buffer)
        buffer = reinterpret_cast<char*>(scratch(kCaptureBodyBytes));
    u32 at = 0;
    for (const char c : "data:")
        if (c)
            buffer[at++] = c;
    copy_bytes(buffer + at, mime.data, mime.bytes);
    at += mime.bytes;
    for (const char c : ";base64,")
        if (c)
            buffer[at++] = c;
    const auto encoded = base64_encode(raw, {buffer + at, kCaptureBodyBytes - at});
    return {buffer, at + encoded.bytes};
}

GuildAssetRequest asset(GuildAssetOperation op, u64 target = 0) noexcept {
    GuildAssetRequest request{};
    request.operation = op;
    request.guild_id = 10;
    request.target_id = target;
    return request;
}

// Protocol ----------------------------------------------------------------------

void test_protocol() noexcept {
    const u8 png[] = {1, 2, 3, 4, 5};
    const GuildPreparedImage image{{png, sizeof(png)}, GuildImageKind::png};
    u8 uri[64];
    const u32 uri_bytes = guild_image_write_data_uri(GuildImageKind::png, {png, sizeof(png)}, {uri, sizeof(uri)});
    check(uri_bytes == guild_image_data_uri_bytes(GuildImageKind::png, 5) &&
              same(uri, uri_bytes, utf8_literal("data:image/png;base64,AQIDBAU=")) &&
              !guild_image_write_data_uri(GuildImageKind::png, {png, 5}, {uri, uri_bytes - 1}) &&
              !guild_image_data_uri_bytes(GuildImageKind::none, 5) &&
              utf8_equal(guild_image_mime(GuildImageKind::lottie), utf8_literal("application/json")),
          "data URI: exact framing and padding; short output and unknown kinds are refused");

    struct Row {
        GuildAssetRequest request;
        GuildPreparedImage image;
        RestMethod method;
        Utf8View target, body;
    };
    Row rows[16]{};
    u32 count = 0;
    const auto add = [&](GuildAssetRequest r, GuildPreparedImage i, RestMethod m, Utf8View t, Utf8View b) noexcept {
        rows[count++] = {r, i, m, t, b};
    };
    auto r = asset(GuildAssetOperation::guild_icon);
    r.image = {png, sizeof(png)};
    add(r, image, RestMethod::patch, utf8_literal("/api/v10/guilds/10"),
        utf8_literal(R"({"icon":"data:image/png;base64,AQIDBAU="})"));
    r.image = {};
    add(r, {}, RestMethod::patch, utf8_literal("/api/v10/guilds/10"), utf8_literal(R"({"icon":null})"));
    r = asset(GuildAssetOperation::guild_banner);
    add(r, {}, RestMethod::patch, utf8_literal("/api/v10/guilds/10"), utf8_literal(R"({"banner":null})"));
    r = asset(GuildAssetOperation::role_icon, 30);
    r.image = {png, sizeof(png)};
    add(r, image, RestMethod::patch, utf8_literal("/api/v10/guilds/10/roles/30"),
        utf8_literal(R"({"icon":"data:image/png;base64,AQIDBAU="})"));
    r = asset(GuildAssetOperation::emoji_create);
    r.name = utf8_literal("party_blob");
    r.image = {png, sizeof(png)};
    add(r, image, RestMethod::post, utf8_literal("/api/v10/guilds/10/emojis"),
        utf8_literal(R"({"name":"party_blob","image":"data:image/png;base64,AQIDBAU="})"));
    r = asset(GuildAssetOperation::emoji_update, 40);
    r.name = utf8_literal("blob2");
    add(r, {}, RestMethod::patch, utf8_literal("/api/v10/guilds/10/emojis/40"), utf8_literal(R"({"name":"blob2"})"));
    add(asset(GuildAssetOperation::emoji_delete, 40), {}, RestMethod::delete_method,
        utf8_literal("/api/v10/guilds/10/emojis/40"), {});
    add(asset(GuildAssetOperation::sticker_list), {}, RestMethod::get, utf8_literal("/api/v10/guilds/10/stickers"),
        {});
    r = asset(GuildAssetOperation::sticker_update, 50);
    r.fields = guild_asset_field_tags | guild_asset_field_description;
    r.tags = utf8_literal("wave");
    r.description = {};
    add(r, {}, RestMethod::patch, utf8_literal("/api/v10/guilds/10/stickers/50"),
        utf8_literal(R"({"tags":"wave","description":""})"));
    add(asset(GuildAssetOperation::sticker_delete, 50), {}, RestMethod::delete_method,
        utf8_literal("/api/v10/guilds/10/stickers/50"), {});
    r = asset(GuildAssetOperation::guild_mfa);
    r.mfa_level = 1;
    add(r, {}, RestMethod::post, utf8_literal("/api/v10/guilds/10/mfa"), utf8_literal(R"({"level":1})"));
    add(asset(GuildAssetOperation::guild_delete), {}, RestMethod::post,
        utf8_literal("/api/v10/guilds/10/delete"), {});
    bool exact = true;
    for (u32 i = 0; i < count; ++i) {
        const auto& row = rows[i];
        u8 target[128], body[256];
        const auto t = guild_asset_write_target(row.request, {target, sizeof(target)});
        const auto b = guild_asset_write_body(row.request, row.image, {}, {body, sizeof(body)});
        const u32 sized = guild_asset_body_bytes(row.request, row.image, {});
        const auto route = guild_asset_route(row.request);
        const bool ok = guild_asset_request_valid(row.request) && t.ok() && b.ok() &&
                        same(target, t.bytes, row.target) && same(body, b.bytes, row.body) && sized == b.bytes &&
                        route.method == row.method && route.major == RestMajorParameter::guild &&
                        route.major_key == 10 &&
                        route.route_id == kGuildAssetRouteBase + static_cast<u64>(row.request.operation) &&
                        guild_asset_has_body(row.request.operation) == (row.body.bytes != 0);
        if (!ok) {
            check(false, "guild assets protocol: route row mismatch");
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), row.target.data, row.target.bytes, nullptr, nullptr);
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, nullptr, nullptr);
        }
        exact = exact && ok;
        // Every short body buffer is refused and left cleared.
        for (u32 size = 1; ok && size < b.bytes; ++size)
            exact = exact && guild_asset_write_body(row.request, row.image, {}, {body, size}).error ==
                                 GuildAssetError::capacity;
    }
    check(exact && count == 12, "guild assets protocol: every operation writes its exact method, path, size and body");

    // Multipart sticker upload with a boundary absent from every value.
    r = asset(GuildAssetOperation::sticker_create);
    r.name = utf8_literal("Wave");
    r.tags = utf8_literal("wave");
    r.description = utf8_literal("Hello there");
    r.image = {png, sizeof(png)};
    u8 boundary[kGuildAssetBoundaryBytes];
    const bool chosen = guild_asset_choose_boundary(r, image, 7, {boundary, sizeof(boundary)});
    u8 body[1024];
    const auto written = guild_asset_write_body(r, image, {boundary, sizeof(boundary)}, {body, sizeof(body)});
    char expected[1024];
    u32 at = 0;
    const auto put = [&](Utf8View text) noexcept {
        copy_bytes(expected + at, text.data, text.bytes);
        at += text.bytes;
    };
    const Utf8View b{reinterpret_cast<const char*>(boundary), sizeof(boundary)};
    const auto part = [&](Utf8View name, Utf8View value) noexcept {
        put(utf8_literal("--"));
        put(b);
        put(utf8_literal("\r\nContent-Disposition: form-data; name=\""));
        put(name);
        put(utf8_literal("\"\r\n\r\n"));
        put(value);
        put(utf8_literal("\r\n"));
    };
    part(utf8_literal("name"), utf8_literal("Wave"));
    part(utf8_literal("tags"), utf8_literal("wave"));
    part(utf8_literal("description"), utf8_literal("Hello there"));
    put(utf8_literal("--"));
    put(b);
    put(utf8_literal("\r\nContent-Disposition: form-data; name=\"file\"; filename=\"sticker.png\"\r\n"
                     "Content-Type: image/png\r\n\r\n"));
    put({reinterpret_cast<const char*>(png), sizeof(png)});
    put(utf8_literal("\r\n--"));
    put(b);
    put(utf8_literal("--\r\n"));
    check(chosen && same(boundary, 19, utf8_literal("MaiCordFormBoundary")) &&
              rest_multipart_boundary_valid({boundary, sizeof(boundary)}) && written.ok() &&
              same(body, written.bytes, {expected, at}) &&
              guild_asset_body_bytes(r, image, {boundary, sizeof(boundary)}) == written.bytes &&
              guild_asset_method(r.operation) == RestMethod::post &&
              !guild_asset_write_body(r, image, {}, {body, sizeof(body)}).ok(),
          "multipart: sticker uploads encode name, tags, description and file parts exactly");
    // A value that contains the first candidate forces another boundary.
    u8 first[kGuildAssetBoundaryBytes];
    guild_asset_choose_boundary(r, image, 99, {first, sizeof(first)});
    auto colliding = r;
    colliding.description = {reinterpret_cast<const char*>(first), sizeof(first)};
    u8 second[kGuildAssetBoundaryBytes];
    check(guild_asset_choose_boundary(colliding, image, 99, {second, sizeof(second)}) &&
              !bytes_equal(first, second, sizeof(first)),
          "multipart: a boundary never occurs inside the encoded values");
    GuildPreparedImage lottie{text_bytes(utf8_literal(R"({"v":"5","layers":[]})")), GuildImageKind::lottie};
    r.image = lottie.bytes;
    const auto json = guild_asset_write_body(r, lottie, {boundary, sizeof(boundary)}, {body, sizeof(body)});
    bool json_part = false;
    constexpr char lottie_head[] = "filename=\"sticker.json\"\r\nContent-Type: application/json\r\n";
    for (u32 i = 0; json.ok() && i + sizeof(lottie_head) - 1 <= json.bytes && !json_part; ++i)
        json_part = bytes_equal(body + i, lottie_head, sizeof(lottie_head) - 1);
    check(json_part, "multipart: Lottie stickers upload as application/json");

    // Rejections differ from a valid request by one property.
    auto bad = asset(GuildAssetOperation::emoji_create);
    bad.image = {png, sizeof(png)};
    bad.name = utf8_literal("a");
    bool rejected = !guild_asset_request_valid(bad);
    bad.name = utf8_literal("has space");
    rejected = rejected && !guild_asset_request_valid(bad);
    bad.name = utf8_literal("abcdefghijklmnopqrstuvwxyz0123456");
    rejected = rejected && !guild_asset_request_valid(bad);
    bad.name = utf8_literal("ok_name");
    bad.image = {};
    rejected = rejected && !guild_asset_request_valid(bad);
    bad = asset(GuildAssetOperation::emoji_delete);
    rejected = rejected && !guild_asset_request_valid(bad);
    bad = asset(GuildAssetOperation::sticker_list);
    bad.reason = utf8_literal("reads carry no reason");
    rejected = rejected && !guild_asset_request_valid(bad);
    bad = asset(GuildAssetOperation::guild_mfa);
    bad.mfa_level = 2;
    rejected = rejected && !guild_asset_request_valid(bad);
    bad = asset(GuildAssetOperation::sticker_update, 50);
    rejected = rejected && !guild_asset_request_valid(bad);
    bad.fields = guild_asset_field_description;
    bad.description = utf8_literal("x");
    rejected = rejected && !guild_asset_request_valid(bad);
    bad = asset(GuildAssetOperation::sticker_create);
    bad.name = utf8_literal("Wave");
    bad.image = {png, sizeof(png)};
    rejected = rejected && !guild_asset_request_valid(bad);
    bad = asset(GuildAssetOperation::guild_delete);
    bad.name = utf8_literal("stray");
    rejected = rejected && !guild_asset_request_valid(bad);
    bad = asset(GuildAssetOperation::guild_icon);
    bad.guild_id = 0;
    rejected = rejected && !guild_asset_request_valid(bad);
    bad = asset(GuildAssetOperation::image_probe);
    bad.image = {png, sizeof(png)};
    bad.profile = GuildImageProfile::count;
    rejected = rejected && !guild_asset_request_valid(bad);
    auto icon = asset(GuildAssetOperation::guild_icon);
    icon.image = {png, sizeof(png)};
    const GuildPreparedImage lottie_icon{{png, sizeof(png)}, GuildImageKind::lottie};
    rejected = rejected && !guild_asset_write_body(icon, lottie_icon, {}, {body, sizeof(body)}).ok();
    check(rejected, "guild assets protocol: names, targets, levels, fields, reasons and image kinds are validated");

    GuildStickerRecord stickers[3]{};
    u32 units[2048];
    const auto decoded = guild_asset_decode_stickers(
        utf8_literal(R"([{"id":"9","name":"Wave","tags":"wave","description":null,"format_type":1,"available":true,)"
                     R"("guild_id":"10","user":{"id":"7"}},{"id":"8","name":"Spin","tags":"","description":"A spin",)"
                     R"("format_type":3,"available":false}])"),
        {units, 2048}, {stickers, 3});
    check(decoded.ok() && decoded.count == 2 && stickers[0].id == 9 && stickers[0].format_type == 1 &&
              !stickers[0].description_bytes && stickers[1].format_type == 3 && !stickers[1].available &&
              stickers[1].description_bytes == 6 &&
              guild_asset_decode_stickers(utf8_literal(R"([{"id":"9","name":"a"},{"id":"9","name":"b"}])"), {units, 2048},
                                          {stickers, 3})
                      .error == GuildAssetError::duplicate &&
              guild_asset_decode_stickers(utf8_literal(R"([{"id":"9"}])"), {units, 2048}, {stickers, 3}).error ==
                  GuildAssetError::identity &&
              guild_asset_decode_stickers(utf8_literal(R"([{"id":"9","name":"a","format_type":9}])"), {units, 2048},
                                          {stickers, 3})
                      .error == GuildAssetError::field &&
              guild_asset_error_code(utf8_literal(R"({"message":"Two factor is required","code":60003})"),
                                     {units, 2048}) == 60003 &&
              !guild_asset_error_code(utf8_literal("<html>"), {units, 2048}),
          "guild assets decode: sticker lists and error codes");

    u8 head[2048];
    RestRequestHead request{};
    request.target = text_bytes(utf8_literal("/api/v10/guilds/10/stickers"));
    request.method = RestMethod::post;
    request.json_body = true;
    request.content_length = 123;
    request.multipart_boundary = {boundary, sizeof(boundary)};
    const auto head_written = write_rest_request_head(request, {head, sizeof(head)});
    char line[160];
    u32 line_bytes = 0;
    for (char c : "\r\nContent-Type: multipart/form-data; boundary=")
        if (c)
            line[line_bytes++] = c;
    copy_bytes(line + line_bytes, boundary, sizeof(boundary));
    line_bytes += sizeof(boundary);
    for (char c : "\r\nContent-Length: 123\r\n")
        if (c)
            line[line_bytes++] = c;
    bool found = false;
    for (u32 i = 0; head_written.ok() && i + line_bytes <= head_written.bytes && !found; ++i)
        found = bytes_equal(head + i, line, line_bytes);
    request.json_body = false;
    request.content_length = 0;
    const bool bodyless = !write_rest_request_head(request, {head, sizeof(head)}).ok();
    request.json_body = true;
    request.multipart_boundary = text_bytes(utf8_literal("has space"));
    check(found && bodyless && !write_rest_request_head(request, {head, sizeof(head)}).ok(),
          "request head: multipart bodies carry their boundary; invalid or bodyless boundaries are refused");
}

// Image preparation ----------------------------------------------------------------

void test_images() noexcept {
    GuildImageBuffer buffer{};
    GuildPreparedImage prepared{};
    const auto emoji_png = make_png(300, 200);
    auto info = guild_image_prepare(GuildImageProfile::emoji, emoji_png, &buffer, &prepared);
    PngImage parsed{};
    const bool parse_ok = prepared.bytes.count && png_parse(prepared.bytes, &parsed).ok();
    bool colors = false;
    if (parse_ok) {
        // Decode the output and sample both halves of the centred crop.
        u8* scan = scratch(parsed.inflated_bytes);
        DeflateWorkspace workspace{};
        if (png_decode_parsed_scanlines(&parsed, &workspace, {scan, parsed.inflated_bytes}).ok()) {
            const u8* row = scan + 64 * parsed.row_stride + 1;
            colors = row[10 * 4] == 255 && row[10 * 4 + 2] == 0 && row[120 * 4] == 0 && row[120 * 4 + 2] == 255 &&
                     row[120 * 4 + 3] == 255 && (scan + 1)[3] < 255;
        }
        VirtualFree(scan, 0, MEM_RELEASE);
    }
    check(info.error == GuildImageError::none && info.resized && info.kind == GuildImageKind::png &&
              info.width == 128 && info.height == 128 && info.source_width == 300 && info.source_height == 200 &&
              info.bytes <= kGuildEmojiImageMaximumBytes && buffer.data && prepared.bytes.data == buffer.data &&
              parse_ok && parsed.width == 128 && parsed.height == 128 && colors,
          "image: an oversized emoji PNG is centre-cropped square, area-averaged to 128x128 and re-encoded");
    guild_image_release(&buffer);
    check(!buffer.data && !buffer.bytes, "image: the re-encoded buffer is erased and released");

    const auto small_png = make_png(64, 64);
    info = guild_image_prepare(GuildImageProfile::emoji, small_png, &buffer, &prepared);
    check(info.error == GuildImageError::none && !info.resized && !buffer.data &&
              prepared.bytes.data == small_png.data && prepared.kind == GuildImageKind::png,
          "image: a square emoji within limits uploads unchanged without allocation");

    const auto banner_png = make_png(1000, 1000);
    info = guild_image_prepare(GuildImageProfile::banner, banner_png, &buffer, &prepared);
    check(info.error == GuildImageError::none && info.resized && info.width == 1000 && info.height == 562 &&
              prepared.bytes.count == info.bytes,
          "image: a square banner is centre-cropped to 16:9");
    guild_image_release(&buffer);
    info = guild_image_prepare(GuildImageProfile::profile_banner, banner_png, &buffer, &prepared);
    check(info.error == GuildImageError::none && info.resized && info.width == 1000 && info.height == 352 &&
              prepared.bytes.count == info.bytes,
          "image: a square profile banner is centre-cropped to the server's 17:6");
    guild_image_release(&buffer);

    const auto tiny = make_png(32, 32);
    check(guild_image_prepare(GuildImageProfile::role_icon, tiny, &buffer, &prepared).error ==
                  GuildImageError::dimensions &&
              guild_image_prepare(GuildImageProfile::sticker, tiny, &buffer, &prepared).error ==
                  GuildImageError::dimensions &&
              !buffer.data,
          "image: role icons need 64x64 and stickers 320x320");
    const auto sticker_png = make_png(400, 400);
    info = guild_image_prepare(GuildImageProfile::sticker, sticker_png, &buffer, &prepared);
    check(info.error == GuildImageError::none && info.width == 320 && info.height == 320 &&
              info.bytes <= kGuildStickerMaximumBytes,
          "image: a larger static PNG sticker is resized to exactly 320x320");
    guild_image_release(&buffer);

    const auto animated = make_gif(320, 320, 2, 64);
    info = guild_image_prepare(GuildImageProfile::sticker, animated, &buffer, &prepared);
    check(info.error == GuildImageError::none && info.animated && info.kind == GuildImageKind::gif &&
              prepared.bytes.data == animated.data &&
              guild_image_prepare(GuildImageProfile::role_icon, animated, &buffer, &prepared).error ==
                  GuildImageError::kind &&
              guild_image_prepare(GuildImageProfile::sticker, make_gif(300, 320, 2, 8), &buffer, &prepared).error ==
                  GuildImageError::dimensions,
          "image: animated GIF stickers must be 320x320; role icons refuse animation");
    const auto big_gif = make_gif(128, 128, 1, 300 * 1024);
    check(guild_image_sniff(big_gif) == GuildImageKind::gif &&
              guild_image_prepare(GuildImageProfile::emoji, big_gif, &buffer, &prepared).error ==
                  GuildImageError::too_large,
          "image: formats that cannot be resized must already fit the 256 KB emoji limit");
    info = guild_image_prepare(GuildImageProfile::icon, make_jpeg(2000, 1000), &buffer, &prepared);
    check(info.error == GuildImageError::none && info.kind == GuildImageKind::jpeg && info.source_width == 2000 &&
              info.source_height == 1000 && !info.resized &&
              guild_image_prepare(GuildImageProfile::sticker, make_jpeg(320, 320), &buffer, &prepared).error ==
                  GuildImageError::kind,
          "image: JPEG dimensions come from the frame header; stickers refuse JPEG");
    info = guild_image_prepare(GuildImageProfile::emoji, make_webp(100, 100, true), &buffer, &prepared);
    check(info.error == GuildImageError::none && info.kind == GuildImageKind::webp && info.animated &&
              info.width == 100 &&
              guild_image_prepare(GuildImageProfile::role_icon, make_webp(100, 100, true), &buffer, &prepared).error ==
                  GuildImageError::kind,
          "image: extended WebP reports size and animation");
    const auto lottie = text_bytes(utf8_literal(R"( {"v":"5.7","fr":30,"layers":[{"ty":4}]} )"));
    check(guild_image_prepare(GuildImageProfile::sticker, lottie, &buffer, &prepared).kind == GuildImageKind::lottie &&
              guild_image_prepare(GuildImageProfile::emoji, lottie, &buffer, &prepared).error ==
                  GuildImageError::format &&
              guild_image_prepare(GuildImageProfile::icon, text_bytes(utf8_literal("not an image")), &buffer,
                                  &prepared)
                      .error == GuildImageError::format &&
              guild_image_prepare(GuildImageProfile::icon, {}, &buffer, &prepared).error == GuildImageError::input,
          "image: Lottie JSON is a sticker format only; unknown and empty files are refused");
}

// Account submission ------------------------------------------------------------------

bool captured(RestMethod method, GuildAssetOperation op, Utf8View target, Utf8View body,
              bool large = false) noexcept {
    return capture.route.method == method && capture.route.major == RestMajorParameter::guild &&
           capture.route.major_key == 10 && capture.route.route_id == kGuildAssetRouteBase + static_cast<u64>(op) &&
           capture.json == (body.bytes != 0) && capture.large == large &&
           capture.timeout_ms == (large ? kGuildAssetLargeTimeoutMs : kRestClientDefaultTimeoutMs) &&
           same(capture.target, capture.target_bytes, target) && same(capture.body, capture.body_bytes, body);
}

void test_account() noexcept {
    capture.body = scratch(kCaptureBodyBytes);
    RestClient details{};
    const auto small_png = make_png(64, 64);
    GuildImageInfo info{};
    auto request = asset(GuildAssetOperation::guild_icon);
    request.image = small_png;
    request.info = &info;
    request.reason = utf8_literal("new look");
    const auto icon_body = [&]() noexcept {
        static char json[256 * 1024];
        const auto uri = data_uri(utf8_literal("image/png"), small_png);
        u32 at = 0;
        for (char c : R"({"icon":")")
            if (c)
                json[at++] = c;
        copy_bytes(json + at, uri.data, uri.bytes);
        at += uri.bytes;
        json[at++] = '"';
        json[at++] = '}';
        return Utf8View{json, at};
    }();
    check(account_guild_assets_submit(&details, &request, 201).ok() && capture.id == 201 &&
              captured(RestMethod::patch, GuildAssetOperation::guild_icon, utf8_literal("/api/v10/guilds/10"),
                       icon_body) &&
              same(capture.reason, capture.reason_bytes, utf8_literal("new%20look")) && !capture.boundary_bytes &&
              info.error == GuildImageError::none && info.width == 64,
          "account: the server icon PATCH carries a data URI and the audit reason");

    request.image = {};
    request.reason = {};
    check(account_guild_assets_submit(&details, &request, 202).ok() &&
              captured(RestMethod::patch, GuildAssetOperation::guild_icon, utf8_literal("/api/v10/guilds/10"),
                       utf8_literal(R"({"icon":null})")),
          "account: removing the icon sends null");

    // 700x394 is within 1% of 16:9: uploaded unchanged, over 64 KiB as base64.
    const auto banner_png = make_png(700, 394);
    request = asset(GuildAssetOperation::guild_banner);
    request.image = banner_png;
    const u32 before_large = capture.large_submissions;
    const bool banner_ok = account_guild_assets_submit(&details, &request, 203).ok();
    const auto banner_uri = data_uri(utf8_literal("image/png"), banner_png);
    check(banner_ok && capture.large_submissions == before_large + 1 && capture.large &&
              capture.timeout_ms == kGuildAssetLargeTimeoutMs &&
              capture.body_bytes == banner_uri.bytes + sizeof(R"({"banner":""})") - 1 &&
              bytes_equal(capture.body + 11, banner_uri.data, banner_uri.bytes),
          "account: a banner body over 64 KiB takes the large-body path with the upload timeout");

    request = asset(GuildAssetOperation::role_icon, 30);
    request.image = make_png(256, 256);
    check(account_guild_assets_submit(&details, &request, 204).ok() && capture.route.route_id ==
                  kGuildAssetRouteBase + static_cast<u64>(GuildAssetOperation::role_icon) &&
              same(capture.target, capture.target_bytes, utf8_literal("/api/v10/guilds/10/roles/30")) &&
              capture.body_bytes < 256 * 1024,
          "account: role icons are resized before the PATCH");

    request = asset(GuildAssetOperation::emoji_create);
    request.name = utf8_literal("blob");
    request.image = small_png;
    static char emoji_json[256 * 1024];
    u32 at = 0;
    for (char c : R"({"name":"blob","image":")")
        if (c)
            emoji_json[at++] = c;
    const auto emoji_uri = data_uri(utf8_literal("image/png"), small_png);
    copy_bytes(emoji_json + at, emoji_uri.data, emoji_uri.bytes);
    at += emoji_uri.bytes;
    emoji_json[at++] = '"';
    emoji_json[at++] = '}';
    check(account_guild_assets_submit(&details, &request, 205).ok() &&
              captured(RestMethod::post, GuildAssetOperation::emoji_create,
                       utf8_literal("/api/v10/guilds/10/emojis"), {emoji_json, at}),
          "account: emoji upload POSTs the name and image data URI");

    const auto submissions = capture.submissions + capture.large_submissions;
    request.image = make_gif(128, 128, 1, 300 * 1024);
    info = {};
    request.info = &info;
    check(account_guild_assets_submit(&details, &request, 206).error == RestClientSubmitError::invalid_input &&
              info.error == GuildImageError::too_large &&
              capture.submissions + capture.large_submissions == submissions,
          "account: an emoji over 256 KB never reaches the transport and reports why");

    request = asset(GuildAssetOperation::sticker_create);
    request.name = utf8_literal("Wave");
    request.tags = utf8_literal("wave");
    const auto sticker_gif = make_gif(320, 320, 2, 70 * 1024);
    request.image = sticker_gif;
    const bool sticker_ok = account_guild_assets_submit(&details, &request, 207).ok();
    bool file_part = false;
    constexpr char head[] = "filename=\"sticker.gif\"\r\nContent-Type: image/gif\r\n\r\n";
    for (u32 i = 0; sticker_ok && i + sizeof(head) - 1 <= capture.body_bytes && !file_part; ++i)
        if (bytes_equal(capture.body + i, head, sizeof(head) - 1))
            file_part = bytes_equal(capture.body + i + sizeof(head) - 1, sticker_gif.data, sticker_gif.count);
    check(sticker_ok && capture.large && capture.boundary_bytes == kGuildAssetBoundaryBytes &&
              same(capture.body, 2, utf8_literal("--")) &&
              bytes_equal(capture.body + 2, capture.boundary, capture.boundary_bytes) && file_part &&
              same(capture.target, capture.target_bytes, utf8_literal("/api/v10/guilds/10/stickers")),
          "account: sticker uploads are multipart with the boundary handed to the transport");

    request = asset(GuildAssetOperation::guild_mfa);
    request.mfa_level = 1;
    check(account_guild_assets_submit(&details, &request, 208).ok() &&
              captured(RestMethod::post, GuildAssetOperation::guild_mfa, utf8_literal("/api/v10/guilds/10/mfa"),
                       utf8_literal(R"({"level":1})")),
          "account: the moderation 2FA level POSTs to /mfa");
    request = asset(GuildAssetOperation::guild_delete);
    const bool deleted = account_guild_assets_submit(&details, &request, 209).ok() &&
                         captured(RestMethod::post, GuildAssetOperation::guild_delete,
                                  utf8_literal("/api/v10/guilds/10/delete"), {}) &&
                         !capture.body_bytes && !capture.json && !capture.reason_bytes;
    // The head the transport writes for it: a zero length and no Content-Type,
    // as a fetch without a body sends.
    RestRequestHead delete_head{};
    delete_head.target = {capture.target, capture.target_bytes};
    delete_head.method = capture.route.method;
    delete_head.json_body = capture.json;
    delete_head.content_length = capture.body_bytes;
    u8 delete_wire[1024];
    const auto head_written = write_rest_request_head(delete_head, {delete_wire, sizeof(delete_wire)});
    const auto contains_text = [&](const char* text) noexcept {
        u32 bytes = 0;
        while (text[bytes])
            ++bytes;
        for (u32 i = 0; head_written.ok() && i + bytes <= head_written.bytes; ++i)
            if (bytes_equal(delete_wire + i, text, bytes))
                return true;
        return false;
    };
    check(deleted && head_written.ok() && contains_text("POST /api/v10/guilds/10/delete HTTP/1.1\r\n") &&
              contains_text("\r\nContent-Length: 0\r\n") && !contains_text("Content-Type"),
          "account: deleting the server POSTs to /delete with a 0-byte body and no Content-Type");
    // Create a server: POST /guilds from the blank template, no guild major.
    request = {};
    request.operation = GuildAssetOperation::guild_create;
    request.name = utf8_literal("Rob\xE2\x80\x99s \"server\"");
    check(account_guild_assets_submit(&details, &request, 220).ok() && capture.route.method == RestMethod::post &&
              capture.route.major == RestMajorParameter::none && capture.route.major_key == 0 &&
              capture.route.route_id == kGuildAssetRouteBase + static_cast<u64>(GuildAssetOperation::guild_create) &&
              capture.json && !capture.large && same(capture.target, capture.target_bytes, utf8_literal("/api/v10/guilds")) &&
              same(capture.body, capture.body_bytes,
                   utf8_literal("{\"name\":\"Rob\xE2\x80\x99s \\\"server\\\"\",\"icon\":null,\"channels\":[],"
                                "\"system_channel_id\":null,\"guild_template_code\":\"2TffvPucqHkN\"}")),
          "account: creating a server POSTs the name with a null icon and the blank template");
    request.image = small_png;
    static char create_json[256 * 1024];
    u32 create_at = 0;
    for (char c : "{\"name\":\"Rob\xE2\x80\x99s \\\"server\\\"\",\"icon\":\"")
        if (c)
            create_json[create_at++] = c;
    const auto create_uri = data_uri(utf8_literal("image/png"), small_png);
    copy_bytes(create_json + create_at, create_uri.data, create_uri.bytes);
    create_at += create_uri.bytes;
    for (char c : "\",\"channels\":[],\"system_channel_id\":null,\"guild_template_code\":\"2TffvPucqHkN\"}")
        if (c)
            create_json[create_at++] = c;
    check(account_guild_assets_submit(&details, &request, 221).ok() &&
              same(capture.body, capture.body_bytes, {create_json, create_at}),
          "account: a picked server icon travels as the new server's data URI");
    auto refused = request;
    refused.guild_id = 10;
    auto unnamed = request;
    unnamed.name = utf8_literal("x");
    auto blank = request;
    blank.name = utf8_literal("   ");
    const u32 create_total = capture.submissions + capture.large_submissions;
    check(account_guild_assets_submit(&details, &refused, 222).error == RestClientSubmitError::invalid_input &&
              account_guild_assets_submit(&details, &unnamed, 223).error == RestClientSubmitError::invalid_input &&
              account_guild_assets_submit(&details, &blank, 224).error == RestClientSubmitError::invalid_input &&
              capture.submissions + capture.large_submissions == create_total,
          "account: a new server needs a 2-100 character name and no guild ID");

    request = asset(GuildAssetOperation::sticker_list);
    check(account_guild_assets_submit(&details, &request, 210).ok() &&
              captured(RestMethod::get, GuildAssetOperation::sticker_list,
                       utf8_literal("/api/v10/guilds/10/stickers"), {}),
          "account: the sticker list is a bodyless GET");

    request = asset(GuildAssetOperation::image_probe);
    request.profile = GuildImageProfile::emoji;
    request.image = make_png(300, 200);
    info = {};
    request.info = &info;
    const u32 total = capture.submissions + capture.large_submissions;
    check(account_guild_assets_probe(&request) && info.resized && info.width == 128 &&
              account_guild_assets_submit(&details, &request, 211).error == RestClientSubmitError::invalid_input &&
              capture.submissions + capture.large_submissions == total,
          "account: probing prepares locally and never submits");
    // A second probe with an output writes the prepared image's data URI.
    request.profile = GuildImageProfile::icon;
    request.image = make_png(64, 64);
    info = {};
    const bool sized = account_guild_assets_probe(&request) && !info.resized;
    const u32 uri_bytes = guild_image_data_uri_bytes(info.kind, info.bytes);
    u8* uri = scratch(uri_bytes);
    request.data_uri = {uri, uri_bytes};
    const auto expected_uri = data_uri(utf8_literal("image/png"), request.image);
    const bool written = sized && account_guild_assets_probe(&request) &&
                         same(uri, uri_bytes, expected_uri);
    request.data_uri = {uri, uri_bytes - 1};
    check(written && !account_guild_assets_probe(&request) &&
              (request.data_uri = {uri, uri_bytes}, request.operation = GuildAssetOperation::guild_icon,
               !guild_asset_request_valid(request)),
          "account: a probe writes the exact data URI and refuses a mis-sized or non-probe output");

    capture.result = RestClientSubmitError::busy;
    request = asset(GuildAssetOperation::guild_delete);
    check(account_guild_assets_submit(&details, &request, 212).error == RestClientSubmitError::busy &&
              (capture.result = RestClientSubmitError::none, account_guild_assets_cancel(&details, 212).ok()) &&
              capture.cancellations == 1 &&
              account_guild_assets_cancel(&details, 0).error == RestClientSubmitError::invalid_input &&
              account_guild_assets_submit(nullptr, &request, 1).error == RestClientSubmitError::invalid_input,
          "account: transport refusals propagate and cancellation targets the exact request");
}

} // namespace guild_assets_test

namespace {
RestClientSubmitResult record(const RestClientRequest& request, bool large) noexcept {
    auto& c = guild_assets_test::capture;
    if (request.target.count > sizeof(c.target) || request.body.count > guild_assets_test::kCaptureBodyBytes ||
        request.audit_reason.count > sizeof(c.reason) || request.multipart_boundary.count > sizeof(c.boundary))
        return {RestClientSubmitError::invalid_input};
    (large ? c.large_submissions : c.submissions)++;
    c.large = large;
    c.route = request.route;
    c.id = request.request_id;
    c.timeout_ms = request.timeout_ms;
    c.json = request.json_body;
    c.target_bytes = request.target.count;
    c.body_bytes = request.body.count;
    c.reason_bytes = request.audit_reason.count;
    c.boundary_bytes = request.multipart_boundary.count;
    copy_bytes(c.target, request.target.data, request.target.count);
    copy_bytes(c.body, request.body.data, request.body.count);
    copy_bytes(c.reason, request.audit_reason.data, request.audit_reason.count);
    copy_bytes(c.boundary, request.multipart_boundary.data, request.multipart_boundary.count);
    return {c.result};
}
} // namespace

RestClientSubmitResult rest_client_submit(RestClient*,
                                          const RestClientRequest& request) noexcept {
    // The ordinary entry keeps its ceiling; larger bodies must use the large one.
    if (request.body.count > kRestClientMaximumRequestBodyBytes)
        return {RestClientSubmitError::body_capacity};
    return record(request, false);
}

RestClientSubmitResult rest_client_submit_large(RestClient*,
                                                const RestClientRequest& request) noexcept {
    if (request.body.count > kRestClientMaximumLargeRequestBodyBytes)
        return {RestClientSubmitError::body_capacity};
    return record(request, true);
}

bool rest_client_cancel(RestClient*, u64 request_id) noexcept {
    ++guild_assets_test::capture.cancellations;
    return request_id == 212;
}

RestClientSubmitResult rest_client_submit_large_owned(
    RestClient* client, const RestClientRequest& request, u8** body_owner) noexcept {
    if (!body_owner || !*body_owner || *body_owner != request.body.data)
        return {RestClientSubmitError::invalid_input};
    // This copy-only backend leaves the body with the caller on every result.
    return rest_client_submit_large(client, request);
}

} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_guild_assets_entry() noexcept {
    using namespace maicord::guild_assets_test;
    test_protocol();
    test_images();
    test_account();
    constexpr char pass[] = "PASS guild assets protocol, images and transport\n";
    constexpr char fail[] = "FAIL guild assets protocol, images and transport\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failures ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failures ? 1 : 0);
}
