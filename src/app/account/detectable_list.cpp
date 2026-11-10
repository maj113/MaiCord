// Discord's detectable list as MaiCord keeps it (game_detection.hpp): the image view, its build from the JSON the
// official client fetches, and one refresh of both lists through an injected HTTPS exchange.
#include "maicord/app/account/game_detection.hpp"
#include "maicord/app/update/https.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/reserved_bytes.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/media/compression/deflate.hpp"
#include "maicord/protocol/http/client_user_agent.hpp"

namespace maicord {
namespace {
constexpr u8 kImageMagic[4] = {'M', 'C', 'D', '1'};
constexpr u32 kDepth = 8;
constexpr u32 kExclusionTextBytes = 8 * 1024;

Utf8View file_of(Utf8View path) noexcept {
    u32 start = path.bytes;
    while (start && path.data[start - 1] != '/')
        --start;
    return {path.data + start, path.bytes - start};
}
bool string_is(const JsonValue& value, Utf8View text) noexcept {
    JsonString string{};
    return json_value_as_string(value, &string) && json_string_equals_ascii(string, text);
}
// A JSON string decoded and folded to ASCII lowercase (and, for a path, '/' separators); 0 when it does not fit,
// is empty or holds a control byte or one of `refused`.
u32 decode_lower(const JsonValue& value, char* output, u32 capacity, Utf8View refused, bool path) noexcept {
    JsonString string{};
    if (!json_value_as_string(value, &string))
        return 0;
    const auto decoded = json_string_decode_utf8(string, {output, capacity});
    if (!decoded.ok() || !decoded.bytes)
        return 0;
    for (u32 i = 0; i < decoded.bytes; ++i) {
        char& c = output[i];
        if (static_cast<u8>(c) < 0x20)
            return 0;
        for (u32 k = 0; k < refused.bytes; ++k)
            if (c == refused.data[k])
                return 0;
        c = path && c == '\\' ? '/' : ascii_lower(c);
    }
    return decoded.bytes;
}

// The games list: [{"id":"...","name":"...","executables":[{"name","os","is_launcher","arguments"}]}, ...].
struct GamesParse {
    JsonIndexedDocument document;
    const DetectableBuildScratch* scratch;
    u32 rows, keys, text;
    // The game being read.
    u64 id;
    JsonValue name, executables;
    u32 entries; // executables kept of it
    // The executable being read.
    JsonValue exe_name, exe_os, exe_arguments;
    bool launcher, overflow;
};
bool append(GamesParse& p, const char* data, u32 bytes) noexcept {
    if (bytes > p.scratch->text.count - p.text) {
        p.overflow = true;
        return false;
    }
    copy_bytes(p.scratch->text.data + p.text, data, bytes);
    p.text += bytes;
    return true;
}
bool executable_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<GamesParse*>(opaque);
    if (json_name_is(name, utf8_literal("name")))
        p.exe_name = value;
    else if (json_name_is(name, utf8_literal("os")))
        p.exe_os = value;
    else if (json_name_is(name, utf8_literal("arguments")))
        p.exe_arguments = value;
    else if (json_name_is(name, utf8_literal("is_launcher")))
        json_value_to_bool(value, &p.launcher);
    return true;
}
bool executable_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<GamesParse*>(opaque);
    p.exe_name = p.exe_os = p.exe_arguments = {};
    p.launcher = false;
    if (value.kind != JsonValueKind::object ||
        !json_visit_indexed_object(p.document, value, kDepth, executable_member, &p).ok())
        return false;
    // macOS bundles never run here; Wine and Proton run the Windows executables.
    if (!string_is(p.exe_os, utf8_literal("win32")) && !string_is(p.exe_os, utf8_literal("linux")))
        return true;
    char name[256], arguments[256];
    const u32 name_bytes = decode_lower(p.exe_name, name, sizeof(name), utf8_literal("|?*"), true);
    const u32 argument_bytes = p.exe_arguments.kind == JsonValueKind::string
                                   ? decode_lower(p.exe_arguments, arguments, sizeof(arguments), utf8_literal("|"),
                                                  false)
                                   : 0;
    if (!name_bytes || (p.exe_arguments.kind == JsonValueKind::string && !argument_bytes))
        return true;
    const Utf8View file = file_of(name[0] == '>' ? Utf8View{name + 1, name_bytes - 1} : Utf8View{name, name_bytes});
    if (!file.bytes)
        return true;
    if (p.keys == p.scratch->keys.count) {
        p.overflow = true;
        return false;
    }
    if ((p.entries && !append(p, "|", 1)) || (p.launcher && !append(p, "*", 1)) || !append(p, name, name_bytes) ||
        (argument_bytes && (!append(p, "?", 1) || !append(p, arguments, argument_bytes))))
        return false;
    p.scratch->keys[p.keys++] = {detectable_hash(file), p.rows};
    ++p.entries;
    return true;
}
bool game_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<GamesParse*>(opaque);
    if (json_name_is(name, utf8_literal("id"))) {
        JsonString id{};
        if (!json_value_as_string(value, &id) || !json_string_to_u64(id, &p.id))
            p.id = 0;
    } else if (json_name_is(name, utf8_literal("name"))) {
        p.name = value;
    } else if (json_name_is(name, utf8_literal("executables"))) {
        p.executables = value;
    }
    return true;
}
bool game_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<GamesParse*>(opaque);
    p.id = 0;
    p.name = p.executables = {};
    if (value.kind != JsonValueKind::object ||
        !json_visit_indexed_object(p.document, value, kDepth, game_member, &p).ok())
        return false;
    if (!p.id || p.name.kind != JsonValueKind::string || p.executables.kind != JsonValueKind::array)
        return true; // Discord's entry for a game it only knows by store SKUs.
    if (p.rows == p.scratch->rows.count) {
        p.overflow = true;
        return false;
    }
    const u32 text = p.text, keys = p.keys;
    JsonString name{};
    json_value_as_string(p.name, &name);
    const u32 room = minimum<u32>(p.scratch->text.count - p.text, 512);
    const auto decoded = json_string_decode_utf8(name, {p.scratch->text.data + p.text, room});
    if (!decoded.ok() || !decoded.bytes) {
        // A name over 512 bytes is skipped; one that does not fit the scratch ends the build.
        p.overflow = room < 512;
        return !p.overflow;
    }
    p.text += decoded.bytes;
    const u32 executables = p.text;
    p.entries = 0;
    if (!json_visit_indexed_array(p.document, p.executables, kDepth, executable_item, &p).ok())
        return false;
    if (!p.entries || p.text - executables > 0xFFFF) {
        p.text = text; // Nothing here can be detected.
        p.keys = keys;
        return true;
    }
    p.scratch->rows[p.rows++] = {p.id, text, executables, static_cast<u16>(decoded.bytes),
                                 static_cast<u16>(p.text - executables), 0};
    return true;
}

