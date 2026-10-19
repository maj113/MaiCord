#include "application_soundboard.hpp"
#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_message_interaction.hpp"
#include "application_state.hpp"
#include "application_voice.hpp"
#include "maicord/app/account_soundboard.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/protocol/guild_assets.hpp"
#include "maicord/ui/call_scene.hpp"
#include "maicord/ui/popup_scene.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace {
// Nitro members can play every server's sounds; storage commits as lists load.
constexpr u32 kSoundCapacity = 8192;
constexpr u32 kListCapacity = 256;
constexpr u32 kLoadCapacity = 96;
constexpr u32 kQueueCapacity = 8;
constexpr u32 kEffectCapacity = 16;
// Other servers' lists load only for Nitro members, a bounded number per session.
constexpr u32 kExternalGuildLoads = 24;
constexpr u64 kEffectMs = 3'000;
constexpr u64 kBusyRetryMs = 250;
constexpr u64 kLoadRetryMs = 30'000;
constexpr u64 kNoticeMs = 8'000;
constexpr u64 kRateLimitMs = 2'000;
constexpr u32 kQueryBytes = 48;
constexpr u32 kLabelBytes = 4 + kSoundboardEmojiNameBytes + kSoundboardNameBytes;
constexpr u32 kPickerItems = kPopupItemCapacity;
constexpr u32 kActionBase = 0x5B00'0000;
constexpr u32 kScratchUnits = 128 * 1024;
constexpr float kPickerWidth = 300;
// API error codes on soundboard writes.
constexpr u64 kMaximumSounds = 30'045;

