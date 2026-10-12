#pragma once

#include "maicord/core/json_reader.hpp"

namespace maicord {

inline constexpr u16 kBrowserSignInFirstPort = 6463;
inline constexpr u16 kBrowserSignInPortCount = 10;
inline constexpr u32 kBrowserSignInMaximumHeaderBytes = 16 * 1024;
inline constexpr u32 kBrowserSignInMaximumBodyBytes = 8 * 1024;
inline constexpr u32 kBrowserSignInMaximumTokenBytes = 4096;
inline constexpr u32 kBrowserSignInMaximumNonceBytes = 64;
inline constexpr u32 kBrowserSignInKeyBytes = 36;
inline constexpr u32 kBrowserSignInStructuralCount = 256;

enum class BrowserSignInRequestKind : u8 { preflight, post, redirect };
enum class BrowserSignInParseState : u8 { need_input, complete, rejected };
enum class BrowserSignInError : u8 {
    none, invalid_input, capacity, http, framing, host, origin, route, preflight, json, payload,
};

// Views are invalid once input/decoded_query is erased or changed. The receiver
// supplies locked storage; the protocol code never allocates or keeps secrets.
struct BrowserSignInRequest {
    JsonString token;
    JsonString nonce;
    u32 consumed_bytes;
    BrowserSignInRequestKind kind;
    bool private_network;
    bool has_token;
};
struct BrowserSignInParseResult {
    BrowserSignInRequest request;
    BrowserSignInError error;
    BrowserSignInParseState state;
    constexpr bool ok() const noexcept { return state == BrowserSignInParseState::complete; }
};
struct BrowserSignInWriteResult {
    u32 bytes;
    BrowserSignInError error;
    constexpr bool ok() const noexcept { return error == BrowserSignInError::none; }
};

// A complete request must be framed exactly: pipelined/trailing bytes are
// rejected. Query decoding and JSON indexes require disjoint caller storage.
BrowserSignInParseResult browser_sign_in_parse_request(ConstSlice<u8> input, u16 port,
    Slice<u8> decoded_query, Slice<u32> structural_scratch) noexcept;
BrowserSignInWriteResult browser_sign_in_write_key(ConstSlice<u8> random, Slice<u8> output) noexcept;
BrowserSignInWriteResult browser_sign_in_write_url(u16 port, Utf8View key, Slice<u8> output) noexcept;
BrowserSignInWriteResult browser_sign_in_write_exchange(Utf8View key, Utf8View token, Slice<u8> output) noexcept;
// Only confirms that the RPC request arrived. Neither this response nor the
// redirect callback shows that the server accepted the key-bound exchange.
BrowserSignInWriteResult browser_sign_in_write_response(const BrowserSignInRequest& request,
    Slice<u8> output) noexcept;

} // namespace maicord
