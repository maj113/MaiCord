#include "maicord/protocol/soundboard.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/writer.hpp"
#include "json_helpers.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
using namespace protocol_detail;
using Op = SoundboardOperation;
constexpr u32 kDepth = 16;

bool guild_operation(Op op) noexcept {
    return op == Op::guild_list || op == Op::create || op == Op::update || op == Op::remove;
}
bool sound_operation(Op op) noexcept {
    return op == Op::send || op == Op::update || op == Op::remove;
}
bool append_optional_id(ProtocolWriter* w, u64 id) noexcept {
    return id ? protocol_writer_append_snowflake(w, id) : protocol_writer_append_literal(w, "null");
}
bool append_volume(ProtocolWriter* w, u32 per_mille) noexcept {
    char text[8];
    const u32 bytes = soundboard_format_volume(per_mille, {text, sizeof(text)});
    return bytes && protocol_writer_append_bytes(w, reinterpret_cast<const u8*>(text), bytes);
}
// ,"emoji_id":<id|null>,"emoji_name":<name|null>
bool append_emoji(ProtocolWriter* w, const SoundboardRequest& r) noexcept {
    return protocol_writer_append_literal(w, ",\"emoji_id\":") && append_optional_id(w, r.emoji_id) &&
           protocol_writer_append_literal(w, ",\"emoji_name\":") &&
           (r.emoji_name.bytes ? protocol_writer_append_json_string(w, r.emoji_name)
                               : protocol_writer_append_literal(w, "null"));
}
// Create body around the data URI: prefix {"name":..,"sound":" and suffix
// ","volume":..,"emoji_id":..,"emoji_name":..}.
bool create_prefix(const SoundboardRequest& r, ProtocolWriter* w) noexcept {
    const auto mime = soundboard_audio_mime(r.format);
    return protocol_writer_append_literal(w, "{\"name\":") && protocol_writer_append_json_string(w, r.name) &&
           protocol_writer_append_literal(w, ",\"sound\":\"data:") &&
           protocol_writer_append_bytes(w, reinterpret_cast<const u8*>(mime.data), mime.bytes) &&
           protocol_writer_append_literal(w, ";base64,");
}
bool create_suffix(const SoundboardRequest& r, ProtocolWriter* w) noexcept {
    return protocol_writer_append_literal(w, "\",\"volume\":") && append_volume(w, r.volume) && append_emoji(w, r) &&
           protocol_writer_append_byte(w, '}');
}
bool small_body(const SoundboardRequest& r, ProtocolWriter* w) noexcept {
    bool ok = protocol_writer_append_byte(w, '{');
    if (r.operation == Op::send) {
        // emoji_name is omitted (not null) when the sound has none.
        ok = ok && protocol_writer_append_literal(w, "\"sound_id\":") &&
             protocol_writer_append_snowflake(w, r.sound_id) &&
             protocol_writer_append_literal(w, ",\"emoji_id\":") && append_optional_id(w, r.emoji_id);
        if (r.emoji_name.bytes)
            ok = ok && protocol_writer_append_literal(w, ",\"emoji_name\":") &&
                 protocol_writer_append_json_string(w, r.emoji_name);
        if (r.guild_id)
            ok = ok && protocol_writer_append_literal(w, ",\"source_guild_id\":") &&
                 protocol_writer_append_snowflake(w, r.guild_id);
    } else {
        ok = ok && protocol_writer_append_literal(w, "\"name\":") && protocol_writer_append_json_string(w, r.name) &&
             protocol_writer_append_literal(w, ",\"volume\":") && append_volume(w, r.volume) && append_emoji(w, r);
    }
    return ok && protocol_writer_append_byte(w, '}');
}
} // namespace

