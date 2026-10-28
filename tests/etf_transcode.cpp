// ETF on the user Gateway: received terms transcode to the JSON the dispatch
// layer reads (erlpack's value model), and JSON commands encode as the desktop
// client's erlpack packs them. Synthetic payloads only.
#include "maicord/protocol/etf.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

u32 checks = 0;
bool print_ok = true;
void print(const char* text) noexcept {
    u32 n = 0;
    while (text[n])
        ++n;
    DWORD written = 0;
    print_ok = print_ok && WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, n, &written, nullptr) && written == n;
}
void number(u64 v) noexcept {
    char digits[21];
    u32 n = 0;
    do {
        digits[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        const char text[]{digits[--n], 0};
        print(text);
    }
}
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            print("FAIL line=");                                                                                       \
            number(__LINE__);                                                                                          \
            print("\n");                                                                                               \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)

// Builds terms the way Erlang's term_to_binary lays them out.
struct Term {
    u8 data[4096];
    u32 used = 0;
    Term& b(u8 v) noexcept {
        data[used++] = v;
        return *this;
    }
    Term& be32(u32 v) noexcept {
        return b(static_cast<u8>(v >> 24)).b(static_cast<u8>(v >> 16)).b(static_cast<u8>(v >> 8)).b(static_cast<u8>(v));
    }
    Term& raw(const char* text, u32 n) noexcept {
        for (u32 i = 0; i < n; ++i)
            b(static_cast<u8>(text[i]));
        return *this;
    }
    Term& version() noexcept { return b(131); }
    Term& atom(const char* text) noexcept { // ATOM_EXT, as the Gateway sends keys
        const u32 n = c_string_length(text);
        return b(100).b(0).b(static_cast<u8>(n)).raw(text, n);
    }
    Term& small_atom(const char* text) noexcept {
        const u32 n = c_string_length(text);
        return b(115).b(static_cast<u8>(n)).raw(text, n);
    }
    Term& binary(const char* text, u32 n) noexcept { return b(109).be32(n).raw(text, n); }
    Term& binary(const char* text) noexcept { return binary(text, c_string_length(text)); }
    Term& map(u32 n) noexcept { return b(116).be32(n); }
    Term& list(u32 n) noexcept { return b(108).be32(n); }
    Term& nil() noexcept { return b(106); }
    Term& small(u8 v) noexcept { return b(97).b(v); }
    Term& integer(i32 v) noexcept { return b(98).be32(static_cast<u32>(v)); }
    Term& big(u64 v, bool negative = false) noexcept {
        u32 n = 0;
        for (u64 t = v; t; t >>= 8)
            ++n;
        b(110).b(static_cast<u8>(n)).b(negative ? 1 : 0);
        for (u32 i = 0; i < n; ++i)
            b(static_cast<u8>(v >> (8 * i)));
        return *this;
    }
    Term& real(u64 bits) noexcept { return b(70).be32(static_cast<u32>(bits >> 32)).be32(static_cast<u32>(bits)); }
};

u8 json[8192];
bool transcodes(const Term& term, const char* expected) noexcept {
    Slice<u8> output{json, sizeof(json)};
    const auto result = etf_to_json({term.data, term.used}, &output, sizeof(json), nullptr, nullptr);
    const u32 n = c_string_length(expected);
    return result.ok() && result.bytes == n && bytes_equal(json, expected, n);
}
EtfError transcode_error(const Term& term) noexcept {
    Slice<u8> output{json, sizeof(json)};
    return etf_to_json({term.data, term.used}, &output, sizeof(json), nullptr, nullptr).error;
}

