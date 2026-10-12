// Manual live-network probe: built only with -DMAICORD_BUILD_INTEGRATION_TESTS=ON
// (cmake/integration.cmake) and run by hand or through tools/probe_*.ps1; it is
// never part of the CTest suite because it talks to the real service.
#include "maicord/app/rest_client.hpp"
#include "maicord/core/ascii.hpp"
#include "../test_output.hpp"

namespace maicord {
namespace {

RestClient g_client{};
u8 g_body[4 * 1024]{};
SRWLOCK g_trace_lock{};
u32 g_trace_starts, g_trace_completes, g_trace_consumes, g_trace_busy, g_trace_first_bytes;
u32 g_trace_gates;
u64 g_trace_max_hold_us;
bool g_trace_leaked;

bool starts_with(const char* text, const char* prefix) noexcept {
    while (*prefix) if (*text++ != *prefix++) return false;
    return true;
}
void trace_record(const char* record) noexcept {
    AcquireSRWLockExclusive(&g_trace_lock);
    g_trace_starts += starts_with(record, "start ");
    g_trace_completes += starts_with(record, "complete ");
    g_trace_consumes += starts_with(record, "consumed ");
    g_trace_busy += starts_with(record, "submit_busy ");
    g_trace_first_bytes += starts_with(record, "first_byte ");
    g_trace_gates += starts_with(record, "gate_wait ");
    u32 count = 0;
    for (; record[count]; ++count) {
        if (starts_with(record + count, "never-log-this") || starts_with(record + count, "/api/"))
            g_trace_leaked = true;
        if (starts_with(record + count, "ready_hold_us=")) {
            const char* value = record + count + 14;
            u64 duration = 0;
            while (*value >= '0' && *value <= '9') duration = duration * 10 + static_cast<u64>(*value++ - '0');
            g_trace_max_hold_us = maximum(g_trace_max_hold_us, duration);
        }
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), record, count, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\r\n", 2, &written, nullptr);
    ReleaseSRWLockExclusive(&g_trace_lock);
}

u32 request_gateway(u64 request_id, bool expect_reuse,
                    RestClientResponse* response) noexcept {
    const RestClientSubmitResult submitted =
        rest_client_submit(
            &g_client,
            RestClientRequest{
                ConstSlice<u8>{
                    reinterpret_cast<const u8*>(
                        "/api/v10/gateway"),
                    16},
                {},
                RestRouteKey{
                    1, 0, RestMethod::get,
                    RestMajorParameter::none},
                request_id, 20'000, false});
    if (!submitted.ok()) {
        return 1u + static_cast<u32>(submitted.error);
    }
    // No extra network request: the occupied slot must reject both attempts.
    constexpr char canary[] = "/api/v10/gateway?trace=never-log-this";
    for (u32 retry = 0; retry < 2; ++retry) {
        const auto busy = rest_client_submit(&g_client,
            {{reinterpret_cast<const u8*>(canary), sizeof(canary) - 1}, {},
             {1, 0, RestMethod::get, RestMajorParameter::none}, request_id + 1000, 20'000});
        if (busy.error != RestClientSubmitError::busy) return 9;
    }
    const RestClientResult waited =
        rest_client_wait_response(&g_client, 25'000);
    if (!waited.ok() ||
        waited.phase != RestClientPhase::response_ready) {
        return 10u + static_cast<u32>(waited.error) * 32u +
               static_cast<u32>(waited.phase);
    }
    const RestClientTakeResult taken =
        rest_client_take_response(
            &g_client, response, Slice<u8>{g_body, 4 * 1024});
    if (!taken.ok()) {
        return 100u + static_cast<u32>(taken.error);
    }
    if (!response->transport_ok()) {
        return 110u + static_cast<u32>(response->error);
    }
    if (response->status_code != 200) {
        return 140u + response->status_code;
    }
    if (response->body_bytes == 0) {
        return 150u;
    }
    if (response->reused_connection != expect_reuse) {
        return 151u;
    }
    // Get Gateway answers {"url": "wss://..."}.
    return ascii_contains_ignore_case(Utf8View{reinterpret_cast<const char*>(g_body), response->body_bytes},
                                      utf8_literal("wss://"))
               ? 0u
               : 240u;
}

u32 cancel_gateway_request() noexcept {
    const RestClientSubmitResult submitted =
        rest_client_submit(
            &g_client,
            RestClientRequest{
                ConstSlice<u8>{
                    reinterpret_cast<const u8*>(
                        "/api/v10/gateway"),
                    16},
                {},
                RestRouteKey{
                    1, 0, RestMethod::get,
                    RestMajorParameter::none},
                3, 20'000, false});
    if (!submitted.ok()) {
        return 1u + static_cast<u32>(submitted.error);
    }
    bool active = false;
    for (u32 poll = 0; poll < 5'000; ++poll) {
        const RestClientResult status =
            rest_client_status(&g_client);
        if (status.phase == RestClientPhase::rate_wait ||
            status.phase == RestClientPhase::resolving ||
            status.phase == RestClientPhase::connecting ||
            status.phase == RestClientPhase::tls_handshake ||
            status.phase == RestClientPhase::sending ||
            status.phase == RestClientPhase::receiving) {
            active = true;
            break;
        }
        if (status.phase == RestClientPhase::response_ready ||
            status.phase == RestClientPhase::failed) {
            return 10u + static_cast<u32>(status.phase);
        }
        Sleep(1);
    }
    if (!active) {
        return 40u;
    }
    if (!rest_client_cancel(&g_client, 3)) {
        return 41u;
    }
    const RestClientResult waited =
        rest_client_wait_response(&g_client, 25'000);
    RestClientResponse response{};
    const RestClientTakeResult taken =
        rest_client_take_response(
            &g_client, &response, Slice<u8>{g_body, 4 * 1024});
    if (!waited.ok() ||
        waited.phase != RestClientPhase::response_ready) {
        return 20u + static_cast<u32>(waited.error) * 32u +
               static_cast<u32>(waited.phase);
    }
    if (!taken.ok()) {
        return 100u + static_cast<u32>(taken.error);
    }
    if (taken.body_bytes != 0) {
        return 110u;
    }
    if (response.request_id != 3) {
        return 111u;
    }
    return response.error == RestClientError::cancelled
               ? 0u
               : 120u + static_cast<u32>(response.error);
}

bool probe_shared_workers() noexcept {
    RestClient primary{}, details{};
    RestSharedState shared{};
    bool ok = rest_client_start_public(&primary, 4096, &shared).ok() &&
              rest_client_start_public(&details, 4096, &shared).ok();
    const u32 gates_before = g_trace_gates;
    constexpr char target[] = "/api/v10/gateway";
    RestClientRequest request{{reinterpret_cast<const u8*>(target), sizeof(target) - 1}, {},
        {1, 0, RestMethod::get, RestMajorParameter::none}, 4, 20'000};
    if (ok) ok = rest_client_submit(&primary, request).ok();
    request.request_id = 5;
    if (ok) ok = rest_client_submit(&details, request).ok();
    // Both slots are occupied, even while a worker awaits the shared route.
    request.request_id = 6;
    if (ok) ok = rest_client_submit(&primary, request).error == RestClientSubmitError::busy;
    if (ok) ok = rest_client_wait_response(&primary, 25'000).phase == RestClientPhase::response_ready &&
                 rest_client_wait_response(&details, 25'000).phase == RestClientPhase::response_ready;
    if (ok) {
        Sleep(25); // Exercise measurable UI consumption delay, only in this probe.
        RestClientResponse a{}, b{};
        ok = rest_client_take_response(&primary, &a, {g_body, sizeof(g_body)}).ok() &&
             rest_client_take_response(&details, &b, {g_body, sizeof(g_body)}).ok() &&
             a.status_code == 200 && b.status_code == 200 &&
             g_trace_gates > gates_before && g_trace_max_hold_us >= 20'000 && !g_trace_leaked;
    }
    if (primary.worker.thread) ok = rest_client_stop(&primary, 5000).ok() && ok;
    if (details.worker.thread) ok = rest_client_stop(&details, 5000).ok() && ok;
    return ok;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI
maicord_rest_client_probe_entry() noexcept {
    maicord::rest_client_set_trace(maicord::trace_record);
    maicord::RestClientResult status =
        maicord::rest_client_start_public(
            &maicord::g_client, 4 * 1024);
    maicord::RestClientResponse first{};
    maicord::RestClientResponse reused{};
    maicord::u32 failure_stage = 0;
    if (!status.ok()) {
        failure_stage = 100u + static_cast<maicord::u32>(status.error);
    }
    if (failure_stage == 0) {
        const maicord::u32 request_result =
            maicord::request_gateway(1, false, &first);
        if (request_result != 0) {
            failure_stage = 1000u + request_result;
        }
    }
    if (failure_stage == 0) {
        const maicord::u32 request_result =
            maicord::request_gateway(2, true, &reused);
        if (request_result != 0) {
            failure_stage = 2000u + request_result;
        }
    }
    status = maicord::rest_client_status(
        &maicord::g_client);
    if (failure_stage == 0 &&
        (status.connections != 1 || status.requests != 2 ||
         status.reused_requests != 1 || status.responses != 2)) {
        failure_stage = 3000u;
    }
    if (failure_stage == 0) {
        const maicord::RestClientResult stopped =
            maicord::rest_client_stop(
                &maicord::g_client, 5'000);
        if (stopped.phase !=
            maicord::RestClientPhase::stopped) {
            failure_stage = 3500u +
                            static_cast<maicord::u32>(stopped.error);
        }
    }
    if (failure_stage == 0) {
        const maicord::RestClientResult restarted =
            maicord::rest_client_start_public(
                &maicord::g_client, 4 * 1024);
        if (!restarted.ok()) {
            failure_stage = 3600u +
                            static_cast<maicord::u32>(restarted.error);
        }
    }
    if (failure_stage == 0) {
        const maicord::u32 cancel_result =
            maicord::cancel_gateway_request();
        if (cancel_result != 0) {
            failure_stage = 4000u + cancel_result;
        }
    }
    if (!failure_stage && (maicord::g_trace_starts < 2 || maicord::g_trace_completes < 3 ||
        maicord::g_trace_consumes < 3 || maicord::g_trace_busy != 2 ||
        maicord::g_trace_first_bytes < 2 || maicord::g_trace_leaked)) failure_stage = 4500;
    if (!failure_stage && !maicord::probe_shared_workers()) failure_stage = 4600;
    bool ok = failure_stage == 0;
    const maicord::u32 runtime_bytes =
        maicord::rest_client_runtime_bytes(4 * 1024);

    if (ok) {
        maicord::test_write_text(
            "PASS rest_async first_us=");
        maicord::test_write_u64(first.elapsed_us);
        maicord::test_write_text(" reused_us=");
        maicord::test_write_u64(reused.elapsed_us);
        maicord::test_write_text(" connections=");
        maicord::test_write_u64(status.connections);
        maicord::test_write_text(" runtime_bytes=");
        maicord::test_write_u64(runtime_bytes);
        maicord::test_write_text("\r\n");
    } else {
        maicord::test_write_text("FAIL rest_async stage=");
        maicord::test_write_u64(failure_stage);
        maicord::test_write_text(" phase=");
        maicord::test_write_u64(static_cast<maicord::u32>(status.phase));
        maicord::test_write_text(" error=");
        maicord::test_write_u64(static_cast<maicord::u32>(status.error));
        maicord::test_write_text(" native=");
        maicord::test_write_u64(status.native_error);
        maicord::test_write_text(" connections=");
        maicord::test_write_u64(status.connections);
        maicord::test_write_text(" requests=");
        maicord::test_write_u64(status.requests);
        maicord::test_write_text(" reused=");
        maicord::test_write_u64(status.reused_requests);
        maicord::test_write_text(" responses=");
        maicord::test_write_u64(status.responses);
        maicord::test_write_text(" first_error=");
        maicord::test_write_u64(static_cast<maicord::u32>(first.error));
        maicord::test_write_text(" first_native=");
        maicord::test_write_u64(first.native_error);
        maicord::test_write_text(" reused_error=");
        maicord::test_write_u64(static_cast<maicord::u32>(reused.error));
        maicord::test_write_text(" reused_native=");
        maicord::test_write_u64(reused.native_error);
        maicord::test_write_text("\r\n");
    }

    if (maicord::g_client.worker.thread) {
        const maicord::RestClientResult stopped =
            maicord::rest_client_stop(
                &maicord::g_client, 5'000);
        ok = ok && stopped.phase ==
                       maicord::RestClientPhase::stopped;
    }
    ExitProcess(ok ? 0 : 1);
}