bool soundboard_name_valid(Utf8View name) noexcept {
    if (!view_valid(name) || !name.bytes || name.bytes > kSoundboardNameBytes)
        return false;
    const auto validation = utf8_validate(name);
    if (!validation.valid || validation.codepoints < kSoundboardNameMinimum ||
        validation.codepoints > kSoundboardNameMaximum)
        return false;
    bool visible = false;
    for (u32 i = 0; i < name.bytes; ++i) {
        const u8 c = static_cast<u8>(name.data[i]);
        if (c < 0x20 || c == 0x7f)
            return false;
        visible = visible || c != ' ';
    }
    return visible;
}

bool soundboard_emoji_name_valid(Utf8View name) noexcept {
    if (!view_valid(name) || name.bytes > kSoundboardEmojiNameBytes || !utf8_validate(name).valid)
        return false;
    for (u32 i = 0; i < name.bytes; ++i)
        if (static_cast<u8>(name.data[i]) < 0x20)
            return false;
    return true;
}

bool soundboard_request_valid(const SoundboardRequest& r) noexcept {
    if (r.operation >= Op::count || !soundboard_emoji_name_valid(r.emoji_name) || !view_valid(r.name) ||
        !view_valid(r.reason) || !byte_range_valid(r.sound) || r.volume > kSoundboardVolumeScale)
        return false;
    if (guild_operation(r.operation) && !r.guild_id)
        return false;
    if (sound_operation(r.operation) && !r.sound_id)
        return false;
    switch (r.operation) {
    case Op::send:
        return r.channel_id != 0;
    case Op::create:
        return soundboard_name_valid(r.name) && r.sound.count && r.sound.count <= kSoundboardMaximumUploadBytes &&
               (r.format == SoundboardAudioFormat::mp3 || r.format == SoundboardAudioFormat::ogg_opus);
    case Op::update:
        return soundboard_name_valid(r.name);
    default:
        return true;
    }
}

RestMethod soundboard_method(Op op) noexcept {
    switch (op) {
    case Op::guild_list:
    case Op::default_list:
        return RestMethod::get;
    case Op::update:
        return RestMethod::patch;
    case Op::remove:
        return RestMethod::delete_method;
    default:
        return RestMethod::post;
    }
}

bool soundboard_has_body(Op op) noexcept {
    return op == Op::send || op == Op::create || op == Op::update;
}

RestRouteKey soundboard_route(const SoundboardRequest& r) noexcept {
    const u64 route = kSoundboardRouteBase + 1 + static_cast<u64>(r.operation);
    if (r.operation == Op::send)
        return {route, r.channel_id, RestMethod::post, RestMajorParameter::channel};
    if (r.operation == Op::default_list)
        return {route, 0, RestMethod::get, RestMajorParameter::none};
    return {route, r.guild_id, soundboard_method(r.operation), RestMajorParameter::guild};
}

u32 soundboard_body_bytes(const SoundboardRequest& r) noexcept {
    if (!soundboard_request_valid(r) || !soundboard_has_body(r.operation))
        return 0;
    u8 staged[kSoundboardMaximumSmallBodyBytes];
    if (r.operation != Op::create) {
        ProtocolWriter w{{staged, sizeof(staged)}, 0};
        return small_body(r, &w) ? w.used : 0;
    }
    ProtocolWriter prefix{{staged, sizeof(staged) / 2}, 0};
    ProtocolWriter suffix{{staged + sizeof(staged) / 2, sizeof(staged) / 2}, 0};
    if (!create_prefix(r, &prefix) || !create_suffix(r, &suffix))
        return 0;
    return prefix.used + base64_encoded_size(r.sound.count) + suffix.used;
}

