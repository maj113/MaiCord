#pragma once

#include "maicord/core/base.hpp"

// All compiled code uses BMI1, BMI2, LZCNT, MOVBE and F16C (the compiler baseline), the hand-written vector code
// (MAICORD_HASWELL) uses AVX2 and some of it (MAICORD_HASWELL_FMA) FMA, with no other version. So MaiCord needs an
// Intel Haswell (2013), an AMD Excavator or Zen, or a later processor. Each client's entry calls processor_too_old()
// before any other code and refuses to start on an older processor. The code that runs before the check returns is
// MAICORD_ANY_PROCESSOR.

// The reason, shown after "MaiCord cannot start: ".
#define MAICORD_PROCESSOR_TOO_OLD "this processor is too old. MaiCord needs a processor from 2013 or later with AVX2."

namespace maicord {

// basic is CPUID.1:ECX, structured is CPUID.7.0:EBX, extended is CPUID.80000001h:ECX and saved_state is XCR0.
struct ProcessorFeatures {
    u32 basic, structured, extended, saved_state;
};

MAICORD_ANY_PROCESSOR constexpr bool processor_supported(const ProcessorFeatures& features) noexcept {
    // FMA, MOVBE, OSXSAVE, AVX, F16C. Compiled code also uses PCLMULQDQ, POPCNT and AES.
    constexpr u32 kBasic = 1u << 12 | 1u << 22 | 1u << 27 | 1u << 28 | 1u << 29 | 1u << 1 | 1u << 23 | 1u << 25;
    constexpr u32 kStructured = 1u << 3 | 1u << 5 | 1u << 8;          // BMI1, AVX2, BMI2
    constexpr u32 kExtended = 1u << 5;                                // LZCNT
    constexpr u32 kSavedState = 1u << 1 | 1u << 2;                    // the system saves the XMM and YMM registers
    return (features.basic & kBasic) == kBasic && (features.structured & kStructured) == kStructured &&
           (features.extended & kExtended) != 0 && (features.saved_state & kSavedState) == kSavedState;
}

MAICORD_ANY_PROCESSOR inline void processor_identify(u32 leaf, u32 (&registers)[4]) noexcept {
    __asm__ volatile("cpuid"
                     : "=a"(registers[0]), "=b"(registers[1]), "=c"(registers[2]), "=d"(registers[3])
                     : "a"(leaf), "c"(0u));
}

// Not inlined: a caller compiled for AVX would compile the check for AVX too.
MAICORD_ANY_PROCESSOR __declspec(noinline) inline bool processor_too_old() noexcept {
    u32 registers[4];
    ProcessorFeatures features{};
    processor_identify(0, registers);
    const u32 leaves = registers[0];
    processor_identify(1, registers);
    features.basic = registers[2];
    if (leaves >= 7) {
        processor_identify(7, registers);
        features.structured = registers[1];
    }
    processor_identify(0x80000001u, registers);
    features.extended = registers[2];
    // XGETBV faults unless the system set OSXSAVE.
    if (features.basic & 1u << 27) {
        __asm__ volatile("xgetbv" : "=a"(features.saved_state), "=d"(registers[3]) : "c"(0u));
    }
    return !processor_supported(features);
}

} // namespace maicord
