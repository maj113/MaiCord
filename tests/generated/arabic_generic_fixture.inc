// First-party identical fonts differing only in GSUB script: arab, DFLT, latn.
// Windows reference SHA256: f1a84435bf989b3898c011fce75baa7ea3f5ff7006797dc0a31e70b04fe3a4ec
// Generic case first also exercises UTF-8, short-storage and work-budget units.
// Corpus SHA256: 630ef05213ea6271b6f6b8d95aa47ea39507cf4cc9a310da0da056a00d29ffe8
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
constexpr u8 arabic_generic_fixture[]{
#embed "arabic_generic_fixture.bin"
};
#pragma clang diagnostic pop
