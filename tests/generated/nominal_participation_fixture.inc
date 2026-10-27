// First-party fonts and Windows font-participation observations; glyph zero excluded.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
constexpr u8 participation_fixture[]{
#embed "nominal_participation_fixture.bin"
};
#pragma clang diagnostic pop