enum class LoadState : u8 { none, wanted, in_flight, loaded, failed };
struct GuildLoad {
    u64 guild_id, retry_at_ms;
    LoadState state;
};
struct QueuedRequest {
    SoundboardRequest request;
    char name[kSoundboardNameBytes];
    char emoji[kSoundboardEmojiNameBytes];
    u8* file;
    u64 account_scope;
};
struct Effect {
    u64 user_id, channel_id, until_ms;
    char text[kUiCallEffectBytes];
    u32 bytes;
};
struct Picker {
    u64 channel_id, guild_id;
    UiRect anchor;
    char query[kQueryBytes];
    u32 query_bytes;
    u64 sounds[kPickerItems];
    PopupItem items[kPickerItems];
    char labels[kPickerItems][kLabelBytes];
    char footer[160];
    u32 footer_bytes;
    u64 error_until_ms;
};
struct SoundboardState {
    SoundboardSound* sounds;
    u32 sound_count;
    GuildLoad loads[kLoadCapacity];
    u32 load_count;
    LoadState defaults;
    u64 defaults_retry_ms;
    QueuedRequest queue[kQueueCapacity];
    u32 head, count;
    u64 in_flight_request, retry_at_ms;
    SoundboardOperation in_flight;
    u64 in_flight_guild, in_flight_sound;
    Effect effects[kEffectCapacity];
    u64 cooldown_until_ms;
    u64 notice_guild, notice_until_ms;
    char notice[128];
    u32 notice_bytes;
    Picker picker;
    u32 revision;
};
SoundboardState board{};
StableStorage g_sound_storage;
// JSON token scratch and decoded-list staging (~570 KB), committed on the first
// soundboard payload rather than with the image: writable image pages are
// charged to every session, and most sessions never receive one.
struct DecodeScratch {
    SoundboardSound list[kListCapacity];
    u32 tokens[kScratchUnits];
};
DecodeScratch* g_decode;
DecodeScratch* decode_scratch() noexcept {
    if (!g_decode)
        g_decode = static_cast<DecodeScratch*>(
            VirtualAlloc(nullptr, sizeof(DecodeScratch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    return g_decode;
}
// Empty when the commit failed; every decoder then reports failure.
Slice<u32> scratch() noexcept {
    auto* d = decode_scratch();
    return d ? Slice<u32>{d->tokens, kScratchUnits} : Slice<u32>{};
}
Slice<SoundboardSound> list_buffer() noexcept {
    auto* d = decode_scratch();
    return d ? Slice<SoundboardSound>{d->list, kListCapacity} : Slice<SoundboardSound>{};
}
ConstSlice<SoundboardSound> decoded_list(u32 count) noexcept {
    return g_decode ? ConstSlice<SoundboardSound>{g_decode->list, count} : ConstSlice<SoundboardSound>{};
}

u64 now_ms() noexcept {
    return clock_monotonic_milliseconds();
}
void repaint() noexcept {
    ++board.revision;
    application_repaint();
}
const GatewayClientVoiceSnapshot* session() noexcept {
    return g_app.account_ui ? &g_app.account_ui->voice_session : nullptr;
}
bool in_call(const GatewayClientVoiceSnapshot& s) noexcept {
    return s.channel_id && s.phase != VoiceSetupPhase::idle;
}
const ChannelRecord* voice_channel(u64 channel_id) noexcept {
    if (!g_app.account_ui || !channel_id)
        return nullptr;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id);
    return channel && channel->guild_id ? channel : nullptr;
}
bool premium() noexcept {
    return g_app.account_ui && g_app.account_ui->chat_state.self_premium_type != 0;
}
void set_notice(u64 guild_id, Utf8View text) noexcept {
    board.notice_bytes = utf8_copy_truncated(board.notice, sizeof(board.notice), text);
    board.notice_guild = guild_id;
    board.notice_until_ms = now_ms() + kNoticeMs;
    repaint();
}

// Catalog -----------------------------------------------------------------------
SoundboardSound* find_sound(u64 sound_id) noexcept {
    for (u32 i = 0; i < board.sound_count; ++i)
        if (board.sounds[i].sound_id == sound_id)
            return &board.sounds[i];
    return nullptr;
}
void remove_sound(u64 sound_id) noexcept {
    for (u32 i = 0; i < board.sound_count; ++i)
        if (board.sounds[i].sound_id == sound_id) {
            for (u32 k = i + 1; k < board.sound_count; ++k)
                board.sounds[k - 1] = board.sounds[k];
            --board.sound_count;
            return;
        }
}
void upsert_sound(const SoundboardSound& sound) noexcept {
    if (auto* existing = find_sound(sound.sound_id)) {
        *existing = sound;
        return;
    }
    if (board.sound_count == g_sound_storage.capacity &&
        ((!g_sound_storage.arena.base &&
          !stable_storage_initialize(&g_sound_storage, kSoundCapacity, 0, sizeof(SoundboardSound),
                                     alignof(SoundboardSound))) ||
         !stable_storage_grow(&g_sound_storage, board.sound_count + 1)))
        return;
    board.sounds = stable_storage_data<SoundboardSound>(g_sound_storage);
    board.sounds[board.sound_count++] = sound;
}
void clear_guild(u64 guild_id) noexcept {
    u32 kept = 0;
    for (u32 i = 0; i < board.sound_count; ++i)
        if (board.sounds[i].guild_id != guild_id)
            board.sounds[kept++] = board.sounds[i];
    board.sound_count = kept;
}
void replace_guild(u64 guild_id, ConstSlice<SoundboardSound> sounds) noexcept {
    clear_guild(guild_id);
    for (u32 i = 0; i < sounds.count; ++i) {
        auto sound = sounds[i];
        sound.guild_id = guild_id;
        upsert_sound(sound);
    }
}
GuildLoad* load_of(u64 guild_id, bool create) noexcept {
    for (u32 i = 0; i < board.load_count; ++i)
        if (board.loads[i].guild_id == guild_id)
            return &board.loads[i];
    if (!create || board.load_count == kLoadCapacity)
        return nullptr;
    auto& load = board.loads[board.load_count++];
    load = {guild_id, 0, LoadState::none};
    return &load;
}
void want_guild(u64 guild_id) noexcept {
    auto* load = load_of(guild_id, true);
    if (load && (load->state == LoadState::none ||
                 (load->state == LoadState::failed && now_ms() >= load->retry_at_ms)))
        load->state = LoadState::wanted;
}
void want_defaults() noexcept {
    if (board.defaults == LoadState::none || (board.defaults == LoadState::failed && now_ms() >= board.defaults_retry_ms))
        board.defaults = LoadState::wanted;
}

// Effects -----------------------------------------------------------------------
u32 effect_text(char* output, u32 capacity, Utf8View emoji, bool custom_emoji, Utf8View name) noexcept {
    u32 bytes = 0;
    if (emoji.bytes && !custom_emoji) {
        bytes = utf8_copy_truncated(output, capacity, emoji);
        if (name.bytes && bytes + 1 < capacity)
            output[bytes++] = ' ';
    }
    bytes += utf8_copy_truncated(output + bytes, capacity - bytes, name);
    return bytes;
}
void add_effect(u64 user_id, u64 channel_id, Utf8View emoji, bool custom_emoji, Utf8View name) noexcept {
    u32 slot = 0;
    for (u32 i = 0; i < kEffectCapacity; ++i) {
        if (board.effects[i].user_id == user_id) {
            slot = i;
            break;
        }
        if (board.effects[i].until_ms < board.effects[slot].until_ms)
            slot = i;
    }
    auto& effect = board.effects[slot];
    effect.bytes = effect_text(effect.text, sizeof(effect.text), emoji, custom_emoji, name);
    if (!effect.bytes) {
        effect = {};
        return;
    }
    effect.user_id = user_id;
    effect.channel_id = channel_id;
    effect.until_ms = now_ms() + kEffectMs;
    repaint();
}

// Account module playback -------------------------------------------------------
bool audio(SoundboardAudioOperation operation, u64 sound_id, u64 user_id, u16 volume) noexcept {
    auto& h = g_app.account;
    if (!h.instance || !h.api.soundboard_audio)
        return false;
    SoundboardAudioCommand command{};
    command.operation = operation;
    command.sound_id = sound_id;
    command.user_id = user_id;
    command.sound_volume = volume;
    return h.api.soundboard_audio(h.instance, &command, nullptr);
}

// Queue -------------------------------------------------------------------------
void drop_head() noexcept {
    auto& slot = board.queue[board.head];
    if (slot.file)
        VirtualFree(slot.file, 0, MEM_RELEASE);
    slot = {};
    board.head = (board.head + 1) % kQueueCapacity;
    --board.count;
}
bool enqueue(SoundboardRequest request, ConstSlice<u8> file = {}) noexcept {
    if (board.count == kQueueCapacity)
        return false;
    auto& slot = board.queue[(board.head + board.count) % kQueueCapacity];
    slot = {};
    request.name = {slot.name, utf8_copy_truncated(slot.name, sizeof(slot.name), request.name)};
    request.emoji_name = {slot.emoji, utf8_copy_truncated(slot.emoji, sizeof(slot.emoji), request.emoji_name)};
    if (file.count) {
        slot.file = static_cast<u8*>(VirtualAlloc(nullptr, file.count, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!slot.file)
            return false;
        copy_bytes(slot.file, file.data, file.count);
        request.sound = {slot.file, file.count};
    }
    if (!soundboard_request_valid(request)) {
        if (slot.file)
            VirtualFree(slot.file, 0, MEM_RELEASE);
        slot = {};
        return false;
    }
    slot.request = request;
    slot.account_scope = g_app.plugin_account_scope_serial;
    ++board.count;
    return true;
}
// Next list to load when no write is queued: default sounds first.
bool next_load(SoundboardRequest* request) noexcept {
    if (board.defaults == LoadState::wanted) {
        *request = {};
        request->operation = SoundboardOperation::default_list;
        return true;
    }
    for (u32 i = 0; i < board.load_count; ++i)
        if (board.loads[i].state == LoadState::wanted) {
            *request = {};
            request->operation = SoundboardOperation::guild_list;
            request->guild_id = board.loads[i].guild_id;
            return true;
        }
    return false;
}

// Picker ------------------------------------------------------------------------
bool picker_open() noexcept {
    return g_app.popup_scene.active && g_app.popup_owner_kind == PopupOwnerKind::soundboard;
}
SoundboardSendContext send_context(const SoundboardSound& sound) noexcept {
    SoundboardSendContext c{};
    const auto* s = session();
    const auto* channel = s && in_call(*s) ? voice_channel(s->channel_id) : nullptr;
    c.in_call = channel != nullptr;
    if (channel) {
        c.permissions = channel->permissions_resolved ? channel->effective_permissions : 0;
        c.channel_guild_id = channel->guild_id;
    }
    c.sound_guild_id = sound.guild_id;
    c.now_ms = now_ms();
    c.cooldown_until_ms = board.cooldown_until_ms;
    c.premium = premium();
    c.available = sound.available;
    const auto view = application_voice_view();
    c.self_mute = view.self_mute;
    c.self_deaf = view.self_deaf;
    c.server_mute = view.server_mute || view.server_deaf;
    c.suppress = s && (s->present_flags & gateway_voice_suppress) && (s->flags & gateway_voice_suppress);
    return c;
}
Utf8View block_text(SoundboardBlock block) noexcept {
    switch (block) {
    case SoundboardBlock::not_in_call: return utf8_literal("Join a server voice channel to play sounds.");
    case SoundboardBlock::permission: return utf8_literal("You can't use the soundboard in this channel.");
    case SoundboardBlock::muted: return utf8_literal("Unmute to play sounds.");
    case SoundboardBlock::external_premium: return utf8_literal("Sounds from other servers need Nitro.");
    case SoundboardBlock::external_permission: return utf8_literal("This channel doesn't allow sounds from other servers.");
    case SoundboardBlock::unavailable: return utf8_literal("This sound isn't available.");
    case SoundboardBlock::cooldown: return utf8_literal("You're playing sounds too fast. Try again shortly.");
    default: return {};
    }
}
bool query_matches(const SoundboardSound& sound) noexcept {
    const auto& p = board.picker;
    return ascii_contains_ignore_case({sound.name, sound.name_bytes}, {p.query, p.query_bytes});
}
void set_footer(Utf8View text, bool error) noexcept {
    auto& p = board.picker;
    p.footer_bytes = utf8_copy_truncated(p.footer, sizeof(p.footer), text);
    p.error_until_ms = error ? now_ms() + kNoticeMs : 0;
}
void default_footer(bool truncated) noexcept {
    auto& p = board.picker;
    if (p.error_until_ms && now_ms() < p.error_until_ms)
        return;
    p.error_until_ms = 0;
    u32 bytes = 0;
    const auto add = [&](Utf8View text) noexcept { bytes += utf8_copy_truncated(p.footer + bytes, static_cast<u32>(sizeof(p.footer)) - bytes, text); };
    if (p.query_bytes) {
        add(utf8_literal("Search: "));
        add({p.query, p.query_bytes});
    } else
        add(truncated ? utf8_literal("Type to search all sounds") : utf8_literal("Type to search"));
    add(utf8_literal(" · Shift-click to preview"));
    p.footer_bytes = bytes;
}
// Rebuilds the popup rows: this server's sounds, other servers' (Nitro), then defaults.
bool build_picker(bool open) noexcept {
    auto& p = board.picker;
    u32 count = 0, sound_count = 0;
    bool truncated = false;
    const auto header = [&](Utf8View label) noexcept {
        if (count + 2 > kPickerItems) {
            truncated = true;
            return false;
        }
        auto& item = p.items[count];
        item = {};
        const u32 bytes = utf8_copy_truncated(p.labels[count], kLabelBytes, label);
        item.label = {p.labels[count], bytes};
        item.flags = static_cast<u16>(count ? popup_item_separator_before : popup_item_none);
        ++count;
        return true;
    };
    const auto row = [&](const SoundboardSound& sound) noexcept {
        if (count == kPickerItems) {
            truncated = true;
            return;
        }
        auto& item = p.items[count];
        item = {};
        char* label = p.labels[count];
        u32 bytes = 0;
        if (sound.emoji_name_bytes && !sound.emoji_id) {
            bytes = utf8_copy_truncated(label, kLabelBytes, {sound.emoji_name, sound.emoji_name_bytes});
            label[bytes++] = ' ';
            label[bytes++] = ' ';
        }
        bytes += utf8_copy_truncated(label + bytes, kLabelBytes - bytes, {sound.name, sound.name_bytes});
        item.label = {label, bytes};
        const auto block = soundboard_send_check(send_context(sound));
        const bool listed = block != SoundboardBlock::unavailable && block != SoundboardBlock::external_premium &&
                            block != SoundboardBlock::external_permission;
        item.flags = static_cast<u16>(listed ? popup_item_enabled : popup_item_none);
        if (sound.emoji_id) {
            item.flags |= popup_item_emoji_preview;
            item.media_id = sound.emoji_id;
        }
        item.action = kActionBase + sound_count;
        p.sounds[sound_count++] = sound.sound_id;
        ++count;
    };
    const auto group = [&](u64 guild_id, Utf8View title) noexcept {
        u32 matches = 0;
        for (u32 i = 0; i < board.sound_count; ++i)
            matches += board.sounds[i].guild_id == guild_id && query_matches(board.sounds[i]);
        if (!matches || !header(title))
            return;
        for (u32 i = 0; i < board.sound_count && count < kPickerItems; ++i)
            if (board.sounds[i].guild_id == guild_id && query_matches(board.sounds[i]))
                row(board.sounds[i]);
    };
    const auto& state = g_app.account_ui->chat_state;
    if (const auto* guild = chat_state_store_find_guild(state, p.guild_id))
        group(p.guild_id, chat_state_store_guild_name(state, *guild));
    if (premium())
        for (u32 i = 0; i < state.guild_count && count < kPickerItems; ++i) {
            const auto& guild = state.guilds[state.guild_order[i]];
            if (guild.id != p.guild_id && !guild.unavailable)
                group(guild.id, chat_state_store_guild_name(state, guild));
        }
    group(0, utf8_literal("Default sounds"));
    if (!count) {
        const bool loading = board.defaults == LoadState::wanted || board.defaults == LoadState::in_flight;
        p.items[0] = {};
        p.items[0].label = loading && !p.query_bytes ? utf8_literal("Loading sounds…")
                           : p.query_bytes           ? utf8_literal("No sounds match your search")
                                                     : utf8_literal("No sounds yet");
        count = 1;
    }
    default_footer(truncated);
    const UiPopupFeedback feedback{{p.footer, p.footer_bytes}, ui_popup_feedback_style(), p.error_until_ms != 0, 0,
                                   0};
    if (!open && !picker_open())
        return false;
    if (!popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, p.channel_id, {p.items, count}, p.anchor,
                          application_viewport_dips(), kPickerWidth, true, 0, 0, 0, &feedback)
             .ok())
        return false;
    set_popup_owner(PopupOwnerKind::soundboard);
    return refresh_popup_text();
}
void refresh_picker() noexcept {
    if (picker_open())
        build_picker(false);
}
bool send(const SoundboardSound& sound) noexcept {
    const auto c = send_context(sound);
    const auto block = soundboard_send_check(c);
    if (block != SoundboardBlock::none) {
        set_footer(block_text(block), true);
        refresh_picker();
        return true;
    }
    SoundboardRequest request{};
    request.operation = SoundboardOperation::send;
    request.channel_id = session()->channel_id;
    request.guild_id = sound.guild_id;
    request.sound_id = sound.sound_id;
    request.emoji_id = sound.emoji_id;
    request.emoji_name = {sound.emoji_name, sound.emoji_name_bytes};
    if (!enqueue(request)) {
        set_footer(block_text(SoundboardBlock::cooldown), true);
        refresh_picker();
        return true;
    }
    // Played locally at once, like the official client; the Gateway echo of
    // our own sound is not played again.
    audio(SoundboardAudioOperation::preview, sound.sound_id, 0, sound.volume);
    add_effect(g_app.account_ui->chat_state.self_user_id, session()->channel_id, {sound.emoji_name, sound.emoji_name_bytes},
               sound.emoji_id != 0, {sound.name, sound.name_bytes});
    application_request_frame();
    return true;
}

// Gateway -----------------------------------------------------------------------
void effect_gateway(Utf8View json) noexcept {
    GatewayVoiceChannelEffect effect{};
    if (!soundboard_decode_effect(json, scratch(), &effect).ok())
        return;
    const auto* s = session();
    if (!s || !in_call(*s) || effect.channel_id != s->channel_id)
        return;
    const u64 self = g_app.account_ui->chat_state.self_user_id;
    const auto* sound = effect.sound_id ? find_sound(effect.sound_id) : nullptr;
    if (effect.sound_id && effect.user_id != self) {
        const auto& preferences = application_voice_preferences();
        const auto view = application_voice_view();
        if (!preferences.soundboard_muted && !view.self_deaf && !view.server_deaf)
            audio(SoundboardAudioOperation::play, effect.sound_id, effect.user_id,
                  static_cast<u16>(effect.has_sound_volume ? effect.sound_volume
                                   : sound                 ? sound->volume
                                                           : kSoundboardVolumeScale));
    }
    add_effect(effect.user_id, effect.channel_id, {effect.emoji_name, effect.emoji_name_bytes}, effect.emoji_id != 0,
               sound ? Utf8View{sound->name, sound->name_bytes} : Utf8View{});
}
} // namespace

void application_soundboard_gateway(const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    if (event.kind == GatewayClientEventKind::ready) {
        // Server sounds belong to the account; default sounds are global.
        u32 kept = 0;
        for (u32 i = 0; i < board.sound_count; ++i)
            if (!board.sounds[i].guild_id)
                board.sounds[kept++] = board.sounds[i];
        board.sound_count = kept;
        board.load_count = 0;
        for (auto& effect : board.effects)
            effect = {};
        return;
    }
    if (event.kind != GatewayClientEventKind::dispatch || !g_app.account_ui)
        return;
    const Utf8View json{reinterpret_cast<const char*>(payload.data), payload.count};
    switch (event.dispatch_kind) {
    case GatewayClientDispatchKind::guild_delete:
        clear_guild(event.state.id);
        if (auto* load = load_of(event.state.id, false))
            load->state = LoadState::none;
        refresh_picker();
        return;
    case GatewayClientDispatchKind::voice_channel_effect_send:
        effect_gateway(json);
        return;
    case GatewayClientDispatchKind::guild_soundboard_sound_create:
    case GatewayClientDispatchKind::guild_soundboard_sound_update: {
        SoundboardSound sound{};
        if (soundboard_decode_sound(json, 0, scratch(), &sound).ok() && sound.guild_id) {
            upsert_sound(sound);
            refresh_picker();
            repaint();
        }
        return;
    }
    case GatewayClientDispatchKind::guild_soundboard_sound_delete: {
        u64 sound_id = 0, guild_id = 0;
        if (soundboard_decode_delete(json, scratch(), &sound_id, &guild_id).ok()) {
            remove_sound(sound_id);
            refresh_picker();
            repaint();
        }
        return;
    }
    case GatewayClientDispatchKind::guild_soundboard_sounds_update:
    case GatewayClientDispatchKind::soundboard_sounds: {
        u64 guild_id = 0;
        const auto decoded =
            soundboard_decode_sound_list(json, 0, scratch(), list_buffer(), &guild_id);
        if (decoded.ok() && guild_id) {
            replace_guild(guild_id, decoded_list(decoded.count));
            if (auto* load = load_of(guild_id, true))
                load->state = LoadState::loaded;
            refresh_picker();
            repaint();
        }
        return;
    }
    default:
        return;
    }
}

bool application_soundboard_available(u64 channel_id) noexcept {
    const auto* s = session();
    return s && in_call(*s) && s->channel_id == channel_id && voice_channel(channel_id) &&
           g_app.account.api.soundboard && g_app.account.api.soundboard_audio;
}

bool application_soundboard_picker_open() noexcept {
    return picker_open();
}

bool application_soundboard_toggle(u64 channel_id, UiRect anchor) noexcept {
    if (picker_open())
        return dismiss_popup();
    const auto* channel = voice_channel(channel_id);
    if (!channel || !application_soundboard_available(channel_id))
        return false;
    auto& p = board.picker;
    p.channel_id = channel_id;
    p.guild_id = channel->guild_id;
    p.anchor = anchor;
    p.query_bytes = 0;
    p.error_until_ms = 0;
    want_defaults();
    want_guild(channel->guild_id);
    if (premium()) {
        const auto& state = g_app.account_ui->chat_state;
        u32 wanted = 0;
        for (u32 i = 0; i < state.guild_count && wanted < kExternalGuildLoads; ++i) {
            const auto& guild = state.guilds[state.guild_order[i]];
            if (guild.id != channel->guild_id && !guild.unavailable) {
                want_guild(guild.id);
                ++wanted;
            }
        }
    }
    application_request_frame();
    return build_picker(true);
}

bool application_soundboard_menu_action(u32 action) noexcept {
    return picker_open() && action >= kActionBase && action < kActionBase + kPickerItems;
}

bool application_soundboard_menu_activate(u32 action, bool preview) noexcept {
    if (!application_soundboard_menu_action(action))
        return false;
    const auto* sound = find_sound(board.picker.sounds[action - kActionBase]);
    if (!sound)
        return true;
    if (preview)
        return audio(SoundboardAudioOperation::preview, sound->sound_id, 0, sound->volume) || true;
    return send(*sound);
}

bool application_soundboard_menu_key(u32 key) noexcept {
    if (!picker_open() || (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000))
        return false;
    auto& p = board.picker;
    char typed = 0;
    if (key >= 'A' && key <= 'Z')
        typed = static_cast<char>((GetKeyState(VK_SHIFT) & 0x8000) ? key : key + 32);
    else if (key >= '0' && key <= '9' && !(GetKeyState(VK_SHIFT) & 0x8000))
        typed = static_cast<char>(key);
    else if (key == VK_SPACE && p.query_bytes)
        typed = ' ';
    else if (key == VK_BACK) {
        if (!p.query_bytes)
            return true;
        --p.query_bytes;
        return build_picker(false) || true;
    } else
        return false;
    if (p.query_bytes + 1 >= kQueryBytes)
        return true;
    p.query[p.query_bytes++] = typed;
    return build_picker(false) || true;
}

Utf8View application_soundboard_effect(u64 user_id) noexcept {
    const auto* s = session();
    if (!s || !in_call(*s) || !user_id)
        return {};
    const u64 now = now_ms();
    for (const auto& effect : board.effects)
        if (effect.user_id == user_id && effect.channel_id == s->channel_id && now < effect.until_ms)
            return {effect.text, effect.bytes};
    return {};
}

SoundboardGuildView application_soundboard_guild(u64 guild_id) noexcept {
    SoundboardGuildView view{};
    const auto* load = load_of(guild_id, false);
    view.loaded = load && load->state == LoadState::loaded;
    view.loading = load && (load->state == LoadState::wanted || load->state == LoadState::in_flight);
    // Rows of one server are contiguous only after a load: a stable partition puts them in front.
    u32 count = 0;
    for (u32 i = 0; i < board.sound_count; ++i)
        if (board.sounds[i].guild_id == guild_id) {
            const auto sound = board.sounds[i];
            for (u32 k = i; k > count; --k)
                board.sounds[k] = board.sounds[k - 1];
            board.sounds[count++] = sound;
        }
    view.sounds = count ? board.sounds : nullptr;
    view.count = count;
    for (u32 i = 0; i < board.count; ++i) {
        const auto& queued = board.queue[(board.head + i) % kQueueCapacity].request;
        view.busy = view.busy || (queued.guild_id == guild_id && queued.operation != SoundboardOperation::send);
    }
    view.busy = view.busy || (board.in_flight_request && board.in_flight_guild == guild_id &&
                              board.in_flight != SoundboardOperation::send &&
                              board.in_flight != SoundboardOperation::guild_list);
    if (board.notice_bytes && board.notice_guild == guild_id && now_ms() < board.notice_until_ms)
        view.notice = {board.notice, board.notice_bytes};
    return view;
}

u32 application_soundboard_revision() noexcept {
    return board.revision;
}

void application_soundboard_request_guild(u64 guild_id) noexcept {
    if (!guild_id)
        return;
    want_guild(guild_id);
    application_request_frame();
}

const SoundboardSound* application_soundboard_find(u64 sound_id) noexcept {
    return find_sound(sound_id);
}

bool application_soundboard_probe(ConstSlice<u8> file, SoundboardFileInfo* info, Utf8View* error) noexcept {
    SoundboardFileInfo local{};
    auto* out = info ? info : &local;
    *out = {};
    SoundboardRequest request{};
    request.operation = SoundboardOperation::probe;
    request.sound = file;
    request.info = out;
    auto& h = g_app.account;
    const bool ok = h.instance && h.api.soundboard && h.api.soundboard(h.instance, &request, 0).ok() && out->ok();
    if (!ok && error) {
        switch (out->error) {
        case SoundboardFileError::too_large: *error = utf8_literal("Choose a sound of 512 KB or less."); break;
        case SoundboardFileError::too_long: *error = utf8_literal("Sounds can be at most 5.2 seconds long."); break;
        case SoundboardFileError::format: *error = utf8_literal("Choose an MP3 or Ogg file."); break;
        case SoundboardFileError::empty: *error = utf8_literal("That file is empty."); break;
        default: *error = utf8_literal("That sound file couldn't be read."); break;
        }
    }
    return ok;
}

bool application_soundboard_create(u64 guild_id, Utf8View name, Utf8View emoji, u16 volume,
                                   ConstSlice<u8> file) noexcept {
    SoundboardFileInfo info{};
    if (!application_soundboard_probe(file, &info, nullptr))
        return false;
    SoundboardRequest request{};
    request.operation = SoundboardOperation::create;
    request.guild_id = guild_id;
    request.name = name;
    request.emoji_name = emoji;
    request.volume = volume;
    request.format = info.format;
    const bool queued = enqueue(request, file);
    repaint();
    return queued;
}

bool application_soundboard_update(u64 guild_id, u64 sound_id, Utf8View name, Utf8View emoji, u64 emoji_id,
                                   u16 volume) noexcept {
    SoundboardRequest request{};
    request.operation = SoundboardOperation::update;
    request.guild_id = guild_id;
    request.sound_id = sound_id;
    request.name = name;
    request.emoji_name = emoji;
    request.emoji_id = emoji_id;
    request.volume = volume;
    const bool queued = enqueue(request);
    repaint();
    return queued;
}

bool application_soundboard_remove(u64 guild_id, u64 sound_id) noexcept {
    SoundboardRequest request{};
    request.operation = SoundboardOperation::remove;
    request.guild_id = guild_id;
    request.sound_id = sound_id;
    const bool queued = enqueue(request);
    repaint();
    return queued;
}

bool application_soundboard_preview(u64 sound_id, u16 volume) noexcept {
    return audio(SoundboardAudioOperation::preview, sound_id, 0, volume);
}

bool application_soundboard_pending(u64 now) noexcept {
    // Writes from a previous account scope are dropped unsent.
    while (board.count && board.queue[board.head].account_scope != g_app.plugin_account_scope_serial)
        drop_head();
    if (board.in_flight_request || (board.retry_at_ms && now < board.retry_at_ms) ||
        !g_app.account.instance || !g_app.account.api.soundboard)
        return false;
    SoundboardRequest load{};
    return board.count || next_load(&load);
}

bool application_soundboard_submit(u64 request_id, u64 now) noexcept {
    if (!application_soundboard_pending(now))
        return false;
    auto& h = g_app.account;
    SoundboardRequest load{};
    const bool write = board.count != 0;
    if (!write)
        next_load(&load);
    const SoundboardRequest& request = write ? board.queue[board.head].request : load;
    const auto submitted = h.api.soundboard(h.instance, &request, request_id);
    if (submitted.error == RestClientSubmitError::busy) {
        board.retry_at_ms = now + kBusyRetryMs;
        return false;
    }
    board.retry_at_ms = 0;
    if (!submitted.ok()) {
        log_u64(LogLevel::warning, "soundboard", "request_refused", static_cast<u64>(submitted.error));
        if (write) {
            if (request.operation != SoundboardOperation::send)
                set_notice(request.guild_id, utf8_literal("That change couldn't be sent."));
            drop_head();
        } else if (request.operation == SoundboardOperation::default_list) {
            board.defaults = LoadState::failed;
            board.defaults_retry_ms = now + kLoadRetryMs;
        } else if (auto* l = load_of(request.guild_id, false)) {
            l->state = LoadState::failed;
            l->retry_at_ms = now + kLoadRetryMs;
        }
        return false;
    }
    board.in_flight_request = request_id;
    board.in_flight = request.operation;
    board.in_flight_guild = request.guild_id;
    board.in_flight_sound = request.sound_id;
    if (request.operation == SoundboardOperation::default_list)
        board.defaults = LoadState::in_flight;
    else if (request.operation == SoundboardOperation::guild_list) {
        if (auto* l = load_of(request.guild_id, false))
            l->state = LoadState::in_flight;
    }
    if (write)
        drop_head();
    return true;
}

bool application_soundboard_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!board.in_flight_request || response.request_id != board.in_flight_request)
        return true;
    board.in_flight_request = 0;
    const auto operation = board.in_flight;
    const u64 guild_id = board.in_flight_guild;
    const u64 now = now_ms();
    const bool ok = response.error == RestClientError::none && response.status_code >= 200 &&
                    response.status_code < 300;
    switch (operation) {
    case SoundboardOperation::default_list:
    case SoundboardOperation::guild_list: {
        SoundboardDecodeResult decoded{0, SoundboardDecodeError::input};
        if (ok && body.bytes)
            decoded = soundboard_decode_sound_list(body, guild_id, scratch(), list_buffer());
        auto* load = operation == SoundboardOperation::guild_list ? load_of(guild_id, false) : nullptr;
        if (decoded.ok()) {
            replace_guild(guild_id, decoded_list(decoded.count));
            if (operation == SoundboardOperation::default_list)
                board.defaults = LoadState::loaded;
            else if (load)
                load->state = LoadState::loaded;
        } else {
            log_u64(LogLevel::warning, "soundboard", "list_failed", response.status_code);
            if (operation == SoundboardOperation::default_list) {
                board.defaults = LoadState::failed;
                board.defaults_retry_ms = now + kLoadRetryMs;
            } else if (load) {
                load->state = LoadState::failed;
                load->retry_at_ms = now + kLoadRetryMs;
                if (response.status_code != 403)
                    set_notice(guild_id, utf8_literal("Couldn't load this server's sounds."));
            }
        }
        break;
    }
    case SoundboardOperation::send:
        if (!ok) {
            const bool limited = response.status_code == 429 || response.rate_limited;
            if (limited)
                board.cooldown_until_ms =
                    now + (response.rate.retry_after_ms ? minimum<u64>(response.rate.retry_after_ms, 60'000)
                                                        : kRateLimitMs);
            set_footer(limited ? block_text(SoundboardBlock::cooldown)
                               : response.status_code == 403 ? block_text(SoundboardBlock::permission)
                                                             : utf8_literal("Couldn't play that sound."),
                       true);
            refresh_picker();
        }
        break;
    case SoundboardOperation::create:
    case SoundboardOperation::update:
        if (ok) {
            SoundboardSound sound{};
            if (body.bytes && soundboard_decode_sound(body, guild_id, scratch(), &sound).ok())
                upsert_sound(sound);
            board.notice_bytes = 0;
        } else {
            u64 code = 0;
            if (body.bytes)
                code = guild_asset_error_code(body, scratch());
            set_notice(guild_id, code == kMaximumSounds ? utf8_literal("This server has no free sound slots.")
                                 : response.status_code == 429
                                     ? utf8_literal("You're making changes too fast. Try again shortly.")
                                 : response.status_code == 403
                                     ? utf8_literal("You don't have permission to manage sounds here.")
                                 : operation == SoundboardOperation::create ? utf8_literal("Couldn't upload that sound.")
                                                                            : utf8_literal("Couldn't save that sound."));
        }
        break;
    case SoundboardOperation::remove:
        if (ok)
            remove_sound(board.in_flight_sound);
        else
            set_notice(guild_id, utf8_literal("Couldn't delete that sound."));
        break;
    default:
        break;
    }
    refresh_picker();
    repaint();
    return true;
}

