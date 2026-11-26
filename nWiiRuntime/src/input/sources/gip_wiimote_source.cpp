#include "input/gip_wiimote.h"

#include "../gip/GipGamepad.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

namespace nwii::runtime::input {
namespace {

using nwii::gip::Button;
using nwii::gip::Event;
using nwii::gip::State;
using nwii::gip::USBHub;

// Guest addresses for SOME01 (Rhythm Heaven Fever). BTA_DmIsDeviceUp returns
// true when btm_cb+0x64E == 5. WPAD keeps one channel object per slot.
constexpr uint32_t kDeviceUpPoll = 0x80140710;
constexpr uint32_t kBtaDeviceUp = 0x803AB4C6;
constexpr uint32_t kWpadChanTable = 0x80378410;
constexpr int kReport0 = 160;
constexpr int kReportStride = 96;

struct Wire {
    std::atomic<uint32_t> buttons{0};
    std::atomic<int32_t> acc_x{0};
    std::atomic<int32_t> acc_y{0};
    std::atomic<int32_t> acc_z{0x200};
    std::atomic<int32_t> ir_x{512};
    std::atomic<int32_t> ir_y{384};
    std::atomic<int32_t> stick_x{0};
    std::atomic<int32_t> stick_y{0};
    std::atomic<bool> nunchuk{false};
};

Wire& wire() {
    static Wire w;
    return w;
}

bool usb_enabled() {
    const char* keys[] = {"NWII_GIP_USB", "RECOMP_GIP_USB", "KIRIKAE_GIP_USB"};
    for (const char* key : keys) {
        const char* v = std::getenv(key);
        if (v && std::strcmp(v, "0") == 0) return false;
    }
    return USBHub::available();
}

uint32_t wiimote_buttons(const State& s) {
    uint32_t b = 0;
    if (s.buttons & Button::A) b |= 0x0800;
    if (s.buttons & Button::B) b |= 0x0400;
    if (s.buttons & Button::X) b |= 0x0200;       // 1
    if (s.buttons & Button::Y) b |= 0x0100;       // 2
    if (s.buttons & Button::Menu) b |= 0x0010;    // +
    if (s.buttons & Button::View) b |= 0x1000;    // -
    if (s.guide) b |= WiiHome;                     // Home
    if (s.buttons & Button::Up) b |= 0x0008;
    if (s.buttons & Button::Down) b |= 0x0004;
    if (s.buttons & Button::Left) b |= 0x0001;
    if (s.buttons & Button::Right) b |= 0x0002;
    return b;
}

void apply_gc(GameCubePadState& pad, const State& s) {
    pad.err = 0;
    if (s.buttons & Button::A) pad.buttons |= 0x0100;
    if (s.buttons & Button::B) pad.buttons |= 0x0200;
    if (s.buttons & Button::X) pad.buttons |= 0x0400;
    if (s.buttons & Button::Y) pad.buttons |= 0x0800;
    if (s.buttons & Button::Menu) pad.buttons |= 0x1000;
    if (s.buttons & Button::Up) pad.buttons |= 0x0008;
    if (s.buttons & Button::Down) pad.buttons |= 0x0004;
    if (s.buttons & Button::Left) pad.buttons |= 0x0001;
    if (s.buttons & Button::Right) pad.buttons |= 0x0002;
    if (s.buttons & Button::LeftBumper) pad.buttons |= 0x0040;
    if (s.buttons & Button::RightBumper) pad.buttons |= 0x0020;
    if (s.rightTrigger > 512) pad.buttons |= 0x0010;
    auto stick = [](int16_t v) -> int8_t {
        return (int8_t)((v / 32767.0f) * 127.0f);
    };
    pad.stick_x = stick(s.leftX);
    pad.stick_y = stick(s.leftY); // GIP Y is already positive up.
    pad.substick_x = stick(s.rightX);
    pad.substick_y = stick(s.rightY);
    pad.trigger_l = (uint8_t)(nwii::gip::triggerValue(s.leftTrigger) * 255.0f);
    pad.trigger_r = (uint8_t)(nwii::gip::triggerValue(s.rightTrigger) * 255.0f);
}

// The wired pad is the remote. Report it up before the guest polls
// BTA_DmIsDeviceUp, so that wait cannot block boot.
bool device_up = true;

void publish_guest(CPUContext& ctx) {
    if (!device_up) return;
    uint32_t dev = ctx.mmu.read32(kWpadChanTable);
    if (dev < 0x80000000u || dev >= 0x81800000u) return;
    if (!ctx.mmu.get_ptr(dev + 0xC00)) return;

    // WPADProbe returns the word at +0x900. -1 means no controller.
    if ((int32_t)ctx.mmu.read32(dev + 2304) == -1)
        ctx.mmu.write32(dev + 2304, 0);
    Wire& w = wire();
    const uint8_t extension = w.nunchuk.load() ? 1 : 0;
    ctx.mmu.write8(dev + 2309, extension);
    if (ctx.mmu.read32(dev + 2336) == 0)
        ctx.mmu.write32(dev + 2336, 1);

    const uint16_t buttons = (uint16_t)w.buttons.load();
    const int16_t ax = (int16_t)w.acc_x.load();
    const int16_t ay = (int16_t)w.acc_y.load();
    const int16_t az = (int16_t)w.acc_z.load();
    const int16_t ix = (int16_t)w.ir_x.load();
    const int16_t iy = (int16_t)w.ir_y.load();

    // Both ring slots. The reader picks one from the byte at +2316.
    for (int slot = 0; slot < 2; ++slot) {
        uint32_t base = dev + kReport0 + slot * kReportStride;
        ctx.mmu.write16(base + 0, buttons);
        ctx.mmu.write16(base + 2, (uint16_t)ax);
        ctx.mmu.write16(base + 4, (uint16_t)ay);
        ctx.mmu.write16(base + 6, (uint16_t)az);
        ctx.mmu.write16(base + 8, (uint16_t)ix);
        ctx.mmu.write16(base + 10, (uint16_t)iy);
        ctx.mmu.write16(base + 12, 8);
        ctx.mmu.write8(base + 14, 1);
        ctx.mmu.write8(base + 40, extension);
        ctx.mmu.write8(base + 41, 0); // WPAD_ERR_NONE
        // WPADFSStatus: extension acceleration and signed, centered stick.
        ctx.mmu.write16(base + 42, 0);
        ctx.mmu.write16(base + 44, 0);
        ctx.mmu.write16(base + 46, 512);
        ctx.mmu.write8(base + 48, (uint8_t)w.stick_x.load());
        ctx.mmu.write8(base + 49, (uint8_t)w.stick_y.load());
    }
}

std::unique_ptr<USBHub>& hub() {
    static std::unique_ptr<USBHub> h;
    return h;
}

bool gip_live = false;
State gip_state{};

}  // namespace

GipWiimoteSource::GipWiimoteSource() {
    if (!usb_enabled()) {
        std::cout << "[GIP] USB discovery off; SDL gamepad is the Wiimote\n";
        return;
    }
    hub() = std::make_unique<USBHub>();
    hub()->start();
    std::cout << "[GIP] USB discovery on; gamepad is Wiimote channel 0\n";
}

GipWiimoteSource::~GipWiimoteSource() {
    if (hub()) hub()->stop();
}

void GipWiimoteSource::update(GameCubePadState pads[4], WiimoteState motes[4]) {
    if (hub()) {
        for (const Event& event : hub()->drain()) {
            if (!event.connected) {
                gip_live = false;
                gip_state = {};
                std::cout << "[GIP] disconnected " << event.device.path << "\n";
                continue;
            }
            if (!gip_live) {
                std::cout << "[GIP] " << event.device.name << " [" << std::hex
                          << event.device.vendor << ":" << event.device.product
                          << std::dec << "] is Wiimote 0\n";
            }
            gip_live = true;
            if (event.report) gip_state = event.state;
            else gip_state.guide = event.state.guide;
        }
    }

    // Channel 0 is a connected remote even before a physical pad enumerates.
    motes[0].err = 0;
    if (motes[0].accel_z == 0.0f) motes[0].accel_z = 1.0f;

    if (gip_live) {
        motes[0].buttons |= wiimote_buttons(gip_state);
        float rx = nwii::gip::stickValue(gip_state.rightX);
        float ry = -nwii::gip::stickValue(gip_state.rightY); // screen Y grows down
        motes[0].ir_x += rx * 0.04f;
        motes[0].ir_y += ry * 0.04f;
        if (motes[0].ir_x < 0.0f) motes[0].ir_x = 0.0f;
        if (motes[0].ir_x > 1.0f) motes[0].ir_x = 1.0f;
        if (motes[0].ir_y < 0.0f) motes[0].ir_y = 0.0f;
        if (motes[0].ir_y > 1.0f) motes[0].ir_y = 1.0f;
        set_nunchuk(motes[0], gip_state.leftX, gip_state.leftY,
            (gip_state.buttons & (Button::LeftBumper | Button::RightBumper)) != 0,
            gip_state.leftTrigger > 512 || gip_state.rightTrigger > 512);
        motes[0].accel_z = 1.0f;
        apply_gc(pads[0], gip_state);
    }

    Wire& w = wire();
    w.buttons.store(motes[0].buttons);
    w.stick_x.store((int32_t)(motes[0].nunchuk_x * 127.0f));
    w.stick_y.store((int32_t)(motes[0].nunchuk_y * 127.0f));
    w.nunchuk.store(motes[0].nunchuk_connected);
    w.acc_x.store((int32_t)(motes[0].accel_x * 256.0f));
    w.acc_y.store((int32_t)(motes[0].accel_y * 256.0f));
    int32_t az = (int32_t)(motes[0].accel_z * 512.0f);
    if (az == 0) az = 0x200;
    w.acc_z.store(az);
    float irx = motes[0].ir_x;
    float iry = motes[0].ir_y;
    if (irx < 0.0f) irx = 0.0f;
    if (irx > 1.0f) irx = 1.0f;
    if (iry < 0.0f) iry = 0.0f;
    if (iry > 1.0f) iry = 1.0f;
    w.ir_x.store((int32_t)(irx * 1023.0f));
    w.ir_y.store((int32_t)(iry * 767.0f));
}

void gip_satisfy_bta_device_up(CPUContext& ctx) {
    device_up = true;
    if (ctx.mmu.get_ptr(kBtaDeviceUp) && ctx.mmu.read8(kBtaDeviceUp) != 5) {
        ctx.mmu.write8(kBtaDeviceUp, 5);
        static bool announced = false;
        if (!announced) {
            announced = true;
            std::cout << "[GIP] Wiimote 0 assumed up; buttons follow the GIP/SDL gamepad\n";
        }
    }
    if (ctx.pc == kDeviceUpPoll && ctx.cr[0].eq) {
        ctx.cr[0].eq = false;
        ctx.cr[0].gt = true;
        ctx.gpr[0] = 1;
        ctx.gpr[3] = 1;
        if (ctx.mmu.get_ptr(kBtaDeviceUp))
            ctx.mmu.write8(kBtaDeviceUp, 5);
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::cout << "[GIP] Wiimote is up; released BTA_DmIsDeviceUp poll\n";
        }
    }
    publish_guest(ctx);
}

}  // namespace nwii::runtime::input
