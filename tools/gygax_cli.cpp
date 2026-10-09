#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gygax/core/charconv.hpp>
#include <gygax/core/json.hpp>
#include <gygax/core/log.hpp>
#include <gygax/inference/backend.hpp>
#include <gygax/net/http.hpp>
#include <gygax/logistics/ledger.hpp>
#include <gygax/net/link.hpp>
#include <gygax/neuro/network.hpp>
#include <gygax/robotics/mavlink.hpp>
#include <gygax/robotics/sim_vehicle.hpp>
#include <gygax/satlink/satlink.hpp>
#include <gygax/satlink/sdr.hpp>
#include <gygax/service/expression.hpp>
#include <gygax/service/service.hpp>
#include <gygax/version.hpp>

namespace {

using gygax::json::Value;
namespace net = gygax::net;
namespace service = gygax::service;

constexpr auto kServicePollInterval = std::chrono::milliseconds(100);

std::atomic<bool> gStop{false};

void onSignal(int) {
    gStop.store(true);
}

void installSignals() {
    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
}

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::vector<std::string>> options;

    static Args parse(int argc, char** argv, int from) {
        Args a;
        for (int i = from; i < argc; ++i) {
            std::string tok = argv[i];
            if (tok.starts_with("--")) {
                std::string key = tok.substr(2);
                std::string value = "1";
                if (const auto eq = key.find('='); eq != std::string::npos) {
                    value = key.substr(eq + 1);
                    key.resize(eq);
                } else if (i + 1 < argc && !std::string_view(argv[i + 1]).starts_with("--")) {
                    value = argv[++i];
                }
                a.options[key].push_back(value);
            } else {
                a.positional.push_back(std::move(tok));
            }
        }
        return a;
    }

    [[nodiscard]] bool has(const std::string& k) const { return options.contains(k); }

    [[nodiscard]] std::string get(const std::string& k, const std::string& fallback = {}) const {
        auto it = options.find(k);
        return it == options.end() ? fallback : it->second.back();
    }

    [[nodiscard]] std::vector<std::string> all(const std::string& k) const {
        auto it = options.find(k);
        return it == options.end() ? std::vector<std::string>{} : it->second;
    }
};

void printUsage() {
    std::cout
        << "gygax " << GYGAX_VERSION_STRING << " - autonomous agent runtime and inference router\n\n"
        << "usage: gygax <command> [options]\n\n"
        << "commands:\n"
        << "  serve      run the HTTP service (standard chat API and Ollama API, agents, tools, metrics)\n"
        << "  doctor     run the built-in self-test suite; add --url to test a running service\n"
        << "  rpc        serve JSON-RPC 2.0 over stdin/stdout\n"
        << "  neuro      run a spiking network spec (file or - for stdin) or 'neuro demo'\n"
        << "  status     show node, engines and agents of a running service\n"
        << "  ask        send a prompt to a running service (--agent to use the tool-using agent)\n"
        << "  sim-autopilot  run a virtual MAVLink multicopter (--uri LINK, --system-id N) to develop against\n"
        << "  track      predict satellite passes, or follow one live with Doppler and rig tuning (gygax track --help)\n"
        << "  version    print the version\n\n"
        << "serve options (environment variables in brackets):\n"
        << "  --host H [GYGAX_HOST]            bind address (default 127.0.0.1)\n"
        << "  --port P [GYGAX_PORT]            listen port (default 1984)\n"
        << "  --token T [GYGAX_API_TOKEN]      bearer token required for all non-health endpoints\n"
        << "  --token-file PATH [GYGAX_API_TOKEN_FILE]  read the token from a file (keeps it out of ps output)\n"
        << "  --engine SPEC [GYGAX_ENGINES]    repeatable: 'echo', 'ollama', 'llama-cpp', 'lmstudio' (optionally '@host:port') or\n"
        << "                                   'http://host:port/v1[;model=M][;key=K]'\n"
        << "  --mcp NAME=COMMAND [GYGAX_MCP]   repeatable: MCP server started over stdio; its tools become mcp.NAME.TOOL\n"
        << "  --peer URL [GYGAX_PEERS]         repeatable: another gygax node, e.g. http://10.0.0.5:1984\n"
        << "  --model M [GYGAX_MODEL]          default model for agents and chat\n"
        << "  --tool-protocol json|atari [GYGAX_TOOL_PROTOCOL]  agent tool-calling wire format (default json); see docs/ATARI.md\n"
        << "  --cors-origin O                  Access-Control-Allow-Origin value\n"
        << "  --rate-limit N                   requests per minute per client (0 disables)\n"
        << "  --workers N --agent-workers N    thread pool sizes\n"
        << "  --agent-timeout-ms N             give up on an agent chat request after N ms (default 120000)\n"
        << "  --allow-insecure-remote          allow non-loopback bind without a token\n"
        << "  --device-commands [GYGAX_DEVICE_COMMANDS=1]  let API clients and agents send commands to robots, vehicles and buses\n"
        << "  --ledger PATH [GYGAX_LEDGER_PATH]  append-only journal that makes the logistics ledger survive restarts\n"
        << "  --plugin PATH [GYGAX_PLUGINS]    repeatable: load a native plugin (shared library built with the plugin SDK)\n"
        << "  --extension URL [GYGAX_EXTENSIONS]  repeatable: register tools served by an extension process (see docs/EXTENSIONS.md)\n"
        << "  --log-level L --log-format F     trace|debug|info|warn|error, text|json\n";
}

void applyLogging(const Args& a) {
    if (a.has("log-level")) gygax::log::setLevel(gygax::log::parseLevel(a.get("log-level")));
    if (a.get("log-format") == "json") gygax::log::setFormat(gygax::log::Format::Json);
}

service::ServiceConfig configFrom(const Args& a) {
    auto c = service::ServiceConfig::fromEnvironment();
    if (a.has("host")) c.host = a.get("host");
    if (a.has("port")) c.port = static_cast<std::uint16_t>(std::strtoul(a.get("port").c_str(), nullptr, 10));
    if (a.has("token")) c.token = a.get("token");
    if (a.has("token-file")) c.token = service::readTokenFile(a.get("token-file"));
    if (a.has("peer-token")) c.peerToken = a.get("peer-token");
    if (a.has("engine")) c.engines = a.all("engine");
    if (a.has("peer")) c.peers = a.all("peer");
    if (a.has("model")) c.defaultModel = a.get("model");
    if (a.has("tool-protocol")) c.toolProtocol = a.get("tool-protocol");
    if (a.has("cors-origin")) c.corsOrigin = a.get("cors-origin");
    if (a.has("rate-limit")) c.rateLimitPerMinute = std::strtoul(a.get("rate-limit").c_str(), nullptr, 10);
    if (a.has("workers")) c.httpWorkers = std::strtoul(a.get("workers").c_str(), nullptr, 10);
    if (a.has("agent-workers")) c.agentWorkers = std::strtoul(a.get("agent-workers").c_str(), nullptr, 10);
    if (a.has("agent-timeout-ms")) c.agentTimeoutMs = std::strtoll(a.get("agent-timeout-ms").c_str(), nullptr, 10);
    if (a.has("allow-insecure-remote")) c.allowInsecureRemote = true;
    if (a.has("device-commands")) c.enableDeviceCommands = true;
    if (a.has("ledger")) c.ledgerPath = a.get("ledger");
    if (a.has("plugin")) c.plugins = a.all("plugin");
    if (a.has("extension")) c.extensions = a.all("extension");
    if (a.has("mcp")) c.mcpServers = a.all("mcp");
    return c;
}