SoundboardWriteResult soundboard_write_target(const SoundboardRequest& r, Slice<u8> output) noexcept {
    if (!soundboard_request_valid(r) || !output.data)
        return {0, SoundboardWriteError::input};
    u8 staged[kSoundboardMaximumTargetBytes];
    ProtocolWriter w{{staged, sizeof(staged)}, 0};
    bool ok = protocol_writer_append_literal(&w, "/api/v9");
    switch (r.operation) {
    case Op::send:
        ok = ok && protocol_writer_append_literal(&w, "/channels/") && protocol_writer_append_u64(&w, r.channel_id) &&
             protocol_writer_append_literal(&w, "/send-soundboard-sound");
        break;
    case Op::default_list:
        ok = ok && protocol_writer_append_literal(&w, "/soundboard-default-sounds");
        break;
    default:
        ok = ok && protocol_writer_append_literal(&w, "/guilds/") && protocol_writer_append_u64(&w, r.guild_id) &&
             protocol_writer_append_literal(&w, "/soundboard-sounds");
        if (r.operation == Op::update || r.operation == Op::remove)
            ok = ok && protocol_writer_append_byte(&w, '/') && protocol_writer_append_u64(&w, r.sound_id);
        break;
    }
    return protocol_writer_finish<SoundboardWriteResult>(w, ok, output, staged);
}

SoundboardWriteResult soundboard_write_body(const SoundboardRequest& r, Slice<u8> output) noexcept {
    if (!soundboard_request_valid(r) || !output.data || !soundboard_has_body(r.operation))
        return {0, SoundboardWriteError::input};
    if (r.operation != Op::create) {
        u8 staged[kSoundboardMaximumSmallBodyBytes];
        ProtocolWriter w{{staged, sizeof(staged)}, 0};
        const bool ok = small_body(r, &w);
        const auto result = protocol_writer_finish<SoundboardWriteResult>(w, ok, output, staged);
        secure_clear_bytes(staged, sizeof(staged));
        return result;
    }
    const u32 bytes = soundboard_body_bytes(r);
    if (!bytes || output.count < bytes)
        return {0, SoundboardWriteError::capacity};
    ProtocolWriter w{output, 0};
    bool ok = create_prefix(r, &w);
    if (ok) {
        const auto encoded = base64_encode(r.sound, {reinterpret_cast<char*>(output.data + w.used), output.count - w.used});
        ok = encoded.ok() && encoded.bytes == base64_encoded_size(r.sound.count);
        w.used += ok ? encoded.bytes : 0;
    }
    ok = ok && create_suffix(r, &w) && w.used == bytes;
    if (!ok) {
        secure_clear_bytes(output.data, minimum(output.count, bytes));
        return {0, SoundboardWriteError::capacity};
    }
    return {bytes, SoundboardWriteError::none};
}

SoundboardWriteResult soundboard_write_cdn_target(u64 sound_id, Slice<u8> output) noexcept {
    if (!sound_id || !output.data)
        return {0, SoundboardWriteError::input};
    u8 staged[kSoundboardCdnTargetBytes];
    ProtocolWriter w{{staged, sizeof(staged)}, 0};
    const bool ok = protocol_writer_append_literal(&w, "/soundboard-sounds/") && protocol_writer_append_u64(&w, sound_id);
    return protocol_writer_finish<SoundboardWriteResult>(w, ok, output, staged);
}

u32 soundboard_format_volume(u32 per_mille, Slice<char> output) noexcept {
    if (per_mille > kSoundboardVolumeScale || !output.data || output.count < 5)
        return 0;
    if (per_mille == kSoundboardVolumeScale) {
        output[0] = '1';
        return 1;
    }
    if (!per_mille) {
        output[0] = '0';
        return 1;
    }
    char digits[3] = {static_cast<char>('0' + per_mille / 100), static_cast<char>('0' + per_mille / 10 % 10),
                      static_cast<char>('0' + per_mille % 10)};
    u32 count = 3;
    while (digits[count - 1] == '0')
        --count;
    output[0] = '0';
    output[1] = '.';
    for (u32 i = 0; i < count; ++i)
        output[2 + i] = digits[i];
    return 2 + count;
}

