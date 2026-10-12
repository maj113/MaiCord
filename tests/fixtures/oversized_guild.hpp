#pragma once

#include "maicord/protocol/writer.hpp"

// A live GUILD_CREATE at the server's per-guild limits (500 channels with eight
// permission overwrites each, 250 roles, 500 custom emoji) whose typed record
// is larger than the ordinary record capacity and so travels as one oversized
// guild record.
namespace maicord::oversized_guild_fixture {

constexpr u32 kChannels = 500, kOverwrites = 8, kRoles = 250, kEmojis = 500;

constexpr u64 channel_id(u64 guild, u32 index) noexcept { return guild + 100000 + index; }

struct Json {
    ProtocolWriter writer;
    bool ok = true;
    Json& s(const char* text) noexcept { ok = ok && protocol_writer_append_c_string(&writer, text); return *this; }
    Json& n(u64 value) noexcept { ok = ok && protocol_writer_append_u64(&writer, value); return *this; }
    Json& q(u64 value) noexcept { return s("\"").n(value).s("\""); }
};

// Returns the guild object (the GUILD_CREATE "d" value), or empty when the
// storage is too small.
inline Utf8View write(Slice<u8> storage, u64 guild, u64 self) noexcept {
    Json j{{storage, 0}};
    j.s(R"({"id":)").q(guild).s(R"(,"name":"Oversized Community","owner_id":"77","large":true,)"
        R"("member_count":18000000,"roles":[)");
    for (u32 i = 0; i < kRoles; ++i) {
        if (i) j.s(",");
        j.s(R"({"id":)").q(i ? guild + 1000 + i : guild).s(R"(,"name":"role )").n(i)
         .s(R"(","permissions":"2251799813685247","position":)").n(i).s(R"(,"color":0,"hoist":false,)"
            R"("managed":false,"mentionable":false,"flags":0})");
    }
    j.s(R"(],"emojis":[)");
    for (u32 i = 0; i < kEmojis; ++i) {
        if (i) j.s(",");
        j.s(R"({"id":)").q(guild + 10000 + i).s(R"(,"name":"emoji_)").n(i)
         .s(R"(","roles":[],"require_colons":true,"managed":false,"animated":false,"available":true})");
    }
    j.s(R"(],"members":[{"user":{"id":)").q(self).s(R"(,"username":"self"},"roles":[]}],"threads":[],"channels":[)");
    for (u32 i = 0; i < kChannels; ++i) {
        if (i) j.s(",");
        const bool category = i % 10 == 0;
        j.s(R"({"type":)").n(category ? 4 : 0).s(R"(,"position":)").n(i).s(R"(,"name":"channel-)").n(i)
         .s(R"(","id":)").q(channel_id(guild, i));
        if (!category) j.s(R"(,"parent_id":)").q(channel_id(guild, i / 10 * 10));
        j.s(R"(,"permission_overwrites":[)");
        for (u32 o = 0; o < kOverwrites; ++o) {
            if (o) j.s(",");
            j.s(R"({"type":0,"id":)").q(o ? guild + 1000 + o : guild)
             .s(o ? R"(,"deny":"0","allow":"1049600"})" : R"(,"deny":"1024","allow":"0"})");
        }
        j.s("]}");
    }
    j.s("]}");
    return j.ok ? Utf8View{reinterpret_cast<const char*>(storage.data), j.writer.used} : Utf8View{};
}

} // namespace maicord::oversized_guild_fixture
