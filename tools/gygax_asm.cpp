#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "gygax/toy/decb.hpp"

namespace {

struct InstrDef {
    int immOp = -1;
    int extOp = -1;
    int relOp = -1;
    int inhOp = -1;
};

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

const std::map<std::string, InstrDef>& table6809() {
    static const std::map<std::string, InstrDef> t = {
        {"LDA", {0x86, 0xB6, -1, -1}}, {"LDB", {0xC6, 0xF6, -1, -1}}, {"STA", {-1, 0xB7, -1, -1}},  {"STB", {-1, 0xF7, -1, -1}},
        {"LDX", {0x8E, -1, -1, -1}},   {"STX", {-1, 0xBF, -1, -1}},   {"JMP", {-1, 0x7E, -1, -1}},  {"JSR", {-1, 0xBD, -1, -1}},
        {"BRA", {-1, -1, 0x20, -1}},   {"BEQ", {-1, -1, 0x27, -1}},   {"BNE", {-1, -1, 0x26, -1}},  {"NOP", {-1, -1, -1, 0x12}},
        {"INCA", {-1, -1, -1, 0x4C}},  {"INCB", {-1, -1, -1, 0x5C}},  {"DECA", {-1, -1, -1, 0x4A}}, {"DECB", {-1, -1, -1, 0x5A}},
        {"HALT", {-1, -1, -1, 0x3F}},
    };
    return t;
}

const std::map<std::string, InstrDef>& table6502() {
    static const std::map<std::string, InstrDef> t = {
        {"LDA", {0xA9, 0xAD, -1, -1}}, {"LDX", {0xA2, -1, -1, -1}}, {"LDY", {0xA0, -1, -1, -1}}, {"STA", {-1, 0x8D, -1, -1}},
        {"JMP", {-1, 0x4C, -1, -1}},   {"BEQ", {-1, -1, 0xF0, -1}}, {"BNE", {-1, -1, 0xD0, -1}}, {"NOP", {-1, -1, -1, 0xEA}},
        {"INX", {-1, -1, -1, 0xE8}},   {"INY", {-1, -1, -1, 0xC8}}, {"DEX", {-1, -1, -1, 0xCA}}, {"DEY", {-1, -1, -1, 0x88}},
        {"HALT", {-1, -1, -1, 0x00}},
    };
    return t;
}

struct Line {
    std::string label, mnemonic, operand;
    int lineNo = 0;
};

struct AsmError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Assembler {
public:
    Assembler(std::string cpu) : cpu_(std::move(cpu)) {}

    void assemble(const std::vector<std::string>& rawLines) {
        parse(rawLines);
        passOne();
        passTwo();
    }

    const std::vector<uint8_t>& code() const { return code_; }
    uint16_t loadAddr() const { return loadAddr_; }
    uint16_t execAddr() const { return execAddr_; }
    const std::string& cpu() const { return cpu_; }

private:
    std::string cpu_;
    std::vector<Line> lines_;
    std::map<std::string, int32_t> symbols_;
    uint16_t loadAddr_ = 0;
    uint16_t execAddr_ = 0;
    bool haveOrg_ = false;
    std::vector<uint8_t> code_;

    static bool isInherentDirective(const std::string& m) { return m == "ORG" || m == "EQU" || m == "FCB" || m == "END"; }

    void parse(const std::vector<std::string>& rawLines) {
        int lineNo = 0;
        for (auto raw : rawLines) {
            ++lineNo;
            auto semi = raw.find(';');
            if (semi != std::string::npos) raw = raw.substr(0, semi);
            raw = trim(raw);
            if (raw.empty()) continue;
            if (raw.rfind(".cpu", 0) == 0 || raw.rfind(".CPU", 0) == 0) {
                cpu_ = upper(trim(raw.substr(4)));
                continue;
            }

            Line l;
            l.lineNo = lineNo;

            std::vector<std::string> toks;
            std::string tok;
            std::istringstream tokenizer(raw);
            while (tokenizer >> tok) toks.push_back(tok);
            if (toks.empty()) continue;

            size_t idx = 0;
            if (!toks[0].empty() && toks[0].back() == ':') {
                l.label = toks[0].substr(0, toks[0].size() - 1);
                idx = 1;
            } else {
                std::string maybeMnemonic = upper(toks[0]);
                bool knownOp = table(cpu_).count(maybeMnemonic) || isInherentDirective(maybeMnemonic);
                if (!knownOp) {
                    l.label = toks[0];
                    idx = 1;
                }
            }
            if (idx < toks.size()) {
                l.mnemonic = upper(toks[idx]);
                ++idx;
            }
            if (idx < toks.size()) {
                std::string operand;
                for (size_t k = idx; k < toks.size(); ++k) {
                    if (k > idx) operand += ' ';
                    operand += toks[k];
                }
                l.operand = operand;
            }
            lines_.push_back(l);
        }
    }