// The exclusions: {"executables":[...], "patterns":[...]}, each list '|'-joined.
struct ExclusionsParse {
    JsonIndexedDocument document;
    char executables[kExclusionTextBytes], patterns[kExclusionTextBytes];
    u32 executables_bytes, patterns_bytes;
    bool pattern;
};
bool exclusion_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ExclusionsParse*>(opaque);
    char* output = p.pattern ? p.patterns : p.executables;
    u32& used = p.pattern ? p.patterns_bytes : p.executables_bytes;
    const u32 at = used + (used ? 1 : 0);
    if (at >= kExclusionTextBytes)
        return true;
    const u32 bytes = decode_lower(value, output + at, kExclusionTextBytes - at, utf8_literal("|"), !p.pattern);
    if (!bytes || (p.pattern && !detectable_pattern_supported({output + at, bytes})))
        return true;
    if (used)
        output[used] = '|';
    used = at + bytes;
    return true;
}
bool exclusions_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<ExclusionsParse*>(opaque);
    p.pattern = json_name_is(name, utf8_literal("patterns"));
    if (value.kind != JsonValueKind::array || (!p.pattern && !json_name_is(name, utf8_literal("executables"))))
        return true;
    return json_visit_indexed_array(p.document, value, kDepth, exclusion_item, &p).ok();
}
bool scan(Utf8View json, Slice<u32> structurals, JsonIndexedDocument* document) noexcept {
    const auto scanned = json_scan_structurals_avx1(json, structurals);
    *document = {json, {structurals.data, scanned.count}, 0};
    return scanned.ok();
}
void copy_etag(char (&output)[kDetectableEtagBytes], u8* bytes, Utf8View etag) noexcept {
    *bytes = etag.bytes <= kDetectableEtagBytes ? static_cast<u8>(etag.bytes) : 0;
    copy_bytes(output, etag.data, *bytes);
}

