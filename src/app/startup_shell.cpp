#include "maicord/app/startup_shell.hpp"

#include "maicord/app/account_profile.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/platform/random.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

constexpr u8 kMagic[4] = {'M', 'C', 'S', 'H'};
constexpr u32 kHeaderBytes = 24;
constexpr u32 kKeyBytes = 32;
constexpr u32 kTextMaximumBytes = 400;
// Clock skew a saved shell may show against this machine's clock.
constexpr u64 kFutureSkewMs = 24ull * 60 * 60 * 1000;

// The body is the shell's header and counts, then each record array's used
// prefix, all in their in-memory (x64) layout: records hold no pointers.
struct Section {
    void* data;
    u32 element, count, capacity;
};
constexpr u32 kSections = 9;
void sections(StartupShell& s, Section* out) noexcept {
    out[0] = {s.guilds, sizeof(StartupShellGuild), s.guild_count, kStartupShellGuildCapacity};
    out[1] = {s.channels, sizeof(StartupShellChannel), s.channel_count, kStartupShellChannelCapacity};
    out[2] = {s.conversations, sizeof(StartupShellConversation), s.conversation_count,
              kStartupShellConversationCapacity};
    out[3] = {s.recipients, sizeof(u64), s.recipient_count, kStartupShellRecipientCapacity};
    out[4] = {s.users, sizeof(StartupShellUser), s.user_count, kStartupShellUserCapacity};
    out[5] = {s.authors, sizeof(StartupShellAuthor), s.author_count, kStartupShellAuthorCapacity};
    out[6] = {s.expanded_folders, sizeof(u64), s.expanded_count, kStartupShellExpandedCapacity};
    out[7] = {s.folders, 1, s.folder_bytes, kStartupShellFolderBytes};
    out[8] = {s.text, 1, s.text_bytes, kStartupShellTextBytes};
}
// Header and counts: everything before the first array.
u32 prefix_bytes(const StartupShell& s) noexcept {
    return static_cast<u32>(reinterpret_cast<const u8*>(s.guilds) - reinterpret_cast<const u8*>(&s));
}
// The body size, or 0 when a count exceeds its capacity.
u32 body_bytes(const Section* list, u32 prefix) noexcept {
    u32 total = prefix;
    for (u32 i = 0; i < kSections; ++i) {
        if (list[i].count > list[i].capacity)
            return 0;
        total += list[i].count * list[i].element;
    }
    return total;
}

bool text_valid(const StartupShell& shell, StartupShellText text) noexcept {
    return text.bytes <= kTextMaximumBytes && text.offset <= shell.text_bytes &&
           text.bytes <= shell.text_bytes - text.offset &&
           utf8_validate({shell.text + text.offset, text.bytes}).valid;
}

void make_entropy(u64 profile_id, u8* output) noexcept {
    constexpr u8 label[12] = {'M', 'a', 'i', 'C', 'o', 'r', 'd', 'S', 'h', 'e', 'l', 'l'};
    copy_bytes(output, label, sizeof(label));
    byte_store_le64(output + 12, profile_id);
    byte_store_le32(output + 20, kStartupShellVersion);
}

u8* allocate(u32 bytes) noexcept {
    return static_cast<u8*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}

} // namespace

Utf8View startup_shell_text(const StartupShell& shell, StartupShellText text) noexcept {
    return text.offset <= shell.text_bytes && text.bytes <= shell.text_bytes - text.offset
               ? Utf8View{shell.text + text.offset, text.bytes}
               : Utf8View{};
}

bool startup_shell_add_text(StartupShell* shell, Utf8View text, StartupShellText* output) noexcept {
    *output = {};
    if (!text.bytes)
        return true;
    if (!text.data || text.bytes > kTextMaximumBytes || text.bytes > kStartupShellTextBytes - shell->text_bytes)
        return false;
    copy_bytes(shell->text + shell->text_bytes, text.data, text.bytes);
    *output = {shell->text_bytes, static_cast<u16>(text.bytes)};
    shell->text_bytes += text.bytes;
    return true;
}

u32 startup_shell_encode(const StartupShell& shell, Slice<u8> output) noexcept {
    Section list[kSections];
    sections(const_cast<StartupShell&>(shell), list);
    const u32 prefix = prefix_bytes(shell);
    const u32 total = body_bytes(list, prefix);
    if (!output.data || !total || total > output.count || total > kStartupShellPlaintextBytes)
        return 0;
    copy_bytes(output.data, &shell, prefix);
    u32 used = prefix;
    for (const Section& section : list) {
        copy_bytes(output.data + used, section.data, section.count * section.element);
        used += section.count * section.element;
    }
    return used;
}

