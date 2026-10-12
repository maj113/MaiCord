#include "maicord/app/account_soundboard.hpp"
#include "maicord/app/voice_playback.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/protocol/base64.hpp"
#include "test_output.hpp"

// Soundboard: Gateway effect and sound decoding, capture-the-submission
// request bodies, send gating, upload checks with generated MP3/Ogg files,
// the decoded-clip LRU, mixer gain and the module engine's fetch-to-play path.
namespace maicord {
namespace soundboard_test {
u32 failures;
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}
bool same(const u8* data, u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(data), bytes}, expected);
}
u32 scratch[16 * 1024];

// Transport capture ---------------------------------------------------------------
struct Capture {
    RestRouteKey route;
    u64 id;
    u32 submissions, large, cancellations, target_bytes, body_bytes, reason_bytes, timeout_ms;
    u8 target[kRestClientMaximumTargetBytes], reason[256];
    u8* body;
    u32 body_capacity;
    bool json;
} capture;

// Generated fixtures --------------------------------------------------------------
u8* allocate(u32 bytes) noexcept {
    return static_cast<u8*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}
// MPEG-1 Layer III, 128 kb/s, 44.1 kHz silent frames (1152 samples, 417 bytes).
u32 mp3_silence(u8* bytes, u32 frames) noexcept {
    for (u32 i = 0; i < frames; ++i) {
        u8* f = bytes + i * 417;
        clear_bytes(f, 417);
        f[0] = 0xff, f[1] = 0xfb, f[2] = 0x90, f[3] = 0xc0;
    }
    return frames * 417;
}
void put32(u8* p, u32 value) noexcept {
    for (u32 i = 0; i < 4; ++i)
        p[i] = static_cast<u8>(value >> (8 * i));
}
u32 ogg_crc(const u8* p, u32 count) noexcept {
    u32 crc = 0;
    for (u32 i = 0; i < count; ++i) {
        crc ^= u32{i >= 22 && i < 26 ? u8{0} : p[i]} << 24;
        for (u32 bit = 0; bit < 8; ++bit)
            crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0);
    }
    return crc;
}
u32 ogg_page(u8* out, u32 sequence, u8 flags, u64 granule, u32 packets, const u8* bodies, u32 packet_bytes) noexcept {
    copy_bytes(out, "OggS", 4);
    out[4] = 0;
    out[5] = flags;
    for (u32 i = 0; i < 8; ++i)
        out[6 + i] = static_cast<u8>(granule >> (8 * i));
    put32(out + 14, 7);
    put32(out + 18, sequence);
    put32(out + 22, 0);
    out[26] = static_cast<u8>(packets);
    for (u32 i = 0; i < packets; ++i)
        out[27 + i] = static_cast<u8>(packet_bytes);
    copy_bytes(out + 27 + packets, bodies, packets * packet_bytes);
    const u32 bytes = 27 + packets + packets * packet_bytes;
    put32(out + 22, ogg_crc(out, bytes));
    return bytes;
}
// Ogg Opus stereo stream of TOC-only CELT 20 ms packets (decoded as PLC silence).
u32 ogg_silence(u8* out, u32 packets) noexcept {
    u8 head[19]{};
    copy_bytes(head, "OpusHead", 8);
    head[8] = 1, head[9] = 2, head[10] = 0x38, head[11] = 0x01; // 312 pre-skip
    put32(head + 12, 48000);
    u8 tags[16]{};
    copy_bytes(tags, "OpusTags", 8);
    u32 at = ogg_page(out, 0, 2, 0, 1, head, 19);
    at += ogg_page(out + at, 1, 0, 0, 1, tags, 16);
    u8 bodies[100];
    for (auto& b : bodies)
        b = 0xfc;
    u32 sequence = 2, done = 0;
    while (done < packets) {
        const u32 count = minimum(packets - done, 100u);
        done += count;
        at += ogg_page(out + at, sequence++, done == packets ? 4 : 0, u64{done} * 960, count, bodies, 1);
    }
    return at;
}

