#pragma once
#include <cstdint>

namespace nwii::runtime::input {

struct GameCubePadState {
    uint16_t buttons;
    int8_t stick_x, stick_y;
    int8_t substick_x, substick_y;
    uint8_t trigger_l, trigger_r;
    uint8_t analog_a, analog_b;
    int8_t err;
};

struct WiimoteState {
    uint32_t buttons;
    float ir_x, ir_y; 
    float accel_x, accel_y, accel_z;
    float gyro_pitch, gyro_yaw, gyro_roll;
    int8_t err;
    float nunchuk_x = 0.0f, nunchuk_y = 0.0f;
    bool nunchuk_connected = false;
};

// Revolution SDK button masks (host-independent, not Bluetooth wire order).
constexpr uint32_t WiiLeft = 0x0001, WiiRight = 0x0002;
constexpr uint32_t WiiDown = 0x0004, WiiUp = 0x0008;
constexpr uint32_t WiiZ = 0x2000, WiiC = 0x4000, WiiHome = 0x8000;

inline float normalized_stick(int value) {
    constexpr int deadzone = 8000;
    if (value >= 32767) return 1.0f;
    if (value > deadzone) return float(value - deadzone) / (32767 - deadzone);
    if (value < -deadzone) return float(value + deadzone) / (32768 - deadzone);
    return 0.0f;
}

inline void set_nunchuk(WiimoteState& mote, int x, int y, bool c, bool z) {
    mote.nunchuk_connected = true;
    mote.nunchuk_x = normalized_stick(x);
    mote.nunchuk_y = normalized_stick(y);
    if (c) mote.buttons |= WiiC;
    if (z) mote.buttons |= WiiZ;
    // Also support titles that navigate with the Remote's directional buttons.
    if (mote.nunchuk_x < -0.5f) mote.buttons |= WiiLeft;
    if (mote.nunchuk_x > 0.5f) mote.buttons |= WiiRight;
    if (mote.nunchuk_y < -0.5f) mote.buttons |= WiiDown;
    if (mote.nunchuk_y > 0.5f) mote.buttons |= WiiUp;
}

class IInputSource {
public:
    virtual ~IInputSource() = default;

    
    
    virtual void update(GameCubePadState pads[4], WiimoteState motes[4]) = 0;
};

} 
