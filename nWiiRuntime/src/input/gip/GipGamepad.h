// Ported from PowerEngine/Axmol; see UPSTREAM.json and LICENSE.
#pragma once


#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nwii::gip
{
// Xbox One/Series wired Game Input Protocol. Protocol layout and PDP setup
// packets were checked against the supplied PSXRecomp and Fiesta drivers.
enum Button : uint16_t
{
    Menu = 0x0004, View = 0x0008, A = 0x0010, B = 0x0020, X = 0x0040, Y = 0x0080,
    Up = 0x0100, Down = 0x0200, Left = 0x0400, Right = 0x0800,
    LeftBumper = 0x1000, RightBumper = 0x2000, LeftStick = 0x4000, RightStick = 0x8000
};

struct State
{
    uint16_t buttons = 0, leftTrigger = 0, rightTrigger = 0;
    int16_t leftX = 0, leftY = 0, rightX = 0, rightY = 0;
    bool guide = false;
};

enum class Packet { Invalid, Other, Announce, Input, Guide };
Packet decode(const uint8_t* bytes, size_t size, State& state);
size_t acknowledgement(const uint8_t* bytes, size_t size, uint8_t sequence, std::array<uint8_t, 13>& out);
float stickValue(int16_t value);
float triggerValue(uint16_t value);
const char* productName(uint16_t vendor, uint16_t product);
std::array<uint8_t, 13> rumblePacket(uint16_t low, uint16_t high, uint8_t sequence);

// SDL callbacks only publish a request. The USB reader serializes motor commands with protocol acknowledgements.
struct Output
{
    std::atomic<bool> connected{false};
    std::atomic<uint64_t> rumble{0}; // upper 32 bits: request generation; lower 32 bits: two 16-bit amplitudes
    bool setRumble(uint16_t low, uint16_t high)
    {
        if (!connected.load()) return false;
        uint64_t previous = rumble.load();
        for (;;)
        {
            const uint64_t next = ((previous + (uint64_t(1) << 32)) & 0xFFFFFFFF00000000ULL) |
                                  uint64_t(low) | (uint64_t(high) << 16);
            if (rumble.compare_exchange_weak(previous, next)) return true;
        }
    }
};

struct Device
{
    std::string path, name;
    uint16_t vendor = 0, product = 0;
    std::shared_ptr<Output> output = std::make_shared<Output>();
};

struct Event
{
    Device device;
    State state;
    bool connected = false;
    bool report = false;
};

// The discovery thread and per-device USB readers never invoke engine events.
// Consumers drain complete state transitions on the Axmol thread.
class USBHub
{
public:
    USBHub();
    ~USBHub();
    void start();
    void stop();
    std::vector<Event> drain();
    static bool available();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace nwii::gip