bool soundboard_parse_volume(Utf8View text, u32* per_mille) noexcept {
    if (!per_mille || !text.data || !text.bytes)
        return false;
    u32 i = 0;
    if (text.data[0] == '-') {
        // Negative zero is the only negative number accepted.
        ++i;
    }
    u64 mantissa = 0;
    i32 exponent = 0;
    u32 digits = 0, significant = 0;
    bool nonzero = false;
    const auto digit_at = [&](u32 at) noexcept { return at < text.bytes && text.data[at] >= '0' && text.data[at] <= '9'; };
    const auto take = [&](char c, bool fraction) noexcept {
        ++digits;
        if (significant < 18) {
            mantissa = mantissa * 10 + static_cast<u64>(c - '0');
            if (mantissa)
                ++significant;
            if (fraction)
                --exponent;
        } else if (!fraction)
            ++exponent;
        nonzero = nonzero || c != '0';
    };
    while (digit_at(i))
        take(text.data[i++], false);
    if (i < text.bytes && text.data[i] == '.') {
        ++i;
        if (!digit_at(i))
            return false;
        while (digit_at(i))
            take(text.data[i++], true);
    }
    if (!digits)
        return false;
    if (i < text.bytes && (text.data[i] == 'e' || text.data[i] == 'E')) {
        ++i;
        bool negative = false;
        if (i < text.bytes && (text.data[i] == '+' || text.data[i] == '-'))
            negative = text.data[i++] == '-';
        if (!digit_at(i))
            return false;
        i32 value = 0;
        while (digit_at(i)) {
            value = value < 10000 ? value * 10 + (text.data[i] - '0') : value;
            ++i;
        }
        exponent += negative ? -value : value;
    }
    if (i != text.bytes || (text.data[0] == '-' && nonzero))
        return false;
    // round(mantissa * 10^(exponent + 3)), clamped to the scale.
    i32 shift = exponent + 3;
    if (!mantissa) {
        *per_mille = 0;
        return true;
    }
    if (shift >= 4) {
        *per_mille = kSoundboardVolumeScale;
        return true;
    }
    u64 value = mantissa;
    for (; shift > 0 && value <= kSoundboardVolumeScale; --shift)
        value *= 10;
    if (shift > 0)
        shift = 0; // Already past the scale: clamped below.
    if (shift < 0) {
        if (shift < -19) {
            *per_mille = 0;
            return true;
        }
        u64 divisor = 1;
        for (; shift < 0; ++shift)
            divisor *= 10;
        value = (value + divisor / 2) / divisor;
    }
    *per_mille = static_cast<u32>(minimum<u64>(value, kSoundboardVolumeScale));
    return true;
}

SoundboardBlock soundboard_send_check(const SoundboardSendContext& c) noexcept {
    if (!c.in_call || !c.channel_guild_id)
        return SoundboardBlock::not_in_call;
    if (!permission_has(c.permissions, kPermissionSpeak | kPermissionUseSoundboard))
        return SoundboardBlock::permission;
    if (c.self_mute || c.self_deaf || c.server_mute || c.suppress)
        return SoundboardBlock::muted;
    const bool external = c.sound_guild_id && c.sound_guild_id != c.channel_guild_id;
    if (external && !c.premium)
        return SoundboardBlock::external_premium;
    if (external && !permission_has(c.permissions, kPermissionUseExternalSounds))
        return SoundboardBlock::external_permission;
    if (!c.available)
        return SoundboardBlock::unavailable;
    if (c.now_ms < c.cooldown_until_ms)
        return SoundboardBlock::cooldown;
    return SoundboardBlock::none;
}

Utf8View soundboard_audio_mime(SoundboardAudioFormat format) noexcept {
    return format == SoundboardAudioFormat::mp3        ? utf8_literal("audio/mpeg")
           : format == SoundboardAudioFormat::ogg_opus ? utf8_literal("audio/ogg")
                                                       : Utf8View{};
}

