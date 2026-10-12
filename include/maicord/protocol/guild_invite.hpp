#pragma once

#include "maicord/protocol/rest.hpp"

namespace maicord {

inline constexpr u64 kGuildInviteResolveRouteId = 0x494e56495445474eull;
inline constexpr u64 kGuildInviteAcceptRouteId = 0x494e564954454143ull;
inline constexpr u32 kGuildInviteCodeMaximumBytes = 32;
inline constexpr u32 kGuildInviteMaximumTargetBytes = 128;
inline constexpr u32 kGuildInviteMaximumBodyBytes = 128;
inline constexpr u32 kGuildInviteMaximumResponseBytes = 256 * 1024;
inline constexpr u32 kGuildInviteNameBytes = 128;
inline constexpr u32 kGuildInviteIconBytes = 40;

enum class GuildInviteOperation : u8 { resolve, accept };
// The code is copied by value; the module serializes it into the target.
// Accept carries where the invite was resolved for X-Context-Properties.
struct GuildInviteRequest {
    char code[kGuildInviteCodeMaximumBytes];
    u8 code_bytes;
    GuildInviteOperation operation;
    u8 location_channel_type;
    u8 reserved[5];
    u64 location_guild_id;
    u64 location_channel_id;
};
static_assert(sizeof(GuildInviteRequest) == 56);

enum class GuildInviteWriteError : u8 { none, invalid_input, output_capacity };
struct GuildInviteWriteResult {
    u64 route_id;
    u32 target_bytes, body_bytes;
    RestMethod method;
    GuildInviteWriteError error;
    constexpr bool ok() const noexcept { return error == GuildInviteWriteError::none; }
};
bool guild_invite_code_valid(const char* code, u32 bytes) noexcept;
bool guild_invite_request_valid(const GuildInviteRequest&) noexcept;
// Resolve: GET /invites/{code}?with_counts=true&with_expiration=true.
// Accept:  POST /invites/{code} with {"session_id":...} (the Gateway session),
// or an empty object when no session is known.
GuildInviteWriteResult guild_invite_write_request(const GuildInviteRequest&, Slice<u8> target, Slice<u8> body,
                                                  Utf8View session_id = {}) noexcept;
// Accept's X-Context-Properties line (base64 JSON, location "Desktop Invite
// Modal"); returns the header bytes written, 0 on failure.
u32 guild_invite_write_context_header(const GuildInviteRequest&, Slice<u8> output) noexcept;

struct GuildInvite {
    u64 guild_id, channel_id;
    u32 member_count, presence_count;
    u8 channel_type, name_bytes, icon_bytes, channel_name_bytes, code_bytes;
    bool counts_known;
    char code[kGuildInviteCodeMaximumBytes];
    char name[kGuildInviteNameBytes];
    char icon[kGuildInviteIconBytes];
    char channel_name[kGuildInviteNameBytes];
};
enum class GuildInviteOutcome : u8 {
    resolved,
    unknown,        // expired, revoked or never existed (10006 / 404)
    captcha,        // the accept needs a CAPTCHA solution
    banned,         // 40007
    rate_limited,   // 429
    unavailable,    // other transport/status failures
    malformed,
};
// Classifies a completed response and decodes the invite object on success.
// Name, icon hash and channel name are truncated at code point boundaries.
GuildInviteOutcome guild_invite_decode_response(u16 http_status, Utf8View body, Slice<u32> structural_scratch,
                                                GuildInvite* output) noexcept;

} // namespace maicord
