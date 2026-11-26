#include "runtime/hw/hw.h"
#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include <iostream>
#include <cstring>
#include <algorithm>

namespace nwii::runtime {
    extern CPUContext* g_ctx_ptr;
}

namespace nwii::runtime::hw {

static uint32_t ai_cr = 0;
static uint32_t ai_vr = 0;
static uint32_t ai_scnt = 0;
static uint32_t ai_it = 0;
static uint64_t ai_start_tb = 0;

// __AI_SRC_INIT times one sample against the guest timebase. A wall-clock
// counter does not match that clock (inst_count unless NWII_WALLTB), so the
// measured period never lands in the 48 kHz window and the init retries
// forever. 48 kHz is tb_freq/48000 ticks, 1265 on a 243 MHz bus, inside the
// window the game accepts (0..1732).
static uint32_t ai_samples_now() {
    uint32_t cur = ai_scnt;
    if ((ai_cr & 0x21) && nwii::runtime::g_ctx_ptr) {
        CPUContext* ctx = nwii::runtime::g_ctx_ptr;
        uint64_t freq = ctx->tb_freq ? ctx->tb_freq : 60750000ull;
        uint64_t step = freq / 48000ull;
        if (step == 0) step = 1;
        uint64_t elapsed = ctx->read_timebase() - ai_start_tb;
        cur = ai_scnt + (uint32_t)(elapsed / step);
        if (ai_it > 0 && cur >= ai_it) {
            ai_cr |= 0x04;
            if (ai_cr & 0x02) trigger_pi_interrupt(0x20);
        }
    }
    return cur;
}

static void ai_latch_start() {
    if (nwii::runtime::g_ctx_ptr)
        ai_start_tb = nwii::runtime::g_ctx_ptr->read_timebase();
}

void register_ai(MMIODispatcher& dispatcher) {{
    dispatcher.register_region(0xCC006C00, 0xCC006CFF,
        [](uint32_t addr) -> uint32_t {
            if (addr == 0xCC006C00) return ai_cr;
            if (addr == 0xCC006C04) return ai_vr;
            if (addr == 0xCC006C08) return ai_samples_now();
            if (addr == 0xCC006C0C) return ai_it;
            return 0;
        },
        [](uint32_t addr, uint32_t val) {
            if (addr == 0xCC006C00) {
                if (val & 0x04) { ai_cr &= ~0x04; clear_pi_interrupt(0x20); }
                bool was_run = (ai_cr & 0x21) != 0;
                if (val & 0x08) ai_scnt = 0;
                bool rising = (val & 0x20) && !(ai_cr & 0x20);
                ai_cr = (ai_cr & 0x04) | (val & ~0x04);
                bool now_run = (ai_cr & 0x21) != 0;
                if ((val & 0x08) || rising || (now_run && !was_run))
                    ai_latch_start();
            } else if (addr == 0xCC006C04) {
                ai_vr = val;
            } else if (addr == 0xCC006C08) {
                ai_scnt = val;
                ai_latch_start();
            } else if (addr == 0xCC006C0C) {
                ai_it = val;
            }
        }
    );
}}

} 
namespace nwii::runtime::hw {
void ai_update() {
    (void)ai_samples_now();
}
}
