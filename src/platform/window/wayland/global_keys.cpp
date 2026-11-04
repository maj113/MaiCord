// GlobalKeys on Wayland: the keys of the focused window, in an atomic bitmap by Key (maicord/platform/window/global_keys.hpp),
// and the one key bound through the GlobalShortcuts portal, which reads as down while the portal says the shortcut is held,
// focus or not.
#include "maicord/platform/dbus/global_shortcuts.hpp"
#include "maicord/platform/window/global_keys.hpp"
#include "maicord/platform/window/wayland.hpp"
#include "backend.hpp"

namespace maicord::wayland {
namespace {

u64 g_down[4];  // bit k of word k / 64: Key k is down

constexpr const char kShortcutId[] = "maicord-hold-to-talk";
u32 g_portal_key;     // the Key bound through the portal, 0 for none
u32 g_portal_active;  // 1 while the portal reports the shortcut held

void on_shortcut(void*, const char* id, bool active, u64) noexcept {
    for (u32 i = 0; i < sizeof(kShortcutId); ++i)
        if (id[i] != kShortcutId[i])
            return;
    __atomic_store_n(&g_portal_active, active ? 1u : 0u, __ATOMIC_RELEASE);
}

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
    if ((word >> (key % 64)) & 1)
        return true;
    return key != 0 && key == __atomic_load_n(&wayland::g_portal_key, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&wayland::g_portal_active, __ATOMIC_ACQUIRE) != 0;
}

void window_wayland_global_shortcut_clear() noexcept {
    dbus::global_shortcuts_stop();
    __atomic_store_n(&wayland::g_portal_key, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&wayland::g_portal_active, 0u, __ATOMIC_RELEASE);
}

bool window_wayland_global_shortcut(Key key, u8 modifiers, const char* description) noexcept {
    window_wayland_global_shortcut_clear();
    char trigger[64];
    // A key with no trigger form (a mouse button, a bare modifier) is still bound: the user picks the trigger in the
    // compositor's settings.
    dbus::global_shortcut_trigger(key, modifiers, trigger, sizeof(trigger));
    const dbus::GlobalShortcut shortcut{wayland::kShortcutId, description ? description : "Push to talk", trigger};
    __atomic_store_n(&wayland::g_portal_key, static_cast<u32>(key), __ATOMIC_RELEASE);
    return dbus::global_shortcuts_start(&shortcut, 1, wayland::on_shortcut, nullptr);
}

bool window_wayland_global_shortcut_ready() noexcept {
    return dbus::global_shortcuts_state() == dbus::GlobalShortcutsState::ready;
}

} // namespace maicord
