#pragma once
#include "input/input_source.h"
#include "runtime/cpu_context.h"

namespace nwii::runtime::input {

// Wired Xbox/PDP/PowerA GIP pad, presented to the game as Wiimote channel 0.
// SDL GameController still fills the same channel when the OS owns the pad.
class GipWiimoteSource : public IInputSource {
public:
    GipWiimoteSource();
    ~GipWiimoteSource() override;
    void update(GameCubePadState pads[4], WiimoteState motes[4]) override;
};

// SOME01 boots by polling BTA_DmIsDeviceUp at 0x80140710. The wired pad is
// the remote, so that poll is already satisfied. Also refreshes the guest
// WPAD report from the latest gamepad sample.
void gip_satisfy_bta_device_up(CPUContext& ctx);

}  // namespace nwii::runtime::input
