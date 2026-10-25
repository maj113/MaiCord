#include "maicord/app/startup_shell.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/app/message_event_pump.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/model/message_store.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/protocol/guild_folders.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/model/user_store.hpp"

namespace maicord {
namespace {
constexpr u64 kProfile = 0x5EED00000001ull, kSelf = 1, kNow = 1'800'000'000'000ull;
constexpr u64 kRead = kPermissionViewChannel | kPermissionReadMessageHistory;
constexpr u64 kSend = kRead | kPermissionSendMessages;
constexpr auto kIcon = utf8_literal("0123456789abcdef0123456789abcdef");

// The stores READY populates, at a test's size.
struct Stores {
    ChatStateStore state;
    GuildRecord guilds[8];
    ChannelRecord channels[32];
    u16 guild_order[8], channel_order[32];
    u8 names[8192];
    RoleRecord roles[8];
    PermissionOverwriteRecord overwrites[8];
    PrivateChannelStore conversations;
    UserStore users;
    UserRecord user_records[32];
    u32 user_order[32];
    u8 user_names[4096];
    GuildMemberStore members;
    GuildMemberRecord member_records[16];
    u32 member_order[16];
    StartupShellStores view() noexcept { return {&state, &users, &members}; }
};

bool initialize(Stores& s) noexcept {
    clear_bytes(&s, sizeof(s));
    const bool ok =
        chat_state_store_initialize(&s.state, {s.guilds, 8}, {s.guild_order, 8}, {s.channels, 32},
                                    {s.channel_order, 32}, {s.names, sizeof(s.names)}) &&
        chat_state_store_bind_permissions(&s.state, {s.roles, 8}, {s.overwrites, 8}) &&
        private_channel_store_initialize_owned(&s.conversations, 8, 32) &&
        user_store_initialize(&s.users, {s.user_records, 32}, {s.user_order, 32}, {s.user_names, sizeof(s.user_names)}) &&
        guild_member_store_initialize(&s.members, {s.member_records, 16}, {s.member_order, 16});
    s.state.private_channels = &s.conversations;
    return ok;
}

void release(Stores& s) noexcept {
    private_channel_store_release(&s.conversations);
    user_appearance_release(&s.users.appearance);
    guild_member_store_release(&s.members);
}

bool add_channel(Stores& s, u64 id, u64 guild, u8 type, u32 position, u64 parent, Utf8View name,
                 u64 permissions) noexcept {
    return chat_state_store_upsert_channel(&s.state, {id, guild, parent, name, position, 0, type}).ok() &&
           chat_state_store_restore_channel(&s.state, id, permissions, 0);
}

// Guild 20 is selected on 201: category 200 (collapsed) holds 201 (send) and
// 202 (read only); 203 is hidden. Guilds 21 and an unavailable 22 sit beside it.
bool populate(Stores& s, MessageStore& messages) noexcept {
    s.state.self_user_id = kSelf;
    GuildInput alpha{20, utf8_literal("Alpha"), GuildMutationKind::create, 9};
    alpha.icon_hash = kIcon;
    alpha.icon_known = true;
    bool ok = chat_state_store_upsert_guild(&s.state, alpha).ok() &&
              chat_state_store_upsert_guild(&s.state, {21, utf8_literal("Beta"), GuildMutationKind::create, 9}).ok() &&
              chat_state_store_upsert_guild(&s.state, {22, {}, GuildMutationKind::unavailable, 0}).ok() &&
              add_channel(s, 200, 20, 4, 0, 0, utf8_literal("Text"), kRead) &&
              add_channel(s, 201, 20, 0, 0, 200, utf8_literal("general"), kSend) &&
              add_channel(s, 202, 20, 0, 1, 200, utf8_literal("announcements"), kRead) &&
              add_channel(s, 203, 20, 0, 2, 200, utf8_literal("staff"), 0) &&
              add_channel(s, 211, 21, 0, 0, 0, utf8_literal("lobby"), kSend) &&
              chat_state_store_select_channel(&s.state, 201).ok();
    s.channels[chat_state_store_find_channel(s.state, 200) - s.channels].collapsed = true;
    ok = ok && user_store_upsert(&s.users, kSelf, utf8_literal("Self"), utf8_literal("self")).ok() &&
         user_store_upsert(&s.users, 5, utf8_literal("Five"), utf8_literal("five")).ok() &&
         user_store_upsert(&s.users, 6, utf8_literal("Six"), utf8_literal("six")).ok() &&
         user_store_set_avatar(&s.users, 5, kIcon) &&
         guild_member_store_restore_display(&s.members, s.state, 20, 5, utf8_literal("Fiver"), 0xFF0000);
    UserAppearance status{};
    status.user_id = kSelf;
    status.fields = appearance_activity | appearance_custom_status;
    copy_bytes(status.activity.data, "Busy", 4);
    status.activity.bytes = 4;
    ok = ok && user_appearance_upsert(&s.users.appearance, status);
    ok = ok && message_store_upsert(&messages, {1001, 201, 5, 1000, 0, utf8_literal("a"), 0}).ok() &&
         message_store_upsert(&messages, {1002, 201, 6, 2000, 0, utf8_literal("b"), 0}).ok() &&
         message_store_upsert(&messages, {1003, 201, 5, 3000, 0, utf8_literal("c"), 0}).ok();
    return ok;
}

StartupShell* allocate_shell() noexcept {
    return static_cast<StartupShell*>(
        VirtualAlloc(nullptr, sizeof(StartupShell), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}

bool temporary_path(wchar_t* path, u32 capacity) noexcept {
    const DWORD length = GetTempPathW(capacity, path);
    constexpr wchar_t name[] = L"maicord-startup-shell-test.shell";
    if (!length || length + sizeof(name) / sizeof(wchar_t) > capacity)
        return false;
    copy_bytes(path + length, name, sizeof(name));
    return true;
}

bool ready(MessageEventPump& pump, ConstSlice<u64> guilds) noexcept {
    u8 payload[64];
    for (u32 i = 0; i < guilds.count; ++i)
        byte_store_le64(payload + i * 8, guilds[i]);
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::ready;
    event.state.id = kSelf;
    event.state.record_count = guilds.count;
    event.dispatch_payload_bytes = guilds.count * 8;
    event.timestamp_ms = 100;
    return message_event_pump_consume(&pump, event, {payload, guilds.count * 8}).ok();
}

} // namespace

void test_startup_shell(void (*check)(bool, const char*) noexcept) noexcept {
    auto* source = static_cast<Stores*>(VirtualAlloc(nullptr, sizeof(Stores), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* target = static_cast<Stores*>(VirtualAlloc(nullptr, sizeof(Stores), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    StartupShell* shell = allocate_shell();
    StartupShell* decoded = allocate_shell();
    auto* plaintext = static_cast<u8*>(VirtualAlloc(nullptr, kStartupShellPlaintextBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* again = static_cast<u8*>(VirtualAlloc(nullptr, kStartupShellPlaintextBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* image = static_cast<u8*>(VirtualAlloc(nullptr, kStartupShellFileBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!source || !target || !shell || !decoded || !plaintext || !again || !image) {
        check(false, "startup shell fixture allocates");
        return;
    }
    MessageRecord message_records[8]{};
    u32 message_order[8]{};
    u8 message_text[256]{};
    MessageStore messages{};
    check(initialize(*source) && message_store_initialize(&messages, {message_records, 8}, {message_order, 8},
                                                          {message_text, sizeof(message_text)}) &&
              populate(*source, messages),
          "startup shell: synthetic session populates");

    // Capture records what the shell shows, and nothing hidden.
    startup_shell_capture(source->view(), &messages, shell);
    shell->profile_id = kProfile;
    shell->saved_unix_ms = kNow;
    const auto* captured_201 = [&]() noexcept -> const StartupShellChannel* {
        for (u32 i = 0; i < shell->channel_count; ++i)
            if (shell->channels[i].id == 201)
                return &shell->channels[i];
        return nullptr;
    }();
    bool hidden_absent = true;
    for (u32 i = 0; i < shell->channel_count; ++i)
        hidden_absent = hidden_absent && shell->channels[i].id != 203 && shell->channels[i].id != 211;
    check(shell->guild_count == 3 && shell->channel_count == 3 && hidden_absent && captured_201 &&
              captured_201->permissions == kSend && shell->guild_id == 20 && shell->channel_id == 201 &&
              shell->self_user_id == kSelf && shell->user_count == 3 && shell->author_count == 1 &&
              shell->authors[0].user_id == 5 && shell->authors[0].role_color == 0xFF0000 &&
              utf8_equal(startup_shell_text(*shell, shell->status_text), utf8_literal("Busy")),
          "startup shell: capture holds the visible channels, the account and the page's authors");
    // Guilds follow the rail's layout, so a full table keeps the rows on screen.
    auto* rail = static_cast<GuildFolderSettings*>(
        VirtualAlloc(nullptr, sizeof(GuildFolderSettings), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (rail) {
        rail->present = true;
        rail->positions[0] = 21;
        rail->positions[1] = 22;
        rail->positions[2] = 20;
        rail->position_count = 3;
        StartupShellStores ordered = source->view();
        ordered.folders = rail;
        startup_shell_capture(ordered, &messages, decoded);
        VirtualFree(rail, 0, MEM_RELEASE);
    }
    check(rail && decoded->guild_count == 3 && decoded->guilds[0].id == 21 && decoded->guilds[1].id == 22 &&
              decoded->guilds[2].id == 20,
          "startup shell: guilds are recorded in rail order");

    // Encode/decode round trip, byte for byte.
    const u32 encoded = startup_shell_encode(*shell, {plaintext, kStartupShellPlaintextBytes});
    check(encoded && startup_shell_decode({plaintext, encoded}, decoded) &&
              startup_shell_encode(*decoded, {again, kStartupShellPlaintextBytes}) == encoded &&
              bytes_equal(plaintext, again, encoded),
          "startup shell: the body round-trips exactly");
    check(!startup_shell_decode({plaintext, encoded - 1}, decoded) &&
              !startup_shell_decode({plaintext, encoded / 2}, decoded),
          "startup shell: a truncated body is rejected");
    plaintext[encoded - 1] = 0xFF;
    check(!startup_shell_decode({plaintext, encoded}, decoded), "startup shell: invalid UTF-8 text is rejected");

    // Sealed through the per-account key; a fresh key unwraps it.
    StartupShellKey key{};
    check(startup_shell_key_create(&key, kProfile), "startup shell: the account key is created and wrapped");
    const u32 body = startup_shell_encode(*shell, {plaintext, kStartupShellPlaintextBytes});
    const u32 sealed = startup_shell_seal(key, {plaintext, body}, {image, kStartupShellFileBytes});
    check(sealed > body && sealed < body + kStartupShellWrappedKeyBytes + 64,
          "startup shell: the image is the body plus header, wrapped key, nonce and tag");
    StartupShellKey opened{};
    check(startup_shell_open({image, sealed}, kProfile, kNow, &opened, {again, kStartupShellPlaintextBytes}, decoded) ==
                  StartupShellOpenError::none &&
              decoded->guild_count == 3 && decoded->channel_id == 201 && opened.aead.handle,
          "startup shell: a sealed shell opens with the unwrapped key");
    const u32 resealed = startup_shell_seal(key, {plaintext, body}, {again, kStartupShellFileBytes});
    check(resealed == sealed && !bytes_equal(image, again, sealed), "startup shell: every seal takes a fresh nonce");
    check(startup_shell_open({image, sealed}, kProfile + 1, kNow, &opened, {again, kStartupShellPlaintextBytes},
                             decoded) == StartupShellOpenError::account,
          "startup shell: another account's shell is ignored");
    image[4] ^= 1;
    check(startup_shell_open({image, sealed}, kProfile, kNow, &opened, {again, kStartupShellPlaintextBytes}, decoded) ==
              StartupShellOpenError::version,
          "startup shell: another version is discarded");
    image[4] ^= 1;
    image[sealed - kAesGcmTagBytes - 1] ^= 0x40;
    check(startup_shell_open({image, sealed}, kProfile, kNow, &opened, {again, kStartupShellPlaintextBytes}, decoded) ==
              StartupShellOpenError::tamper,
          "startup shell: a changed ciphertext fails authentication");
    image[sealed - kAesGcmTagBytes - 1] ^= 0x40;
    image[24 + key.wrapped_bytes] ^= 1;
    check(startup_shell_open({image, sealed}, kProfile, kNow, &opened, {again, kStartupShellPlaintextBytes}, decoded) ==
              StartupShellOpenError::tamper,
          "startup shell: a changed nonce fails authentication");
    image[24 + key.wrapped_bytes] ^= 1;
    check(startup_shell_open({image, sealed - 1}, kProfile, kNow, &opened, {again, kStartupShellPlaintextBytes},
                             decoded) == StartupShellOpenError::format &&
              startup_shell_open({image, 20}, kProfile, kNow, &opened, {again, kStartupShellPlaintextBytes},
                                 decoded) == StartupShellOpenError::format,
          "startup shell: a truncated image is malformed");
    check(startup_shell_open({image, sealed}, kProfile, kNow + kStartupShellMaximumAgeMs + 1, &opened,
                             {again, kStartupShellPlaintextBytes}, decoded) == StartupShellOpenError::expired &&
              startup_shell_open({image, sealed}, kProfile, kNow + kStartupShellMaximumAgeMs, &opened,
                                 {again, kStartupShellPlaintextBytes}, decoded) == StartupShellOpenError::none,
          "startup shell: one older than seven days is discarded");
    StartupShellKey foreign{};
    check(startup_shell_key_create(&foreign, kProfile) &&
              startup_shell_open({image, sealed}, kProfile, kNow, &foreign, {again, kStartupShellPlaintextBytes},
                                 decoded) == StartupShellOpenError::tamper,
          "startup shell: another key cannot open it");
    startup_shell_key_release(&foreign);
    startup_shell_key_release(&opened);

    // Through the file: atomic write, then a fresh read unwraps the key.
    wchar_t path[512];
    StartupShellKey loaded{};
    check(temporary_path(path, 512) && startup_shell_save_file(path, &key, *shell) &&
              startup_shell_load_file(path, kProfile, kNow, &loaded, decoded) == StartupShellOpenError::none &&
              decoded->channel_count == 3 && decoded->author_count == 1 && loaded.wrapped_bytes == key.wrapped_bytes,
          "startup shell: the file round-trips through the encryption layer");
    check(startup_shell_load_file(path, kProfile + 1, kNow, &loaded, decoded) == StartupShellOpenError::account,
          "startup shell: the file is ignored for another account");
    DeleteFileW(path);
    check(startup_shell_load_file(path, kProfile, kNow, &loaded, decoded) == StartupShellOpenError::format,
          "startup shell: a missing file is no shell");
    startup_shell_key_release(&loaded);
    startup_shell_key_release(&key);

    // Before READY: the stores READY fills hold the shell.
    check(initialize(*target), "startup shell: empty stores initialize");
    ChatStateStore& state = target->state;
    state.restore_channel_id = 201;
    state.restore_guild_id = 20;
    check(startup_shell_apply(*shell, target->view()) && state.guild_count == 3 && state.channel_count == 3 &&
              state.selected_channel_id == 201 && state.selected_guild_id == 20 && !state.restore_channel_id &&
              state.self_user_id == kSelf,
          "startup shell: before READY the shell's guilds, channels and selection are resident");
    const ChannelRecord* general = chat_state_store_find_channel(state, 201);
    const ChannelRecord* news = chat_state_store_find_channel(state, 202);
    const ChannelRecord* category = chat_state_store_find_channel(state, 200);
    const GuildMemberRecord* author = guild_member_store_find(target->members, 20, 5);
    const UserRecord* self = user_store_find(target->users, kSelf);
    const auto* status = user_appearance_find(target->users.appearance, kSelf);
    check(general && (general->actions & channel_action_send_messages) && !chat_state_store_channel_unread(state, *general) &&
              news && (news->actions & channel_action_read_history) && !(news->actions & channel_action_send_messages) &&
              category && category->collapsed && !chat_state_store_find_channel(state, 203),
          "startup shell: channels carry their saved send capability and collapsed state");
    check(author && !author->roles_known && author->role_color == 0xFF0000 &&
              utf8_equal(guild_member_store_nickname(*author), utf8_literal("Fiver")) && self &&
              utf8_equal(user_store_name(target->users, *self), utf8_literal("Self")) && status &&
              utf8_equal(status->activity.view(), utf8_literal("Busy")),
          "startup shell: the account panel and the authors' nickname and colour are restored");
    const GuildRecord* alpha = chat_state_store_find_guild(state, 20);
    const GuildRecord* outage = chat_state_store_find_guild(state, 22);
    check(alpha && utf8_equal(chat_state_store_guild_icon_hash(*alpha), kIcon) && !alpha->unread_channels &&
              !alpha->mention_count && outage && outage->unavailable,
          "startup shell: the rail keeps icons and outages, without unread state");
    check(chat_state_store_note_history_latest(&state, 201, 5000) && general->last_message_id == 5000 &&
              !chat_state_store_channel_unread(state, *general) && !alpha->unread_channels,
          "startup shell: a page applied before READY leaves its channel read");

    // READY replaces the shell wholesale and lands on its selection.
    MessageTimeline timeline{};
    MessageEventPump pump{};
    u64 scratch[8]{};
    pump.state = &state;
    pump.users = &target->users;
    pump.members = &target->members;
    pump.timeline = &timeline;
    pump.guild_id_scratch = {scratch, 8};
    const u64 ready_guilds[] = {20};
    check(ready(pump, {ready_guilds, 1}) && state.guild_count == 1 && !chat_state_store_find_guild(state, 21) &&
              !chat_state_store_find_guild(state, 22) && state.channel_count == 0 && state.restore_channel_id == 201 &&
              !state.selected_channel_id,
          "startup shell: READY removes every shell entry it does not list");
    const GuildMemberRecord* retained = guild_member_store_find(target->members, 20, 5);
    check(retained && retained->role_color == 0xFF0000 && !retained->roles_known,
          "startup shell: READY keeps an author's shown colour until roles arrive");
    // GUILD_CREATE resolves the channel's permissions; the restoration then places it.
    check(chat_state_store_upsert_guild(&state, {20, utf8_literal("Alpha"), GuildMutationKind::create, 9}).ok() &&
              add_channel(*target, 201, 20, 0, 0, 0, utf8_literal("general"), kSend) &&
              chat_state_store_select_default(&state) && state.selected_channel_id == 201 && !state.restore_channel_id && !chat_state_store_find_channel(state, 202),
          "startup shell: READY's channels keep the shell's selection");

    // A saved channel that no longer exists leaves its server pending.
    check(initialize(*target), "startup shell: second fixture initializes");
    shell->channel_count = 0;
    check(startup_shell_apply(*shell, target->view()) && target->state.selected_guild_id == 20 &&
              !target->state.selected_channel_id && target->state.restore_channel_id == 201 &&
              target->state.restore_guild_id == 20,
          "startup shell: without its channel the saved server shows loading until READY");
    release(*target);
    release(*source);
    VirtualFree(image, 0, MEM_RELEASE);
    VirtualFree(again, 0, MEM_RELEASE);
    VirtualFree(plaintext, 0, MEM_RELEASE);
    VirtualFree(decoded, 0, MEM_RELEASE);
    VirtualFree(shell, 0, MEM_RELEASE);
    VirtualFree(target, 0, MEM_RELEASE);
    VirtualFree(source, 0, MEM_RELEASE);
}

} // namespace maicord
