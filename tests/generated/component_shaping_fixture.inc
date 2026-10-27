// First-party Windows glyph/cluster/placement expectations; no attachment seeds.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
constexpr u8 component_fixture[]{
#embed "component_shaping_fixture.bin"
};
#pragma clang diagnostic pop