// One fetch of the refresh into a growing body; If-None-Match carries the saved ETag.
struct Body {
    ReservedBytes bytes;
    u32 used, limit;
};
bool body_sink(void* context, ConstSlice<u8> data) noexcept {
    auto& b = *static_cast<Body*>(context);
    if (data.count > b.limit - b.used || !reserved_bytes_commit(&b.bytes, b.used + data.count))
        return false;
    copy_bytes(b.bytes.base + b.used, data.data, data.count);
    b.used += data.count;
    return true;
}
struct Fetched {
    UpdateHttpResponse response;
    bool fresh, answered; // 200 with a body; that or a 304
    Utf8View etag;        // the list's ETag to save: the fresh body's, else the saved one
};
// A gzip body replaced by its inflated bytes. The trailer's ISIZE (one member, as Discord sends) sizes the output;
// gzip_inflate checks the header, the CRC-32 and ISIZE, and refuses a truncated or corrupt stream.
bool gunzip(Body* body) noexcept {
    if (body->used < 18)
        return false;
    const u8* tail = body->bytes.base + body->used - 4;
    const u32 size = tail[0] | static_cast<u32>(tail[1]) << 8 | static_cast<u32>(tail[2]) << 16 |
                     static_cast<u32>(tail[3]) << 24;
    ReservedBytes inflated{};
    DeflateWorkspace* workspace = nullptr;
    bool ok = size && size <= body->limit && reserved_bytes_initialize(&inflated, size) &&
              reserved_bytes_commit(&inflated, size) && (workspace = deflate_workspace_allocate()) != nullptr;
    if (ok) {
        const auto result = gzip_inflate({body->bytes.base, body->used}, {inflated.base, size}, workspace);
        ok = result.ok() && result.output_bytes == size;
    }
    deflate_workspace_free(workspace);
    if (!ok) {
        reserved_bytes_release(&inflated);
        return false;
    }
    reserved_bytes_release(&body->bytes);
    body->bytes = inflated;
    body->used = size;
    return true;
}
void fetch(const DetectableRefresh& r, Utf8View host, Utf8View target, Utf8View etag, Body* body,
           Fetched* fetched) noexcept {
    const HttpsHeader headers[]{{utf8_literal("User-Agent"), utf8_literal(kClientUserAgent)},
                                {utf8_literal("If-None-Match"), etag}};
    const UpdateHttpRequest request{UpdateHost::provider, host, {target.data, target.bytes}, body->limit, body_sink,
                                    body};
    HttpsExchangeOptions options{{headers, etag.bytes ? 2u : 1u}};
    options.accept_gzip = true; // The list is about a quarter of its size compressed, as Discord's client gets it.
    const auto error = r.exchange(r.context, request, options, &fetched->response);
    fetched->fresh = error == UpdateTransportError::none && fetched->response.status == 200 && body->used &&
                     (!fetched->response.gzip || gunzip(body));
    fetched->answered =
        fetched->fresh || (error == UpdateTransportError::none && fetched->response.status == 304 && etag.bytes);
    fetched->etag = fetched->fresh ? Utf8View{fetched->response.etag, fetched->response.etag_bytes} : etag;
}
} // namespace

bool detectable_list_view(ConstSlice<u8> image, DetectableList* list) noexcept {
    if (!list)
        return false;
    *list = {};
    using H = DetectableImageHeader;
    if (!image.data || image.count < sizeof(H) || (reinterpret_cast<usize>(image.data) & 7))
        return false;
    const auto& h = *reinterpret_cast<const H*>(image.data);
    const u64 bytes = sizeof(H) + static_cast<u64>(h.game_count) * sizeof(DetectableGameRow) +
                      static_cast<u64>(h.key_count) * sizeof(DetectableKey) + h.text_bytes;
    if (!bytes_equal(h.magic, kImageMagic, 4) || h.bytes != image.count || bytes != image.count ||
        static_cast<u64>(h.games_text_bytes) + h.exclusions_bytes + h.patterns_bytes != h.text_bytes ||
        h.games_etag_bytes > kDetectableEtagBytes || h.exclusions_etag_bytes > kDetectableEtagBytes)
        return false;
    const auto* games = reinterpret_cast<const DetectableGameRow*>(image.data + sizeof(H));
    const auto* keys = reinterpret_cast<const DetectableKey*>(games + h.game_count);
    for (u32 i = 0; i < h.game_count; ++i) {
        const auto& g = games[i];
        if (static_cast<u64>(g.name) + g.name_bytes > h.games_text_bytes || !g.executables_bytes ||
            static_cast<u64>(g.executables) + g.executables_bytes > h.games_text_bytes)
            return false;
    }
    for (u32 i = 0; i < h.key_count; ++i)
        if (keys[i].game >= h.game_count ||
            (i && (keys[i].hash < keys[i - 1].hash ||
                   (keys[i].hash == keys[i - 1].hash && keys[i].game <= keys[i - 1].game))))
            return false;
    *list = {&h, games, keys, reinterpret_cast<const char*>(keys + h.key_count)};
    return true;
}