int cmdServe(const Args& a) {
    applyLogging(a);
    installSignals();
    service::Service svc(configFrom(a));
    std::string error;
    if (!svc.start(&error)) {
        std::cerr << "gygax: cannot start: " << error << "\n";
        return 1;
    }
    while (!gStop.load()) std::this_thread::sleep_for(kServicePollInterval);
    gygax::log::info("main", "shutting down");
    svc.stop();
    return 0;
}

int cmdSimAutopilot(const Args& a) {
    applyLogging(a);
    installSignals();
    const std::string uri = a.has("uri") ? a.get("uri") : "udp://127.0.0.1:14550?bind=14551";
    std::string error;
    auto link = net::openLink(uri, &error);
    if (!link) {
        std::cerr << "gygax: cannot open " << uri << ": " << error << "\n";
        return 1;
    }
    gygax::robotics::VirtualAutopilotOptions options;
    if (a.has("system-id")) options.systemId = static_cast<std::uint8_t>(std::strtoul(a.get("system-id").c_str(), nullptr, 10));
    gygax::robotics::VirtualAutopilot pilot(link, options);
    pilot.start();
    std::cout << "virtual autopilot (MAVLink, system " << static_cast<int>(options.systemId) << ") on " << uri << "\n" << std::flush;
    while (!gStop.load()) std::this_thread::sleep_for(kServicePollInterval);
    pilot.stop();
    return 0;
}

int cmdRpc(const Args& a) {
    applyLogging(a);
    if (!a.has("log-level")) gygax::log::setLevel(gygax::log::Level::Warn);
    auto config = configFrom(a);
    config.enableHttp = false;
    service::Service svc(std::move(config));
    std::string error;
    if (!svc.start(&error)) {
        std::cerr << "gygax: cannot start: " << error << "\n";
        return 1;
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::string perr;
        auto parsed = gygax::json::parse(line, &perr);
        Value response;
        if (!parsed) {
            response = Value::object();
            response["jsonrpc"] = "2.0";
            response["id"] = nullptr;
            response["error"]["code"] = -32700;
            response["error"]["message"] = "parse error: " + perr;
        } else {
            response = svc.rpc(*parsed);
        }
        std::cout << response.dump() << std::endl;
    }
    svc.stop();
    return 0;
}

std::string readAll(std::istream& in) {
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

Value demoSpec() {
    std::string text = R"({
      "dt": 0.1, "seed": 42, "run_ms": 1000,
      "populations": [
        {"name": "input", "type": "poisson", "n": 100, "rate": 40},
        {"name": "hidden", "type": "lif", "n": 200},
        {"name": "output", "type": "lif", "n": 10, "params": {"tau_m": 30}}
      ],
      "projections": [
        {"pre": "input", "post": "hidden", "connect": {"type": "fixed_prob", "p": 0.2}, "weight": {"mean": 6.0, "std": 1.0}, "delay_ms": 1.0,
         "stdp": {"a_plus": 0.01, "a_minus": 0.0105, "w_min": 0, "w_max": 12}},
        {"pre": "hidden", "post": "output", "connect": {"type": "fixed_prob", "p": 0.3}, "weight": {"mean": 4.0, "std": 0.5}, "delay_ms": 2.0}
      ]
    })";
    auto parsed = gygax::json::parse(text);
    if (!parsed) throw std::logic_error("built-in demo spec is invalid");
    return std::move(*parsed);
}

int cmdNeuro(const Args& a) {
    Value spec;
    if (a.positional.empty() || a.positional[0] == "demo") {
        spec = demoSpec();
    } else {
        std::string text;
        if (a.positional[0] == "-") {
            text = readAll(std::cin);
        } else {
            std::ifstream f(a.positional[0]);
            if (!f) {
                std::cerr << "gygax: cannot read " << a.positional[0] << "\n";
                return 1;
            }
            text = readAll(f);
        }
        std::string error;
        auto parsed = gygax::json::parse(text, &error);
        if (!parsed) {
            std::cerr << "gygax: invalid JSON: " << error << "\n";
            return 1;
        }
        spec = std::move(*parsed);
    }
    std::string error;
    auto out = gygax::neuro::simulateSpec(spec, &error);
    if (out.isNull()) {
        std::cerr << "gygax: " << error << "\n";
        return 1;
    }
    std::cout << out.dump(2) << "\n";
    return 0;
}

// --- gygax track ---------------------------------------------------------------------------------

namespace satlink = gygax::satlink;

void printTrackUsage() {
    std::cout << "usage: gygax track --tle FILE --at LAT,LON[,METRES] [options]\n\n"
              << "Predict passes of a satellite over a station from a two-line element set, or follow it live.\n\n"
              << "  --tle FILE          TLE file (two- or three-line sets; - for stdin)\n"
              << "  --sat NAME|NUMBER   which satellite in the file (name substring or catalog number; default: the first)\n"
              << "  --at LAT,LON[,M]    station: degrees north, degrees east, metres above the WGS-84 ellipsoid\n"
              << "  --start TIME        'now' (default), Unix seconds, or UTC as YYYY-MM-DD[THH:MM[:SS]]\n"
              << "  --hours H           how far ahead to look for passes (default 24)\n"
              << "  --min-elevation D   only passes that rise above D degrees (default 10)\n"
              << "  --downlink HZ       show the Doppler-corrected receive frequency\n"
              << "  --live              print look angles once a second instead of a pass table (Ctrl-C to stop)\n"
              << "  --count N           with --live, stop after N lines\n"
              << "  --rig HOST[:PORT]   with --live and --downlink, retune Hamlib rigctld every second (port 4532)\n"
              << "  --sdr HOST[:PORT]   with --live and --downlink, retune an rtl_tcp SDR every second (port 1234) and report\n"
              << "                      the strongest signal: its offset from the predicted carrier and its SNR\n"
              << "  --sdr-rate S/s      SDR sample rate (default 1024000)    --sdr-gain DB  manual gain (default: tuner AGC)\n"
              << "  --sdr-ppm N         SDR frequency correction in ppm\n";
}

