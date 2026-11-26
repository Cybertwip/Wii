#include "runtime/devices.h"
#include <iostream>

namespace nwii::runtime::devices {

class STMDevice : public IDevice {
public:
    const char* get_name() const override { return "/dev/stm"; }
    bool matches_path(const std::string& path) const override {
        return path.find("/dev/stm") == 0;
    }
    
    int32_t ioctl(CPUContext& ctx, const IpcRequest& req) override {
        std::cout << "[STM] ioctl cmd=" << req.ioctl_cmd << std::endl;
        // 0x1000 registers the event hook, 0x2001 releases it. Boot issues
        // the register call through synchronous IOS and waits on the reply.
        // Holding the request until a power button press leaves that thread
        // in OSSleepThread and the screen stays black. Success means the
        // hook is registered; a later button event can post its own reply.
        (void)ctx;
        return IPC_OK;
    }
    int32_t ioctlv(CPUContext& ctx, const IpcRequest& req) override {
        std::cout << "[STM] ioctlv cmd=" << req.ioctl_cmd << std::endl;
        return IPC_OK;
    }
};

std::unique_ptr<IDevice> create_stm_device() {
    return std::make_unique<STMDevice>();
}

} 
