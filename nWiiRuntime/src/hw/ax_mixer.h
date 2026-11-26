#pragma once

#include "runtime/cpu_context.h"
#include <array>
#include <algorithm>
#include <cstdint>
#include <iostream>

namespace nwii::runtime::hw {

// Wii AX command and PB wire layouts checked against Dolphin's AXWii protocol
// documentation in AXWii.cpp/AXStructs.h. The mixer below is implemented locally.
class AxMixer {
    static constexpr unsigned frames = 96;
    using Channel = std::array<int64_t, frames>;
    std::array<Channel, 12> mix{};
    std::array<uint16_t, 4> last_volume{0x8000, 0x8000, 0x8000, 0x8000};

    static int16_t clip(int64_t v) {
        return static_cast<int16_t>(std::clamp<int64_t>(v, -32768, 32767));
    }
    static uint8_t* range(MMU& m, uint32_t address, uint32_t size) {
        if (!size || address > UINT32_MAX - (size - 1)) return nullptr;
        auto* first = m.get_ptr(address);
        auto* last = m.get_ptr(address + size - 1);
        return first && last && reinterpret_cast<uintptr_t>(last) - reinterpret_cast<uintptr_t>(first) == size - 1 ? first : nullptr;
    }
    static uint16_t word(const uint8_t* p, unsigned w) {
        return (uint16_t(p[w * 2]) << 8) | p[w * 2 + 1];
    }
    static void set(uint8_t* p, unsigned w, uint16_t v) {
        p[w * 2] = v >> 8;
        p[w * 2 + 1] = v;
    }
    static uint32_t pair(const uint8_t* p, unsigned w) {
        return (uint32_t(word(p, w)) << 16) | word(p, w + 1);
    }
    static void set_pair(uint8_t* p, unsigned w, uint32_t v) {
        set(p, w, v >> 16);
        set(p, w + 1, v);
    }
    static void store_channel(MMU& m, uint32_t addr, const Channel& channel) {
        if (auto* p = range(m, addr, frames * 4)) {
            for (unsigned i = 0; i < frames; ++i)
                set_pair(p, 2 * i, uint32_t(std::clamp<int64_t>(channel[i], INT32_MIN, INT32_MAX)));
        }
    }
    static uint8_t sample_byte(MMU& m, uint32_t a) {
        auto* p = range(m, a, 1);
        return p ? *p : 0;
    }

