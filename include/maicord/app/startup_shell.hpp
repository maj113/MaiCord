#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/platform/aes_gcm.hpp"

namespace maicord {

struct ChatStateStore;
struct GuildMemberStore;
struct MessageStore;
struct UserStore;

// The authenticated shell as last shown, so the next launch draws it before
// READY: the guild rail, the selected server's visible channels (or the DM
// list), the account panel and the display of the last page's authors.
// Names, asset hashes and permissions only: no message content, unread or
// mention counts, presence or member lists.
inline constexpr u32 kStartupShellVersion = 1;
inline constexpr u32 kStartupShellGuildCapacity = 256;
inline constexpr u32 kStartupShellChannelCapacity = 640;
inline constexpr u32 kStartupShellConversationCapacity = 64;
inline constexpr u32 kStartupShellRecipientCapacity = 512;
inline constexpr u32 kStartupShellConversationRecipients = 16;
inline constexpr u32 kStartupShellUserCapacity = 384;
inline constexpr u32 kStartupShellAuthorCapacity = 64;
inline constexpr u32 kStartupShellExpandedCapacity = 64;
inline constexpr u32 kStartupShellFolderBytes = 16 * 1024;
inline constexpr u32 kStartupShellTextBytes = 96 * 1024;
inline constexpr u32 kStartupShellPlaintextBytes = 192 * 1024;
inline constexpr u32 kStartupShellWrappedKeyBytes = 1024;
inline constexpr u32 kStartupShellFileBytes = 64 + kStartupShellWrappedKeyBytes + kStartupShellPlaintextBytes;
inline constexpr u64 kStartupShellMaximumAgeMs = 7ull * 24 * 60 * 60 * 1000;

// A string in StartupShell::text.
struct StartupShellText {
    u32 offset;
    u16 bytes;
};

enum StartupShellGuildFlag : u8 {
    startup_shell_guild_unavailable = 1u << 0,
    startup_shell_guild_muted = 1u << 1,
    startup_shell_guild_hide_muted = 1u << 2,
    startup_shell_guild_opt_in = 1u << 3,
};
struct StartupShellGuild {
    u64 id;
    StartupShellText name, icon, banner;
    u8 flags;
};

enum StartupShellChannelFlag : u8 {
    startup_shell_channel_collapsed = 1u << 0,
    startup_shell_channel_muted = 1u << 1,
    startup_shell_channel_dimmed = 1u << 2,
    startup_shell_channel_opted_in = 1u << 3,
    startup_shell_thread_joined = 1u << 4,
    startup_shell_thread_archived = 1u << 5,
    startup_shell_thread_locked = 1u << 6,
};
// A channel of the selected server; threads take their parent's permissions.
struct StartupShellChannel {
    u64 id, parent_id, permissions;
    u32 position, flags;
    u8 type, state;
    StartupShellText name;
};

// A DM or group DM; its recipients are recipients[first_recipient...].
struct StartupShellConversation {
    u64 id, owner_id, last_message_id;
    u32 first_recipient;
    u16 recipient_count;
    u8 type;
    StartupShellText name, icon;
};

struct StartupShellUser {
    u64 id;
    StartupShellText name, username, avatar;
};

// How an author of the last page was shown in the selected server.
struct StartupShellAuthor {
    u64 user_id;
    u32 role_color;
    StartupShellText nickname;
};

// The file stores the records in this layout: a change needs a new version.
static_assert(sizeof(StartupShellText) == 8 && sizeof(StartupShellGuild) == 40 &&
              sizeof(StartupShellChannel) == 48 && sizeof(StartupShellConversation) == 48 &&
              sizeof(StartupShellUser) == 32 && sizeof(StartupShellAuthor) == 24);

struct StartupShell {
    u64 profile_id, self_user_id, saved_unix_ms;
    // The selection; guild 0 is Home.
    u64 guild_id, channel_id;
    // The account's custom status line.
    u64 status_emoji_id;
    StartupShellText status_text, status_emoji;
    u32 guild_count, channel_count, conversation_count, recipient_count, user_count, author_count;
    u32 expanded_count, folder_bytes, text_bytes;
    StartupShellGuild guilds[kStartupShellGuildCapacity];
    StartupShellChannel channels[kStartupShellChannelCapacity];
    StartupShellConversation conversations[kStartupShellConversationCapacity];
    u64 recipients[kStartupShellRecipientCapacity];
    StartupShellUser users[kStartupShellUserCapacity];
    StartupShellAuthor authors[kStartupShellAuthorCapacity];
    // Open rail folders and the rail layout (an encoded GuildFolders message).
    u64 expanded_folders[kStartupShellExpandedCapacity];
    u8 folders[kStartupShellFolderBytes];
    char text[kStartupShellTextBytes];
};

Utf8View startup_shell_text(const StartupShell& shell, StartupShellText text) noexcept;
// Appends text to the bank; false (and an empty reference) when it does not fit.
bool startup_shell_add_text(StartupShell* shell, Utf8View text, StartupShellText* output) noexcept;

// The plaintext body; 0 when output is too small.
u32 startup_shell_encode(const StartupShell& shell, Slice<u8> output) noexcept;
// Strict: a short, oversized or inconsistent body, or invalid UTF-8, is
// rejected whole and leaves the shell unspecified.
bool startup_shell_decode(ConstSlice<u8> input, StartupShell* shell) noexcept;

// Per-account key: a random AES-256-GCM key kept DPAPI-wrapped (user-bound)
// in every file, unwrapped once per process into locked memory. Zero
// initialize; one thread at a time; release explicitly.
struct StartupShellKey {
    AesGcmKey aead;
    u64 profile_id;
    u32 wrapped_bytes;
    u8 wrapped[kStartupShellWrappedKeyBytes];
};
bool startup_shell_key_create(StartupShellKey* key, u64 profile_id) noexcept;
void startup_shell_key_release(StartupShellKey* key) noexcept;

// File image: header (magic, version, profile id, sizes), the wrapped key,
// a fresh nonce, the ciphertext and its tag; the header, key and nonce are
// authenticated data. Returns the image bytes, 0 on failure.
u32 startup_shell_seal(const StartupShellKey& key, ConstSlice<u8> plaintext, Slice<u8> output) noexcept;

enum class StartupShellOpenError : u8 {
    none,
    format,    // short, oversized or malformed image
    version,
    account,   // saved for another account
    key,       // the wrapped key does not unwrap for this user
    tamper,    // authentication failed
    malformed, // the plaintext does not decode
    expired,   // older than kStartupShellMaximumAgeMs (or from the future)
};
// Opens an image saved for profile_id. A live key for that profile decrypts
// directly; otherwise the image's key is unwrapped into *key, which then
// stays live for later seals. plaintext is caller scratch, erased on return.
StartupShellOpenError startup_shell_open(ConstSlice<u8> image, u64 profile_id, u64 now_unix_ms,
                                         StartupShellKey* key, Slice<u8> plaintext,
                                         StartupShell* shell) noexcept;

// accounts\[slot-N\]active.shell. A missing file is StartupShellOpenError::format.
StartupShellOpenError startup_shell_load_file(const wchar_t* path, u64 profile_id, u64 now_unix_ms,
                                              StartupShellKey* key, StartupShell* shell) noexcept;
// Encodes, seals and atomically replaces the file; creates the key on first use.
bool startup_shell_save_file(const wchar_t* path, StartupShellKey* key, const StartupShell& shell) noexcept;

// The stores READY populates.
struct StartupShellStores {
    ChatStateStore* state;
    UserStore* users;
    GuildMemberStore* members;
};
// Records the live stores: every guild, the selected server's visible
// channels or (on Home) the newest conversations, the account and the
// authors of the newest resident messages of the selected channel. The
// caller sets profile_id, saved_unix_ms, the folders and expanded_folders.
void startup_shell_capture(const StartupShellStores& stores, const MessageStore* messages,
                           StartupShell* shell) noexcept;
// Fills empty stores as READY would, before READY: guilds, channels with
// their saved permissions, conversations, users, author display and the
// account, then selects the saved channel (or the saved server, pending).
// True when the selection was placed.
bool startup_shell_apply(const StartupShell& shell, const StartupShellStores& stores) noexcept;

} // namespace maicord
