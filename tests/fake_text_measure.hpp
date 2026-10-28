#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/ui/text_measure_cache.hpp"

namespace maicord {

// Deterministic measurer for layout tests: 8 DIP per code point, 20 DIP per line, 16 DIP baseline,
// wrapping at the constraint width. A no-wrap request keeps its natural width unless kClipNoWrap
// clips it to the constraint like wrapped text.
template <bool kClipNoWrap = false>
inline bool fake_text_measure(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* value) noexcept {
    const auto text = utf8_validate(request.text);
    if (!text.valid || request.constraint.width <= 0)
        return false;
    const float natural = static_cast<float>(text.codepoints) * 8;
    const u32 columns = maximum(1u, static_cast<u32>(request.constraint.width / 8));
    const u32 lines = request.no_wrap ? 1u : maximum(1u, (text.codepoints + columns - 1) / columns);
    const float width = request.no_wrap && !kClipNoWrap ? natural : minimum(natural, request.constraint.width);
    *value = {{width, static_cast<float>(lines) * 20}, 16, lines};
    return true;
}

} // namespace maicord
