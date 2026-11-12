#pragma once
#include "settings.h"
#include "ssne/input_glyphs.h"
namespace port {
ssne::InputGlyphConfig input_prompt_config(const Settings&, bool controller, SDL_GameControllerType type);
}