bool decoding() noexcept {
    // HELLO as the Gateway sends it: atom keys in term order, nil atoms.
    Term hello;
    hello.version().map(4).atom("d").map(2).atom("_trace").list(1).binary("[\"gateway-test-1\",{\"micros\":0.0}]")
        .nil().atom("heartbeat_interval").integer(41250).atom("op").small(10).atom("s").atom("nil").atom("t").atom("nil");
    CHECK(transcodes(hello, R"({"d":{"_trace":["[\"gateway-test-1\",{\"micros\":0.0}]"],"heartbeat_interval":41250},)"
                            R"("op":10,"s":null,"t":null})"));

    // Integers: snowflakes (at or past 2^53) are strings, smaller ones numbers.
    Term ints;
    ints.version().list(9).small(0).integer(-7).integer(2147483647).big(4294967295ull).big(1759800000000ull)
        .big(9007199254740991ull).big(9007199254740992ull).big(1097750000000000001ull).big(5, true).nil();
    CHECK(transcodes(ints, R"([0,-7,2147483647,4294967295,1759800000000,9007199254740991,"9007199254740992",)"
                           R"("1097750000000000001",-5])"));
    Term large;
    large.version().b(111).be32(8).b(0);
    for (u32 i = 0; i < 8; ++i)
        large.b(0xFF);
    CHECK(transcodes(large, R"("18446744073709551615")"));
    // IDs and permission sets are strings whatever their size, as in JSON.
    Term named;
    named.version().map(8).atom("id").small(5).atom("guild_id").integer(300).atom("role_ids").list(2).small(1)
        .big(1ull << 60).nil().atom("permissions").big(1ull << 50).binary("allow").small(0).atom("count").small(3)
        .atom("user").map(1).atom("id").big(7).atom("ids").list(1).small(9).nil();
    CHECK(transcodes(named, R"({"id":"5","guild_id":"300","role_ids":["1","1152921504606846976"],)"
                            R"("permissions":"1125899906842624","allow":"0","count":3,"user":{"id":"7"},"ids":[9]})"));
    Term too_big;
    too_big.version().b(110).b(9).b(0);
    for (u32 i = 0; i < 9; ++i)
        too_big.b(1);
    CHECK(transcode_error(too_big) == EtfError::unsupported_term);

    // Floats: exact 17 significant digits, integral values as integers.
    const struct {
        u64 bits;
        const char* text;
    } floats[] = {
        {0x3FF8000000000000ull, "1.5e0"},
        {0x3FB999999999999Aull, "1.0000000000000001e-1"},
        {0x7E37E43C8800759Cull, "1.0000000000000001e300"},
        {0x0000000000000001ull, "4.9406564584124654e-324"},
        {0xC006000000000000ull, "-2.75e0"},
        {0x419D6F3454800000ull, "1.23456789125e8"},
        {0x3E7AD7F29ABCAF48ull, "9.9999999999999995e-8"},
        {0x4340000000000000ull, "9007199254740992"},
        {0x4278BCFE56800000ull, "1700000000000"},
        {0x8000000000000000ull, "0"},
        {0x7FF0000000000000ull, "null"},
    };
    for (const auto& f : floats) {
        Term t;
        t.version().real(f.bits);
        CHECK(transcodes(t, f.text));
    }
    Term old_float;
    old_float.version().b(99).raw("1.50000000000000000000e+00", 26);
    for (u32 i = 26; i < 31; ++i)
        old_float.b(0);
    CHECK(transcodes(old_float, "1.50000000000000000000e+00"));
    old_float.data[2] = 'x';
    CHECK(transcode_error(old_float) == EtfError::unsupported_term);

    // Atoms and strings. ATOM_EXT is Latin-1; binaries are UTF-8 with
    // invalid bytes replaced.
    Term text;
    text.version().list(9).small_atom("true").small_atom("false").small_atom("null").atom("caf\xe9")
        .b(119).b(5).raw("\xc3\xa9t\xc3\xa9", 5).binary("q\"b\\n\n\x01\x1f\t").binary("\xf0\x9f\x94\xa5 \xff\xc3(", 8)
        .binary("").binary("0123456789abcdefghij\"0123456789abcdefghij").nil();
    CHECK(transcodes(text, "[true,false,null,\"caf\xc3\xa9\",\"\xc3\xa9t\xc3\xa9\",\"q\\\"b\\\\n\\n\\u0001\\u001f\\t\","
                           "\"\xf0\x9f\x94\xa5 \xef\xbf\xbd\xef\xbf\xbd(\",\"\",\"0123456789abcdefghij\\\"0123456789abcdefghij\"]"));

    // Byte lists (STRING_EXT), tuples, empty containers and keys of every kind.
    Term mixed;
    mixed.version().map(7).atom("ranges").b(107).b(0).b(3).b(0).b(99).b(200).binary("pair").b(104).b(2).small(1)
        .binary("x").small(5).nil().integer(-3).map(0).big(1097750000000000001ull).list(0).nil().atom("nil").small(1)
        .b(119).b(2).raw("\xc3\xa9", 2).small(2);
    CHECK(transcodes(mixed, R"({"ranges":[0,99,200],"pair":[1,"x"],"5":[],"-3":{},"1097750000000000001":[],)"
                            "\"null\":1,\"\xc3\xa9\":2}"));

    // Malformed terms.
    Term bad;
    bad.b(130).small(1);
    CHECK(transcode_error(bad) == EtfError::bad_version);
    Term truncated;
    truncated.version().list(2).small(1);
    CHECK(transcode_error(truncated) == EtfError::truncated);
    Term improper;
    improper.version().list(1).small(1).small(2);
    CHECK(transcode_error(improper) == EtfError::improper_list);
    Term pid;
    pid.version().b(103).small_atom("a");
    CHECK(transcode_error(pid) == EtfError::unsupported_term);
    Term compressed;
    compressed.version().b(80).be32(1).b(0x78);
    CHECK(transcode_error(compressed) == EtfError::unsupported_term);
    Term list_key;
    list_key.version().map(1).nil().small(1);
    CHECK(transcode_error(list_key) == EtfError::invalid_key);
    Term trailing;
    trailing.version().small(1).small(2);
    CHECK(transcode_error(trailing) == EtfError::trailing_data);
    Term deep;
    deep.version();
    for (u32 i = 0; i <= kEtfMaximumDepth; ++i)
        deep.list(1);
    deep.small(1);
    for (u32 i = 0; i <= kEtfMaximumDepth; ++i)
        deep.nil();
    CHECK(transcode_error(deep) == EtfError::depth_limit);
    Term wide;
    wide.version().map(0x7FFFFFFF);
    CHECK(transcode_error(wide) == EtfError::truncated);
    return true;
}

