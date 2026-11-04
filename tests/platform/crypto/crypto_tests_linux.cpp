// The platform crypto tests of the Linux build in one process: the vectors tests/platform/crypto shares with the
// Windows suite (AES-GCM, DAVE's 64-bit-tag AES-GCM, XChaCha20-Poly1305, P-256) and the ones for the parts a Linux
// backend adds. The order is the order of tests/voice_setup_entry.cpp's, then the new ones.
#include "../../support/test_output.hpp"

namespace maicord {

using Check = void (*)(bool, const char*) noexcept;

void test_aes_gcm(Check check) noexcept;
void test_aes_gcm64(Check check) noexcept;
void test_xchacha20_poly1305(Check check) noexcept;
void test_p256(Check check) noexcept;
void test_kdf_sha256(Check check) noexcept;
void test_secure_random(Check check) noexcept;
void test_hash_stream(Check check) noexcept;
void test_remote_auth_crypto(Check check) noexcept;

namespace {

u32 g_failures = 0;

void check(bool condition, const char* label) noexcept {
    test_check(&g_failures, condition, label);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_crypto_tests_entry) {
    maicord::test_aes_gcm(maicord::check);
    maicord::test_aes_gcm64(maicord::check);
    maicord::test_xchacha20_poly1305(maicord::check);
    maicord::test_p256(maicord::check);
    maicord::test_kdf_sha256(maicord::check);
    maicord::test_secure_random(maicord::check);
    maicord::test_hash_stream(maicord::check);
    maicord::test_remote_auth_crypto(maicord::check);
    maicord::test_exit(maicord::g_failures, "maicord_crypto");
}
