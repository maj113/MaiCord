// Real zones compiled by zic (slim and fat) and what Python's zoneinfo says about them (tools/tzif_fixtures.py).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
static constexpr u8 tzif_fixture[]{
#embed "tzif_fixture.bin"
};
static constexpr u8 tzif_oracle[]{
#embed "tzif_oracle.bin"
};
#pragma clang diagnostic pop
