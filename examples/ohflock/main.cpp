// ohflock: watch a flock of unarmed aircraft hold a corridor open with light, sound and SatLink.
// See docs/OHFLOCK.md.

#include <algorithm>
#include <atomic>
#include <optional>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gygax/core/json.hpp>
#include <gygax/core/log.hpp>
#include <gygax/service/service.hpp>

import gygax.tools;

#include "sim.hpp"

using gygax::json::Value;
using namespace ohflock;

namespace {

std::atomic<bool> gStop{false};

struct Options {
    Config config;
    double speed{0};  // 0: as fast as possible; 1: real time; 8: eight times
    int servePort{0}; // >0: run an embedded Gygax service for gyde
    std::string script, html, json;
    bool selfcheck{false}, quiet{false};
};

void usage() {
    std::cout << "usage: ohflock [options]\n\n"
                 "Watch twenty-four unarmed kites hold a relief corridor open against fighters, a SAM battery,\n"
                 "AAA and strike jets, with low-power lasers on sensors, thrown sound and SatLink. Fictional.\n\n"
                 "  --html FILE        write a self-contained 3D replay (open it in a browser)\n"
                 "  --serve PORT       run live, with flock.* tools on 127.0.0.1:PORT for gyde (gyde --url http://127.0.0.1:PORT)\n"
                 "  --speed X          with --serve: X times real time (default 4)\n"
                 "  --script FILE      timed orders: '<seconds> <order>' per line\n"
                 "  --no-satlink       the same battle without satellite tracking aboard (the counterfactual)\n"
                 "  --seed N           engagement dice (default 1984)\n"
                 "  --json FILE        outcome and statistics\n"
                 "  --quiet            only the final report\n"
                 "  --selfcheck        determinism and outcome checks (a ctest entry)\n\n"
                 "orders: posture screen|tight|wide · lasers on|off · acoustics on|off · corridor east|west|centre\n";
}

// --- Replay ------------------------------------------------------------------------------------

class Recorder {
public:
    explicit Recorder(const Sim& sim) {
        Value units = Value::array();
        for (const auto& u : sim.units()) {
            Value v = Value::object();
            v["id"] = u.id;
            v["kind"] = name(u.kind);
            v["call"] = u.call;
            v["section"] = std::string(1, u.section);
            units.push(std::move(v));
        }
        meta_["units"] = std::move(units);
        meta_["startUnix"] = sim.startUnix();
        meta_["satlink"] = sim.config().satlink;
        meta_["seed"] = static_cast<double>(sim.config().seed);
        meta_["origin"] = Value::array();
        meta_["origin"].push(Sim::kOriginLat);
        meta_["origin"].push(Sim::kOriginLon);
    }

    void frame(const Sim& sim) {
        Value f = Value::object();
        f["t"] = sim.time();
        Value u = Value::array();
        for (const auto& x : sim.units()) {
            Value row = Value::array();
            row.push(std::lround(x.pos.x));
            row.push(std::lround(x.pos.y));
            row.push(std::lround(x.pos.z));
            row.push(x.alive && !x.gone ? 1 : 0);
            row.push(intern(x.alive ? x.task : ""));
            u.push(std::move(row));
        }
        f["u"] = std::move(u);
        Value shots = Value::array();
        for (const auto& s : sim.shots()) {
            if (s.launched > sim.time() || s.impactAt < sim.time() - 1.5) continue;
            Value row = Value::array();
            row.push(s.id);
            row.push(static_cast<int>(s.weapon));
            row.push(std::lround(s.pos.x));
            row.push(std::lround(s.pos.y));
            row.push(std::lround(s.pos.z));
            row.push(s.impactAt <= sim.time() ? static_cast<int>(s.outcome) : -1);
            shots.push(std::move(row));
        }
        f["s"] = std::move(shots);
        Value beams = Value::array();
        for (const auto& b : sim.beams()) {
            Value row = Value::array();
            row.push(b.from);
            row.push(std::lround(b.to.x));
            row.push(std::lround(b.to.y));
            row.push(std::lround(b.to.z));
            row.push(std::string(1, b.colour));
            row.push(intern(b.purpose));
            beams.push(std::move(row));
        }
        f["b"] = std::move(beams);
        Value ph = Value::array();
        for (const auto& p : sim.phantoms()) {
            Value row = Value::array();
            row.push(std::lround(p.pos.x));
            row.push(std::lround(p.pos.y));
            row.push(std::lround(p.pos.z));
            ph.push(std::move(row));
        }
        f["p"] = std::move(ph);
        Value sky = Value::array();
        for (const auto& s : sim.skyTracks()) {
            if (s.elevation < 0) continue; // only what is above the horizon
            Value row = Value::array();
            row.push(s.name);
            row.push(std::round(s.elevation * 10) / 10);
            row.push(std::round(s.azimuth * 10) / 10);
            row.push(s.relay ? 1 : 0);
            row.push(std::lround(s.doppler_hz));
            sky.push(std::move(row));
        }
        f["k"] = std::move(sky);
        const auto& st = sim.stats();
        Value hud = Value::object();
        hud["shots"] = st.opforShots;
        hud["hits"] = st.hits;
        hud["lost"] = st.kitesLost;
        hud["convoyHits"] = st.convoyHits;
        hud["nav"] = std::lround(st.navErrorM);
        hud["navSrc"] = sim.navSource();
        hud["jam"] = sim.gnssJammed();
        hud["recon"] = sim.reconOverhead();
        hud["disp"] = sim.dispersed();
        Value outcomes = Value::array();
        for (int c : st.byOutcome) outcomes.push(c);
        hud["out"] = std::move(outcomes);
        f["h"] = std::move(hud);
        frames_.push(std::move(f));
    }

