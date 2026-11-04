// secure_buffer.hpp on Linux (src/platform/crypto/linux/secure_buffer.cpp): the storage and its kernel protections,
// and the user-bound blobs sealed under the 0600 key file. argv[1] selects a case; the registry runs the default one
// with a private XDG_DATA_HOME and the others with the environment they need.
#include "maicord/platform/crypto/secure_buffer.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/path.hpp"
#include "maicord/sys/linux/process.hpp"
#include "../../support/test_output.hpp"

namespace maicord {
namespace {

constexpr u32 kBlobOverhead = 1 + 24 + 16;
constexpr long kCallStat = 4;
constexpr long kCallGetrlimit = 97;
constexpr long kCallSetrlimit = 160;
constexpr long kResourceMemoryLock = 8;

u32 g_failures = 0;

void check(bool condition, const char* name) noexcept {
    if (!condition) {
        ++g_failures;
        test_write_text("FAIL ");
        test_write_line(name);
    }
}

bool text_equal(const char* left, const char* right) noexcept {
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

ConstSlice<u8> text(const char* value) noexcept {
    return {reinterpret_cast<const u8*>(value), c_string_length(value)};
}

bool line_starts_with(const char* line, const char* end, const char* prefix) noexcept {
    for (; *prefix; ++prefix, ++line)
        if (line >= end || *line != *prefix)
            return false;
    return true;
}

// The VmFlags line of the mapping that holds `address` ("rd wr mr mw me ac dd wf ..."), from /proc/self/smaps; false
// when the file cannot be read or the address is not in it.
bool mapping_flags(const void* address, char (&flags)[256]) noexcept {
    const long opened = sys_open("/proc/self/smaps", kOpenReadOnly);
    if (syscall_failed(opened))
        return false;
    static char buffer[1 << 20];
    u32 used = 0;
    while (used < sizeof(buffer) - 1) {
        const long got = sys_read(static_cast<int>(opened), buffer + used, sizeof(buffer) - 1 - used);
        if (syscall_failed(got) || got == 0)
            break;
        used += static_cast<u32>(got);
    }
    sys_close(static_cast<int>(opened));
    const usize target = reinterpret_cast<usize>(address);
    bool inside = false;
    for (u32 at = 0; at < used;) {
        u32 end = at;
        while (end < used && buffer[end] != '\n')
            ++end;
        const char* line = buffer + at;
        const char* line_end = buffer + end;
        const bool hexadecimal = (line[0] >= '0' && line[0] <= '9') || (line[0] >= 'a' && line[0] <= 'f');
        if (hexadecimal) {
            // "start-end perms offset device inode path": the range is hexadecimal on both sides of the dash.
            usize low = 0, high = 0;
            const char* c = line;
            for (; c < line_end && *c != '-'; ++c)
                low = low * 16 + static_cast<usize>(*c <= '9' ? *c - '0' : *c - 'a' + 10);
            for (++c; c < line_end && *c != ' '; ++c)
                high = high * 16 + static_cast<usize>(*c <= '9' ? *c - '0' : *c - 'a' + 10);
            inside = target >= low && target < high;
        } else if (inside && line_starts_with(line, line_end, "VmFlags:")) {
            u32 length = 0;
            for (; length < 255 && line + length < line_end; ++length)
                flags[length] = line[length];
            flags[length] = 0;
            return true;
        }
        at = end + 1;
    }
    return false;
}

bool has_flag(const char* flags, const char* flag) noexcept {
    for (const char* c = flags; *c;) {
        while (*c == ' ')
            ++c;
        const char* token = c;
        while (*c && *c != ' ')
            ++c;
        if (c - token == 2 && token[0] == flag[0] && token[1] == flag[1])
            return true;
    }
    return false;
}

bool contains(ConstSlice<u8> haystack, ConstSlice<u8> needle) noexcept {
    for (u32 i = 0; needle.count <= haystack.count && i <= haystack.count - needle.count; ++i)
        if (bytes_equal(haystack.data + i, needle.data, needle.count))
            return true;
    return false;
}

void storage_cases() noexcept {
    SecureBuffer buffer{};
    check(secure_buffer_initialize(nullptr, 16).error == SecureBufferError::invalid_input &&
              secure_buffer_initialize(&buffer, 0).error == SecureBufferError::invalid_input && !buffer.data,
          "secure buffer rejects a null owner and an empty capacity");
    check(secure_buffer_initialize(&buffer, 100).ok() && buffer.data && buffer.capacity == 100 && buffer.bytes == 0 &&
              buffer.locked && reinterpret_cast<usize>(buffer.data) % 4096 == 0,
          "secure buffer is page-aligned storage that reports itself ready");
    char flags[256];
    if (mapping_flags(buffer.data, flags)) {
        check(has_flag(flags, "dd"), "secure buffer pages are excluded from core dumps");
        check(has_flag(flags, "wf"), "secure buffer pages are wiped in a forked child");
    }
    const ConstSlice<u8> secret = text("the quick brown fox");
    check(secure_buffer_assign(&buffer, secret).ok() && buffer.bytes == secret.count &&
              bytes_equal(secure_buffer_view(buffer).data, secret.data, secret.count),
          "secure buffer holds an assigned value");
    u8 big[101];
    memset(big, 7, sizeof(big));
    const auto too_big = secure_buffer_assign(&buffer, {big, 101});
    check(too_big.error == SecureBufferError::output_too_small && too_big.bytes == 101 &&
              buffer.bytes == secret.count && bytes_equal(buffer.data, secret.data, secret.count),
          "secure buffer reports the size it needs and keeps its value");
    check(secure_buffer_assign(&buffer, {nullptr, 3}).error == SecureBufferError::invalid_input,
          "secure buffer rejects a null value with a length");
    SecureBuffer moved{};
    check(secure_buffer_transfer(&moved, &buffer) && !buffer.data && !buffer.locked && moved.data && moved.locked &&
              moved.bytes == secret.count && !secure_buffer_transfer(&moved, &moved),
          "secure buffer ownership moves once");
    SecureBuffer empty{};
    check(secure_buffer_initialize(&empty, 16).ok() && !secure_buffer_transfer(&buffer, &empty),
          "an empty secure buffer does not transfer");
    secure_buffer_release(&empty);
    const u8* retained = moved.data;
    secure_buffer_clear(&moved);
    check(moved.data == retained && moved.bytes == 0 && test_filled(moved.data, moved.capacity, 0),
          "clearing wipes the whole capacity");
    secure_buffer_release(&moved);
    check(!moved.data && !moved.locked && !moved.capacity && !moved.bytes, "release retires the owner");
    secure_buffer_release(&moved);
    secure_buffer_release(nullptr);
}

// A refused mlock must not fail the buffer (D19). Running as root or with CAP_IPC_LOCK the limit is not enforced, and
// the case then only proves the buffer still works.
void unlocked_case() noexcept {
    u64 original[2]{};
    const bool read = !syscall_failed(syscall2(kCallGetrlimit, kResourceMemoryLock, reinterpret_cast<long>(original)));
    u64 none[2]{0, original[1]};
    const bool lowered = read && !syscall_failed(syscall2(kCallSetrlimit, kResourceMemoryLock, reinterpret_cast<long>(none)));
    SecureBuffer buffer{};
    check(secure_buffer_initialize(&buffer, 4096).ok() && buffer.data && buffer.locked,
          "secure buffer degrades when mlock is refused");
    check(secure_buffer_assign(&buffer, text("still usable")).ok(), "an unlocked secure buffer holds a value");
    secure_buffer_release(&buffer);
    if (lowered)
        syscall2(kCallSetrlimit, kResourceMemoryLock, reinterpret_cast<long>(original));
}

bool key_file(char (&path)[1024]) noexcept {
    wchar_t wide[1024];
    return data_path(wide, 1024, L"account-secret.key", false).ok() && path_to_utf8(wide, path, 1024) != 0;
}

void user_bound_cases() noexcept {
    const ConstSlice<u8> secret = text("the quick brown fox jumps over the lazy dog");
    const u32 blob_bytes = secret.count + kBlobOverhead;
    SecureBuffer plaintext{}, opened{};
    check(secure_buffer_initialize(&plaintext, 64).ok() && secure_buffer_assign(&plaintext, secret).ok() &&
              secure_buffer_initialize(&opened, 64).ok(),
          "user-bound fixture buffers");
    const ConstSlice<u8> entropy = text("MaiCord test entropy");
    u8 blob[200], other[200];
    memset(blob, 0xa5, sizeof(blob));

    // A blob is the plaintext's length plus the version, nonce and tag, and it is not the plaintext.
    const auto sealed = secure_buffer_protect_user_bound(plaintext, entropy, {blob, 200});
    check(sealed.ok() && sealed.bytes == blob_bytes && blob[0] == 1 && test_filled(blob + blob_bytes, 200 - blob_bytes, 0xa5) &&
              !contains({blob, blob_bytes}, {secret.data, 12}),
          "a protected blob is the plaintext plus 41 bytes and does not contain it");
    const auto unsealed = secure_buffer_unprotect_user_bound({blob, blob_bytes}, entropy, &opened);
    check(unsealed.ok() && unsealed.bytes == secret.count && opened.bytes == secret.count &&
              bytes_equal(opened.data, secret.data, secret.count),
          "a protected blob opens to the plaintext");

    // The key file: 0600, the magic and 32 key bytes, in the data directory.
    char path[1024];
    u8 status[144]{};
    check(key_file(path) && !syscall_failed(syscall2(kCallStat, reinterpret_cast<long>(path), reinterpret_cast<long>(status))),
          "the key file exists in the data directory");
    u32 mode = 0;
    u64 size = 0;
    copy_bytes(&mode, status + 24, 4);
    copy_bytes(&size, status + 48, 8);
    check((mode & 0777) == 0600 && size == 36, "the key file is private and 36 bytes");

    // The same plaintext seals differently each time (a fresh nonce).
    check(secure_buffer_protect_user_bound(plaintext, entropy, {other, 200}).ok() && !bytes_equal(blob, other, blob_bytes),
          "two protections of one plaintext differ");

    // Output sizing never writes.
    memset(other, 0x5a, sizeof(other));
    const auto short_output = secure_buffer_protect_user_bound(plaintext, entropy, {other, blob_bytes - 1});
    check(short_output.error == SecureBufferError::output_too_small && short_output.bytes == blob_bytes &&
              test_filled(other, 200, 0x5a),
          "a short protect output reports the size and stays untouched");
    check(secure_buffer_protect_user_bound(plaintext, entropy, {}).bytes == blob_bytes, "an empty protect output sizes the blob");

    // Another entropy, a damaged blob and a short blob fail and leave the caller's buffer alone.
    SecureBuffer sentinel{};
    check(secure_buffer_initialize(&sentinel, 64).ok() && secure_buffer_assign(&sentinel, text("untouched")).ok(),
          "sentinel buffer");
    check(secure_buffer_unprotect_user_bound({blob, blob_bytes}, text("another entropy"), &sentinel).error ==
                  SecureBufferError::unprotect_failed &&
              bytes_equal(sentinel.data, "untouched", 9) && sentinel.bytes == 9,
          "another entropy does not open a blob");
    check(secure_buffer_unprotect_user_bound({blob, blob_bytes}, {}, &sentinel).error == SecureBufferError::unprotect_failed,
          "no entropy does not open a blob that was sealed with some");
    for (u32 i = 0; i != blob_bytes; ++i) {
        copy_bytes(other, blob, blob_bytes);
        other[i] ^= 1;
        if (secure_buffer_unprotect_user_bound({other, blob_bytes}, entropy, &sentinel).ok()) {
            check(false, "a blob damaged at any byte is rejected");
            break;
        }
    }
    check(bytes_equal(sentinel.data, "untouched", 9) && sentinel.bytes == 9, "rejected blobs leave the destination as it was");
    check(secure_buffer_unprotect_user_bound({blob, blob_bytes - 1}, entropy, &sentinel).error ==
                  SecureBufferError::unprotect_failed &&
              secure_buffer_unprotect_user_bound({blob, kBlobOverhead}, entropy, &sentinel).error ==
                  SecureBufferError::unprotect_failed &&
              secure_buffer_unprotect_user_bound({blob, 3}, entropy, &sentinel).error == SecureBufferError::unprotect_failed,
          "a truncated blob is rejected");
    SecureBuffer small{};
    check(secure_buffer_initialize(&small, 8).ok(), "small destination");
    const auto overflow = secure_buffer_unprotect_user_bound({blob, blob_bytes}, entropy, &small);
    check(overflow.error == SecureBufferError::output_too_small && overflow.bytes == secret.count && small.bytes == 0,
          "a destination that is too small reports the plaintext size");
    secure_buffer_release(&small);
    secure_buffer_release(&sentinel);

    u8 long_entropy[513]{};
    check(secure_buffer_protect_user_bound(plaintext, {long_entropy, 513}, {other, 200}).error ==
              SecureBufferError::invalid_input,
          "entropy beyond 512 bytes is rejected");
    check(secure_buffer_protect_user_bound(plaintext, {long_entropy, 512}, {other, 200}).ok(), "entropy of 512 bytes is accepted");
    SecureBuffer unready{};
    check(secure_buffer_protect_user_bound(unready, entropy, {other, 200}).error == SecureBufferError::invalid_input &&
              secure_buffer_unprotect_user_bound({blob, blob_bytes}, entropy, &unready).error == SecureBufferError::invalid_input &&
              secure_buffer_unprotect_user_bound({nullptr, 0}, entropy, &opened).error == SecureBufferError::invalid_input,
          "user-bound calls reject unready buffers and empty input");

    // The block form: caller-owned header bytes in front, one allocation.
    u8* block = nullptr;
    u32 block_bytes = 0;
    const auto in_block = secure_buffer_protect_user_bound_block(plaintext, entropy, 16, 4096, &block, &block_bytes);
    check(in_block.ok() && in_block.bytes == blob_bytes && block && block_bytes == 16 + blob_bytes &&
              test_filled(block, 16, 0) &&
              secure_buffer_unprotect_user_bound({block + 16, blob_bytes}, entropy, &opened).ok() &&
              bytes_equal(opened.data, secret.data, secret.count),
          "the block form puts the blob after the header and it opens");
    if (block) {
        secure_clear_bytes(block, block_bytes);
        pages_release(block, block_bytes);
    }
    block = nullptr;
    const auto too_large = secure_buffer_protect_user_bound_block(plaintext, entropy, 16, 16 + blob_bytes - 1, &block, &block_bytes);
    check(too_large.error == SecureBufferError::output_too_small && !block && block_bytes == 0,
          "the block form stops at its maximum without allocating");
    check(secure_buffer_protect_user_bound_block(plaintext, entropy, 0, 4096, &block, &block_bytes).error ==
                  SecureBufferError::invalid_input &&
              secure_buffer_protect_user_bound_block(plaintext, entropy, 4096, 4096, &block, &block_bytes).error ==
                  SecureBufferError::invalid_input,
          "the block form needs a header and room after it");

    // With the key file gone nothing seals or opens as before: a blob made under the old key does not open under a
    // new one, and opening alone never creates a key.
    check(!syscall_failed(sys_unlink(path)), "the key file is removable");
    check(secure_buffer_unprotect_user_bound({blob, blob_bytes}, entropy, &opened).error == SecureBufferError::unprotect_failed &&
              syscall_failed(syscall2(kCallStat, reinterpret_cast<long>(path), reinterpret_cast<long>(status))),
          "opening without a key file fails and creates none");
    check(secure_buffer_protect_user_bound(plaintext, entropy, {other, 200}).ok() &&
              secure_buffer_unprotect_user_bound({other, blob_bytes}, entropy, &opened).ok() &&
              secure_buffer_unprotect_user_bound({blob, blob_bytes}, entropy, &opened).error ==
                  SecureBufferError::unprotect_failed,
          "a new key file seals new blobs and does not open the old ones");

    secure_buffer_release(&plaintext);
    secure_buffer_release(&opened);
}

// A data directory that cannot hold the key: protecting fails with an error, never with a blob under no key.
void unavailable_case() noexcept {
    SecureBuffer plaintext{};
    check(secure_buffer_initialize(&plaintext, 16).ok() && secure_buffer_assign(&plaintext, text("secret")).ok(),
          "fixture buffer");
    u8 out[100];
    memset(out, 0xa5, sizeof(out));
    const auto result = secure_buffer_protect_user_bound(plaintext, text("entropy"), {out, 100});
    check(result.error == SecureBufferError::protect_failed && result.native_error != 0 && test_filled(out + 47, 53, 0xa5),
          "protecting without a usable data directory fails");
    u8* block = nullptr;
    u32 block_bytes = 0;
    check(secure_buffer_protect_user_bound_block(plaintext, text("entropy"), 8, 4096, &block, &block_bytes).error ==
                  SecureBufferError::protect_failed &&
              !block && block_bytes == 0,
          "the block form fails the same way and allocates nothing");
    secure_buffer_release(&plaintext);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_secure_buffer_entry) {
    const maicord::ProcessStartup& startup = maicord::process_startup();
    const char* mode = startup.argc > 1 ? startup.argv[1] : "";
    if (maicord::text_equal(mode, "--unavailable")) {
        maicord::unavailable_case();
        maicord::test_exit(maicord::g_failures, "maicord_secure_buffer_unavailable");
    }
    maicord::storage_cases();
    maicord::unlocked_case();
    maicord::user_bound_cases();
    maicord::test_exit(maicord::g_failures, "maicord_secure_buffer");
}
