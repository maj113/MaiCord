#pragma once
#include "maicord/core/base.hpp"

namespace maicord {
// Caret/selection rectangle in layout coordinates. Source positions are UTF-16
// units, bounded to 65535 by paragraph fitting. Producers write the whole value.
// Keep owner arrays trivially initialized.
struct NativeParagraphHitMetric {
    u16 source_first, source_count;
    float left, top, width, height;
    u32 bidi_level;
};
static_assert(sizeof(NativeParagraphHitMetric) == 24 && is_trivial_v<NativeParagraphHitMetric>);
} // namespace maicord