    const std::map<std::string, InstrDef>& table(const std::string& cpu) const { return cpu == "6502" ? table6502() : table6809(); }

    int32_t evalExpr(const std::string& e, int lineNo, bool duringPassOne) const {
        std::string s = trim(e);
        if (s.empty()) throw AsmError("line " + std::to_string(lineNo) + ": empty expression");
        char sign = 0;
        size_t splitPos = std::string::npos;
        for (size_t i = 1; i < s.size(); ++i) {
            if (s[i] == '+' || s[i] == '-') {
                splitPos = i;
                sign = s[i];
                break;
            }
        }
        std::string base = splitPos == std::string::npos ? s : s.substr(0, splitPos);
        std::string offStr = splitPos == std::string::npos ? "" : s.substr(splitPos + 1);
        int32_t baseVal = evalAtom(base, lineNo, duringPassOne);
        if (splitPos == std::string::npos) return baseVal;
        int32_t offVal = evalAtom(offStr, lineNo, duringPassOne);
        return sign == '+' ? baseVal + offVal : baseVal - offVal;
    }

    int32_t evalAtom(const std::string& s, int lineNo, bool duringPassOne) const {
        if (s.empty()) throw AsmError("line " + std::to_string(lineNo) + ": bad expression");
        if (s[0] == '$') return static_cast<int32_t>(std::stoul(s.substr(1), nullptr, 16));
        if (std::isdigit(static_cast<unsigned char>(s[0]))) return static_cast<int32_t>(std::stoul(s, nullptr, 10));
        auto it = symbols_.find(s);
        if (it == symbols_.end()) {
            if (duringPassOne) return 0;
            throw AsmError("line " + std::to_string(lineNo) + ": undefined symbol '" + s + "'");
        }
        return it->second;
    }

    void passOne() {
        int32_t addr = 0;
        for (auto& l : lines_) {
            if (l.mnemonic == "ORG") {
                addr = evalExpr(l.operand, l.lineNo, true);
                if (!haveOrg_) {
                    loadAddr_ = static_cast<uint16_t>(addr);
                    haveOrg_ = true;
                }
                if (!l.label.empty()) symbols_[l.label] = addr;
                continue;
            }
            if (!l.label.empty() && l.mnemonic != "EQU") {
                symbols_[l.label] = addr;
            }
            if (l.mnemonic == "EQU") {
                symbols_[l.label] = evalExpr(l.operand, l.lineNo, false);
                continue;
            }
            if (l.mnemonic == "FCB") {
                size_t n = 1 + std::count(l.operand.begin(), l.operand.end(), ',');
                addr += static_cast<int32_t>(n);
                continue;
            }
            if (l.mnemonic == "END") continue;
            if (l.mnemonic.empty()) continue;

            auto it = table(cpu_).find(l.mnemonic);
            if (it == table(cpu_).end()) {
                throw AsmError("line " + std::to_string(l.lineNo) + ": unknown mnemonic '" + l.mnemonic + "'");
            }
            addr += instrSize(it->second, l);
        }
    }

    int instrSize(const InstrDef& def, const Line& l) const {
        bool isImm = !l.operand.empty() && l.operand[0] == '#';
        if (isImm && def.immOp >= 0) return 2;
        if (def.relOp >= 0) return 2;
        if (def.inhOp >= 0 && l.operand.empty()) return 1;
        if (def.extOp >= 0) return 3;
        throw AsmError("line " + std::to_string(l.lineNo) + ": no valid addressing mode for '" + l.mnemonic + "'");
    }

