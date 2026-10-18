// Manual live-network probe: built only with -DMAICORD_BUILD_INTEGRATION_TESTS=ON
// (cmake/integration.cmake) and run by hand or through tools/probe_*.ps1; it is
// never part of the CTest suite because it talks to the real service.
#include "maicord/app/cdn_client.hpp"
#include "maicord/media/webp.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "../test_output.hpp"

namespace maicord {
namespace {

inline constexpr u32 kBodyCapacity = 64 * 1024;

CdnClient g_client{};
u8 g_body[kBodyCapacity]{};
alignas(u32) u8 g_webp_rgba[64 * 64 * 4]{};
Vp8lWorkspace g_webp_workspace{};

u32 request_avatar(u64 request_id, ConstSlice<u8> etag,
                   bool expect_reuse,
                   CdnClientResponse* response) noexcept {
    const CdnClientSubmitResult submitted =
        cdn_client_submit(
            &g_client,
            CdnClientRequest{
                ConstSlice<u8>{reinterpret_cast<const u8*>(
                                   "/embed/avatars/0.png"),
                               20},
                etag, request_id, 20'000, CdnHost::cdn});
    if (!submitted.ok()) {
        return 1u + static_cast<u32>(submitted.error);
    }
    const CdnClientResult waited =
        cdn_client_wait_response(&g_client, 25'000);
    if (!waited.ok() ||
        waited.phase != CdnClientPhase::response_ready) {
        return 20u + static_cast<u32>(waited.error) * 32u +
               static_cast<u32>(waited.phase);
    }
    const CdnClientTakeResult taken =
        cdn_client_take_response(
            &g_client, response, Slice<u8>{g_body, kBodyCapacity});
    if (!taken.ok()) {
        return 100u + static_cast<u32>(taken.error);
    }
    if (!response->transport_ok() || response->http_error !=
                                         CdnHttpError::none) {
        return 120u + static_cast<u32>(response->error) * 32u +
               static_cast<u32>(response->http_error);
    }
    if (response->reused_connection != expect_reuse) {
        return 180u;
    }
    if (response->status_code == 304) {
        return response->body_bytes == 0 ? 0u : 181u;
    }
    if (response->status_code != 200 || response->body_bytes < 8) {
        return 200u + response->status_code;
    }
    constexpr u8 png_signature[]{137, 80, 78, 71, 13, 10, 26, 10};
    for (u32 index = 0; index < sizeof(png_signature); ++index) {
        if (g_body[index] != png_signature[index]) {
            return 800u + index;
        }
    }
    return 0;
}

u32 cancel_avatar() noexcept {
    const CdnClientSubmitResult submitted =
        cdn_client_submit(
            &g_client,
            CdnClientRequest{
                ConstSlice<u8>{reinterpret_cast<const u8*>(
                                   "/embed/avatars/0.png"),
                               20},
                {}, 4, 20'000, CdnHost::cdn});
    if (!submitted.ok()) {
        return 1u + static_cast<u32>(submitted.error);
    }
    // Claim the still-pending generation before the worker consumes its
    // request signal. Polling for an active socket races a tiny cached asset.
    if (!cdn_client_cancel(&g_client, 4)) {
        return 40u;
    }
    const CdnClientResult waited =
        cdn_client_wait_response(&g_client, 25'000);
    CdnClientResponse response{};
    const CdnClientTakeResult taken =
        cdn_client_take_response(
            &g_client, &response, Slice<u8>{g_body, kBodyCapacity});
    if (!waited.ok() ||
        waited.phase != CdnClientPhase::response_ready ||
        !taken.ok() || taken.body_bytes != 0 || response.request_id != 4) {
        return 50u + static_cast<u32>(waited.error);
    }
    return response.error == CdnClientError::cancelled
               ? 0u
               : 100u + static_cast<u32>(response.error);
}

u32 request_static_webp(CdnClientResponse* response,
                        WebpImage* image) noexcept {
    constexpr char target[] =
        "/emojis/984111734434435142.webp?size=64";
    const CdnClientSubmitResult submitted =
        cdn_client_submit(
            &g_client,
            CdnClientRequest{
                ConstSlice<u8>{reinterpret_cast<const u8*>(target),
                               sizeof(target) - 1},
                {}, 3, 20'000, CdnHost::cdn,
                CdnImageType::webp});
    if (!submitted.ok()) {
        return 1u + static_cast<u32>(submitted.error);
    }
    const CdnClientResult waited =
        cdn_client_wait_response(&g_client, 25'000);
    if (!waited.ok() ||
        waited.phase != CdnClientPhase::response_ready) {
        return 20u + static_cast<u32>(waited.error) * 32u +
               static_cast<u32>(waited.phase);
    }
    const CdnClientTakeResult taken =
        cdn_client_take_response(
            &g_client, response, Slice<u8>{g_body, kBodyCapacity});
    if (!taken.ok() || !response->transport_ok() ||
        response->http_error != CdnHttpError::none ||
        response->status_code != 200 || !response->reused_connection) {
        return 100u + static_cast<u32>(taken.error) * 64u +
               static_cast<u32>(response->http_error);
    }
    const WebpDecodeResult decoded = webp_lossless_decode_rgba8(
        ConstSlice<u8>{g_body, response->body_bytes}, image,
        &g_webp_workspace,
        Slice<u8>{g_webp_rgba, sizeof(g_webp_rgba)});
    return decoded.ok() && image->width == 64 && image->height == 64
               ? 0u
               : 500u + static_cast<u32>(decoded.error);
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI
maicord_cdn_client_probe_entry() noexcept {
    using namespace maicord;
    CdnClientResult status =
        cdn_client_start(&g_client, kBodyCapacity);
    CdnClientResponse first{};
    CdnClientResponse reused{};
    CdnClientResponse webp{};
    WebpImage webp_image{};
    u32 failure_stage = status.ok()
                            ? 0u
                            : 100u + static_cast<u32>(status.error);
    if (failure_stage == 0) {
        const u32 result = request_avatar(1, {}, false, &first);
        if (result != 0) {
            failure_stage = 1000u + result;
        }
    }
    if (failure_stage == 0) {
        const u32 result = request_avatar(
            2, ConstSlice<u8>{first.etag, first.etag_bytes}, true,
            &reused);
        if (result != 0) {
            failure_stage = 2000u + result;
        }
    }
    status = cdn_client_status(&g_client);
    if (failure_stage == 0 &&
        (status.connections != 1 || status.requests != 2 ||
         status.reused_requests != 1 || status.responses != 2)) {
        failure_stage = 3000u;
    }
    if (failure_stage == 0) {
        const u32 result = request_static_webp(&webp, &webp_image);
        if (result != 0) {
            failure_stage = 3000u + result;
        }
    }
    if (failure_stage == 0) {
        const u32 result = cancel_avatar();
        if (result != 0) {
            failure_stage = 5000u + result;
        }
    }
    bool ok = failure_stage == 0;
    status = cdn_client_status(&g_client);
    if (ok) {
        test_write_text("PASS cdn_async first_us=");
        test_write_u64(first.elapsed_us);
        test_write_text(" reused_us=");
        test_write_u64(reused.elapsed_us);
        test_write_text(" reused_status=");
        test_write_u64(reused.status_code);
        test_write_text(" first_body=");
        test_write_u64(first.body_bytes);
        test_write_text(" webp_body=");
        test_write_u64(webp.body_bytes);
        test_write_text(" webp_width=");
        test_write_u64(webp_image.width);
        test_write_text(" webp_height=");
        test_write_u64(webp_image.height);
        test_write_text(" connections=");
        test_write_u64(status.connections);
        test_write_text(" runtime_bytes=");
        test_write_u64(cdn_client_runtime_bytes(kBodyCapacity));
        test_write_text("\r\n");
    } else {
        test_write_text("FAIL cdn_async stage=");
        test_write_u64(failure_stage);
        test_write_text(" phase=");
        test_write_u64(static_cast<u32>(status.phase));
        test_write_text(" error=");
        test_write_u64(static_cast<u32>(status.error));
        test_write_text(" native=");
        test_write_u64(status.native_error);
        test_write_text(" first_error=");
        test_write_u64(static_cast<u32>(first.error));
        test_write_text(" first_http=");
        test_write_u64(static_cast<u32>(first.http_error));
        test_write_text(" reused_error=");
        test_write_u64(static_cast<u32>(reused.error));
        test_write_text(" reused_http=");
        test_write_u64(static_cast<u32>(reused.http_error));
        test_write_text("\r\n");
    }
    if (g_client.worker.thread) {
        const CdnClientResult stopped =
            cdn_client_stop(&g_client, 5'000);
        ok = ok && stopped.phase == CdnClientPhase::stopped;
    }
    ExitProcess(ok ? 0 : 1);
}
