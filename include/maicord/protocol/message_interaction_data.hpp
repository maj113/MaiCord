#pragma once
#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/rest.hpp"

namespace maicord {

// The command a message answers, shown by the command chip on click: GET
// /channels/{channel}/messages/{message}/interaction-data
// returns {"id","type","name","application_command","options":[...]} for a
// message created by an interaction (any user may read it).
inline constexpr u32 kInteractionDataTargetBytes = 96;
inline constexpr u64 kInteractionDataRouteId = 0x494e544144415441ull; // "INTADATA"

// "/api/v10/channels/{channel}/messages/{message}/interaction-data" into output;
// the byte count, or zero for a zero id or a short buffer.
u32 interaction_data_write_target(u64 channel_id, u64 message_id, Slice<u8> output) noexcept;
// Its rate-limit identity: one route per channel, a bodyless GET.
RestRouteKey interaction_data_route(u64 channel_id) noexcept;

// How option values naming an entity read: users and roles as "@name",
// channels as "#name". A null result shows the raw id (an unknown entity).
enum class InteractionDataEntity : u8 { user, channel, role };
struct InteractionDataNames {
    void* context;
    Utf8View (*resolve)(void* context, InteractionDataEntity entity, u64 id) noexcept;
};

// The command line of an interaction-data body: "/name option: value" for each
// option, sub commands and groups as bare words, attachments as "[attachment]",
// line breaks as spaces. Cut at a character boundary with an ellipsis when it
// exceeds output. Zero bytes when body is not an interaction-data object.
inline constexpr u32 kInteractionDataScratchStructurals = 1024;
u32 interaction_data_text(Utf8View body, const InteractionDataNames& names, Slice<u32> scratch,
                          Slice<char> output) noexcept;

} // namespace maicord
