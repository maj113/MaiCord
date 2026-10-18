// Manual measurement probe (MaiCordGlyphCacheProbe, cmake/benchmarks.cmake with
// -DMAICORD_BUILD_BENCHMARKS=ON; tools/measure_glyph_cache.py runs it). Not part of the CTest suite.
#include "glyph_cache_measurement.hpp"
#include "maicord/core/log.hpp"
#include "cjk_pixel_probe.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/gdi.hpp"

namespace maicord {namespace {
D3D11Renderer renderer;

bool capture(const wchar_t* filename) noexcept {
    const bool stream=probe_cjk_pixel_stream();
    wchar_t path[1024]{};
    u32 end=GetEnvironmentVariableW(L"MAICORD_UI_PROBE_CAPTURE_DIR",path,900);
    if(!end&&!stream)return true;
    if(end>=900)return false;
    path[end++]=L'/';
    for(u32 i=0;filename[i];++i){if(end+1>=1024)return false;path[end++]=filename[i];}
    if(!d3d11_renderer_draw_stream(&renderer,kDefaultTheme,true,true,nullptr,10000))return false;
    ID3D11Resource* resource=nullptr;ID3D11Texture2D* source=nullptr;ID3D11Texture2D* staging=nullptr;
    struct Release {ID3D11Resource*& r;ID3D11Texture2D*& s;ID3D11Texture2D*& t;
        ~Release() noexcept {com_release(r);com_release(s);com_release(t);}} release{resource,source,staging};
    renderer.graphics.target->GetResource(&resource);
    if(FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&source))))return false;
    D3D11_TEXTURE2D_DESC description{};source->GetDesc(&description);
    description.Usage=D3D11_USAGE_STAGING;description.BindFlags=0;description.MiscFlags=0;
    description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if(FAILED(renderer.graphics.device->CreateTexture2D(&description,nullptr,&staging)))return false;
    auto* context=renderer.graphics.context;context->CopyResource(staging,source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(context->Map(staging,0,D3D11_MAP_READ,0,&mapped)))return false;
    struct Unmap {ID3D11DeviceContext* c;ID3D11Texture2D* t;~Unmap() noexcept {c->Unmap(t,0);}} unmap{context,staging};
    HANDLE file=stream?GetStdHandle(STD_OUTPUT_HANDLE):CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    struct Close {HANDLE h;bool owned;~Close() noexcept {if(owned)CloseHandle(h);}} close{file,!stream};
    const auto write=[&](const void* data,u32 bytes) noexcept {
        DWORD written=0;return WriteFile(file,data,bytes,&written,nullptr)&&written==bytes;
    };
    BITMAPFILEHEADER header{};header.bfType=0x4d42;
    header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);
    header.bfSize=header.bfOffBits+description.Width*description.Height*4;
    BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=description.Width;
    info.biHeight=-static_cast<LONG>(description.Height);info.biPlanes=1;info.biBitCount=32;
    if(!write(&header,sizeof(header))||!write(&info,sizeof(info)))return false;
    for(u32 y=0;y<description.Height;++y)
        if(!write(static_cast<const u8*>(mapped.pData)+y*mapped.RowPitch,description.Width*4))return false;
    return true;
}

bool run() noexcept {
    if(!log_open())return false;
    const HINSTANCE instance=GetModuleHandleW(nullptr);
    WNDCLASSW type{};type.lpfnWndProc=DefWindowProcW;type.hInstance=instance;type.lpszClassName=L"MaiCordGlyphCacheProbe";
    if(!RegisterClassW(&type))return false;
    RECT bounds{0,0,1200,970};if(!AdjustWindowRectEx(&bounds,WS_OVERLAPPEDWINDOW,FALSE,0))return false;
    const HWND window=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP,type.lpszClassName,L"MaiCord glyph cache",WS_OVERLAPPEDWINDOW,
        32,32,bounds.right-bounds.left,bounds.bottom-bounds.top,nullptr,nullptr,instance,nullptr);
    if(!window)return false;
    struct Cleanup {HWND w;~Cleanup() noexcept {renderer_shutdown(&renderer);DestroyWindow(w);log_close();}} cleanup{window};
    LARGE_INTEGER begin{},end{},frequency{};ULONG64 first=0,last=0;
    if(!QueryPerformanceFrequency(&frequency)||!QueryThreadCycleTime(GetCurrentThread(),&first)||!QueryPerformanceCounter(&begin))return false;
    if(!renderer_initialize(&renderer,window))return false;
    if(!QueryPerformanceCounter(&end)||!QueryThreadCycleTime(GetCurrentThread(),&last))return false;
    log_u64(LogLevel::info,"bootstrap","renderer_init_us",static_cast<u64>(end.QuadPart-begin.QuadPart)*1000000/frequency.QuadPart);
    log_u64(LogLevel::info,"bootstrap","renderer_init_cycles",last-first);
    wchar_t cjk[64]{};const DWORD cjk_size=GetEnvironmentVariableW(L"MAICORD_CJK_PIXEL_CASE",cjk,64);
    if(cjk_size)return cjk_size<64&&probe_cjk_pixels(renderer,cjk,capture);
    return probe_glyph_cache(renderer,capture);
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_glyph_cache_probe_entry() noexcept {
    ExitProcess(maicord::run()?0:1);
}
