#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {

// Loopback page that lets the default browser solve a server CAPTCHA. The
// protocol code is pure: it never allocates, logs or retains secrets.
inline constexpr u32 kCaptchaBridgeMaximumHeaderBytes = 16 * 1024;
inline constexpr u32 kCaptchaBridgeMaximumBodyBytes = 8 * 1024;
inline constexpr u32 kCaptchaBridgeStateBytes = 32;  // 128 random bits, lowercase hex
inline constexpr u32 kCaptchaBridgeNonceBytes = 32;  // 128 random bits, lowercase hex
inline constexpr u32 kCaptchaBridgeMinimumTokenBytes = 16;
inline constexpr u32 kCaptchaBridgeMaximumTokenBytes = 4096;
inline constexpr u32 kCaptchaBridgeMaximumSitekeyBytes = 128;
inline constexpr u32 kCaptchaBridgeMaximumRqdataBytes = 4096;
inline constexpr u32 kCaptchaBridgeMaximumUserFlowBytes = 128;
inline constexpr u32 kCaptchaBridgeMaximumUrlBytes = 64;
// Fits the page, maximal challenge data, escaped user flow and response headers.
inline constexpr u32 kCaptchaBridgeMaximumResponseBytes = 24 * 1024;

enum class CaptchaBridgeProvider : u8 { unknown, hcaptcha, recaptcha, recaptcha_enterprise, turnstile };
enum class CaptchaBridgeRequestKind : u8 { page, submit };
enum class CaptchaBridgeParseState : u8 { need_input, complete, rejected };
enum class CaptchaBridgeProtocolError : u8 {
    none, invalid_input, capacity, http, framing, host, origin, fetch_metadata, route, state, json, token,
};
enum class CaptchaBridgeResponse : u8 { accepted, gone };

// token views the caller's input buffer and is valid until it is erased.
struct CaptchaBridgeRequest {
    Utf8View token;
    CaptchaBridgeRequestKind kind;
};
struct CaptchaBridgeParseResult {
    CaptchaBridgeRequest request;
    CaptchaBridgeProtocolError error;
    CaptchaBridgeParseState state;
    constexpr bool ok() const noexcept { return state == CaptchaBridgeParseState::complete; }
};
struct CaptchaBridgeWriteResult {
    u32 bytes;
    CaptchaBridgeProtocolError error;
    constexpr bool ok() const noexcept { return error == CaptchaBridgeProtocolError::none; }
};
struct CaptchaBridgePage {
    Utf8View sitekey;
    Utf8View rqdata;  // optional
    Utf8View state;
    Utf8View nonce;
    u16 port;
    CaptchaBridgeProvider provider;
    bool invisible;
    Utf8View user_flow; // optional bounded UTF-8 enterprise action; empty omits it
};

bool captcha_bridge_sitekey_valid(Utf8View sitekey) noexcept;
bool captcha_bridge_rqdata_valid(Utf8View rqdata) noexcept;
bool captcha_bridge_user_flow_valid(Utf8View user_flow) noexcept;
bool captcha_bridge_token_valid(Utf8View token) noexcept;
bool captcha_bridge_hex_valid(Utf8View value, u32 bytes) noexcept;

// Strict HTTP/1.1: exactly one framed request, no pipelining, no chunking,
// no Expect/Cookie/Authorization, no duplicate framing or security headers.
// GET /c/<state> is a top-level navigation; POST /s/<state> carries
// {"token":"..."} from the same origin. State is compared in constant time.
CaptchaBridgeParseResult captcha_bridge_parse_request(ConstSlice<u8> input, u16 port,
                                                      Utf8View state) noexcept;
// Lowercase hex of exactly 16 random bytes; used for both state and nonce.
CaptchaBridgeWriteResult captcha_bridge_write_hex(ConstSlice<u8> random, Slice<u8> output) noexcept;
CaptchaBridgeWriteResult captcha_bridge_write_url(u16 port, Utf8View state, Slice<u8> output) noexcept;
// Complete HTTP response with a per-page nonce CSP scoped to one provider.
CaptchaBridgeWriteResult captcha_bridge_write_page(const CaptchaBridgePage& page, Slice<u8> output) noexcept;
CaptchaBridgeWriteResult captcha_bridge_write_response(CaptchaBridgeResponse response,
                                                       Slice<u8> output) noexcept;

} // namespace maicord
