#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/crypto/secure_buffer.hpp"
#include "maicord/platform/dbus/connection.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/path.hpp"
#include "maicord/sys/linux/process.hpp"
#include "../../support/test_output.hpp"

// The user-bound secure buffer (src/platform/crypto/linux/secure_buffer.cpp) with its master key in the Secret Service
// (the mock of tools/test_dbus.sh) or, on a bus without one (--bare), in the 0600 key file:
//
//   - opening a blob with no key anywhere fails and makes no key in either store;
//   - the first protect creates the key in the service, not in a file; later calls fetch it from there;
//   - a locked keyring falls back to the key file; a key file is moved into the service on the next call that finds the
//     service holding no key, and the same key bytes arrive there (the mock logs them), so older blobs still open;
//   - on a bus without a Secret Service the key file is used as before.
//
// Without MAICORD_DBUS_TEST_BUS naming the bus (mock, or bare with --bare) the test skips (77).

namespace maicord {
namespace {

using namespace dbus;

u32 g_failures = 0;
Connection* g_control;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

bool text_equal(const char* a, const char* b) noexcept {
    if (!a || !b)
        return a == b;
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

bool starts_with(const char* text, const char* prefix) noexcept {
    while (*prefix && *text == *prefix)
        ++text, ++prefix;
    return *prefix == 0;
}

bool contains(const char* text, const char* needle) noexcept {
    for (u32 i = 0; text[i]; ++i)
        if (starts_with(text + i, needle))
            return true;
    return false;
}

CallTarget mock(const char* member) noexcept {
    return CallTarget{"org.maicord.Mock", "/org/maicord/Mock", "org.maicord.Mock", member};
}

void control(const char* member, const Builder* body) noexcept {
    Message reply;
    const Status status = connection_call_sync(g_control, mock(member), body, 3000, &reply);
    check(status == Status::ok, member);
    if (status == Status::ok || status == Status::error_reply)
        message_release(&reply);
}

void set_mode(const char* name, bool value) noexcept {
    Builder b;
    builder_init(&b);
    builder_string(&b, name);
    builder_bool(&b, value);
    control("SetMode", &b);
    builder_release(&b);
}

char g_lines[32][1024];
u32 g_line_count;

void take_log() noexcept {
    g_line_count = 0;
    Message reply;
    if (connection_call_sync(g_control, mock("Take"), nullptr, 3000, &reply) != Status::ok)
        return;
    Reader r;
    message_reader(reply, &r);
    if (reader_enter_array(&r)) {
        while (reader_more(r) && g_line_count < 32) {
            const char* text = nullptr;
            u32 length = 0;
            if (!reader_string(&r, &text, &length))
                break;
            if (length >= sizeof(g_lines[0]))
                length = sizeof(g_lines[0]) - 1;
            copy_bytes(g_lines[g_line_count], text, length);
            g_lines[g_line_count][length] = 0;
            ++g_line_count;
        }
    }
    message_release(&reply);
}

const char* find_line(const char* prefix) noexcept {
    for (u32 i = 0; i < g_line_count; ++i)
        if (starts_with(g_lines[i], prefix))
            return g_lines[i];
    return nullptr;
}

// The key file --------------------------------------------------------------------------------------------------------

bool key_file_path(char (&path)[1024]) noexcept {
    wchar_t wide[1024];
    return data_path(wide, 1024, L"account-secret.key", false).ok() && path_to_utf8(wide, path, 1024) != 0;
}

// The file's 32 key bytes; false when there is no file of the right size.
bool read_key_file(u8 (&key)[32]) noexcept {
    char path[1024];
    if (!key_file_path(path))
        return false;
    const long opened = sys_openat(kAtCurrentDirectory, path, kOpenReadOnly | kOpenCloseOnExec);
    if (syscall_failed(opened))
        return false;
    u8 bytes[64];
    const long got = sys_read(static_cast<int>(opened), bytes, sizeof(bytes));
    sys_close(static_cast<int>(opened));
    if (got != 36)
        return false;
    copy_bytes(key, bytes + 4, 32);
    return true;
}

bool key_file_exists() noexcept {
    u8 key[32];
    return read_key_file(key);
}

void remove_key_file() noexcept {
    char path[1024];
    if (key_file_path(path))
        sys_unlink(path);
}

void hex_of(const u8* bytes, u32 count, char* out) noexcept {
    for (u32 i = 0; i < count; ++i) {
        out[i * 2] = "0123456789abcdef"[bytes[i] >> 4];
        out[i * 2 + 1] = "0123456789abcdef"[bytes[i] & 15];
    }
    out[count * 2] = 0;
}

// Secure buffer helpers -----------------------------------------------------------------------------------------------

constexpr u32 kBlobMax = 128;
const u8 kEntropy[4] = {'t', 'e', 's', 't'};

// Seals `text` (up to 40 bytes); the blob's length, 0 on failure.
u32 seal(const char* text, u8 (&blob)[kBlobMax]) noexcept {
    SecureBuffer plain{};
    if (!secure_buffer_initialize(&plain, 64).ok())
        return 0;
    u32 length = 0;
    while (text[length])
        ++length;
    secure_buffer_assign(&plain, ConstSlice<u8>{reinterpret_cast<const u8*>(text), length});
    const auto sealed = secure_buffer_protect_user_bound(plain, ConstSlice<u8>{kEntropy, 4}, Slice<u8>{blob, kBlobMax});
    secure_buffer_release(&plain);
    return sealed.ok() ? sealed.bytes : 0;
}

bool opens_to(const u8* blob, u32 length, const char* text) noexcept {
    SecureBuffer out{};
    if (!secure_buffer_initialize(&out, 64).ok())
        return false;
    const auto opened = secure_buffer_unprotect_user_bound(ConstSlice<u8>{blob, length}, ConstSlice<u8>{kEntropy, 4}, &out);
    u32 expected = 0;
    while (text[expected])
        ++expected;
    const bool ok = opened.ok() && out.bytes == expected && bytes_equal(out.data, text, expected);
    secure_buffer_release(&out);
    return ok;
}

void test_mock() noexcept {
    control("Reset", nullptr);
    remove_key_file();

    // A blob that was never sealed: no key exists, and opening makes none anywhere.
    u8 stray[61] = {1};
    SecureBuffer out{};
    secure_buffer_initialize(&out, 64);
    const auto missing = secure_buffer_unprotect_user_bound(ConstSlice<u8>{stray, sizeof(stray)}, ConstSlice<u8>{kEntropy, 4}, &out);
    secure_buffer_release(&out);
    take_log();
    check(!missing.ok() && !key_file_exists() && find_line("CreateItem") == nullptr,
          "opening a blob with no key anywhere fails and creates no key in either store");

    // The first protect creates the key in the Secret Service.
    u8 first[kBlobMax];
    const u32 first_bytes = seal("first secret", first);
    take_log();
    const char* created = find_line("CreateItem");
    check(first_bytes != 0 && !key_file_exists(), "the first protect works and writes no key file");
    check(created && contains(created, "label=MaiCord master key") &&
              contains(created, "attrs={s:application=s:maicord,s:purpose=s:master-key}") &&
              contains(created, "type=application/octet-stream") && contains(created, "replace=1"),
          "the key is created in the Secret Service with the attributes application=maicord, purpose=master-key");
    check(opens_to(first, first_bytes, "first secret"), "the blob opens");
    take_log();
    check(find_line("GetSecret") != nullptr && find_line("CreateItem") == nullptr,
          "opening fetched the key from the service and made no new one");
    u8 second[kBlobMax];
    const u32 second_bytes = seal("second", second);
    check(second_bytes != 0 && opens_to(first, first_bytes, "first secret") && opens_to(second, second_bytes, "second"),
          "later blobs use the same key");
    check(!opens_to(second, second_bytes - 1, "second"), "a truncated blob does not open");

    // A locked keyring: the key file stands in.
    control("Reset", nullptr);
    set_mode("secret-locked", true);
    u8 filed[kBlobMax];
    const u32 filed_bytes = seal("filed secret", filed);
    check(filed_bytes != 0 && key_file_exists(), "a locked keyring falls back to the key file");
    check(opens_to(filed, filed_bytes, "filed secret"), "and the blob opens under it");
    u8 file_key[32];
    check(read_key_file(file_key), "the key file holds the key");

    // The keyring unlocks: the file's key moves in (the same bytes), the file goes, the older blob still opens.
    set_mode("secret-locked", false);
    take_log();
    check(opens_to(filed, filed_bytes, "filed secret"), "after the unlock the older blob still opens");
    take_log();
    char hex[65];
    hex_of(file_key, 32, hex);
    char want[96] = "secret=";
    copy_bytes(want + 7, hex, 65);
    created = find_line("CreateItem");
    check(created && contains(created, want) && contains(created, "attrs={s:application=s:maicord,s:purpose=s:master-key}"),
          "the key file's key bytes were stored in the Secret Service");
    check(!key_file_exists(), "the key file was removed after the move");
    u8 later[kBlobMax];
    const u32 later_bytes = seal("later secret", later);
    check(later_bytes != 0 && !key_file_exists() && opens_to(later, later_bytes, "later secret") &&
              opens_to(filed, filed_bytes, "filed secret"),
          "afterwards the service is the only key store");

    // A service that refuses to create the key: the file takes over.
    control("Reset", nullptr);
    set_mode("secret-create-error", true);
    u8 refused[kBlobMax];
    const u32 refused_bytes = seal("refused", refused);
    check(refused_bytes != 0 && key_file_exists() && opens_to(refused, refused_bytes, "refused"),
          "a service that cannot store the key leaves the file as the store");
    set_mode("secret-create-error", false);
    remove_key_file();
    control("Reset", nullptr);
}

void test_bare() noexcept {
    remove_key_file();
    u8 blob[kBlobMax];
    const u32 bytes = seal("bare secret", blob);
    check(bytes != 0 && key_file_exists(), "without a Secret Service the key file is made, as before");
    check(opens_to(blob, bytes, "bare secret"), "and the blob opens");
    remove_key_file();
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_secure_buffer_service_entry) {
    using namespace maicord;
    const ProcessStartup& startup = process_startup();
    const bool bare = startup.argc > 1 && text_equal(startup.argv[1], "--bare");
    const char* kind = environment_get("MAICORD_DBUS_TEST_BUS");
    const char* address = environment_get("MAICORD_DBUS_TEST_ADDRESS");
    const char* session = environment_get("DBUS_SESSION_BUS_ADDRESS");
    const char* service = environment_get("MAICORD_SECRET_SERVICE");
    if (!kind || !address || !session || !text_equal(kind, bare ? "bare" : "mock") || !text_equal(address, session) ||
        (service && text_equal(service, "off"))) {
        test_write_line("SKIP secure_buffer_service (tools/test_dbus.sh sets the private bus and MAICORD_SECRET_SERVICE=auto)");
        test_process_exit(77);
    }
    if (bare) {
        test_bare();
        test_exit(g_failures, "secure_buffer_service_bare");
    }
    g_control = connection_open_address(address);
    check(g_control != nullptr, "a control connection to the mock");
    if (g_control) {
        test_mock();
        connection_close(g_control);
    }
    test_exit(g_failures, "secure_buffer_service");
}
