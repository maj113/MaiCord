#pragma once

// Haswell instructions (AVX2, BMI1, BMI2, LZCNT, MOVBE, F16C) for a hot loop that is written by hand. The
// compiler stays on the Sandy Bridge baseline for all other code. A function with MAICORD_HASWELL holds the
// whole loop: the compiler does not inline it into a caller without the attribute. There is one version of
// each loop and no CPU dispatch. Declare an intrinsic here, with the same attribute, when a loop needs one.
#define MAICORD_HASWELL __attribute__((__target__("avx2,bmi,bmi2,lzcnt,movbe,f16c")))
