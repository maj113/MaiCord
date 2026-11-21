// First-party CFF face: .notdef and a box, a horizontal line, a vertical line, a point and a lone move. DirectWrite's
// bold simulation widened the advances of the first two glyphs only.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
static constexpr u8 cff_ink_fixture[]{
#embed "cff_ink_fixture.bin"
};
#pragma clang diagnostic pop