    void voice(MMU& m, uint8_t* pb) {
        // Modern AX Wii PB (without the obsolete 1ms update table).
        constexpr unsigned envelope = 53, format = 56, loop = 57, end = 59,
            current = 61, coefficients = 63, predictor = 80, history = 81,
            ratio = 83, fraction = 85, src_history = 86, loop_state = 90;
        uint32_t position = pair(pb, current);
        const uint32_t finish = pair(pb, end), restart = pair(pb, loop);
        const uint16_t encoding = word(pb, format);
        if (encoding != 0 && encoding != 0xa && encoding != 0x19) return;
        bool running = true;
        int16_t yn1 = word(pb, history), yn2 = word(pb, history + 1);
        uint16_t pred = word(pb, predictor);
        auto decode = [&]() -> int16_t {
            if (!running) return 0;
            int16_t value = 0;
            if (encoding == 0) {
                if ((position & 15) < 2) {
                    pred = sample_byte(m, position / 2);
                    position = (position & ~15u) + 2;
                }
                uint8_t packed = sample_byte(m, position / 2);
                int nibble = (position & 1) ? packed & 15 : packed >> 4;
                if (nibble >= 8) nibble -= 16;
                const unsigned index = ((pred >> 4) & 7) * 2;
                const int64_t predicted = int64_t(int16_t(word(pb, coefficients + index))) * yn1 +
                    int64_t(int16_t(word(pb, coefficients + index + 1))) * yn2;
                value = clip((predicted + int64_t(nibble) * (1 << (pred & 15)) * 2048 + 1024) >> 11);
                yn2 = yn1;
                yn1 = value;
            } else if (encoding == 0xa) {
                if (position <= UINT32_MAX / 2) {
                    if (auto* p = range(m, position * 2, 2)) value = int16_t(word(p, 0));
                }
            } else {
                value = int16_t(int8_t(sample_byte(m, position))) * 256;
            }
            if (position >= finish) {
                position = restart;
                if (word(pb, 55)) {
                    if (encoding == 0) {
                        pred = word(pb, loop_state);
                        if (word(pb, 9) != 1) {
                            yn1 = word(pb, loop_state + 1);
                            yn2 = word(pb, loop_state + 2);
                        }
                    }
                } else {
                    running = false;
                }
            } else {
                ++position;
            }
            return value;
        };
        uint32_t phase = word(pb, fraction);
        const uint32_t step = std::min(pair(pb, ratio), 0x40000u);
        std::array<int16_t, 4> recent{};
        for (unsigned i = 0; i < 4; ++i) recent[i] = word(pb, src_history + i);
        uint16_t env = word(pb, envelope);
        const int16_t env_delta = word(pb, envelope + 1);
        const uint32_t control = pair(pb, 6);
        constexpr std::array<unsigned, 12> mixer_slots{0,1,8,2,3,9,4,5,10,6,7,11};
        std::array<uint16_t, 12> volume{};
        std::array<int16_t, 12> delta{};
        std::array<bool, 12> enabled{};
        for (unsigned c = 0; c < 12; ++c) {
            unsigned group = c / 3, channel = c % 3;
            unsigned shift = group == 0 ? 0 : 16 + 5 * (group - 1);
            unsigned flags = (control >> shift) & 31;
            bool ramp = flags & (channel == 2 ? 16 : 4);
            enabled[c] = ramp || (flags & (channel == 2 ? 8 : 1u << channel));
            volume[c] = word(pb, 10 + mixer_slots[c] * 2);
            if (ramp) delta[c] = word(pb, 11 + mixer_slots[c] * 2);
        }
        int16_t lowpass_history = word(pb, 94);
        for (unsigned i = 0; i < frames; ++i) {
            auto advance = [&]() {
                recent[0] = recent[1]; recent[1] = recent[2];
                recent[2] = recent[3]; recent[3] = decode();
            };
            int16_t s;
            if (word(pb, 4) == 2) {
                advance();
                s = recent[3];
            } else {
                phase += step;
                while (phase >= 65536) { advance(); phase -= 65536; }
                s = clip((int64_t(recent[0]) * (65536 - phase) + int64_t(recent[1]) * phase) >> 16);
            }
            s = clip((int64_t(s) * env) >> 15);
            env = uint16_t(env + env_delta);
            if (word(pb, 93)) {
                s = clip((int64_t(s) * word(pb, 95) + int64_t(lowpass_history) * int16_t(word(pb, 96))) >> 15);
                lowpass_history = s;
            }
            for (unsigned c = 0; c < 12; ++c) {
                if (!enabled[c]) continue;
                int16_t contribution = clip((int64_t(s) * volume[c]) >> 15);
                mix[c][i] += contribution;
                volume[c] = uint16_t(volume[c] + delta[c]);
                if (i == frames - 1) set(pb, 41 + (c % 3) * 4 + c / 3, contribution);
            }
        }
        set(pb, 8, running ? 1 : 0);
        set_pair(pb, current, position);
        set(pb, predictor, pred);
        set(pb, history, yn1); set(pb, history + 1, yn2);
        set(pb, fraction, phase);
        for (unsigned i = 0; i < 4; ++i) set(pb, src_history + i, recent[i]);
        set(pb, envelope, env);
        set(pb, 94, lowpass_history);
        for (unsigned c = 0; c < 12; ++c)
            if (enabled[c]) set(pb, 10 + mixer_slots[c] * 2, volume[c]);
    }

public:
    void process(MMU& m, uint32_t address, unsigned bytes) {
        const auto* list = range(m, address, bytes);
        if (!list || bytes > 512 || (bytes & 1)) return;
        unsigned cursor = 0, count = bytes / 2, active = 0;
        while (cursor < count) {
            const unsigned cmd = word(list, cursor++);
            constexpr std::array<unsigned, 15> lengths{2,2,2,2,2,5,5,5,13,13,4,5,5,8,0};
            if (cmd >= lengths.size() || lengths[cmd] > count - cursor) break;
            const auto* args = list + cursor * 2;
            cursor += lengths[cmd];
            if (cmd == 0) {
                for (auto& c : mix) c.fill(0);
            } else if (cmd >= 1 && cmd <= 3) {
                if (auto* p = range(m, pair(args, 0), frames * 4 * (cmd == 3 ? 2 : 1))) {
                    for (unsigned i = 0; i < frames; ++i) {
                        int32_t s = pair(p, i * 2);
                        mix[0][i] += cmd == 2 ? -int64_t(s) : s;
                        mix[1][i] += cmd == 3 ? -int64_t(int32_t(pair(p, (i + frames) * 2))) : s;
                    }
                }
            } else if (cmd == 4) {
                uint32_t pb_addr = pair(args, 0);
                for (unsigned i = 0; pb_addr && i < 256; ++i) {
                    auto* pb = range(m, pb_addr, 97 * 2);
                    if (!pb) break;
                    const uint32_t next = pair(pb, 0);
                    if (word(pb, 8) == 1) { voice(m, pb); ++active; }
                    if (next == pb_addr) break;
                    pb_addr = next;
                }
            } else if (cmd >= 5 && cmd <= 7) {
                unsigned aux = cmd - 4;
                uint32_t send = pair(args, 1), receive = pair(args, 3);
                uint16_t target = word(args, 0);
                for (unsigned c = 0; c < 3; ++c) {
                    if (send) store_channel(m, send + c * frames * 4, mix[aux * 3 + c]);
                    if (auto* p = receive ? range(m, receive + c * frames * 4, frames * 4) : nullptr) {
                        for (unsigned i = 0; i < frames; ++i) {
                            int vol = last_volume[aux] + (int(target) - last_volume[aux]) * int(i + 1) / int(frames);
                            mix[c][i] += (int64_t(int32_t(pair(p, 2 * i))) * vol) >> 15;
                        }
                    }
                }
                last_volume[aux] = target;
            } else if (cmd == 11 || cmd == 12) {
                uint16_t target = word(args, 0);
                store_channel(m, pair(args, 1), mix[2]);
                if (cmd == 12) store_channel(m, pair(args, 1) + frames * 4, mix[9]);
                if (auto* p = range(m, pair(args, 3), frames * 4)) {
                    unsigned nonzero = 0;
                    int peak = 0;
                    for (unsigned i = 0; i < frames; ++i) {
                        int vol = last_volume[0] + (int(target) - last_volume[0]) * int(i + 1) / int(frames);
                        int16_t r = clip((mix[1][i] * vol) >> 15), l = clip((mix[0][i] * vol) >> 15);
                        set(p, i * 2, r); set(p, i * 2 + 1, l);
                        nonzero += (r != 0) + (l != 0);
                        peak = std::max({peak, std::abs(int(r)), std::abs(int(l))});
                    }
                    static unsigned reports = 0;
                    if (std::getenv("NWII_DSPTRACE") && nonzero && (++reports <= 12 || reports % 1000 == 0))
                        std::cout << "[AX] voices=" << active << " PCM nonzero=" << nonzero << " peak=" << peak << " dst=0x" << std::hex << pair(args, 3) << std::dec << '\n';
                }
                last_volume[0] = target;
            } else if (cmd == 13) {
                for (unsigned c = 0; c < 4; ++c)
                    if (auto* p = range(m, pair(args, c * 2), 36)) std::fill_n(p, 36, 0);
            } else if (cmd == 14) {
                break;
            }
        }
    }
};

} // namespace nwii::runtime::hw