double nowUnix() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// A whole string as a number, or nothing.
std::optional<double> number(std::string_view text) {
    double v = 0.0;
    const auto [end, ec] = gygax::fromChars(text.data(), text.data() + text.size(), v);
    if (text.empty() || ec != std::errc() || end != text.data() + text.size() || !std::isfinite(v)) return std::nullopt;
    return v;
}

std::vector<std::string_view> split(std::string_view text, char sep) {
    std::vector<std::string_view> out;
    for (std::size_t from = 0;;) {
        const auto at = text.find(sep, from);
        out.push_back(text.substr(from, at == std::string_view::npos ? std::string_view::npos : at - from));
        if (at == std::string_view::npos) return out;
        from = at + 1;
    }
}

// 'now', Unix seconds, or UTC as YYYY-MM-DD[THH:MM[:SS[.fff]]].
std::optional<double> parseUtc(const std::string& text) {
    if (text.empty() || text == "now") return nowUnix();
    if (text.find('-', 1) == std::string::npos) return number(text);
    const std::string_view all(text);
    const auto t = all.find_first_of("T ");
    const auto date = split(all.substr(0, t), '-');
    const auto clock = t == std::string_view::npos ? std::vector<std::string_view>{} : split(all.substr(t + 1), ':');
    if (date.size() != 3 || clock.size() == 1 || clock.size() > 3) return std::nullopt;
    double parts[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0}; // year month day hour minute second
    for (std::size_t i = 0; i < 3 + clock.size(); ++i) {
        const auto v = number(i < 3 ? date[i] : clock[i - 3]);
        if (!v) return std::nullopt;
        parts[i] = *v;
    }
    using namespace std::chrono;
    const year_month_day ymd{year{static_cast<int>(parts[0])}, month{static_cast<unsigned>(parts[1])},
                             day{static_cast<unsigned>(parts[2])}};
    if (!ymd.ok() || parts[3] >= 24 || parts[4] >= 60 || parts[5] >= 61) return std::nullopt;
    return static_cast<double>(sys_days{ymd}.time_since_epoch().count()) * 86400.0 + parts[3] * 3600.0 + parts[4] * 60.0 + parts[5];
}

std::string utc(double unix_seconds, bool withDate = true) {
    using namespace std::chrono;
    const sys_seconds t{seconds{static_cast<long long>(std::floor(unix_seconds + 0.5))}};
    return withDate ? std::format("{:%F %T}", t) : std::format("{:%T}", t);
}

std::string duration(double seconds) {
    const auto s = static_cast<long long>(std::llround(seconds));
    if (s >= 3600) return std::format("{}h{:02}m", s / 3600, (s % 3600) / 60);
    return std::format("{}m{:02}s", s / 60, s % 60);
}

std::string compass(double azimuth_deg) {
    static constexpr const char* kPoints[] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                              "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    return kPoints[static_cast<int>(std::floor(azimuth_deg / 22.5 + 0.5)) % 16];
}

std::string kilohertz(double hz) {
    return std::format("{:+.2f} kHz", hz / 1000.0);
}

// Reads every element set in a file; a line before "1 ..." that is not itself an element line names it.
std::vector<satlink::Tle> readTles(std::istream& in) {
    std::vector<satlink::Tle> out;
    std::string line, name, line1;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line.starts_with("#")) continue;
        if (line.starts_with("1 ") && line.size() >= 64) {
            line1 = line;
        } else if (line.starts_with("2 ") && !line1.empty()) {
            out.push_back(satlink::Tle::parse(line1, line, name));
            line1.clear();
            name.clear();
        } else {
            name = line;
            line1.clear();
        }
    }
    return out;
}

