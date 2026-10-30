#include "maicord/core/time.hpp"
#include "maicord/protocol/rest.hpp"
#include "benchmark_output.hpp"

#include <Windows.h>

namespace maicord {
namespace {

inline constexpr u32 kRequestIterations = 5'000'000;
inline constexpr u32 kHeadIterations = 1'000'000;
inline constexpr u32 kResponseIterations = 500'000;
inline constexpr u32 kSchedulerIterations = 1'000'000;

u8 g_request[2048]{};
u8 g_response_input[kRestMaximumHeaderBytes]{};
u8 g_response_body[256]{};
RestRateBucket g_buckets[8]{};
RestRateBinding g_bindings[16]{};
volatile u64 g_rest_checksum = 0;

template <u32 Count>
constexpr ConstSlice<u8> bytes(const char (&value)[Count]) noexcept {
    return ConstSlice<u8>{reinterpret_cast<const u8*>(value), Count - 1};
}

int run() noexcept {
    const Clock clock = clock_initialize();
    u64 checksum = 0;
    const RestUserRequestHeaders page{bytes("e30="), utf8_literal("en-US"), utf8_literal("Europe/Ljubljana"),
                                      utf8_literal("en-US,en;q=0.9")};
    const RestRequestHead request{
        bytes("/api/v10/channels/1260000000000000001/messages?limit=100"),
        bytes("benchmark.token"), 0, RestMethod::get,
        RestAuthorization::normal_user, false, &page, true};
    i64 begin = clock_now();
    for (u32 iteration = 0; iteration < kRequestIterations; ++iteration) {
        const RestRequestWriteResult written =
            write_rest_request_head(request,
                                    Slice<u8>{g_request, sizeof(g_request)});
        if (!written.ok()) {
            return 1;
        }
        checksum += written.bytes + g_request[iteration & 255u];
    }
    const u64 request_us = clock_microseconds(clock, begin, clock_now());

    const auto rate_head = bytes(
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
        "X-RateLimit-Limit: 50\r\n"
        "X-RateLimit-Remaining: 49\r\n"
        "X-RateLimit-Reset: 1470173023.123\r\n"
        "X-RateLimit-Reset-After: 1.234\r\n"
        "X-RateLimit-Bucket: benchmark-bucket\r\n\r\n");
    begin = clock_now();
    for (u32 iteration = 0; iteration < kHeadIterations; ++iteration) {
        const RestHeadResult parsed =
            parse_rest_response_head(rate_head);
        if (!parsed.ok()) {
            return 2;
        }
        checksum += parsed.rate.reset_after_ms + parsed.rate.remaining;
    }
    const u64 head_us = clock_microseconds(clock, begin, clock_now());

    const auto chunked = bytes(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
        "X-RateLimit-Limit: 5\r\nX-RateLimit-Remaining: 4\r\n"
        "X-RateLimit-Reset-After: 1\r\n"
        "X-RateLimit-Bucket: response\r\n\r\n"
        "1C\r\n{\"url\":\"wss://gateway.test\"}\r\n0\r\n\r\n");
    begin = clock_now();
    for (u32 iteration = 0; iteration < kResponseIterations; ++iteration) {
        RestResponse response{};
        if (!rest_response_initialize(
                &response,
                Slice<u8>{g_response_input,
                          kRestMaximumHeaderBytes},
                Slice<u8>{g_response_body, 256})) {
            return 3;
        }
        const RestResponseStep step =
            rest_response_feed(&response, chunked);
        if (!step.ok() || step.state != RestResponseState::complete ||
            response.http.body_bytes != 28) {
            return 4;
        }
        checksum += response.http.body_bytes +
                    response.parsed_head.rate.remaining;
    }
    const u64 response_us = clock_microseconds(clock, begin, clock_now());

    RestRateScheduler scheduler{};
    if (!rest_rate_scheduler_initialize(
            &scheduler, Slice<RestRateBucket>{g_buckets, 8},
            Slice<RestRateBinding>{g_bindings, 16})) {
        return 5;
    }
    const RestRouteKey route{
        1, 1'260'000'000'000'000'001ull, RestMethod::get,
        RestMajorParameter::channel};
    const RestHeadResult scheduler_head =
        parse_rest_response_head(bytes(
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
            "X-RateLimit-Limit: 50\r\n"
            "X-RateLimit-Remaining: 49\r\n"
            "X-RateLimit-Reset-After: 1\r\n"
            "X-RateLimit-Bucket: scheduler\r\n\r\n"));
    if (!scheduler_head.ok()) {
        return 6;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kSchedulerIterations;
         ++iteration) {
        // Each iteration starts after the one-second window of the last one.
        const u64 now_ms = (iteration + 1) * 2000ull;
        RestRateAcquireResult acquired = rest_rate_acquire(&scheduler, route, now_ms);
        if (!acquired.acquired_request()) {
            return 7;
        }
        const RestRateObservation observed =
            rest_rate_observe(
                &scheduler, &acquired.lease, scheduler_head, nullptr,
                now_ms, 1'000'000 + iteration);
        if (!observed.ok()) {
            return 8;
        }
        checksum += iteration;
    }
    const u64 scheduler_us =
        clock_microseconds(clock, begin, clock_now());

    g_rest_checksum = checksum;
    if (request_us == 0 || head_us == 0 || response_us == 0 ||
        scheduler_us == 0 || g_rest_checksum == 0) {
        return 9;
    }
    write_ops("rest_request_head", per_second(kRequestIterations, request_us));
    write_ops("rest_rate_head", per_second(kHeadIterations, head_us));
    write_ops("rest_response", per_second(kResponseIterations, response_us));
    write_ops("rest_rate_cycle", per_second(kSchedulerIterations, scheduler_us));
    return 0;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_rest_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
