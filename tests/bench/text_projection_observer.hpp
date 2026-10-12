#pragma once
#include "maicord/ui/text_presentation.hpp"

namespace maicord::text_projection_bench {
inline constexpr u64 kObservationSeed = 14695981039346656037ull;
struct Observation {
    u64 hash = kObservationSeed;
    u32 count = 0;
    const UiTextCommandStream* expected = nullptr; // Exact comparison only outside timing.
};
// Compile the implementation TU with /GL-: producers cannot inspect these bodies.
bool consume(void*, const UiTextCommand&, u32 ordinal) noexcept;
bool observe_stored(const UiTextCommandStream&, Observation*) noexcept;
u64 observe_preflight(const UiTextCommandStream*, const UiTextEmitter*) noexcept;
}
