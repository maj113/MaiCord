#pragma once

// Direct3D 11, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/dxgi.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <d3d11_4.h>
#endif

// <d3d11.h>
#define D3D11_SDK_VERSION ( 7 )

MAICORD_SYS_TYPES_BEGIN
struct ID3D11Asynchronous;
struct ID3D11BlendState;
struct ID3D11Buffer;
struct ID3D11ClassInstance;
struct ID3D11ClassLinkage;
struct ID3D11ComputeShader;
struct ID3D11DepthStencilView;
struct ID3D11Device1;
struct ID3D11Device2;
struct ID3D11Device3;
struct ID3D11Device4;
struct ID3D11Device5;
struct ID3D11Device;
struct ID3D11DeviceChild;
struct ID3D11DeviceContext1;
struct ID3D11DeviceContext2;
struct ID3D11DeviceContext3;
struct ID3D11DeviceContext4;
struct ID3D11DeviceContext;
struct ID3D11Fence;
struct ID3D11InputLayout;
struct ID3D11Multithread;
struct ID3D11PixelShader;
struct ID3D11Query;
struct ID3D11RasterizerState;
struct ID3D11RenderTargetView;
struct ID3D11Resource;
struct ID3D11SamplerState;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11UnorderedAccessView;
struct ID3D11VertexShader;
struct ID3D11VideoContext;
struct ID3D11VideoDecoder;
struct ID3D11VideoDecoderOutputView;
struct ID3D11VideoDevice1;
struct ID3D11VideoDevice;
struct ID3D11View;

