// D3D11 implementation of the small command API used by the GX executor.
// This deliberately shares TEV generation/copy rules with OpenGL; all device
// objects, draw calls and presentation on this path are native D3D11.
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "gx_gl.h"
#include "d3d11_shader.h"
#include "ssne/d3d11_device.h"
#include "ssne/d3d11_gx.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <filesystem>
namespace ssne::d3d11 {
using Microsoft::WRL::ComPtr;
using namespace ssne::gl;
namespace {
void check(HRESULT h,const char* op){if(FAILED(h)){std::ostringstream s;s<<"DX11 "<<op<<": 0x"<<std::hex<<uint32_t(h);throw std::runtime_error(s.str());}}
struct Sampling { int min=NEAREST,mag=NEAREST,s=CLAMP_TO_EDGE,t=CLAMP_TO_EDGE,max=1000,aniso=1;ID3D11SamplerState* native=nullptr; };
struct Texture {
    ComPtr<ID3D11Texture2D> resource;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11DepthStencilView> depth;
    ComPtr<ID3D11Texture2D> resolved;
    ComPtr<ID3D11ShaderResourceView> resolved_view;
    unsigned w=0,h=0,levels=1,samples=1;
    DXGI_FORMAT format=DXGI_FORMAT_R8G8B8A8_UNORM;
    Sampling sampling;
};
struct Framebuffer { unsigned color[2]{},depth=0; int count=1; };
struct Buffer { ComPtr<ID3D11Buffer> resource; unsigned size=0; };
struct Attribute { unsigned buffer=0,offset=0,stride=0; int components=4; unsigned type=FLOAT; bool normalized=false,enabled=false; };
struct Array { Attribute attrs[16];unsigned indices=0; };
struct Shader { unsigned type=0;std::string source,log;bool compiled=false; };
struct Uniform { unsigned offset=0,bytes=0,stride=0,count=1; };
struct Constants {
    ComPtr<ID3D11Buffer> buffer;
    std::vector<uint8_t> data;
    std::map<std::string,Uniform> fields;
    unsigned slot=0;bool dirty=true;
};
struct Stage {
    ComPtr<ID3DBlob> code;
    std::vector<Constants> constants;
    struct Sampler { std::string name;unsigned slot=0,count=1,element=0; };
    std::vector<Sampler> samplers;
};
struct Program {
    Shader vertex,fragment;
    Stage vsdata,psdata;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    std::map<std::array<unsigned,16>,ComPtr<ID3D11InputLayout>> layouts;
    std::vector<unsigned> inputs;
    std::vector<std::string> locations;
    std::map<std::string,std::vector<int>> units;
    std::string log;bool linked=false;
};
struct TimerQuery {ComPtr<ID3D11Query> disjoint,start,end;uint64_t result=0;bool complete=false;};
template<class T> using Cache=std::map<std::string,ComPtr<T>>;
struct State {
    ID3D11Device* device=nullptr;ID3D11DeviceContext* context=nullptr;
    ComPtr<ID3D11Device1> device1;
    unsigned next=1,unit=0,read=0,draw=0,program=0,array=0,vertex=0;
    unsigned textures_bound[16]{},samplers_bound[16]{};
    std::unordered_map<unsigned,Texture> textures;
    std::unordered_map<unsigned,Framebuffer> fbos;
    std::unordered_map<unsigned,Buffer> buffers;
    std::unordered_map<unsigned,Array> arrays;
    std::unordered_map<unsigned,Sampling> samplers;
    std::unordered_map<unsigned,Shader> shaders;
    std::unordered_map<unsigned,Program> programs;
    std::unordered_map<unsigned,TimerQuery> timers;unsigned active_timer=0;
    Cache<ID3D11BlendState1> blends;Cache<ID3D11DepthStencilState> depths;
    Cache<ID3D11RasterizerState> rasters;Cache<ID3D11SamplerState> sampler_states;
    bool depth_test=false,depth_write=true,blend=false,logic=false,cull=false,scissor=false,clamp=false;
    unsigned depth_func=LESS,cull_face=BACK,front=CCW,logic_op=COPY;
    unsigned sr=ONE,dr=ZERO,sa=ONE,da=ZERO,eqrgb=FUNC_ADD,eqalpha=FUNC_ADD;
    unsigned color_mask=15;
    D3D11_VIEWPORT viewport{0,0,640,528,0,1};D3D11_RECT rect{0,0,640,528};
    float clear[4]{},clear_depth=1;
    int unpack_row=0;
    std::string description;
    ComPtr<ID3D11Buffer> zero_attribute;
    ComPtr<ID3D11VertexShader> fullscreen_vs;
    ComPtr<ID3D11PixelShader> clear_ps,blit_ps,depth_read_ps;
    ComPtr<ID3D11Buffer> utility_cb;
    bool logic_supported=false;
    bool state_dirty=true;unsigned bound_framebuffer=0;
};
std::unique_ptr<State> state;
State& s(){if(!state)throw std::runtime_error("DX11 GX backend is not initialized");return *state;}
template<class T> std::string key(const T& value){return std::string(reinterpret_cast<const char*>(&value),sizeof(value));}
ComPtr<ID3DBlob> compile(const std::string& text,const char* profile,const char* entry="main"){
    // Many TEV programs share exactly the same translated XF vertex shader.
    // Avoid invoking D3DCompiler repeatedly while warming a new disk cache.
    static std::unordered_map<std::string,ComPtr<ID3DBlob>> compiled;
    const std::string id=std::string(profile)+entry+text;
    if(auto it=compiled.find(id);it!=compiled.end())return it->second;
    ComPtr<ID3DBlob> code,errors;
    auto hr=D3DCompile(text.data(),text.size(),"GX",nullptr,nullptr,entry,profile,
        D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if(FAILED(hr))throw std::runtime_error(errors?std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):"D3DCompile failed");
    if(compiled.size()<256)compiled.emplace(id,code);
    return code;
}
Stage reflect(ComPtr<ID3DBlob> code){
    Stage out;out.code=code;ComPtr<ID3D11ShaderReflection> r;
    check(D3DReflect(code->GetBufferPointer(),code->GetBufferSize(),IID_ID3D11ShaderReflection,reinterpret_cast<void**>(r.GetAddressOf())),"reflect shader");
    D3D11_SHADER_DESC desc{};check(r->GetDesc(&desc),"shader description");
    for(unsigned i=0;i<desc.BoundResources;++i){
        D3D11_SHADER_INPUT_BIND_DESC binding{};r->GetResourceBindingDesc(i,&binding);
        if(getenv("SSNE_DX11_REFLECT"))fprintf(stdout,"[dx-reflect] %s type=%u slot=%u count=%u\n",binding.Name,binding.Type,binding.BindPoint,binding.BindCount);
        if(binding.Type==D3D_SIT_TEXTURE){
            std::string name=binding.Name;unsigned element=0;auto bracket=name.find('[');
            if(bracket!=std::string::npos){element=unsigned(std::stoul(name.substr(bracket+1)));name.resize(bracket);}
            out.samplers.push_back({name,binding.BindPoint,binding.BindCount,element});
        }
        if(binding.Type!=D3D_SIT_CBUFFER)continue;
        auto* cb=r->GetConstantBufferByName(binding.Name);D3D11_SHADER_BUFFER_DESC bd{};cb->GetDesc(&bd);
        Constants c;c.slot=binding.BindPoint;c.data.resize((bd.Size+15)&~15u);
        for(unsigned j=0;j<bd.Variables;++j){
            auto* v=cb->GetVariableByIndex(j);D3D11_SHADER_VARIABLE_DESC vd{};v->GetDesc(&vd);
            D3D11_SHADER_TYPE_DESC td{};v->GetType()->GetDesc(&td);
            c.fields[vd.Name]={vd.StartOffset,vd.Size,td.Elements?16u:td.Columns*4u,std::max(1u,td.Elements)};
        }
        D3D11_BUFFER_DESC b{};b.ByteWidth=unsigned(c.data.size());b.Usage=D3D11_USAGE_DEFAULT;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        check(s().device->CreateBuffer(&b,nullptr,&c.buffer),"uniform buffer");out.constants.push_back(std::move(c));
    }
    return out;
}
void parameter(Sampling& p,unsigned n,int v){p.native=nullptr;switch(n){case TEXTURE_MIN_FILTER:p.min=v;break;case TEXTURE_MAG_FILTER:p.mag=v;break;case TEXTURE_WRAP_S:p.s=v;break;case TEXTURE_WRAP_T:p.t=v;break;case TEXTURE_MAX_LEVEL:p.max=v;break;case TEXTURE_MAX_ANISOTROPY:p.aniso=v;break;default:throw std::runtime_error("DX11 unsupported texture parameter");}}
unsigned address(int mode){switch(mode){case REPEAT:return D3D11_TEXTURE_ADDRESS_WRAP;case MIRRORED_REPEAT:return D3D11_TEXTURE_ADDRESS_MIRROR;default:return D3D11_TEXTURE_ADDRESS_CLAMP;}}
ID3D11SamplerState* sampler(Sampling& p){
    if(p.native)return p.native;
    D3D11_SAMPLER_DESC d{};
    bool min=p.min==LINEAR||p.min==LINEAR_MIPMAP_NEAREST||p.min==LINEAR_MIPMAP_LINEAR;
    bool mip=p.min==NEAREST_MIPMAP_LINEAR||p.min==LINEAR_MIPMAP_LINEAR;
    d.Filter=p.aniso>1?D3D11_FILTER_ANISOTROPIC:D3D11_FILTER((min?0x10:0)|(p.mag==LINEAR?4:0)|(mip?1:0));
    d.AddressU=D3D11_TEXTURE_ADDRESS_MODE(address(p.s));d.AddressV=D3D11_TEXTURE_ADDRESS_MODE(address(p.t));d.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxAnisotropy=std::clamp(p.aniso,1,16);d.ComparisonFunc=D3D11_COMPARISON_NEVER;
    d.MaxLOD=(p.min==LINEAR||p.min==NEAREST)?0.f:float(p.max);
    auto& v=s().sampler_states[key(d)];if(!v)check(s().device->CreateSamplerState(&d,&v),"sampler state");p.native=v.Get();return p.native;
}
void allocate(Texture& t,unsigned w,unsigned h,unsigned samples,unsigned format){
    if(!w||!h||w>16384||h>16384)throw std::runtime_error("DX11 invalid texture dimensions");
    t.resource.Reset();t.view.Reset();t.target.Reset();t.depth.Reset();t.resolved.Reset();t.resolved_view.Reset();t.w=w;t.h=h;t.samples=samples;
    bool depth=format==DEPTH24_STENCIL8||format==DEPTH_COMPONENT24;
    t.format=depth?DXGI_FORMAT_R24G8_TYPELESS:DXGI_FORMAT_R8G8B8A8_UNORM;
    t.levels=1;if(!depth&&samples==1){for(unsigned n=std::max(w,h);n>1;n>>=1)++t.levels;}
    D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=t.levels;d.ArraySize=1;d.Format=t.format;d.SampleDesc.Count=samples;
    d.BindFlags=D3D11_BIND_SHADER_RESOURCE|(depth?D3D11_BIND_DEPTH_STENCIL:D3D11_BIND_RENDER_TARGET);
    if(!depth&&samples==1)d.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;
    check(s().device->CreateTexture2D(&d,nullptr,&t.resource),"GX texture");
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=depth?DXGI_FORMAT_R24_UNORM_X8_TYPELESS:t.format;
    srv.ViewDimension=samples>1?D3D11_SRV_DIMENSION_TEXTURE2DMS:D3D11_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=std::min(t.levels,unsigned(std::max(0,t.sampling.max))+1);
    check(s().device->CreateShaderResourceView(t.resource.Get(),&srv,&t.view),"GX texture view");
    if(depth){D3D11_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;dv.ViewDimension=samples>1?D3D11_DSV_DIMENSION_TEXTURE2DMS:D3D11_DSV_DIMENSION_TEXTURE2D;
        check(s().device->CreateDepthStencilView(t.resource.Get(),&dv,&t.depth),"GX depth view");
    }else check(s().device->CreateRenderTargetView(t.resource.Get(),nullptr,&t.target),"GX render target");
    s().bound_framebuffer=0;
}
void unbind_textures(){ID3D11ShaderResourceView* empty[16]{};s().context->PSSetShaderResources(0,16,empty);s().context->VSSetShaderResources(0,16,empty);}
Framebuffer& framebuffer(unsigned id){if(!id)throw std::runtime_error("DX11 GX cannot draw to default framebuffer");return s().fbos.at(id);}
void targets(){
    if(s().bound_framebuffer==s().draw && s().draw)return;
    unbind_textures();auto& f=framebuffer(s().draw);ID3D11RenderTargetView* v[2]{};
    for(int i=0;i<f.count;++i)if(f.color[i])v[i]=s().textures.at(f.color[i]).target.Get();
    auto* depth=f.depth?s().textures.at(f.depth).depth.Get():nullptr;
    s().context->OMSetRenderTargets(f.count,v,depth);
    s().bound_framebuffer=s().draw;
}
D3D11_BLEND factor(unsigned f){switch(f){
    case ZERO:return D3D11_BLEND_ZERO;case ONE:return D3D11_BLEND_ONE;
    case SRC_COLOR:return D3D11_BLEND_SRC_COLOR;case ONE_MINUS_SRC_COLOR:return D3D11_BLEND_INV_SRC_COLOR;
    case DST_COLOR:return D3D11_BLEND_DEST_COLOR;case ONE_MINUS_DST_COLOR:return D3D11_BLEND_INV_DEST_COLOR;
    case SRC_ALPHA:return D3D11_BLEND_SRC_ALPHA;case ONE_MINUS_SRC_ALPHA:return D3D11_BLEND_INV_SRC_ALPHA;
    case DST_ALPHA:return D3D11_BLEND_DEST_ALPHA;case ONE_MINUS_DST_ALPHA:return D3D11_BLEND_INV_DEST_ALPHA;
    case SRC1_ALPHA:return D3D11_BLEND_SRC1_ALPHA;case ONE_MINUS_SRC1_ALPHA:return D3D11_BLEND_INV_SRC1_ALPHA;
    default:throw std::runtime_error("DX11 unknown blend factor");}}
D3D11_LOGIC_OP logic(unsigned op){static const D3D11_LOGIC_OP table[]={D3D11_LOGIC_OP_CLEAR,D3D11_LOGIC_OP_AND,D3D11_LOGIC_OP_AND_REVERSE,D3D11_LOGIC_OP_COPY,D3D11_LOGIC_OP_AND_INVERTED,D3D11_LOGIC_OP_NOOP,D3D11_LOGIC_OP_XOR,D3D11_LOGIC_OP_OR,D3D11_LOGIC_OP_NOR,D3D11_LOGIC_OP_EQUIV,D3D11_LOGIC_OP_INVERT,D3D11_LOGIC_OP_OR_REVERSE,D3D11_LOGIC_OP_COPY_INVERTED,D3D11_LOGIC_OP_OR_INVERTED,D3D11_LOGIC_OP_NAND,D3D11_LOGIC_OP_SET};if(op<CLEAR||op>SET)throw std::runtime_error("DX11 unknown logic op");return table[op-CLEAR];}
void render_state(bool utility=false,bool clear_depth=false,unsigned clear_mask=15){
    if(!utility&&!s().state_dirty)return;
    s().state_dirty=utility;
    auto& a=s();D3D11_RASTERIZER_DESC r{};r.FillMode=D3D11_FILL_SOLID;
    r.CullMode=utility||!a.cull?D3D11_CULL_NONE:a.cull_face==FRONT?D3D11_CULL_FRONT:D3D11_CULL_BACK;
    // flip_vert_y reverses the winding in D3D's render-target coordinates.
    r.FrontCounterClockwise=a.front==CW;r.DepthClipEnable=utility||!a.clamp;r.ScissorEnable=a.scissor;r.MultisampleEnable=true;
    auto& rs=a.rasters[key(r)];if(!rs)check(a.device->CreateRasterizerState(&r,&rs),"rasterizer state");a.context->RSSetState(rs.Get());
    a.context->RSSetViewports(1,&a.viewport);a.context->RSSetScissorRects(1,&a.rect);
    D3D11_DEPTH_STENCIL_DESC d{};d.DepthEnable=utility?clear_depth:a.depth_test;d.DepthWriteMask=(utility?clear_depth:a.depth_write)?D3D11_DEPTH_WRITE_MASK_ALL:D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc=utility?D3D11_COMPARISON_ALWAYS:D3D11_COMPARISON_FUNC(a.depth_func-NEVER+1);
    auto& ds=a.depths[key(d)];if(!ds)check(a.device->CreateDepthStencilState(&d,&ds),"depth state");a.context->OMSetDepthStencilState(ds.Get(),0);
    D3D11_BLEND_DESC1 b{};auto& rt=b.RenderTarget[0];
    rt.RenderTargetWriteMask=uint8_t(utility?clear_mask:a.color_mask);rt.BlendEnable=!utility&&a.blend;
    rt.LogicOpEnable=!utility&&a.logic;rt.LogicOp=logic(a.logic_op);
    if(rt.LogicOpEnable&&!a.logic_supported)throw std::runtime_error("DX11 adapter does not support GX logic operations");
    rt.SrcBlend=factor(a.sr);rt.DestBlend=factor(a.dr);rt.SrcBlendAlpha=factor(a.sa);rt.DestBlendAlpha=factor(a.da);
    rt.BlendOp=a.eqrgb==FUNC_REVERSE_SUBTRACT?D3D11_BLEND_OP_REV_SUBTRACT:D3D11_BLEND_OP_ADD;
    rt.BlendOpAlpha=a.eqalpha==FUNC_REVERSE_SUBTRACT?D3D11_BLEND_OP_REV_SUBTRACT:D3D11_BLEND_OP_ADD;
    auto& bs=a.blends[key(b)];if(!bs)check(a.device1->CreateBlendState1(&b,&bs),"blend state");a.context->OMSetBlendState(bs.Get(),nullptr,~0u);
}
void uniforms(Stage& stage,bool vertex){
    for(auto& c:stage.constants){if(c.dirty){s().context->UpdateSubresource(c.buffer.Get(),0,nullptr,c.data.data(),0,0);c.dirty=false;}
        auto* b=c.buffer.Get();if(vertex)s().context->VSSetConstantBuffers(c.slot,1,&b);else s().context->PSSetConstantBuffers(c.slot,1,&b);}
}
void bind_program(){
    auto& a=s();auto& p=a.programs.at(a.program);if(!p.linked)throw std::runtime_error("DX11 invalid GX shader program: "+p.log);
    a.context->VSSetShader(p.vs.Get(),nullptr,0);a.context->PSSetShader(p.ps.Get(),nullptr,0);
    a.context->GSSetShader(nullptr,nullptr,0);a.context->HSSetShader(nullptr,nullptr,0);a.context->DSSetShader(nullptr,nullptr,0);
    uniforms(p.vsdata,true);uniforms(p.psdata,false);
    auto bind=[&](Stage& st,bool vertex){
      ID3D11ShaderResourceView* views[16]{};ID3D11SamplerState* samples[16]{};unsigned slots=0;
      for(auto& binding:st.samplers){
        auto unit=p.units.find(binding.name);
        for(unsigned j=0;j<binding.count;++j){unsigned element=binding.element+j;int i=unit!=p.units.end()&&element<unit->second.size()?unit->second[element]:0;
            if(i<0||i>=16)throw std::runtime_error("DX11 sampler unit out of range");
            ID3D11ShaderResourceView* srv=nullptr;ID3D11SamplerState* samp=nullptr;
            auto t=a.textures.find(a.textures_bound[i]);if(t!=a.textures.end()){
                srv=t->second.view.Get();samp=sampler(a.samplers_bound[i]?a.samplers.at(a.samplers_bound[i]):t->second.sampling);
            }
            const unsigned slot=binding.slot+j;if(slot>=16)throw std::runtime_error("DX11 sampler binding out of range");views[slot]=srv;samples[slot]=samp;slots=std::max(slots,slot+1);
        }
      }
      if(slots){if(vertex){a.context->VSSetShaderResources(0,slots,views);a.context->VSSetSamplers(0,slots,samples);}
        else{a.context->PSSetShaderResources(0,slots,views);a.context->PSSetSamplers(0,slots,samples);}}
    };bind(p.vsdata,true);bind(p.psdata,false);
    std::array<D3D11_INPUT_ELEMENT_DESC,16> descs{};unsigned desc_count=0;std::array<unsigned,16> layoutkey{};
    ID3D11Buffer* buffers[16]{};unsigned strides[16]{},offsets[16]{},buffer_count=0;
    auto& va=a.arrays[a.array];
    for(auto i:p.inputs){if(i>=16)throw std::runtime_error("DX11 shader input location out of range");auto& v=va.attrs[i];
        DXGI_FORMAT fmt=DXGI_FORMAT_R32G32B32A32_FLOAT;
        if(v.enabled){if(v.type==UNSIGNED_BYTE&&v.normalized&&v.components==4)fmt=DXGI_FORMAT_R8G8B8A8_UNORM;
            else if(v.type==FLOAT){static DXGI_FORMAT f[]={DXGI_FORMAT_UNKNOWN,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32G32_FLOAT,DXGI_FORMAT_R32G32B32_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT};fmt=f[std::clamp(v.components,1,4)];}
            else throw std::runtime_error("DX11 unsupported vertex attribute");}
        descs[desc_count++]={"TEXCOORD",i,fmt,i,0,D3D11_INPUT_PER_VERTEX_DATA,0};layoutkey[i]=fmt;
        buffers[i]=v.enabled?a.buffers.at(v.buffer).resource.Get():a.zero_attribute.Get();strides[i]=v.enabled?v.stride:0;offsets[i]=v.enabled?v.offset:0;buffer_count=std::max(buffer_count,i+1);
    }
    if(buffer_count)a.context->IASetVertexBuffers(0,buffer_count,buffers,strides,offsets);
    auto& layout=p.layouts[layoutkey];if(!layout&&desc_count)check(a.device->CreateInputLayout(descs.data(),desc_count,p.vsdata.code->GetBufferPointer(),p.vsdata.code->GetBufferSize(),&layout),"GX input layout");
    a.context->IASetInputLayout(layout.Get());a.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}
void utility(const float* constants,ID3D11PixelShader* ps, bool depth,unsigned mask){
    auto& a=s();targets();render_state(true,depth,mask);
    a.context->UpdateSubresource(a.utility_cb.Get(),0,nullptr,constants,0,0);auto* cb=a.utility_cb.Get();
    a.context->VSSetConstantBuffers(0,1,&cb);a.context->PSSetConstantBuffers(0,1,&cb);
    a.context->VSSetShader(a.fullscreen_vs.Get(),nullptr,0);a.context->PSSetShader(ps,nullptr,0);
    a.context->GSSetShader(nullptr,nullptr,0);a.context->IASetInputLayout(nullptr);a.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}
// Entry points intentionally match the GX command API, not a general GL driver.
const GLubyte* SSNE_GLAPI GetString_(GLenum name){static const std::string version="Direct3D 11 / GX";static const std::string vendor="D3D11";return reinterpret_cast<const GLubyte*>((name==VERSION?version:name==VENDOR?vendor:s().description).c_str());}
GLenum SSNE_GLAPI GetError_(){return 0;}
void cap(unsigned c,bool on){auto& a=s();a.state_dirty=true;switch(c){case DEPTH_TEST:a.depth_test=on;break;case BLEND:a.blend=on;break;case COLOR_LOGIC_OP:a.logic=on;break;case CULL_FACE:a.cull=on;break;case SCISSOR_TEST:a.scissor=on;break;case DEPTH_CLAMP:a.clamp=on;break;case MULTISAMPLE:break;
    // GX shaders always emit the same three distances. The executor keeps all
    // three enabled for GX geometry; fullscreen utility shaders emit none.
    case CLIP_DISTANCE0:case CLIP_DISTANCE1:case CLIP_DISTANCE1+1:break;
    default:throw std::runtime_error("DX11 unsupported GX capability "+std::to_string(c));}}
void SSNE_GLAPI Enable_(GLenum c){cap(c,true);}void SSNE_GLAPI Disable_(GLenum c){cap(c,false);}
void SSNE_GLAPI DepthRange_(GLdouble n,GLdouble f){
    s().viewport.MinDepth=std::clamp(float(n),0.f,1.f);s().viewport.MaxDepth=std::clamp(float(f),0.f,1.f);s().state_dirty=true;
}
void SSNE_GLAPI Viewport_(GLint x,GLint y,GLsizei w,GLsizei h){auto& v=s().viewport;v={float(x),float(y),float(w),float(h),v.MinDepth,v.MaxDepth};s().state_dirty=true;}
void SSNE_GLAPI Scissor_(GLint x,GLint y,GLsizei w,GLsizei h){s().rect={x,y,x+w,y+h};s().state_dirty=true;}
void SSNE_GLAPI ClearColor_(float r,float g,float b,float a){s().clear[0]=r;s().clear[1]=g;s().clear[2]=b;s().clear[3]=a;}
void SSNE_GLAPI ClearDepth_(double d){s().clear_depth=float(d);}
void SSNE_GLAPI Clear_(GLbitfield mask){
    auto& a=s();auto saved=a.viewport;auto& f=framebuffer(a.draw);auto& t=a.textures.at(f.color[0]?f.color[0]:f.depth);
    a.viewport={0,0,float(t.w),float(t.h),0,1};float c[12]{};std::copy(a.clear,a.clear+4,c);c[4]=a.clear_depth;
    utility(c,a.clear_ps.Get(),(mask&DEPTH_BUFFER_BIT)&&a.depth_write,(mask&COLOR_BUFFER_BIT)?a.color_mask:0);a.context->Draw(3,0);a.viewport=saved;
}
void SSNE_GLAPI ColorMask_(GLboolean r,GLboolean g,GLboolean b,GLboolean a){s().color_mask=(r?1:0)|(g?2:0)|(b?4:0)|(a?8:0);s().state_dirty=true;}
void SSNE_GLAPI DepthMask_(GLboolean v){s().depth_write=v;s().state_dirty=true;}void SSNE_GLAPI DepthFunc_(GLenum v){s().depth_func=v;s().state_dirty=true;}
void SSNE_GLAPI BlendFuncSeparate_(GLenum sr,GLenum dr,GLenum sa,GLenum da){s().sr=sr;s().dr=dr;s().sa=sa;s().da=da;s().state_dirty=true;}
void SSNE_GLAPI BlendEquationSeparate_(GLenum r,GLenum a){s().eqrgb=r;s().eqalpha=a;s().state_dirty=true;}
void SSNE_GLAPI LogicOp_(GLenum v){s().logic_op=v;s().state_dirty=true;}void SSNE_GLAPI CullFace_(GLenum v){s().cull_face=v;s().state_dirty=true;}void SSNE_GLAPI FrontFace_(GLenum v){s().front=v;s().state_dirty=true;}
void SSNE_GLAPI GenTextures_(GLsizei n,GLuint* out){for(int i=0;i<n;++i){out[i]=s().next++;s().textures.try_emplace(out[i]);}}
void SSNE_GLAPI DeleteTextures_(GLsizei n,const GLuint* ids){unbind_textures();s().bound_framebuffer=0;for(int i=0;i<n;++i){s().textures.erase(ids[i]);for(auto& t:s().textures_bound)if(t==ids[i])t=0;}}
void SSNE_GLAPI BindTexture_(GLenum,GLuint id){s().textures_bound[s().unit]=id;}
void SSNE_GLAPI ActiveTexture_(GLenum unit){if(unit<TEXTURE0||unit>=TEXTURE0+16)throw std::runtime_error("DX11 texture unit out of range");s().unit=unit-TEXTURE0;}
void SSNE_GLAPI TexImage2D_(GLenum,GLint level,GLint fmt,GLsizei w,GLsizei h,GLint,GLenum,GLenum type,const void* data){
    auto& t=s().textures.at(s().textures_bound[s().unit]);if(!level)allocate(t,w,h,1,fmt);
    if(level<0||unsigned(level)>=t.levels||unsigned(w)!=std::max(1u,t.w>>level)||unsigned(h)!=std::max(1u,t.h>>level))throw std::runtime_error("DX11 invalid texture mip level");
    if(data){if(type!=UNSIGNED_BYTE&&type!=UNSIGNED_INT_24_8)throw std::runtime_error("DX11 unsupported texture upload");s().context->UpdateSubresource(t.resource.Get(),level,nullptr,data,4*(s().unpack_row?s().unpack_row:w),0);}
}
void SSNE_GLAPI TexImage2DMultisample_(GLenum,GLsizei samples,GLenum fmt,GLsizei w,GLsizei h,GLboolean){allocate(s().textures.at(s().textures_bound[s().unit]),w,h,samples,fmt);}
void SSNE_GLAPI TexParameteri_(GLenum,GLenum p,GLint v){auto& t=s().textures.at(s().textures_bound[s().unit]);parameter(t.sampling,p,v);
    // GL texture MAX_LEVEL still applies when a separate sampler is bound.
    // Limit the SRV itself so samplers cannot read uninitialized mip levels.
    if(p==TEXTURE_MAX_LEVEL&&t.resource&&t.samples==1){D3D11_SHADER_RESOURCE_VIEW_DESC d{};t.view->GetDesc(&d);d.Texture2D.MipLevels=std::min(t.levels,unsigned(std::max(0,v))+1);
        ComPtr<ID3D11ShaderResourceView> replacement;check(s().device->CreateShaderResourceView(t.resource.Get(),&d,&replacement),"texture mip range");t.view=std::move(replacement);}
}
void SSNE_GLAPI PixelStorei_(GLenum p,GLint v){if(p==UNPACK_ROW_LENGTH)s().unpack_row=v;else if((p!=PACK_ALIGNMENT&&p!=UNPACK_ALIGNMENT)||(v!=1&&v!=4))throw std::runtime_error("DX11 unsupported pixel packing");}
void SSNE_GLAPI GenFramebuffers_(GLsizei n,GLuint* ids){for(int i=0;i<n;++i){ids[i]=s().next++;s().fbos.try_emplace(ids[i]);}}
void SSNE_GLAPI DeleteFramebuffers_(GLsizei n,const GLuint* ids){for(int i=0;i<n;++i)s().fbos.erase(ids[i]);}
void SSNE_GLAPI BindFramebuffer_(GLenum target,GLuint id){if(target!=DRAW_FRAMEBUFFER)s().read=id;if(target!=READ_FRAMEBUFFER){s().draw=id;s().bound_framebuffer=0;}}
void SSNE_GLAPI FramebufferTexture2D_(GLenum target,GLenum attachment,GLenum,GLuint texture,GLint level){
    if(level)throw std::runtime_error("DX11 GX framebuffer mip attachment unsupported");auto& f=framebuffer(target==READ_FRAMEBUFFER?s().read:s().draw);
    if(attachment==COLOR_ATTACHMENT0||attachment==COLOR_ATTACHMENT1)f.color[attachment-COLOR_ATTACHMENT0]=texture;else f.depth=texture;
    s().bound_framebuffer=0;
}
GLenum SSNE_GLAPI CheckFramebufferStatus_(GLenum target){auto& f=framebuffer(target==READ_FRAMEBUFFER?s().read:s().draw);unsigned w=0,h=0,samples=0;
    for(unsigned id:{f.color[0],f.color[1],f.depth})if(id){auto& t=s().textures.at(id);if(!t.resource)return 0;if(w&&(w!=t.w||h!=t.h||samples!=t.samples))return 0;w=t.w;h=t.h;samples=t.samples;}return w?FRAMEBUFFER_COMPLETE:0;}
void SSNE_GLAPI DrawBuffers_(GLsizei n,const GLenum* bufs){if(n<1||n>2)throw std::runtime_error("DX11 unsupported MRT count");for(int i=0;i<n;++i)if(bufs[i]!=COLOR_ATTACHMENT0+unsigned(i))throw std::runtime_error("DX11 unsupported MRT mapping");framebuffer(s().draw).count=n;s().bound_framebuffer=0;}
void SSNE_GLAPI GenerateMipmap_(GLenum){auto& t=s().textures.at(s().textures_bound[s().unit]);unbind_textures();s().context->OMSetRenderTargets(0,nullptr,nullptr);s().bound_framebuffer=0;s().context->GenerateMips(t.view.Get());}
void SSNE_GLAPI BlitFramebuffer_(GLint sx0,GLint sy0,GLint sx1,GLint sy1,GLint dx0,GLint dy0,GLint dx1,GLint dy1,GLbitfield mask,GLenum filter){
    if(mask!=COLOR_BUFFER_BIT)throw std::runtime_error("DX11 unsupported framebuffer blit mask");
    auto& a=s();auto& src=a.textures.at(framebuffer(a.read).color[0]);
    ComPtr<ID3D11ShaderResourceView> view=src.view;
    unbind_textures();a.context->OMSetRenderTargets(0,nullptr,nullptr);a.bound_framebuffer=0;
    if(src.samples>1){if(!src.resolved){D3D11_TEXTURE2D_DESC d{};src.resource->GetDesc(&d);d.SampleDesc.Count=1;d.SampleDesc.Quality=0;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.MiscFlags=0;
        check(a.device->CreateTexture2D(&d,nullptr,&src.resolved),"MSAA resolve texture");check(a.device->CreateShaderResourceView(src.resolved.Get(),nullptr,&src.resolved_view),"MSAA resolve view");}
        a.context->ResolveSubresource(src.resolved.Get(),0,src.resource.Get(),0,src.format);view=src.resolved_view;}
    auto vp=a.viewport;a.viewport={float(std::min(dx0,dx1)),float(std::min(dy0,dy1)),float(abs(dx1-dx0)),float(abs(dy1-dy0)),0,1};
    float c[12]{};c[4]=float(sx0)/src.w;c[5]=float(sy0)/src.h;c[6]=float(sx1-sx0)/src.w;c[7]=float(sy1-sy0)/src.h;
    c[8]=float(dx0);c[9]=float(dy0);c[10]=1.f/(dx1-dx0);c[11]=1.f/(dy1-dy0);
    utility(c,a.blit_ps.Get(),false,15);auto* srv=view.Get();a.context->PSSetShaderResources(0,1,&srv);
    Sampling sample;sample.min=sample.mag=filter;auto* sm=sampler(sample);a.context->PSSetSamplers(0,1,&sm);a.context->Draw(3,0);a.viewport=vp;unbind_textures();
}
void SSNE_GLAPI ReadPixels_(GLint x,GLint y,GLsizei w,GLsizei h,GLenum fmt,GLenum type,void* output){
    auto& a=s();auto& f=framebuffer(a.read);auto& t=a.textures.at(fmt==DEPTH_COMPONENT?f.depth:f.color[0]);
    if(x<0||y<0||w<0||h<0||unsigned(x+w)>t.w||unsigned(y+h)>t.h)throw std::runtime_error("DX11 readback rectangle out of range");
    if((fmt!=RGBA||type!=UNSIGNED_BYTE)&&(fmt!=DEPTH_COMPONENT||type!=FLOAT))throw std::runtime_error("DX11 unsupported readback format");
    unbind_textures();a.context->OMSetRenderTargets(0,nullptr,nullptr);a.bound_framebuffer=0;
    D3D11_TEXTURE2D_DESC d{};t.resource->GetDesc(&d);ComPtr<ID3D11Texture2D> resolved;ID3D11Texture2D* source=t.resource.Get();
    bool float_depth=fmt==DEPTH_COMPONENT&&d.SampleDesc.Count>1;
    if(d.SampleDesc.Count>1){d.SampleDesc.Count=1;d.SampleDesc.Quality=0;d.BindFlags=float_depth?D3D11_BIND_RENDER_TARGET:0;d.MiscFlags=0;
        if(float_depth)d.Format=DXGI_FORMAT_R32_FLOAT;
        check(a.device->CreateTexture2D(&d,nullptr,&resolved),"readback resolve");
        if(float_depth){
            // Diagnostic depth dumps use sample zero explicitly (there is no
            // fixed-function D3D11 depth resolve). Never alter the EFB depth.
            if(!a.depth_read_ps){auto code=compile("Texture2DMS<float> source:register(t0);float main(float4 p:SV_Position):SV_Target{return source.Load(int2(p.xy),0);}","ps_5_0");check(a.device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&a.depth_read_ps),"MSAA depth read shader");}
            ComPtr<ID3D11RenderTargetView> target;check(a.device->CreateRenderTargetView(resolved.Get(),nullptr,&target),"depth read target");
            auto vp=a.viewport;bool sc=a.scissor;unsigned draw=a.draw;a.draw=a.read;a.viewport={0,0,float(t.w),float(t.h),0,1};a.scissor=false;
            float constants[12]{};utility(constants,a.depth_read_ps.Get(),false,15);auto* rt=target.Get();a.context->OMSetRenderTargets(1,&rt,nullptr);auto* srv=t.view.Get();a.context->PSSetShaderResources(0,1,&srv);a.context->Draw(3,0);
            unbind_textures();a.context->OMSetRenderTargets(0,nullptr,nullptr);a.bound_framebuffer=0;a.viewport=vp;a.scissor=sc;a.draw=draw;
        }else a.context->ResolveSubresource(resolved.Get(),0,source,0,t.format);
        source=resolved.Get();
    }
    // D3D11 depth/stencil copies must cover the entire subresource, without
    // a source box. Crop the mapped staging image instead of the GPU copy.
    d.MipLevels=1;d.BindFlags=0;d.MiscFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;check(a.device->CreateTexture2D(&d,nullptr,&staging),"readback staging");a.context->CopySubresourceRegion(staging.Get(),0,0,0,0,source,0,nullptr);
    D3D11_MAPPED_SUBRESOURCE map{};check(a.context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"readback map");
    for(int row=0;row<h;++row){auto* from=static_cast<const uint8_t*>(map.pData)+(row+y)*map.RowPitch+x*4;
        if(fmt==RGBA||float_depth)memcpy(static_cast<uint8_t*>(output)+size_t(row)*w*4,from,size_t(w)*4);
        else for(int col=0;col<w;++col){uint32_t raw;memcpy(&raw,from+col*4,4);static_cast<float*>(output)[size_t(row)*w+col]=float(raw&0xffffff)/16777215.f;}}
    a.context->Unmap(staging.Get(),0);
}
GLuint SSNE_GLAPI CreateShader_(GLenum type){auto id=s().next++;s().shaders[id].type=type;return id;}
void SSNE_GLAPI ShaderSource_(GLuint id,GLsizei n,const GLchar*const* text,const GLint* lengths){auto& out=s().shaders.at(id).source;out.clear();for(int i=0;i<n;++i)out.append(text[i],lengths&&lengths[i]>=0?size_t(lengths[i]):strlen(text[i]));}
void SSNE_GLAPI CompileShader_(GLuint id){s().shaders.at(id).compiled=true;/* Pair compilation happens at LinkProgram, to match IO locations. */}
void SSNE_GLAPI GetShaderiv_(GLuint id,GLenum p,GLint* v){auto& sh=s().shaders.at(id);*v=p==COMPILE_STATUS?sh.compiled:int(sh.log.size()+1);}
void copy_log(const std::string& text,int max,int* len,char* output){int n=std::min(int(text.size()),std::max(0,max-1));if(max>0){memcpy(output,text.data(),n);output[n]=0;}if(len)*len=n;}
void SSNE_GLAPI GetShaderInfoLog_(GLuint id,GLsizei max,GLsizei* len,GLchar* out){copy_log(s().shaders.at(id).log,max,len,out);}
GLuint SSNE_GLAPI CreateProgram_(){auto id=s().next++;s().programs.try_emplace(id);return id;}
void SSNE_GLAPI AttachShader_(GLuint p,GLuint sh){auto& shader=s().shaders.at(sh);if(shader.type==VERTEX_SHADER)s().programs.at(p).vertex=shader;else s().programs.at(p).fragment=shader;}
void finish_link(Program& p){
    check(s().device->CreateVertexShader(p.vsdata.code->GetBufferPointer(),p.vsdata.code->GetBufferSize(),nullptr,&p.vs),"GX vertex shader");
    check(s().device->CreatePixelShader(p.psdata.code->GetBufferPointer(),p.psdata.code->GetBufferSize(),nullptr,&p.ps),"GX pixel shader");
    ComPtr<ID3D11ShaderReflection> r;check(D3DReflect(p.vsdata.code->GetBufferPointer(),p.vsdata.code->GetBufferSize(),IID_ID3D11ShaderReflection,reinterpret_cast<void**>(r.GetAddressOf())),"input reflection");
    D3D11_SHADER_DESC d{};r->GetDesc(&d);p.inputs.clear();for(unsigned i=0;i<d.InputParameters;++i){D3D11_SIGNATURE_PARAMETER_DESC input{};r->GetInputParameterDesc(i,&input);if(input.SystemValueType==D3D_NAME_UNDEFINED)p.inputs.push_back(input.SemanticIndex);}
    p.linked=true;
}
void SSNE_GLAPI LinkProgram_(GLuint id){auto& p=s().programs.at(id);try{
    auto translated=translate_gx(p.vertex.source,p.fragment.source);
    if(const char* dir=getenv("SSNE_DX11_DUMP_SHADERS")){std::filesystem::create_directories(dir);const auto base=std::filesystem::path(dir)/std::to_string(id);std::ofstream(base.string()+".vs.hlsl")<<translated.vertex;std::ofstream(base.string()+".ps.hlsl")<<translated.fragment;}
    p.vsdata=reflect(compile(translated.vertex,"vs_5_0"));p.psdata=reflect(compile(translated.fragment,"ps_5_0"));
    finish_link(p);
}catch(const std::exception& e){p.log=e.what();p.linked=false;}}
void SSNE_GLAPI GetProgramiv_(GLuint id,GLenum p,GLint* v){auto& sh=s().programs.at(id);*v=p==LINK_STATUS?sh.linked:p==INFO_LOG_LENGTH?int(sh.log.size()+1):p==PROGRAM_BINARY_LENGTH&&sh.linked?int(8+sh.vsdata.code->GetBufferSize()+sh.psdata.code->GetBufferSize()):0;}
void SSNE_GLAPI ProgramParameteri_(GLuint,GLenum p,GLint){if(p!=PROGRAM_BINARY_RETRIEVABLE_HINT)throw std::runtime_error("DX11 invalid program parameter");}
constexpr unsigned binary_format=0x53534431;
void SSNE_GLAPI GetProgramBinary_(GLuint id,GLsizei max,GLsizei* len,GLenum* format,void* bytes){auto& p=s().programs.at(id);
    uint32_t sizes[2]={uint32_t(p.vsdata.code->GetBufferSize()),uint32_t(p.psdata.code->GetBufferSize())};unsigned total=8+sizes[0]+sizes[1];if(max<0||unsigned(max)<total)throw std::runtime_error("DX11 shader cache buffer too short");
    auto* out=static_cast<uint8_t*>(bytes);memcpy(out,sizes,8);memcpy(out+8,p.vsdata.code->GetBufferPointer(),sizes[0]);memcpy(out+8+sizes[0],p.psdata.code->GetBufferPointer(),sizes[1]);*len=total;*format=binary_format;
}
void SSNE_GLAPI ProgramBinary_(GLuint id,GLenum format,const void* bytes,GLsizei len){auto& p=s().programs.at(id);p.linked=false;try{
    if(format!=binary_format||len<8)throw std::runtime_error("Invalid DX11 shader cache header");uint32_t sizes[2];memcpy(sizes,bytes,8);
    if(!sizes[0]||!sizes[1]||uint64_t(sizes[0])+sizes[1]+8!=unsigned(len))throw std::runtime_error("Invalid DX11 shader cache size");
    auto* in=static_cast<const uint8_t*>(bytes)+8;ComPtr<ID3DBlob> vs,ps;check(D3DCreateBlob(sizes[0],&vs),"cached vertex blob");check(D3DCreateBlob(sizes[1],&ps),"cached pixel blob");
    memcpy(vs->GetBufferPointer(),in,sizes[0]);memcpy(ps->GetBufferPointer(),in+sizes[0],sizes[1]);p.vsdata=reflect(vs);p.psdata=reflect(ps);finish_link(p);
}catch(const std::exception& e){p.log=e.what();}}
void SSNE_GLAPI GetProgramInfoLog_(GLuint id,GLsizei max,GLsizei* len,GLchar* out){copy_log(s().programs.at(id).log,max,len,out);}
void SSNE_GLAPI DeleteShader_(GLuint id){s().shaders.erase(id);}void SSNE_GLAPI DeleteProgram_(GLuint id){s().programs.erase(id);}void SSNE_GLAPI UseProgram_(GLuint id){s().program=id;}
GLint SSNE_GLAPI GetUniformLocation_(GLuint id,const GLchar* name){auto& p=s().programs.at(id);std::string n=name;if(n.ends_with("[0]"))n.resize(n.size()-3);
    for(size_t i=0;i<p.locations.size();++i)if(p.locations[i]==n)return int(i);
    bool found=false;for(auto* stage:{&p.vsdata,&p.psdata}){for(auto& c:stage->constants)found|=c.fields.count(n)!=0;for(auto& b:stage->samplers)found|=b.name==n;}
    if(!found)return -1;p.locations.push_back(n);return int(p.locations.size()-1);
}
void uniform(int loc,int n,const void* value,unsigned components,bool integer){if(loc<0)return;auto& p=s().programs.at(s().program);const auto& name=p.locations.at(loc);
    if(integer&&components==1){auto* v=static_cast<const int*>(value);p.units[name]=std::vector<int>(v,v+n);}
    for(auto* stage:{&p.vsdata,&p.psdata})for(auto& c:stage->constants){auto it=c.fields.find(name);if(it==c.fields.end())continue;auto& f=it->second;
        if(n<0||unsigned(n)>f.count)throw std::runtime_error("DX11 uniform array overflow: "+name);
        for(int i=0;i<n;++i){unsigned offset=f.offset+i*f.stride;unsigned bytes=components*4;if(offset+bytes>c.data.size())throw std::runtime_error("DX11 uniform buffer overflow");memcpy(c.data.data()+offset,static_cast<const uint8_t*>(value)+i*bytes,bytes);}c.dirty=true;}
}
void SSNE_GLAPI Uniform1i_(GLint l,GLint v){uniform(l,1,&v,1,true);}void SSNE_GLAPI Uniform1f_(GLint l,GLfloat v){uniform(l,1,&v,1,false);}
void SSNE_GLAPI Uniform1iv_(GLint l,GLsizei n,const GLint* v){uniform(l,n,v,1,true);}void SSNE_GLAPI Uniform2fv_(GLint l,GLsizei n,const GLfloat* v){uniform(l,n,v,2,false);}
void SSNE_GLAPI Uniform4fv_(GLint l,GLsizei n,const GLfloat* v){uniform(l,n,v,4,false);}void SSNE_GLAPI Uniform4iv_(GLint l,GLsizei n,const GLint* v){uniform(l,n,v,4,true);}
void SSNE_GLAPI GenVertexArrays_(GLsizei n,GLuint* ids){for(int i=0;i<n;++i){ids[i]=s().next++;s().arrays.try_emplace(ids[i]);}}
void SSNE_GLAPI BindVertexArray_(GLuint id){s().array=id;}
void SSNE_GLAPI GenBuffers_(GLsizei n,GLuint* ids){for(int i=0;i<n;++i){ids[i]=s().next++;s().buffers.try_emplace(ids[i]);}}
void SSNE_GLAPI BindBuffer_(GLenum t,GLuint id){if(t==ARRAY_BUFFER)s().vertex=id;else s().arrays[s().array].indices=id;}
void SSNE_GLAPI BufferData_(GLenum t,GLsizeiptr size,const void* data,GLenum){
    if(size<0||size>256*1024*1024)throw std::runtime_error("DX11 GX buffer size out of range");auto& a=s();auto& b=a.buffers.at(t==ARRAY_BUFFER?a.vertex:a.arrays[a.array].indices);if(!size)return;
    if(b.size<unsigned(size)){b.resource.Reset();D3D11_BUFFER_DESC d{};d.ByteWidth=unsigned(size);d.Usage=D3D11_USAGE_DYNAMIC;d.BindFlags=t==ARRAY_BUFFER?D3D11_BIND_VERTEX_BUFFER:D3D11_BIND_INDEX_BUFFER;d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;check(a.device->CreateBuffer(&d,nullptr,&b.resource),"GX vertex/index buffer");b.size=unsigned(size);}
    D3D11_MAPPED_SUBRESOURCE m{};check(a.context->Map(b.resource.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&m),"GX buffer upload");if(data)memcpy(m.pData,data,size);a.context->Unmap(b.resource.Get(),0);
}
void SSNE_GLAPI VertexAttribPointer_(GLuint i,GLint count,GLenum type,GLboolean norm,GLsizei stride,const void* offset){auto& v=s().arrays[s().array].attrs[i];v.buffer=s().vertex;v.components=count;v.type=type;v.normalized=norm;v.stride=stride;v.offset=unsigned(reinterpret_cast<uintptr_t>(offset));}
void SSNE_GLAPI EnableVertexAttribArray_(GLuint i){s().arrays[s().array].attrs[i].enabled=true;}void SSNE_GLAPI DisableVertexAttribArray_(GLuint i){s().arrays[s().array].attrs[i].enabled=false;}
void SSNE_GLAPI DrawElements_(GLenum mode,GLsizei count,GLenum type,const void* offset){if(mode!=TRIANGLES||type!=UNSIGNED_INT)throw std::runtime_error("DX11 unsupported primitive");if(s().cull&&s().cull_face==FRONT_AND_BACK)return;
    targets();render_state();bind_program();auto& b=s().buffers.at(s().arrays[s().array].indices);unsigned first=unsigned(reinterpret_cast<uintptr_t>(offset));if(first+uint64_t(count)*4>b.size)throw std::runtime_error("DX11 index buffer overflow");s().context->IASetIndexBuffer(b.resource.Get(),DXGI_FORMAT_R32_UINT,first);s().context->DrawIndexed(count,0,0);
}
void SSNE_GLAPI DrawArrays_(GLenum mode,GLint first,GLsizei count){if(mode!=TRIANGLES)throw std::runtime_error("DX11 unsupported primitive");targets();render_state();bind_program();s().context->Draw(count,first);}
void SSNE_GLAPI GenSamplers_(GLsizei n,GLuint* ids){for(int i=0;i<n;++i){ids[i]=s().next++;s().samplers.try_emplace(ids[i]);}}
void SSNE_GLAPI SamplerParameteri_(GLuint id,GLenum p,GLint v){parameter(s().samplers.at(id),p,v);}void SSNE_GLAPI SamplerParameterf_(GLuint id,GLenum p,GLfloat v){parameter(s().samplers.at(id),p,int(v));}
void SSNE_GLAPI BindSampler_(GLuint unit,GLuint id){s().samplers_bound[unit]=id;}
void SSNE_GLAPI Finish_(){wait_idle();}void SSNE_GLAPI Flush_(){s().context->Flush();}
void SSNE_GLAPI GenQueries_(GLsizei n,GLuint* ids){for(int i=0;i<n;++i){ids[i]=s().next++;auto& q=s().timers[ids[i]];D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT,0};check(s().device->CreateQuery(&d,&q.disjoint),"timestamp disjoint query");d.Query=D3D11_QUERY_TIMESTAMP;check(s().device->CreateQuery(&d,&q.start),"timestamp start");check(s().device->CreateQuery(&d,&q.end),"timestamp end");}}
void SSNE_GLAPI BeginQuery_(GLenum target,GLuint id){if(target!=TIME_ELAPSED||s().active_timer)throw std::runtime_error("DX11 invalid timer begin");auto& q=s().timers.at(id);q.complete=false;q.result=0;s().active_timer=id;s().context->Begin(q.disjoint.Get());s().context->End(q.start.Get());}
void SSNE_GLAPI EndQuery_(GLenum target){if(target!=TIME_ELAPSED||!s().active_timer)throw std::runtime_error("DX11 invalid timer end");auto& q=s().timers.at(s().active_timer);s().context->End(q.end.Get());s().context->End(q.disjoint.Get());s().active_timer=0;}
bool timer_result(TimerQuery& q){if(q.complete)return true;auto* c=s().context;D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};UINT64 start=0,end=0;
    for(auto hr:{c->GetData(q.disjoint.Get(),&d,sizeof(d),D3D11_ASYNC_GETDATA_DONOTFLUSH),c->GetData(q.start.Get(),&start,sizeof(start),D3D11_ASYNC_GETDATA_DONOTFLUSH),c->GetData(q.end.Get(),&end,sizeof(end),D3D11_ASYNC_GETDATA_DONOTFLUSH)}){check(hr,"timestamp query");if(hr==S_FALSE)return false;}
    q.complete=true;q.result=(!d.Disjoint&&d.Frequency&&end>=start)?uint64_t((long double)(end-start)*1000000000.L/d.Frequency):0;return true;
}
void SSNE_GLAPI GetQueryObjectiv_(GLuint id,GLenum p,GLint* out){if(p!=QUERY_RESULT_AVAILABLE)throw std::runtime_error("DX11 invalid timer query");*out=timer_result(s().timers.at(id));}
void SSNE_GLAPI GetQueryObjectui64v_(GLuint id,GLenum p,uint64_t* out){if(p!=QUERY_RESULT)throw std::runtime_error("DX11 invalid timer result");auto& q=s().timers.at(id);if(!timer_result(q)){wait_idle();if(!timer_result(q))throw std::runtime_error("DX11 timer did not complete");}*out=q.result;}
void SSNE_GLAPI GetIntegerv_(GLenum p,GLint* out){if(p==NUM_PROGRAM_BINARY_FORMATS)*out=1;else if(p==MAX_TEXTURE_MAX_ANISOTROPY)*out=16;else if(p==MAX_SAMPLES){*out=1;for(unsigned n:{2u,4u,8u}){UINT color=0,depth=0;s().device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM,n,&color);s().device->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT,n,&depth);if(color&&depth)*out=n;}}else throw std::runtime_error("DX11 unknown integer query");}
} // namespace
bool initialize_gx(Device& device,std::string& error){try{
    if(state)throw std::runtime_error("DX11 GX already initialized");state=std::make_unique<State>();auto& a=s();a.device=static_cast<ID3D11Device*>(device.native_device());a.context=static_cast<ID3D11DeviceContext*>(device.native_context());a.description=device.info();
    check(a.device->QueryInterface(IID_PPV_ARGS(a.device1.GetAddressOf())),"D3D11.1 interface");D3D11_FEATURE_DATA_D3D11_OPTIONS options{};a.logic_supported=SUCCEEDED(a.device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,&options,sizeof(options)))&&options.OutputMergerLogicOp;
    float zero[4]={0,0,0,1};D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.Usage=D3D11_USAGE_IMMUTABLE;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA data{zero,0,0};check(a.device->CreateBuffer(&bd,&data,&a.zero_attribute),"default attribute");
    bd.ByteWidth=48;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;check(a.device->CreateBuffer(&bd,nullptr,&a.utility_cb),"utility constants");
    const std::string shared="cbuffer Data:register(b0){float4 color;float4 source;float4 dest;}\n";
    auto vs=compile(shared+"float4 main(uint id:SV_VertexID):SV_Position{float2 p=float2((id<<1)&2,id&2);return float4(p*2-1,source.x,1);}","vs_5_0");check(a.device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&a.fullscreen_vs),"utility vertex shader");
    auto ps=compile(shared+"float4 main():SV_Target{return color;}","ps_5_0");check(a.device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&a.clear_ps),"clear shader");
    ps=compile(shared+"Texture2D image:register(t0);SamplerState sample0:register(s0);float4 main(float4 p:SV_Position):SV_Target{return image.SampleLevel(sample0,source.xy+(p.xy-dest.xy)*dest.zw*source.zw,0);}","ps_5_0");check(a.device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&a.blit_ps),"blit shader");
    return true;
}catch(const std::exception& e){error=e.what();state.reset();return false;}}
void shutdown_gx(){if(state){state->context->ClearState();state.reset();}}
void* texture_view(uint32_t id){auto it=s().textures.find(id);return it==s().textures.end()?nullptr:it->second.view.Get();}
void invalidate_gx_context(){s().state_dirty=true;s().bound_framebuffer=0;}
void wait_idle(){auto& a=s();D3D11_QUERY_DESC d{D3D11_QUERY_EVENT,0};ComPtr<ID3D11Query> q;check(a.device->CreateQuery(&d,&q),"GPU fence");a.context->End(q.Get());a.context->Flush();auto start=GetTickCount64();HRESULT hr;
    while((hr=a.context->GetData(q.Get(),nullptr,0,0))==S_FALSE){if(GetTickCount64()-start>5000)throw std::runtime_error("DX11 GPU wait exceeded five seconds");Sleep(1);}check(hr,"GPU fence wait");}
