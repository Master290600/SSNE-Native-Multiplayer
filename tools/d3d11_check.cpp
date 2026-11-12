#define NOMINMAX
#include <windows.h>
#include "ssne/d3d11_device.h"
#include "png_write.h"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
using ssne::d3d11::Device;
using ssne::d3d11::Vertex;
namespace {
void require(bool ok,const std::string& error){if(!ok)throw std::runtime_error(error);}
LRESULT CALLBACK proc(HWND window,UINT message,WPARAM w,LPARAM l){
    if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
    if(message==WM_KEYDOWN && w==VK_ESCAPE){DestroyWindow(window);return 0;}
    return DefWindowProcW(window,message,w,l);
}
struct Window {
    HWND handle=nullptr;
    Window(){WNDCLASSW wc{};wc.lpfnWndProc=proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"SSNED3D11Check";wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
        require(RegisterClassW(&wc)!=0,"Cannot register test window");
        handle=CreateWindowW(wc.lpszClassName,L"SSNE-RECOOP - DX11 renderer foundation (not the game)",WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,CW_USEDEFAULT,960,600,nullptr,nullptr,wc.hInstance,nullptr);
        require(handle!=nullptr,"Cannot create test window");}
    ~Window(){if(IsWindow(handle))DestroyWindow(handle);UnregisterClassW(L"SSNED3D11Check",GetModuleHandleW(nullptr));}
};
const uint32_t indices[]={0,1,2,0,2,3};
std::array<Vertex,4> quad(float size,float z,uint32_t color){
    return {{{{-size,size,z,1},color,{0,0}},{{size,size,z,1},color,{1,0}},
             {{size,-size,z,1},color,{1,1}},{{-size,-size,z,1},color,{0,1}}}};
}
const uint8_t checker[]={255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
const uint8_t white[]={255,255,255,255};
const float background[]={.0625f,.0625f,.0625f,1};
void scene(Device& d,std::string& error){
    require(d.begin(background,error),error);require(d.texture(2,2,checker,error),error);
    require(d.draw(quad(.9f,.5f,0xffffffff),indices,false,true,error),error);
    require(d.texture(1,1,white,error),error);
    require(d.draw(quad(.2f,-.5f,0xff00ffff),indices,false,true,error),error); // yellow near
    require(d.draw(quad(.2f,.75f,0xffff00ff),indices,false,true,error),error); // magenta behind, rejected
}
void pixel(const std::vector<uint8_t>& pixels,unsigned w,unsigned h,float x,float y,std::array<int,4> expected){
    const size_t at=(size_t(unsigned(y*h))*w+unsigned(x*w))*4;
    for(unsigned k=0;k<4;++k)require(std::abs(int(pixels[at+k])-expected[k])<=1,"Pixel mismatch at "+std::to_string(x)+","+std::to_string(y)+" channel "+std::to_string(k));
}
void validate(Device& d,std::string& error){
    scene(d,error);std::vector<uint8_t> pixels;require(d.readback(pixels,error),error);
    require(pixels.size()==size_t(d.width())*d.height()*4,"Readback size mismatch");
    pixel(pixels,d.width(),d.height(),.25f,.25f,{255,0,0,255});
    pixel(pixels,d.width(),d.height(),.75f,.25f,{0,255,0,255});
    pixel(pixels,d.width(),d.height(),.25f,.75f,{0,0,255,255});
    pixel(pixels,d.width(),d.height(),.75f,.75f,{255,255,255,255});
    pixel(pixels,d.width(),d.height(),.5f,.5f,{255,255,0,255});
    pixel(pixels,d.width(),d.height(),.01f,.01f,{16,16,16,255});
    require(d.present(false,error),error);
}
}
int main(int argc,char** argv){try{
    bool warp=false,demo=false;std::string output;
    for(int i=1;i<argc;++i){const std::string arg=argv[i];
        if(arg=="--warp")warp=true;else if(arg=="--demo")demo=true;
        else if(arg=="--output" && i+1<argc)output=argv[++i];else throw std::runtime_error("Usage: SSNE-RECOOP-DX11-Check [--warp] [--demo] [--output image.png]");}
    Window window;Device device;std::string error;
    require(!device.resize(320,240,error),"Uninitialized resize accepted");
    require(device.initialize(window.handle,320,240,warp,error),error);
    require(!device.initialize(window.handle,320,240,warp,error),"Second initialization accepted");
    std::cout<<device.info()<<std::endl;validate(device,error);
    const float gray[]={.25f,.25f,.25f,1};require(device.begin(gray,error),error);
    require(device.draw(quad(1,0,0x800000ff),indices,true,false,error),error);
    std::vector<uint8_t> rgba;require(device.readback(rgba,error),error);
    pixel(rgba,device.width(),device.height(),.5f,.5f,{160,32,32,255});
    const uint32_t bad[]={0,1,99};require(!device.draw(quad(1,0,~0u),bad,false,false,error),"Out-of-range index accepted");
    require(!device.texture(2,2,white,error),"Short texture accepted");
    require(device.present(false,error),error);
    require(device.resize(257,129,error),error);validate(device,error);
    require(device.resize(0,0,error),error);require(!device.begin(background,error),"Minimized draw accepted");
    require(device.present(false,error),error);
    require(device.resize(640,360,error),error);validate(device,error);
    scene(device,error);require(device.readback(rgba,error),error);
    if(!output.empty()){std::vector<uint32_t> pixels(rgba.size()/4);std::memcpy(pixels.data(),rgba.data(),rgba.size());
        require(port::write_png(output,pixels.data(),device.width(),device.height()),"Cannot write PNG");}
    require(device.present(false,error),error);
    std::cout<<"PASS: HLSL, indexed texture/color, depth, alpha blend, readback pitch, resize, minimize/restore and invalid inputs. GX/TEV game integration NOT IMPLEMENTED."<<std::endl;
    if(demo){
        ShowWindow(window.handle,SW_SHOW);SetForegroundWindow(window.handle);
        const auto until=GetTickCount64()+30000;MSG message{};
        while(IsWindow(window.handle) && GetTickCount64()<until){
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
            if(!IsWindow(window.handle))break;
            RECT rect{};GetClientRect(window.handle,&rect);
            if(IsIconic(window.handle)){Sleep(30);continue;}
            require(device.resize(unsigned(rect.right),unsigned(rect.bottom),error),error);
            scene(device,error);require(device.present(true,error),error);Sleep(8);
        }
    }
    return 0;
}catch(const std::exception& e){std::cerr<<"DX11 CHECK FAILED: "<<e.what()<<std::endl;return 1;}}
