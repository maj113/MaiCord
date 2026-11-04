// GlobalKeys on Wayland: the keys of the focused window, in an atomic bitmap by Key (maicord/platform/window/global_keys.hpp).
#include "maicord/platform/window/global_keys.hpp"
#include "backend.hpp"

namespace maicord::wayland {
namespace {

u64 g_down[4];  // bit k of word k / 64: Key k is down

} // namespace

void global_keys_set(Key key, bool down) noexcept {
    const u64 bit = 1ull << (key % 64);
    if (down)
        __atomic_or_fetch(&g_down[key / 64], bit, __ATOMIC_RELEASE);
    else
        __atomic_and_fetch(&g_down[key / 64], ~bit, __ATOMIC_RELEASE);
}

void global_keys_clear() noexcept {
    for (u64& word : g_down)
        __atomic_store_n(&word, 0ull, __ATOMIC_RELEASE);
}

} // namespace maicord::wayland

namespace maicord {

bool global_key_down(Key key) noexcept {
    const u64 word = __atomic_load_n(&wayland::g_down[key / 64], __ATOMIC_ACQUIRE);
    return (word >> (key % 64)) & 1;
}

} // namespace maicord
