#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <string>

namespace gygax::toy {

class Cpu6809 {
public:
    explicit Cpu6809(std::array<uint8_t, 65536>& mem) : mem_(mem) {}

    uint16_t pc = 0;
    uint16_t x = 0;
    uint8_t a = 0, b = 0;
    uint8_t cc = 0;
    bool halted = false;

    static constexpr uint8_t kZ = 0x04, kN = 0x08;

    void setZN(uint8_t v) { cc = static_cast<uint8_t>((cc & ~(kZ | kN)) | (v == 0 ? kZ : 0) | (v & 0x80 ? kN : 0)); }

    uint8_t fetch8() { return mem_[pc++]; }
    uint16_t fetch16() {
        uint16_t hi = fetch8();
        uint16_t lo = fetch8();
        return static_cast<uint16_t>((hi << 8) | lo);
    }

    bool step() {
        if (halted) return false;
        uint8_t op = fetch8();
        switch (op) {
        case 0x86:
            a = fetch8();
            setZN(a);
            break;
        case 0xC6:
            b = fetch8();
            setZN(b);
            break;
        case 0x96: {
            uint8_t addr = fetch8();
            a = mem_[addr];
            setZN(a);
            break;
        }
        case 0xD6: {
            uint8_t addr = fetch8();
            b = mem_[addr];
            setZN(b);
            break;
        }
        case 0xB6: {
            uint16_t addr = fetch16();
            a = mem_[addr];
            setZN(a);
            break;
        }
        case 0xF6: {
            uint16_t addr = fetch16();
            b = mem_[addr];
            setZN(b);
            break;
        }
        case 0x97: {
            uint8_t addr = fetch8();
            mem_[addr] = a;
            setZN(a);
            break;
        }
        case 0xD7: {
            uint8_t addr = fetch8();
            mem_[addr] = b;
            setZN(b);
            break;
        }
        case 0xB7: {
            uint16_t addr = fetch16();
            mem_[addr] = a;
            setZN(a);
            break;
        }
        case 0xF7: {
            uint16_t addr = fetch16();
            mem_[addr] = b;
            setZN(b);
            break;
        }
        case 0x8E: x = fetch16(); break;
        case 0xBF: {
            uint16_t addr = fetch16();
            mem_[addr] = static_cast<uint8_t>(x >> 8);
            mem_[addr + 1] = static_cast<uint8_t>(x & 0xFF);
            break;
        }
        case 0x4A:
            a = static_cast<uint8_t>(a - 1);
            setZN(a);
            break;
        case 0x5A:
            b = static_cast<uint8_t>(b - 1);
            setZN(b);
            break;
        case 0x4C:
            a = static_cast<uint8_t>(a + 1);
            setZN(a);
            break;
        case 0x5C:
            b = static_cast<uint8_t>(b + 1);
            setZN(b);
            break;
        case 0x1F: fetch8(); break;
        case 0x7E: pc = fetch16(); break;
        case 0xBD: {
            uint16_t addr = fetch16();
            pc = addr;
            break;
        }
        case 0x20: {
            int8_t d = static_cast<int8_t>(fetch8());
            pc = static_cast<uint16_t>(pc + d);
            break;
        }
        case 0x27: {
            int8_t d = static_cast<int8_t>(fetch8());
            if (cc & kZ) pc = static_cast<uint16_t>(pc + d);
            break;
        }
        case 0x26: {
            int8_t d = static_cast<int8_t>(fetch8());
            if (!(cc & kZ)) pc = static_cast<uint16_t>(pc + d);
            break;
        }
        case 0x12: break;
        case 0x3F: halted = true; break;
        default: halted = true; return false;
        }
        return true;
    }

    std::string describe() const { return std::format("PC=${:04X} A=${:02X} B=${:02X} X=${:04X} CC=${:02X}", pc, a, b, x, cc); }

private:
    std::array<uint8_t, 65536>& mem_;
};

}
