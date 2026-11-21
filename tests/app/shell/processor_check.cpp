#include "maicord/app/shell/processor_check.hpp"

namespace maicord {

// Feature words as the named processors report them: CPUID.1:ECX, CPUID.7.0:EBX, CPUID.80000001h:ECX, XCR0.
void test_processor_check(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr ProcessorFeatures sandy_bridge{0x1FBAE3FFu, 0, 0x00000001u, 7};
    constexpr ProcessorFeatures ivy_bridge{0x7FBAE3FFu, 0x00000281u, 0x00000001u, 7};
    constexpr ProcessorFeatures piledriver{0x3E98320Bu, 0x00000008u, 0x01EBBFFFu, 7};
    constexpr ProcessorFeatures haswell{0x7FFAFBFFu, 0x000027ABu, 0x00000021u, 7};
    constexpr ProcessorFeatures zen{0x7ED8320Bu, 0x209C01A9u, 0x35C233FFu, 7};
    check(!processor_supported(sandy_bridge), "Sandy Bridge has no AVX2: refused");
    check(!processor_supported(ivy_bridge), "Ivy Bridge has F16C but no AVX2: refused");
    check(!processor_supported(piledriver), "Piledriver has FMA, BMI1 and LZCNT but no AVX2: refused");
    check(processor_supported(haswell), "Haswell is the minimum processor");
    check(processor_supported(zen), "Zen has every Haswell feature");

    // The system must save the YMM registers: XCR0 without bit 2, and OSXSAVE clear (XCR0 is then not read).
    ProcessorFeatures no_ymm_state = haswell;
    no_ymm_state.saved_state = 3;
    check(!processor_supported(no_ymm_state), "Haswell whose system saves only the XMM registers: refused");
    ProcessorFeatures no_xsave = haswell;
    no_xsave.basic &= ~(1u << 27);
    no_xsave.saved_state = 0;
    check(!processor_supported(no_xsave), "Haswell whose system does not use XSAVE: refused");

    // One feature absent at a time.
    constexpr u32 basic_bits[]{1, 12, 22, 23, 25, 28, 29}, structured_bits[]{3, 5, 8};
    for (const u32 bit : basic_bits) {
        ProcessorFeatures features = haswell;
        features.basic &= ~(1u << bit);
        check(!processor_supported(features), "PCLMULQDQ, FMA, MOVBE, POPCNT, AES, AVX and F16C are each required");
    }
    for (const u32 bit : structured_bits) {
        ProcessorFeatures features = haswell;
        features.structured &= ~(1u << bit);
        check(!processor_supported(features), "BMI1, AVX2 and BMI2 are each required");
    }
    ProcessorFeatures no_lzcnt = haswell;
    no_lzcnt.extended = 1;
    check(!processor_supported(no_lzcnt), "LZCNT is required");

    // The tests run MAICORD_HASWELL code, so the processor that runs them passes the real check.
    check(!processor_too_old(), "the processor that runs the tests is supported");
}

} // namespace maicord