// Requests --------------------------------------------------------------------------
RestClient* rest() noexcept {
    return reinterpret_cast<RestClient*>(&capture);
}
SoundboardRequest base(SoundboardOperation operation) noexcept {
    SoundboardRequest r{};
    r.operation = operation;
    r.guild_id = 333;
    r.channel_id = 444;
    r.sound_id = 222;
    return r;
}
bool submitted(const SoundboardRequest& r, RestMethod method, RestMajorParameter major, u64 major_key,
               Utf8View target, Utf8View body) noexcept {
    const u32 before = capture.submissions;
    if (!account_soundboard_submit(rest(), &r, 91).ok() || capture.submissions != before + 1)
        return false;
    return capture.id == 91 && capture.route.method == method && capture.route.major == major &&
           capture.route.major_key == major_key && (capture.route.route_id >> 24) == (kSoundboardRouteBase >> 24) &&
           capture.json == (body.bytes != 0) && same(capture.target, capture.target_bytes, target) &&
           same(capture.body, capture.body_bytes, body);
}

void test_requests() noexcept {
    using M = RestMethod;
    using P = RestMajorParameter;
    auto send = base(SoundboardOperation::send);
    send.guild_id = 0;
    send.sound_id = 1;
    send.emoji_name = utf8_literal("\xF0\x9F\xA6\x86");
    check(submitted(send, M::post, P::channel, 444, utf8_literal("/api/v9/channels/444/send-soundboard-sound"),
                    utf8_literal("{\"sound_id\":\"1\",\"emoji_id\":null,\"emoji_name\":\"\xF0\x9F\xA6\x86\"}")),
          "Soundboard send: default sounds omit source_guild_id; a Unicode emoji travels by name");
    send = base(SoundboardOperation::send);
    send.emoji_id = 555;
    send.emoji_name = utf8_literal("pog");
    check(submitted(send, M::post, P::channel, 444, utf8_literal("/api/v9/channels/444/send-soundboard-sound"),
                    utf8_literal("{\"sound_id\":\"222\",\"emoji_id\":\"555\",\"emoji_name\":\"pog\","
                                 "\"source_guild_id\":\"333\"}")),
          "Soundboard send: server sounds carry the custom emoji and the source server");
    send.emoji_id = 0;
    send.emoji_name = {};
    check(submitted(send, M::post, P::channel, 444, utf8_literal("/api/v9/channels/444/send-soundboard-sound"),
                    utf8_literal("{\"sound_id\":\"222\",\"emoji_id\":null,\"source_guild_id\":\"333\"}")),
          "Soundboard send: a sound without emoji omits emoji_name");
    check(submitted(base(SoundboardOperation::guild_list), M::get, P::guild, 333,
                    utf8_literal("/api/v9/guilds/333/soundboard-sounds"), {}) &&
              submitted(base(SoundboardOperation::default_list), M::get, P::none, 0,
                        utf8_literal("/api/v9/soundboard-default-sounds"), {}),
          "Soundboard lists: server and default sound GETs");
    auto update = base(SoundboardOperation::update);
    update.name = utf8_literal("duck \"loud\"");
    update.volume = 750;
    update.emoji_name = utf8_literal("\xF0\x9F\xA6\x86");
    check(submitted(update, M::patch, P::guild, 333, utf8_literal("/api/v9/guilds/333/soundboard-sounds/222"),
                    utf8_literal("{\"name\":\"duck \\\"loud\\\"\",\"volume\":0.75,\"emoji_id\":null,"
                                 "\"emoji_name\":\"\xF0\x9F\xA6\x86\"}")),
          "Soundboard update: name, volume, emoji_id and emoji_name in the official order");
    auto remove = base(SoundboardOperation::remove);
    remove.reason = utf8_literal("old sound");
    check(submitted(remove, M::delete_method, P::guild, 333, utf8_literal("/api/v9/guilds/333/soundboard-sounds/222"),
                    {}) &&
              same(capture.reason, capture.reason_bytes, utf8_literal("old%20sound")),
          "Soundboard delete: bodyless DELETE with the audit log reason");
    send = base(SoundboardOperation::send);
    send.reason = utf8_literal("nope");
    const u32 before = capture.submissions;
    check(!account_soundboard_submit(rest(), &send, 91).ok() && !account_soundboard_submit(rest(), nullptr, 91).ok() &&
              capture.submissions == before,
          "Soundboard requests: sends never carry audit reasons; null requests are refused");
    check(account_soundboard_cancel(rest(), 5).ok() && capture.cancellations == 1, "Soundboard cancel: exact request ID");
}

