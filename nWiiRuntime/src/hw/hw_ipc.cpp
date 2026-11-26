#include "runtime/hw/hw.h"
#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include "runtime/event_scheduler.h"
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <deque>

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
static std::deque<uint32_t> pending_replies;

// The ARM mailbox holds exactly one reply until PPC acknowledges Y1.
static void publish_reply() {
    if ((ipc_ppc_ctrl & 0x04) || pending_replies.empty()) return;
    ipc_arm_msg = pending_replies.front();
    pending_replies.pop_front();
    ipc_ppc_ctrl |= 0x04;
    ipc_arm_ctrl = 0x02;
    nwii::runtime::g_ipc_interrupt_delay = 50;
}

static void queue_reply(uint32_t req_addr) {
    pending_replies.push_back(req_addr & 0x1FFFFFFF);
    publish_reply();
}

extern "C" int32_t handle_ios_ipc(nwii::runtime::CPUContext& ctx, uint32_t request_addr);

int32_t dispatch_ipc(CPUContext& ctx, uint32_t virt_addr) {
  return handle_ios_ipc(ctx, virt_addr);
}

int32_t ipc_dispatch_request(CPUContext &ctx, uint32_t req_addr) {
  uint32_t virt_addr = req_addr;
  if ((virt_addr & 0xF0000000) == 0xC0000000) virt_addr = (virt_addr & 0x0FFFFFFF) | 0x80000000;
  if ((virt_addr & 0xF0000000) == 0xD0000000) virt_addr = (virt_addr & 0x0FFFFFFF) | 0x90000000;

  return dispatch_ipc(ctx, virt_addr);
}

void hle_set_ipc_arm_msg(uint32_t req_addr) {
  queue_reply(req_addr);
}

void ipc_post_reply(uint32_t req_addr) {
  queue_reply(req_addr);
}

// A DI read sets g_di_reply_ticks to model real disc transfer/seek latency.
// The PPC mailbox is freed immediately (Y2) so the SDK can queue the next
// command, but the completion reply (Y1 + IPC interrupt) is deferred by that
// many timebase ticks. Instant completion lets the content loader reuse a
// command-block slot while the previous entry still references it, so the
// completion callback matches the stale entry and the waited entry stalls.
uint64_t g_di_reply_ticks = 0;

void ipc_schedule_di_reply(uint32_t req_addr, uint64_t ticks) {
  nwii::runtime::EventScheduler::get().schedule_after(
      ticks, [req_addr](CPUContext&, uint64_t) {
        queue_reply(req_addr);
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
                // Acknowledge existing events before handling a new request.
                ipc_ppc_ctrl = (ipc_ppc_ctrl & ~0x30) | (val & 0x30);
                if (val & 0x04) ipc_ppc_ctrl &= ~0x04;
                if (val & 0x02) ipc_ppc_ctrl &= ~0x02;
                if (val & 0x08) ipc_ppc_ctrl &= ~0x08;

                if (val & 0x01) {
                    ipc_ppc_ctrl |= 0x01;
                    const uint32_t request = ipc_ppc_msg;
                    int32_t result = g_ctx_ptr
                        ? ipc_dispatch_request(*g_ctx_ptr, request) : -8;
                    std::cout << "[IPC] Request sent from PPC! result=" << result << "\n";
                    ipc_ppc_ctrl &= ~0x01;
                    ipc_ppc_ctrl |= 0x02;
                    g_ipc_interrupt_delay = 50;
                    if (result != -0x70000001) {
                        if (g_di_reply_ticks > 0) {
                            ipc_schedule_di_reply(request, g_di_reply_ticks);
                            g_di_reply_ticks = 0;
                        } else {
                            queue_reply(request);
                        }
                    }
                }
                publish_reply();
                break;
            }

            case 0x000030:
                if (val & 0x4000) {
                    clear_pi_interrupt(0x4000);
                    if (((ipc_ppc_ctrl & 0x14) == 0x14) ||
                        ((ipc_ppc_ctrl & 0x22) == 0x22))
                        g_ipc_interrupt_delay = 50;
                }
                break;
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
