#pragma once
#include <SDL.h>
#include <imgui.h>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace port::coop {struct ChatMessage;}
namespace port {
// UI-thread chat editor. The caller consumes events before gameplay hotkeys and
// sends neutral controls while active(). No game or network state is owned here.
class ChatOverlay {
public:
    bool active() const {return active_;}
    bool handle_event(const SDL_Event&,bool multiplayer_available,bool can_open);
    bool handle_event(const SDL_Event&,bool multiplayer_available,bool can_open,bool open_requested);
    std::optional<std::string> take_submission();
    void close();
    void reset_session();
    void send_failed();
    void draw(const std::vector<coop::ChatMessage>&,unsigned language,ImVec2 image_origin,ImVec2 image_size);
private:
    struct Line {uint64_t sequence=0;unsigned slot=0;std::string name,text;double received=0;};
    bool active_=false,suppress_open_text_=false,scroll_to_bottom_=false;
    uint32_t open_timestamp_=0,last_trace_=0;
    SDL_Scancode opening_key_=SDL_SCANCODE_UNKNOWN;
    std::string opening_text_,opening_upper_text_;
    uint64_t last_sequence_=0;
    std::string draft_,composition_,last_submitted_;
    size_t cursor_=0,anchor_=0;
    float input_scroll_=0;
    double error_until_=0;
    std::deque<std::string> submissions_;
    std::deque<Line> history_;
    void erase_selection();
    void insert_text(const std::string&);
    void input_field(unsigned language,ImVec2& minimum,ImVec2& maximum);
    void write_trace(const std::vector<const Line*>& visible,ImVec2 minimum,ImVec2 maximum,bool drawn=false,float newest_alpha=0);
};
}
