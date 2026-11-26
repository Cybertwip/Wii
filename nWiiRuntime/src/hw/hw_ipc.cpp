#include "runtime/hw/hw.h"
#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include "runtime/event_scheduler.h"
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace nwii::runtime {
    extern CPUContext* g_ctx_ptr;
    extern MMU* g_mmu;
    extern int g_ipc_interrupt_delay;
}

namespace nwii::runtime::hw {

static uint32_t ipc_arm_msg = 0;
static uint32_t ipc_arm_ctrl = 0;
static uint32_t ipc_ppc_ctrl = 0;
uint32_t ipc_ppc_msg = 0;

extern "C" int32_t handle_ios_ipc(nwii::runtime::CPUContext& ctx, uint32_t request_addr);

int32_t dispatch_ipc(CPUContext& ctx, uint32_t virt_addr) {
  int32_t result = handle_ios_ipc(ctx, virt_addr);

  ipc_arm_msg  = virt_addr & 0x1FFFFFFF;
  ipc_arm_ctrl = 0x00000003; 
  
  return result;
}

int32_t ipc_dispatch_request(CPUContext &ctx, uint32_t req_addr) {
  uint32_t virt_addr = req_addr;
  if ((virt_addr & 0xF0000000) == 0xC0000000) virt_addr = (virt_addr & 0x0FFFFFFF) | 0x80000000;
  if ((virt_addr & 0xF0000000) == 0xD0000000) virt_addr = (virt_addr & 0x0FFFFFFF) | 0x90000000;

  return dispatch_ipc(ctx, virt_addr);
}

void hle_set_ipc_arm_msg(uint32_t req_addr) {
  ipc_arm_msg = req_addr & 0x1FFFFFFF;
  ipc_arm_ctrl = 0x00000002;
  ipc_ppc_ctrl |= 0x00000004;
  g_ipc_interrupt_delay = 5000;
}



void ipc_post_reply(uint32_t req_addr) {
  ipc_arm_msg = req_addr & 0x1FFFFFFF;
  ipc_ppc_ctrl &= ~0x01;
  ipc_ppc_ctrl |= 0x26; 
  g_ipc_interrupt_delay = 50;
}

// A DI read sets g_di_reply_ticks to model real disc transfer/seek latency.
// The PPC mailbox is freed immediately (Y2) so the SDK can queue the next
// command, but the completion reply (Y1 + IPC interrupt) is deferred by that
// many timebase ticks. Instant completion lets the content loader reuse a
// command-block slot while the previous entry still references it, so the
// completion callback matches the stale entry and the waited entry stalls.
uint64_t g_di_reply_ticks = 0;

void ipc_schedule_di_reply(uint64_t ticks) {
  uint32_t arm = ipc_arm_msg;
  nwii::runtime::EventScheduler::get().schedule_after(
      ticks, [arm](CPUContext&, uint64_t) {
        ipc_arm_msg = arm;
        ipc_ppc_ctrl &= ~0x01;
        ipc_ppc_ctrl |= 0x26;
        trigger_pi_interrupt(0x00004000);
      });
}

void register_ipc(MMIODispatcher& dispatcher) {{
    dispatcher.register_region(0xCD000000, 0xCD00FFFF, 
        [](uint32_t addr) -> uint32_t {
            switch (addr & 0x00FFFFFF) {
            case 0x000000: return ipc_ppc_msg;
            case 0x000004: return ipc_ppc_ctrl;
            case 0x000008: {
                if (std::getenv("NWII_IPCDBG")) {
                    uint32_t m = ipc_arm_msg;
                    uint32_t cmd = 0, st = 0;
                    if (nwii::runtime::g_ctx_ptr && m >= 0x80000000u && m < 0x94000000u) {
                        cmd = nwii::runtime::g_ctx_ptr->mmu.read32(m);
                        st = nwii::runtime::g_ctx_ptr->mmu.read32(m + 12);
                    }
                    std::cout << "[IPCDBG] arm_msg read=0x" << std::hex << m
                              << " cmd=" << cmd << " blkstate=" << std::dec << st
                              << " pc=0x" << std::hex
                              << (nwii::runtime::g_ctx_ptr ? nwii::runtime::g_ctx_ptr->pc : 0)
                              << std::dec << "\n";
                }
                return ipc_arm_msg;
            }
            case 0x00000C: return ipc_arm_ctrl;
            default: return 0;
            }
        },
        [](uint32_t addr, uint32_t val) {
            switch (addr & 0x00FFFFFF) {
            case 0x000000: {
                ipc_ppc_msg = val;
                break;
            }
            case 0x000004: {
                uint32_t old_ctrl = ipc_ppc_ctrl;

                ipc_ppc_ctrl = (ipc_ppc_ctrl & ~0x30) | (val & 0x30);

                if (val & 0x01) {
                    
                    ipc_ppc_ctrl |= 0x01;
                    int32_t result = 0;
                    if (g_ctx_ptr) {
                        
                        result = ipc_dispatch_request(*g_ctx_ptr, ipc_ppc_msg);
                    }
                    std::cout << "[IPC] Request sent from PPC! result=" << result << "\n";
                    if (result != -0x70000001) {
                        ipc_ppc_ctrl &= ~0x01;
                        if (g_di_reply_ticks > 0) {
                            // Free the mailbox now; defer the completion reply
                            // (Y1 + interrupt) to model disc latency.
                            ipc_ppc_ctrl |= 0x22;
                            ipc_schedule_di_reply(g_di_reply_ticks);
                            g_di_reply_ticks = 0;
                        } else {
                            ipc_ppc_ctrl |= 0x26;
                            g_ipc_interrupt_delay = 50;
                        }
                    } else {
                        // The result word stays unset and ipc_post_reply()
                        // raises Y1 later. Y2 alone frees the PPC mailbox.
                        // The client will not send the next queued request
                        // until Y2, so a pending USB read must not hold it.
                        // Do not set Y1 here: that delivers a completion for
                        // a request that is still pending.
                        ipc_ppc_ctrl &= ~0x01;
                        ipc_ppc_ctrl |= 0x22;
                        g_ipc_interrupt_delay = 50;
                        std::cout << "[IPC] NO_REPLY, mailbox free (Y2)\n";
                    }
                }

                if (val & 0x04) ipc_ppc_ctrl &= ~0x04; 
                if (val & 0x02) ipc_ppc_ctrl &= ~0x02; 
                if (val & 0x08) ipc_ppc_ctrl &= ~0x08; 
                (void)old_ctrl;
                break;
            }

            case 0x00000C: {
                ipc_arm_ctrl &= ~(val & 0x03);
                if (!(ipc_arm_ctrl & 3)) {
                    if (std::getenv("NWII_SAMPLE") && (pi_intsr & 0x4000)) {
                        static int n = 0;
                        if (n++ < 6)
                            std::cout << "[IPC] armctrl write 0x" << std::hex
                                      << val << " swallows pending irq\n" << std::dec;
                    }
                    clear_pi_interrupt(0x00004000);
                }
                break;
            }
            } 
        }
    );
    dispatcher.register_region(0x0D000000, 0x0D00FFFF, 
        [&dispatcher](uint32_t a) { return dispatcher.read32(a | 0xC0000000); },
        [&dispatcher](uint32_t a, uint32_t v) { dispatcher.write32(a | 0xC0000000, v); }
    );
}}

} 
