#pragma once

#include "maicord/app/shell/shortcut_registry.hpp"
#include "maicord/platform/window/key.hpp"

namespace maicord {

// Background voice keybinds: while a call is active and another window has the focus, the custom chords are sampled
// from the system key state, as push to talk is. A chord acts one time when it goes down.
struct BackgroundKeybinds {
    u32 revision;
    bool armed;
    bool down[kCustomKeybindMaximum];
};

// One sample. `active`: the conditions hold (a call, the window not in the foreground, a keybind). `key_down(key)` is
// the system state of one key. Returns one bit for each keybind whose chord went down since the last sample. The first
// sample after the conditions start to hold, or after the keybinds change, only records the state: a key that was
// down before never acts.
template <typename KeyDown>
u32 background_keybinds_sample(BackgroundKeybinds* state, const CustomKeybinds& keybinds, u32 revision, bool active,
                               KeyDown key_down) noexcept {
    if (!active) {
        state->armed = false;
        return 0;
    }
    const bool rebase = !state->armed || state->revision != revision;
    state->armed = true;
    state->revision = revision;
    // The modifier keys are read on the first held key and shared by the chords after it.
    constexpr u32 kUnread = ~0u;
    u32 held = kUnread, fired = 0;
    for (u32 i = 0; i < keybinds.count; ++i) {
        const ShortcutChord chord = keybinds.items[i].chord;
        bool down = chord.key && key_down(static_cast<Key>(chord.key));
        if (down) {
            if (held == kUnread)
                held = (key_down(key::ctrl) ? shortcut_ctrl : 0) | (key_down(key::alt) ? shortcut_alt : 0) |
                       (key_down(key::shift) ? shortcut_shift : 0);
            down = held == chord.modifiers;
        }
        if (down && !state->down[i] && !rebase)
            fired |= 1u << i;
        state->down[i] = down;
    }
    return fired;
}

} // namespace maicord
