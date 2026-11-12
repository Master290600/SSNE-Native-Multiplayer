#include "dx11_backend.h"
#include <SDL.h>
#include <stdexcept>
#ifdef _WIN32
#include <SDL_syswm.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_sdl2.h>
#include "ssne/d3d11_device.h"
#include "ssne/d3d11_gx.h"
#include <deque>
namespace port {
struct Dx11Backend::Impl {
    ssne::d3d11::Device device;bool ui=false;
    std::deque<Microsoft::WRL::ComPtr<ID3D11Query>> pending;
    ID3D11Device* native(){return static_cast<ID3D11Device*>(device.native_device());}
    ID3D11DeviceContext* context(){return static_cast<ID3D11DeviceContext*>(device.native_context());}
};
Dx11Backend::Dx11Backend():impl(std::make_unique<Impl>()){}
Dx11Backend::~Dx11Backend(){shutdown();}
bool Dx11Backend::initialize(SDL_Window* window,std::string& error){
    SDL_SysWMinfo wm{};SDL_VERSION(&wm.version);
    if(!SDL_GetWindowWMInfo(window,&wm)){error=SDL_GetError();return false;}
    int w,h;SDL_GetWindowSizeInPixels(window,&w,&h);
    return impl->device.initialize(wm.info.win.window,w,h,false,error)&&ssne::d3d11::initialize_gx(impl->device,error);
}
bool Dx11Backend::initialize_ui(SDL_Window* window){impl->ui=ImGui_ImplSDL2_InitForD3D(window)&&ImGui_ImplDX11_Init(impl->native(),impl->context());return impl->ui;}
void* Dx11Backend::getproc(const char* name){return ssne::d3d11::gx_proc(name);}
void* Dx11Backend::image(uint32_t t){return ssne::d3d11::texture_view(t);}
void Dx11Backend::new_frame(){ImGui_ImplDX11_NewFrame();}
void Dx11Backend::render(SDL_Window* window){int w,h;SDL_GetWindowSizeInPixels(window,&w,&h);std::string error;
    ssne::d3d11::invalidate_gx_context();
    if(!impl->device.resize(w,h,error))throw std::runtime_error(error);
    if(!w||!h)return;float clear[4]={0,0,0,1};
    if(!impl->device.begin(clear,error))throw std::runtime_error(error);ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
bool Dx11Backend::ready(){auto& a=*impl;while(!a.pending.empty()){
    HRESULT hr=a.context()->GetData(a.pending.front().Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if(hr==S_FALSE)break;if(FAILED(hr))throw std::runtime_error("DX11 GPU completion failed");a.pending.pop_front();}return a.pending.size()<2;
}
size_t Dx11Backend::pending()const{return impl->pending.size();}
void Dx11Backend::present(bool vsync,bool visible){auto& a=*impl;std::string error;if(visible&&!a.device.present(vsync,error))throw std::runtime_error(error);
    if(a.pending.size()>=2)throw std::runtime_error("DX11 submission exceeded the two-frame queue limit");
    D3D11_QUERY_DESC d{D3D11_QUERY_EVENT,0};Microsoft::WRL::ComPtr<ID3D11Query> query;
    if(FAILED(a.native()->CreateQuery(&d,&query)))throw std::runtime_error("DX11 GPU fence creation failed");a.context()->End(query.Get());a.context()->Flush();a.pending.push_back(std::move(query));
}
void Dx11Backend::readback(std::vector<uint8_t>& pixels){std::string error;if(!impl->device.readback(pixels,error))throw std::runtime_error(error);}
void Dx11Backend::shutdown(){if(!impl)return;if(impl->ui){ImGui_ImplDX11_Shutdown();impl->ui=false;}impl->pending.clear();ssne::d3d11::shutdown_gx();}
}
#else
namespace port {
struct Dx11Backend::Impl{};
Dx11Backend::Dx11Backend()=default;Dx11Backend::~Dx11Backend()=default;
bool Dx11Backend::initialize(SDL_Window*,std::string& error){error="Direct3D 11 requires Windows";return false;}
bool Dx11Backend::initialize_ui(SDL_Window*){return false;}
void* Dx11Backend::getproc(const char*){return nullptr;}void* Dx11Backend::image(uint32_t){return nullptr;}
void Dx11Backend::new_frame(){}void Dx11Backend::render(SDL_Window*){}void Dx11Backend::present(bool,bool){}
bool Dx11Backend::ready(){return false;}size_t Dx11Backend::pending()const{return 0;}
void Dx11Backend::readback(std::vector<uint8_t>&){}void Dx11Backend::shutdown(){}
}
#endif
