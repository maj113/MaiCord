#include "maicord/render/colr_paint.hpp"
#include "maicord/render/glyph_atlas.hpp"
#include "dwrite_paint_builder.hpp"
#include "native_source.hpp"
#include "maicord/render/native_glyph_outlines.hpp"
#include "maicord/text/font_run.hpp"
#include <d2d1_3.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <d3d11_1.h>
using namespace maicord;
// Exercises the native run/instance/renderer ownership path; NativePaintContext
// is the lower-level paint-source oracle.
struct AppPaintContext {
    NativeFontSession* fonts=nullptr;NativeFontHandle face{};NativeFontInstanceHandle instance{};
    NativeFontRunHandle run{};NativeGlyphOutlines* outlines=nullptr;Arena scratch{};
    bool open(const wchar_t* path,ConstSlice<FontAxisSetting> settings) noexcept {
        if(!native_font_session_create(&fonts)||!arena_initialize(&scratch,4*1024*1024,0,4096)||
           !native_font_session_face(fonts,path,0,&face).ok()||
           !native_font_prepare(fonts,face,settings,0,false,&scratch,&instance).ok())return false;
        const auto* font=native_font_get(*native_font_session_fonts(fonts),face);NativeFontSelection source{};source.face=face;source.count=font->geometry.axes.axis_count;
        if(source.count>2)return false;
        for(u32 i=0;i<source.count;++i){FontAxis axis{};if(font_axis(font->geometry.axes,i,&axis)!=SfntStatus::ok)return false;
            source.settings[i]={axis.tag,axis.normal/65536.0};for(u32 j=0;j<settings.count;++j)if(settings[j].tag==axis.tag)source.settings[i]=settings[j];}
        printf("Native app axes: %u; gvar: %u\n",source.count,font->geometry.gvar.table.count?1:0);
        return native_font_session_run(fonts,instance,source,0,&run).ok();
    }
    bool build(ColrPaintProgram* program,u16 glyph,float em) noexcept {
        bool foreground=false;return native_color_glyph_build(&outlines,1,glyph,{1,1,1,1},em,program,&foreground,fonts,run);
    }
    void close() noexcept {
        native_glyph_outlines_release(&outlines);
        if(fonts){if(run.slot)native_font_run_release(native_font_session_runs(fonts),run);
            if(instance.slot)native_font_instance_release(native_font_session_instances(fonts),instance);
            if(face.slot)native_font_release(native_font_session_fonts(fonts),face);native_font_session_release(&fonts);}
        arena_release(&scratch);run={};instance={};face={};
    }
    ~AppPaintContext() noexcept {close();}
};
void check(HRESULT hr, const char* name) {
    if (FAILED(hr)) {
        printf("%s: %08lx\n", name, hr);
        exit(1);
    }
}
int main(int argc, char** argv) {
    const bool app_provider=argc>=4&&strcmp(argv[3],"--paired-app")==0;
    if((argc>4&&!app_provider) || (argc>=4 && !app_provider && strcmp(argv[3],"--native")!=0 && strcmp(argv[3],"--paired-native")!=0)) {
        fprintf(stderr,"Usage: ColrReference [glyphs.txt [font.ttf [--native|--paired-native|--paired-app [--weight value] [--width value]]]]\n");
        return 10;
    }
    const bool use_native=argc==4 && strcmp(argv[3],"--native")==0;
    const bool paired=app_provider||(argc==4 && strcmp(argv[3],"--paired-native")==0);
    FontAxisSetting settings[2]{};DWRITE_FONT_AXIS_VALUE windows_settings[2]{};u32 setting_count=0;bool compare_limits=false;
    for(int i=4;i<argc;i+=2){if(strcmp(argv[i],"--compare-limits")==0){if(compare_limits)return 10;compare_limits=true;--i;continue;}
        if(i+1==argc||setting_count==2)return 10;u32 tag=0;DWRITE_FONT_AXIS_TAG dwtag{};
        if(strcmp(argv[i],"--weight")==0){tag=sfnt_tag('w','g','h','t');dwtag=DWRITE_FONT_AXIS_TAG_WEIGHT;}
        else if(strcmp(argv[i],"--width")==0){tag=sfnt_tag('w','d','t','h');dwtag=DWRITE_FONT_AXIS_TAG_WIDTH;}else return 10;
        for(u32 j=0;j<setting_count;++j)if(settings[j].tag==tag)return 10;
        char* end=nullptr;const float value=strtof(argv[i+1],&end);if(end==argv[i+1]||*end||!std::isfinite(value))return 10;
        settings[setting_count]={tag,value};windows_settings[setting_count++]={dwtag,value};}
    AppPaintContext app_source;
    static NativePaintContext native_source;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL level{};
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                            nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context),
          "D3D11");
    IDXGIDevice* dxgi = nullptr;
    check(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi), "dxgi");
    IDXGIAdapter* adapter=nullptr;
    check(dxgi->GetAdapter(&adapter),"adapter");
    DXGI_ADAPTER_DESC adapter_desc{};
    check(adapter->GetDesc(&adapter_desc),"adapter description");
    char adapter_name[512]{};
    WideCharToMultiByte(CP_UTF8,0,adapter_desc.Description,-1,adapter_name,sizeof(adapter_name),nullptr,nullptr);
    LARGE_INTEGER driver{};
    check(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver),"driver version");
    printf("GPU: %s; vendor=%04x device=%04x; driver=%llu; feature_level=%x\n",adapter_name,
           adapter_desc.VendorId,adapter_desc.DeviceId,static_cast<unsigned long long>(driver.QuadPart),level);
    adapter->Release();
    ID2D1Device* d2d = nullptr;
    check(D2D1CreateDevice(dxgi, nullptr, &d2d), "d2d");
    ID2D1DeviceContext* dc0 = nullptr;
    check(d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc0), "context");
    ID2D1DeviceContext7* dc = nullptr;
    check(dc0->QueryInterface(__uuidof(ID2D1DeviceContext7), (void**)&dc), "context7");
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 512;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* target = nullptr;
    check(device->CreateTexture2D(&td, nullptr, &target), "target");
    IDXGISurface* surface = nullptr;
    check(target->QueryInterface(__uuidof(IDXGISurface), (void**)&surface), "surface");
    D2D1_BITMAP_PROPERTIES1 prop{};
    prop.pixelFormat = {td.Format, D2D1_ALPHA_MODE_PREMULTIPLIED};
    prop.dpiX = prop.dpiY = 96;
    prop.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    ID2D1Bitmap1* bitmap = nullptr;
    check(dc->CreateBitmapFromDxgiSurface(surface, &prop, &bitmap), "bitmap");
    dc->SetTarget(bitmap);
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* reference = nullptr;
    check(device->CreateTexture2D(&td, nullptr, &reference), "reference");
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ID3D11Texture2D* candidate = nullptr;
    check(device->CreateTexture2D(&td, nullptr, &candidate), "candidate");
    IDWriteFactory* factory = nullptr;
    check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&factory),
          "factory");
    IDWriteFontCollection* fonts = nullptr;
    factory->GetSystemFontCollection(&fonts);
    UINT32 family_index = 0;
    BOOL found = false;
    fonts->FindFamilyName(L"Segoe UI Emoji", &family_index, &found);
    if (!found)
        return 2;
    IDWriteFontFamily* family = nullptr;
    fonts->GetFontFamily(family_index, &family);
    IDWriteFont* font = nullptr;
    family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL, &font);
    IDWriteFontFace* face = nullptr;
    check(font->CreateFontFace(&face), "face");
    if (argc > 2) {
        wchar_t path[32768];
        if(!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[2], -1, path, 32768))return 10;
        IDWriteFontFile* file = nullptr;
        check(factory->CreateFontFileReference(path, nullptr, &file), "file");
        face->Release();
        check(factory->CreateFontFace(DWRITE_FONT_FACE_TYPE_TRUETYPE, 1, &file, 0,
                                      DWRITE_FONT_SIMULATIONS_NONE, &face),
              "custom face");
        file->Release();
        if((use_native || (paired&&!app_provider)) && !native_source.open(path)) {
            fprintf(stderr,"Native font preparation failed\n");native_source.close();return 11;
        }
        if(app_provider&&!app_source.open(path,{settings,setting_count})){fprintf(stderr,"Native app instance preparation failed\n");return 11;}
    }
    if(setting_count){IDWriteFontFace5* base=nullptr;IDWriteFontResource* resource=nullptr;IDWriteFontFace5* varied=nullptr;
        check(face->QueryInterface(__uuidof(IDWriteFontFace5),reinterpret_cast<void**>(&base)),"variable base");
        check(base->GetFontResource(&resource),"font resource");
        check(resource->CreateFontFace(DWRITE_FONT_SIMULATIONS_NONE,windows_settings,setting_count,&varied),"varied face");
        resource->Release();base->Release();face->Release();face=varied;
        for(u32 i=0;i<setting_count;++i)printf("Axis: %08x %.9g\n",settings[i].tag,settings[i].value);
    }
    IDWriteFontFace7* face7 = nullptr;
    check(face->QueryInterface(__uuidof(IDWriteFontFace7), (void**)&face7), "face7");
    IDWritePaintReader* reader = nullptr;
    check(face7->CreatePaintReader(DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE,
                                   DWRITE_PAINT_FEATURE_LEVEL_COLR_V1, &reader),
          "reader");
    reader->SetTextColor({1, 1, 1, 1});
    ID2D1SolidColorBrush* white = nullptr;
    dc->CreateSolidColorBrush({1, 1, 1, 1}, &white);
    FILE* input = fopen(argc > 1 ? argv[1] : "glyphs.txt", "r");
    FILE* output = fopen("pixels.bin", "wb");
    FILE* timings = fopen("timings.csv", "w");
    if (!input || !output || !timings) {
        fprintf(stderr, "Cannot open corpus/output files\n");
        return 10;
    }
    fprintf(timings, "glyph,em,nodes,bytes,types,build_us,raster_readback_us,reference_readback_us,masks,mask_"
                     "pixels,peak_stack\n");
    printf("Provider: %s\n",use_native?"first-party":"existing app");
    printf("Pixel reference: %s\n",app_provider?"native app ownership, shared app shader":paired?"first-party, shared app shader":"Direct2D");
    ColrPaintProgram program{};
    ColrPaintProgram paired_program{};
    ColrPaintGpu gpu{};
    ColrPaintGpu paired_gpu{};
    unsigned gid, em;
    unsigned total = 0;
    unsigned records = 0,empty = 0,limited = 0;
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    while (fscanf(input, "%u %u", &gid, &em) == 2) {
        ++records;
        DWRITE_PAINT_ELEMENT root{};
        D2D_RECT_F clip{};
        if(gid>65535 || em==0 || em>256)return 10;
        LARGE_INTEGER t0, t1, t2, t3;
        QueryPerformanceCounter(&t0);
        bool built=false;
        if(use_native)built=native_source.build(&program,static_cast<u16>(gid),static_cast<float>(em));
        else {
            check(reader->SetCurrentGlyph(gid, &root, &clip), "glyph");
            if(root.paintType==DWRITE_PAINT_TYPE_NONE) {fprintf(stderr,"No paint for corpus glyph %u\n",gid);return 3;}
            built=colr_paint_build(&program, reader, face, root, clip, (float)em);
        }
        if (!built) {
            printf("build failed %u\n", gid);
            return 3;
        }
        GlyphRasterBounds bounds{};
        auto& b = program.bounds;
        if (!b.has_bounds) {
            if(paired){
                if(!(app_provider?app_source.build(&paired_program,static_cast<u16>(gid),static_cast<float>(em)):
                     native_source.build(&paired_program,static_cast<u16>(gid),static_cast<float>(em)))||paired_program.bounds.has_bounds){fprintf(stderr,"Empty bounds differ: glyph=%u em=%u\n",gid,em);return 4;}
                // No raster exists on either app path. Keep the corpus record
                // as one transparent texel rather than silently dropping it.
                const UINT32 header[]{gid,em,1,1};const UINT32 transparent=0;
                fwrite(header,4,4,output);fwrite(&transparent,4,1,output);fwrite(&transparent,4,1,output);
                ++total;
            }
            ++empty;
            continue;
        }
        const bool raster_valid=glyph_raster_bounds(b.minimum_x,b.minimum_y,b.maximum_x,b.maximum_y,static_cast<float>(em),1,&bounds);
        if (!raster_valid || bounds.width > 500 || bounds.height > 500){
            if(compare_limits&&app_provider){
                if(!app_source.build(&paired_program,static_cast<u16>(gid),static_cast<float>(em)))return 3;
                const auto& pb=paired_program.bounds;
                if(!pb.has_bounds||pb.minimum_x!=b.minimum_x||pb.minimum_y!=b.minimum_y||pb.maximum_x!=b.maximum_x||pb.maximum_y!=b.maximum_y)return 4;
                printf("LIMIT {\"glyph\":%u,\"em\":%u,\"kind\":\"%s\",\"exact_bounds\":true,\"bounds\":[%.9g,%.9g,%.9g,%.9g]}\n",gid,em,
                    raster_valid?"probe-atlas":"app-raster-policy",b.minimum_x,b.minimum_y,b.maximum_x,b.maximum_y);
                ++limited;continue;
            }
            fprintf(stderr,"Reference exceeds probe atlas: glyph=%u em=%u bounds=%g,%g,%g,%g raster=%u,%u\n",gid,em,b.minimum_x,b.minimum_y,b.maximum_x,b.maximum_y,bounds.width,bounds.height);return 4;}
        QueryPerformanceCounter(&t1);
        GlyphAtlasAllocation allocation{};
        allocation.x = allocation.y = 2;
        allocation.width = bounds.width;
        allocation.height = bounds.height;
        if(paired) {
            if(!gpu.uav && !colr_paint_rasterize(&gpu,context,program,bounds,allocation,static_cast<float>(em),512,1))return 5;
            const float poison[]{1,0,1,1};context->ClearUnorderedAccessViewFloat(gpu.uav,poison);
        }
        if (!colr_paint_rasterize(&gpu, context, program, bounds, allocation, (float)em, 512, 1))
            return 5;
        context->CopyResource(candidate, gpu.texture);
        D3D11_MAPPED_SUBRESOURCE ours{};
        check(context->Map(candidate, 0, D3D11_MAP_READ, 0, &ours), "readback");
        QueryPerformanceCounter(&t2);
        UINT32 header[]{gid, em, bounds.width, bounds.height};
        fwrite(header, 4, 4, output);
        for (unsigned y = 0; y < bounds.height; ++y)
            fwrite((char*)ours.pData + (y + 2) * ours.RowPitch + 8, 4, bounds.width, output);
        context->Unmap(candidate, 0);
        if(paired) {
            if(!(app_provider?app_source.build(&paired_program,static_cast<u16>(gid),static_cast<float>(em)):
                 native_source.build(&paired_program,static_cast<u16>(gid),static_cast<float>(em))))return 3;
            const auto& pb=paired_program.bounds;GlyphRasterBounds paired_bounds{};
            if(!pb.has_bounds || !glyph_raster_bounds(pb.minimum_x,pb.minimum_y,pb.maximum_x,pb.maximum_y,
                                                      static_cast<float>(em),1,&paired_bounds) ||
               paired_bounds.left!=bounds.left || paired_bounds.top!=bounds.top ||
               paired_bounds.width!=bounds.width || paired_bounds.height!=bounds.height) {
                fprintf(stderr,"Native placement/bounds differ: glyph=%u em=%u\n",gid,em);return 6;
            }
            if(!paired_gpu.uav && !colr_paint_rasterize(&paired_gpu,context,paired_program,paired_bounds,allocation,static_cast<float>(em),512,1))return 5;
            const float poison[]{0,1,0,1};context->ClearUnorderedAccessViewFloat(paired_gpu.uav,poison);
            if(!colr_paint_rasterize(&paired_gpu,context,paired_program,paired_bounds,allocation,static_cast<float>(em),512,1))return 5;
            context->CopyResource(candidate,paired_gpu.texture);
            D3D11_MAPPED_SUBRESOURCE second{};
            check(context->Map(candidate,0,D3D11_MAP_READ,0,&second),"paired readback");
            QueryPerformanceCounter(&t3);
            for(unsigned y=0;y<bounds.height;++y)
                fwrite(static_cast<const char*>(second.pData)+(y+2)*second.RowPitch+8,4,bounds.width,output);
            context->Unmap(candidate,0);
        } else {
        dc->BeginDraw();
        dc->Clear({0, 0, 0, 0});
        UINT16 index = (UINT16)gid;
        float advance = 0;
        DWRITE_GLYPH_OFFSET offset{};
        DWRITE_GLYPH_RUN run{face, (float)em, 1, &index, &advance, &offset, FALSE, 0};
        dc->DrawPaintGlyphRun({2.f - bounds.left, 2.f - bounds.top}, &run, white);
        check(dc->EndDraw(), "draw");
        context->CopyResource(reference, target);
        D3D11_MAPPED_SUBRESOURCE native{};
        check(context->Map(reference, 0, D3D11_MAP_READ, 0, &native), "native readback");
        QueryPerformanceCounter(&t3);
        for (unsigned y = 0; y < bounds.height; ++y)
            for (unsigned x = 0; x < bounds.width; ++x) {
                const auto* c = (unsigned char*)native.pData + (y + 2) * native.RowPitch + (x + 2) * 4;
                unsigned char rgba[]{c[2], c[1], c[0], c[3]};
                fwrite(rgba, 4, 1, output);
            }
        context->Unmap(reference, 0);
        }
        fprintf(timings, "%u,%u,%u,%zu,%u,%.3f,%.3f,%.3f,%u,%u,%u\n", gid, em, program.nodes,
                program.storage.offset, program.paint_types, (t1.QuadPart - t0.QuadPart) * 1e6 / f.QuadPart,
                (t2.QuadPart - t1.QuadPart) * 1e6 / f.QuadPart,
                (t3.QuadPart - t2.QuadPart) * 1e6 / f.QuadPart, program.mask_count, program.mask_pixels,
                program.peak_stack);
        if (++total % 100 == 0) {
            printf("%u glyphs\n", total);
            fflush(stdout);
        }
    }
    if(!feof(input) || ferror(input) || ferror(output) || ferror(timings))return 12;
    fclose(input);
    if(fclose(output)!=0 || fclose(timings)!=0)return 12;
    native_source.close();
    colr_paint_gpu_release(&gpu);
    colr_paint_gpu_release(&paired_gpu);
    arena_release(&program.storage);
    arena_release(&paired_program.storage);
    printf("PASS %u glyphs; input=%u empty_bounds=%u unrendered_limits=%u\n", total,records,empty,limited);
    return 0;
}
