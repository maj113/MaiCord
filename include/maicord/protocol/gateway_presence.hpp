#pragma once
#include "maicord/protocol/gateway_commands.hpp"

namespace maicord {
// Client presence (opcode 3). MaiCord publishes the account's custom status
// and, when the user shares activity, one detected or added game.
struct GatewayPresenceUpdate {
    u64 since_ms;
    GatewayUserStatus status;
    bool has_since;
    bool afk;
};
// {"name":"Custom Status","type":4,"state":text,"emoji":{...}} with an end
// timestamp when it expires. Text or an emoji is required.
struct GatewayCustomStatusActivity {
    Utf8View text, emoji_name;
    u64 emoji_id, expires_at_ms;
};
// {"name":...,"type":0,"application_id":"...","details":...,"state":...,
// "timestamps":{"start":ms,"end":ms}}; zero ids/timestamps and empty text are
// omitted. type is 0 playing, 2 listening, 3 watching or 5 competing.
struct GatewayGameActivity {
    Utf8View name;
    u64 application_id, started_ms;
    u64 ends_ms;
    Utf8View details, state;
    u8 type;
};
GatewayCommandWriteResult gateway_write_presence_update(const GatewayPresenceUpdate& presence,
                                                        const GatewayCustomStatusActivity* custom,
                                                        const GatewayGameActivity* game,
                                                        Slice<u8> output) noexcept;

// Local presence the UI owns: automatic idle and the shared activity (a
// detected game or a plugin's rich presence). It never changes the synced
// status setting.
inline constexpr u32 kGatewayLocalGameNameBytes = 128;
inline constexpr u32 kGatewayLocalActivityTextBytes = 128;
struct GatewayLocalPresence {
    u64 idle_since_ms;       // Unix ms the user went idle; used while idle.
    u64 game_application_id; // Zero for games without a registered application.
    u64 game_started_ms;     // Unix ms.
    char game_name[kGatewayLocalGameNameBytes];
    u8 game_name_bytes;
    bool idle;
    bool game;
    u8 game_type; // 0 playing, 2 listening, 3 watching, 5 competing
    u8 game_details_bytes, game_state_bytes;
    u8 reserved[2];
    u64 game_ends_ms; // Unix ms; zero omits the end.
    char game_details[kGatewayLocalActivityTextBytes];
    char game_state[kGatewayLocalActivityTextBytes];
};
static_assert(sizeof(GatewayLocalPresence) == 424);
bool gateway_local_presence_valid(const GatewayLocalPresence&) noexcept;
bool gateway_local_presence_equal(const GatewayLocalPresence&, const GatewayLocalPresence&) noexcept;
// Automatic idle: an online user becomes idle with since and afk; a chosen idle,
// Do Not Disturb or invisible status is kept and only afk is set, so a manual
// choice is never overridden.
GatewayPresenceUpdate gateway_presence_effective(const GatewayPresenceUpdate& desired,
                                                 const GatewayLocalPresence* local) noexcept;
} // namespace maicord
