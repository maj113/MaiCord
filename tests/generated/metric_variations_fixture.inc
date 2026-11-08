// First-party variable metric fonts and design-unit expectations from HarfBuzz: hmtx advance plus the HVAR
// delta indexed by glyph ID (OpenType HVAR), rounded half up and clamped to the uint16 range.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
constexpr u8 metric_variations_fixture[]{
#embed "metric_variations_fixture.bin"
};
#pragma clang diagnostic pop