void test_uploads() noexcept {
    u8* file = allocate(600 * 1024);
    if (!file)
        return check(false, "Soundboard uploads: fixture memory");
    // 5.0 s of Ogg Opus: accepted, sent as a data URI on the ordinary path.
    u32 bytes = ogg_silence(file, 250);
    SoundboardFileInfo info{};
    auto create = base(SoundboardOperation::create);
    create.name = utf8_literal("quack");
    create.volume = 1000;
    create.format = SoundboardAudioFormat::ogg_opus;
    create.sound = {file, bytes};
    create.info = &info;
    const u32 before_large = capture.large;
    check(account_soundboard_submit(rest(), &create, 92).ok() && info.ok() &&
              info.format == SoundboardAudioFormat::ogg_opus && info.duration_ms == 4994 && info.channels == 2 &&
              capture.large == before_large,
          "Soundboard upload: a 5 s Ogg Opus file passes the demuxer check");
    constexpr char prefix[] = "{\"name\":\"quack\",\"sound\":\"data:audio/ogg;base64,";
    constexpr char suffix[] = "\",\"volume\":1,\"emoji_id\":null,\"emoji_name\":null}";
    const u32 encoded = base64_encoded_size(bytes);
    bool body = capture.body_bytes == sizeof(prefix) - 1 + encoded + sizeof(suffix) - 1 &&
                same(capture.body, sizeof(prefix) - 1, utf8_literal(prefix)) &&
                same(capture.body + sizeof(prefix) - 1 + encoded, sizeof(suffix) - 1, utf8_literal(suffix)) &&
                same(capture.target, capture.target_bytes, utf8_literal("/api/v9/guilds/333/soundboard-sounds"));
    static u8 decoded[600 * 1024];
    const auto round = base64_decode({reinterpret_cast<const char*>(capture.body + sizeof(prefix) - 1), encoded},
                                     {decoded, sizeof(decoded)});
    body = body && round.ok() && round.bytes == bytes && bytes_equal(decoded, file, bytes);
    check(body, "Soundboard upload: JSON body with the file as a base64 data URI");
    // 5.0 s of MP3 (~83 KB): the data URI exceeds 64 KB and takes the large path.
    bytes = mp3_silence(file, 191);
    create.format = SoundboardAudioFormat::mp3;
    create.sound = {file, bytes};
    check(account_soundboard_submit(rest(), &create, 93).ok() && info.ok() && info.sample_rate == 44100 &&
              info.duration_ms <= kSoundboardMaximumDurationMs && capture.large == before_large + 1 &&
              same(capture.body + 25, 23, utf8_literal("data:audio/mpeg;base64,")),
          "Soundboard upload: an MP3 data URI over the ordinary ceiling uses the large-body path");
    // 5.5 s: too long; nothing reaches the transport.
    const u32 submissions = capture.submissions;
    bytes = mp3_silence(file, 211);
    create.sound = {file, bytes};
    check(!account_soundboard_submit(rest(), &create, 94).ok() && info.error == SoundboardFileError::too_long &&
              info.duration_ms > kSoundboardMaximumDurationMs && capture.submissions == submissions,
          "Soundboard upload: files over 5.2 seconds are refused before sending");
    // Over 512 KiB, a mismatched declared format, and not audio at all.
    bytes = mp3_silence(file, 1300);
    create.sound = {file, bytes};
    check(!account_soundboard_submit(rest(), &create, 95).ok(), "Soundboard upload: files over 512 KB are refused");
    auto probe = base(SoundboardOperation::probe);
    probe.sound = {file, bytes};
    probe.info = &info;
    check(!account_soundboard_probe(&probe) && info.error == SoundboardFileError::too_large,
          "Soundboard probe: size limit");
    bytes = mp3_silence(file, 40);
    create.sound = {file, bytes};
    create.format = SoundboardAudioFormat::ogg_opus;
    check(!account_soundboard_submit(rest(), &create, 96).ok() && capture.submissions == submissions,
          "Soundboard upload: the declared format must match the file");
    for (u32 i = 0; i < 4096; ++i)
        file[i] = static_cast<u8>(i * 7 + 1);
    probe.sound = {file, 4096};
    check(!account_soundboard_probe(&probe) && info.error == SoundboardFileError::format,
          "Soundboard probe: non-audio files are refused");
    probe.sound = {file, mp3_silence(file, 100)};
    check(account_soundboard_probe(&probe) && info.ok() && info.format == SoundboardAudioFormat::mp3,
          "Soundboard probe: a short MP3 passes");
    VirtualFree(file, 0, MEM_RELEASE);
}

