#include "binding_internal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/crypto/secure_buffer.hpp"

// SECRET settings at rest: a value sealed with DPAPI for the current Windows user, bound to the
// plugin and the setting by entropy, so another plugin's settings file cannot be moved over this one.
namespace maicord::plugin_runtime_detail {
namespace {

constexpr char kDomain[] = "MaiCord plugin secret v1";

u32 entropy_for(u64 owner_id, mc_str key, u8 (&entropy)[sizeof(kDomain) + 8 + MC_SETTING_KEY_MAX]) noexcept {
    u32 used = sizeof(kDomain) - 1;
    copy_bytes(entropy, kDomain, used);
    for (u32 shift = 0; shift < 64; shift += 8) entropy[used++] = static_cast<u8>(owner_id >> shift);
    copy_bytes(entropy + used, key.data, key.bytes);
    return used + key.bytes;
}

} // namespace

bool plugin_secret_seal(u64 owner_id, mc_str key, ConstSlice<u8> plain, Slice<u8> sealed, u32* sealed_bytes) noexcept {
    if (!plain.count || key.bytes > MC_SETTING_KEY_MAX) return false;
    u8 entropy[sizeof(kDomain) + 8 + MC_SETTING_KEY_MAX];
    const u32 entropy_bytes = entropy_for(owner_id, key, entropy);
    SecureBuffer buffer{};
    bool ok = secure_buffer_initialize(&buffer, plain.count).ok() && secure_buffer_assign(&buffer, plain).ok();
    if (ok) {
        const SecureBufferResult result =
            secure_buffer_protect_user_bound(buffer, {entropy, entropy_bytes}, sealed);
        ok = result.ok();
        *sealed_bytes = result.bytes;
    }
    secure_buffer_release(&buffer);
    return ok;
}

bool plugin_secret_unseal(u64 owner_id, mc_str key, ConstSlice<u8> sealed, Slice<u8> plain, u32* plain_bytes) noexcept {
    if (!sealed.count || key.bytes > MC_SETTING_KEY_MAX) return false;
    u8 entropy[sizeof(kDomain) + 8 + MC_SETTING_KEY_MAX];
    const u32 entropy_bytes = entropy_for(owner_id, key, entropy);
    SecureBuffer buffer{};
    bool ok = secure_buffer_initialize(&buffer, plain.count).ok() &&
              secure_buffer_unprotect_user_bound(sealed, {entropy, entropy_bytes}, &buffer).ok();
    if (ok) {
        const ConstSlice<u8> view = secure_buffer_view(buffer);
        ok = view.count <= plain.count;
        if (ok) {
            copy_bytes(plain.data, view.data, view.count);
            *plain_bytes = view.count;
        }
    }
    secure_buffer_release(&buffer);
    return ok;
}

} // namespace maicord::plugin_runtime_detail
