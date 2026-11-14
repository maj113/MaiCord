#pragma once
#include "maicord/model/user/profile_editor.hpp"
#include "maicord/ui/widgets/color_picker.hpp"

namespace maicord::app_detail {

// Try It Out's Shuffle, as the official client's: a random two-colour theme and a random name style from `seed`.
// A zero seed keeps the generator at zero: the draft then gets the first font and the first effect.
inline void profile_editor_shuffle(u64 seed, ProfileEditorDraft* draft) noexcept {
    u64 state = seed;
    const auto next = [&state](u32 bound) noexcept {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<u32>(state % bound);
    };
    const float hue = next(360) / 360.f;
    draft->theme_set = true;
    draft->theme_reset = false;
    draft->theme[0] = color_picker_rgb({hue, .45f + next(40) / 100.f, .55f + next(40) / 100.f});
    draft->theme[1] =
        color_picker_rgb({hue < .5f ? hue + .5f : hue - .5f, .45f + next(40) / 100.f, .3f + next(40) / 100.f});
    draft->name_style_set = true;
    const u16 font = kProfileNameFontIds[next(kProfileNameFontCount)];
    draft->name_style = profile_name_style_with_effect({font, 0, 0, {}}, next(kProfileNameEffectCount));
}

} // namespace maicord::app_detail
