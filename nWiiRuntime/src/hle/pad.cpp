#include "runtime/cpu_context.h"
#include "input/input_manager.h"

using namespace nwii::runtime;
using namespace nwii::runtime::input;

extern "C" {

void PADInit(CPUContext& ctx) {}
void WPADInit(CPUContext& ctx) {}
void KPADInit(CPUContext& ctx) {}

void PADRead(CPUContext& ctx) {
    uint32_t pad_status_array_addr = ctx.gpr[3];
    GameCubePadState state = InputManager::get().get_gcpad_state(0);

    ctx.mmu.write16(pad_status_array_addr, state.buttons);
    ctx.mmu.write8(pad_status_array_addr + 2, (uint8_t)state.stick_x);
    ctx.mmu.write8(pad_status_array_addr + 3, (uint8_t)state.stick_y);
    ctx.mmu.write8(pad_status_array_addr + 4, (uint8_t)state.substick_x);
    ctx.mmu.write8(pad_status_array_addr + 5, (uint8_t)state.substick_y);
    ctx.mmu.write8(pad_status_array_addr + 6, state.trigger_l);
    ctx.mmu.write8(pad_status_array_addr + 7, state.trigger_r);
    ctx.mmu.write8(pad_status_array_addr + 8, state.buttons & 0x0100 ? 0xFF : 0);
    ctx.mmu.write8(pad_status_array_addr + 9, state.buttons & 0x0200 ? 0xFF : 0);
    ctx.mmu.write8(pad_status_array_addr + 10, state.err);
    ctx.mmu.write8(pad_status_array_addr + 11, 0);

    for (int i = 1; i < 4; i++) {
        uint32_t offset = pad_status_array_addr + (i * 12);
        ctx.mmu.write16(offset, 0);
        ctx.mmu.write8(offset + 10, (uint8_t)-1); 
    }
}

void WPADRead(CPUContext& ctx) {
    int32_t chan = (int32_t)ctx.gpr[3];
    uint32_t data_ptr = ctx.gpr[4];

    if (chan >= 0 && chan < 4 && data_ptr != 0) {
        WiimoteState state = InputManager::get().get_wiimote_state(chan);
        ctx.mmu.write16(data_ptr, (uint16_t)state.buttons);
        ctx.mmu.write16(data_ptr + 2, (uint16_t)(int16_t)(state.accel_x * 256.0f));
        ctx.mmu.write16(data_ptr + 4, (uint16_t)(int16_t)(state.accel_y * 256.0f));
        ctx.mmu.write16(data_ptr + 6, (uint16_t)(int16_t)(state.accel_z * 512.0f));
        for (int offset = 8; offset < 40; offset += 4)
            ctx.mmu.write32(data_ptr + offset, 0);
        ctx.mmu.write8(data_ptr + 40, state.nunchuk_connected ? 1 : 0);
        ctx.mmu.write8(data_ptr + 41, (uint8_t)state.err);
    }
    ctx.gpr[3] = 0; 
}

void KPADRead(CPUContext& ctx) {
    int32_t chan = (int32_t)ctx.gpr[3];
    uint32_t data_array_ptr = ctx.gpr[4];
    uint32_t length = ctx.gpr[5];

    int read_count = 0;
    if (chan >= 0 && chan < 4 && data_array_ptr != 0 && length > 0) {
        WiimoteState state = InputManager::get().get_wiimote_state(chan);

        if (state.err == 0) {

            

            

            
            static uint32_t prev_buttons[4] = {};
            uint32_t hold    = state.buttons;
            uint32_t trigger = hold & ~prev_buttons[chan];  
            uint32_t release = ~hold & prev_buttons[chan];  
            prev_buttons[chan] = hold;

            ctx.mmu.write32(data_array_ptr + 0x00, hold);
            ctx.mmu.write32(data_array_ptr + 0x04, trigger);
            ctx.mmu.write32(data_array_ptr + 0x08, release);
            
            for (int offset = 0x0C; offset < 0x7C; offset += 4)
                ctx.mmu.write32(data_array_ptr + offset, 0);
            ctx.mmu.write_f32(data_array_ptr + 0x0C, state.accel_x);
            ctx.mmu.write_f32(data_array_ptr + 0x10, state.accel_y);
            ctx.mmu.write_f32(data_array_ptr + 0x14, state.accel_z);
            ctx.mmu.write8(data_array_ptr + 0x5C, state.nunchuk_connected ? 1 : 0);
            ctx.mmu.write8(data_array_ptr + 0x5D, (uint8_t)state.err);
            ctx.mmu.write8(data_array_ptr + 0x5F, state.nunchuk_connected ? 5 : 2);
            if (state.nunchuk_connected) {
                ctx.mmu.write_f32(data_array_ptr + 0x60, state.nunchuk_x);
                ctx.mmu.write_f32(data_array_ptr + 0x64, state.nunchuk_y);
                ctx.mmu.write_f32(data_array_ptr + 0x70, 1.0f);
                ctx.mmu.write_f32(data_array_ptr + 0x74, 1.0f);
            }

            read_count = 1;
        }
    }
    ctx.gpr[3] = read_count;
}

} 