u64 application_soundboard_next_wake(bool lane_idle) noexcept {
    const u64 now = now_ms();
    u64 wake = 0;
    const auto earliest = [&](u64 at) noexcept { wake = !wake || at < wake ? at : wake; };
    bool expired = false;
    for (auto& effect : board.effects) {
        if (!effect.user_id)
            continue;
        if (now >= effect.until_ms) {
            effect = {};
            expired = true;
        } else
            earliest(effect.until_ms);
    }
    if (board.notice_bytes && now >= board.notice_until_ms) {
        board.notice_bytes = 0;
        expired = true;
    } else if (board.notice_bytes)
        earliest(board.notice_until_ms);
    if (board.picker.error_until_ms) {
        if (now >= board.picker.error_until_ms) {
            board.picker.error_until_ms = 0;
            refresh_picker();
        } else
            earliest(board.picker.error_until_ms);
    }
    if (expired)
        repaint();
    SoundboardRequest load{};
    if (lane_idle && !board.in_flight_request && (board.count || next_load(&load)))
        earliest(board.retry_at_ms > now ? board.retry_at_ms : now + 1);
    return wake;
}

void application_soundboard_release() noexcept {
    while (board.count)
        drop_head();
    // In place: the state is too large for a temporary.
    secure_clear_bytes(&board, sizeof(board));
}
} // namespace maicord::app_detail
