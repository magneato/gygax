#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace gygax::toy {

class Cpu6502 {
public:
    explicit Cpu6502(std::array<uint8_t, 65536>& mem) : mem_(mem) {}

    uint16_t pc = 0;
    uint8_t a = 0, x = 0, y = 0;
    uint8_t p = 0;
    bool halted = false;

    static constexpr uint8_t kZ = 0x02, kN = 0x80;

    void setZN(uint8_t v) { p = static_cast<uint8_t>((p & ~(kZ | kN)) | (v == 0 ? kZ : 0) | (v & 0x80)); }

    uint8_t fetch8() { return mem_[pc++]; }
    uint16_t fetch16() {
        uint16_t lo = fetch8();
        uint16_t hi = fetch8();
        return static_cast<uint16_t>((hi << 8) | lo);
    }

    bool step() {
        if (halted) return false;
        uint8_t op = fetch8();
        switch (op) {
        case 0xA9:
            a = fetch8();
            setZN(a);
            break;
        case 0xA2:
            x = fetch8();
            setZN(x);
            break;
        case 0xA0:
            y = fetch8();
            setZN(y);
            break;
        case 0xA5: {
            uint8_t addr = fetch8();
            a = mem_[addr];
            setZN(a);
            break;
        }
        case 0xAD: {
            uint16_t addr = fetch16();
            a = mem_[addr];
            setZN(a);
            break;
        }
        case 0x85: {
            uint8_t addr = fetch8();
            mem_[addr] = a;
            break;
        }
        case 0x8D: {
            uint16_t addr = fetch16();
            mem_[addr] = a;
            break;
        }
        case 0xE8:
            x = static_cast<uint8_t>(x + 1);
            setZN(x);
            break;
        case 0xC8:
            y = static_cast<uint8_t>(y + 1);
            setZN(y);
            break;
        case 0xCA:
            x = static_cast<uint8_t>(x - 1);
            setZN(x);
            break;
        case 0x88:
            y = static_cast<uint8_t>(y - 1);
            setZN(y);
            break;
        case 0x4C: pc = fetch16(); break;
        case 0xF0: {
            int8_t d = static_cast<int8_t>(fetch8());
            if (p & kZ) pc = static_cast<uint16_t>(pc + d);
            break;
        }
        case 0xD0: {
            int8_t d = static_cast<int8_t>(fetch8());
            if (!(p & kZ)) pc = static_cast<uint16_t>(pc + d);
            break;
        }
        case 0xEA: break;
        case 0x00: halted = true; break;
        default: halted = true; return false;
        }
        return true;
    }

    std::string describe() const { return std::format("PC=${:04X} A=${:02X} X=${:02X} Y=${:02X} P=${:02X}", pc, a, x, y, p); }

private:
    std::array<uint8_t, 65536>& mem_;
};

}
