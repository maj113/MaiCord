#include "maicord/gpu/api.hpp"
#include "../support/test_output.hpp"

#include "maicord/sys/d3d11.hpp"
namespace maicord { namespace {
#define CHECK(x) do{if(!(x)){test_write_text("FAIL: " #x "\n");return false;}}while(0)
// Input layouts mirror render/d3d11/draw_pipeline.cpp; CreateInputLayout validates
// them against each vertex shader's packed input signature.
constexpr GpuInputElement kUiElements[]={{"RECT",0,0},{"COLOR",0,16},{"COLOR",1,32},{"TEXCOORD",0,48}};
constexpr GpuInputElement kGlyphElements[]={{"RECT",0,0},{"TEXCOORD",0,16},{"COLOR",0,32}};
constexpr GpuInputElement kMediaElements[]={{"RECT",0,0},{"TEXCOORD",0,16},{"TEXCOORD",1,32},{"TEXCOORD",2,48}};
bool create_vertex(ID3D11Device* device,ShaderId id,ConstSlice<GpuInputElement> elements) noexcept {
    ID3D11VertexShader* shader=nullptr;ID3D11InputLayout* layout=nullptr;bool layout_failed=true;
    const bool ok=SUCCEEDED(gpu_vertex_shader_create(device,id,elements,&shader,&layout,&layout_failed)) &&
        shader && layout && !layout_failed;
    if(layout)layout->Release();
    if(shader)shader->Release();
    return ok;
}
bool create_pixel(ID3D11Device* device,ShaderId id) noexcept {
    ID3D11PixelShader* shader=nullptr;
    const bool ok=SUCCEEDED(gpu_pixel_shader_create(device,id,&shader)) && shader;
    if(shader)shader->Release();
    return ok;
}
bool create_compute(ID3D11Device* device,ShaderId id) noexcept {
    ID3D11ComputeShader* shader=nullptr;
    const bool ok=SUCCEEDED(gpu_compute_shader_create(device,id,&shader)) && shader;
    if(shader)shader->Release();
    return ok;
}
bool create(ID3D11Device* device,ShaderId id) noexcept {
    switch(id){
    case ShaderId::ui_vertex: return create_vertex(device,id,{kUiElements,4});
    case ShaderId::glyph_vertex: return create_vertex(device,id,{kGlyphElements,3});
    case ShaderId::media_vertex: return create_vertex(device,id,{kMediaElements,4});
    case ShaderId::fx_vertex: case ShaderId::fx_target_vertex: case ShaderId::fx_composite_vertex:
    case ShaderId::encode_vertex: { // vertex-id driven: no input layout
        ID3D11VertexShader* shader=nullptr;bool layout_failed=false;
        const bool ok=SUCCEEDED(gpu_vertex_shader_create(device,id,{},&shader,nullptr,&layout_failed)) && shader && !layout_failed;
        if(shader)shader->Release();
        return ok;
    }
    case ShaderId::ui_pixel: case ShaderId::glyph_pixel: case ShaderId::media_pixel: case ShaderId::fx_builtin:
    case ShaderId::fx_composite_pixel: case ShaderId::encode_pixel:
        return create_pixel(device,id);
    default: return create_compute(device,id);
    }
}
bool run() noexcept {
    ID3D11Device* device=nullptr;ID3D11DeviceContext* context=nullptr;
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)));
    // The runtime validates DXBC, so a corrupt or truncated inflate cannot
    // create a shader. Each id is created as the stage it ships for.
    u32 failed=static_cast<u32>(ShaderId::count);
    for(u32 id=0;id<static_cast<u32>(ShaderId::count);++id)
        if(!create(device,static_cast<ShaderId>(id))){failed=id;break;}
    ID3D11ComputeShader* invalid_compute=nullptr;ID3D11PixelShader* invalid_pixel=nullptr;
    ID3D11VertexShader* invalid_vertex=nullptr;ID3D11InputLayout* invalid_layout=nullptr;bool layout_failed=true;
    const bool rejected=FAILED(gpu_compute_shader_create(device,ShaderId::count,&invalid_compute)) && !invalid_compute &&
        FAILED(gpu_pixel_shader_create(device,ShaderId::count,&invalid_pixel)) && !invalid_pixel &&
        FAILED(gpu_vertex_shader_create(device,ShaderId::count,{kUiElements,4},&invalid_vertex,&invalid_layout,&layout_failed)) &&
        !invalid_vertex && !invalid_layout && !layout_failed;
    context->Release();device->Release();
    if(failed!=static_cast<u32>(ShaderId::count)){
        char line[]="FAIL: packed shader id 00 could not be created\n";
        line[23]=static_cast<char>('0'+failed/10);line[24]=static_cast<char>('0'+failed%10);
        test_write_text(line);return false;
    }
    CHECK(rejected);
    test_write_text("PASS packed shaders inflate and create\n");
    return true;
}
}}
extern "C" void maicord_shader_bytecode_probe_entry(){ExitProcess(maicord::run()?0:1);}
