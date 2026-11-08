// First-party HVAR-free fonts and phantom-point advance expectations measured on Windows (each phantom
// delta rounded, as FreeType does), except that negative advances clamp to zero (uint16 advanceWidth; HarfBuzz).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
constexpr u8 gvar_metrics_fixture[]{
#embed "gvar_metrics_fixture.bin"
};
#pragma clang diagnostic pop
