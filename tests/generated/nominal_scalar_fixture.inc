// Uncompressed Windows observation, independent of native page encoding.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
constexpr u8 nominal_scalar_fixture[]{
#embed "nominal_scalar_fixture.bin"
};
#pragma clang diagnostic pop
