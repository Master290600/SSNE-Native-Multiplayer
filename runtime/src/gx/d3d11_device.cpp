#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <sstream>
#include "ssne/d3d11_device.h"

namespace ssne::d3d11 {
using Microsoft::WRL::ComPtr;
namespace {
bool checked(HRESULT hr,const char* operation,std::string& error){
    if(SUCCEEDED(hr))return true;
    std::ostringstream out;out<<operation<<" failed (HRESULT 0x"<<std::hex<<uint32_t(hr)<<")";error=out.str();return false;
}
constexpr char shader[]=R"(
struct Input { float4 pos:POSITION; float4 color:COLOR0; float2 uv:TEXCOORD0; };
struct Output { float4 pos:SV_Position; float4 color:COLOR0; float2 uv:TEXCOORD0; };
Output vs(Input i) { Output o; o.pos=float4(i.pos.xy,(i.pos.z+i.pos.w)*0.5,i.pos.w); o.color=i.color; o.uv=i.uv; return o; }
Texture2D image:register(t0); SamplerState sampler0:register(s0);
float4 ps(Output i):SV_Target { return image.Sample(sampler0,i.uv)*i.color; }
)";
}
struct Device::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11Texture2D> back,depth,staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vertices,indices;
    ComPtr<ID3D11ShaderResourceView> image;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> blends[2];
    ComPtr<ID3D11DepthStencilState> depths[2];
    unsigned w=0,h=0,vertex_capacity=0,index_capacity=0;
    std::string description;
    bool suspended=false,frame=false;
    ~Impl(){if(context){context->ClearState();context->Flush();}}
    bool targets(std::string& error){
        if(!checked(swap->GetBuffer(0,IID_PPV_ARGS(back.GetAddressOf())),"GetBuffer",error) ||
           !checked(device->CreateRenderTargetView(back.Get(),nullptr,rtv.GetAddressOf()),"CreateRenderTargetView",error))return false;
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        return checked(device->CreateTexture2D(&desc,nullptr,depth.GetAddressOf()),"Create depth texture",error) &&
            checked(device->CreateDepthStencilView(depth.Get(),nullptr,dsv.GetAddressOf()),"Create depth view",error);
    }
    bool buffer(ComPtr<ID3D11Buffer>& out,unsigned& capacity,size_t bytes,unsigned bind,std::string& error){
        if(bytes>64u*1024*1024){error="Diagnostic draw buffer exceeds 64 MiB";return false;}
        if(bytes<=capacity)return true;
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=unsigned(std::max(bytes,size_t(4096)));
        desc.Usage=D3D11_USAGE_DYNAMIC;desc.BindFlags=bind;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> replacement;
        if(!checked(device->CreateBuffer(&desc,nullptr,&replacement),"Create draw buffer",error))return false;
        out=std::move(replacement);capacity=desc.ByteWidth;return true;
    }
    bool upload(ID3D11Buffer* buffer,const void* data,size_t bytes,std::string& error){
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(!checked(context->Map(buffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Map draw buffer",error))return false;
        std::memcpy(mapped.pData,data,bytes);context->Unmap(buffer,0);return true;
    }
};
Device::Device():impl(std::make_unique<Impl>()){}
Device::~Device()=default;
void* Device::native_device()const{return impl->device.Get();}
void* Device::native_context()const{return impl->context.Get();}
bool Device::initialize(void* hwnd,unsigned width,unsigned height,bool warp,std::string& error){
    if(impl->device){error="DX11 device is already initialized";return false;}
    if(!hwnd || !IsWindow(static_cast<HWND>(hwnd)) || !width || !height || width>16384 || height>16384){error="Invalid DX11 window or dimensions";return false;}
    auto next=std::make_unique<Impl>();auto& d=*next;d.w=width;d.h=height;
    const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0};D3D_FEATURE_LEVEL level{};
    if(!checked(D3D11CreateDevice(nullptr,warp?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,1,
        D3D11_SDK_VERSION,&d.device,&level,&d.context),"D3D11CreateDevice",error))return false;
    ComPtr<IDXGIDevice1> dxgi;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory2> factory;
    if(!checked(d.device.As(&dxgi),"Query DXGI device",error) || !checked(dxgi->SetMaximumFrameLatency(1),"SetMaximumFrameLatency",error) ||
       !checked(dxgi->GetAdapter(&adapter),"GetAdapter",error) || !checked(adapter->GetParent(IID_PPV_ARGS(&factory)),"Get DXGI factory",error))return false;
    DXGI_ADAPTER_DESC ad{};if(!checked(adapter->GetDesc(&ad),"Get adapter description",error))return false;
    char name[256]{};WideCharToMultiByte(CP_UTF8,0,ad.Description,-1,name,sizeof(name),nullptr,nullptr);
    d.description=std::string(warp?"Direct3D 11 WARP: ":"Direct3D 11 hardware: ")+name+" (feature level 11.0)";
    DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=width;sd.Height=height;sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;
    sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    if(!checked(factory->CreateSwapChainForHwnd(d.device.Get(),static_cast<HWND>(hwnd),&sd,nullptr,nullptr,&d.swap),"CreateSwapChainForHwnd",error) ||
       !checked(factory->MakeWindowAssociation(static_cast<HWND>(hwnd),DXGI_MWA_NO_ALT_ENTER),"MakeWindowAssociation",error) || !d.targets(error))return false;
    ComPtr<ID3DBlob> vertex,pixel,errors;
    auto compile=[&](const char* entry,const char* profile,ComPtr<ID3DBlob>& blob){
        const auto hr=D3DCompile(shader,sizeof(shader)-1,"ssne-d3d11-foundation",nullptr,nullptr,entry,profile,
            D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors);
        if(checked(hr,"D3DCompile",error))return true;
        if(errors)error.append(": ").append(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize());return false;
    };
    if(!compile("vs","vs_5_0",vertex) || !compile("ps","ps_5_0",pixel))return false;
    if(!checked(d.device->CreateVertexShader(vertex->GetBufferPointer(),vertex->GetBufferSize(),nullptr,&d.vs),"CreateVertexShader",error) ||
       !checked(d.device->CreatePixelShader(pixel->GetBufferPointer(),pixel->GetBufferSize(),nullptr,&d.ps),"CreatePixelShader",error))return false;
    const D3D11_INPUT_ELEMENT_DESC elements[]={
        {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,20,D3D11_INPUT_PER_VERTEX_DATA,0}};
    static_assert(sizeof(Vertex)==28);
    if(!checked(d.device->CreateInputLayout(elements,3,vertex->GetBufferPointer(),vertex->GetBufferSize(),&d.layout),"CreateInputLayout",error))return false;
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    if(!checked(d.device->CreateRasterizerState(&raster,&d.raster),"CreateRasterizerState",error))return false;
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
    if(!checked(d.device->CreateSamplerState(&sampler,&d.sampler),"CreateSamplerState",error))return false;
    for(unsigned i=0;i<2;++i){
        D3D11_BLEND_DESC blend{};auto& rt=blend.RenderTarget[0];rt.BlendEnable=i;
        rt.SrcBlend=D3D11_BLEND_SRC_ALPHA;rt.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;rt.BlendOp=D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthEnable=i;depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;depth.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
        if(!checked(d.device->CreateBlendState(&blend,&d.blends[i]),"CreateBlendState",error) ||
           !checked(d.device->CreateDepthStencilState(&depth,&d.depths[i]),"CreateDepthStencilState",error))return false;
    }
    impl=std::move(next);const uint8_t white[]={255,255,255,255};return texture(1,1,white,error);
}
bool Device::resize(unsigned width,unsigned height,std::string& error){
    auto& d=*impl;if(!d.device){error="DX11 is not initialized";return false;}
    d.frame=false;if(!width || !height){d.suspended=true;return true;}
    if(width>16384 || height>16384){error="DX11 dimensions exceed 16384";return false;}
    if(width==d.w && height==d.h && d.rtv && d.dsv){d.suspended=false;return true;}
    d.suspended=true;d.context->ClearState();d.rtv.Reset();d.dsv.Reset();d.back.Reset();d.depth.Reset();d.staging.Reset();d.context->Flush();
    if(!checked(d.swap->ResizeBuffers(0,width,height,DXGI_FORMAT_UNKNOWN,0),"ResizeBuffers",error))return false;
    d.w=width;d.h=height;if(!d.targets(error))return false;d.suspended=false;return true;
}
bool Device::texture(unsigned width,unsigned height,std::span<const uint8_t> rgba,std::string& error){
    auto& d=*impl;
    if(!d.device || !width || !height || width>16384 || height>16384 || uint64_t(width)*height*4!=rgba.size()){error="Invalid RGBA texture";return false;}
    D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};data.pSysMem=rgba.data();data.SysMemPitch=width*4;
    ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> view;
    if(!checked(d.device->CreateTexture2D(&desc,&data,&texture),"Create RGBA texture",error) ||
       !checked(d.device->CreateShaderResourceView(texture.Get(),nullptr,&view),"Create texture view",error))return false;
    d.image=std::move(view);return true;
}
bool Device::begin(const float clear[4],std::string& error){
    auto& d=*impl;if(!d.rtv || !d.dsv || d.suspended){error="DX11 target is unavailable/minimized";return false;}
    d.context->OMSetRenderTargets(1,d.rtv.GetAddressOf(),d.dsv.Get());
    d.context->ClearRenderTargetView(d.rtv.Get(),clear);d.context->ClearDepthStencilView(d.dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
    D3D11_VIEWPORT viewport{0,0,float(d.w),float(d.h),0,1};d.context->RSSetViewports(1,&viewport);d.context->RSSetState(d.raster.Get());
    d.context->IASetInputLayout(d.layout.Get());d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.context->VSSetShader(d.vs.Get(),nullptr,0);d.context->PSSetShader(d.ps.Get(),nullptr,0);d.context->PSSetSamplers(0,1,d.sampler.GetAddressOf());
    d.frame=true;return true;
}
bool Device::draw(std::span<const Vertex> vertices,std::span<const uint32_t> indices,bool blend,bool depth,std::string& error){
    auto& d=*impl;if(!d.frame || indices.empty() || indices.size()%3 || vertices.empty()){error="Invalid DX11 draw/frame";return false;}
    for(auto i:indices)if(i>=vertices.size()){error="DX11 index outside vertex range";return false;}
    if(!d.buffer(d.vertices,d.vertex_capacity,vertices.size_bytes(),D3D11_BIND_VERTEX_BUFFER,error) ||
       !d.buffer(d.indices,d.index_capacity,indices.size_bytes(),D3D11_BIND_INDEX_BUFFER,error) ||
       !d.upload(d.vertices.Get(),vertices.data(),vertices.size_bytes(),error) || !d.upload(d.indices.Get(),indices.data(),indices.size_bytes(),error))return false;
    const UINT stride=sizeof(Vertex),offset=0;d.context->IASetVertexBuffers(0,1,d.vertices.GetAddressOf(),&stride,&offset);
    d.context->IASetIndexBuffer(d.indices.Get(),DXGI_FORMAT_R32_UINT,0);
    d.context->OMSetBlendState(d.blends[blend].Get(),nullptr,~0u);d.context->OMSetDepthStencilState(d.depths[depth].Get(),0);
    d.context->PSSetShaderResources(0,1,d.image.GetAddressOf());d.context->DrawIndexed(UINT(indices.size()),0,0);return true;
}
bool Device::readback(std::vector<uint8_t>& rgba,std::string& error){
    auto& d=*impl;if(!d.frame){error="Readback requires a rendered frame before Present";return false;}
    if(!d.staging){D3D11_TEXTURE2D_DESC desc{};d.back->GetDesc(&desc);desc.BindFlags=0;desc.MiscFlags=0;
        desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(!checked(d.device->CreateTexture2D(&desc,nullptr,&d.staging),"Create readback texture",error))return false;}
    rgba.resize(size_t(d.w)*d.h*4);
    d.context->CopyResource(d.staging.Get(),d.back.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
    if(!checked(d.context->Map(d.staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Map readback",error))return false;
    for(unsigned y=0;y<d.h;++y)std::memcpy(rgba.data()+size_t(y)*d.w*4,static_cast<uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,size_t(d.w)*4);
    d.context->Unmap(d.staging.Get(),0);return true;
}
bool Device::present(bool vsync,std::string& error){
    auto& d=*impl;if(d.suspended)return true;if(!d.frame){error="Present requires begin/draw";return false;}
    d.frame=false;const auto hr=d.swap->Present(vsync?1:0,0);
    if(checked(hr,"Present",error))return true;
    std::string reason;if(!checked(d.device->GetDeviceRemovedReason(),"Device removal reason",reason))error+="; "+reason;return false;
}
const std::string& Device::info()const{return impl->description;}
unsigned Device::width()const{return impl->w;}
unsigned Device::height()const{return impl->h;}
}
