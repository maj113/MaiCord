#include "maicord/protocol/guild/game_profile.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"
#include "../encoding/json_helpers.hpp"
#include "field_table.hpp"

namespace maicord {
namespace {
using namespace protocol_detail;

constexpr u32 kDepth = 16;

// Which source filled a field: the top level (2) wins over the supplemental data (1).
enum Field : u32 { summary, genres, platforms, publishers, developers, release, links, field_count };

struct Parse {
    const JsonIndexedDocument* document;
    GameProfile* game;
    u64 id;
    u8 rank[field_count];
    u8 current; // the rank of the object being read
    bool found;
};

bool take(Parse& p, Field field) noexcept {
    if (p.rank[field] > p.current)
        return false;
    p.rank[field] = p.current;
    return true;
}

// A publisher's or developer's name after the others, ", "-joined.
void append_name(GameProfile& g, bool publisher, const JsonValue& value) noexcept {
    char* names = publisher ? g.publishers : g.developers;
    u16& bytes = publisher ? g.publishers_bytes : g.developers_bytes;
    char name[128];
    const u32 length = clip_copy(value, name, sizeof(name), 512);
    if (!length || bytes + (bytes ? 2u : 0u) + length > kGameProfileNamesBytes)
        return;
    if (bytes) {
        copy_bytes(names + bytes, ", ", 2);
        bytes = static_cast<u16>(bytes + 2);
    }
    copy_bytes(names + bytes, name, length);
    bytes = static_cast<u16>(bytes + length);
    ++(publisher ? g.publisher_count : g.developer_count);
}

bool visit_genre(void* opaque, u32, JsonValue value) noexcept {
    auto& g = *static_cast<Parse*>(opaque)->game;
    u64 genre = 0;
    if (json_value_to_u64(value, &genre) && genre < 256 && g.genre_count < kGameProfileGenreMaximum)
        g.genres[g.genre_count++] = static_cast<u8>(genre);
    return true;
}

bool visit_platform(void* opaque, u32, JsonValue value) noexcept {
    auto& g = *static_cast<Parse*>(opaque)->game;
    u64 platform = 0;
    if (json_value_to_u64(value, &platform) && platform < 16)
        g.platforms = static_cast<u16>(g.platforms | (1u << platform));
    return true;
}

struct NameList {
    GameProfile* game;
    bool publishers;
};
bool visit_name(void* opaque, u32, JsonValue value) noexcept {
    const auto& l = *static_cast<NameList*>(opaque);
    append_name(*l.game, l.publishers, value);
    return true;
}

struct Company {
    const JsonIndexedDocument* document;
    JsonValue name;
    u32 roles; // bit per role
};
bool visit_role(void* opaque, u32, JsonValue value) noexcept {
    u64 role = 0;
    if (json_value_to_u64(value, &role) && role < 32)
        static_cast<Company*>(opaque)->roles |= 1u << role;
    return true;
}
bool visit_company_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<Company*>(opaque);
    if (name_is(name, "name"))
        c.name = value;
    else if (name_is(name, "roles") && value.kind == JsonValueKind::array)
        json_visit_indexed_array(*c.document, value, kDepth, visit_role, &c);
    return true;
}
bool visit_company(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<Parse*>(opaque);
    if (value.kind != JsonValueKind::object)
        return true;
    Company company{p.document, {}, 0};
    json_visit_indexed_object(*p.document, value, kDepth, visit_company_member, &company);
    if (company.roles & (1u << 1))
        append_name(*p.game, true, company.name);
    if (company.roles & (1u << 2))
        append_name(*p.game, false, company.name);
    return true;
}

struct Website {
    JsonValue url;
    u64 category;
};
bool visit_website_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& w = *static_cast<Website*>(opaque);
    if (name_is(name, "url"))
        w.url = value;
    else if (name_is(name, "category"))
        json_value_to_u64(value, &w.category);
    return true;
}
bool visit_website(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<Parse*>(opaque);
    auto& g = *p.game;
    if (value.kind != JsonValueKind::object || g.link_count == kGameProfileLinkMaximum)
        return true;
    Website website{};
    json_visit_indexed_object(*p.document, value, kDepth, visit_website_member, &website);
    auto& link = g.links[g.link_count];
    u16 bytes = 0;
    if (website.category < 256 && website.url.kind == JsonValueKind::string &&
        decode_text(website.url, link.url, kGameProfileUrlBytes, &bytes, false) && bytes) {
        link.category = static_cast<u8>(website.category);
        link.url_bytes = bytes;
        ++g.link_count;
    }
    return true;
}

