#include "crosshair_renderer.h"
#include "crosshair_image.h"
#include "native_crosshair.h"
#include "settings.h"
#include "settings_menu.h"
#include "ssne/custom_crosshair.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <algorithm>
#include <filesystem>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace port { namespace {
namespace fs=std::filesystem;
struct State {
    bool initialized=false,catalog_dirty=false,selected_known=false;
    fs::path directory;
    std::string directory_text,selected,catalog_error,image_error,preview_error;
    std::vector<std::string> files;
    std::shared_ptr<const ssne::CustomCrosshairImage> image;
    GLuint preview=0;
    SDL_GLContext preview_context=nullptr,last_attempt_context=nullptr;
    bool published=false,published_enabled=false,requested_enabled=false;
    bool published_native_color=true,requested_native_color=true;
    int published_size=32,published_opacity=100,requested_size=32,requested_opacity=100;
    uint64_t catalog_scans=0,image_loads=0,preview_uploads=0;
    std::shared_ptr<const ssne::CustomCrosshairImage> published_image;
} state;
std::string utf8(const fs::path& path){const auto text=path.u8string();return {text.begin(),text.end()};}
const char* localized(const char* en,const char* de,const char* fr){const auto language=native_crosshair_language();return language==1?de:language==2?fr:en;}
void publish(bool enabled,int size,int opacity,bool native_color) {
    size=std::clamp(size,8,256);opacity=std::clamp(opacity,1,100);
    if(state.published&&state.published_enabled==enabled&&state.published_size==size&&state.published_native_color==native_color&&
       state.published_opacity==opacity&&state.published_image==state.image)return;
    ssne::CustomCrosshairConfig config;config.enabled=enabled;config.size=size;config.opacity=opacity;config.image=state.image;config.native_color=native_color;
    ssne::set_custom_crosshair(config);
    state.published=true;state.published_enabled=enabled;state.published_size=size;
    state.published_opacity=opacity;state.published_native_color=native_color;state.published_image=state.image;
}
void release_preview() {
    // A replaced/destroyed context owns its old objects; never delete its numeric
    // texture ID in a different context, where it might name an unrelated object.
    if(state.preview&&state.preview_context&&state.preview_context==SDL_GL_GetCurrentContext())glDeleteTextures(1,&state.preview);
    state.preview=0;state.preview_context=nullptr;
}
void clear_image(int size,int opacity) {
    state.image.reset();publish(false,size,opacity,state.requested_native_color);release_preview();
    state.image_error.clear();state.preview_error.clear();state.last_attempt_context=nullptr;
}
struct UploadState {
    GLint texture=0,pbo=0,stores[4]{};
    using BindBuffer=void (APIENTRY*)(GLenum,GLuint);
    BindBuffer bind_buffer=nullptr;
    static constexpr GLenum names[4]={GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,GL_UNPACK_SKIP_ROWS,GL_UNPACK_SKIP_PIXELS};
    UploadState() {
        glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);
        for(unsigned i=0;i<4;++i)glGetIntegerv(names[i],&stores[i]);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&pbo);
        bind_buffer=reinterpret_cast<BindBuffer>(SDL_GL_GetProcAddress("glBindBuffer"));
    }
    bool prepare() {
        if(pbo&&!bind_buffer)return false;
        if(pbo)bind_buffer(GL_PIXEL_UNPACK_BUFFER,0);
        glPixelStorei(GL_UNPACK_ALIGNMENT,1);glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS,0);glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);return true;
    }
    ~UploadState() {
        glBindTexture(GL_TEXTURE_2D,GLuint(texture));
        for(unsigned i=0;i<4;++i)glPixelStorei(names[i],stores[i]);
        if(pbo&&bind_buffer)bind_buffer(GL_PIXEL_UNPACK_BUFFER,GLuint(pbo));
    }
};
bool upload_preview() {
    state.preview_error.clear();state.last_attempt_context=SDL_GL_GetCurrentContext();
    if(!state.last_attempt_context) {state.preview_error="Crosshair preview is not ready.";return false;}
    if(!state.image)return false;
    // Isolate this upload from an earlier GL error without spinning indefinitely.
    for(unsigned i=0;i<16&&glGetError()!=GL_NO_ERROR;++i){}
    GLuint texture=0;bool okay=false;
    {
        UploadState previous;
        if(!previous.prepare()){state.preview_error="Crosshair preview upload is not ready.";return false;}
        glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,GLsizei(state.image->width),GLsizei(state.image->height),0,
                     GL_RGBA,GL_UNSIGNED_BYTE,state.image->rgba.data());
        okay=texture&&glGetError()==GL_NO_ERROR;
    }
    if(!okay) {if(texture)glDeleteTextures(1,&texture);state.preview_error="Cannot upload the crosshair preview; the original is kept.";return false;}
    state.preview=texture;state.preview_context=state.last_attempt_context;++state.preview_uploads;return true;
}
void scan_catalog() {
    ++state.catalog_scans;state.files.clear();state.catalog_error.clear();std::error_code error;
    fs::create_directories(state.directory,error);
    if(error) {state.catalog_error="Cannot create the crosshairs folder: "+error.message();return;}
    state.files=list_crosshair_pngs(state.directory,state.catalog_error);
}
void load_selected(const Settings& settings) {
    clear_image(settings.crosshair_size,settings.crosshair_opacity);
    if(state.selected.empty())return;
    if(!safe_crosshair_filename(state.selected)) {state.image_error="Choose a PNG filename from the crosshairs folder.";return;}
    CrosshairImage decoded;++state.image_loads;
    if(!load_crosshair_png(state.directory/fs::u8path(state.selected),decoded,state.image_error))return;
    auto next=std::make_shared<ssne::CustomCrosshairImage>();
    next->width=decoded.width;next->height=decoded.height;
    next->left=decoded.visible.left;next->top=decoded.visible.top;
    next->right=decoded.visible.right;next->bottom=decoded.visible.bottom;
    next->rgba=std::move(decoded.rgba);state.image=std::move(next);
    upload_preview();
}
std::string json_string(const std::string& value) {
    static constexpr char hex[]="0123456789abcdef";std::string out="\"";
    for(unsigned char c:value){if(c=='\"'||c=='\\'){out+='\\';out+=char(c);}else if(c<32||c==127){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=char(c);}return out+'\"';
}
bool trace_enabled() {
    static const bool enabled=[] {const auto hidden=std::getenv("SSNE_TEST_HIDDEN");return hidden&&std::strcmp(hidden,"1")==0&&std::getenv("SSNE_TEST_CROSSHAIR");}();return enabled;
}
std::string current_error() {
    if(!state.image_error.empty())return state.image_error;
    if(!state.preview_error.empty())return state.preview_error;
    if(!state.catalog_error.empty())return state.catalog_error;
    if(state.requested_enabled&&state.image&&!native_crosshair_supported())return localized(
        "The original HUD is not ready; the original crosshair remains active.",
        "Das Original-HUD ist noch nicht bereit; das Original-Fadenkreuz bleibt aktiv.",
        "L'affichage d'origine n'est pas encore pret ; le viseur d'origine reste actif.");
    return {};
}
void write_render_state() {
    if(!trace_enabled())return;static uint32_t last=0;static bool written=false;
    const auto now=SDL_GetTicks();if(written&&uint32_t(now-last)<25)return;last=now;written=true;
    const auto core=ssne::custom_crosshair_stats();std::ostringstream out;out<<std::setprecision(9);
    out<<"{\"enabled\":"<<(state.requested_enabled?"true":"false")<<",\"file\":"<<json_string(state.selected)
       <<",\"size\":"<<state.requested_size<<",\"opacity\":"<<state.requested_opacity
       <<",\"native_color\":"<<(state.requested_native_color?"true":"false")
       <<",\"hook_supported\":"<<(native_crosshair_supported()?"true":"false")
       <<",\"ready\":"<<(ssne::custom_crosshair_active()?"true":"false")
       <<",\"image_loaded\":"<<(state.image?"true":"false")<<",\"preview_ready\":"<<(state.preview?"true":"false");
    const auto image=state.image;
    out<<",\"width\":"<<(image?image->width:0)<<",\"height\":"<<(image?image->height:0)
       <<",\"left\":"<<(image?image->left:0)<<",\"top\":"<<(image?image->top:0)
       <<",\"right\":"<<(image?image->right:0)<<",\"bottom\":"<<(image?image->bottom:0)
       <<",\"error\":"<<json_string(current_error())
       <<",\"catalog_scans\":"<<state.catalog_scans<<",\"image_loads\":"<<state.image_loads<<",\"preview_uploads\":"<<state.preview_uploads;
    out<<",\"core\":{\"ready\":"<<(core.ready?"true":"false")<<",\"stream_active\":"<<(core.stream_active?"true":"false")
       <<",\"last_replaced\":"<<(core.last_replaced?"true":"false")
       <<",\"last_native_color\":"<<(core.last_native_color?"true":"false")
       <<",\"queued\":"<<core.queued<<",\"parsed\":"<<core.parsed<<",\"tags\":"<<core.tags
       <<",\"replacements\":"<<core.replacements<<",\"fallbacks\":"<<core.fallbacks<<",\"rejected\":"<<core.rejected
       <<",\"dropped\":"<<core.dropped<<",\"resets\":"<<core.resets<<",\"texture_id\":"<<core.texture_id
       <<",\"width\":"<<core.width<<",\"height\":"<<core.height<<",\"left\":"<<core.left<<",\"top\":"<<core.top
       <<",\"right\":"<<core.right<<",\"bottom\":"<<core.bottom<<",\"last_primitive\":"<<core.last_primitive
       <<",\"last_stages\":"<<core.last_stages<<",\"last_cenv\":"<<core.last_cenv<<",\"last_aenv\":"<<core.last_aenv
       <<",\"last_texmap\":"<<core.last_texmap<<",\"last_texcoord\":"<<core.last_texcoord;
    auto coordinate=[&](const char* key,float value){out<<",\""<<key<<"\":";if(std::isfinite(value))out<<value;else out<<"null";};
    coordinate("original_left",core.original_left);coordinate("original_top",core.original_top);
    coordinate("original_right",core.original_right);coordinate("original_bottom",core.original_bottom);
    coordinate("drawn_left",core.drawn_left);coordinate("drawn_top",core.drawn_top);
    coordinate("drawn_right",core.drawn_right);coordinate("drawn_bottom",core.drawn_bottom);out<<"}}";
    std::ofstream file("crosshair-render-state.json.pending",std::ios::binary|std::ios::trunc);file<<out.str();file.close();
    if(file){std::remove("crosshair-render-state.json");std::rename("crosshair-render-state.json.pending","crosshair-render-state.json");}
}

}
void initialize_crosshair_renderer(const std::string& executable_directory) {
    shutdown_crosshair_renderer();state.directory=fs::u8path(executable_directory)/"crosshairs";
    state.directory_text=utf8(state.directory);state.initialized=true;state.catalog_dirty=true;
}
void update_crosshair_renderer(Settings& settings,bool reload) {
    if(!state.initialized)return;
    state.requested_enabled=settings.crosshair_enabled;state.requested_native_color=settings.crosshair_native_color;
    state.requested_size=std::clamp(settings.crosshair_size,8,256);state.requested_opacity=std::clamp(settings.crosshair_opacity,1,100);
    if(state.catalog_dirty||reload){scan_catalog();state.catalog_dirty=false;}
    if(!state.selected_known||state.selected!=settings.crosshair_file||reload) {
        state.selected=settings.crosshair_file;state.selected_known=true;load_selected(settings);
    } else if(state.image) {
        const auto current=SDL_GL_GetCurrentContext();
        if(state.preview&&current&&state.preview_context!=current){state.preview=0;state.preview_context=nullptr;}
        // Context recovery retries the upload, never filesystem decoding. An
        // upload failure in the same context retries only on explicit Refresh.
        if(!state.preview&&current&&state.last_attempt_context!=current)upload_preview();
    }
    publish(settings.crosshair_enabled&&state.image&&state.preview&&native_crosshair_supported(),
            settings.crosshair_size,settings.crosshair_opacity,settings.crosshair_native_color);
    write_render_state();
}
void fill_crosshair_menu_info(MenuInfo& info) {
    info.crosshair_directory=state.directory_text;info.crosshair_files=state.files;
    info.crosshair_preview_tex=0;info.crosshair_preview_w=info.crosshair_preview_h=0;
    info.crosshair_preview_u0=info.crosshair_preview_v0=0;info.crosshair_preview_u1=info.crosshair_preview_v1=1;
    info.crosshair_language=int(native_crosshair_language());info.crosshair_error=current_error();
    if(state.preview&&state.image) {
        info.crosshair_preview_tex=uintptr_t(state.preview);
        info.crosshair_preview_w=int(state.image->right-state.image->left);
        info.crosshair_preview_h=int(state.image->bottom-state.image->top);
        info.crosshair_preview_u0=float(state.image->left)/state.image->width;
        info.crosshair_preview_v0=float(state.image->top)/state.image->height;
        info.crosshair_preview_u1=float(state.image->right)/state.image->width;
        info.crosshair_preview_v1=float(state.image->bottom)/state.image->height;
    }
}
void shutdown_crosshair_renderer() {
    ssne::set_custom_crosshair({});release_preview();state=State{};
}
}