int cmdTrack(const Args& a) {
    if (a.has("help") || a.has("h")) {
        printTrackUsage();
        return 0;
    }
    if (!a.has("tle") || !a.has("at")) {
        printTrackUsage();
        return 2;
    }
    try {
        std::vector<satlink::Tle> tles;
        if (a.get("tle") == "-") {
            tles = readTles(std::cin);
        } else {
            std::ifstream f(a.get("tle"));
            if (!f) throw std::runtime_error("cannot read " + a.get("tle"));
            tles = readTles(f);
        }
        if (tles.empty()) throw std::runtime_error("no two-line element sets in " + a.get("tle"));

        const satlink::Tle* chosen = &tles.front();
        if (a.has("sat")) {
            const auto want = a.get("sat");
            auto lower = [](std::string t) {
                for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return t;
            };
            chosen = nullptr;
            for (const auto& t : tles) {
                if (t.catalog == want || lower(t.name).find(lower(want)) != std::string::npos) {
                    chosen = &t;
                    break;
                }
            }
            if (chosen == nullptr) throw std::runtime_error("no satellite matching '" + want + "'");
        }

        satlink::Observer station;
        {
            const auto at = a.get("at");
            const auto fields = split(at, ',');
            const auto lat = fields.size() >= 2 ? number(fields[0]) : std::nullopt;
            const auto lon = fields.size() >= 2 ? number(fields[1]) : std::nullopt;
            const auto height = fields.size() == 3 ? number(fields[2]) : std::optional<double>(0.0);
            if (fields.size() > 3 || !lat || !lon || !height || std::abs(*lat) > 90.0 || std::abs(*lon) > 180.0)
                throw std::runtime_error("--at wants LAT,LON[,METRES] in degrees, e.g. 51.4779,-0.0015,46");
            station = {*lat, *lon, *height};
        }
        const auto start = parseUtc(a.get("start", "now"));
        if (!start) throw std::runtime_error("--start wants 'now', Unix seconds or YYYY-MM-DD[THH:MM[:SS]]");
        const auto option = [&](const char* name, const char* fallback) {
            const auto v = number(a.get(name, fallback));
            if (!v) throw std::runtime_error(std::format("--{} wants a number, not '{}'", name, a.get(name)));
            return *v;
        };
        const double hours = option("hours", "24");
        const double minEl = option("min-elevation", "10");
        const double downlink = option("downlink", "0");
        if (!(hours > 0.0)) throw std::runtime_error("--hours must be positive");
        if (downlink < 0.0) throw std::runtime_error("--downlink must be a frequency in Hz");

        const satlink::Satellite sat(*chosen);
        const auto& t = sat.tle();
        const auto title = t.name.empty() ? "NORAD " + t.catalog : t.name;
        std::cout << std::format("{}  ·  NORAD {}  ·  epoch {} UTC  ·  period {:.1f} min  ·  {}\n", title, t.catalog,
                                 utc(t.epochUnixSeconds()), t.periodMinutes(), sat.deepSpace() ? "SDP4 (deep space)" : "SGP4");
        std::cout << std::format("station {:.4f}°{} {:.4f}°{} {:.0f} m", std::abs(station.latitude_deg),
                                 station.latitude_deg >= 0 ? 'N' : 'S', std::abs(station.longitude_deg),
                                 station.longitude_deg >= 0 ? 'E' : 'W', station.elevation_m);
        const double age = (*start - t.epochUnixSeconds()) / 86400.0;
        if (std::abs(age) > 14.0) std::cout << std::format("  ·  elements are {:.0f} days from epoch: refresh them for accuracy", age);
        std::cout << "\n\n";

        if (a.has("live")) {
            installSignals();
            const long count = std::lround(option("count", "0"));
            std::optional<satlink::RigClient> rig;
            if (a.has("rig")) {
                if (!(downlink > 0.0)) throw std::runtime_error("--rig needs --downlink HZ");
                auto spec = a.get("rig");
                int port = satlink::kHamlibDefaultPort;
                if (const auto colon = spec.rfind(':'); colon != std::string::npos) {
                    const auto p = number(std::string_view(spec).substr(colon + 1));
                    if (!p) throw std::runtime_error("--rig wants HOST[:PORT]");
                    port = static_cast<int>(*p);
                    spec.resize(colon);
                }
                rig.emplace(spec, port);
            }
            std::optional<satlink::RtlTcpClient> sdr;
            constexpr std::size_t kSdrFft = 8192, kSdrCapture = 8 * kSdrFft;
            if (a.has("sdr")) {
                if (!(downlink > 0.0)) throw std::runtime_error("--sdr needs --downlink HZ");
                auto spec = a.get("sdr");
                int port = satlink::kRtlTcpDefaultPort;
                if (const auto colon = spec.rfind(':'); colon != std::string::npos) {
                    const auto p = number(std::string_view(spec).substr(colon + 1));
                    if (!p) throw std::runtime_error("--sdr wants HOST[:PORT]");
                    port = static_cast<int>(*p);
                    spec.resize(colon);
                }
                sdr.emplace(spec, port);
                sdr->setSampleRate(static_cast<std::uint32_t>(option("sdr-rate", "1024000")));
                if (a.has("sdr-gain")) {
                    sdr->setGainDb(option("sdr-gain", "0"));
                } else {
                    sdr->setAutomaticGain();
                }
                if (a.has("sdr-ppm")) sdr->setFrequencyCorrection(static_cast<int>(std::lround(option("sdr-ppm", "0"))));
                std::cout << std::format("sdr     rtl_tcp {}:{}  ·  {} tuner  ·  {} S/s  ·  {:.1f} Hz bins\n\n", spec, port,
                                         sdr->info().tunerName(), sdr->sampleRate(), sdr->sampleRate() / static_cast<double>(kSdrFft));
            }
            std::cout << "  time (UTC)    azimuth      elev      range     rate km/s"
                      << (downlink > 0.0 ? "   doppler       receive Hz" : "") << (sdr ? "   sdr peak     snr" : "") << "\n";
            const bool fromNow = a.get("start", "now") == "now";
            for (long i = 0; (count <= 0 || i < count) && !gStop.load(); ++i) {
                const double when = fromNow ? nowUnix() : *start + static_cast<double>(i);
                const auto look = sat.observe(station, when);
                std::cout << std::format("  {}   {:6.1f}° {:<3}  {:+6.1f}°  {:7.1f} km  {:+7.3f}", utc(when, false), look.azimuth_deg,
                                         compass(look.azimuth_deg), look.elevation_deg, look.range_km, look.range_rate_km_s);
                if (downlink > 0.0) {
                    const double shift = satlink::dopplerShiftHz(downlink, look.range_rate_km_s);
                    std::cout << std::format("   {:>11}   {:>12.0f}", kilohertz(shift), downlink + shift);
                    if (rig) {
                        try {
                            rig->setFrequency(downlink + shift);
                            std::cout << "  rig ok";
                        } catch (const satlink::RigError& e) {
                            std::cout << "  rig: " << e.what();
                        }
                    }
                    if (sdr) {
                        try {
                            // Retune, let the queue of samples from the old frequency drain, then look.
                            sdr->setCenterFrequency(downlink + shift);
                            sdr->discard(sdr->sampleRate() / 20);
                            const auto iq = sdr->read(kSdrCapture);
                            const auto peak = satlink::strongestPeak(satlink::powerSpectrumDb(iq, kSdrFft), sdr->sampleRate());
                            std::cout << std::format("   {:+8.0f} Hz  {:4.1f} dB", peak.offset_hz, peak.snr_db);
                        } catch (const satlink::SdrError& e) {
                            std::cout << "  sdr: " << e.what();
                        }
                    }
                }
                std::cout << (look.elevation_deg >= 0.0 ? "" : "  (below horizon)") << std::endl;
                // Following the clock: wake on the next whole second, so lines land on the tick.
                if (fromNow && (count <= 0 || i + 1 < count)) {
                    using namespace std::chrono;
                    const auto now = system_clock::now();
                    std::this_thread::sleep_until(ceil<seconds>(now + milliseconds(1)));
                }
            }
            return 0;
        }

        const auto passes = sat.passes(station, *start, *start + hours * 3600.0, minEl);
        std::cout << std::format("{} pass{} above {:g}° in the {:g} hours from {} UTC\n\n", passes.size(), passes.size() == 1 ? "" : "es",
                                 minEl, hours, utc(*start));
        if (passes.empty()) return 0;
        std::cout << "  rise (UTC)            from       peak (UTC)  max el   set (UTC)   to         lasts"
                  << (downlink > 0.0 ? "    doppler rise → set" : "") << "\n";
        for (const auto& p : passes) {
            std::cout << std::format("  {}{}  {:5.1f}° {:<3}  {}    {:5.1f}°  {}{}  {:5.1f}° {:<3}  {:>6}", utc(p.rise_unix),
                                     p.rise_clipped ? "<" : " ", p.rise_azimuth_deg, compass(p.rise_azimuth_deg),
                                     utc(p.culmination_unix, false), p.max_elevation_deg, utc(p.set_unix, false), p.set_clipped ? ">" : " ",
                                     p.set_azimuth_deg, compass(p.set_azimuth_deg), duration(p.durationSeconds()));
            if (downlink > 0.0) {
                std::cout << std::format("    {} → {}", kilohertz(sat.dopplerShiftHz(station, p.rise_unix, downlink)),
                                         kilohertz(sat.dopplerShiftHz(station, p.set_unix, downlink)));
            }
            std::cout << "\n";
        }
        if (std::any_of(passes.begin(), passes.end(), [](const auto& p) { return p.rise_clipped || p.set_clipped; }))
            std::cout << "\n  < already up when the window opened   > still up when it closed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "gygax track: " << e.what() << "\n";
        return 1;
    }
}

