#include "maicord/protocol/gateway_presence.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {
bool custom_present(const GatewayCustomStatusActivity* custom) noexcept {
    return custom && (custom->text.bytes || custom->emoji_name.bytes || custom->emoji_id);
}
bool write_custom(ProtocolWriter* w, const GatewayCustomStatusActivity& custom) noexcept {
    bool ok = protocol_write(w, "{\"name\":\"Custom Status\",\"type\":4");
    if (custom.text.bytes)
        ok = ok && protocol_write(w, ",\"state\":", json_text(custom.text));
    if (custom.expires_at_ms)
        ok = ok && protocol_write(w, ",\"timestamps\":{\"end\":", custom.expires_at_ms, '}');
    if (custom.emoji_name.bytes || custom.emoji_id) {
        ok = ok && protocol_write(w, ",\"emoji\":{\"id\":", or_null(custom.emoji_id != 0, snowflake_text(custom.emoji_id)));
        ok = ok && protocol_write(w, ",\"name\":", json_text(custom.emoji_name), ",\"animated\":false}");
    }
    return ok && protocol_write(w, '}');
}
bool game_type_valid(u8 type) noexcept {
    return type == 0 || type == 2 || type == 3 || type == 5;
}
bool write_game(ProtocolWriter* w, const GatewayGameActivity& game) noexcept {
    bool ok = protocol_write(w, "{\"name\":", json_text(game.name), ",\"type\":", game.type);
    if (game.application_id)
        ok = ok && protocol_write(w, ",\"application_id\":", snowflake_text(game.application_id));
    if (game.details.bytes)
        ok = ok && protocol_write(w, ",\"details\":", json_text(game.details));
    if (game.state.bytes)
        ok = ok && protocol_write(w, ",\"state\":", json_text(game.state));
    if (game.started_ms || game.ends_ms) {
        ok = ok && protocol_write(w, ",\"timestamps\":{");
        if (game.started_ms)
            ok = ok && protocol_write(w, "\"start\":", game.started_ms);
        if (game.ends_ms)
            ok = ok && (!game.started_ms || protocol_write(w, ',')) && protocol_write(w, "\"end\":", game.ends_ms);
        ok = ok && protocol_write(w, '}');
    }
    return ok && protocol_write(w, '}');
}
} // namespace

GatewayCommandWriteResult gateway_write_presence_update(const GatewayPresenceUpdate& presence,
                                                        const GatewayCustomStatusActivity* custom,
                                                        const GatewayGameActivity* game,
                                                        Slice<u8> output) noexcept {
    const Utf8View status = gateway_user_status_name(presence.status);
    if (!status.bytes || !output.data ||
        (presence.has_since && presence.status == GatewayUserStatus::online) ||
        (game && (!game->name.bytes || !utf8_validate(game->name).valid || !game_type_valid(game->type) ||
                  !utf8_validate(game->details).valid || !utf8_validate(game->state).valid)))
        return {0, GatewayCommandError::invalid_input};
    // The desktop client's order; since is 0 unless idle.
    ProtocolWriter w{output, 0};
    bool ok = protocol_write(&w, "{\"op\":3,\"d\":{\"status\":\"", status, "\",\"since\":",
                             presence.has_since ? presence.since_ms : 0, ",\"activities\":[");
    const bool with_custom = custom_present(custom);
    if (with_custom)
        ok = ok && write_custom(&w, *custom);
    if (game)
        ok = ok && (!with_custom || protocol_write(&w, ',')) && write_game(&w, *game);
    ok = ok && protocol_write(&w, "],\"afk\":", presence.afk ? "true" : "false", "}}");
    if (!ok)
        return {0, GatewayCommandError::output_too_small};
    return {w.used, GatewayCommandError::none};
}

bool gateway_local_presence_valid(const GatewayLocalPresence& local) noexcept {
    if (local.game_name_bytes > kGatewayLocalGameNameBytes || (local.idle && !local.idle_since_ms) ||
        local.game_details_bytes > kGatewayLocalActivityTextBytes || local.game_state_bytes > kGatewayLocalActivityTextBytes ||
        (local.game && (!local.game_name_bytes || !utf8_validate({local.game_name, local.game_name_bytes}).valid ||
                        !game_type_valid(local.game_type) ||
                        !utf8_validate({local.game_details, local.game_details_bytes}).valid ||
                        !utf8_validate({local.game_state, local.game_state_bytes}).valid ||
                        (local.game_ends_ms && local.game_ends_ms < local.game_started_ms))))
        return false;
    for (u8 b : local.reserved)
        if (b)
            return false;
    return true;
}
bool gateway_local_presence_equal(const GatewayLocalPresence& a, const GatewayLocalPresence& b) noexcept {
    return a.idle == b.idle && (!a.idle || a.idle_since_ms == b.idle_since_ms) && a.game == b.game &&
           (!a.game || (a.game_application_id == b.game_application_id && a.game_started_ms == b.game_started_ms &&
                        a.game_ends_ms == b.game_ends_ms && a.game_type == b.game_type &&
                        a.game_name_bytes == b.game_name_bytes &&
                        bytes_equal(a.game_name, b.game_name, a.game_name_bytes) &&
                        a.game_details_bytes == b.game_details_bytes &&
                        bytes_equal(a.game_details, b.game_details, a.game_details_bytes) &&
                        a.game_state_bytes == b.game_state_bytes &&
                        bytes_equal(a.game_state, b.game_state, a.game_state_bytes)));
}
GatewayPresenceUpdate gateway_presence_effective(const GatewayPresenceUpdate& desired,
                                                 const GatewayLocalPresence* local) noexcept {
    if (!local || !local->idle || !local->idle_since_ms)
        return desired;
    // Other statuses stay chosen but report when the user went idle.
    return {local->idle_since_ms, desired.status == GatewayUserStatus::online ? GatewayUserStatus::idle : desired.status,
            true, true};
}
} // namespace maicord
