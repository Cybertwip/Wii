#pragma once
#include "input/input_source.h"
#include <vector>

struct _SDL_GameController;

namespace nwii::runtime::input {

class GamepadSource : public IInputSource {
public:
    GamepadSource() = default;
    ~GamepadSource() override;

    void update(GameCubePadState pads[4], WiimoteState motes[4]) override;

private:
    std::vector<_SDL_GameController*> controllers;
};

} 