struct Remote {
    std::string base;
    std::string token;

    net::ClientResponse call(const std::string& method, const std::string& path, const std::string& body = {}) const {
        std::string error;
        auto url = net::Url::parse(base + path, &error);
        if (!url) {
            net::ClientResponse r;
            r.error = error;
            return r;
        }
        net::Headers h;
        h["Content-Type"] = "application/json";
        if (!token.empty()) h["Authorization"] = "Bearer " + token;
        net::ClientOptions o;
        o.readTimeout = std::chrono::seconds(180);
        return net::httpRequest(*url, method, body, h, o);
    }
};

Remote remoteFrom(const Args& a) {
    Remote r;
    r.base = a.get("url", "http://127.0.0.1:1984");
    while (r.base.size() > 1 && r.base.back() == '/') r.base.pop_back();
    const char* envToken = std::getenv("GYGAX_API_TOKEN");
    r.token = a.get("token", envToken != nullptr ? envToken : "");
    return r;
}

int cmdStatus(const Args& a) {
    const auto remote = remoteFrom(a);
    for (const char* path : {"/v1/node", "/v1/engines", "/v1/agents"}) {
        auto r = remote.call("GET", path);
        if (!r.error.empty() || r.status != 200) {
            std::cerr << "gygax: GET " << path << " failed: " << (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error) << "\n";
            return 1;
        }
        auto parsed = gygax::json::parse(r.body);
        std::cout << "== " << path << " ==\n" << (parsed ? parsed->dump(2) : r.body) << "\n";
    }
    return 0;
}

int cmdAsk(const Args& a) {
    if (a.positional.empty()) {
        std::cerr << "gygax: ask needs a prompt\n";
        return 2;
    }
    const auto remote = remoteFrom(a);
    Value body = Value::object();
    std::string model = a.get("model", a.has("agent") ? "gygax-agent" : "");
    if (model.empty()) {
        auto models = remote.call("GET", "/v1/models");
        auto parsed = gygax::json::parse(models.body);
        if (parsed && parsed->find("data") && !parsed->find("data")->asArray().empty())
            model = parsed->find("data")->asArray().front().getString("id");
    }
    body["model"] = model;
    Value msg = Value::object();
    msg["role"] = "user";
    msg["content"] = a.positional[0];
    body["messages"].push(std::move(msg));
    auto r = remote.call("POST", "/v1/chat/completions", body.dump());
    if (!r.error.empty()) {
        std::cerr << "gygax: " << r.error << "\n";
        return 1;
    }
    auto parsed = gygax::json::parse(r.body);
    if (r.status != 200 || !parsed) {
        std::cerr << "gygax: HTTP " << r.status << ": " << r.body << "\n";
        return 1;
    }
    const auto* choices = parsed->find("choices");
    std::cout << (choices && !choices->asArray().empty() ? choices->asArray().front().find("message")->getString("content") : std::string())
              << "\n";
    return 0;
}

struct Check {
    std::string name;
    std::function<std::string()> run;
};

class Doctor {
public:
    void add(std::string name, std::function<std::string()> fn) { checks_.push_back({std::move(name), std::move(fn)}); }