void* gx_proc(const char* name){
#define PROC(ret,n,args) if(strcmp(name,"gl" #n)==0)return reinterpret_cast<void*>(&n##_);
    SSNE_GL_FUNCS(PROC)
#undef PROC
    if(strcmp(name,"glTexImage2DMultisample")==0)return reinterpret_cast<void*>(&TexImage2DMultisample_);
    if(strcmp(name,"glSamplerParameterf")==0)return reinterpret_cast<void*>(&SamplerParameterf_);
    if(strcmp(name,"glProgramParameteri")==0)return reinterpret_cast<void*>(&ProgramParameteri_);
    if(strcmp(name,"glProgramBinary")==0)return reinterpret_cast<void*>(&ProgramBinary_);
    if(strcmp(name,"glGetProgramBinary")==0)return reinterpret_cast<void*>(&GetProgramBinary_);
    if(strcmp(name,"glGenQueries")==0)return reinterpret_cast<void*>(&GenQueries_);
    if(strcmp(name,"glBeginQuery")==0)return reinterpret_cast<void*>(&BeginQuery_);
    if(strcmp(name,"glEndQuery")==0)return reinterpret_cast<void*>(&EndQuery_);
    if(strcmp(name,"glGetQueryObjectiv")==0)return reinterpret_cast<void*>(&GetQueryObjectiv_);
    if(strcmp(name,"glGetQueryObjectui64v")==0)return reinterpret_cast<void*>(&GetQueryObjectui64v_);
    return nullptr;
}
} // namespace ssne::d3d11