    void passTwo() {
        int32_t addr = loadAddr_;
        std::string startLabel;
        for (auto& l : lines_) {
            if (l.mnemonic == "ORG") {
                addr = evalExpr(l.operand, l.lineNo, false);
                continue;
            }
            if (l.mnemonic == "EQU") continue;
            if (l.mnemonic == "END") {
                if (!l.operand.empty()) startLabel = l.operand;
                continue;
            }
            if (l.mnemonic == "FCB") {
                std::istringstream iss(l.operand);
                std::string part;
                while (std::getline(iss, part, ',')) {
                    int32_t v = evalExpr(trim(part), l.lineNo, false);
                    code_.push_back(static_cast<uint8_t>(v & 0xFF));
                }
                addr += static_cast<int32_t>(code_.size());
                continue;
            }
            if (l.mnemonic.empty()) continue;

            const auto& def = table(cpu_).at(l.mnemonic);
            bool isImm = !l.operand.empty() && l.operand[0] == '#';
            if (isImm && def.immOp >= 0) {
                int32_t v = evalExpr(l.operand.substr(1), l.lineNo, false);
                code_.push_back(static_cast<uint8_t>(def.immOp));
                code_.push_back(static_cast<uint8_t>(v & 0xFF));
                addr += 2;
            } else if (def.relOp >= 0) {
                int32_t target = evalExpr(l.operand, l.lineNo, false);
                int32_t nextPc = addr + 2;
                int32_t disp = target - nextPc;
                if (disp < -128 || disp > 127) {
                    throw AsmError("line " + std::to_string(l.lineNo) + ": branch out of range");
                }
                code_.push_back(static_cast<uint8_t>(def.relOp));
                code_.push_back(static_cast<uint8_t>(disp & 0xFF));
                addr += 2;
            } else if (def.inhOp >= 0 && l.operand.empty()) {
                code_.push_back(static_cast<uint8_t>(def.inhOp));
                addr += 1;
            } else if (def.extOp >= 0) {
                int32_t v = evalExpr(l.operand, l.lineNo, false);
                code_.push_back(static_cast<uint8_t>(def.extOp));
                if (cpu_ == "6502") {
                    code_.push_back(static_cast<uint8_t>(v & 0xFF));
                    code_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
                } else {
                    code_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
                    code_.push_back(static_cast<uint8_t>(v & 0xFF));
                }
                addr += 3;
            } else {
                throw AsmError("line " + std::to_string(l.lineNo) + ": no valid addressing mode for '" + l.mnemonic + "'");
            }
        }
        execAddr_ = startLabel.empty() ? loadAddr_ : static_cast<uint16_t>(symbols_.at(startLabel));
    }
};

}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: gygax_asm <input.asm> -o <output.dsk> [--cpu 6809|6502] [--name NAME]\n";
        return 1;
    }
    std::string input, output = "out.dsk", cpu = "6809", name = "PROGRAM.BIN";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc)
            output = argv[++i];
        else if (a == "--cpu" && i + 1 < argc)
            cpu = argv[++i];
        else if (a == "--name" && i + 1 < argc)
            name = argv[++i];
        else if (input.empty())
            input = a;
    }
    if (input.empty()) {
        std::cerr << "gygax_asm: no input file given\n";
        return 1;
    }

    std::ifstream in(input);
    if (!in) {
        std::cerr << "gygax_asm: cannot open " << input << "\n";
        return 1;
    }
    std::vector<std::string> lines;
    std::string ln;
    while (std::getline(in, ln)) lines.push_back(ln);

    try {
        Assembler asmr(upper(cpu));
        asmr.assemble(lines);
        auto payload = gygax::toy::decb::makeBinaryPayload(asmr.code(), asmr.loadAddr(), asmr.execAddr());
        gygax::toy::decb::writeDsk(output, name, payload);
        std::cout << "gygax_asm: assembled " << input << " [" << asmr.cpu() << "] -> " << output << "  (" << asmr.code().size()
                  << " bytes @ $" << std::hex << asmr.loadAddr() << ", exec $" << asmr.execAddr() << std::dec << ")\n";
    } catch (const std::exception& e) {
        std::cerr << "gygax_asm: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
