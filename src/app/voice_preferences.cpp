#include "maicord/app/voice_preferences.hpp"
#include "maicord/app/account_profile.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"

#include <Windows.h>

namespace maicord {
namespace {
constexpr u8 kMagic[8]{'M', 'A', 'I', 'V', 'O', 'I', 'C', 'E'};
constexpr u32 kHeaderBytes = 40, kChecksumOffset = 32, kUserBytes = 11;
constexpr u8 kAutomatic = 1, kProcessing = 2, kSelfMute = 4, kSelfDeaf = 8, kSoundboardMuted = 16;
constexpr u8 kKnownFlagsV1 = 15, kKnownFlags = 31;

void store16(u8* output, u16 value) noexcept {
    output[0] = static_cast<u8>(value);
    output[1] = static_cast<u8>(value >> 8);
}

u32 id_length(const wchar_t* id) noexcept {
    u32 length = 0;
    while (length + 1 < kVoiceDeviceIdCharacters && id[length])
        ++length;
    return length;
}
u64 checksum(ConstSlice<u8> record) noexcept {
    u64 hash = hash64_bytes(kHash64Seed, record.data, kChecksumOffset);
    return hash64_bytes(hash, record.data + kHeaderBytes, record.count - kHeaderBytes);
}
bool user_path(wchar_t* path, bool create) noexcept {
    return data_path(path, kAccountProfileMaximumPathCharacters, L"voice.bin", create).ok();
}
} // namespace

VoicePreferences voice_preferences_defaults() noexcept {
    VoicePreferences preferences{};
    preferences.audio = voice_audio_settings_defaults();
    preferences.soundboard_volume = 100;
    return preferences;
}

bool voice_preferences_valid(const VoicePreferences& p) noexcept {
    const auto& a = p.audio;
    if (a.input_gain_percent > 200 || a.output_gain_percent > 200 ||
        a.push_to_talk_release_ms > kVoiceMaximumPushToTalkReleaseMs ||
        static_cast<u8>(a.mode) > static_cast<u8>(VoiceInputMode::push_to_talk) || a.sensitivity > kVoiceLevelMaximum ||
        p.user_count > kVoicePreferenceUsers || p.soundboard_volume > 100 ||
        a.input_device[kVoiceDeviceIdCharacters - 1] ||
        a.output_device[kVoiceDeviceIdCharacters - 1])
        return false;
    for (u32 i = 0; i < p.user_count; ++i)
        if (!p.users[i].user_id || p.users[i].gain_percent > 200)
            return false;
    return true;
}

VoiceUserVolume voice_preferences_user(const VoicePreferences& p, u64 user_id) noexcept {
    for (u32 i = 0; i < p.user_count; ++i)
        if (p.users[i].user_id == user_id)
            return p.users[i];
    return {user_id, 100, false};
}

bool voice_preferences_set_user(VoicePreferences* p, u64 user_id, u32 gain_percent, bool muted) noexcept {
    if (!p || !user_id || gain_percent > 200)
        return false;
    u32 index = 0;
    while (index < p->user_count && p->users[index].user_id != user_id)
        ++index;
    if (index < p->user_count) {
        // Keep the table ordered by last change: remove, then append.
        for (u32 i = index + 1; i < p->user_count; ++i)
            p->users[i - 1] = p->users[i];
        --p->user_count;
    }
    if (gain_percent == 100 && !muted)
        return true;
    if (p->user_count == kVoicePreferenceUsers) {
        for (u32 i = 1; i < p->user_count; ++i)
            p->users[i - 1] = p->users[i];
        --p->user_count;
    }
    p->users[p->user_count++] = {user_id, static_cast<u16>(gain_percent), muted};
    return true;
}

u32 voice_preferences_encode(const VoicePreferences& p, Slice<u8> output) noexcept {
    if (!output.data || !voice_preferences_valid(p))
        return 0;
    const auto& a = p.audio;
    const u32 input = id_length(a.input_device), speaker = id_length(a.output_device);
    const u32 bytes = kHeaderBytes + (input + speaker) * 2 + 2 + p.user_count * kUserBytes;
    if (output.count < bytes)
        return 0;
    clear_bytes(output.data, bytes);
    u8* r = output.data;
    copy_bytes(r, kMagic, 8);
    byte_store_le32(r + 8, kVoicePreferencesVersion);
    byte_store_le32(r + 12, bytes);
    store16(r + 16, a.input_gain_percent);
    store16(r + 18, a.output_gain_percent);
    store16(r + 20, a.push_to_talk_release_ms);
    r[22] = static_cast<u8>(a.mode);
    r[23] = a.sensitivity;
    r[24] = a.push_to_talk_key;
    r[25] = static_cast<u8>((a.auto_sensitivity ? kAutomatic : 0) | (a.voice_processing ? kProcessing : 0) |
                            (p.self_mute ? kSelfMute : 0) | (p.self_deaf ? kSelfDeaf : 0) |
                            (p.soundboard_muted ? kSoundboardMuted : 0));
    r[26] = p.soundboard_volume;
    store16(r + 28, static_cast<u16>(input));
    store16(r + 30, static_cast<u16>(speaker));
    u32 at = kHeaderBytes;
    for (u32 i = 0; i < input; ++i, at += 2)
        store16(r + at, static_cast<u16>(a.input_device[i]));
    for (u32 i = 0; i < speaker; ++i, at += 2)
        store16(r + at, static_cast<u16>(a.output_device[i]));
    store16(r + at, static_cast<u16>(p.user_count));
    at += 2;
    for (u32 i = 0; i < p.user_count; ++i, at += kUserBytes) {
        byte_store_le64(r + at, p.users[i].user_id);
        store16(r + at + 8, p.users[i].gain_percent);
        r[at + 10] = p.users[i].muted ? 1 : 0;
    }
    byte_store_le64(r + kChecksumOffset, checksum({r, bytes}));
    return bytes;
}

bool voice_preferences_decode(ConstSlice<u8> record, VoicePreferences* preferences) noexcept {
    const u8* r = record.data;
    if (!preferences || !r || record.count < kHeaderBytes + 2 || !bytes_equal(r, kMagic, 8))
        return false;
    const u32 version = byte_load_le32(r + 8);
    if ((version != 1 && version != kVoicePreferencesVersion) || byte_load_le32(r + 12) != record.count ||
        byte_load_le64(r + kChecksumOffset) != checksum(record) ||
        (r[25] & ~(version == 1 ? kKnownFlagsV1 : kKnownFlags)) || (version == 1 && r[26]) || r[27] ||
        r[26] > 100)
        return false;
    const u32 input = byte_load_le16(r + 28), speaker = byte_load_le16(r + 30);
    if (input >= kVoiceDeviceIdCharacters || speaker >= kVoiceDeviceIdCharacters ||
        kHeaderBytes + (input + speaker) * 2 + 2 > record.count)
        return false;
    VoicePreferences decoded = voice_preferences_defaults();
    auto& a = decoded.audio;
    a.input_gain_percent = byte_load_le16(r + 16);
    a.output_gain_percent = byte_load_le16(r + 18);
    a.push_to_talk_release_ms = byte_load_le16(r + 20);
    a.mode = static_cast<VoiceInputMode>(r[22]);
    a.sensitivity = r[23];
    a.push_to_talk_key = r[24];
    a.auto_sensitivity = (r[25] & kAutomatic) != 0;
    a.voice_processing = (r[25] & kProcessing) != 0;
    decoded.self_mute = (r[25] & kSelfMute) != 0;
    decoded.self_deaf = (r[25] & kSelfDeaf) != 0;
    decoded.soundboard_muted = (r[25] & kSoundboardMuted) != 0;
    decoded.soundboard_volume = version == 1 ? 100 : r[26];
    u32 at = kHeaderBytes;
    for (u32 i = 0; i < input; ++i, at += 2)
        a.input_device[i] = static_cast<wchar_t>(byte_load_le16(r + at));
    for (u32 i = 0; i < speaker; ++i, at += 2)
        a.output_device[i] = static_cast<wchar_t>(byte_load_le16(r + at));
    decoded.user_count = byte_load_le16(r + at);
    at += 2;
    if (decoded.user_count > kVoicePreferenceUsers || at + decoded.user_count * kUserBytes != record.count)
        return false;
    for (u32 i = 0; i < decoded.user_count; ++i, at += kUserBytes) {
        if (r[at + 10] > 1)
            return false;
        decoded.users[i] = {byte_load_le64(r + at), byte_load_le16(r + at + 8), r[at + 10] != 0};
    }
    if (!voice_preferences_valid(decoded))
        return false;
    *preferences = decoded;
    return true;
}

bool voice_preferences_load_file(const wchar_t* path, VoicePreferences* preferences) noexcept {
    if (!path || !preferences)
        return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    static u8 record[kVoicePreferencesMaximumBytes];
    LARGE_INTEGER size{};
    u32 native = 0;
    const bool read = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= sizeof(record) &&
                      file_persistence_read_complete(file, {record, static_cast<u32>(size.QuadPart)}, &native);
    CloseHandle(file);
    return read && voice_preferences_decode({record, static_cast<u32>(size.QuadPart)}, preferences);
}

bool voice_preferences_save_file(const wchar_t* path, const VoicePreferences& preferences) noexcept {
    static u8 record[kVoicePreferencesMaximumBytes];
    const u32 bytes = voice_preferences_encode(preferences, {record, sizeof(record)});
    const u32 length = path ? wide_length(path, kAccountProfileMaximumPathCharacters) : 0;
    return bytes && length && file_persistence_write_atomic(path, length, {record, bytes}).ok();
}

bool voice_preferences_load(VoicePreferences* preferences) noexcept {
    wchar_t path[kAccountProfileMaximumPathCharacters]{};
    return user_path(path, false) && voice_preferences_load_file(path, preferences);
}

bool voice_preferences_save(const VoicePreferences& preferences) noexcept {
    wchar_t path[kAccountProfileMaximumPathCharacters]{};
    return user_path(path, true) && voice_preferences_save_file(path, preferences);
}
} // namespace maicord
