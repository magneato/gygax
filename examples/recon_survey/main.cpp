// Recon survey: a Local-level monitor cross-checks a site's "all secure" status report against OSINT
// events, verifies a survey photograph's geolocation, and raises an evacuation alarm on a discrepancy.
//
//   recon_survey [--seconds N] [--evac-test] [--feed events.json] [--verify-image photo.jpg]
//                [--report out.json] [--keys]
//
// --keys reads single keystrokes from the terminal: O toggles the overlay, L cycles its level,
// E forces evacuation, Q quits.
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <gygax/core/json.hpp>
#include <gygax/intel/metadata.hpp>
#include <gygax/intel/osint.hpp>

namespace {

using namespace gygax;
using namespace gygax::intel;

constexpr double kSiteLat = 54.3028;
constexpr double kSiteLon = 6.8061;

struct Options {
    double seconds = 4.0;
    bool evacTest = false;
    bool keys = false;
    std::string feedPath;
    std::string verifyImage;
    std::string reportPath = "recon_survey_report.json";
};

struct RawTerminal {
    termios saved{};
    bool active = false;
    RawTerminal() {
        if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved) != 0) return;
        termios raw = saved;
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        active = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
    }
    ~RawTerminal() {
        if (active) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    }
    int read() const {
        char c = 0;
        return active && ::read(STDIN_FILENO, &c, 1) == 1 ? c : -1;
    }
};

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--seconds" && more) o.seconds = std::strtod(argv[++i], nullptr);
        else if (a == "--evac-test") o.evacTest = true;
        else if (a == "--keys") o.keys = true;
        else if (a == "--feed" && more) o.feedPath = argv[++i];
        else if (a == "--verify-image" && more) o.verifyImage = argv[++i];
        else if (a == "--report" && more) o.reportPath = argv[++i];
        else {
            std::cout << "Usage: recon_survey [--seconds N] [--evac-test] [--feed events.json] "
                         "[--verify-image photo] [--report out.json] [--keys]\n";
            return false;
        }
    }
    return true;
}

Event sensorEvent(const char* title, const char* source, double lat, double lon, int severity, double confidence,
                  std::int64_t age) {
    Event e;
    e.title = title;
    e.source = source;
    e.domain = Domain::Infrastructure;
    e.level = Level::Local;
    e.latitude = lat;
    e.longitude = lon;
    e.severity = severity;
    e.confidence = confidence;
    e.time = static_cast<std::int64_t>(std::time(nullptr)) - age;
    return e;
}

} // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!parseArgs(argc, argv, opts)) return 2;

    OsintEngine engine;
    engine.addSite({"Europipe Subsea Power Terminal", kSiteLat, kSiteLon, 200.0});

    if (!opts.feedPath.empty()) {
        std::string err;
        if (engine.loadFile(opts.feedPath, &err) == 0 && !err.empty()) {
            std::cerr << "feed: " << err << "\n";
            return 1;
        }
    }

    // Standalone geolocation check of a supplied photograph.
    if (!opts.verifyImage.empty()) {
        const auto match = engine.verifyLocation(opts.verifyImage);
        std::cout << OsintEngine::toJson(match).dump() << "\n";
        return match.verified ? 0 : 3;
    }

    // Survey photograph taken at the terminal; built through the EXIF writer and read back.
    ImageMetadata shot;
    shot.description = "Europipe Subsea Power Terminal, bay 3";
    shot.make = "Gygax";
    shot.model = "VALKYRIE-03 survey cam";
    shot.latitude = kSiteLat + 0.00002;
    shot.longitude = kSiteLon - 0.00001;
    ImageMetadata decoded;
    if (!parseImage(buildJpeg(shot), decoded)) {
        std::cerr << "survey image failed to decode\n";
        return 1;
    }
    const auto geo = engine.verifyLocation(decoded);

    // Field sensors report in at Local level.
    if (opts.evacTest) {
        engine.addEvent(sensorEvent("Pressure loss on feeder line 2", "scada-gateway", kSiteLat, kSiteLon, 5, 0.8, 120));
        engine.addEvent(sensorEvent("Perimeter breach, east fence", "ugv-titan", kSiteLat + 0.001, kSiteLon + 0.001, 4, 0.7, 60));
    } else {
        engine.addEvent(sensorEvent("Routine valve cycling", "scada-gateway", kSiteLat, kSiteLon, 1, 0.9, 900));
    }

    RawTerminal term;
    engine.setOverlay(true);
    Report claim;
    claim.latitude = kSiteLat;
    claim.longitude = kSiteLon;
    claim.radiusKm = 2.0;
    claim.claimsSecure = true;
    claim.source = "site operator";

    bool evacuate = false;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(opts.seconds * 1000));
    while (std::chrono::steady_clock::now() < end) {
        const int key = opts.keys ? term.read() : -1;
        if (key == 'q' || key == 'Q') break;
        if (key == 'o' || key == 'O') engine.toggleOverlay();
        if (key == 'l' || key == 'L') engine.cycleLevel();
        if (key == 'e' || key == 'E') evacuate = true;

        const auto rec = engine.reconcile(claim);
        evacuate = evacuate || (rec.discrepancy && rec.assessment.threat >= Threat::Elevated);
        std::cout << "\n== GYGAX recon survey ==  site reported SECURE  photo: " << (geo.verified ? "VERIFIED" : "UNVERIFIED")
                  << " (" << geo.deltaMeters << " m from " << geo.locationName << ")\n";
        if (engine.isOverlayActive()) {
            std::cout << "[overlay: " << levelName(engine.overlayLevel()) << "]\n";
            for (const auto& line : engine.overlayLines()) std::cout << "  " << line << "\n";
        }
        if (rec.discrepancy) std::cout << "!! DISCREPANCY: " << rec.finding << "\n";
        if (evacuate) {
            std::cout << "!! EVACUATE: egress to the north gate; activity contradicts the secure report\n";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(opts.keys ? 100 : 500));
    }

    const auto finalRec = engine.reconcile(claim);
    json::Value report = json::Value::object();
    report["site"] = "Europipe Subsea Power Terminal";
    report["claimed_secure"] = claim.claimsSecure;
    report["discrepancy"] = finalRec.discrepancy;
    report["finding"] = finalRec.finding;
    report["evacuate"] = evacuate;
    report["assessment"] = OsintEngine::toJson(finalRec.assessment);
    report["geolocation"] = OsintEngine::toJson(geo);
    std::ofstream out(opts.reportPath);
    out << report.dump() << "\n";
    std::cout << "\nreport written to " << opts.reportPath << "\n";
    return evacuate ? 10 : 0;
}