void test_decoding() noexcept {
    const char effect_json[] = "{\"user_id\":\"111\",\"sound_volume\":0.5299999713897705,\"sound_id\":\"222\","
                               "\"guild_id\":\"333\",\"emoji\":{\"name\":\"\xF0\x9F\xA6\x86\",\"id\":null},"
                               "\"channel_id\":\"444\",\"animation_type\":1,\"animation_id\":5}";
    GatewayVoiceChannelEffect effect{};
    check(soundboard_decode_effect({effect_json, sizeof(effect_json) - 1}, {scratch, 16 * 1024}, &effect).ok() &&
              effect.user_id == 111 && effect.sound_id == 222 && effect.guild_id == 333 && effect.channel_id == 444 &&
              effect.sound_volume == 530 && effect.has_sound_volume && effect.animation_type == 1 &&
              effect.animation_id == 5 && !effect.emoji_id &&
              utf8_equal({effect.emoji_name, effect.emoji_name_bytes}, utf8_literal("\xF0\x9F\xA6\x86")),
          "VOICE_CHANNEL_EFFECT_SEND: sender, sound, volume, emoji and animation decode");
    const char emoji_only[] = "{\"channel_id\":\"444\",\"user_id\":\"7\",\"emoji\":{\"id\":\"88\",\"name\":\"pog\","
                              "\"animated\":true},\"animation_type\":0,\"animation_id\":2}";
    check(soundboard_decode_effect({emoji_only, sizeof(emoji_only) - 1}, {scratch, 16 * 1024}, &effect).ok() &&
              !effect.sound_id && effect.emoji_id == 88 && effect.emoji_animated && effect.sound_volume == 1000 &&
              !effect.has_sound_volume,
          "VOICE_CHANNEL_EFFECT_SEND: emoji-only effects have no sound");
    const char no_channel[] = "{\"user_id\":\"7\",\"sound_id\":\"1\"}";
    check(!soundboard_decode_effect({no_channel, sizeof(no_channel) - 1}, {scratch, 16 * 1024}, &effect).ok(),
          "VOICE_CHANNEL_EFFECT_SEND: a channel and sender are required");

    static SoundboardSound sounds[8];
    const char defaults[] = "[{\"name\":\"quack\",\"sound_id\":\"1\",\"volume\":1.0,\"emoji_id\":null,"
                            "\"emoji_name\":\"\xF0\x9F\xA6\x86\",\"available\":true},"
                            "{\"name\":\"airhorn\",\"sound_id\":2,\"volume\":0.5,\"user_id\":\"0\"}]";
    const auto listed = soundboard_decode_sound_list({defaults, sizeof(defaults) - 1}, 0, {scratch, 16 * 1024},
                                                     {sounds, 8});
    check(listed.ok() && listed.count == 2 && sounds[0].sound_id == 1 && sounds[0].volume == 1000 &&
              !sounds[0].guild_id && sounds[1].sound_id == 2 && sounds[1].volume == 500 && sounds[1].available &&
              utf8_equal({sounds[1].name, sounds[1].name_bytes}, utf8_literal("airhorn")),
          "Default sounds: a root array with string or numeric sound IDs");
    const char guild[] = "{\"items\":[{\"name\":\"bonk\",\"sound_id\":\"900\",\"volume\":0.25,\"emoji_id\":\"55\","
                         "\"emoji_name\":null,\"guild_id\":\"333\",\"available\":false,"
                         "\"user\":{\"id\":\"9\"}}]}";
    const auto items = soundboard_decode_sound_list({guild, sizeof(guild) - 1}, 333, {scratch, 16 * 1024}, {sounds, 8});
    check(items.ok() && items.count == 1 && sounds[0].guild_id == 333 && sounds[0].emoji_id == 55 &&
              !sounds[0].emoji_name_bytes && !sounds[0].available && sounds[0].volume == 250,
          "Server sounds: the items envelope");
    const char gateway[] = "{\"soundboard_sounds\":[{\"name\":\"a\\u00e9\",\"sound_id\":\"5\"}],\"guild_id\":\"77\"}";
    u64 document_guild = 0;
    const auto updated = soundboard_decode_sound_list({gateway, sizeof(gateway) - 1}, 0, {scratch, 16 * 1024},
                                                      {sounds, 8}, &document_guild);
    check(updated.ok() && updated.count == 1 && document_guild == 77 && sounds[0].guild_id == 77 &&
              utf8_equal({sounds[0].name, sounds[0].name_bytes}, utf8_literal("a\xC3\xA9")),
          "SOUNDBOARD_SOUNDS: rows inherit the document's server");
    const char created[] = "{\"sound_id\":\"6\",\"name\":\"hey\",\"volume\":1e0,\"guild_id\":\"77\"}";
    SoundboardSound sound{};
    u64 sound_id = 0, guild_id = 0;
    const char deleted[] = "{\"sound_id\":\"6\",\"guild_id\":\"77\"}";
    check(soundboard_decode_sound({created, sizeof(created) - 1}, 0, {scratch, 16 * 1024}, &sound).ok() &&
              sound.sound_id == 6 && sound.volume == 1000 && sound.guild_id == 77 &&
              soundboard_decode_delete({deleted, sizeof(deleted) - 1}, {scratch, 16 * 1024}, &sound_id, &guild_id)
                  .ok() &&
              sound_id == 6 && guild_id == 77,
          "GUILD_SOUNDBOARD_SOUND_CREATE/DELETE decode");
    u32 volume = 0;
    char text[8];
    check(soundboard_parse_volume(utf8_literal("0.0005"), &volume) && volume == 1 &&
              soundboard_parse_volume(utf8_literal("2"), &volume) && volume == 1000 &&
              soundboard_parse_volume(utf8_literal("-0.0"), &volume) && volume == 0 &&
              !soundboard_parse_volume(utf8_literal("-0.5"), &volume) &&
              !soundboard_parse_volume(utf8_literal("0."), &volume) &&
              soundboard_parse_volume(utf8_literal("7.5E-1"), &volume) && volume == 750 &&
              soundboard_format_volume(5, {text, 8}) == 5 && same(reinterpret_cast<u8*>(text), 5, utf8_literal("0.005")) &&
              soundboard_format_volume(1000, {text, 8}) == 1 && soundboard_format_volume(1001, {text, 8}) == 0,
          "Soundboard volume: JSON numbers round to per-mille and back");
}

