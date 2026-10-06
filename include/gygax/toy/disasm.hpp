#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace gygax::toy::disasm {

enum class Mode { Inherent, Immediate, Extended, Relative };

struct OpInfo {
    std::string mnemonic;
    Mode mode;
    int size;
};

inline const std::map<uint8_t, OpInfo>& table6809() {
    static const std::map<uint8_t, OpInfo> t = {
        {0x86, {"LDA", Mode::Immediate, 2}}, {0xC6, {"LDB", Mode::Immediate, 2}}, {0x96, {"LDA", Mode::Extended, 2}},
        {0xD6, {"LDB", Mode::Extended, 2}},  {0xB6, {"LDA", Mode::Extended, 3}},  {0xF6, {"LDB", Mode::Extended, 3}},
        {0x97, {"STA", Mode::Extended, 2}},  {0xD7, {"STB", Mode::Extended, 2}},  {0xB7, {"STA", Mode::Extended, 3}},
        {0xF7, {"STB", Mode::Extended, 3}},  {0x8E, {"LDX", Mode::Immediate, 3}}, {0xBF, {"STX", Mode::Extended, 3}},
        {0x4A, {"DECA", Mode::Inherent, 1}}, {0x5A, {"DECB", Mode::Inherent, 1}}, {0x4C, {"INCA", Mode::Inherent, 1}},
        {0x5C, {"INCB", Mode::Inherent, 1}}, {0x1F, {"TFR", Mode::Immediate, 2}}, {0x7E, {"JMP", Mode::Extended, 3}},
        {0xBD, {"JSR", Mode::Extended, 3}},  {0x20, {"BRA", Mode::Relative, 2}},  {0x27, {"BEQ", Mode::Relative, 2}},
        {0x26, {"BNE", Mode::Relative, 2}},  {0x12, {"NOP", Mode::Inherent, 1}},  {0x3F, {"HALT", Mode::Inherent, 1}},
    };
    return t;
}

inline const std::map<uint8_t, OpInfo>& table6502() {
    static const std::map<uint8_t, OpInfo> t = {
        {0xA9, {"LDA", Mode::Immediate, 2}}, {0xA2, {"LDX", Mode::Immediate, 2}}, {0xA0, {"LDY", Mode::Immediate, 2}},
        {0xA5, {"LDA", Mode::Extended, 2}},  {0xAD, {"LDA", Mode::Extended, 3}},  {0x85, {"STA", Mode::Extended, 2}},
        {0x8D, {"STA", Mode::Extended, 3}},  {0xE8, {"INX", Mode::Inherent, 1}},  {0xC8, {"INY", Mode::Inherent, 1}},
        {0xCA, {"DEX", Mode::Inherent, 1}},  {0x88, {"DEY", Mode::Inherent, 1}},  {0x4C, {"JMP", Mode::Extended, 3}},
        {0xF0, {"BEQ", Mode::Relative, 2}},  {0xD0, {"BNE", Mode::Relative, 2}},  {0xEA, {"NOP", Mode::Inherent, 1}},
        {0x00, {"HALT", Mode::Inherent, 1}},
    };
    return t;
}

}