namespace {
// Snowflakes are strings; default sound IDs may arrive as numbers.
bool parse_optional_id(JsonValue value, u64* output) noexcept {
    if (value.kind == JsonValueKind::null_value) {
        *output = 0;
        return true;
    }
    return json_value_to_u64_or_string(value, output);
}

constexpr const char* kSoundFields[] = {"sound_id", "name",      "volume",  "emoji_id",
                                        "emoji_name", "guild_id", "available", "user_id"};
constexpr u32 kSoundFieldCount = sizeof(kSoundFields) / sizeof(kSoundFields[0]);
struct SoundParse {
    SoundboardSound value;
    u32 seen;
    SoundboardDecodeError error;
};
bool visit_sound(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<SoundParse*>(opaque);
    u32 field = kSoundFieldCount;
    for (u32 i = 0; i < kSoundFieldCount && field == kSoundFieldCount; ++i)
        if (name_is(name, kSoundFields[i]))
            field = i;
    if (field == kSoundFieldCount)
        return true;
    if (p.seen & (1u << field)) {
        p.error = SoundboardDecodeError::duplicate;
        return false;
    }
    p.seen |= 1u << field;
    auto& v = p.value;
    bool valid = true;
    switch (field) {
    case 0: valid = json_value_to_u64_or_string(value, &v.sound_id) && v.sound_id; break;
    case 1: valid = decode_text(value, v.name, kSoundboardNameBytes, &v.name_bytes); break;
    case 2: {
        u32 volume = kSoundboardVolumeScale;
        valid = value.kind == JsonValueKind::null_value ||
                (value.kind == JsonValueKind::number && soundboard_parse_volume(value.source, &volume));
        v.volume = static_cast<u16>(volume);
        break;
    }
    case 3: valid = parse_optional_id(value, &v.emoji_id); break;
    case 4: valid = decode_text(value, v.emoji_name, kSoundboardEmojiNameBytes, &v.emoji_name_bytes); break;
    case 5: valid = parse_optional_id(value, &v.guild_id); break;
    case 6: valid = value.kind == JsonValueKind::null_value || json_value_to_bool(value, &v.available); break;
    default: valid = parse_optional_id(value, &v.user_id); break;
    }
    if (!valid)
        p.error = SoundboardDecodeError::field;
    return valid;
}
void sound_defaults(SoundParse* p) noexcept {
    *p = {};
    p->value.available = true;
    p->value.volume = kSoundboardVolumeScale;
}
SoundboardDecodeError finish_sound(const SoundParse& p) noexcept {
    if (!(p.seen & 1u) || !p.value.sound_id)
        return SoundboardDecodeError::identity;
    return SoundboardDecodeError::none;
}

struct SoundList {
    const JsonIndexedDocument* document;
    Slice<SoundboardSound> output;
    u64 guild_id;
    u32 count;
    SoundboardDecodeError error;
};
bool visit_sound_item(void* opaque, u32, JsonValue value) noexcept {
    auto& list = *static_cast<SoundList*>(opaque);
    if (value.kind != JsonValueKind::object) {
        list.error = SoundboardDecodeError::field;
        return false;
    }
    SoundParse parse{};
    sound_defaults(&parse);
    const auto read = json_visit_indexed_object(*list.document, value, kDepth, visit_sound, &parse);
    if (!read.ok()) {
        list.error = parse.error != SoundboardDecodeError::none ? parse.error : SoundboardDecodeError::json;
        return false;
    }
    if ((list.error = finish_sound(parse)) != SoundboardDecodeError::none)
        return false;
    if (!(parse.seen & (1u << 5)))
        parse.value.guild_id = list.guild_id;
    for (u32 i = 0; i < list.count; ++i)
        if (list.output[i].sound_id == parse.value.sound_id)
            return true; // Repeated rows collapse to the first.
    if (list.count >= list.output.count) {
        list.error = SoundboardDecodeError::capacity;
        return false;
    }
    list.output[list.count++] = parse.value;
    return true;
}
struct ListEnvelope {
    JsonValue items;
    u64 guild_id;
    bool has_items;
    SoundboardDecodeError error;
};
bool visit_envelope(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& e = *static_cast<ListEnvelope*>(opaque);
    if (name_is(name, "items") || name_is(name, "soundboard_sounds")) {
        if (e.has_items || value.kind != JsonValueKind::array) {
            e.error = SoundboardDecodeError::field;
            return false;
        }
        e.items = value;
        e.has_items = true;
    } else if (name_is(name, "guild_id") && !parse_optional_id(value, &e.guild_id)) {
        e.error = SoundboardDecodeError::field;
        return false;
    }
    return true;
}
} // namespace