    int run(bool asJson) {
        Value report = Value::object();
        Value list = Value::array();
        int failed = 0;
        for (const auto& c : checks_) {
            const auto start = std::chrono::steady_clock::now();
            std::string failure;
            try {
                failure = c.run();
            } catch (const std::exception& e) {
                failure = std::string("exception: ") + e.what();
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (!failure.empty()) ++failed;
            Value item = Value::object();
            item["name"] = c.name;
            item["ok"] = failure.empty();
            item["ms"] = ms;
            if (!failure.empty()) item["detail"] = failure;
            list.push(std::move(item));
            if (!asJson) {
                std::cout << (failure.empty() ? "PASS " : "FAIL ") << c.name << " (" << ms << " ms)";
                if (!failure.empty()) std::cout << "\n     " << failure;
                std::cout << "\n";
            }
        }
        report["checks"] = std::move(list);
        report["passed"] = static_cast<std::int64_t>(checks_.size()) - failed;
        report["failed"] = failed;
        report["version"] = GYGAX_VERSION_STRING;
        if (asJson)
            std::cout << report.dump(2) << "\n";
        else
            std::cout << "\n" << (checks_.size() - static_cast<std::size_t>(failed)) << "/" << checks_.size() << " checks passed\n";
        return failed == 0 ? 0 : 1;
    }

private:
    std::vector<Check> checks_;
};

std::string expect(bool cond, const std::string& message) {
    return cond ? std::string() : message;
}

std::string expectStatus(const net::ClientResponse& r, int status) {
    if (!r.error.empty()) return "transport error: " + r.error;
    if (r.status != status)
        return "expected HTTP " + std::to_string(status) + " got " + std::to_string(r.status) + ": " + r.body.substr(0, 200);
    return {};
}

Value chatBody(const std::string& model, const std::string& content, bool stream = false) {
    Value b = Value::object();
    b["model"] = model;
    b["stream"] = stream;
    Value m = Value::object();
    m["role"] = "user";
    m["content"] = content;
    b["messages"].push(std::move(m));
    return b;
}

std::string chatContent(const std::string& body) {
    auto parsed = gygax::json::parse(body);
    if (!parsed || !parsed->find("choices") || parsed->find("choices")->asArray().empty()) return {};
    return parsed->find("choices")->asArray().front().find("message")->getString("content");
}

void addRemoteChecks(Doctor& d, const Remote& remote, bool full) {
    d.add("http: GET /healthz", [remote] { return expectStatus(remote.call("GET", "/healthz"), 200); });
    d.add("http: GET /readyz reports ready", [remote] { return expectStatus(remote.call("GET", "/readyz"), 200); });
    d.add("http: GET /version", [remote] {
        auto r = remote.call("GET", "/version");
        if (auto e = expectStatus(r, 200); !e.empty()) return e;
        auto p = gygax::json::parse(r.body);
        return expect(p && p->getString("version") == GYGAX_VERSION_STRING, "version mismatch: " + r.body);
    });
    d.add("api: GET /v1/models lists models", [remote] {
        auto r = remote.call("GET", "/v1/models");
        if (auto e = expectStatus(r, 200); !e.empty()) return e;
        auto p = gygax::json::parse(r.body);
        return expect(p && p->find("data") && !p->find("data")->asArray().empty(), "no models returned");
    });
    d.add("api: GET /v1/node reports hardware", [remote] {
        auto r = remote.call("GET", "/v1/node");
        if (auto e = expectStatus(r, 200); !e.empty()) return e;
        auto p = gygax::json::parse(r.body);
        return expect(p && p->getInt("cpu_cores") > 0 && p->getInt("memory_total_mb") > 0, "node info incomplete");
    });
    d.add("api: GET /v1/tools includes builtin tools", [remote] {
        auto r = remote.call("GET", "/v1/tools");
        if (auto e = expectStatus(r, 200); !e.empty()) return e;
        return expect(r.body.find("math.eval") != std::string::npos && r.body.find("neuro.run") != std::string::npos,
                      "builtin tools missing");
    });
    d.add("api: POST /v1/tools/math.eval/invoke", [remote] {
        Value b = Value::object();
        b["input"] = "2 + 3 * (4 - 1) ^ 2";
        auto r = remote.call("POST", "/v1/tools/math.eval/invoke", b.dump());
        if (auto e = expectStatus(r, 200); !e.empty()) return e;
        auto p = gygax::json::parse(r.body);
        return expect(p && p->getString("output") == "29", "math.eval returned " + r.body);
    });
    d.add("api: POST /v1/neuro/simulate", [remote] {
        auto r = remote.call("POST", "/v1/neuro/simulate", demoSpec().dump());
        if (auto e = expectStatus(r, 200); !e.empty()) return e;
        auto p = gygax::json::parse(r.body);
        return expect(p && p->find("results") && p->find("results")->asArray().size() == 3, "unexpected simulation output");
    });
    d.add("api: GET /metrics exposes gygax_build_info", [remote] {
        auto r = remote.call("GET", "/metrics");
        if (auto e = expectStatus(r, 200); !e.empty()) return e;
        return expect(r.body.find("gygax_build_info") != std::string::npos, "metrics missing build info");
    });
    d.add("api: unknown route returns 404 JSON", [remote] {
        auto r = remote.call("GET", "/v1/nope");
        if (auto e = expectStatus(r, 404); !e.empty()) return e;
        return expect(r.body.find("not_found") != std::string::npos, "error body malformed");
    });
    d.add("api: malformed JSON returns 400",
          [remote] { return expectStatus(remote.call("POST", "/v1/chat/completions", "{not json"), 400); });
    d.add("rpc: POST /rpc node.info", [remote] {
        auto r = remote.call("POST", "/rpc", R"({"jsonrpc":"2.0","id":7,"method":"node.info"})");
        if (auto e = expectStatus(r, 200); !e.empty()) return e;
        auto p = gygax::json::parse(r.body);
        return expect(p && p->getInt("id") == 7 && p->find("result"), "rpc failed: " + r.body);
    });
    if (!full) return;
    d.add("agent: create, run tool loop, read answer", [remote] {
        auto created = remote.call("POST", "/v1/agents", R"({"name":"doctor"})");
        if (auto e = expectStatus(created, 201); !e.empty()) return e;
        auto sid = gygax::json::parse(created.body)->getInt("id");
        Value b = Value::object();
        b["objective"] = "!tool math.eval 6*7";
        b["model"] = "echo";
        b["wait_seconds"] = 30;
        auto run = remote.call("POST", "/v1/agents/" + std::to_string(sid) + "/objectives", b.dump());
        if (auto e = expectStatus(run, 200); !e.empty()) return e;
        auto snap = gygax::json::parse(run.body);
        std::string result =
            expect(snap && snap->getString("status") == "completed" && snap->getString("answer") == "42", "agent result: " + run.body);
        auto mem = remote.call("GET", "/v1/agents/" + std::to_string(sid) + "/memory");
        if (result.empty() && mem.body.find("math.eval") == std::string::npos) result = "memory does not record the tool call";
        remote.call("DELETE", "/v1/agents/" + std::to_string(sid));
        return result;
    });
    d.add("chat: completion via the echo engine", [remote] {
        auto r = remote.call("POST", "/v1/chat/completions", chatBody("echo", "ping").dump());
        if (auto e = expectStatus(r, 200); !e.empty()) return e;
        return expect(chatContent(r.body) == "echo: ping", "unexpected completion: " + r.body);
    });
    d.add("chat: SSE streaming terminates with [DONE]", [remote] {
        auto r = remote.call("POST", "/v1/chat/completions", chatBody("echo", "stream me", true).dump());
        if (auto e = expectStatus(r, 200); !e.empty()) return e;
        return expect(r.body.find("data: [DONE]") != std::string::npos && r.body.find("echo: stream me") != std::string::npos,
                      "stream malformed: " + r.body.substr(0, 300));
    });
    d.add("chat: gygax-agent model runs the tool loop", [remote] {
        auto r = remote.call("POST", "/v1/chat/completions", chatBody("gygax-agent:echo", "!tool math.eval 10/4").dump());
        if (auto e = expectStatus(r, 200); !e.empty()) return e;
        return expect(chatContent(r.body) == "2.5", "agent completion: " + r.body);
    });
    d.add("ollama: POST /api/chat (non-stream)", [remote] {
        Value b = chatBody("echo", "hello");
        b["stream"] = false;
        auto r = remote.call("POST", "/api/chat", b.dump());
        if (auto e = expectStatus(r, 200); !e.empty()) return e;
        auto p = gygax::json::parse(r.body);
        return expect(p && p->find("message") && p->find("message")->getString("content") == "echo: hello" && p->getBool("done"),
                      "ollama reply: " + r.body);
    });
}

int cmdDoctor(const Args& a) {
    gygax::log::setLevel(gygax::log::Level::Error);
    applyLogging(a);
    Doctor d;
    const bool asJson = a.has("json");

    if (a.has("url")) {
        auto remote = remoteFrom(a);
        addRemoteChecks(d, remote, a.has("full"));
        return d.run(asJson);
    }

    d.add("core: JSON round trip with unicode and nesting", [] {
        auto v = gygax::json::parse(R"({"a":[1,2.5,"x\u00e9\ud83d\ude00",null,true],"b":{"c":-3}})");
        if (!v) return std::string("parse failed");
        auto again = gygax::json::parse(v->dump());
        return expect(again && *again == *v && v->find("b")->getInt("c") == -3, "round trip changed the document");
    });
    d.add("core: JSON rejects malformed documents", [] {
        for (const char* bad : {"{", "[1,]", "01", "\"\\ud800\"", "{\"a\":1} x", "nul"}) {
            if (gygax::json::parse(bad)) return std::string("accepted malformed input: ") + bad;
        }
        return std::string();
    });
    d.add("core: expression evaluator", [] {
        double v = 0;
        std::string err;
        if (!service::evaluateExpression("sqrt(16) + 2^3 * (1 + 1)", v, err)) return err;
        if (v != 20.0) return "sqrt(16)+2^3*2 = " + std::to_string(v);
        return expect(!service::evaluateExpression("1/0", v, err), "division by zero accepted");
    });
    d.add("neuro: LIF firing rate matches the analytic F-I curve", [] {
        for (double current : {16.0, 20.0, 30.0}) {
            gygax::neuro::Network net(0.05, 1);
            gygax::neuro::LifParams p;
            auto pop = net.addLif("n", 1, p);
            net.setBias(pop, current);
            net.run(4000);
            const double expected = gygax::neuro::lifSteadyStateRateHz(p, current);
            if (std::abs(net.meanRateHz(pop) - expected) > 0.05 * expected + 0.5) {
                return "I=" + std::to_string(current) + " simulated " + std::to_string(net.meanRateHz(pop)) + " Hz, analytic " +
                       std::to_string(expected);
            }
        }
        return std::string();
    });
    d.add("neuro: identical seeds give identical rasters", [] {
        auto run = [] {
            auto spec = demoSpec();
            spec["run_ms"] = 200;
            spec["raster_limit"] = 100000;
            std::string err;
            return gygax::neuro::simulateSpec(spec, &err).dump();
        };
        const auto first = run();
        return expect(!first.empty() && first == run(), "simulation is not deterministic");
    });

    static std::unique_ptr<service::Service> upstream;
    static std::unique_ptr<service::Service> primary;
    static std::unique_ptr<service::Service> router;
    static std::string token = "doctor-token";

    d.add("logistics: ledger tracks production and loss with guids", [] {
        gygax::logistics::Ledger ledger;
        auto made = ledger.record(*gygax::json::parse(R"({"line":"+8 ba99x drone=quadcopter power=solar"})"));
        if (!made.ok || made.body.find("guids")->size() != 8) return std::string("production event failed: ") + made.message;
        auto lost = ledger.record(*gygax::json::parse(R"({"line":"-3 ba99x drone=quadcopter power=solar"})"));
        if (!lost.ok) return std::string("loss event failed: ") + lost.message;
        const auto totals = ledger.summary(0).find("totals")->getInt("active");
        return expect(totals == 5 && ledger.units("ba99x", "lost", 10).size() == 3, "unexpected inventory");
    });
    d.add("logistics: planner inserts a refuel stop", [] {
        gygax::logistics::PlanRequest req;
        req.origin = {0.0, 0.0};
        req.waypoints = {{0.0, 0.3}};
        req.maxRangeM = 20000;
        gygax::logistics::Site site;
        site.id = "mid";
        site.position = {0.0, 0.15};
        const auto plan = gygax::logistics::planRoute(req, {site});
        return expect(plan.getBool("feasible") && plan.getInt("refuel_stops") == 1, "plan: " + plan.dump());
    });
    d.add("satlink: SGP4 and SDP4 reproduce the Vallado reference to 1 mm", [] {
        // Vanguard 1 (near Earth) and 04632 (deep space, 20 h) from tcppver.out; TEME km.
        struct Case {
            const char* l1;
            const char* l2;
            double minutes;
            satlink::Vector3 r;
        };
        const Case cases[] = {
            {"1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753",
             "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667",
             360.0,
             {-7154.03120202, -3783.17682504, -3536.19412294}},
            {"1 04632U 70093B   04031.91070959 -.00000084  00000-0  10000-3 0  9955",
             "2 04632  11.4628 273.1101 1450506 207.6000 143.9350  1.20231981 44145",
             -5184.0,
             {-29020.02587128, 13819.84419063, -5713.33679183}},
        };
        for (const auto& c : cases) {
            const satlink::Sgp4 sgp4(satlink::Tle::parse(c.l1, c.l2));
            const double err = (sgp4.propagate(c.minutes).position_km - c.r).norm();
            if (err > 1e-6)
                return std::format("{} at {} min is {:.3f} mm from the reference", std::string(c.l1).substr(2, 5), c.minutes, err * 1e6);
        }
        return std::string();
    });
    d.add("robotics: MAVLink client flies the virtual autopilot", [] {
        auto [autopilotLink, clientLink] = net::makeLinkPair();
        gygax::robotics::VirtualAutopilotOptions options;
        options.climbRate = 50.0;
        gygax::robotics::VirtualAutopilot pilot(autopilotLink, options);
        pilot.start();
        gygax::robotics::mavlink::Vehicle vehicle(clientLink);
        vehicle.start();
        using namespace std::chrono_literals;
        if (!vehicle.waitForHeartbeat(3000ms)) return std::string("no heartbeat");
        if (vehicle.setMode(gygax::robotics::VirtualAutopilot::kModeGuided) != 0 || vehicle.arm() != 0 || vehicle.takeoff(10.0) != 0)
            return std::string("command rejected");
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline && vehicle.state().altitudeRelative < 9.5) std::this_thread::sleep_for(20ms);
        const auto altitude = vehicle.state().altitudeRelative;
        vehicle.stop();
        pilot.stop();
        return expect(altitude >= 9.5, "climb stalled at " + std::to_string(altitude));
    });
    d.add("service: start upstream node on an ephemeral port", [] {
        service::ServiceConfig c;
        c.port = 0;
        c.token = token;
        c.engines = {"echo"};
        upstream = std::make_unique<service::Service>(c);
        std::string err;
        return upstream->start(&err) ? std::string() : err;
    });
    d.add("service: refuses non-loopback bind without a token", [] {
        service::ServiceConfig c;
        c.host = "0.0.0.0";
        c.port = 0;
        service::Service s(c);
        std::string err;
        return expect(!s.start(&err) && err.find("GYGAX_API_TOKEN") != std::string::npos, "insecure bind was allowed");
    });
    d.add("service: start primary node", [] {
        service::ServiceConfig c;
        c.port = 0;
        c.token = token;
        c.engines = {"echo"};
        primary = std::make_unique<service::Service>(c);
        std::string err;
        return primary->start(&err) ? std::string() : err;
    });