// <d3dcommon.h>
enum D3D_DRIVER_TYPE {
    D3D_DRIVER_TYPE_UNKNOWN = 0,
    D3D_DRIVER_TYPE_HARDWARE = (D3D_DRIVER_TYPE_UNKNOWN + 1),
    D3D_DRIVER_TYPE_REFERENCE = (D3D_DRIVER_TYPE_HARDWARE + 1),
    D3D_DRIVER_TYPE_NULL = (D3D_DRIVER_TYPE_REFERENCE + 1),
    D3D_DRIVER_TYPE_SOFTWARE = (D3D_DRIVER_TYPE_NULL + 1),
    D3D_DRIVER_TYPE_WARP = (D3D_DRIVER_TYPE_SOFTWARE + 1)
};
enum D3D_FEATURE_LEVEL {
    D3D_FEATURE_LEVEL_11_0 = 0xb000,
    D3D_FEATURE_LEVEL_11_1 = 0xb100,
};
enum D3D_PRIMITIVE_TOPOLOGY {
    D3D_PRIMITIVE_TOPOLOGY_UNDEFINED = 0,
    D3D_PRIMITIVE_TOPOLOGY_POINTLIST = 1,
    D3D_PRIMITIVE_TOPOLOGY_LINELIST = 2,
    D3D_PRIMITIVE_TOPOLOGY_LINESTRIP = 3,
    D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST = 4,
    D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP = 5,
    D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ = 10,
    D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ = 11,
    D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ = 12,
    D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ = 13,
    D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST = 33,
    D3D_PRIMITIVE_TOPOLOGY_2_CONTROL_POINT_PATCHLIST = 34,
    D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST = 35,
    D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST = 36,
    D3D_PRIMITIVE_TOPOLOGY_5_CONTROL_POINT_PATCHLIST = 37,
    D3D_PRIMITIVE_TOPOLOGY_6_CONTROL_POINT_PATCHLIST = 38,
    D3D_PRIMITIVE_TOPOLOGY_7_CONTROL_POINT_PATCHLIST = 39,
    D3D_PRIMITIVE_TOPOLOGY_8_CONTROL_POINT_PATCHLIST = 40,
    D3D_PRIMITIVE_TOPOLOGY_9_CONTROL_POINT_PATCHLIST = 41,
    D3D_PRIMITIVE_TOPOLOGY_10_CONTROL_POINT_PATCHLIST = 42,
    D3D_PRIMITIVE_TOPOLOGY_11_CONTROL_POINT_PATCHLIST = 43,
    D3D_PRIMITIVE_TOPOLOGY_12_CONTROL_POINT_PATCHLIST = 44,
    D3D_PRIMITIVE_TOPOLOGY_13_CONTROL_POINT_PATCHLIST = 45,
    D3D_PRIMITIVE_TOPOLOGY_14_CONTROL_POINT_PATCHLIST = 46,
    D3D_PRIMITIVE_TOPOLOGY_15_CONTROL_POINT_PATCHLIST = 47,
    D3D_PRIMITIVE_TOPOLOGY_16_CONTROL_POINT_PATCHLIST = 48,
    D3D_PRIMITIVE_TOPOLOGY_17_CONTROL_POINT_PATCHLIST = 49,
    D3D_PRIMITIVE_TOPOLOGY_18_CONTROL_POINT_PATCHLIST = 50,
    D3D_PRIMITIVE_TOPOLOGY_19_CONTROL_POINT_PATCHLIST = 51,
    D3D_PRIMITIVE_TOPOLOGY_20_CONTROL_POINT_PATCHLIST = 52,
    D3D_PRIMITIVE_TOPOLOGY_21_CONTROL_POINT_PATCHLIST = 53,
    D3D_PRIMITIVE_TOPOLOGY_22_CONTROL_POINT_PATCHLIST = 54,
    D3D_PRIMITIVE_TOPOLOGY_23_CONTROL_POINT_PATCHLIST = 55,
    D3D_PRIMITIVE_TOPOLOGY_24_CONTROL_POINT_PATCHLIST = 56,
    D3D_PRIMITIVE_TOPOLOGY_25_CONTROL_POINT_PATCHLIST = 57,
    D3D_PRIMITIVE_TOPOLOGY_26_CONTROL_POINT_PATCHLIST = 58,
    D3D_PRIMITIVE_TOPOLOGY_27_CONTROL_POINT_PATCHLIST = 59,
    D3D_PRIMITIVE_TOPOLOGY_28_CONTROL_POINT_PATCHLIST = 60,
    D3D_PRIMITIVE_TOPOLOGY_29_CONTROL_POINT_PATCHLIST = 61,
    D3D_PRIMITIVE_TOPOLOGY_30_CONTROL_POINT_PATCHLIST = 62,
    D3D_PRIMITIVE_TOPOLOGY_31_CONTROL_POINT_PATCHLIST = 63,
    D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST = 64,
    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
};
enum D3D_SRV_DIMENSION {
    D3D_SRV_DIMENSION_UNKNOWN = 0,
    D3D_SRV_DIMENSION_BUFFER = 1,
    D3D_SRV_DIMENSION_TEXTURE1D = 2,
    D3D_SRV_DIMENSION_TEXTURE1DARRAY = 3,
    D3D_SRV_DIMENSION_TEXTURE2D = 4,
    D3D_SRV_DIMENSION_TEXTURE2DARRAY = 5,
    D3D_SRV_DIMENSION_TEXTURE2DMS = 6,
    D3D_SRV_DIMENSION_TEXTURE2DMSARRAY = 7,
    D3D_SRV_DIMENSION_TEXTURE3D = 8,
    D3D_SRV_DIMENSION_TEXTURECUBE = 9,
    D3D_SRV_DIMENSION_TEXTURECUBEARRAY = 10,
    D3D_SRV_DIMENSION_BUFFEREX = 11,
    D3D11_SRV_DIMENSION_BUFFER = D3D_SRV_DIMENSION_BUFFER,
    D3D11_SRV_DIMENSION_TEXTURE2DARRAY = D3D_SRV_DIMENSION_TEXTURE2DARRAY,
    D3D11_SRV_DIMENSION_BUFFEREX = D3D_SRV_DIMENSION_BUFFEREX
};
// <d3d11.h>
enum D3D11_INPUT_CLASSIFICATION {
    D3D11_INPUT_PER_INSTANCE_DATA = 1
};
struct D3D11_INPUT_ELEMENT_DESC {
    LPCSTR SemanticName;
    UINT SemanticIndex;
    DXGI_FORMAT Format;
    UINT InputSlot;
    UINT AlignedByteOffset;
    D3D11_INPUT_CLASSIFICATION InputSlotClass;
    UINT InstanceDataStepRate;
};
enum D3D11_FILL_MODE {
    D3D11_FILL_SOLID = 3
};
typedef D3D_PRIMITIVE_TOPOLOGY D3D11_PRIMITIVE_TOPOLOGY;
enum D3D11_CULL_MODE {
    D3D11_CULL_NONE = 1,
};
struct D3D11_VIEWPORT {
    FLOAT TopLeftX;
    FLOAT TopLeftY;
    FLOAT Width;
    FLOAT Height;
    FLOAT MinDepth;
    FLOAT MaxDepth;
};
typedef D3D_SRV_DIMENSION D3D11_SRV_DIMENSION;
enum D3D11_DSV_DIMENSION {
    D3D11_DSV_DIMENSION_UNKNOWN = 0,
};
enum D3D11_RTV_DIMENSION {
    D3D11_RTV_DIMENSION_UNKNOWN = 0,
};
enum D3D11_UAV_DIMENSION {
    D3D11_UAV_DIMENSION_BUFFER = 1,
    D3D11_UAV_DIMENSION_TEXTURE2D = 4,
};
enum D3D11_USAGE {
    D3D11_USAGE_DEFAULT = 0,
    D3D11_USAGE_IMMUTABLE = 1,
    D3D11_USAGE_DYNAMIC = 2,
    D3D11_USAGE_STAGING = 3
};
enum D3D11_BIND_FLAG {
    D3D11_BIND_VERTEX_BUFFER = 0x1L,
    D3D11_BIND_CONSTANT_BUFFER = 0x4L,
    D3D11_BIND_SHADER_RESOURCE = 0x8L,
    D3D11_BIND_RENDER_TARGET = 0x20L,
    D3D11_BIND_UNORDERED_ACCESS = 0x80L,
    D3D11_BIND_DECODER = 0x200L,
};
enum D3D11_CPU_ACCESS_FLAG {
    D3D11_CPU_ACCESS_WRITE = 0x10000L,
    D3D11_CPU_ACCESS_READ = 0x20000L
};
enum D3D11_RESOURCE_MISC_FLAG {
    D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS = 0x20L,
    D3D11_RESOURCE_MISC_BUFFER_STRUCTURED = 0x40L,
    D3D11_RESOURCE_MISC_TILE_POOL = 0x20000L,
    D3D11_RESOURCE_MISC_TILED = 0x40000L,
};
enum D3D11_MAP {
    D3D11_MAP_READ = 1,
    D3D11_MAP_WRITE_DISCARD = 4,
    D3D11_MAP_WRITE_NO_OVERWRITE = 5
};
enum D3D11_MAP_FLAG {
    D3D11_MAP_FLAG_DO_NOT_WAIT = 0x100000L
};
typedef RECT D3D11_RECT;
struct D3D11_BOX {
    UINT left;
    UINT top;
    UINT front;
    UINT right;
    UINT bottom;
    UINT back;
};
struct __declspec(uuid("1841e5c8-16b0-489b-bcc8-44cfb0d5deae")) __declspec(novtable) ID3D11DeviceChild : IUnknown {
    virtual void __stdcall GetDevice(ID3D11Device** ppDevice) = 0;
    MAICORD_COM_UNUSED(GetPrivateData);
    MAICORD_COM_UNUSED(SetPrivateData);
    virtual HRESULT __stdcall SetPrivateDataInterface(const GUID& guid, const IUnknown* pData) = 0;
};
enum D3D11_COMPARISON_FUNC {
    D3D11_COMPARISON_NEVER = 1,
};
enum D3D11_BLEND {
    D3D11_BLEND_ONE = 2,
    D3D11_BLEND_INV_SRC_ALPHA = 6,
    D3D11_BLEND_BLEND_FACTOR = 14,
};
enum D3D11_BLEND_OP {
    D3D11_BLEND_OP_ADD = 1,
};
enum D3D11_COLOR_WRITE_ENABLE {
    D3D11_COLOR_WRITE_ENABLE_RED = 1,
    D3D11_COLOR_WRITE_ENABLE_GREEN = 2,
    D3D11_COLOR_WRITE_ENABLE_BLUE = 4,
    D3D11_COLOR_WRITE_ENABLE_ALPHA = 8,
    D3D11_COLOR_WRITE_ENABLE_ALL = (((D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN) | D3D11_COLOR_WRITE_ENABLE_BLUE) | D3D11_COLOR_WRITE_ENABLE_ALPHA)
};
struct D3D11_RENDER_TARGET_BLEND_DESC {
    BOOL BlendEnable;
    D3D11_BLEND SrcBlend;
    D3D11_BLEND DestBlend;
    D3D11_BLEND_OP BlendOp;
    D3D11_BLEND SrcBlendAlpha;
    D3D11_BLEND DestBlendAlpha;
    D3D11_BLEND_OP BlendOpAlpha;
    UINT8 RenderTargetWriteMask;
};
struct D3D11_BLEND_DESC {
    BOOL AlphaToCoverageEnable;
    BOOL IndependentBlendEnable;
    D3D11_RENDER_TARGET_BLEND_DESC RenderTarget[8];
};
struct __declspec(uuid("75b68faa-347d-4159-8f45-a0640f01cd9a")) __declspec(novtable) ID3D11BlendState : ID3D11DeviceChild {
    virtual void __stdcall GetDesc(D3D11_BLEND_DESC* pDesc) = 0;
};
struct D3D11_RASTERIZER_DESC {
    D3D11_FILL_MODE FillMode;
    D3D11_CULL_MODE CullMode;
    BOOL FrontCounterClockwise;
    INT DepthBias;
    FLOAT DepthBiasClamp;
    FLOAT SlopeScaledDepthBias;
    BOOL DepthClipEnable;
    BOOL ScissorEnable;
    BOOL MultisampleEnable;
    BOOL AntialiasedLineEnable;
};
struct __declspec(uuid("9bb4ab81-ab1a-4d8f-b506-fc04200b6ee7")) __declspec(novtable) ID3D11RasterizerState : ID3D11DeviceChild {
    virtual void __stdcall GetDesc(D3D11_RASTERIZER_DESC* pDesc) = 0;
};
struct D3D11_SUBRESOURCE_DATA {
    const void* pSysMem;
    UINT SysMemPitch;
    UINT SysMemSlicePitch;
};
struct D3D11_MAPPED_SUBRESOURCE {
    void* pData;
    UINT RowPitch;
    UINT DepthPitch;
};
struct __declspec(uuid("dc8e63f3-d12b-4952-b47b-5e45026a862d")) __declspec(novtable) ID3D11Resource : ID3D11DeviceChild {
    MAICORD_COM_UNUSED(GetType);
    MAICORD_COM_UNUSED(SetEvictionPriority);
    MAICORD_COM_UNUSED(GetEvictionPriority);
};
struct D3D11_BUFFER_DESC {
    UINT ByteWidth;
    D3D11_USAGE Usage;
    UINT BindFlags;
    UINT CPUAccessFlags;
    UINT MiscFlags;
    UINT StructureByteStride;
};
struct __declspec(uuid("48570b85-d1ee-4fcd-a250-eb350722b037")) __declspec(novtable) ID3D11Buffer : ID3D11Resource {
    virtual void __stdcall GetDesc(D3D11_BUFFER_DESC* pDesc) = 0;
};
struct D3D11_TEXTURE2D_DESC {
    UINT Width;
    UINT Height;
    UINT MipLevels;
    UINT ArraySize;
    DXGI_FORMAT Format;
    DXGI_SAMPLE_DESC SampleDesc;
    D3D11_USAGE Usage;
    UINT BindFlags;
    UINT CPUAccessFlags;
    UINT MiscFlags;
};
struct __declspec(uuid("6f15aaf2-d208-4e89-9ab4-489535d34f9c")) __declspec(novtable) ID3D11Texture2D : ID3D11Resource {
    virtual void __stdcall GetDesc(D3D11_TEXTURE2D_DESC* pDesc) = 0;
};
struct __declspec(uuid("839d1216-bb2e-412b-b7f4-a9dbebe08ed1")) __declspec(novtable) ID3D11View : ID3D11DeviceChild {
    virtual void __stdcall GetResource(ID3D11Resource** ppResource) = 0;
};
struct D3D11_BUFFER_SRV {
    union {
        UINT FirstElement;
        UINT ElementOffset;
    };
    union {
        UINT NumElements;
        UINT ElementWidth;
    };
};
enum D3D11_BUFFEREX_SRV_FLAG {
    D3D11_BUFFEREX_SRV_FLAG_RAW = 0x1
};
struct D3D11_BUFFEREX_SRV {
    UINT FirstElement;
    UINT NumElements;
    UINT Flags;
};
struct D3D11_TEX1D_SRV {
    UINT MostDetailedMip;
    UINT MipLevels;
};
struct D3D11_TEX1D_ARRAY_SRV {
    UINT MostDetailedMip;
    UINT MipLevels;
    UINT FirstArraySlice;
    UINT ArraySize;
};
struct D3D11_TEX2D_SRV {
    UINT MostDetailedMip;
    UINT MipLevels;
};
struct D3D11_TEX2D_ARRAY_SRV {
    UINT MostDetailedMip;
    UINT MipLevels;
    UINT FirstArraySlice;
    UINT ArraySize;
};
struct D3D11_TEX3D_SRV {
    UINT MostDetailedMip;
    UINT MipLevels;
};
struct D3D11_TEXCUBE_SRV {
    UINT MostDetailedMip;
    UINT MipLevels;
};
struct D3D11_TEXCUBE_ARRAY_SRV {
    UINT MostDetailedMip;
    UINT MipLevels;
    UINT First2DArrayFace;
    UINT NumCubes;
};
struct D3D11_TEX2DMS_SRV {
    UINT UnusedField_NothingToDefine;
};
struct D3D11_TEX2DMS_ARRAY_SRV {
    UINT FirstArraySlice;
    UINT ArraySize;
};
struct D3D11_SHADER_RESOURCE_VIEW_DESC {
    DXGI_FORMAT Format;
    D3D11_SRV_DIMENSION ViewDimension;
    union {
        D3D11_BUFFER_SRV Buffer;
        D3D11_TEX1D_SRV Texture1D;
        D3D11_TEX1D_ARRAY_SRV Texture1DArray;
        D3D11_TEX2D_SRV Texture2D;
        D3D11_TEX2D_ARRAY_SRV Texture2DArray;
        D3D11_TEX2DMS_SRV Texture2DMS;
        D3D11_TEX2DMS_ARRAY_SRV Texture2DMSArray;
        D3D11_TEX3D_SRV Texture3D;
        D3D11_TEXCUBE_SRV TextureCube;
        D3D11_TEXCUBE_ARRAY_SRV TextureCubeArray;
        D3D11_BUFFEREX_SRV BufferEx;
    };
};
struct __declspec(uuid("b0e06fe0-8192-4e1a-b1ca-36d7414710b2")) __declspec(novtable) ID3D11ShaderResourceView : ID3D11View {
    virtual void __stdcall GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc) = 0;
};
struct D3D11_BUFFER_RTV {
    union {
        UINT FirstElement;
        UINT ElementOffset;
    };
    union {
        UINT NumElements;
        UINT ElementWidth;
    };
};
struct D3D11_TEX1D_RTV {
    UINT MipSlice;
};
struct D3D11_TEX1D_ARRAY_RTV {
    UINT MipSlice;
    UINT FirstArraySlice;
    UINT ArraySize;
};
struct D3D11_TEX2D_RTV {
    UINT MipSlice;
};
struct D3D11_TEX2DMS_RTV {
    UINT UnusedField_NothingToDefine;
};
struct D3D11_TEX2D_ARRAY_RTV {
    UINT MipSlice;
    UINT FirstArraySlice;
    UINT ArraySize;
};
struct D3D11_TEX2DMS_ARRAY_RTV {
    UINT FirstArraySlice;
    UINT ArraySize;
};
struct D3D11_TEX3D_RTV {
    UINT MipSlice;
    UINT FirstWSlice;
    UINT WSize;
};
struct D3D11_RENDER_TARGET_VIEW_DESC {
    DXGI_FORMAT Format;
    D3D11_RTV_DIMENSION ViewDimension;
    union {
        D3D11_BUFFER_RTV Buffer;
        D3D11_TEX1D_RTV Texture1D;
        D3D11_TEX1D_ARRAY_RTV Texture1DArray;
        D3D11_TEX2D_RTV Texture2D;
        D3D11_TEX2D_ARRAY_RTV Texture2DArray;
        D3D11_TEX2DMS_RTV Texture2DMS;
        D3D11_TEX2DMS_ARRAY_RTV Texture2DMSArray;
        D3D11_TEX3D_RTV Texture3D;
    };
};
struct __declspec(uuid("dfdba067-0b8d-4865-875b-d7b4516cc164")) __declspec(novtable) ID3D11RenderTargetView : ID3D11View {
    virtual void __stdcall GetDesc(D3D11_RENDER_TARGET_VIEW_DESC* pDesc) = 0;
};
struct D3D11_TEX1D_DSV {
    UINT MipSlice;
};
struct D3D11_TEX1D_ARRAY_DSV {
    UINT MipSlice;
    UINT FirstArraySlice;
    UINT ArraySize;
};
struct D3D11_TEX2D_DSV {
    UINT MipSlice;
};
struct D3D11_TEX2D_ARRAY_DSV {
    UINT MipSlice;
    UINT FirstArraySlice;
    UINT ArraySize;
};
struct D3D11_TEX2DMS_DSV {
    UINT UnusedField_NothingToDefine;
};
struct D3D11_TEX2DMS_ARRAY_DSV {
    UINT FirstArraySlice;
    UINT ArraySize;
};
struct D3D11_DEPTH_STENCIL_VIEW_DESC {
    DXGI_FORMAT Format;
    D3D11_DSV_DIMENSION ViewDimension;
    UINT Flags;
    union {
        D3D11_TEX1D_DSV Texture1D;
        D3D11_TEX1D_ARRAY_DSV Texture1DArray;
        D3D11_TEX2D_DSV Texture2D;
        D3D11_TEX2D_ARRAY_DSV Texture2DArray;
        D3D11_TEX2DMS_DSV Texture2DMS;
        D3D11_TEX2DMS_ARRAY_DSV Texture2DMSArray;
    };
};
struct __declspec(uuid("9fdac92a-1876-48c3-afad-25b94f84a9b6")) __declspec(novtable) ID3D11DepthStencilView : ID3D11View {
    virtual void __stdcall GetDesc(D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc) = 0;
};
enum D3D11_BUFFER_UAV_FLAG {
    D3D11_BUFFER_UAV_FLAG_RAW = 0x1,
};
struct D3D11_BUFFER_UAV {
    UINT FirstElement;
    UINT NumElements;
    UINT Flags;
};
struct D3D11_TEX1D_UAV {
    UINT MipSlice;
};
struct D3D11_TEX1D_ARRAY_UAV {
    UINT MipSlice;
    UINT FirstArraySlice;
    UINT ArraySize;
};
struct D3D11_TEX2D_UAV {
    UINT MipSlice;
};
struct D3D11_TEX2D_ARRAY_UAV {
    UINT MipSlice;
    UINT FirstArraySlice;
    UINT ArraySize;
};
struct D3D11_TEX3D_UAV {
    UINT MipSlice;
    UINT FirstWSlice;
    UINT WSize;
};
struct D3D11_UNORDERED_ACCESS_VIEW_DESC {
    DXGI_FORMAT Format;
    D3D11_UAV_DIMENSION ViewDimension;
    union {
        D3D11_BUFFER_UAV Buffer;
        D3D11_TEX1D_UAV Texture1D;
        D3D11_TEX1D_ARRAY_UAV Texture1DArray;
        D3D11_TEX2D_UAV Texture2D;
        D3D11_TEX2D_ARRAY_UAV Texture2DArray;
        D3D11_TEX3D_UAV Texture3D;
    };
};
struct __declspec(uuid("28acf509-7f5c-48f6-8611-f316010a6380")) __declspec(novtable) ID3D11UnorderedAccessView : ID3D11View {
    virtual void __stdcall GetDesc(D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc) = 0;
};
struct __declspec(uuid("3b301d64-d678-4289-8897-22f8928b72f3")) __declspec(novtable) ID3D11VertexShader : public ID3D11DeviceChild {
};
struct __declspec(uuid("ea82e40d-51dc-4f33-93d4-db7c9125ae8c")) __declspec(novtable) ID3D11PixelShader : public ID3D11DeviceChild {
};
struct __declspec(uuid("4f5b196e-c2bd-495e-bd01-1fded38e4969")) __declspec(novtable) ID3D11ComputeShader : public ID3D11DeviceChild {
};
struct __declspec(uuid("e4819ddc-4cf0-4025-bd26-5de82a3e07b7")) __declspec(novtable) ID3D11InputLayout : public ID3D11DeviceChild {
};
enum D3D11_FILTER {
    D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT = 0x14,
};
enum D3D11_TEXTURE_ADDRESS_MODE {
    D3D11_TEXTURE_ADDRESS_CLAMP = 3,
};
struct D3D11_SAMPLER_DESC {
    D3D11_FILTER Filter;
    D3D11_TEXTURE_ADDRESS_MODE AddressU;
    D3D11_TEXTURE_ADDRESS_MODE AddressV;
    D3D11_TEXTURE_ADDRESS_MODE AddressW;
    FLOAT MipLODBias;
    UINT MaxAnisotropy;
    D3D11_COMPARISON_FUNC ComparisonFunc;
    FLOAT BorderColor[4];
    FLOAT MinLOD;
    FLOAT MaxLOD;
};
struct __declspec(uuid("da6fea51-564c-4487-9810-f0d0f9b4e3a5")) __declspec(novtable) ID3D11SamplerState : ID3D11DeviceChild {
    virtual void __stdcall GetDesc(D3D11_SAMPLER_DESC* pDesc) = 0;
};
enum D3D11_FORMAT_SUPPORT {
    D3D11_FORMAT_SUPPORT_TEXTURE2D = 0x20,
    D3D11_FORMAT_SUPPORT_SHADER_LOAD = 0x100,
    D3D11_FORMAT_SUPPORT_SHADER_SAMPLE = 0x200,
    D3D11_FORMAT_SUPPORT_RENDER_TARGET = 0x4000,
    D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW = 0x2000000,
    D3D11_FORMAT_SUPPORT_DECODER_OUTPUT = 0x8000000,
};
struct __declspec(uuid("4b35d0cd-1e15-4258-9c98-1b1333f6dd3b")) __declspec(novtable) ID3D11Asynchronous : ID3D11DeviceChild {
    MAICORD_COM_UNUSED(GetDataSize);
};
enum D3D11_ASYNC_GETDATA_FLAG {
    D3D11_ASYNC_GETDATA_DONOTFLUSH = 0x1
};
enum D3D11_QUERY {
    D3D11_QUERY_EVENT = 0,
    D3D11_QUERY_OCCLUSION = (D3D11_QUERY_EVENT + 1),
    D3D11_QUERY_TIMESTAMP = (D3D11_QUERY_OCCLUSION + 1),
    D3D11_QUERY_TIMESTAMP_DISJOINT = (D3D11_QUERY_TIMESTAMP + 1),
    D3D11_QUERY_PIPELINE_STATISTICS = (D3D11_QUERY_TIMESTAMP_DISJOINT + 1),
    D3D11_QUERY_OCCLUSION_PREDICATE = (D3D11_QUERY_PIPELINE_STATISTICS + 1),
    D3D11_QUERY_SO_STATISTICS = (D3D11_QUERY_OCCLUSION_PREDICATE + 1),
    D3D11_QUERY_SO_OVERFLOW_PREDICATE = (D3D11_QUERY_SO_STATISTICS + 1),
    D3D11_QUERY_SO_STATISTICS_STREAM0 = (D3D11_QUERY_SO_OVERFLOW_PREDICATE + 1),
    D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0 = (D3D11_QUERY_SO_STATISTICS_STREAM0 + 1),
    D3D11_QUERY_SO_STATISTICS_STREAM1 = (D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0 + 1),
    D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1 = (D3D11_QUERY_SO_STATISTICS_STREAM1 + 1),
    D3D11_QUERY_SO_STATISTICS_STREAM2 = (D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1 + 1),
    D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2 = (D3D11_QUERY_SO_STATISTICS_STREAM2 + 1),
    D3D11_QUERY_SO_STATISTICS_STREAM3 = (D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2 + 1),
};
struct D3D11_QUERY_DESC {
    D3D11_QUERY Query;
    UINT MiscFlags;
};
struct __declspec(uuid("d6c00747-87b7-425e-b84d-44d108560afd")) __declspec(novtable) ID3D11Query : ID3D11Asynchronous {
    virtual void __stdcall GetDesc(D3D11_QUERY_DESC* pDesc) = 0;
};
struct D3D11_QUERY_DATA_TIMESTAMP_DISJOINT {
    UINT64 Frequency;
    BOOL Disjoint;
};
struct D3D11_QUERY_DATA_PIPELINE_STATISTICS {
    UINT64 IAVertices;
    UINT64 IAPrimitives;
    UINT64 VSInvocations;
    UINT64 GSInvocations;
    UINT64 GSPrimitives;
    UINT64 CInvocations;
    UINT64 CPrimitives;
    UINT64 PSInvocations;
    UINT64 HSInvocations;
    UINT64 DSInvocations;
    UINT64 CSInvocations;
};
struct D3D11_CLASS_INSTANCE_DESC {
    UINT InstanceId;
    UINT InstanceIndex;
    UINT TypeId;
    UINT ConstantBuffer;
    UINT BaseConstantBufferOffset;
    UINT BaseTexture;
    UINT BaseSampler;
    BOOL Created;
};
struct __declspec(uuid("a6cd7faa-b0b7-4a2f-9436-8662a65797cb")) __declspec(novtable) ID3D11ClassInstance : ID3D11DeviceChild {
    MAICORD_COM_UNUSED(GetClassLinkage);
    virtual void __stdcall GetDesc(D3D11_CLASS_INSTANCE_DESC* pDesc) = 0;
};
struct __declspec(uuid("ddf57cba-9543-46e4-a12b-f207a0fe7fed")) __declspec(novtable) ID3D11ClassLinkage : ID3D11DeviceChild {
};
enum D3D11_FEATURE {
    D3D11_FEATURE_THREADING = 0,
    D3D11_FEATURE_DOUBLES = (D3D11_FEATURE_THREADING + 1),
    D3D11_FEATURE_FORMAT_SUPPORT = (D3D11_FEATURE_DOUBLES + 1),
    D3D11_FEATURE_FORMAT_SUPPORT2 = (D3D11_FEATURE_FORMAT_SUPPORT + 1),
    D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS = (D3D11_FEATURE_FORMAT_SUPPORT2 + 1),
    D3D11_FEATURE_D3D11_OPTIONS = (D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS + 1),
    D3D11_FEATURE_ARCHITECTURE_INFO = (D3D11_FEATURE_D3D11_OPTIONS + 1),
    D3D11_FEATURE_D3D9_OPTIONS = (D3D11_FEATURE_ARCHITECTURE_INFO + 1),
    D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT = (D3D11_FEATURE_D3D9_OPTIONS + 1),
    D3D11_FEATURE_D3D9_SHADOW_SUPPORT = (D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT + 1),
    D3D11_FEATURE_D3D11_OPTIONS1 = (D3D11_FEATURE_D3D9_SHADOW_SUPPORT + 1),
    D3D11_FEATURE_D3D9_SIMPLE_INSTANCING_SUPPORT = (D3D11_FEATURE_D3D11_OPTIONS1 + 1),
    D3D11_FEATURE_MARKER_SUPPORT = (D3D11_FEATURE_D3D9_SIMPLE_INSTANCING_SUPPORT + 1),
    D3D11_FEATURE_D3D9_OPTIONS1 = (D3D11_FEATURE_MARKER_SUPPORT + 1),
    D3D11_FEATURE_D3D11_OPTIONS2 = (D3D11_FEATURE_D3D9_OPTIONS1 + 1),
    D3D11_FEATURE_D3D11_OPTIONS3 = (D3D11_FEATURE_D3D11_OPTIONS2 + 1),
    D3D11_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT = (D3D11_FEATURE_D3D11_OPTIONS3 + 1),
    D3D11_FEATURE_D3D11_OPTIONS4 = (D3D11_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT + 1),
    D3D11_FEATURE_SHADER_CACHE = (D3D11_FEATURE_D3D11_OPTIONS4 + 1),
    D3D11_FEATURE_D3D11_OPTIONS5 = (D3D11_FEATURE_SHADER_CACHE + 1),
    D3D11_FEATURE_DISPLAYABLE = (D3D11_FEATURE_D3D11_OPTIONS5 + 1),
};
struct D3D11_FEATURE_DATA_D3D11_OPTIONS {
    BOOL OutputMergerLogicOp;
    BOOL UAVOnlyRenderingForcedSampleCount;
    BOOL DiscardAPIsSeenByDriver;
    BOOL FlagsForUpdateAndCopySeenByDriver;
    BOOL ClearView;
    BOOL CopyWithOverlap;
    BOOL ConstantBufferPartialUpdate;
    BOOL ConstantBufferOffsetting;
    BOOL MapNoOverwriteOnDynamicConstantBuffer;
    BOOL MapNoOverwriteOnDynamicBufferSRV;
    BOOL MultisampleRTVWithForcedSampleCountOne;
    BOOL SAD4ShaderInstructions;
    BOOL ExtendedDoublesShaderInstructions;
    BOOL ExtendedResourceSharing;
};
struct __declspec(uuid("c0bfa96c-e089-44fb-8eaf-26f8796190da")) __declspec(novtable) ID3D11DeviceContext : ID3D11DeviceChild {
    virtual void __stdcall VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
        ID3D11Buffer* const* ppConstantBuffers) = 0;
    virtual void __stdcall PSSetShaderResources(UINT StartSlot, UINT NumViews,
        ID3D11ShaderResourceView* const* ppShaderResourceViews) = 0;
    virtual void __stdcall PSSetShader(ID3D11PixelShader* pPixelShader, ID3D11ClassInstance* const* ppClassInstances,
        UINT NumClassInstances) = 0;
    virtual void __stdcall PSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) = 0;
    virtual void __stdcall VSSetShader(ID3D11VertexShader* pVertexShader, ID3D11ClassInstance* const* ppClassInstances,
        UINT NumClassInstances) = 0;
    MAICORD_COM_UNUSED(DrawIndexed);
    virtual void __stdcall Draw(UINT VertexCount, UINT StartVertexLocation) = 0;
    virtual HRESULT __stdcall Map(ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags,
        D3D11_MAPPED_SUBRESOURCE* pMappedResource) = 0;
    virtual void __stdcall Unmap(ID3D11Resource* pResource, UINT Subresource) = 0;
    virtual void __stdcall PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
        ID3D11Buffer* const* ppConstantBuffers) = 0;
    virtual void __stdcall IASetInputLayout(ID3D11InputLayout* pInputLayout) = 0;
    virtual void __stdcall IASetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppVertexBuffers,
        const UINT* pStrides, const UINT* pOffsets) = 0;
    MAICORD_COM_UNUSED(IASetIndexBuffer);
    MAICORD_COM_UNUSED(DrawIndexedInstanced);
    virtual void __stdcall DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation,
        UINT StartInstanceLocation) = 0;
    MAICORD_COM_UNUSED(GSSetConstantBuffers);
    MAICORD_COM_UNUSED(GSSetShader);
    virtual void __stdcall IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) = 0;
    MAICORD_COM_UNUSED(VSSetShaderResources);
    MAICORD_COM_UNUSED(VSSetSamplers);
    virtual void __stdcall Begin(ID3D11Asynchronous* pAsync) = 0;
    virtual void __stdcall End(ID3D11Asynchronous* pAsync) = 0;
    virtual HRESULT __stdcall GetData(ID3D11Asynchronous* pAsync, void* pData, UINT DataSize, UINT GetDataFlags) = 0;
    MAICORD_COM_UNUSED(SetPredication);
    MAICORD_COM_UNUSED(GSSetShaderResources);
    MAICORD_COM_UNUSED(GSSetSamplers);
    virtual void __stdcall OMSetRenderTargets(UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews,
        ID3D11DepthStencilView* pDepthStencilView) = 0;
    MAICORD_COM_UNUSED(OMSetRenderTargetsAndUnorderedAccessViews);
    virtual void __stdcall OMSetBlendState(ID3D11BlendState* pBlendState, const FLOAT BlendFactor[4],
        UINT SampleMask) = 0;
    MAICORD_COM_UNUSED(OMSetDepthStencilState);
    MAICORD_COM_UNUSED(SOSetTargets);
    MAICORD_COM_UNUSED(DrawAuto);
    MAICORD_COM_UNUSED(DrawIndexedInstancedIndirect);
    MAICORD_COM_UNUSED(DrawInstancedIndirect);
    virtual void __stdcall Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) = 0;
    MAICORD_COM_UNUSED(DispatchIndirect);
    virtual void __stdcall RSSetState(ID3D11RasterizerState* pRasterizerState) = 0;
    virtual void __stdcall RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT* pViewports) = 0;
    virtual void __stdcall RSSetScissorRects(UINT NumRects, const D3D11_RECT* pRects) = 0;
    virtual void __stdcall CopySubresourceRegion(ID3D11Resource* pDstResource, UINT DstSubresource, UINT DstX,
        UINT DstY, UINT DstZ, ID3D11Resource* pSrcResource, UINT SrcSubresource, const D3D11_BOX* pSrcBox) = 0;
    virtual void __stdcall CopyResource(ID3D11Resource* pDstResource, ID3D11Resource* pSrcResource) = 0;
    virtual void __stdcall UpdateSubresource(ID3D11Resource* pDstResource, UINT DstSubresource,
        const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) = 0;
    MAICORD_COM_UNUSED(CopyStructureCount);
    virtual void __stdcall ClearRenderTargetView(ID3D11RenderTargetView* pRenderTargetView,
        const FLOAT ColorRGBA[4]) = 0;
    virtual void __stdcall ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView* pUnorderedAccessView,
        const UINT Values[4]) = 0;
    virtual void __stdcall ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView* pUnorderedAccessView,
        const FLOAT Values[4]) = 0;
    MAICORD_COM_UNUSED(ClearDepthStencilView);
    MAICORD_COM_UNUSED(GenerateMips);
    MAICORD_COM_UNUSED(SetResourceMinLOD);
    MAICORD_COM_UNUSED(GetResourceMinLOD);
    MAICORD_COM_UNUSED(ResolveSubresource);
    MAICORD_COM_UNUSED(ExecuteCommandList);
    MAICORD_COM_UNUSED(HSSetShaderResources);
    MAICORD_COM_UNUSED(HSSetShader);
    MAICORD_COM_UNUSED(HSSetSamplers);
    MAICORD_COM_UNUSED(HSSetConstantBuffers);
    MAICORD_COM_UNUSED(DSSetShaderResources);
    MAICORD_COM_UNUSED(DSSetShader);
    MAICORD_COM_UNUSED(DSSetSamplers);
    MAICORD_COM_UNUSED(DSSetConstantBuffers);
    virtual void __stdcall CSSetShaderResources(UINT StartSlot, UINT NumViews,
        ID3D11ShaderResourceView* const* ppShaderResourceViews) = 0;
    virtual void __stdcall CSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs,
        ID3D11UnorderedAccessView* const* ppUnorderedAccessViews, const UINT* pUAVInitialCounts) = 0;
    virtual void __stdcall CSSetShader(ID3D11ComputeShader* pComputeShader,
        ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) = 0;
    MAICORD_COM_UNUSED(CSSetSamplers);
    virtual void __stdcall CSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
        ID3D11Buffer* const* ppConstantBuffers) = 0;
    MAICORD_COM_UNUSED(VSGetConstantBuffers);
    MAICORD_COM_UNUSED(PSGetShaderResources);
    MAICORD_COM_UNUSED(PSGetShader);
    MAICORD_COM_UNUSED(PSGetSamplers);
    MAICORD_COM_UNUSED(VSGetShader);
    MAICORD_COM_UNUSED(PSGetConstantBuffers);
    MAICORD_COM_UNUSED(IAGetInputLayout);
    MAICORD_COM_UNUSED(IAGetVertexBuffers);
    MAICORD_COM_UNUSED(IAGetIndexBuffer);
    MAICORD_COM_UNUSED(GSGetConstantBuffers);
    MAICORD_COM_UNUSED(GSGetShader);
    MAICORD_COM_UNUSED(IAGetPrimitiveTopology);
    MAICORD_COM_UNUSED(VSGetShaderResources);
    MAICORD_COM_UNUSED(VSGetSamplers);
    MAICORD_COM_UNUSED(GetPredication);
    MAICORD_COM_UNUSED(GSGetShaderResources);
    MAICORD_COM_UNUSED(GSGetSamplers);
    MAICORD_COM_UNUSED(OMGetRenderTargets);
    MAICORD_COM_UNUSED(OMGetRenderTargetsAndUnorderedAccessViews);
    MAICORD_COM_UNUSED(OMGetBlendState);
    MAICORD_COM_UNUSED(OMGetDepthStencilState);
    MAICORD_COM_UNUSED(SOGetTargets);
    MAICORD_COM_UNUSED(RSGetState);
    MAICORD_COM_UNUSED(RSGetViewports);
    MAICORD_COM_UNUSED(RSGetScissorRects);
    MAICORD_COM_UNUSED(HSGetShaderResources);
    MAICORD_COM_UNUSED(HSGetShader);
    MAICORD_COM_UNUSED(HSGetSamplers);
    MAICORD_COM_UNUSED(HSGetConstantBuffers);
    MAICORD_COM_UNUSED(DSGetShaderResources);
    MAICORD_COM_UNUSED(DSGetShader);
    MAICORD_COM_UNUSED(DSGetSamplers);
    MAICORD_COM_UNUSED(DSGetConstantBuffers);
    virtual void __stdcall CSGetShaderResources(UINT StartSlot, UINT NumViews,
        ID3D11ShaderResourceView** ppShaderResourceViews) = 0;
    virtual void __stdcall CSGetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs,
        ID3D11UnorderedAccessView** ppUnorderedAccessViews) = 0;
    virtual void __stdcall CSGetShader(ID3D11ComputeShader** ppComputeShader, ID3D11ClassInstance** ppClassInstances,
        UINT* pNumClassInstances) = 0;
    MAICORD_COM_UNUSED(CSGetSamplers);
    virtual void __stdcall CSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) = 0;
    virtual void __stdcall ClearState() = 0;
    virtual void __stdcall Flush() = 0;
    MAICORD_COM_UNUSED(GetType);
    MAICORD_COM_UNUSED(GetContextFlags);
    MAICORD_COM_UNUSED(FinishCommandList);
};
struct D3D11_VIDEO_DECODER_DESC {
    GUID Guid;
    UINT SampleWidth;
    UINT SampleHeight;
    DXGI_FORMAT OutputFormat;
};
struct D3D11_VIDEO_DECODER_CONFIG {
    GUID guidConfigBitstreamEncryption;
    GUID guidConfigMBcontrolEncryption;
    GUID guidConfigResidDiffEncryption;
    UINT ConfigBitstreamRaw;
    UINT ConfigMBcontrolRasterOrder;
    UINT ConfigResidDiffHost;
    UINT ConfigSpatialResid8;
    UINT ConfigResid8Subtraction;
    UINT ConfigSpatialHost8or9Clipping;
    UINT ConfigSpatialResidInterleaved;
    UINT ConfigIntraResidUnsigned;
    UINT ConfigResidDiffAccelerator;
    UINT ConfigHostInverseScan;
    UINT ConfigSpecificIDCT;
    UINT Config4GroupedCoefs;
    USHORT ConfigMinRenderTargetBuffCount;
    USHORT ConfigDecoderSpecific;
};
enum D3D11_VIDEO_DECODER_BUFFER_TYPE {
    D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS = 0,
    D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX = 4,
    D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL = 5,
    D3D11_VIDEO_DECODER_BUFFER_BITSTREAM = 6,
};
struct D3D11_ENCRYPTED_BLOCK_INFO {
    UINT NumEncryptedBytesAtBeginning;
    UINT NumBytesInSkipPattern;
    UINT NumBytesInEncryptPattern;
};
struct D3D11_VIDEO_DECODER_BUFFER_DESC {
    D3D11_VIDEO_DECODER_BUFFER_TYPE BufferType;
    UINT BufferIndex;
    UINT DataOffset;
    UINT DataSize;
    UINT FirstMBaddress;
    UINT NumMBsInBuffer;
    UINT Width;
    UINT Height;
    UINT Stride;
    UINT ReservedBits;
    void* pIV;
    UINT IVSize;
    BOOL PartialEncryption;
    D3D11_ENCRYPTED_BLOCK_INFO EncryptedBlockInfo;
};
struct __declspec(uuid("3C9C5B51-995D-48d1-9B8D-FA5CAEDED65C")) __declspec(novtable) ID3D11VideoDecoder : ID3D11DeviceChild {
};
enum D3D11_VDOV_DIMENSION {
    D3D11_VDOV_DIMENSION_TEXTURE2D = 1
};
struct D3D11_TEX2D_VDOV {
    UINT ArraySlice;
};
struct D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC {
    GUID DecodeProfile;
    D3D11_VDOV_DIMENSION ViewDimension;
    union {
        D3D11_TEX2D_VDOV Texture2D;
    };
};
struct __declspec(uuid("C2931AEA-2A85-4f20-860F-FBA1FD256E18")) __declspec(novtable) ID3D11VideoDecoderOutputView : ID3D11View {
    virtual void __stdcall GetDesc(D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC* pDesc) = 0;
};
struct __declspec(uuid("61F21C45-3C0E-4a74-9CEA-67100D9AD5E4")) __declspec(novtable) ID3D11VideoContext : ID3D11DeviceChild {
    virtual HRESULT __stdcall GetDecoderBuffer(ID3D11VideoDecoder* pDecoder, D3D11_VIDEO_DECODER_BUFFER_TYPE Type,
        UINT* pBufferSize, void** ppBuffer) = 0;
    virtual HRESULT __stdcall ReleaseDecoderBuffer(ID3D11VideoDecoder* pDecoder,
        D3D11_VIDEO_DECODER_BUFFER_TYPE Type) = 0;
    virtual HRESULT __stdcall DecoderBeginFrame(ID3D11VideoDecoder* pDecoder, ID3D11VideoDecoderOutputView* pView,
        UINT ContentKeySize, const void* pContentKey) = 0;
    virtual HRESULT __stdcall DecoderEndFrame(ID3D11VideoDecoder* pDecoder) = 0;
    virtual HRESULT __stdcall SubmitDecoderBuffers(ID3D11VideoDecoder* pDecoder, UINT NumBuffers,
        const D3D11_VIDEO_DECODER_BUFFER_DESC* pBufferDesc) = 0;
};
struct __declspec(uuid("10EC4D5B-975A-4689-B9E4-D0AAC30FE333")) __declspec(novtable) ID3D11VideoDevice : IUnknown {
    virtual HRESULT __stdcall CreateVideoDecoder(const D3D11_VIDEO_DECODER_DESC* pVideoDesc,
        const D3D11_VIDEO_DECODER_CONFIG* pConfig, ID3D11VideoDecoder** ppDecoder) = 0;
    MAICORD_COM_UNUSED(CreateVideoProcessor);
    MAICORD_COM_UNUSED(CreateAuthenticatedChannel);
    MAICORD_COM_UNUSED(CreateCryptoSession);
    virtual HRESULT __stdcall CreateVideoDecoderOutputView(ID3D11Resource* pResource,
        const D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC* pDesc, ID3D11VideoDecoderOutputView** ppVDOVView) = 0;
    MAICORD_COM_UNUSED(CreateVideoProcessorInputView);
    MAICORD_COM_UNUSED(CreateVideoProcessorOutputView);
    MAICORD_COM_UNUSED(CreateVideoProcessorEnumerator);
    MAICORD_COM_UNUSED(GetVideoDecoderProfileCount);
    MAICORD_COM_UNUSED(GetVideoDecoderProfile);
    virtual HRESULT __stdcall CheckVideoDecoderFormat(const GUID* pDecoderProfile, DXGI_FORMAT Format,
        BOOL* pSupported) = 0;
    virtual HRESULT __stdcall GetVideoDecoderConfigCount(const D3D11_VIDEO_DECODER_DESC* pDesc, UINT* pCount) = 0;
    virtual HRESULT __stdcall GetVideoDecoderConfig(const D3D11_VIDEO_DECODER_DESC* pDesc, UINT Index,
        D3D11_VIDEO_DECODER_CONFIG* pConfig) = 0;
    MAICORD_COM_UNUSED(GetContentProtectionCaps);
    MAICORD_COM_UNUSED(CheckCryptoKeyExchange);
    MAICORD_COM_UNUSED(SetPrivateData);
    virtual HRESULT __stdcall SetPrivateDataInterface(const GUID& guid, const IUnknown* pData) = 0;
};
struct __declspec(uuid("db6f6ddb-ac77-4e88-8253-819df9bbf140")) __declspec(novtable) ID3D11Device : IUnknown {
    virtual HRESULT __stdcall CreateBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData,
        ID3D11Buffer** ppBuffer) = 0;
    MAICORD_COM_UNUSED(CreateTexture1D);
    virtual HRESULT __stdcall CreateTexture2D(const D3D11_TEXTURE2D_DESC* pDesc,
        const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D) = 0;
    MAICORD_COM_UNUSED(CreateTexture3D);
    virtual HRESULT __stdcall CreateShaderResourceView(ID3D11Resource* pResource,
        const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc, ID3D11ShaderResourceView** ppSRView) = 0;
    virtual HRESULT __stdcall CreateUnorderedAccessView(ID3D11Resource* pResource,
        const D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc, ID3D11UnorderedAccessView** ppUAView) = 0;
    virtual HRESULT __stdcall CreateRenderTargetView(ID3D11Resource* pResource,
        const D3D11_RENDER_TARGET_VIEW_DESC* pDesc, ID3D11RenderTargetView** ppRTView) = 0;
    MAICORD_COM_UNUSED(CreateDepthStencilView);
    virtual HRESULT __stdcall CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements,
        const void* pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength, ID3D11InputLayout** ppInputLayout) = 0;
    virtual HRESULT __stdcall CreateVertexShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
        ID3D11ClassLinkage* pClassLinkage, ID3D11VertexShader** ppVertexShader) = 0;
    MAICORD_COM_UNUSED(CreateGeometryShader);
    MAICORD_COM_UNUSED(CreateGeometryShaderWithStreamOutput);
    virtual HRESULT __stdcall CreatePixelShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
        ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader) = 0;
    MAICORD_COM_UNUSED(CreateHullShader);
    MAICORD_COM_UNUSED(CreateDomainShader);
    virtual HRESULT __stdcall CreateComputeShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
        ID3D11ClassLinkage* pClassLinkage, ID3D11ComputeShader** ppComputeShader) = 0;
    MAICORD_COM_UNUSED(CreateClassLinkage);
    virtual HRESULT __stdcall CreateBlendState(const D3D11_BLEND_DESC* pBlendStateDesc,
        ID3D11BlendState** ppBlendState) = 0;
    MAICORD_COM_UNUSED(CreateDepthStencilState);
    virtual HRESULT __stdcall CreateRasterizerState(const D3D11_RASTERIZER_DESC* pRasterizerDesc,
        ID3D11RasterizerState** ppRasterizerState) = 0;
    virtual HRESULT __stdcall CreateSamplerState(const D3D11_SAMPLER_DESC* pSamplerDesc,
        ID3D11SamplerState** ppSamplerState) = 0;
    virtual HRESULT __stdcall CreateQuery(const D3D11_QUERY_DESC* pQueryDesc, ID3D11Query** ppQuery) = 0;
    MAICORD_COM_UNUSED(CreatePredicate);
    MAICORD_COM_UNUSED(CreateCounter);
    MAICORD_COM_UNUSED(CreateDeferredContext);
    MAICORD_COM_UNUSED(OpenSharedResource);
    virtual HRESULT __stdcall CheckFormatSupport(DXGI_FORMAT Format, UINT* pFormatSupport) = 0;
    MAICORD_COM_UNUSED(CheckMultisampleQualityLevels);
    MAICORD_COM_UNUSED(CheckCounterInfo);
    MAICORD_COM_UNUSED(CheckCounter);
    virtual HRESULT __stdcall CheckFeatureSupport(D3D11_FEATURE Feature, void* pFeatureSupportData,
        UINT FeatureSupportDataSize) = 0;
    MAICORD_COM_UNUSED(GetPrivateData);
    MAICORD_COM_UNUSED(SetPrivateData);
    virtual HRESULT __stdcall SetPrivateDataInterface(const GUID& guid, const IUnknown* pData) = 0;
    MAICORD_COM_UNUSED(GetFeatureLevel);
    MAICORD_COM_UNUSED(GetCreationFlags);
    virtual HRESULT __stdcall GetDeviceRemovedReason() = 0;
    virtual void __stdcall GetImmediateContext(ID3D11DeviceContext** ppImmediateContext) = 0;
    MAICORD_COM_UNUSED(SetExceptionMode);
    MAICORD_COM_UNUSED(GetExceptionMode);
};
enum D3D11_CREATE_DEVICE_FLAG {
    D3D11_CREATE_DEVICE_DEBUG = 0x2,
    D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS = 0x8,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT = 0x20,
    D3D11_CREATE_DEVICE_VIDEO_SUPPORT = 0x800
};
// <d3d11_1.h>
struct __declspec(uuid("bb2c6faa-b5fb-4082-8e6b-388b8cfa90e1")) __declspec(novtable) ID3D11DeviceContext1 : ID3D11DeviceContext {
    MAICORD_COM_UNUSED(CopySubresourceRegion1);
    MAICORD_COM_UNUSED(UpdateSubresource1);
    MAICORD_COM_UNUSED(DiscardResource);
    MAICORD_COM_UNUSED(DiscardView);
    MAICORD_COM_UNUSED(VSSetConstantBuffers1);
    MAICORD_COM_UNUSED(HSSetConstantBuffers1);
    MAICORD_COM_UNUSED(DSSetConstantBuffers1);
    MAICORD_COM_UNUSED(GSSetConstantBuffers1);
    MAICORD_COM_UNUSED(PSSetConstantBuffers1);
    virtual void __stdcall CSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
        ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) = 0;
    MAICORD_COM_UNUSED(VSGetConstantBuffers1);
    MAICORD_COM_UNUSED(HSGetConstantBuffers1);
    MAICORD_COM_UNUSED(DSGetConstantBuffers1);
    MAICORD_COM_UNUSED(GSGetConstantBuffers1);
    MAICORD_COM_UNUSED(PSGetConstantBuffers1);
    MAICORD_COM_UNUSED(CSGetConstantBuffers1);
    MAICORD_COM_UNUSED(SwapDeviceContextState);
    virtual void __stdcall ClearView(ID3D11View* pView, const FLOAT Color[4], const D3D11_RECT* pRect,
        UINT NumRects) = 0;
    MAICORD_COM_UNUSED(DiscardView1);
};
enum D3D11_VIDEO_DECODER_CAPS {
    D3D11_VIDEO_DECODER_CAPS_UNSUPPORTED = 0x10
};
struct __declspec(uuid("29DA1D51-1321-4454-804B-F5FC9F861F0F")) __declspec(novtable) ID3D11VideoDevice1 : ID3D11VideoDevice {
    MAICORD_COM_UNUSED(GetCryptoSessionPrivateDataSize);
    virtual HRESULT __stdcall GetVideoDecoderCaps(const GUID* pDecoderProfile, UINT SampleWidth, UINT SampleHeight,
        const DXGI_RATIONAL* pFrameRate, UINT BitRate, const GUID* pCryptoType, UINT* pDecoderCaps) = 0;
};
struct __declspec(uuid("a04bfb29-08ef-43d6-a49c-a9bdbdcbe686")) __declspec(novtable) ID3D11Device1 : ID3D11Device {
    MAICORD_COM_UNUSED(GetImmediateContext1);
    MAICORD_COM_UNUSED(CreateDeferredContext1);
    MAICORD_COM_UNUSED(CreateBlendState1);
    MAICORD_COM_UNUSED(CreateRasterizerState1);
    MAICORD_COM_UNUSED(CreateDeviceContextState);
    MAICORD_COM_UNUSED(OpenSharedResource1);
    MAICORD_COM_UNUSED(OpenSharedResourceByName);
};
// <d3d11_2.h>
struct __declspec(uuid("420d5b32-b90c-4da4-bef0-359f6a24a83a")) __declspec(novtable) ID3D11DeviceContext2 : ID3D11DeviceContext1 {
    MAICORD_COM_UNUSED(UpdateTileMappings);
    MAICORD_COM_UNUSED(CopyTileMappings);
    MAICORD_COM_UNUSED(CopyTiles);
    MAICORD_COM_UNUSED(UpdateTiles);
    MAICORD_COM_UNUSED(ResizeTilePool);
    MAICORD_COM_UNUSED(TiledResourceBarrier);
    MAICORD_COM_UNUSED(IsAnnotationEnabled);
    MAICORD_COM_UNUSED(SetMarkerInt);
    MAICORD_COM_UNUSED(BeginEventInt);
    MAICORD_COM_UNUSED(EndEvent);
};
struct __declspec(uuid("9d06dffa-d1e5-4d07-83a8-1bb123f2f841")) __declspec(novtable) ID3D11Device2 : ID3D11Device1 {
    MAICORD_COM_UNUSED(GetImmediateContext2);
    MAICORD_COM_UNUSED(CreateDeferredContext2);
    MAICORD_COM_UNUSED(GetResourceTiling);
    MAICORD_COM_UNUSED(CheckMultisampleQualityLevels1);
};
// <d3d11_3.h>
enum D3D11_FENCE_FLAG {
    D3D11_FENCE_FLAG_NONE = 0,
};
struct __declspec(uuid("b4e3c01d-e79e-4637-91b2-510e9f4c9b8f")) __declspec(novtable) ID3D11DeviceContext3 : ID3D11DeviceContext2 {
    MAICORD_COM_UNUSED(Flush1);
    MAICORD_COM_UNUSED(SetHardwareProtectionState);
    MAICORD_COM_UNUSED(GetHardwareProtectionState);
};
struct __declspec(uuid("affde9d1-1df7-4bb7-8a34-0f46251dab80")) __declspec(novtable) ID3D11Fence : ID3D11DeviceChild {
    MAICORD_COM_UNUSED(CreateSharedHandle);
    MAICORD_COM_UNUSED(GetCompletedValue);
    virtual HRESULT __stdcall SetEventOnCompletion(UINT64 Value, HANDLE hEvent) = 0;
};
struct __declspec(uuid("917600da-f58c-4c33-98d8-3e15b390fa24")) __declspec(novtable) ID3D11DeviceContext4 : ID3D11DeviceContext3 {
    virtual HRESULT __stdcall Signal(ID3D11Fence* pFence, UINT64 Value) = 0;
};
struct __declspec(uuid("A05C8C37-D2C6-4732-B3A0-9CE0B0DC9AE6")) __declspec(novtable) ID3D11Device3 : ID3D11Device2 {
    MAICORD_COM_UNUSED(CreateTexture2D1);
    MAICORD_COM_UNUSED(CreateTexture3D1);
    MAICORD_COM_UNUSED(CreateRasterizerState2);
    MAICORD_COM_UNUSED(CreateShaderResourceView1);
    MAICORD_COM_UNUSED(CreateUnorderedAccessView1);
    MAICORD_COM_UNUSED(CreateRenderTargetView1);
    MAICORD_COM_UNUSED(CreateQuery1);
    MAICORD_COM_UNUSED(GetImmediateContext3);
    MAICORD_COM_UNUSED(CreateDeferredContext3);
    MAICORD_COM_UNUSED(WriteToSubresource);
    MAICORD_COM_UNUSED(ReadFromSubresource);
};
// <d3d11_4.h>
struct __declspec(uuid("8992ab71-02e6-4b8d-ba48-b056dcda42c4")) __declspec(novtable) ID3D11Device4 : ID3D11Device3 {
    MAICORD_COM_UNUSED(RegisterDeviceRemovedEvent);
    MAICORD_COM_UNUSED(UnregisterDeviceRemoved);
};
struct __declspec(uuid("8ffde202-a0e7-45df-9e01-e837801b5ea0")) __declspec(novtable) ID3D11Device5 : ID3D11Device4 {
    MAICORD_COM_UNUSED(OpenSharedFence);
    virtual HRESULT __stdcall CreateFence(UINT64 InitialValue, D3D11_FENCE_FLAG Flags, const IID& ReturnedInterface,
        void** ppFence) = 0;
};
struct __declspec(uuid("9B7E4E00-342C-4106-A19F-4F2704F689F0")) __declspec(novtable) ID3D11Multithread : IUnknown {
    virtual void __stdcall Enter() = 0;
    virtual void __stdcall Leave() = 0;
    virtual BOOL __stdcall SetMultithreadProtected(BOOL bMTProtect) = 0;
};
MAICORD_SYS_TYPES_END

// <d3d11.h>
DEFINE_GUID(D3D11_DECODER_PROFILE_H264_VLD_NOFGT, 0x1b81be68, 0xa0c7, 0x11d3, 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5);
DEFINE_GUID(D3D11_DECODER_PROFILE_HEVC_VLD_MAIN, 0x5b11d51b, 0x2f4c, 0x4452, 0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0);
DEFINE_GUID(D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, 0xb8be4ccb, 0xcf53, 0x46ba, 0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a);
extern "C" HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext);
