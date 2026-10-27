// Expected app admission and packed identities per UnicodeScript value, including
// historic and simple scripts. Admission bit 1: input path; bit 2: resolved path.
// Braille (enum 16) uses Common profile 16 in both paths since 2026-10-04.
// Simple scripts (Bamum, Yi, Vai, Tifinagh, Canadian Syllabics, N'Ko, Ol Chiki,
// Lisu) use profiles 64-78 since 2026-10-11; the other simple_script.hpp
// scripts follow from 80 since 2026-10-19.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
constexpr u8 script_dispatch_profile[]{
#embed "script_dispatch_fixture/script_dispatch_profile.bin"
};
constexpr u8 script_dispatch_admission[]{
#embed "script_dispatch_fixture/script_dispatch_admission.bin"
};
#pragma clang diagnostic pop