    auto remoteFor = [](service::Service& s, const std::string& tok) {
        Remote r;
        r.base = "http://127.0.0.1:" + std::to_string(s.port());
        r.token = tok;
        return r;
    };
    d.add("auth: missing token is rejected with 401",
          [remoteFor] { return expectStatus(remoteFor(*primary, "").call("GET", "/v1/models"), 401); });
    d.add("auth: wrong token is rejected with 401",
          [remoteFor] { return expectStatus(remoteFor(*primary, "wrong").call("GET", "/v1/models"), 401); });
    d.add("auth: health endpoints stay public", [remoteFor] { return expectStatus(remoteFor(*primary, "").call("GET", "/healthz"), 200); });
    d.add("http: oversized body is rejected with 413", [remoteFor] {
        return expectStatus(remoteFor(*primary, token).call("POST", "/v1/chat/completions", std::string(9 * 1024 * 1024, 'x')), 413);
    });

    d.add("suite: full API against the primary node", [remoteFor] {
        Doctor inner;
        addRemoteChecks(inner, remoteFor(*primary, token), true);
        std::ostringstream capture;
        auto* old = std::cout.rdbuf(capture.rdbuf());
        const int rc = inner.run(false);
        std::cout.rdbuf(old);
        return rc == 0 ? std::string() : capture.str();
    });

