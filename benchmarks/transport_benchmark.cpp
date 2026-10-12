#include "maicord/app/cdn_client.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/core/time.hpp"
#include "maicord/protocol/writer.hpp"

#include <Windows.h>

namespace maicord {
namespace {
void emit(const char* kind, u64 index, u64 microseconds, u64 bytes, u64 status, u64 reused,
          u64 checksum = 0, u64 wire_bytes = 0, u64 decode_us = 0) noexcept {
    u8 buffer[512]{};
    ProtocolWriter w{{buffer, sizeof(buffer)}, 0};
    protocol_writer_append_literal(&w, "{\"kind\":\"");
    protocol_writer_append_c_string(&w, kind);
    const auto field = [&](const char* name, u64 value) noexcept {
        protocol_writer_append_c_string(&w, name);
        protocol_writer_append_u64(&w, value);
    };
    field("\",\"index\":", index);
    field(",\"elapsed_us\":", microseconds);
    field(",\"bytes\":", bytes);
    field(",\"status\":", status);
    field(",\"reused\":", reused);
    field(",\"checksum\":", checksum);
    field(",\"wire_bytes\":", wire_bytes);
    field(",\"decode_us\":", decode_us);
    protocol_writer_append_literal(&w, "}\n");
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buffer, w.used, &written, nullptr);
}
u32 hash(ConstSlice<u8> body) noexcept {
    u32 value = 2166136261u;
    for (u32 n = 0; n < body.count; ++n)
        value = (value ^ body[n]) * 16777619u;
    return value;
}
struct Lane {
    CdnClient cdn;
    RestClient rest;
    HANDLE notification;
    bool active;
};
int run_mixed(u32 rest_count) noexcept {
    Lane lanes[6]{};
    RestSharedState shared{};
    HANDLE handles[6]{};
    u8 body[262144]{};
    u32 rest_queued = 0, cdn_queued = 0, rest_done = 0, cdn_done = 0, initialized = 0;
    int error = 0;
    const Clock clock = clock_initialize();
    const i64 begin = clock_now();
    u64 rest_end = 0, cdn_end = 0;
    for (u32 n = 0; n < rest_count + 4; ++n) {
        bool ok = n < rest_count ? rest_client_start_public(&lanes[n].rest, sizeof(body), &shared).ok()
                                 : cdn_client_start(&lanes[n].cdn, sizeof(body)).ok();
        if (!ok) {
            error = 3;
            break;
        }
        ++initialized;
        handles[n] = n < rest_count ? rest_client_notification_handle(&lanes[n].rest)
                                    : cdn_client_notification_handle(&lanes[n].cdn);
    }
    while (!error && rest_done + cdn_done < 12) {
        for (u32 n = 0; n < initialized; ++n) {
            auto& lane = lanes[n];
            if (lane.active)
                continue;
            bool submitted = false;
            if (n < rest_count && rest_queued < 6) {
                const bool json = (rest_queued & 1) != 0;
                const auto target =
                    json ? utf8_literal("/api/v10/sticker-packs") : utf8_literal("/api/v10/gateway");
                submitted =
                    rest_client_submit(
                        &lane.rest,
                        {{reinterpret_cast<const u8*>(target.data), target.bytes},
                         {},
                         {json ? 2ull : 1ull, 0, RestMethod::get, RestMajorParameter::none},
                         ++rest_queued,
                         8000,
                         false})
                        .ok();
            } else if (n >= rest_count && cdn_queued < 6) {
                char target[] = "/embed/avatars/0.png";
                target[15] = static_cast<char>('0' + cdn_queued);
                submitted = cdn_client_submit(
                                &lane.cdn, {{reinterpret_cast<const u8*>(target), sizeof(target) - 1},
                                            {},
                                            101 + cdn_queued,
                                            8000,
                                            CdnHost::cdn,
                                            CdnImageType::png})
                                .ok();
                ++cdn_queued;
            } else
                continue;
            if (!submitted) {
                error = 5;
                break;
            }
            lane.active = true;
        }
        if (error)
            break;
        const DWORD ready = WaitForMultipleObjects(initialized, handles, FALSE, 10000);
        if (ready >= WAIT_OBJECT_0 + initialized) {
            error = 6;
            break;
        }
        const u32 n = ready - WAIT_OBJECT_0;
        auto& lane = lanes[n];
        bool ok = false;
        if (n < rest_count) {
            RestClientResponse response{};
            const auto taken =
                rest_client_take_response(&lane.rest, &response, {body, sizeof(body)});
            emit("rest", response.request_id, response.elapsed_us, taken.body_bytes, response.status_code,
                 response.reused_connection, hash({body, taken.body_bytes}), response.wire_body_bytes,
                 response.decode_us);
            ok = taken.ok() && response.transport_ok() && response.status_code == 200;
            if (++rest_done == 6)
                rest_end = clock_microseconds(clock, begin, clock_now());
        } else {
            CdnClientResponse response{};
            const auto taken = cdn_client_take_response(&lane.cdn, &response, {body, sizeof(body)});
            emit("cdn", response.request_id, response.elapsed_us, taken.body_bytes, response.status_code,
                 response.reused_connection, hash({body, taken.body_bytes}));
            ok = taken.ok() && response.transport_ok() && response.status_code == 200;
            if (++cdn_done == 6)
                cdn_end = clock_microseconds(clock, begin, clock_now());
        }
        if (!ok)
            error = 7;
        lane.active = false;
    }
    const u64 elapsed = clock_microseconds(clock, begin, clock_now());
    emit("rest_complete", rest_done, rest_end, 0, error, 0);
    emit("cdn_complete", cdn_done, cdn_end, 0, error, 0);
    // Separate idle sample: owners remain connected but have no work queued.
    Sleep(50); // Let final IOCP completions and CPU accounting settle first.
    u64 cycles_before = 0;
    for (u32 n = 0; n < initialized; ++n) {
        ULONG64 cycles = 0;
        QueryThreadCycleTime(n < rest_count ? lanes[n].rest.worker.thread : lanes[n].cdn.worker.thread, &cycles);
        cycles_before += cycles;
    }
    FILETIME created{}, exited{}, kernel_before{}, user_before{}, kernel_after{}, user_after{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel_before, &user_before);
    const i64 idle_begin = clock_now();
    wchar_t long_idle[2]{};
    Sleep(GetEnvironmentVariableW(L"MAICORD_TRANSPORT_LONG_IDLE", long_idle, 2) ? 2000 : 250);
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel_after, &user_after);
    u64 cycles_after = 0;
    for (u32 n = 0; n < initialized; ++n) {
        ULONG64 cycles = 0;
        QueryThreadCycleTime(n < rest_count ? lanes[n].rest.worker.thread : lanes[n].cdn.worker.thread, &cycles);
        cycles_after += cycles;
    }
    emit("idle_worker_cycles", initialized, 0, cycles_after - cycles_before, 0, 0);
    const auto ticks = [](FILETIME t) noexcept {
        return (static_cast<u64>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    emit("idle_cpu", 0,
         (ticks(kernel_after) + ticks(user_after) - ticks(kernel_before) - ticks(user_before)) / 10,
         clock_microseconds(clock, idle_begin, clock_now()), 0, 0);
    u64 allocated = sizeof(shared);
    for (u32 n = 0; n < initialized; ++n) {
        if (n < rest_count) {
            allocated += rest_client_runtime_bytes(sizeof(body));
            rest_client_stop(&lanes[n].rest, 10000);
        } else
            cdn_client_stop(&lanes[n].cdn, 10000);
    }
    emit("rest_runtime_bytes", rest_count, 0, allocated, 0, 0);
    emit("batch", rest_done + cdn_done, elapsed, 0, error, 0);
    return error;
}
int run() noexcept {
    wchar_t mode[16]{};
    // This opt-in probe has no credential or account-file path. Its only URLs
    // are public gateway discovery and the six built-in default avatars.
    const DWORD length = GetEnvironmentVariableW(L"MAICORD_TRANSPORT_BENCH", mode, 16);
    if (length == 6 && mode[0] == L'm' && mode[1] == L'i' && mode[2] == L'x' && mode[3] == L'e' &&
        mode[4] == L'd' && (mode[5] == L'1' || mode[5] == L'2'))
        return run_mixed(mode[5] - L'0');
    const bool json = length == 5 && mode[0] == L'j' && mode[1] == L's' && mode[2] == L'o' &&
                      mode[3] == L'n' && (mode[4] == L'0' || mode[4] == L'1');
    const bool rest =
        json || (length == 5 && mode[0] == L'r' && mode[1] == L'e' && mode[2] == L's' && mode[3] == L't');
    const bool cdn = length == 4 && mode[0] == L'c' && mode[1] == L'd' && mode[2] == L'n';
    const u32 count = json ? 1 : rest ? mode[4] - L'0' : cdn ? mode[3] - L'0' : 0;
    if ((!rest && !cdn) || count < 1 || count > 4)
        return 2;
    Lane lanes[4]{};
    HANDLE notifications[4]{};
    u8 body[262144]{};
    const Clock clock = clock_initialize();
    const i64 started = clock_now();
    u32 initialized = 0, queued = 0, completed = 0;
    int failure = 0;
    for (; initialized < count; ++initialized) {
        auto& lane = lanes[initialized];
        const bool ok = rest ? rest_client_start_public(&lane.rest, sizeof(body)).ok()
                             : cdn_client_start(&lane.cdn, sizeof(body)).ok();
        if (!ok) {
            failure = 3;
            break;
        }
        notifications[initialized] = rest ? rest_client_notification_handle(&lane.rest)
                                          : cdn_client_notification_handle(&lane.cdn);
    }
    while (!failure && completed < 6) {
        for (u32 n = 0; n < count && queued < 6; ++n) {
            auto& lane = lanes[n];
            if (lane.active)
                continue;
            bool ok = false;
            if (rest) {
                const auto target =
                    json ? utf8_literal("/api/v10/sticker-packs") : utf8_literal("/api/v10/gateway");
                const RestClientRequest request{
                    {reinterpret_cast<const u8*>(target.data), target.bytes},
                    {},
                    {1, 0, RestMethod::get, RestMajorParameter::none},
                    queued + 1,
                    8000,
                    false,
                    json && mode[4] == L'0'};
                ok = rest_client_submit(&lane.rest, request).ok();
            } else {
                char target[] = "/embed/avatars/0.png";
                target[15] = static_cast<char>('0' + queued);
                const CdnClientRequest request{
                    {reinterpret_cast<const u8*>(target), sizeof(target) - 1},
                    {},
                    queued + 1,
                    8000,
                    CdnHost::cdn,
                    CdnImageType::png};
                ok = cdn_client_submit(&lane.cdn, request).ok();
            }
            if (!ok) {
                failure = 4;
                break;
            }
            lane.active = true;
            ++queued;
        }
        if (failure)
            break;
        const DWORD ready = WaitForMultipleObjects(count, notifications, FALSE, 10000);
        if (ready < WAIT_OBJECT_0 || ready >= WAIT_OBJECT_0 + count) {
            failure = 5;
            break;
        }
        auto& lane = lanes[ready - WAIT_OBJECT_0];
        bool ok = false;
        if (rest) {
            RestClientResponse response{};
            const auto taken =
                rest_client_take_response(&lane.rest, &response, {body, sizeof(body)});
            emit("request", response.request_id, response.elapsed_us, taken.body_bytes,
                 response.status_code, response.reused_connection, hash({body, taken.body_bytes}),
                 response.wire_body_bytes, response.decode_us);
            ok = taken.ok() && response.transport_ok() && response.status_code == 200 &&
                 taken.body_bytes != 0;
        } else {
            CdnClientResponse response{};
            const auto taken = cdn_client_take_response(&lane.cdn, &response, {body, sizeof(body)});
            emit("request", response.request_id, response.elapsed_us, taken.body_bytes,
                 response.status_code, response.reused_connection, hash({body, taken.body_bytes}));
            ok = taken.ok() && response.transport_ok() && response.status_code == 200 &&
                 taken.body_bytes != 0;
        }
        if (!ok) {
            failure = 6;
            break;
        }
        lane.active = false;
        ++completed;
    }
    const u64 elapsed = clock_microseconds(clock, started, clock_now());
    u64 connections = 0, reused = 0;
    for (u32 n = 0; n < initialized; ++n) {
        if (rest) {
            const auto result = rest_client_stop(&lanes[n].rest, 10000);
            connections += result.connections;
            reused += result.reused_requests;
        } else {
            const auto result = cdn_client_stop(&lanes[n].cdn, 10000);
            connections += result.connections;
            reused += result.reused_requests;
        }
    }
    emit("batch", completed, elapsed, connections, failure, reused);
    return failure;
}
} // namespace
} // namespace maicord
extern "C" void maicord_transport_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