    std::string json(const Sim& sim) {
        Value events = Value::array();
        for (const auto& e : sim.events()) {
            Value row = Value::array();
            row.push(std::round(e.t * 10) / 10);
            row.push(e.kind);
            row.push(e.text);
            events.push(std::move(row));
        }
        Value strings = Value::array();
        for (const auto& t : strings_) strings.push(t);
        meta_["strings"] = std::move(strings);
        Value root = Value::object();
        root["meta"] = meta_;
        root["frames"] = frames_;
        root["events"] = std::move(events);
        return root.dump();
    }

private:
    int intern(const std::string& text) {
        const auto [it, added] = index_.try_emplace(text, static_cast<int>(strings_.size()));
        if (added) strings_.push_back(text);
        return it->second;
    }

    std::map<std::string, int> index_;
    std::vector<std::string> strings_;
    Value meta_ = Value::object();
    Value frames_ = Value::array();
};

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

std::vector<std::pair<double, std::string>> readScript(const std::string& path) {
    std::vector<std::pair<double, std::string>> out;
    std::istringstream in(readFile(path));
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream l(line);
        double t = 0;
        std::string rest;
        if (!(l >> t)) throw std::runtime_error("script line without a time: " + line);
        std::getline(l >> std::ws, rest);
        out.emplace_back(t, rest);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

void report(const Sim& sim) {
    const auto& st = sim.stats();
    std::cout << "\n== OHFLOCK " << (sim.config().satlink ? "" : "(without SatLink) ") << "==\n";
    std::cout << std::format(
        "battle began {} UTC and lasted {:.0f} s\n",
        std::format("{:%F %T}", std::chrono::sys_seconds{std::chrono::seconds{static_cast<long long>(sim.startUnix())}}), sim.time());
    std::cout << std::format("convoy: {}, {} hits      kites: {} of 24 lost      shots fired by the flock: {}\n",
                             st.convoyThrough ? "through" : "NOT through", st.convoyHits, st.kitesLost, st.flockShots);
    std::cout << std::format("shots fired at the flock and convoy: {}, of which hit: {}\n", st.opforShots, st.hits);
    const Outcome order[] = {Outcome::Phantom,       Outcome::Dazzled,   Outcome::Notched,
                             Outcome::Outmaneuvered, Outcome::DecoySpot, Outcome::LostLock};
    for (auto o : order)
        if (st.byOutcome[static_cast<int>(o)] > 0) std::cout << std::format("  {:>4}  {}\n", st.byOutcome[static_cast<int>(o)], name(o));
    std::cout << std::format("navigation: worst error {:.0f} m ({})   lasers on sensors: {} beam-seconds   phantoms thrown: {}\n",
                             st.worstNavErrorM, sim.navSource(), std::lround(st.lasersOnSensors * sim.config().dt), st.phantomsThrown);
    std::cout << std::format("orders: {} reached the flock\n", st.ordersApplied);
    std::cout << (st.convoyThrough && st.convoyHits == 0 ? "result: DECISIVE: corridor held, not one shot fired by the flock\n"
                                                         : "result: the corridor did not hold cleanly\n");
}

Sim runBattle(const Options& o, Recorder* rec, bool narrate) {
    Sim sim(o.config);
    std::vector<std::pair<double, std::string>> script;
    if (!o.script.empty()) script = readScript(o.script);
    std::size_t nextOrder = 0, printed = 0;
    while (!sim.done()) {
        while (nextOrder < script.size() && script[nextOrder].first <= sim.time()) {
            const auto reply = sim.order(script[nextOrder].second);
            if (narrate) std::cout << std::format("T+{:03.0f}s  [you]    {} -> {}\n", sim.time(), script[nextOrder].second, reply);
            ++nextOrder;
        }
        sim.step();
        if (rec && sim.tick() % 4 == 0) rec->frame(sim); // 1 Hz; the viewer interpolates
        if (narrate) {
            for (; printed < sim.events().size(); ++printed) {
                const auto& e = sim.events()[printed];
                std::cout << std::format("T+{:03.0f}s  [{:<6}] {}\n", e.t, e.kind, e.text);
            }
        }
    }
    if (rec) rec->frame(sim);
    return sim;
}

int selfcheck() {
    Options o;
    o.config.seed = 1984;
    const auto a = runBattle(o, nullptr, false);
    const auto b = runBattle(o, nullptr, false);
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) {
        std::cout << (ok ? "PASS " : "FAIL ") << what << "\n";
        failures += ok ? 0 : 1;
    };
    check(a.digest() == b.digest(), "same seed, same battle");
    const auto& s = a.stats();
    check(s.flockShots == 0 && std::none_of(a.shots().begin(), a.shots().end(),
                                            [&](const Shot& x) {
                                                for (const auto& u : a.units())
                                                    if (u.id == x.shooter) return u.kind == Kind::Kite;
                                                return false;
                                            }),
          "the flock fires nothing");
    check(s.opforShots >= 150, std::format("a large volume is fired at the flock ({} shots)", s.opforShots));
    check(s.convoyThrough && s.convoyHits == 0, "the convoy is through untouched");
    check(s.kitesLost <= 2, std::format("the flock dominates ({} of 24 lost)", s.kitesLost));
    check(s.worstNavErrorM < 1500 && s.navErrorM < 250,
          std::format("SatLink Doppler navigation holds under jamming (now {:.0f} m)", s.navErrorM));
    // Over many seeds, and against the same battle without SatLink.
    int with = 0, without = 0, lostWith = 0, lostWithout = 0;
    double navWith = 0, navWithout = 0;
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        Options x;
        x.config.seed = seed;
        const auto s1 = runBattle(x, nullptr, false);
        x.config.satlink = false;
        const auto s2 = runBattle(x, nullptr, false);
        with += s1.stats().hits, without += s2.stats().hits;
        navWith += s1.stats().worstNavErrorM, navWithout += s2.stats().worstNavErrorM;
        lostWith += s1.stats().kitesLost, lostWithout += s2.stats().kitesLost;
    }
    std::cout << std::format(
        "     12 seeds: hits {} with SatLink, {} without; kites lost {} vs {}; mean worst navigation error {:.0f} m vs {:.0f} m\n", with,
        without, lostWith, lostWithout, navWith / 12, navWithout / 12);
    check(without > with, "SatLink makes the difference");
    std::cout << (failures ? "selfcheck FAILED\n" : "selfcheck ok\n");
    return failures ? 1 : 0;
}

