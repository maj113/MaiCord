// First face of font_family_fixture.inc (bytes 24..847), byte for byte; the catalog tests need only this one font.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
constexpr u8 font_catalog_probe_font[]{
#embed "font_catalog_probe_font.bin"
};
#pragma clang diagnostic pop
