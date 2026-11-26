// Ported from PowerEngine/Axmol; see UPSTREAM.json and LICENSE.
#include "GipGamepad.h"

#include <algorithm>

namespace nwii::gip
{
namespace
{
uint16_t word(const uint8_t* value) { return uint16_t(value[0]) | uint16_t(uint16_t(value[1]) << 8); }
int16_t signedWord(const uint8_t* value) { return static_cast<int16_t>(word(value)); }
}

Packet decode(const uint8_t* bytes, size_t size, State& state)
{
    if (!bytes || size < 4 || size < size_t(bytes[3]) + 4) return Packet::Invalid;
    // Chunked/secondary-client packets are not a complete gamepad report.
    if ((bytes[1] & 0xC0) || (bytes[1] & 0x0F)) return Packet::Other;
    switch (bytes[0])
    {
    case 0x02: return Packet::Announce;
    case 0x20:
        if (bytes[3] < 14) return Packet::Invalid;
        state.buttons = word(bytes + 4);
        state.leftTrigger = word(bytes + 6);
        state.rightTrigger = word(bytes + 8);
        state.leftX = signedWord(bytes + 10);
        state.leftY = signedWord(bytes + 12); // Npad uses positive Y for up.
        state.rightX = signedWord(bytes + 14);
        state.rightY = signedWord(bytes + 16); // Preserve GIP packet Y polarity.
        return Packet::Input;
    case 0x07:
        if (bytes[3] < 1) return Packet::Invalid;
        state.guide = (bytes[4] & 1) != 0;
        return Packet::Guide;
    default: return Packet::Other;
    }
}

size_t acknowledgement(const uint8_t* bytes, size_t size, uint8_t sequence, std::array<uint8_t, 13>& out)
{
    if (!bytes || size < 4 || !(bytes[1] & 0x10)) return 0;
    out = {0x01, 0x20, sequence, 0x09, 0x00, bytes[0], bytes[1], bytes[2], bytes[3], 0, 0, 0, 0};
    return out.size();
}

float stickValue(int16_t value) { return value < 0 ? value / 32768.f : value / 32767.f; }
float triggerValue(uint16_t value) { return std::min(value, uint16_t(1023)) / 1023.f; }

std::array<uint8_t, 13> rumblePacket(uint16_t low, uint16_t high, uint8_t sequence)
{
    // Wired GIP command 0x09: enable four motors, leave trigger motors off, set the two main motors to 0..100.
    return {0x09, 0, sequence, 0x09, 0, 0x0F, 0, 0,
            uint8_t(uint32_t(low) * 100 / 65535), uint8_t(uint32_t(high) * 100 / 65535), 0xFF, 0, 0xEB};
}

const char* productName(uint16_t vendor, uint16_t product)
{
    if (vendor == 0x0E6F) return "PDP Xbox GIP Controller";
    if (vendor == 0x045E)
    {
        switch (product)
        {
        case 0x02D1: case 0x02DD: case 0x02E3: case 0x02EA:
        case 0x0B00: case 0x0B12: case 0x0B13: return "Microsoft Xbox GIP Controller";
        default: return nullptr;
        }
    }
    if (vendor == 0x24C6 && (product == 0x541A || product == 0x542A)) return "PowerA Xbox GIP Controller";
    return nullptr;
}
}  // namespace nwii::gip
