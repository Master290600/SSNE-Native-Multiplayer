#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
struct SDL_Window;
namespace port {
class Dx11Backend {
public:
    Dx11Backend();~Dx11Backend();
    bool initialize(SDL_Window*,std::string& error);
    bool initialize_ui(SDL_Window*);
    static void* getproc(const char*);
    void* image(uint32_t texture);
    void new_frame();
    void render(SDL_Window*);
    void present(bool vsync,bool visible=true);
    bool ready(); // Nonblocking GPU backpressure. Events continue while busy.
    size_t pending()const;
    void readback(std::vector<uint8_t>& pixels);
    void shutdown();
private:
    struct Impl;std::unique_ptr<Impl> impl;
};
}