SoundboardDecodeResult soundboard_decode_sound(Utf8View object, u64 guild_id, Slice<u32> scratch,
                                               SoundboardSound* output) noexcept {
    if (!output)
        return {0, SoundboardDecodeError::input};
    const auto s = scan_json<SoundboardDecodeError>(object, scratch);
    if (s.error != SoundboardDecodeError::none)
        return {0, s.error};
    SoundParse parse{};
    sound_defaults(&parse);
    const auto read = json_visit_root_object(object, s.document.structurals, kDepth, visit_sound, &parse);
    if (!read.ok())
        return {0, parse.error != SoundboardDecodeError::none ? parse.error : SoundboardDecodeError::json};
    if (const auto error = finish_sound(parse); error != SoundboardDecodeError::none)
        return {0, error};
    if (!(parse.seen & (1u << 5)))
        parse.value.guild_id = guild_id;
    *output = parse.value;
    return {1, SoundboardDecodeError::none};
}

SoundboardDecodeResult soundboard_decode_sound_list(Utf8View body, u64 guild_id, Slice<u32> scratch,
                                                    Slice<SoundboardSound> output, u64* document_guild) noexcept {
    if (!output.data && output.count)
        return {0, SoundboardDecodeError::input};
    const auto s = scan_json<SoundboardDecodeError>(body, scratch);
    if (s.error != SoundboardDecodeError::none)
        return {0, s.error};
    const auto& document = s.document;
    SoundList list{&document, output, guild_id, 0, SoundboardDecodeError::none};
    u32 first = 0;
    while (first < body.bytes && (body.data[first] == ' ' || body.data[first] == '\t' || body.data[first] == '\r' ||
                                  body.data[first] == '\n'))
        ++first;
    if (first < body.bytes && body.data[first] == '[') {
        const auto read = json_visit_root_array(body, document.structurals, kDepth, visit_sound_item, &list);
        if (!read.ok())
            return {0, list.error != SoundboardDecodeError::none ? list.error : SoundboardDecodeError::json};
        if (document_guild)
            *document_guild = guild_id;
        return {list.count, SoundboardDecodeError::none};
    }
    ListEnvelope envelope{};
    const auto read = json_visit_root_object(body, document.structurals, kDepth, visit_envelope, &envelope);
    if (!read.ok())
        return {0, envelope.error != SoundboardDecodeError::none ? envelope.error : SoundboardDecodeError::json};
    if (!envelope.has_items)
        return {0, SoundboardDecodeError::field};
    if (envelope.guild_id)
        list.guild_id = envelope.guild_id;
    const auto items = json_visit_indexed_array(document, envelope.items, kDepth, visit_sound_item, &list);
    if (!items.ok())
        return {0, list.error != SoundboardDecodeError::none ? list.error : SoundboardDecodeError::json};
    if (document_guild)
        *document_guild = list.guild_id;
    return {list.count, SoundboardDecodeError::none};
}

