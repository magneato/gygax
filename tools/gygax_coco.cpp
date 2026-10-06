#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

import gygax.core.base;

#include "gygax/toy/cpu6502.hpp"
#include "gygax/toy/cpu6809.hpp"
#include "gygax/toy/decb.hpp"
#include "gygax/toy/disasm.hpp"

namespace {

constexpr uint16_t kScreenBase = 0x0400;
constexpr int kScreenW = 64;
constexpr int kScreenH = 64;
constexpr std::size_t kMemorySize = 1U << 16;
constexpr int kMaxExecutionSteps = 200000;
constexpr int kAnalysisInstructionLimit = 256;
constexpr int kSelfLoopLimit = 2;

struct Rgb {
    int r, g, b;
};

const std::array<Rgb, 9>& palette() {
    static const std::array<Rgb, 9> p = {{
        {0x00, 0x00, 0x00},
        {0xE0, 0x20, 0x20},
        {0x20, 0xC0, 0x40},
        {0xE0, 0xD0, 0x20},
        {0x20, 0x60, 0xE0},
        {0xE0, 0xE0, 0xE0},
        {0x20, 0xC0, 0xC0},
        {0xC0, 0x30, 0xC0},
        {0xE0, 0x90, 0x20},
    }};
    return p;
}

std::string repeatUtf8(const char* glyph, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += glyph;
    return s;
}

void printBanner(const std::string& title, const std::vector<std::string>& fields) {
    std::cout << "\n╔" << repeatUtf8("═", 46) << "╗\n";
    std::string line = "  " + title;
    line.resize(46, ' ');
    std::cout << "║" << line << "║\n";
    for (auto& f : fields) {
        std::string l = "  " + f;
        l.resize(46, ' ');
        std::cout << "║" << l << "║\n";
    }
    std::cout << "╚" << repeatUtf8("═", 46) << "╝\n\n";
}

void summarizeScreen(const std::array<uint8_t, kMemorySize>& mem) {
    int lit = 0;
    for (int i = 0; i < kScreenW * kScreenH; ++i) {
        if (mem[kScreenBase + i] != 0) ++lit;
    }
    std::cout << "framebuffer: " << lit << " of " << kScreenW * kScreenH << " pixels set\n";
}

void renderScreen(const std::array<uint8_t, kMemorySize>& mem) {
    const auto& pal = palette();
    std::cout << "  ┌" << repeatUtf8("─", kScreenW) << "┐\n";
    for (int row = 0; row < kScreenH; row += 2) {
        std::cout << "  │";
        for (int col = 0; col < kScreenW; ++col) {
            uint8_t top = mem[kScreenBase + row * kScreenW + col];
            uint8_t bot = (row + 1 < kScreenH) ? mem[kScreenBase + (row + 1) * kScreenW + col] : 0;
            Rgb ct = pal[top % pal.size()];
            Rgb cb = pal[bot % pal.size()];
            std::cout << "\x1b[38;2;" << ct.r << ";" << ct.g << ";" << ct.b << "m" << "\x1b[48;2;" << cb.r << ";" << cb.g << ";" << cb.b
                      << "m" << "▀" << "\x1b[0m";
        }
        std::cout << "│\n";
    }
    std::cout << "  └" << repeatUtf8("─", kScreenW) << "┘\n\n";
    std::cout << "  legend: 0 black  1 red  2 green  3 yellow  4 blue  5 white  6 cyan  7 magenta  8 orange\n\n";
}

void applyPokeLine(std::array<uint8_t, kMemorySize>& mem, const std::string& line) {
    auto parseNum = [](std::string s) -> int {
        s = s.substr(s.find_first_not_of(" \t"));
        if (!s.empty() && s[0] == '$') return std::stoi(s.substr(1), nullptr, 16);
        return std::stoi(s, nullptr, 0);
    };
    auto eq = line.find('=');
    if (eq == std::string::npos) throw std::runtime_error("bad patch line (expected ADDR=VAL): " + line);
    int addr = parseNum(line.substr(0, eq));
    std::istringstream vals(line.substr(eq + 1));
    std::string tok;
    int offset = 0;
    while (std::getline(vals, tok, ',')) {
        mem[static_cast<uint16_t>(addr + offset)] = static_cast<uint8_t>(parseNum(tok) & 0xFF);
        ++offset;
    }
}

template <typename CpuT> const std::map<uint8_t, gygax::toy::disasm::OpInfo>& disasmTable() {
    if constexpr (std::is_same_v<CpuT, gygax::toy::Cpu6809>)
        return gygax::toy::disasm::table6809();
    else
        return gygax::toy::disasm::table6502();
}

template <typename CpuT> std::string disassembleAt(const std::array<uint8_t, kMemorySize>& mem, uint16_t addr, int& size) {
    const auto& table = disasmTable<CpuT>();
    uint8_t op = mem[addr];
    auto it = table.find(op);
    if (it == table.end()) {
        size = 1;
        return std::format("??? (${:02X} unsupported opcode)", op);
    }
    const auto& info = it->second;
    size = info.size;
    using gygax::toy::disasm::Mode;
    switch (info.mode) {
    case Mode::Inherent: return info.mnemonic;
    case Mode::Immediate: return std::format("{} #${:02X}", info.mnemonic, mem[addr + 1]);
    case Mode::Extended: {
        if (size == 3) {
            uint16_t operand = std::is_same_v<CpuT, gygax::toy::Cpu6809> ? static_cast<uint16_t>((mem[addr + 1] << 8) | mem[addr + 2])
                                                                         : static_cast<uint16_t>((mem[addr + 2] << 8) | mem[addr + 1]);
            return std::format("{} ${:04X}", info.mnemonic, operand);
        }
        return std::format("{} ${:02X}", info.mnemonic, mem[addr + 1]);
    }
    case Mode::Relative: {
        int8_t disp = static_cast<int8_t>(mem[addr + 1]);
        uint16_t target = static_cast<uint16_t>(addr + 2 + disp);
        return std::format("{} ${:04X}", info.mnemonic, target);
    }
    }
    return info.mnemonic;
}

template <typename CpuT> class CocoAgent : public gygax::BaseObject {
public:
    CocoAgent(std::string diskPath, std::string cpuName)
        : gygax::BaseObject("CocoAgent:" + diskPath, gygax::Capability::Cognition | gygax::Capability::Manipulation),
          diskPath_(std::move(diskPath)), cpuName_(std::move(cpuName)), cpu_(mem_) {}

    void load() {
        auto payload = gygax::toy::decb::readDsk(diskPath_);
        auto prog = gygax::toy::decb::parseBinaryPayload(payload);
        for (auto& [addr, data] : prog.segments) {
            for (size_t i = 0; i < data.size(); ++i) mem_[addr + i] = data[i];
        }
        cpu_.pc = prog.execAddr;
        addGoal("run " + diskPath_ + " to completion");
    }

    void applyPatches(const std::vector<std::string>& pokeLines) {
        for (auto& line : pokeLines) {
            applyPokeLine(mem_, line);
            pushThought("mod/patch applied: " + line);
        }
    }

    std::array<uint8_t, kMemorySize>& mem() { return mem_; }
    CpuT& cpu() { return cpu_; }

    int run(int maxSteps) {
        int steps = 0;
        int selfLoopCount = 0;
        for (; steps < maxSteps; ++steps) {
            uint16_t pcBefore = cpu_.pc;
            if (breakpoints_.count(pcBefore)) break;
            if (!cpu_.step() || cpu_.halted) break;
            if (cpu_.pc == pcBefore) {
                if (++selfLoopCount > kSelfLoopLimit) break;
            } else {
                selfLoopCount = 0;
            }
        }
        return steps;
    }

    void addBreakpoint(uint16_t addr) { breakpoints_.insert(addr); }

    void debugRepl() {
        std::cout << "  gygax debug. Commands: s[tep] c[ontinue] b ADDR r[egs] m ADDR [LEN] p[ixels] q[uit]\n\n";
        std::string cmdLine;
        while (true) {
            int size = 1;
            std::string dis = disassembleAt<CpuT>(mem_, cpu_.pc, size);
            std::cout << "  " << cpu_.describe() << "   next: " << dis << "\n  (gygax-dbg) ";
            if (!std::getline(std::cin, cmdLine)) break;
            std::istringstream iss(cmdLine);
            std::string cmd;
            iss >> cmd;
            if (cmd.empty()) continue;
            if (cmd == "q" || cmd == "quit") break;
            if (cmd == "s" || cmd == "step") {
                if (cpu_.halted || !cpu_.step()) std::cout << "  (halted)\n";
                pushThought("debug: single-stepped to " + cpu_.describe());
            } else if (cmd == "c" || cmd == "continue") {
                run(kMaxExecutionSteps);
                std::cout << "  stopped: " << cpu_.describe() << "\n";
            } else if (cmd == "b") {
                std::string a;
                iss >> a;
                uint16_t addr = static_cast<uint16_t>(std::stoi(a, nullptr, a.rfind("$", 0) == 0 ? 16 : 0));
                addBreakpoint(addr);
                std::cout << "  breakpoint set at $" << std::hex << addr << std::dec << "\n";
            } else if (cmd == "r" || cmd == "regs") {
                std::cout << "  " << cpu_.describe() << "\n";
            } else if (cmd == "m" || cmd == "mem") {
                std::string a;
                int len = 16;
                iss >> a;
                if (iss >> len) {
                }
                uint16_t addr = static_cast<uint16_t>(std::stoi(a, nullptr, a.rfind("$", 0) == 0 ? 16 : 0));
                std::cout << "  ";
                for (int i = 0; i < len; ++i) std::cout << std::format("{:02X} ", mem_[static_cast<uint16_t>(addr + i)]);
                std::cout << "\n";
            } else if (cmd == "p" || cmd == "pixels") {
                renderScreen(mem_);
            } else {
                std::cout << "  unknown command '" << cmd << "'\n";
            }
        }
    }

    void aiAnalyze() {
        uint16_t addr = cpu_.pc;
        int guard = 0;
        std::vector<std::string> findings;
        while (guard++ < kAnalysisInstructionLimit) {
            int size = 1;
            std::string dis = disassembleAt<CpuT>(mem_, addr, size);
            bool unsupported = dis.rfind("???", 0) == 0;
            findings.push_back(std::format("${:04X}: {}", addr, dis));
            if (unsupported) {
                findings.push_back("  -> unsupported opcode: this CPU core would halt here, not guess.");
                break;
            }
            if (dis.rfind("HALT", 0) == 0) {
                findings.push_back("  -> explicit halt instruction.");
                break;
            }
            if ((dis.rfind("BRA", 0) == 0 || dis.rfind("JMP", 0) == 0)) {
                std::string target = dis.substr(dis.find('$'));
                if (target == std::format("${:04X}", addr)) {
                    findings.push_back("  -> self-branch idiom: treated as 'done drawing', not a real infinite-loop bug.");
                    break;
                }
            }
            addr = static_cast<uint16_t>(addr + size);
        }
        for (auto& f : findings) pushThought(f);
        printBanner("GYGAX AI ASSISTANT", {"rule-based static analyzer", "(not a live LLM; see source banner)"});
        for (auto& f : findings) std::cout << "  " << f << "\n";
        std::cout << "\n  " << summarizeThoughts() << "\n\n";
    }

private:
    std::string diskPath_, cpuName_;
    std::array<uint8_t, kMemorySize> mem_{};
    CpuT cpu_;
    std::set<uint16_t> breakpoints_;
};

}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: gygax_coco <disk.dsk> [--cpu 6809|6502] [--steps N] [--quiet]\n"
                     "                   [--debug] [--ai] [--poke ADDR=VAL]... [--patch FILE]\n";
        return 1;
    }
    std::string diskPath, cpu = "6809", patchFile;
    int maxSteps = kMaxExecutionSteps;
    bool verbose = true, debug = false, ai = false;
    std::vector<std::string> pokes;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--cpu" && i + 1 < argc)
            cpu = argv[++i];
        else if (a == "--steps" && i + 1 < argc)
            maxSteps = std::stoi(argv[++i]);
        else if (a == "--quiet")
            verbose = false;
        else if (a == "--debug")
            debug = true;
        else if (a == "--ai")
            ai = true;
        else if (a == "--poke" && i + 1 < argc)
            pokes.push_back(argv[++i]);
        else if (a == "--patch" && i + 1 < argc)
            patchFile = argv[++i];
        else if (diskPath.empty())
            diskPath = a;
    }
    if (diskPath.empty()) {
        std::cerr << "gygax_coco: no disk image given\n";
        return 1;
    }

    try {
        if (!patchFile.empty()) {
            std::ifstream pf(patchFile);
            if (!pf) throw std::runtime_error("cannot open patch file " + patchFile);
            std::string line;
            while (std::getline(pf, line)) {
                if (!line.empty() && line[0] != ';') pokes.push_back(line);
            }
        }

        if (verbose) printBanner("GYGAX  ::  CoCo2/CoCo3 Toy Emulator", {"disk : " + diskPath, "cpu  : " + cpu});

        auto runWith = [&](auto& agent) {
            agent.initialize();
            agent.load();
            if (!pokes.empty()) agent.applyPatches(pokes);
            if (debug) {
                agent.debugRepl();
            } else if (ai) {
                agent.aiAnalyze();
            } else {
                int steps = agent.run(maxSteps);
                if (verbose) {
                    std::cout << "  ran " << steps << " step(s), halted at " << agent.cpu().describe() << "\n\n";
                }
            }
            if (verbose)
                renderScreen(agent.mem());
            else
                summarizeScreen(agent.mem());
            agent.shutdown();
        };

        if (cpu == "6502") {
            CocoAgent<gygax::toy::Cpu6502> agent(diskPath, cpu);
            runWith(agent);
        } else {
            CocoAgent<gygax::toy::Cpu6809> agent(diskPath, cpu);
            runWith(agent);
        }
    } catch (const std::exception& e) {
        std::cerr << "gygax_coco: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