bool startup_shell_decode(ConstSlice<u8> input, StartupShell* shell) noexcept {
    if (!shell || !input.data)
        return false;
    StartupShell& s = *shell;
    const u32 prefix = prefix_bytes(s);
    if (input.count < prefix || input.count > kStartupShellPlaintextBytes)
        return false;
    copy_bytes(&s, input.data, prefix);
    Section list[kSections];
    sections(s, list);
    if (body_bytes(list, prefix) != input.count || !s.profile_id || !s.self_user_id || !s.saved_unix_ms)
        return false;
    u32 used = prefix;
    for (const Section& section : list) {
        copy_bytes(section.data, input.data + used, section.count * section.element);
        used += section.count * section.element;
    }
    // Every ID is set, every reference lies in the bank as valid UTF-8.
    bool valid = text_valid(s, s.status_text) && text_valid(s, s.status_emoji);
    for (u32 i = 0; valid && i < s.guild_count; ++i) {
        const auto& g = s.guilds[i];
        valid = g.id && text_valid(s, g.name) && text_valid(s, g.icon) && text_valid(s, g.banner);
    }
    for (u32 i = 0; valid && i < s.channel_count; ++i) {
        const auto& c = s.channels[i];
        valid = c.id && c.parent_id != c.id && c.type < 32 && text_valid(s, c.name);
    }
    for (u32 i = 0; valid && i < s.conversation_count; ++i) {
        const auto& c = s.conversations[i];
        valid = c.id && (c.type == 1 || c.type == 3) && c.first_recipient <= s.recipient_count &&
                c.recipient_count <= s.recipient_count - c.first_recipient && text_valid(s, c.name) &&
                text_valid(s, c.icon);
    }
    for (u32 i = 0; valid && i < s.recipient_count; ++i)
        valid = s.recipients[i] != 0;
    for (u32 i = 0; valid && i < s.user_count; ++i) {
        const auto& u = s.users[i];
        valid = u.id && text_valid(s, u.name) && text_valid(s, u.username) && text_valid(s, u.avatar);
    }
    for (u32 i = 0; valid && i < s.author_count; ++i)
        valid = s.authors[i].user_id && text_valid(s, s.authors[i].nickname);
    return valid;
}

bool startup_shell_key_create(StartupShellKey* key, u64 profile_id) noexcept {
    if (!key || key->aead.handle || !profile_id)
        return false;
    SecureBuffer secret{};
    bool ok = secure_buffer_initialize(&secret, kKeyBytes).ok() &&
              secure_random_fill({secret.data, kKeyBytes}).ok();
    if (ok)
        secret.bytes = kKeyBytes;
    u8 entropy[24];
    make_entropy(profile_id, entropy);
    const SecureBufferResult wrapped =
        ok ? secure_buffer_protect_user_bound(secret, {entropy, sizeof(entropy)},
                                              {key->wrapped, kStartupShellWrappedKeyBytes})
           : SecureBufferResult{};
    ok = ok && wrapped.ok() && wrapped.bytes && aes_gcm_initialize(&key->aead, secure_buffer_view(secret)).ok();
    secure_buffer_release(&secret);
    if (!ok) {
        startup_shell_key_release(key);
        return false;
    }
    key->wrapped_bytes = wrapped.bytes;
    key->profile_id = profile_id;
    return true;
}

void startup_shell_key_release(StartupShellKey* key) noexcept {
    if (!key)
        return;
    aes_gcm_release(&key->aead);
    secure_clear_bytes(key, sizeof(*key));
}

u32 startup_shell_seal(const StartupShellKey& key, ConstSlice<u8> plaintext, Slice<u8> output) noexcept {
    const u32 wrapped = key.wrapped_bytes;
    if (!key.aead.handle || !key.profile_id || !wrapped || wrapped > kStartupShellWrappedKeyBytes ||
        !plaintext.data || !plaintext.count || plaintext.count > kStartupShellPlaintextBytes || !output.data)
        return 0;
    const u32 aad_bytes = kHeaderBytes + wrapped + kAesGcmNonceBytes;
    const u32 total = aad_bytes + plaintext.count + kAesGcmTagBytes;
    if (output.count < total)
        return 0;
    u8* header = output.data;
    copy_bytes(header, kMagic, 4);
    byte_store_le32(header + 4, kStartupShellVersion);
    byte_store_le64(header + 8, key.profile_id);
    byte_store_le32(header + 16, wrapped);
    byte_store_le32(header + 20, plaintext.count);
    copy_bytes(header + kHeaderBytes, key.wrapped, wrapped);
    u8* nonce = header + kHeaderBytes + wrapped;
    if (!secure_random_fill({nonce, kAesGcmNonceBytes}).ok())
        return 0;
    const auto sealed = aes_gcm_encrypt(key.aead, {nonce, kAesGcmNonceBytes}, {header, aad_bytes}, plaintext,
                                        {header + aad_bytes, plaintext.count},
                                        {header + aad_bytes + plaintext.count, kAesGcmTagBytes});
    return sealed.ok() ? total : 0;
}