u32 detectable_list_build(const DetectableBuild& build, const DetectableBuildScratch& scratch,
                          Slice<u8> image) noexcept {
    const DetectableList& previous = build.previous;
    GamesParse games{};
    games.scratch = &scratch;
    if (build.games_json.bytes) {
        if (!scan(build.games_json, scratch.structurals, &games.document) ||
            !json_visit_root_array(build.games_json, games.document.structurals, kDepth, game_item, &games).ok() ||
            games.overflow)
            return 0;
    } else if (previous.header) {
        const auto& h = *previous.header;
        if (h.game_count > scratch.rows.count || h.key_count > scratch.keys.count ||
            h.games_text_bytes > scratch.text.count)
            return 0;
        copy_bytes(scratch.rows.data, previous.games, h.game_count * sizeof(DetectableGameRow));
        copy_bytes(scratch.keys.data, previous.keys, h.key_count * sizeof(DetectableKey));
        copy_bytes(scratch.text.data, previous.text, h.games_text_bytes);
        games.rows = h.game_count;
        games.keys = h.key_count;
        games.text = h.games_text_bytes;
    }
    ExclusionsParse exclusions{};
    if (build.exclusions_json.bytes) {
        if (!scan(build.exclusions_json, scratch.structurals, &exclusions.document) ||
            !json_visit_root_object(build.exclusions_json, exclusions.document.structurals, kDepth,
                                    exclusions_member, &exclusions)
                 .ok())
            return 0;
    } else if (previous.header) {
        const auto& h = *previous.header;
        const char* text = previous.text + h.games_text_bytes;
        if (h.exclusions_bytes > kExclusionTextBytes || h.patterns_bytes > kExclusionTextBytes)
            return 0;
        copy_bytes(exclusions.executables, text, h.exclusions_bytes);
        copy_bytes(exclusions.patterns, text + h.exclusions_bytes, h.patterns_bytes);
        exclusions.executables_bytes = h.exclusions_bytes;
        exclusions.patterns_bytes = h.patterns_bytes;
    }
    // Keys sorted by hash, then game, for the scans' binary search; a game naming one file twice keeps one key.
    heap_sort(scratch.keys.data, games.keys, [](const DetectableKey& a, const DetectableKey& b) noexcept {
        return a.hash != b.hash ? a.hash < b.hash : a.game < b.game;
    });
    u32 keys = 0;
    for (u32 i = 0; i < games.keys; ++i)
        if (!keys || scratch.keys[i].hash != scratch.keys[keys - 1].hash ||
            scratch.keys[i].game != scratch.keys[keys - 1].game)
            scratch.keys[keys++] = scratch.keys[i];
    using H = DetectableImageHeader;
    const u32 text = games.text + exclusions.executables_bytes + exclusions.patterns_bytes;
    const u64 bytes = sizeof(H) + static_cast<u64>(games.rows) * sizeof(DetectableGameRow) +
                      static_cast<u64>(keys) * sizeof(DetectableKey) + text;
    if (!image.data || bytes > image.count)
        return 0;
    auto& h = *reinterpret_cast<H*>(image.data);
    reset_value(&h);
    copy_bytes(h.magic, kImageMagic, 4);
    h.bytes = static_cast<u32>(bytes);
    h.game_count = games.rows;
    h.key_count = keys;
    h.text_bytes = text;
    h.games_text_bytes = games.text;
    h.exclusions_bytes = exclusions.executables_bytes;
    h.patterns_bytes = exclusions.patterns_bytes;
    h.games_checked_ms = build.games_checked_ms;
    h.exclusions_checked_ms = build.exclusions_checked_ms;
    copy_etag(h.games_etag, &h.games_etag_bytes, build.games_etag);
    copy_etag(h.exclusions_etag, &h.exclusions_etag_bytes, build.exclusions_etag);
    u8* at = image.data + sizeof(H);
    copy_bytes(at, scratch.rows.data, games.rows * sizeof(DetectableGameRow));
    at += games.rows * sizeof(DetectableGameRow);
    copy_bytes(at, scratch.keys.data, keys * sizeof(DetectableKey));
    at += keys * sizeof(DetectableKey);
    copy_bytes(at, scratch.text.data, games.text);
    copy_bytes(at + games.text, exclusions.executables, exclusions.executables_bytes);
    copy_bytes(at + games.text + exclusions.executables_bytes, exclusions.patterns, exclusions.patterns_bytes);
    return h.bytes;
}

