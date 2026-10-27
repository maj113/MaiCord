// First-party synthetic fonts and Windows observations; QA only.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
constexpr u8 emoji_fixture[]{
#embed "emoji_run_fixture.bin"
};
#pragma clang diagnostic pop
