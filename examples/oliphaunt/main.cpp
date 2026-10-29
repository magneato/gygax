// Operation Oliphaunt command line: record what you see, ask what keeps turning up.
//
//   oliphaunt [--file PATH] observe KIND IDENTIFIER [--label TEXT] [--lat D --lon D] [--place NAME] [--time EPOCH]
//   oliphaunt [--file PATH] report [--min none|noted|notice|alert|persistent]
//   oliphaunt [--file PATH] safe NAME LAT LON [RADIUS_M]
//   oliphaunt [--file PATH] trust KIND IDENTIFIER | untrust KIND IDENTIFIER
//   oliphaunt [--file PATH] forget KIND IDENTIFIER | forget --all
//   oliphaunt demo        three weeks of a car that keeps turning up, in a throwaway memory
//
// The default file is $GYGAX_OLIPHAUNT_PATH, else ~/.local/share/gygax/oliphaunt.json.
#include <sys/stat.h>

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

#include <gygax/oliphaunt/memory.hpp>

namespace {

using namespace gygax;
using namespace gygax::oliphaunt;

std::string defaultFile() {
    if (const char* p = std::getenv("GYGAX_OLIPHAUNT_PATH"); p && *p) return p;
    const char* home = std::getenv("HOME");
    if (!home) return "oliphaunt.json";
    const std::string dir = std::string(home) + "/.local/share/gygax";
    ::mkdir((std::string(home) + "/.local").c_str(), 0700);
    ::mkdir((std::string(home) + "/.local/share").c_str(), 0700);
    ::mkdir(dir.c_str(), 0700);
    return dir + "/oliphaunt.json";
}

void usage() {
    std::cout << "usage: oliphaunt [--file PATH] observe KIND ID [--label T] [--lat D --lon D] [--place N] [--time EPOCH]\n"
                 "       oliphaunt [--file PATH] report [--min LEVEL]\n"
                 "       oliphaunt [--file PATH] safe NAME LAT LON [RADIUS_M]\n"
                 "       oliphaunt [--file PATH] trust|untrust|forget KIND ID    (forget --all erases everything)\n"
                 "       oliphaunt demo\n";
}

void print(const Finding& f) {
    std::cout << "[" << concernName(f.concern) << "] " << f.summary << "\n";
}

int demo() {
    Memory m;
    m.addSafePlace({"home", 51.5000, -0.1200, 200});
    const std::int64_t start = std::time(nullptr) - 21 * 86400;
    const double route[3][2] = {{51.5200, -0.1000}, {51.5400, -0.1400}, {51.4800, -0.1600}};
    std::cout << "Three weeks of a grey hatchback in a throwaway memory (plate stored only as a salted hash):\n\n";
    for (int day = 0; day < 21; day += 3) {
        for (int stop = 0; stop < 3; ++stop) {
            Sighting s;
            s.kind = Kind::Vehicle;
            s.identifier = "AB12 CDE";
            s.label = "grey hatchback";
            s.latitude = route[stop][0];
            s.longitude = route[stop][1];
            s.time = start + day * 86400 + stop * 5400;
            const auto f = m.observe(s);
            if (stop == 2) {
                std::cout << "day " << (day < 10 ? " " : "") << day << ": ";
                print(*f);
            }
        }
    }
    Sighting neighbour;
    neighbour.identifier = "XY99 ZZZ";
    neighbour.label = "neighbour's van";
    neighbour.latitude = 51.5001;
    neighbour.longitude = -0.1201;
    for (int d = 0; d < 21; ++d) {
        neighbour.time = start + d * 86400 + 8 * 3600;
        m.observe(neighbour);
    }
    std::cout << "\nReport (notice and above):\n";
    for (const auto& f : m.report()) print(f);
    std::cout << "\nThe neighbour's van was seen every day, but only at home: not a concern.\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string file;
    std::vector<std::string> rest;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--file" && i + 1 < args.size()) file = args[++i];
        else rest.push_back(args[i]);
    }
    if (rest.empty() || rest[0] == "--help" || rest[0] == "-h") {
        usage();
        return rest.empty() ? 2 : 0;
    }
    const std::string cmd = rest[0];
    if (cmd == "demo") return demo();

    Memory memory(file.empty() ? defaultFile() : file);
    if (!memory.lastError().empty()) {
        std::cerr << "oliphaunt: " << memory.lastError() << "\n";
        return 1;
    }
    auto option = [&](const std::string& name) -> std::string {
        for (std::size_t i = 1; i + 1 < rest.size(); ++i)
            if (rest[i] == name) return rest[i + 1];
        return {};
    };
    auto kindArg = [&](std::size_t i) -> std::optional<Kind> {
        if (i >= rest.size()) return std::nullopt;
        return parseKind(rest[i]);
    };

    if (cmd == "observe") {
        const auto kind = kindArg(1);
        if (!kind || rest.size() < 3) {
            usage();
            return 2;
        }
        Sighting s;
        s.kind = *kind;
        s.identifier = rest[2];
        s.label = option("--label");
        s.place = option("--place");
        if (!option("--lat").empty() && !option("--lon").empty()) {
            s.latitude = std::strtod(option("--lat").c_str(), nullptr);
            s.longitude = std::strtod(option("--lon").c_str(), nullptr);
        }
        if (!option("--time").empty()) s.time = std::strtoll(option("--time").c_str(), nullptr, 10);
        std::string error;
        const auto f = memory.observe(s, &error);
        if (!f) {
            std::cerr << "oliphaunt: " << error << "\n";
            return 1;
        }
        print(*f);
        return f->concern >= Concern::Alert ? 10 : 0;
    }
    if (cmd == "report") {
        Concern minimum = Concern::Notice;
        if (const auto m = parseConcern(option("--min")); m) minimum = *m;
        const auto findings = memory.report(minimum);
        for (const auto& f : findings) print(f);
        std::cout << findings.size() << " of " << memory.entityCount() << " remembered\n";
        return 0;
    }
    if (cmd == "safe" && rest.size() >= 4) {
        memory.addSafePlace({rest[1], std::strtod(rest[2].c_str(), nullptr), std::strtod(rest[3].c_str(), nullptr),
                             rest.size() > 4 ? std::strtod(rest[4].c_str(), nullptr) : 150.0});
        std::cout << "safe place added\n";
        return 0;
    }
    if (cmd == "trust" || cmd == "untrust") {
        const auto kind = kindArg(1);
        if (!kind || rest.size() < 3) {
            usage();
            return 2;
        }
        return memory.trust(*kind, rest[2], cmd == "trust") ? 0 : 1;
    }
    if (cmd == "forget") {
        if (rest.size() > 1 && rest[1] == "--all") {
            memory.forgetAll();
            std::cout << "everything forgotten\n";
            return 0;
        }
        const auto kind = kindArg(1);
        if (!kind || rest.size() < 3) {
            usage();
            return 2;
        }
        return memory.forget(*kind, rest[2]) ? 0 : 1;
    }
    usage();
    return 2;
}