    d.add("concurrency: 64 parallel chat requests", [remoteFor] {
        auto remote = remoteFor(*primary, token);
        std::vector<std::future<std::string>> jobs;
        jobs.reserve(64);
        for (int i = 0; i < 64; ++i) {
            jobs.push_back(std::async(std::launch::async, [remote, i] {
                auto r = remote.call("POST", "/v1/chat/completions", chatBody("echo", "req" + std::to_string(i)).dump());
                if (r.status != 200) return "HTTP " + std::to_string(r.status) + " " + r.error;
                return chatContent(r.body) == "echo: req" + std::to_string(i) ? std::string() : "wrong content for " + std::to_string(i);
            }));
        }
        for (auto& j : jobs) {
            if (auto e = j.get(); !e.empty()) return e;
        }
        return std::string();
    });

    d.add("routing: fails over from an engine that errors to a healthy one", [remoteFor] {
        static net::Server flaky([] {
            net::ServerOptions o;
            o.port = 0;
            return o;
        }());
        static bool flakyStarted = false;
        if (!flakyStarted) {
            flaky.route("GET", "/v1/models", [](net::Request&) { return net::Response::json(200, R"({"data":[{"id":"echo"}]})"); });
            flaky.route("POST", "/v1/chat/completions",
                        [](net::Request&) { return net::Response::error(500, "boom", "simulated engine failure"); });
            std::string startError;
            if (!flaky.start(&startError)) return startError;
            flakyStarted = true;
        }
        service::ServiceConfig c;
        c.port = 0;
        c.token = token;
        c.engines = {"http://127.0.0.1:" + std::to_string(flaky.port()) + "/v1",
                     "http://127.0.0.1:" + std::to_string(upstream->port()) + "/v1;key=" + token};
        router = std::make_unique<service::Service>(c);
        std::string err;
        if (!router->start(&err)) return err;
        auto remote = remoteFor(*router, token);
        auto r = remote.call("POST", "/v1/chat/completions", chatBody("echo", "via router").dump());
        if (auto e = expectStatus(r, 200); !e.empty()) return e;
        if (chatContent(r.body) != "echo: via router") return "router reply: " + r.body;
        auto engines = remote.call("GET", "/v1/engines");
        auto parsed = gygax::json::parse(engines.body);
        if (!parsed) return std::string("engines endpoint returned invalid JSON");
        int failed = 0;
        int completed = 0;
        for (const auto& e : parsed->find("engines")->asArray()) {
            failed += static_cast<int>(e.getInt("failed"));
            completed += static_cast<int>(e.getInt("completed"));
        }
        return expect(failed == 1 && completed == 1, "expected one failed and one completed engine call: " + engines.body);
    });
    d.add("routing: a dead engine is detected by probing and excluded", [remoteFor] {
        service::ServiceConfig c;
        c.port = 0;
        c.token = token;
        c.engines = {"http://127.0.0.1:1/v1", "http://127.0.0.1:" + std::to_string(upstream->port()) + "/v1;key=" + token};
        service::Service dead(c);
        std::string err;
        if (!dead.start(&err)) return err;
        auto remote = remoteFor(dead, token);
        auto engines = gygax::json::parse(remote.call("GET", "/v1/engines").body);
        if (!engines) return std::string("engines endpoint returned invalid JSON");
        int healthy = 0;
        for (const auto& e : engines->find("engines")->asArray()) healthy += e.getBool("healthy") ? 1 : 0;
        if (healthy != 1) return "expected exactly one healthy engine, got " + std::to_string(healthy);
        return expectStatus(remote.call("POST", "/v1/chat/completions", chatBody("echo", "x").dump()), 200);
    });

    d.add("routing: peer nodes are discovered as engines and never loop", [remoteFor] {
        service::ServiceConfig c;
        c.port = 0;
        c.token = token;
        c.engines = {"echo"};
        c.peers = {"http://127.0.0.1:" + std::to_string(upstream->port())};
        service::Service withPeer(c);
        std::string err;
        if (!withPeer.start(&err)) return err;
        auto remote = remoteFor(withPeer, token);
        auto cluster = remote.call("GET", "/v1/cluster");
        if (auto e = expectStatus(cluster, 200); !e.empty()) return e;
        auto parsed = gygax::json::parse(cluster.body);
        if (!parsed || parsed->find("peers")->asArray().size() != 1) return "cluster view: " + cluster.body;
        if (!parsed->find("peers")->asArray().front().getBool("healthy")) return "peer not healthy: " + cluster.body;
        auto forwarded =
            net::httpRequest(*net::Url::parse(remote.base + "/v1/chat/completions"), "POST", chatBody("gygax-agent", "x").dump(),
                             {{"Authorization", "Bearer " + token}, {"Content-Type", "application/json"}, {"X-Gygax-Forwarded", "1"}});
        return expectStatus(forwarded, 400);
    });

    const int rc = d.run(asJson);
    if (router) router->stop();
    if (primary) primary->stop();
    if (upstream) upstream->stop();
    return rc;
}

}

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        printUsage();
        return 0;
    }
    if (cmd == "version" || cmd == "--version") {
        std::cout << "gygax " << GYGAX_VERSION_STRING;
        if (!std::string_view(GYGAX_VERSION_CODENAME).empty()) std::cout << " \"" << GYGAX_VERSION_CODENAME << "\"";
        std::cout << "\n";
        return 0;
    }
    const auto args = Args::parse(argc, argv, 2);
    if (cmd == "serve") return cmdServe(args);
    if (cmd == "doctor") return cmdDoctor(args);
    if (cmd == "rpc") return cmdRpc(args);
    if (cmd == "sim-autopilot") return cmdSimAutopilot(args);
    if (cmd == "neuro") return cmdNeuro(args);
    if (cmd == "status") return cmdStatus(args);
    if (cmd == "ask") return cmdAsk(args);
    if (cmd == "track") return cmdTrack(args);
    std::cerr << "gygax: unknown command '" << cmd << "'\n\n";
    printUsage();
    return 2;
}