DetectableRefreshResult detectable_refresh_run(const DetectableRefresh& r) noexcept {
    if (!r.exchange)
        return {};
    DetectableList previous{};
    detectable_list_view(r.previous, &previous);
    const DetectableImageHeader* p = previous.header;
    // The official desktop client reads the games from the CDN and the exclusions from the API.
    Body games{{}, 0, kDetectableGamesMaximumBytes};
    Body exclusions{{}, 0, kDetectableExclusionsMaximumBytes};
    DetectableRefreshResult result{};
    if (reserved_bytes_initialize(&games.bytes, games.limit) &&
        reserved_bytes_initialize(&exclusions.bytes, exclusions.limit)) {
        Fetched g{}, e{};
        fetch(r, utf8_literal("cdn.discordapp.com"), utf8_literal("/detectables/games-v1.json"),
              p ? Utf8View{p->games_etag, p->games_etag_bytes} : Utf8View{}, &games, &g);
        fetch(r, utf8_literal("discord.com"), utf8_literal("/api/v9/games/detectable/exclusions"),
              p ? Utf8View{p->exclusions_etag, p->exclusions_etag_bytes} : Utf8View{}, &exclusions, &e);
        // Nothing new, or no games at all: keep what is saved.
        if ((g.answered || e.answered) && (g.fresh || p)) {
            const u32 json = g.fresh ? games.used : 0;
            const u32 rows = g.fresh ? json / 32 + 16 : p->game_count;
            const u32 keys = g.fresh ? json / 16 + 16 : p->key_count;
            const u32 text = g.fresh ? json + 1024 : p->games_text_bytes;
            const u32 structurals = maximum(json, exclusions.used) / 2 + 64;
            const usize scratch_bytes = static_cast<usize>(structurals) * 4 + rows * sizeof(DetectableGameRow) +
                                        keys * sizeof(DetectableKey) + text;
            const usize image_bytes = sizeof(DetectableImageHeader) + rows * sizeof(DetectableGameRow) +
                                      keys * sizeof(DetectableKey) + text + 2 * kExclusionTextBytes;
            // Committed pages cost memory only as the build touches them.
            u8* scratch = static_cast<u8*>(commit_pages(scratch_bytes));
            u8* image = static_cast<u8*>(commit_pages(image_bytes));
            if (scratch && image) {
                auto* rows_at = reinterpret_cast<DetectableGameRow*>(scratch);
                auto* keys_at = reinterpret_cast<DetectableKey*>(rows_at + rows);
                auto* structurals_at = reinterpret_cast<u32*>(keys_at + keys);
                char* text_at = reinterpret_cast<char*>(structurals_at + structurals);
                const DetectableBuild build{
                    {reinterpret_cast<const char*>(games.bytes.base), json},
                    {reinterpret_cast<const char*>(exclusions.bytes.base), e.fresh ? exclusions.used : 0},
                    previous,
                    g.etag,
                    e.etag,
                    g.answered ? r.now_ms : p->games_checked_ms,
                    e.answered ? r.now_ms : p ? p->exclusions_checked_ms : 0};
                const u32 built = detectable_list_build(
                    build, {{structurals_at, structurals}, {rows_at, rows}, {keys_at, keys}, {text_at, text}},
                    {image, static_cast<u32>(image_bytes)});
                // The image keeps exactly its bytes.
                result.image = built ? static_cast<u8*>(commit_pages(built)) : nullptr;
                if (result.image) {
                    copy_bytes(result.image, image, built);
                    result.bytes = built;
                }
            }
            if (scratch)
                pages_release(scratch, scratch_bytes);
            if (image)
                pages_release(image, image_bytes);
        }
    }
    reserved_bytes_release(&games.bytes);
    reserved_bytes_release(&exclusions.bytes);
    return result;
}
} // namespace maicord
