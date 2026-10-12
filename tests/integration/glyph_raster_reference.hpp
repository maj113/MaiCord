#pragma once
#include "maicord_glyph_distance_reference_cs.hpp"

#include <d3d11_1.h>

namespace maicord {
// QA-only external bytecode permits comparisons with the immediately preceding
// shader without repeatedly adding compiled reference variants to the target.
inline bool probe_create_glyph_raster_reference(ID3D11Device* device,ID3D11ComputeShader** shader) noexcept {
    wchar_t path[1024]{};const DWORD length=GetEnvironmentVariableW(L"MAICORD_GLYPH_RASTER_REFERENCE",path,1024);
    if(!length)return SUCCEEDED(device->CreateComputeShader(g_maicord_glyph_distance_reference_shader,sizeof(g_maicord_glyph_distance_reference_shader),nullptr,shader));
    if(length>=1024)return false;
    const HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    static u8 bytes[65536];LARGE_INTEGER size{};DWORD read=0;
    const bool loaded=GetFileSizeEx(file,&size)&&size.QuadPart>0&&size.QuadPart<=sizeof(bytes)&&ReadFile(file,bytes,static_cast<DWORD>(size.QuadPart),&read,nullptr)&&read==size.QuadPart;
    CloseHandle(file);
    return loaded&&SUCCEEDED(device->CreateComputeShader(bytes,read,nullptr,shader));
}
struct ProbeGlyphRasterReference {
    ID3D11Buffer* commands=nullptr;ID3D11ShaderResourceView* view=nullptr;
    ID3D11Buffer* constants=nullptr;ID3D11ComputeShader* shader=nullptr;u32 capacity=0;
    ~ProbeGlyphRasterReference() noexcept {com_release(shader);com_release(constants);com_release(view);com_release(commands);}
    bool initialize(ID3D11Device* device,u32 maximum) noexcept {
        capacity=maximum;D3D11_BUFFER_DESC d{};d.ByteWidth=maximum*sizeof(GlyphPathCommand);
        d.Usage=D3D11_USAGE_DYNAMIC;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=sizeof(GlyphPathCommand);
        if(FAILED(device->CreateBuffer(&d,nullptr,&commands))||FAILED(device->CreateShaderResourceView(commands,nullptr,&view)))return false;
        d.ByteWidth=384;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;d.MiscFlags=0;d.StructureByteStride=0;
        return SUCCEEDED(device->CreateBuffer(&d,nullptr,&constants))&&probe_create_glyph_raster_reference(device,&shader);
    }
    void bind(ID3D11DeviceContext* context,ID3D11UnorderedAccessView* atlas) noexcept {
        context->CSSetShader(shader,nullptr,0);context->CSSetConstantBuffers(0,1,&constants);
        context->CSSetShaderResources(0,1,&view);context->CSSetUnorderedAccessViews(0,1,&atlas,nullptr);
    }
    bool rasterize(ID3D11DeviceContext* context,ID3D11UnorderedAccessView* atlas,const GlyphOutline& outline,
        const GlyphRasterBounds& bounds,const GlyphAtlasAllocation& a,float scale,u16 padding) noexcept {
        if(!outline.count||outline.count>capacity)return false;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(FAILED(context->Map(commands,0,D3D11_MAP_WRITE_DISCARD,0,&mapped)))return false;
        CopyMemory(mapped.pData,outline.commands,outline.count*sizeof(GlyphPathCommand));context->Unmap(commands,0);
        if(FAILED(context->Map(constants,0,D3D11_MAP_WRITE_DISCARD,0,&mapped)))return false;
        struct Values {u32 x,y,width,height,count,fill;float scale;u32 padding;float ox,oy;u32 unused[2];};
        static_assert(sizeof(Values)==48);
        const Values data{static_cast<u32>(a.x-padding),static_cast<u32>(a.y-padding),a.width+2u*padding,a.height+2u*padding,
            outline.count,outline.fill_rule==GlyphFillRule::winding?1u:0u,scale,padding,bounds.left/scale,bounds.top/scale,{}};
        CopyMemory(mapped.pData,&data,sizeof(data));context->Unmap(constants,0);
        bind(context,atlas);context->Dispatch((data.width+7)/8,(data.height+7)/8,1);
        ID3D11UnorderedAccessView* uav=nullptr;ID3D11ShaderResourceView* srv=nullptr;ID3D11Buffer* cb=nullptr;
        context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);context->CSSetShaderResources(0,1,&srv);
        context->CSSetConstantBuffers(0,1,&cb);context->CSSetShader(nullptr,nullptr,0);return true;
    }
};

}
