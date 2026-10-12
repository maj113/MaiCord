#pragma once
#include "maicord/protocol/writer.hpp"
namespace maicord {
inline bool member_role_target(ProtocolWriter* writer, u64 guild, u64 user, Utf8View value) noexcept {
    if (!writer || !guild || !user || !value.data || !value.bytes || value.bytes > 20)
        return false;
    u64 role = 0;
    for (u32 i = 0; i < value.bytes; ++i) {
        const auto c = value.data[i];
        if (c < '0' || c > '9' || role > (~0ull - (c - '0')) / 10)
            return false;
        role = role * 10 + c - '0';
    }
    return role && protocol_writer_append_literal(writer, "/api/v10/guilds/") &&
           protocol_writer_append_u64(writer, guild) &&
           protocol_writer_append_literal(writer, "/members/") &&
           protocol_writer_append_u64(writer, user) && protocol_writer_append_literal(writer, "/roles/") &&
           protocol_writer_append_u64(writer, role);
}
} // namespace maicord
