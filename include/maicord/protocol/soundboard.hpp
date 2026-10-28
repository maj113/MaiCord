#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/gateway_events.hpp"
#include "maicord/model/permission_bits.hpp"

namespace maicord {
enum class RestMethod : u8;
struct RestRouteKey;

// Soundboard: guild and default sounds, sending a sound into the current voice
// channel and the Manage Expressions page. Default sounds belong to the
// pseudo-guild 0.
inline constexpr u32 kSoundboardNameMinimum = 2;
inline constexpr u32 kSoundboardNameMaximum = 32;
inline constexpr u32 kSoundboardNameBytes = kSoundboardNameMaximum * 4;
// A Unicode emoji sequence or a custom emoji's name.
inline constexpr u32 kSoundboardEmojiNameBytes = 64;
inline constexpr u32 kSoundboardMaximumUploadBytes = 512 * 1024;
inline constexpr u32 kSoundboardMaximumDurationMs = 5200;
// Sound volume is a 0..1 float on the wire; MaiCord keeps it in per-mille.
inline constexpr u32 kSoundboardVolumeScale = 1000;
inline constexpr u32 kSoundboardMaximumTargetBytes = 96;
inline constexpr u32 kSoundboardMaximumSmallBodyBytes = 1024;
inline constexpr u64 kSoundboardRouteBase = 0x534F554E44000000ull; // "SOUND"
// Free guilds get 8 slots; boosts raise it (level 1: 24, 2: 36, 3: 48).
inline constexpr u32 kSoundboardGuildSlots[4] = {8, 24, 36, 48};
// CDN path of every sound, default or custom: /soundboard-sounds/{id}.
inline constexpr u32 kSoundboardCdnTargetBytes = 48;

enum class SoundboardOperation : u8 {
    send,         // POST   /channels/{c}/send-soundboard-sound {sound_id, emoji_id, emoji_name?, source_guild_id?}
    guild_list,   // GET    /guilds/{g}/soundboard-sounds -> {"items":[...]}
    default_list, // GET    /soundboard-default-sounds -> [...]
    create,       // POST   /guilds/{g}/soundboard-sounds {name, sound, volume, emoji_id, emoji_name}
    update,       // PATCH  /guilds/{g}/soundboard-sounds/{s} {name, volume, emoji_id, emoji_name}
    remove,       // DELETE /guilds/{g}/soundboard-sounds/{s}
    probe,        // local: checks `sound` (MP3/Ogg, size, duration); no request
    count
};

enum class SoundboardAudioFormat : u8 {
    unknown,
    mp3,
    ogg_opus,
};

// Upload admission: an MP3 or Ogg Opus file of at most 512 KiB and 5.2 s,
// measured by the native demuxers (every frame/page is parsed). The account
// module checks it (probe, and again before create); the client has no decoder.
enum class SoundboardFileError : u8 { none, input, empty, too_large, format, malformed, too_long, memory };
struct SoundboardFileInfo {
    u64 frames;
    u32 duration_ms;
    u32 sample_rate;
    u32 bytes;
    SoundboardAudioFormat format;
    u8 channels;
    SoundboardFileError error;
    constexpr bool ok() const noexcept {
        return error == SoundboardFileError::none;
    }
};
static_assert(sizeof(SoundboardFileInfo) == 24);

// Crosses the account module ABI (append-only). Views are borrowed for the call.
struct SoundboardRequest {
    SoundboardOperation operation;
    SoundboardAudioFormat format; // create: the sound file's format.
    u16 volume;                   // create/update: 0..1000 per-mille.
    u32 reserved;
    // list/create/update/remove: the owning guild. send: the sound's guild
    // (0 for a default sound; source_guild_id is then omitted).
    u64 guild_id;
    u64 channel_id; // send
    u64 sound_id;   // send/update/remove
    u64 emoji_id;   // send/create/update: custom emoji, 0 for none or Unicode.
    Utf8View emoji_name;
    Utf8View name;        // create/update
    ConstSlice<u8> sound; // create: the MP3/Ogg file
    Utf8View reason;      // Audit log; empty sends no header.
    // create/probe: receives the file check (format, duration) when set.
    SoundboardFileInfo* info;
};
static_assert(sizeof(SoundboardRequest) == 112);

enum class SoundboardWriteError : u8 { none, input, capacity };
struct SoundboardWriteResult {
    u32 bytes;
    SoundboardWriteError error;
    constexpr bool ok() const noexcept {
        return error == SoundboardWriteError::none;
    }
};

bool soundboard_name_valid(Utf8View) noexcept;
bool soundboard_emoji_name_valid(Utf8View) noexcept;
bool soundboard_request_valid(const SoundboardRequest&) noexcept;
RestMethod soundboard_method(SoundboardOperation) noexcept;
bool soundboard_has_body(SoundboardOperation) noexcept;
RestRouteKey soundboard_route(const SoundboardRequest&) noexcept;
// Exact body size (zero when bodyless); create grows with the data URI.
u32 soundboard_body_bytes(const SoundboardRequest&) noexcept;
// Writers leave output untouched on failure.
SoundboardWriteResult soundboard_write_target(const SoundboardRequest&, Slice<u8> output) noexcept;
SoundboardWriteResult soundboard_write_body(const SoundboardRequest&, Slice<u8> output) noexcept;
// "/soundboard-sounds/{id}" on cdn.discordapp.com.
SoundboardWriteResult soundboard_write_cdn_target(u64 sound_id, Slice<u8> output) noexcept;
// Per-mille as a JSON number: 1000 -> "1", 500 -> "0.5", 5 -> "0.005".
u32 soundboard_format_volume(u32 per_mille, Slice<char> output) noexcept;
// JSON number in [0, 1] (exponent forms included) rounded to per-mille.
bool soundboard_parse_volume(Utf8View number, u32* per_mille) noexcept;

// Sound object (REST lists, GUILD_SOUNDBOARD_SOUND_CREATE/UPDATE). Strings are
// owned and not NUL-terminated.
struct SoundboardSound {
    u64 sound_id;
    u64 guild_id; // 0 for default sounds.
    u64 emoji_id;
    u64 user_id; // Uploader, when the service includes it.
    u16 volume;  // per-mille
    u8 name_bytes;
    u8 emoji_name_bytes;
    bool available;
    u8 reserved[3];
    char name[kSoundboardNameBytes];
    char emoji_name[kSoundboardEmojiNameBytes];
};

enum class SoundboardDecodeError : u8 { none, input, json, field, identity, capacity, duplicate };
struct SoundboardDecodeResult {
    u32 count;
    SoundboardDecodeError error;
    constexpr bool ok() const noexcept {
        return error == SoundboardDecodeError::none;
    }
};
// One sound object. guild_id falls back to `guild_id` when absent.
SoundboardDecodeResult soundboard_decode_sound(Utf8View object, u64 guild_id, Slice<u32> scratch,
                                               SoundboardSound* output) noexcept;
// A root array (default sounds), {"items":[...]} (guild list) or the Gateway's
// {"guild_id","soundboard_sounds":[...]} (SOUNDBOARD_SOUNDS,
// GUILD_SOUNDBOARD_SOUNDS_UPDATE). Rows inherit guild_id (or the document's)
// when absent; *document_guild receives the document's guild when given.
SoundboardDecodeResult soundboard_decode_sound_list(Utf8View body, u64 guild_id, Slice<u32> scratch,
                                                    Slice<SoundboardSound> output,
                                                    u64* document_guild = nullptr) noexcept;
// GUILD_SOUNDBOARD_SOUND_DELETE {"sound_id","guild_id"}.
SoundboardDecodeResult soundboard_decode_delete(Utf8View object, Slice<u32> scratch, u64* sound_id,
                                                u64* guild_id) noexcept;

// VOICE_CHANNEL_EFFECT_SEND: a soundboard sound or an emoji reaction in a
// voice channel. sound_id is zero for emoji-only effects.
struct GatewayVoiceChannelEffect {
    u64 channel_id, guild_id, user_id;
    u64 sound_id;
    u64 emoji_id;
    u64 animation_id;
    u16 sound_volume; // per-mille; 1000 when absent.
    u8 animation_type;
    u8 emoji_name_bytes;
    bool emoji_animated;
    bool has_sound_volume;
    u8 reserved[2];
    char emoji_name[kSoundboardEmojiNameBytes];
};
SoundboardDecodeResult soundboard_decode_effect(Utf8View object, Slice<u32> scratch,
                                                GatewayVoiceChannelEffect* output) noexcept;

// Who may play a sound in the current voice channel: Speak and Use Soundboard
// there, not muted or suppressed, the sound available, and a sound from another
// server only with Nitro and Use External Sounds. Default sounds (guild 0) play
// everywhere.
enum class SoundboardBlock : u8 {
    none,
    not_in_call,
    permission,       // Speak or Use Soundboard missing.
    muted,            // Self or server muted, deafened, or suppressed on a stage.
    external_premium, // Another server's sound without Nitro.
    external_permission,
    unavailable,      // The sound is unavailable (lost boosts).
    cooldown,
};
struct SoundboardSendContext {
    PermissionSet permissions; // Effective permissions in the voice channel.
    u64 channel_guild_id;
    u64 sound_guild_id;
    u64 now_ms, cooldown_until_ms;
    bool in_call, premium, available;
    bool self_mute, self_deaf, server_mute, suppress;
};
SoundboardBlock soundboard_send_check(const SoundboardSendContext&) noexcept;

} // namespace maicord
