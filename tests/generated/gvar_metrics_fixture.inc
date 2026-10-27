// First-party HVAR-free fonts and DirectWrite advance expectations.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
constexpr u8 gvar_metrics_fixture[]{
#embed "gvar_metrics_fixture.bin"
};
#pragma clang diagnostic pop