StartupShellOpenError startup_shell_open(ConstSlice<u8> image, u64 profile_id, u64 now_unix_ms,
                                         StartupShellKey* key, Slice<u8> plaintext,
                                         StartupShell* shell) noexcept {
    using E = StartupShellOpenError;
    if (!key || !shell || !plaintext.data || !image.data || image.count < kHeaderBytes ||
        !bytes_equal(image.data, kMagic, 4))
        return E::format;
    if (byte_load_le32(image.data + 4) != kStartupShellVersion)
        return E::version;
    if (!profile_id || byte_load_le64(image.data + 8) != profile_id)
        return E::account;
    const u32 wrapped = byte_load_le32(image.data + 16);
    const u32 ciphertext = byte_load_le32(image.data + 20);
    if (!wrapped || wrapped > kStartupShellWrappedKeyBytes || !ciphertext ||
        ciphertext > kStartupShellPlaintextBytes || ciphertext > plaintext.count ||
        image.count != kHeaderBytes + wrapped + kAesGcmNonceBytes + ciphertext + kAesGcmTagBytes)
        return E::format;
    if (!key->aead.handle || key->profile_id != profile_id) {
        startup_shell_key_release(key);
        SecureBuffer secret{};
        u8 entropy[24];
        make_entropy(profile_id, entropy);
        const bool unwrapped =
            secure_buffer_initialize(&secret, kKeyBytes).ok() &&
            secure_buffer_unprotect_user_bound({image.data + kHeaderBytes, wrapped}, {entropy, sizeof(entropy)},
                                               &secret)
                .ok() &&
            secret.bytes == kKeyBytes && aes_gcm_initialize(&key->aead, secure_buffer_view(secret)).ok();
        secure_buffer_release(&secret);
        if (!unwrapped) {
            startup_shell_key_release(key);
            return E::key;
        }
        copy_bytes(key->wrapped, image.data + kHeaderBytes, wrapped);
        key->wrapped_bytes = wrapped;
        key->profile_id = profile_id;
    }
    const u32 aad_bytes = kHeaderBytes + wrapped + kAesGcmNonceBytes;
    const auto opened =
        aes_gcm_decrypt(key->aead, {image.data + kHeaderBytes + wrapped, kAesGcmNonceBytes},
                        {image.data, aad_bytes}, {image.data + aad_bytes, ciphertext},
                        {image.data + aad_bytes + ciphertext, kAesGcmTagBytes}, {plaintext.data, ciphertext});
    E result = E::none;
    if (!opened.ok())
        result = E::tamper;
    else if (!startup_shell_decode({plaintext.data, ciphertext}, shell))
        result = E::malformed;
    else if (shell->profile_id != profile_id)
        result = E::account;
    else if (shell->saved_unix_ms > now_unix_ms + kFutureSkewMs ||
             now_unix_ms - minimum(now_unix_ms, shell->saved_unix_ms) > kStartupShellMaximumAgeMs)
        result = E::expired;
    secure_clear_bytes(plaintext.data, ciphertext);
    return result;
}

StartupShellOpenError startup_shell_load_file(const wchar_t* path, u64 profile_id, u64 now_unix_ms,
                                              StartupShellKey* key, StartupShell* shell) noexcept {
    if (!path || !key || !shell)
        return StartupShellOpenError::format;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return StartupShellOpenError::format;
    Win32LargeInteger size{};
    u32 native_error = 0;
    u8* image = nullptr;
    u32 bytes = 0;
    if (GetFileSizeEx(file, size.native()) && size.QuadPart > 0 && size.QuadPart <= kStartupShellFileBytes) {
        bytes = static_cast<u32>(size.QuadPart);
        image = allocate(bytes);
        if (image && !file_persistence_read_complete(file, {image, bytes}, &native_error)) {
            file_persistence_secure_release(image, bytes);
            image = nullptr;
        }
    }
    CloseHandle(file);
    if (!image)
        return StartupShellOpenError::format;
    u8* plaintext = allocate(kStartupShellPlaintextBytes);
    const StartupShellOpenError result =
        plaintext ? startup_shell_open({image, bytes}, profile_id, now_unix_ms, key,
                                       {plaintext, kStartupShellPlaintextBytes}, shell)
                  : StartupShellOpenError::format;
    file_persistence_secure_release(plaintext, kStartupShellPlaintextBytes);
    file_persistence_secure_release(image, bytes);
    return result;
}

bool startup_shell_save_file(const wchar_t* path, StartupShellKey* key, const StartupShell& shell) noexcept {
    const u32 length = wide_length(path, kAccountProfileMaximumPathCharacters);
    if (!length || !key || !shell.profile_id)
        return false;
    if (!key->aead.handle || key->profile_id != shell.profile_id) {
        startup_shell_key_release(key);
        if (!startup_shell_key_create(key, shell.profile_id))
            return false;
    }
    u8* plaintext = allocate(kStartupShellPlaintextBytes);
    u8* image = allocate(kStartupShellFileBytes);
    const u32 encoded = plaintext ? startup_shell_encode(shell, {plaintext, kStartupShellPlaintextBytes}) : 0;
    const u32 sealed = encoded && image ? startup_shell_seal(*key, {plaintext, encoded}, {image, kStartupShellFileBytes})
                                        : 0;
    const bool saved = sealed && file_persistence_write_atomic(path, length, {image, sealed}).ok();
    file_persistence_secure_release(plaintext, kStartupShellPlaintextBytes);
    file_persistence_secure_release(image, kStartupShellFileBytes);
    return saved;
}

} // namespace maicord