// --- Live, with gyde ---------------------------------------------------------------------------

int serve(const Options& o) {
    std::mutex mutex;
    Sim sim(o.config);
    auto& tools = gygax::tools::ToolRegistry::getInstance();
    auto locked = [&](auto fn) {
        return [&, fn](const std::string& input) {
            std::lock_guard lock(mutex);
            return fn(input);
        };
    };
    tools.registerTool("flock.status", locked([&](const std::string&) { return sim.status(); }), "OHFLOCK: the battle at a glance");
    tools.registerTool("flock.order", locked([&](const std::string& in) { return sim.order(in); }),
                       "OHFLOCK: posture screen|tight|wide, lasers on|off, acoustics on|off, corridor east|west|centre");
    tools.registerTool("flock.explain", locked([&](const std::string&) { return sim.explain(10); }),
                       "OHFLOCK: the flock's recent decisions and why");
    tools.registerTool("flock.sky", locked([&](const std::string&) { return sim.sky(); }),
                       "OHFLOCK: satellites over the corridor (SatLink)");

    gygax::service::ServiceConfig sc;
    sc.port = static_cast<std::uint16_t>(o.servePort);
    sc.engines = {"echo"};
    gygax::service::Service svc(sc);
    std::string error;
    if (!svc.start(&error)) {
        std::cerr << "ohflock: cannot start the service: " << error << "\n";
        return 1;
    }
    std::cout << std::format("OHFLOCK live on 127.0.0.1:{}. In another terminal:\n  gyde --url http://127.0.0.1:{}\n"
                             "  > tool flock.status        > tool flock.sky        > tool flock.explain\n"
                             "  > tool flock.order posture wide\n\n",
                             svc.port(), svc.port());
    std::signal(SIGINT, [](int) { gStop = true; });
    const double speed = o.speed > 0 ? o.speed : 4.0;
    std::size_t printed = 0;
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        {
            std::lock_guard lock(mutex);
            if (sim.done() || gStop) break;
            sim.step();
            for (; printed < sim.events().size(); ++printed) {
                const auto& e = sim.events()[printed];
                std::cout << std::format("T+{:03.0f}s  [{:<6}] {}\n", e.t, e.kind, e.text);
            }
        }
        next += std::chrono::microseconds(static_cast<long long>(o.config.dt / speed * 1e6));
        std::this_thread::sleep_until(next);
    }
    {
        std::lock_guard lock(mutex);
        report(sim);
    }
    std::cout << "(service stays up for a minute for last questions; Ctrl-C to leave)\n";
    for (int i = 0; i < 600 && !gStop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    svc.stop();
    for (const char* t : {"flock.status", "flock.order", "flock.explain", "flock.sky"}) tools.unregisterTool(t);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    gygax::log::setLevel(gygax::log::Level::Error);
    Options o;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
                return argv[++i];
            };
            if (a == "--html")
                o.html = next();
            else if (a == "--json")
                o.json = next();
            else if (a == "--script")
                o.script = next();
            else if (a == "--serve")
                o.servePort = std::stoi(next());
            else if (a == "--speed")
                o.speed = std::stod(next());
            else if (a == "--seed")
                o.config.seed = std::stoull(next());
            else if (a == "--no-satlink")
                o.config.satlink = false;
            else if (a == "--quiet")
                o.quiet = true;
            else if (a == "--selfcheck")
                o.selfcheck = true;
            else if (a == "--help" || a == "-h") {
                usage();
                return 0;
            } else {
                usage();
                return 2;
            }
        }
        if (o.selfcheck) return selfcheck();
        if (o.servePort > 0) return serve(o);

        Recorder* recPtr = nullptr;
        std::optional<Recorder> rec;
        if (!o.html.empty()) {
            Sim probe(o.config);
            rec.emplace(probe);
            recPtr = &*rec;
        }
        const auto sim = runBattle(o, recPtr, !o.quiet);
        report(sim);
        if (rec) {
            std::string page = readFile(GYGAX_OHFLOCK_VIEWER);
            const std::string marker = "/*OHFLOCK_DATA*/null";
            const auto at = page.find(marker);
            if (at == std::string::npos) throw std::runtime_error("viewer template has no data marker");
            page.replace(at, marker.size(), rec->json(sim));
            std::ofstream(o.html) << page;
            std::cout << "replay: " << o.html << "\n";
        }
        if (!o.json.empty()) {
            const auto& st = sim.stats();
            Value v = Value::object();
            v["convoyThrough"] = st.convoyThrough;
            v["convoyHits"] = st.convoyHits;
            v["kitesLost"] = st.kitesLost;
            v["shotsAtFlock"] = st.opforShots;
            v["shotsByFlock"] = st.flockShots;
            v["hits"] = st.hits;
            v["worstNavErrorM"] = st.worstNavErrorM;
            v["satlink"] = o.config.satlink;
            Value outs = Value::object();
            for (int i = 0; i < 9; ++i) outs[name(static_cast<Outcome>(i))] = st.byOutcome[i];
            v["outcomes"] = std::move(outs);
            std::ofstream(o.json) << v.dump(2) << "\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ohflock: " << e.what() << "\n";
        return 1;
    }
}
