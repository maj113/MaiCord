#include "maicord/core/time.hpp"
#include "maicord/protocol/rest.hpp"

#include <Windows.h>

namespace maicord {
namespace {

inline constexpr u32 kRequestIterations = 5'000'000;
inline constexpr u32 kHeadIterations = 1'000'000;
inline constexpr u32 kResponseIterations = 500'000;
inline constexpr u32 kSchedulerIterations = 1'000'000;

u8 g_request[512]{};
u8 g_response_input[kRestMaximumHeaderBytes]{};
u8 g_response_body[256]{};
RestRateBucket g_buckets[8]{};
RestRateBinding g_bindings[16]{};
volatile u64 g_rest_checksum = 0;

u32 text_length(const char* value) noexcept {
    u32 length = 0;
    while (value[length]) {
        ++length;
    }
    return length;
}

void write_text(const char* value) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), value, text_length(value),
              &written, nullptr);
}

void write_u64(u64 value) noexcept {
    char digits[24]{};
    u32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    char output[24]{};
    for (u32 index = 0; index < count; ++index) {
        output[index] = digits[count - index - 1];
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, count, &written,
              nullptr);
}

void write_metric(const char* name, u64 value) noexcept {
    write_text(name);
    write_text("=");
    write_u64(value);
    write_text(" ops/s\r\n");
}

template <u32 Count>
constexpr ConstSlice<u8> bytes(const char (&value)[Count]) noexcept {
    return ConstSlice<u8>{reinterpret_cast<const u8*>(value), Count - 1};
}

int run() noexcept {
    const Clock clock = clock_initialize();
    u64 checksum = 0;
    const RestRequestHead request{
        bytes("/api/v10/channels/1260000000000000001/messages?limit=100"),
        bytes("benchmark.token"), 0, RestMethod::get,
        RestAuthorization::bearer, false, false};
    i64 begin = clock_now();
    for (u32 iteration = 0; iteration < kRequestIterations; ++iteration) {
        const RestRequestWriteResult written =
            write_rest_request_head(request,
                                    Slice<u8>{g_request, 512});
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
        RestRateAcquireResult acquired =
            rest_rate_acquire(&scheduler, route, iteration + 1);
        if (!acquired.acquired_request()) {
            return 7;
        }
        const RestRateObservation observed =
            rest_rate_observe(
                &scheduler, &acquired.lease, scheduler_head, nullptr,
                iteration + 1, 1'000'000 + iteration);
        if (!observed.ok()) {
            return 8;
        }
        checksum += scheduler.acquired + scheduler.observations;
    }
    const u64 scheduler_us =
        clock_microseconds(clock, begin, clock_now());

    g_rest_checksum = checksum;
    if (request_us == 0 || head_us == 0 || response_us == 0 ||
        scheduler_us == 0 || g_rest_checksum == 0) {
        return 9;
    }
    write_metric("rest_request_head",
                 static_cast<u64>(kRequestIterations) * 1'000'000ull /
                     request_us);
    write_metric("rest_rate_head",
                 static_cast<u64>(kHeadIterations) * 1'000'000ull /
                     head_us);
    write_metric("rest_response",
                 static_cast<u64>(kResponseIterations) * 1'000'000ull /
                     response_us);
    write_metric("rest_rate_cycle",
                 static_cast<u64>(kSchedulerIterations) * 1'000'000ull /
                     scheduler_us);
    return 0;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_rest_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
