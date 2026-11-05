// secure_buffer.hpp on Linux.
//
// Storage: private anonymous pages, excluded from core dumps (MADV_DONTDUMP) and from a forked child
// (MADV_WIPEONFORK), locked with mlock. RLIMIT_MEMLOCK is 8 MiB on current kernels and as little as 64 KiB on old
// ones, so a refused mlock does not fail the buffer (D19): `locked` means "secure storage ready", the failure is
// logged once and the pages keep the other two protections. The Windows working-set recovery and its named budget
// mutex have no counterpart here: nothing else shares an mlock quota with this process.
//
// User-bound protection: there is no DPAPI. A blob is sealed with XChaCha20-Poly1305 under a 32-byte master key. The
// key lives in the freedesktop Secret Service (GNOME Keyring, KWallet, KeePassXC) when one is running and unlocked
// (attributes application=maicord, purpose=master-key); an earlier 0600 key file is moved into it and deleted. Without a
// usable Secret Service (none running, a locked keyring that would need a prompt, MAICORD_SECRET_SERVICE=off) the key is
// kept in a file of the data directory with mode 0600 (written once, atomically, next to the secrets it protects), and
// the first use logs that. The file protects against another user and against a backup or a copied data directory that
// leaves the key file behind, and nothing else: unlike DPAPI the key is not bound to a login session, so any process of
// the same user can read it. Blob layout:
//     0x01 | nonce (24) | ciphertext (the plaintext's length) | tag (16)
// with the version byte followed by the caller's `entropy` as the authenticated data, so a blob opens only for the
// entropy it was sealed with, as with DPAPI.
#include "maicord/platform/crypto/secure_buffer.hpp"
#include "maicord/platform/crypto/random.hpp"
#include "maicord/platform/crypto/xchacha20_poly1305.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/dbus/secret_service.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/path.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord {
namespace {

// Linux x86-64 system calls and constants this file needs beyond sys/linux/calls.hpp.
constexpr long kCallFsync = 74;
constexpr long kCallFchmod = 91;
constexpr long kCallLinkat = 265;
constexpr long kCallRenameat2 = 316;
constexpr int kAdviseDontDump = 16;
constexpr int kAdviseWipeOnFork = 18;
constexpr unsigned kRenameNoReplace = 1;
constexpr u32 kErrorInvalidData = 13; // ERROR_INVALID_DATA, the Win32 number the callers log for a malformed blob

constexpr u8 kBlobVersion = 1;
constexpr u32 kNonceBytes = 24;
constexpr u32 kTagBytes = 16;
constexpr u32 kBlobOverheadBytes = 1 + kNonceBytes + kTagBytes;
constexpr u32 kMaximumEntropyBytes = 512;
constexpr u32 kKeyFileBytes = 4 + 32;
constexpr char kKeyFileMagic[4] = {'M', 'C', 'K', '1'};
constexpr u32 kPathCapacity = 1024;

SecureBufferResult success(u32 bytes = 0) noexcept {
    return SecureBufferResult{bytes, 0, SecureBufferError::none};
}

SecureBufferResult failure(SecureBufferError error, u32 native_error = 0, u32 bytes = 0) noexcept {
    return SecureBufferResult{bytes, native_error, error};
}

u32 error_number(long result) noexcept {
    return static_cast<u32>(-result);
}

volatile u32 g_mlock_refusal_logged;
volatile u32 g_key_file_noted;

// The first refused mlock is logged; the pages stay usable.
void note_unlocked(long result) noexcept {
    if (__atomic_exchange_n(&g_mlock_refusal_logged, 1u, __ATOMIC_RELAXED) == 0) {
        log_u64(LogLevel::warning, "secure_buffer", "mlock refused, errno", error_number(result));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The master key file.

// Where the key lives, as UTF-8; `create` also creates the directories above it.
bool key_path(char (&path)[kPathCapacity], u32* native_error, bool create) noexcept {
    wchar_t wide[kPathCapacity];
    const DataPathResult located = data_path(wide, kPathCapacity, L"account-secret.key", create);
    if (!located.ok()) {
        *native_error = located.native_error ? located.native_error : static_cast<u32>(kErrorInvalid);
        return false;
    }
    if (path_to_utf8(wide, path, kPathCapacity) == 0) {
        *native_error = static_cast<u32>(kErrorInvalid);
        return false;
    }
    return true;
}

// Reads the key from an open file; the file holds exactly the magic and the 32 key bytes.
bool read_key(int file, u8 (&key)[32]) noexcept {
    u8 bytes[kKeyFileBytes + 1];
    u32 done = 0;
    for (;;) {
        const long got = sys_read(file, bytes + done, sizeof(bytes) - done);
        if (got == -kErrorInterrupted)
            continue;
        if (syscall_failed(got) || got == 0)
            break;
        done += static_cast<u32>(got);
        if (done == sizeof(bytes))
            break;
    }
    const bool valid = done == kKeyFileBytes && bytes_equal(bytes, kKeyFileMagic, 4);
    if (valid)
        copy_bytes(key, bytes + 4, 32);
    secure_clear_bytes(bytes, sizeof(bytes));
    return valid;
}

// Writes the new key file without a window in which a reader sees a partial one: a private temporary file, then a
// link (or a no-replace rename) to the final name, which fails when another process created the key first.
// Returns 0 when this call installed `key`, kErrorExists when another file is already there, or an errno.
long install_key(const char* path, const u8 (&key)[32]) noexcept {
    char temporary[kPathCapacity + 24];
    u32 length = 0;
    while (path[length] && length < kPathCapacity)
        temporary[length] = path[length], ++length;
    const char suffix[] = ".tmp.";
    copy_bytes(temporary + length, suffix, sizeof(suffix) - 1);
    length += sizeof(suffix) - 1;
    char digits[12];
    u32 digit_count = 0;
    for (u32 pid = process_id(); pid || digit_count == 0; pid /= 10)
        digits[digit_count++] = static_cast<char>('0' + pid % 10);
    while (digit_count)
        temporary[length++] = digits[--digit_count];
    temporary[length] = 0;

    const long opened = sys_openat(kAtCurrentDirectory, temporary,
                                   kOpenWriteOnly | kOpenCreate | kOpenExclusive | kOpenNoFollow | kOpenCloseOnExec,
                                   0600);
    if (syscall_failed(opened))
        return -opened;
    const int file = static_cast<int>(opened);
    // The umask could only narrow the mode, but a fixed mode is not left to it.
    syscall2(kCallFchmod, file, 0600);
    u8 bytes[kKeyFileBytes];
    copy_bytes(bytes, kKeyFileMagic, 4);
    copy_bytes(bytes + 4, key, 32);
    u32 done = 0;
    long error = 0;
    while (done != sizeof(bytes) && !error) {
        const long wrote = sys_write(file, bytes + done, sizeof(bytes) - done);
        if (wrote == -kErrorInterrupted)
            continue;
        if (syscall_failed(wrote) || wrote == 0)
            error = syscall_failed(wrote) ? -wrote : kErrorAgain;
        else
            done += static_cast<u32>(wrote);
    }
    secure_clear_bytes(bytes, sizeof(bytes));
    if (!error) {
        const long synced = syscall1(kCallFsync, file);
        if (syscall_failed(synced))
            error = -synced;
    }
    sys_close(file);
    if (!error) {
        long linked = syscall5(kCallLinkat, kAtCurrentDirectory, reinterpret_cast<long>(temporary), kAtCurrentDirectory,
                               reinterpret_cast<long>(path), 0);
        if (syscall_failed(linked) && -linked != kErrorExists) {
            // No hard links here (some network and mount filesystems): rename without replacing.
            linked = syscall5(kCallRenameat2, kAtCurrentDirectory, reinterpret_cast<long>(temporary),
                              kAtCurrentDirectory, reinterpret_cast<long>(path), kRenameNoReplace);
            if (!syscall_failed(linked))
                return 0;
        }
        error = syscall_failed(linked) ? -linked : 0;
    }
    sys_unlink(temporary);
    return error;
}

// The master key from the key file; with `create` a missing one is made (protecting a secret), without it a missing one is
// an error (opening one, which cannot succeed under a key that did not exist). *native_error receives the errno of a
// failure.
bool file_master_key(u8 (&key)[32], u32* native_error, bool create) noexcept {
    char path[kPathCapacity];
    if (!key_path(path, native_error, create))
        return false;
    if (__atomic_exchange_n(&g_key_file_noted, 1u, __ATOMIC_RELAXED) == 0) {
        log_write(LogLevel::info, "secure_buffer",
                  "user-bound secrets use a 0600 key file in the data directory: no usable Secret Service keyring");
    }
    for (u32 attempt = 0; attempt != 3; ++attempt) {
        const long opened = sys_openat(kAtCurrentDirectory, path, kOpenReadOnly | kOpenNoFollow | kOpenCloseOnExec);
        if (!syscall_failed(opened)) {
            const bool valid = read_key(static_cast<int>(opened), key);
            sys_close(static_cast<int>(opened));
            if (valid)
                return true;
            *native_error = kErrorInvalidData;
            return false;
        }
        if (-opened != kErrorNoEntry || !create) {
            *native_error = error_number(opened);
            return false;
        }
        const auto random = secure_random_fill({key, 32});
        if (!random.ok()) {
            *native_error = static_cast<u32>(-random.platform_status);
            return false;
        }
        const long installed = install_key(path, key);
        if (installed == 0) {
            log_write(LogLevel::info, "secure_buffer", "created the user-bound secrets key file");
            return true;
        }
        if (installed != kErrorExists) {
            secure_clear_bytes(key, 32);
            *native_error = static_cast<u32>(installed);
            return false;
        }
        // Another process won the race: read its key.
    }
    secure_clear_bytes(key, 32);
    *native_error = static_cast<u32>(kErrorAgain);
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// The Secret Service.

constexpr const char* kServiceKeyLabel = "MaiCord master key";
volatile u32 g_service_lock;
volatile u32 g_service_noted;

// MAICORD_SECRET_SERVICE=off keeps the key in the file (the tests, and a user who wants no keyring).
bool service_enabled() noexcept {
    const char* value = environment_get("MAICORD_SECRET_SERVICE");
    return !(value && value[0] == 'o' && value[1] == 'f' && value[2] == 'f' && value[3] == 0);
}

enum class ServiceKey : u8 {
    used,     // `key` holds the master key from the Secret Service
    none,     // the service cannot be used: the key file is the store
    missing,  // the service is there and has no key, and opening a blob cannot make one
};

void note_service(const char* message) noexcept {
    if (__atomic_exchange_n(&g_service_noted, 1u, __ATOMIC_RELAXED) == 0)
        log_write(LogLevel::info, "secure_buffer", message);
}

const dbus::SecretAttribute kServiceAttributes[2] = {{"application", "maicord"}, {"purpose", "master-key"}};

// Looks the key up in the Secret Service; the first-ever use stores one (a key file of the earlier version moves in
// first, so blobs sealed under it stay readable). One thread at a time, so two do not both make a key.
ServiceKey service_master_key(u8 (&key)[32], u32* native_error, bool create) noexcept {
    if (!service_enabled())
        return ServiceKey::none;
    u32 length = 0;
    const dbus::SecretStatus looked = dbus::secret_lookup(kServiceAttributes, 2, key, 32, &length);
    if (looked == dbus::SecretStatus::ok && length == 32) {
        note_service("user-bound secrets use a master key in the Secret Service");
        return ServiceKey::used;
    }
    if (looked == dbus::SecretStatus::ok) {
        secure_clear_bytes(key, 32);
        log_u64(LogLevel::warning, "secure_buffer", "the Secret Service's master key has an unexpected size", length);
        return ServiceKey::none;
    }
    if (looked != dbus::SecretStatus::not_found) {
        secure_clear_bytes(key, 32);
        if (looked != dbus::SecretStatus::unavailable)
            note_service("the Secret Service cannot be used (locked or failing): the key file is used");
        return ServiceKey::none;
    }

    // The service is there and holds no key. An earlier key file moves in.
    char path[kPathCapacity];
    u32 ignored = 0;
    if (key_path(path, &ignored, false)) {
        const long opened = sys_openat(kAtCurrentDirectory, path, kOpenReadOnly | kOpenNoFollow | kOpenCloseOnExec);
        if (!syscall_failed(opened)) {
            const bool valid = read_key(static_cast<int>(opened), key);
            sys_close(static_cast<int>(opened));
            if (!valid)
                return ServiceKey::none;  // the file path reports it
            if (dbus::secret_store(kServiceKeyLabel, kServiceAttributes, 2, key, 32) != dbus::SecretStatus::ok) {
                secure_clear_bytes(key, 32);
                return ServiceKey::none;  // the service refuses: the file stays the key
            }
            if (syscall_failed(sys_unlink(path)))
                log_write(LogLevel::warning, "secure_buffer", "the key file could not be removed after the move");
            log_write(LogLevel::info, "secure_buffer", "moved the master key from the key file into the Secret Service");
            return ServiceKey::used;
        }
    }
    if (!create) {
        *native_error = static_cast<u32>(kErrorNoEntry);
        return ServiceKey::missing;
    }
    const auto random = secure_random_fill({key, 32});
    if (!random.ok()) {
        *native_error = static_cast<u32>(-random.platform_status);
        secure_clear_bytes(key, 32);
        return ServiceKey::missing;
    }
    if (dbus::secret_store(kServiceKeyLabel, kServiceAttributes, 2, key, 32) != dbus::SecretStatus::ok) {
        secure_clear_bytes(key, 32);
        return ServiceKey::none;
    }
    // What the service holds is the key, whichever of two first launches stored last.
    u8 stored[32];
    if (dbus::secret_lookup(kServiceAttributes, 2, stored, 32, &length) == dbus::SecretStatus::ok && length == 32)
        copy_bytes(key, stored, 32);
    secure_clear_bytes(stored, sizeof(stored));
    log_write(LogLevel::info, "secure_buffer", "created the master key in the Secret Service");
    return ServiceKey::used;
}

// The master key: the Secret Service's when there is one, else the key file's.
bool master_key(u8 (&key)[32], u32* native_error, bool create) noexcept {
    while (__atomic_exchange_n(&g_service_lock, 1u, __ATOMIC_ACQUIRE))
        syscall1(24 /* sched_yield */, 0);
    const ServiceKey service = service_master_key(key, native_error, create);
    __atomic_store_n(&g_service_lock, 0u, __ATOMIC_RELEASE);
    if (service == ServiceKey::used)
        return true;
    if (service == ServiceKey::missing)
        return false;
    return file_master_key(key, native_error, create);
}

// The version byte and the entropy as one authenticated-data slice.
struct Binding {
    u8 bytes[1 + kMaximumEntropyBytes];
    u32 count;
};

bool bind(ConstSlice<u8> entropy, Binding* binding) noexcept {
    if (entropy.count > kMaximumEntropyBytes || (!entropy.data && entropy.count != 0))
        return false;
    binding->bytes[0] = kBlobVersion;
    copy_bytes(binding->bytes + 1, entropy.data, entropy.count);
    binding->count = 1 + entropy.count;
    return true;
}

// An XChaCha key for the master key, wiped by the owner's release.
bool open_cipher(XChaChaKey* cipher, u32* native_error, bool create) noexcept {
    u8 key[32];
    if (!master_key(key, native_error, create))
        return false;
    const AeadResult initialized = xchacha_initialize(cipher, {key, 32});
    secure_clear_bytes(key, sizeof(key));
    if (!initialized.ok()) {
        *native_error = static_cast<u32>(initialized.native_status);
        return false;
    }
    return true;
}

} // namespace

SecureBufferResult secure_buffer_initialize(SecureBuffer* buffer, u32 capacity) noexcept {
    if (!buffer || capacity == 0) {
        return failure(SecureBufferError::invalid_input, static_cast<u32>(kErrorInvalid));
    }
    reset_value<SecureBuffer>(buffer);
    auto* allocation = static_cast<u8*>(commit_pages(capacity));
    if (!allocation) {
        return failure(SecureBufferError::allocation_failed, static_cast<u32>(kErrorNoMemory));
    }
    // Best effort: a refused DONTDUMP or WIPEONFORK only widens a core dump or the key's reach into a child.
    sys_madvise(allocation, capacity, kAdviseDontDump);
    sys_madvise(allocation, capacity, kAdviseWipeOnFork);
    const long locked = sys_mlock(allocation, capacity);
    if (syscall_failed(locked)) {
        note_unlocked(locked);
    }
    buffer->data = allocation;
    buffer->capacity = capacity;
    buffer->locked = true;
    return success();
}

SecureBufferResult secure_buffer_assign(SecureBuffer* buffer, ConstSlice<u8> value) noexcept {
    if (!buffer || !buffer->data || !buffer->locked || (!value.data && value.count != 0)) {
        return failure(SecureBufferError::invalid_input, static_cast<u32>(kErrorInvalid));
    }
    if (value.count > buffer->capacity) {
        return failure(SecureBufferError::output_too_small, 0, value.count);
    }
    secure_clear_bytes(buffer->data, buffer->capacity);
    copy_bytes(buffer->data, value.data, value.count);
    buffer->bytes = value.count;
    return success(value.count);
}

bool secure_buffer_transfer(SecureBuffer* destination, SecureBuffer* source) noexcept {
    if (!destination || !source || destination == source || destination->data || destination->locked ||
        !source->data || !source->locked || source->capacity == 0 || source->bytes == 0) {
        return false;
    }
    *destination = *source;
    reset_value<SecureBuffer>(source);
    return true;
}

void secure_buffer_clear(SecureBuffer* buffer) noexcept {
    if (buffer && buffer->data) {
        secure_clear_bytes(buffer->data, buffer->capacity);
        buffer->bytes = 0;
    }
}

void secure_buffer_release(SecureBuffer* buffer) noexcept {
    if (!buffer) {
        return;
    }
    if (buffer->data) {
        secure_clear_bytes(buffer->data, buffer->capacity);
        // munlock of pages that were never locked is harmless, so the outcome of the lock is not tracked.
        sys_munlock(buffer->data, buffer->capacity);
        pages_release(buffer->data, buffer->capacity);
    }
    reset_value<SecureBuffer>(buffer);
}

ConstSlice<u8> secure_buffer_view(const SecureBuffer& buffer) noexcept {
    return ConstSlice<u8>{buffer.data, buffer.bytes};
}

SecureBufferResult secure_buffer_protect_user_bound(const SecureBuffer& plaintext, ConstSlice<u8> entropy,
                                                    Slice<u8> protected_output) noexcept {
    if (!plaintext.data || !plaintext.locked || plaintext.bytes == 0 ||
        (!protected_output.data && protected_output.count != 0) || (!entropy.data && entropy.count != 0)) {
        return failure(SecureBufferError::invalid_input, static_cast<u32>(kErrorInvalid));
    }
    Binding binding;
    if (!bind(entropy, &binding) || plaintext.bytes > ~u32{0} - kBlobOverheadBytes) {
        return failure(SecureBufferError::invalid_input, static_cast<u32>(kErrorInvalid));
    }
    const u32 required = plaintext.bytes + kBlobOverheadBytes;
    if (required > protected_output.count) {
        return failure(SecureBufferError::output_too_small, 0, required);
    }
    u32 native_error = 0;
    XChaChaKey cipher{};
    if (!open_cipher(&cipher, &native_error, true)) {
        return failure(SecureBufferError::protect_failed, native_error);
    }
    u8* out = protected_output.data;
    u8* nonce = out + 1;
    u8* ciphertext = nonce + kNonceBytes;
    u8* tag = ciphertext + plaintext.bytes;
    SecureBufferResult result = success(required);
    const auto random = secure_random_fill({nonce, kNonceBytes});
    if (!random.ok()) {
        result = failure(SecureBufferError::protect_failed, static_cast<u32>(-random.platform_status));
    } else {
        const AeadResult sealed =
            xchacha_encrypt(cipher, {nonce, kNonceBytes}, {binding.bytes, binding.count},
                            {plaintext.data, plaintext.bytes}, {ciphertext, plaintext.bytes}, {tag, kTagBytes});
        if (sealed.ok()) {
            out[0] = kBlobVersion;
        } else {
            result = failure(SecureBufferError::protect_failed, static_cast<u32>(sealed.native_status));
        }
    }
    xchacha_release(&cipher);
    if (!result.ok()) {
        secure_clear_bytes(out, required);
    }
    return result;
}

SecureBufferResult secure_buffer_protect_user_bound_block(const SecureBuffer& plaintext, ConstSlice<u8> entropy,
                                                          u32 header_bytes, u32 maximum_bytes, u8** block,
                                                          u32* block_bytes) noexcept {
    if (!block || !block_bytes || header_bytes == 0 || header_bytes >= maximum_bytes) {
        return failure(SecureBufferError::invalid_input, static_cast<u32>(kErrorInvalid));
    }
    *block = nullptr;
    *block_bytes = 0;
    if (!plaintext.data || !plaintext.locked || plaintext.bytes == 0) {
        return failure(SecureBufferError::invalid_input, static_cast<u32>(kErrorInvalid));
    }
    // The blob's size is known exactly, unlike a DPAPI blob's: one allocation, one call.
    if (plaintext.bytes > ~u32{0} - kBlobOverheadBytes ||
        plaintext.bytes + kBlobOverheadBytes > maximum_bytes - header_bytes) {
        return failure(SecureBufferError::output_too_small, 0, plaintext.bytes + kBlobOverheadBytes);
    }
    const u32 capacity = plaintext.bytes + kBlobOverheadBytes;
    const u32 total = header_bytes + capacity;
    auto* bytes = static_cast<u8*>(commit_pages(total));
    if (!bytes) {
        return failure(SecureBufferError::allocation_failed, static_cast<u32>(kErrorNoMemory));
    }
    const SecureBufferResult result =
        secure_buffer_protect_user_bound(plaintext, entropy, Slice<u8>{bytes + header_bytes, capacity});
    if (result.ok() && result.bytes != 0) {
        *block = bytes;
        *block_bytes = total;
        return result;
    }
    secure_clear_bytes(bytes, total);
    pages_release(bytes, total);
    return result.ok() ? failure(SecureBufferError::protect_failed) : result;
}

SecureBufferResult secure_buffer_unprotect_user_bound(ConstSlice<u8> protected_input, ConstSlice<u8> entropy,
                                                      SecureBuffer* plaintext) noexcept {
    if (!protected_input.data || protected_input.count == 0 || !plaintext || !plaintext->data ||
        !plaintext->locked || (!entropy.data && entropy.count != 0)) {
        return failure(SecureBufferError::invalid_input, static_cast<u32>(kErrorInvalid));
    }
    Binding binding;
    if (!bind(entropy, &binding)) {
        return failure(SecureBufferError::invalid_input, static_cast<u32>(kErrorInvalid));
    }
    if (protected_input.count <= kBlobOverheadBytes || protected_input.data[0] != kBlobVersion) {
        return failure(SecureBufferError::unprotect_failed, kErrorInvalidData);
    }
    const u32 plain_bytes = protected_input.count - kBlobOverheadBytes;
    const u8* nonce = protected_input.data + 1;
    const u8* ciphertext = nonce + kNonceBytes;
    const u8* tag = ciphertext + plain_bytes;
    u32 native_error = 0;
    XChaChaKey cipher{};
    if (!open_cipher(&cipher, &native_error, false)) {
        return failure(SecureBufferError::unprotect_failed, native_error);
    }
    // Decrypted in a buffer of its own, so a wrong key, entropy or tampered blob leaves the caller's buffer as it
    // was, as DPAPI's separate output does.
    SecureBuffer opened{};
    SecureBufferResult result = secure_buffer_initialize(&opened, plain_bytes);
    if (result.ok()) {
        const AeadResult unsealed =
            xchacha_decrypt(cipher, {nonce, kNonceBytes}, {binding.bytes, binding.count}, {ciphertext, plain_bytes},
                            {tag, kTagBytes}, {opened.data, plain_bytes});
        if (unsealed.ok()) {
            opened.bytes = plain_bytes;
            result = secure_buffer_assign(plaintext, secure_buffer_view(opened));
        } else {
            result = failure(SecureBufferError::unprotect_failed,
                             unsealed.error == AeadError::authentication_failed ? kErrorInvalidData
                                                                                : static_cast<u32>(unsealed.native_status));
        }
    } else {
        result = failure(SecureBufferError::lock_failed, result.native_error);
    }
    secure_buffer_release(&opened);
    xchacha_release(&cipher);
    return result;
}

} // namespace maicord