namespace {
struct DeleteParse {
    u64 sound_id, guild_id;
    SoundboardDecodeError error;
};
bool visit_delete(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& d = *static_cast<DeleteParse*>(opaque);
    bool valid = true;
    if (name_is(name, "sound_id"))
        valid = json_value_to_u64_or_string(value, &d.sound_id);
    else if (name_is(name, "guild_id"))
        valid = parse_optional_id(value, &d.guild_id);
    if (!valid)
        d.error = SoundboardDecodeError::field;
    return valid;
}

struct EffectParse {
    const JsonIndexedDocument* document;
    GatewayVoiceChannelEffect value;
    SoundboardDecodeError error;
};
bool visit_effect_emoji(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& e = *static_cast<EffectParse*>(opaque);
    bool valid = true;
    if (name_is(name, "id"))
        valid = parse_optional_id(value, &e.value.emoji_id);
    else if (name_is(name, "name"))
        valid = decode_text(value, e.value.emoji_name, kSoundboardEmojiNameBytes, &e.value.emoji_name_bytes);
    else if (name_is(name, "animated"))
        valid = value.kind == JsonValueKind::null_value || json_value_to_bool(value, &e.value.emoji_animated);
    if (!valid)
        e.error = SoundboardDecodeError::field;
    return valid;
}
bool visit_effect(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& e = *static_cast<EffectParse*>(opaque);
    auto& v = e.value;
    bool valid = true;
    u64 number = 0;
    if (name_is(name, "channel_id"))
        valid = json_value_to_u64_or_string(value, &v.channel_id);
    else if (name_is(name, "guild_id"))
        valid = parse_optional_id(value, &v.guild_id);
    else if (name_is(name, "user_id"))
        valid = json_value_to_u64_or_string(value, &v.user_id);
    else if (name_is(name, "sound_id"))
        valid = parse_optional_id(value, &v.sound_id);
    else if (name_is(name, "animation_id"))
        valid = value.kind == JsonValueKind::null_value || json_value_to_u64_or_string(value, &v.animation_id);
    else if (name_is(name, "animation_type")) {
        valid = value.kind == JsonValueKind::null_value || (json_value_to_u64(value, &number) && number <= 255);
        v.animation_type = static_cast<u8>(number);
    } else if (name_is(name, "sound_volume")) {
        u32 volume = kSoundboardVolumeScale;
        valid = value.kind == JsonValueKind::null_value ||
                (value.kind == JsonValueKind::number && soundboard_parse_volume(value.source, &volume));
        v.sound_volume = static_cast<u16>(volume);
        v.has_sound_volume = value.kind != JsonValueKind::null_value;
    } else if (name_is(name, "emoji")) {
        if (value.kind == JsonValueKind::object) {
            const auto read = json_visit_indexed_object(*e.document, value, kDepth, visit_effect_emoji, &e);
            valid = read.ok();
        } else
            valid = value.kind == JsonValueKind::null_value;
    }
    if (!valid && e.error == SoundboardDecodeError::none)
        e.error = SoundboardDecodeError::field;
    return valid;
}
} // namespace

SoundboardDecodeResult soundboard_decode_delete(Utf8View object, Slice<u32> scratch, u64* sound_id,
                                                u64* guild_id) noexcept {
    if (!sound_id || !guild_id)
        return {0, SoundboardDecodeError::input};
    const auto s = scan_json<SoundboardDecodeError>(object, scratch);
    if (s.error != SoundboardDecodeError::none)
        return {0, s.error};
    DeleteParse d{};
    const auto read = json_visit_root_object(object, s.document.structurals, kDepth, visit_delete, &d);
    if (!read.ok())
        return {0, d.error != SoundboardDecodeError::none ? d.error : SoundboardDecodeError::json};
    if (!d.sound_id)
        return {0, SoundboardDecodeError::identity};
    *sound_id = d.sound_id;
    *guild_id = d.guild_id;
    return {1, SoundboardDecodeError::none};
}

SoundboardDecodeResult soundboard_decode_effect(Utf8View object, Slice<u32> scratch,
                                                GatewayVoiceChannelEffect* output) noexcept {
    if (!output)
        return {0, SoundboardDecodeError::input};
    const auto s = scan_json<SoundboardDecodeError>(object, scratch);
    if (s.error != SoundboardDecodeError::none)
        return {0, s.error};
    EffectParse e{&s.document, {}, SoundboardDecodeError::none};
    e.value.sound_volume = kSoundboardVolumeScale;
    const auto read = json_visit_root_object(object, s.document.structurals, kDepth, visit_effect, &e);
    if (!read.ok())
        return {0, e.error != SoundboardDecodeError::none ? e.error : SoundboardDecodeError::json};
    if (!e.value.channel_id || !e.value.user_id)
        return {0, SoundboardDecodeError::identity};
    *output = e.value;
    return {1, SoundboardDecodeError::none};
}

} // namespace maicord