void test_gating() noexcept {
    SoundboardSendContext c{};
    c.in_call = true;
    c.channel_guild_id = 333;
    c.sound_guild_id = 333;
    c.available = true;
    c.permissions = kPermissionSpeak | kPermissionUseSoundboard;
    check(soundboard_send_check(c) == SoundboardBlock::none, "Soundboard gating: own server sound with permissions");
    auto t = c;
    t.permissions = kPermissionSpeak;
    check(soundboard_send_check(t) == SoundboardBlock::permission, "Soundboard gating: Use Soundboard is required");
    t = c;
    t.permissions = kPermissionUseSoundboard;
    check(soundboard_send_check(t) == SoundboardBlock::permission, "Soundboard gating: Speak is required");
    t = c;
    t.permissions = kPermissionAdministrator;
    check(soundboard_send_check(t) == SoundboardBlock::none, "Soundboard gating: Administrator implies both");
    t = c;
    t.self_mute = true;
    check(soundboard_send_check(t) == SoundboardBlock::muted, "Soundboard gating: muted members can't play sounds");
    t = c;
    t.suppress = true;
    check(soundboard_send_check(t) == SoundboardBlock::muted, "Soundboard gating: stage audience can't play sounds");
    t = c;
    t.sound_guild_id = 999;
    check(soundboard_send_check(t) == SoundboardBlock::external_premium, "Soundboard gating: other servers need Nitro");
    t.premium = true;
    check(soundboard_send_check(t) == SoundboardBlock::external_permission,
          "Soundboard gating: other servers need Use External Sounds");
    t.permissions |= kPermissionUseExternalSounds;
    check(soundboard_send_check(t) == SoundboardBlock::none, "Soundboard gating: external sound with Nitro and permission");
    t = c;
    t.sound_guild_id = 0;
    check(soundboard_send_check(t) == SoundboardBlock::none, "Soundboard gating: default sounds play everywhere");
    t = c;
    t.available = false;
    check(soundboard_send_check(t) == SoundboardBlock::unavailable, "Soundboard gating: unavailable sounds");
    t = c;
    t.now_ms = 10;
    t.cooldown_until_ms = 20;
    check(soundboard_send_check(t) == SoundboardBlock::cooldown, "Soundboard gating: rate limit cooldown");
    t = c;
    t.in_call = false;
    check(soundboard_send_check(t) == SoundboardBlock::not_in_call, "Soundboard gating: only in a server call");
}