u8 grown[1 << 16];
u32 grow_calls;
bool grow(void*, u32 required, Slice<u8>* output) noexcept {
    if (required > sizeof(grown))
        return false;
    ++grow_calls;
    *output = {grown, required};
    return true;
}

bool growth() noexcept {
    Term t;
    t.version().list(40);
    for (u32 i = 0; i < 40; ++i)
        t.binary("message content of a synthetic payload");
    t.nil();
    Slice<u8> output{grown, 16};
    auto result = etf_to_json({t.data, t.used}, &output, sizeof(grown), grow, nullptr);
    CHECK(result.ok() && grow_calls != 0 && output.data == grown && result.bytes == 2 + 40 * 40 + 39);
    // Without growth, or past the maximum, the output overflows.
    output = {grown, 16};
    CHECK(etf_to_json({t.data, t.used}, &output, sizeof(grown), nullptr, nullptr).error == EtfError::output_capacity);
    output = {grown, 16};
    CHECK(etf_to_json({t.data, t.used}, &output, 100, grow, nullptr).error == EtfError::output_capacity);
    return true;
}

u8 etf[4096];
bool encodes(const char* text, const Term& expected) noexcept {
    const auto result = etf_from_json({text, c_string_length(text)}, {etf, sizeof(etf)});
    return result.ok() && result.bytes == expected.used && bytes_equal(etf, expected.data, expected.used);
}

bool encoding() noexcept {
    // Heartbeat: binary keys, small integer.
    Term heartbeat;
    heartbeat.version().map(2).binary("op").small(1).binary("d").integer(251000);
    CHECK(encodes(R"({"op":1,"d":251000})", heartbeat));
    // Numbers by range, literals as small atoms, empty and filled arrays.
    // 2^31 and 2^32 - 1 are small bigs, integers past uint32 floats.
    Term values;
    values.version().list(11).small(255).integer(256).integer(-1).integer(-2147483647 - 1);
    values.b(110).b(4).b(0).b(0).b(0).b(0).b(0x80);
    values.b(110).b(4).b(0).b(0xFF).b(0xFF).b(0xFF).b(0xFF);
    values.real(0x4278BCFE56800000ull).real(0xC278BCFE56800000ull).small_atom("nil").small_atom("true")
        .small_atom("false").nil();
    CHECK(encodes(R"([255,256,-1,-2147483648,2147483648,4294967295,1700000000000,-1700000000000,null,true,false])",
                  values));
    Term nested;
    nested.version().map(3).binary("d").map(2).binary("guild_versions").map(0).binary("list").list(2).small(1)
        .list(1).binary("\"\n\xc3\xa9").nil().nil().binary("e").nil().binary("s").binary("");
    CHECK(encodes("{ \"d\" : {\"guild_versions\":{},\"list\":[1,[\"\\\"\\n\\u00e9\"]]}, \"e\":[], \"s\":\"\"}", nested));
    // Fractions, exponents, unsafe integers and malformed JSON are refused.
    const char* refused[] = {"1.5", "1e3", "9007199254740992", "[1,]", "{\"a\" 1}", "01", "{} x", "nul", "\"open"};
    for (const char* text : refused)
        CHECK(etf_from_json({text, c_string_length(text)}, {etf, sizeof(etf)}).error == EtfError::invalid_json);
    CHECK(etf_from_json({"[1,2,3]", 7}, {etf, 9}).error == EtfError::output_capacity);
    CHECK(etf_from_json({"[1,2,3]", 7}, {etf, 13}).ok());

    // Commands round-trip to the same JSON.
    const char* command = R"({"op":2,"d":{"token":"t","capabilities":1734397,"properties":{"os":"Windows",)"
                          R"("client_build_number":627798,"client_event_source":null,"is_fast_connect":true},)"
                          R"("client_state":{"guild_versions":{}},"ids":["1097750000000000001"],"ranges":[[0,99]]}})";
    const auto packed = etf_from_json({command, c_string_length(command)}, {etf, sizeof(etf)});
    CHECK(packed.ok());
    Slice<u8> output{json, sizeof(json)};
    const auto unpacked = etf_to_json({etf, packed.bytes}, &output, sizeof(json), nullptr, nullptr);
    CHECK(unpacked.ok() && unpacked.bytes == c_string_length(command) && bytes_equal(json, command, unpacked.bytes));
    return true;
}

bool run() noexcept {
    CHECK(decoding());
    CHECK(growth());
    CHECK(encoding());
    return true;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_etf_transcode_entry() noexcept {
    using namespace maicord;
    const bool ok = run();
    print("checks=");
    number(checks);
    print(ok ? " PASS ETF transcoding\n" : " FAIL ETF transcoding\n");
    ExitProcess(ok && print_ok ? 0 : 1);
}