bool visit_game(void* opaque, JsonString name, JsonValue value) noexcept;

// One list field: emptied before the higher-ranked source fills it.
bool list(Parse& p, Field field, JsonValue value, JsonArrayVisitor visitor, void* context) noexcept {
    if (value.kind != JsonValueKind::array || !take(p, field))
        return true;
    auto& g = *p.game;
    switch (field) {
    case genres: g.genre_count = 0; break;
    case platforms: g.platforms = 0; break;
    case publishers:
        g.publishers_bytes = 0;
        g.publisher_count = 0;
        break;
    case developers:
        g.developers_bytes = 0;
        g.developer_count = 0;
        break;
    case links: g.link_count = 0; break;
    default: break;
    }
    json_visit_indexed_array(*p.document, value, kDepth, visitor, context);
    return true;
}

bool visit_game(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<Parse*>(opaque);
    auto& g = *p.game;
    if (name_is(name, "id")) {
        u64 id = 0;
        if (json_value_to_snowflake(value, &id) || json_value_to_u64(value, &id))
            g.id = id;
    } else if (name_is(name, "summary_localized") || name_is(name, "summary") || name_is(name, "description")) {
        // summary_localized before summary or description, each source's ahead of the next.
        const u8 rank = static_cast<u8>(p.current * 2 + (name_is(name, "summary_localized") ? 1 : 0));
        if (value.kind == JsonValueKind::string && rank > p.rank[summary]) {
            const u32 bytes =
                copy_text_truncated<kGameProfileSummaryBytes * 2>(value, g.summary, kGameProfileSummaryBytes);
            if (bytes) {
                g.summary_bytes = static_cast<u16>(bytes);
                p.rank[summary] = rank;
            }
        }
    } else if (name_is(name, "genres")) {
        list(p, genres, value, visit_genre, &p);
    } else if (name_is(name, "platforms")) {
        list(p, platforms, value, visit_platform, &p);
    } else if (name_is(name, "companies")) {
        if (value.kind == JsonValueKind::array && p.rank[publishers] <= p.current &&
            p.rank[developers] <= p.current) {
            take(p, publishers);
            take(p, developers);
            g.publishers_bytes = g.developers_bytes = 0;
            g.publisher_count = g.developer_count = 0;
            json_visit_indexed_array(*p.document, value, kDepth, visit_company, &p);
        }
    } else if (name_is(name, "publisher_names")) {
        NameList names{&g, true};
        list(p, publishers, value, visit_name, &names);
    } else if (name_is(name, "developer_names")) {
        NameList names{&g, false};
        list(p, developers, value, visit_name, &names);
    } else if (name_is(name, "first_release_date")) {
        u64 ms = 0;
        if (json_value_to_timestamp(value, &ms) && take(p, release))
            g.release_ms = ms;
    } else if (name_is(name, "websites")) {
        list(p, links, value, visit_website, &p);
    } else if (name_is(name, "supplemental_game_data") && value.kind == JsonValueKind::object) {
        const u8 outer = p.current;
        p.current = 1;
        json_visit_indexed_object(*p.document, value, kDepth, visit_game, &p);
        p.current = outer;
    }
    return true;
}

bool visit_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<Parse*>(opaque);
    if (p.found || value.kind != JsonValueKind::object)
        return true;
    reset_value(p.game);
    reset_value(&p.rank);
    p.current = 2;
    json_visit_indexed_object(*p.document, value, kDepth, visit_game, &p);
    p.found = p.game->id == p.id;
    return true;
}

} // namespace

bool game_profile_decode(Utf8View body, Slice<u32> scratch, u64 id, GameProfile* output) noexcept {
    if (!id)
        return false;
    enum class Error : u8 { none, input, capacity, json };
    const auto scanned = scan_json<Error>(body, scratch);
    if (scanned.error != Error::none)
        return false;
    Parse p{&scanned.document, output, id, {}, 2, false};
    const auto read =
        json_visit_root_array(body, scanned.document.structurals, kDepth, visit_item, &p);
    if (!read.ok() || !p.found) {
        reset_value(output);
        return false;
    }
    return true;
}

} // namespace maicord