void test_cache_and_mixer() noexcept {
    i16 pcm[2 * 480];
    for (u32 i = 0; i < 960; ++i)
        pcm[i] = 10000;
    SoundboardCache cache{};
    SoundboardClip* a = soundboard_clip_from_pcm(1, {pcm, 960});
    const u32 clip_bytes = a ? a->allocation_bytes : 0;
    soundboard_cache_initialize(&cache, clip_bytes * 2);
    SoundboardClip* b = soundboard_clip_from_pcm(2, {pcm, 960});
    SoundboardClip* c = soundboard_clip_from_pcm(3, {pcm, 960});
    check(a && b && c && soundboard_cache_insert(&cache, a) && soundboard_cache_insert(&cache, b) &&
              soundboard_cache_find(&cache, 1) == a,
          "Soundboard cache: two clips fit the budget");
    SoundboardMixer mixer{};
    check(soundboard_mixer_play(&mixer, a, 500, 7) && a->references == 2, "Soundboard mixer: a playing voice holds a reference");
    // 2 is least recently used: it goes; 1 keeps its cache and voice references.
    check(soundboard_cache_insert(&cache, c) && !soundboard_cache_find(&cache, 2) && soundboard_cache_find(&cache, 1) &&
              soundboard_cache_find(&cache, 3) && cache.count == 2 && cache.bytes == u64{clip_bytes} * 2,
          "Soundboard cache: least recently used clips are evicted within the byte budget");
    soundboard_cache_remove(&cache, 1);
    check(a->references == 1 && a->pcm[0] == 10000, "Soundboard cache: an evicted clip lives while it plays");
    // Gain: 0.5 sound volume x 50 % soundboard volume x 100 % output.
    soundboard_mixer_set_volume(&mixer, 50);
    i32 mix[2 * 256]{};
    u32 active = soundboard_mixer_render(&mixer, {mix, 512}, 256, 100, true, 1'000);
    check(active == 1 && mix[0] >= 2499 && mix[0] <= 2500 && mix[511] == mix[0],
          "Soundboard mixer: sound volume x soundboard volume x output volume");
    // A preview render yields to the call output that rendered just now.
    i32 other[2 * 256]{};
    check(soundboard_mixer_render(&mixer, {other, 512}, 256, 100, false, 1'050) == 1 && !other[0] &&
              soundboard_mixer_call_rendering(&mixer, 1'050) && !soundboard_mixer_call_rendering(&mixer, 900'000),
          "Soundboard mixer: the preview output stays silent while a call renders");
    clear_bytes(mix, sizeof(mix));
    active = soundboard_mixer_render(&mixer, {mix, 512}, 256, 200, true, 2'000);
    check(!active && mix[0] >= 4998 && mix[0] <= 5000 && !mix[2 * 224] && soundboard_mixer_active(&mixer) == 0,
          "Soundboard mixer: the voice ends with its clip and releases it");
    soundboard_mixer_play(&mixer, c, 1000, 7);
    soundboard_mixer_play(&mixer, c, 1000, 8);
    soundboard_mixer_stop(&mixer, 7);
    check(soundboard_mixer_active(&mixer) == 1 && c->references == 2, "Soundboard mixer: stop by sender");
    soundboard_mixer_stop(&mixer, 0);
    for (u32 i = 0; i < kSoundboardVoices + 2; ++i)
        soundboard_mixer_play(&mixer, c, 1000, i + 1);
    check(soundboard_mixer_active(&mixer) == kSoundboardVoices && c->references == 1 + kSoundboardVoices,
          "Soundboard mixer: a full mixer replaces a voice instead of growing");
    soundboard_mixer_stop(&mixer, 0);
    soundboard_cache_clear(&cache);
    check(cache.count == 0 && cache.bytes == 0, "Soundboard cache: clear releases every clip");
}

// Engine stubs: CDN, Gateway attach, audio output, clock ------------------------------
struct Cdn {
    u32 starts, submits, stops;
    u64 request_id;
    u8 target[64];
    u32 target_bytes;
    const u8* body;
    u32 body_bytes;
    u16 status;
    bool ready;
} cdn;
bool call_live = true;
u64 clock_us = 1;

void test_engine() noexcept {
    static AccountSoundboard engine;
    account_soundboard_initialize(&engine);
    auto* gateway = reinterpret_cast<GatewayClient*>(&cdn);
    SoundboardAudioCommand command{};
    command.operation = SoundboardAudioOperation::configure;
    command.volume_percent = 40;
    command.muted = true;
    SoundboardAudioStatus status{};
    check(account_soundboard_command(&engine, gateway, &command, &status, 100) && engine.muted &&
              engine.mixer.volume_delta == -60,
          "Soundboard engine: volume and mute settings");
    command = {};
    command.operation = SoundboardAudioOperation::play;
    command.sound_id = 1;
    command.user_id = 7;
    command.sound_volume = 1000;
    check(!account_soundboard_command(&engine, gateway, &command, &status, 100) && !status.pending,
          "Soundboard engine: received sounds stay silent while muted");
    SoundboardAudioCommand unmute{};
    unmute.operation = SoundboardAudioOperation::configure;
    unmute.volume_percent = 100;
    account_soundboard_command(&engine, gateway, &unmute, nullptr, 100);
    call_live = false;
    check(!account_soundboard_command(&engine, gateway, &command, &status, 100),
          "Soundboard engine: received sounds play only during calls");
    call_live = true;
    check(account_soundboard_command(&engine, gateway, &command, &status, 100) && status.pending == 1 &&
              status.fetching_sound_id == 1 && cdn.starts == 1 && cdn.submits == 1 &&
              same(cdn.target, cdn.target_bytes, utf8_literal("/soundboard-sounds/1")),
          "Soundboard engine: an uncached sound is fetched from the CDN");
    static u8 file[48 * 1024];
    cdn.body = file;
    cdn.body_bytes = mp3_silence(file, 40);
    cdn.status = 200;
    cdn.ready = true;
    account_soundboard_pump(&engine, gateway, 200);
    check(!cdn.ready && engine.cache.count == 1 && soundboard_mixer_active(&engine.mixer) == 1 && !engine.pending_count &&
              engine.cache.entries[0]->frames >= 40u * 1152u * 48000u / 44100u,
          "Soundboard engine: the fetched MP3 is decoded at 48 kHz, cached and mixed");
    // A second play of the same sound needs no fetch.
    check(account_soundboard_command(&engine, gateway, &command, &status, 300) && cdn.submits == 1 &&
              status.active_voices == 2 && status.cached_clips == 1,
          "Soundboard engine: cached sounds play at once");
    // A failed fetch is remembered for a while instead of retried every play.
    command.sound_id = 2;
    account_soundboard_command(&engine, gateway, &command, &status, 400);
    cdn.status = 404;
    cdn.ready = true;
    account_soundboard_pump(&engine, gateway, 500);
    const u32 submits = cdn.submits;
    account_soundboard_command(&engine, gateway, &command, &status, 600);
    check(status.failed_sound_id == 2 && status.last_error == SoundboardAudioError::fetch && cdn.submits == submits &&
              !status.pending,
          "Soundboard engine: failed sounds back off");
    check(account_soundboard_stop(&engine, gateway, 100) && !engine.cache.count && !soundboard_mixer_active(&engine.mixer) &&
              cdn.stops == 1,
          "Soundboard engine: stop releases the CDN client, voices and clips");
}
} // namespace soundboard_test

// Transport stubs.
RestClientSubmitResult capture_request(const RestClientRequest& request) noexcept {
    auto& c = soundboard_test::capture;
    ++c.submissions;
    c.route = request.route;
    c.id = request.request_id;
    c.timeout_ms = request.timeout_ms;
    c.json = request.json_body;
    if (request.target.count > sizeof(c.target) || request.audit_reason.count > sizeof(c.reason))
        return {RestClientSubmitError::invalid_input};
    c.target_bytes = request.target.count;
    copy_bytes(c.target, request.target.data, request.target.count);
    c.reason_bytes = request.audit_reason.count;
    copy_bytes(c.reason, request.audit_reason.data, request.audit_reason.count);
    if (request.body.count > c.body_capacity) {
        if (c.body)
            VirtualFree(c.body, 0, MEM_RELEASE);
        c.body = soundboard_test::allocate(request.body.count);
        c.body_capacity = c.body ? request.body.count : 0;
    }
    c.body_bytes = request.body.count;
    if (request.body.count && c.body)
        copy_bytes(c.body, request.body.data, request.body.count);
    return {};
}
RestClientSubmitResult rest_client_submit(RestClient*, const RestClientRequest& request) noexcept {
    if (request.body.count > kRestClientMaximumRequestBodyBytes)
        return {RestClientSubmitError::body_capacity};
    return capture_request(request);
}
RestClientSubmitResult rest_client_submit_large(RestClient*,
                                                const RestClientRequest& request) noexcept {
    ++soundboard_test::capture.large;
    return capture_request(request);
}
bool rest_client_cancel(RestClient*, u64 request_id) noexcept {
    ++soundboard_test::capture.cancellations;
    return request_id == 5;
}

// CDN stubs: one staged response at a time.
CdnClientResult cdn_client_start(CdnClient* client, u32) noexcept {
    ++soundboard_test::cdn.starts;
    client->worker.thread = reinterpret_cast<HANDLE>(1);
    return {};
}
CdnClientSubmitResult cdn_client_submit(CdnClient*, const CdnClientRequest& request) noexcept {
    auto& c = soundboard_test::cdn;
    ++c.submits;
    c.request_id = request.request_id;
    c.target_bytes = minimum<u32>(request.target.count, sizeof(c.target));
    copy_bytes(c.target, request.target.data, c.target_bytes);
    return request.image_type == CdnImageType::soundboard_sound && request.host == CdnHost::cdn
               ? CdnClientSubmitResult{}
               : CdnClientSubmitResult{CdnClientSubmitError::invalid_input};
}
CdnClientResult cdn_client_status(const CdnClient*) noexcept {
    CdnClientResult result{};
    result.phase = soundboard_test::cdn.ready ? CdnClientPhase::response_ready : CdnClientPhase::idle;
    return result;
}
CdnClientTakeResult cdn_client_consume_response(CdnClient*, CdnClientResponseConsumer consumer,
                                                void* context) noexcept {
    auto& c = soundboard_test::cdn;
    CdnClientResponse response{};
    response.request_id = c.request_id;
    response.status_code = c.status;
    response.body_bytes = c.status == 200 ? c.body_bytes : 0;
    c.ready = false;
    consumer(context, response, {c.body, response.body_bytes});
    return {};
}
CdnResponseResult cdn_client_response_policy(const CdnClientResponse& response) noexcept {
    CdnResponseResult result{};
    result.status_code = response.status_code;
    result.disposition =
        response.status_code == 200 ? CdnResponseDisposition::accept_body : CdnResponseDisposition::none;
    return result;
}
CdnClientResult cdn_client_stop(CdnClient* client, u32) noexcept {
    ++soundboard_test::cdn.stops;
    *client = {};
    return {};
}
HANDLE cdn_client_notification_handle(const CdnClient*) noexcept {
    return nullptr;
}
bool gateway_client_attach_soundboard(GatewayClient*, SoundboardMixer*) noexcept {
    return soundboard_test::call_live;
}
bool voice_audio_output_start_device(VoiceAudioOutput*, const wchar_t*, VoiceAudioRender, void*) noexcept {
    return false;
}
void voice_audio_output_stop(VoiceAudioOutput* output) noexcept {
    output->runtime = nullptr;
}
bool voice_audio_output_running(const VoiceAudioOutput& output) noexcept {
    return output.runtime != nullptr;
}
u64 voice_playback_now_us() noexcept {
    return soundboard_test::clock_us += 1000;
}
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_soundboard_entry() noexcept {
    using namespace maicord::soundboard_test;
    test_requests();
    test_uploads();
    test_decoding();
    test_gating();
    test_cache_and_mixer();
    test_engine();
    constexpr char pass[] = "PASS soundboard\n";
    constexpr char fail[] = "FAIL soundboard\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failures ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failures ? 1 : 0);
}
