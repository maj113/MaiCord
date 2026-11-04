// The Direct3D 11 side of the ui_quads scene (ui_quads_scene.hpp): the draw pipeline's own shaders, states and
// buffers draw the quads into an RGBA8 target, and the result must match tests/gpu/golden/ui_quads.png, which the
// OpenGL ES test (ui_quads.cpp) reproduces on Linux. To renew the golden after a shader or scene change, run with
// MAICORD_GPU_QUAD_CAPTURE=<file> set: the render is written there as raw RGBA bytes (kWidth * kHeight * 4) and
// tools/update_gpu_goldens.py turns it into the PNG.
#include "maicord/core/win32_kernel.hpp"
#include "maicord/render/d3d11/draw_pipeline.hpp"
#include "maicord/sys/d3d11.hpp"
#include "../support/test_output.hpp"
#include "ui_quads_scene.hpp"

namespace maicord {
namespace {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
alignas(16) constexpr u8 kGolden[] = {
#embed "golden/ui_quads.png"
};
#pragma clang diagnostic pop

u8 g_pixels[ui_quads::kWidth * ui_quads::kHeight * 4];
u8 g_golden[ui_quads::kWidth * ui_quads::kHeight * 4];

bool fail(const char* what) noexcept {
    test_write_text("FAIL: ");
    test_write_line(what);
    return false;
}

bool capture_requested(const WCHAR* name, WCHAR* path, DWORD capacity) noexcept {
    const DWORD length = GetEnvironmentVariableW(name, path, capacity);
    return length != 0 && length < capacity;
}

bool write_file(const WCHAR* path, const void* data, DWORD bytes) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data, bytes, &written, nullptr) && written == bytes;
    CloseHandle(file);
    return ok;
}

bool run() noexcept {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &context)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &context)))
        return fail("no Direct3D 11 device");

    D3D11DrawPipelineOwner pipeline{};
    const D3D11DrawPipelineConfig config{64, 16, 48, 16, 48, 16};
    if (!d3d11_draw_pipeline_create(&pipeline, device, config).ok())
        return fail("draw pipeline creation");

    D3D11_TEXTURE2D_DESC description{};
    description.Width = ui_quads::kWidth;
    description.Height = ui_quads::kHeight;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* target = nullptr;
    ID3D11RenderTargetView* view = nullptr;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &target)) ||
        FAILED(device->CreateRenderTargetView(target, nullptr, &view)))
        return fail("render target");
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &staging)))
        return fail("staging texture");

    ID3D11Buffer* background = nullptr;
    {
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(ui_quads::BackgroundConstants);
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const D3D11_SUBRESOURCE_DATA data{&ui_quads::kBackground, 0, 0};
        if (FAILED(device->CreateBuffer(&buffer, &data, &background)))
            return fail("background constants");
    }
    const ui_quads::FrameConstants frame = ui_quads::frame_constants();
    context->UpdateSubresource(pipeline.ui_frame_buffer, 0, nullptr, &frame, 0, 0);

    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(ui_quads::kWidth), static_cast<float>(ui_quads::kHeight), 0, 1};
    const D3D11_RECT scissor{0, 0, static_cast<LONG>(ui_quads::kWidth), static_cast<LONG>(ui_quads::kHeight)};
    const float blend_factor[4]{};
    context->OMSetRenderTargets(1, &view, nullptr);
    context->ClearRenderTargetView(view, ui_quads::kClear);
    context->RSSetViewports(1, &viewport);
    context->RSSetState(pipeline.ui_rasterizer_state);
    context->RSSetScissorRects(1, &scissor);
    context->OMSetBlendState(pipeline.ui_blend_state, blend_factor, 0xFFFFFFFFu);
    context->VSSetConstantBuffers(0, 1, &pipeline.ui_frame_buffer);
    ID3D11Buffer* pixel_constants[2] = {pipeline.ui_frame_buffer, background};
    context->PSSetConstantBuffers(0, 2, pixel_constants);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(pipeline.ui_instance_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return fail("instance buffer map");
    copy_bytes(mapped.pData, ui_quads::kQuads, sizeof(ui_quads::kQuads));
    context->Unmap(pipeline.ui_instance_buffer, 0);
    const UINT stride = config.ui_instance_stride, offset = 0;
    context->IASetInputLayout(pipeline.ui_input_layout);
    context->IASetVertexBuffers(0, 1, &pipeline.ui_instance_buffer, &stride, &offset);
    context->VSSetShader(pipeline.ui_vertex_shader, nullptr, 0);
    context->PSSetShader(pipeline.ui_pixel_shader, nullptr, 0);
    context->DrawInstanced(4, ui_quads::kQuadCount, 0, 0);

    context->CopyResource(staging, target);
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
        return fail("staging map");
    for (u32 y = 0; y < ui_quads::kHeight; ++y)
        copy_bytes(g_pixels + y * ui_quads::kWidth * 4, static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch,
                   ui_quads::kWidth * 4);
    context->Unmap(staging, 0);

    com_release(staging);
    com_release(background);
    com_release(view);
    com_release(target);
    d3d11_draw_pipeline_release(&pipeline);
    com_release(context);
    com_release(device);

    WCHAR path[520];
    if (capture_requested(L"MAICORD_GPU_QUAD_CAPTURE", path, 520)) {
        if (!write_file(path, g_pixels, sizeof(g_pixels)))
            return fail("the capture file cannot be written");
        test_write_line("PASS captured the ui quads scene");
        return true;
    }
    if (!ui_quads::decode_png({kGolden, sizeof(kGolden)}, g_golden))
        return fail("the golden PNG does not decode");
    const ui_quads::Difference difference = ui_quads::compare(g_pixels, g_golden);
    test_write_metric("maximum channel difference=", difference.maximum);
    test_write_metric("pixels over tolerance=", difference.over_tolerance);
    if (!ui_quads::within_tolerance(difference))
        return fail("the render differs from tests/gpu/golden/ui_quads.png");
    test_write_line("PASS ui quads match the golden");
    return true;
}

} // namespace
} // namespace maicord

extern "C" void maicord_gpu_ui_quads_windows_entry() {
    ExitProcess(maicord::run() ? 0 : 1);
}
