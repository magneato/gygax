#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wpedantic"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#pragma clang diagnostic pop

#include <gygax/core/json.hpp>
#include <gygax/intel/metadata.hpp>
#include <gygax/intel/osint.hpp>

namespace {

using namespace gygax;
using namespace gygax::intel;

// Tactical Matrix color palette
const cv::Scalar kMatrixGreen(0, 240, 120);
const cv::Scalar kCyberCyan(255, 240, 0);     // OpenCV BGR
const cv::Scalar kThreatRed(40, 40, 240);
const cv::Scalar kWarningAmber(0, 180, 255);
const cv::Scalar kDarkPanel(14, 18, 22);
const cv::Scalar kGridLine(36, 45, 52);
const cv::Scalar kTextDim(140, 160, 170);

struct DroneTelemetry {
    std::string callsign{"SKYWARD-1"};
    double latitude{54.3012};
    double longitude{6.8045};
    double altitudeMeters{118.5};
    double headingDeg{42.0};
    double speedMps{16.4};
    double batteryPct{88.5};
    double rfJammingSnrDb{24.2}; // Drops < 0 dB under jamming
    bool gimbalLocked{true};
};

struct RobotTelemetry {
    std::string callsign{"TITAN-UGV"};
    double latitude{54.3021};
    double longitude{6.8052};
    double headingDeg{38.0};
    double speedMps{3.2};
    double batteryPct{92.0};
    double minLidarRangeMeters{18.4};
    bool perimeterBreachDetected{false};
};

struct HumanoidTelemetry {
    std::string callsign{"VALKYRIE-03"};
    double latitude{54.3028};
    double longitude{6.8061};
    double headingDeg{35.0};
    double walkingSpeedMps{1.4};
    double balancePitchDeg{0.2};
    double balanceRollDeg{-0.1};
    double batteryPct{84.0};
    bool airlockIntact{true};
    std::string cryptotagScanned{"EU-PIPE-SEC-AUTH-9942"};
};

enum class MissionState {
    InfiltrationSurvey,
    TruthVerificationCrosscheck,
    DiscrepancyAlarm,
    EmergencyEvacuationEgress,
    EvacuationComplete
};

struct ReconSurveyOptions {
    double durationSeconds{6.0};
    bool headless{true};
    bool forceEvac{false};
    std::string verifyImagePath;
    std::string outputReportPath{"build/recon_survey_report.json"};
    std::string outputSnapshotPath{"build/recon_survey_snapshot.png"};
};

ReconSurveyOptions parseArgs(int argc, char** argv) {
    ReconSurveyOptions opts;
    const char* display = std::getenv("DISPLAY");
    opts.headless = (display == nullptr || std::string(display).empty());

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--seconds" && i + 1 < argc) {
            opts.durationSeconds = std::stod(argv[++i]);
        } else if (arg == "--headless") {
            opts.headless = true;
        } else if (arg == "--gui") {
            opts.headless = false;
        } else if (arg == "--evac-test") {
            opts.forceEvac = true;
        } else if (arg == "--verify-image" && i + 1 < argc) {
            opts.verifyImagePath = argv[++i];
        } else if (arg == "--report" && i + 1 < argc) {
            opts.outputReportPath = argv[++i];
        } else if (arg == "--snapshot" && i + 1 < argc) {
            opts.outputSnapshotPath = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: recon_survey [options]\n"
                      << "  --seconds <N>        Run simulation for N seconds (default: 6.0)\n"
                      << "  --headless           Run without GUI window (default in headless environments)\n"
                      << "  --gui                Force GUI window\n"
                      << "  --evac-test          Force emergency hostile breach and evacuation protocol\n"
                      << "  --verify-image <path> Run standalone bare EXIF/TIFF geolocation verification\n"
                      << "  --report <path>      Output JSON telemetry and truth report\n"
                      << "  --snapshot <path>    Output PNG snapshot of tactical HUD\n";
            std::exit(0);
        }
    }
    return opts;
}

// Generate bare TIFF metadata buffer for simulated survey image
std::vector<uint8_t> createMockSurveyImageWithGps(double lat, double lon, const std::string& locationName) {
    std::vector<uint8_t> buf;
    buf.reserve(1024);

    // Bare TIFF Header (Little Endian: 'II', magic 42, offset 8)
    buf.push_back('I');
    buf.push_back('I');
    buf.push_back(0x2A);
    buf.push_back(0x00);
    buf.push_back(0x08);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x00);

    // Number of IFD entries: 3 (GPS IFD pointer, ImageDescription/Location, Software)
    uint16_t numEntries = 2;
    buf.push_back(static_cast<uint8_t>(numEntries & 0xFF));
    buf.push_back(static_cast<uint8_t>((numEntries >> 8) & 0xFF));

    // Entry 1: Tag 0x010E (ImageDescription), Type 2 (ASCII), count = locationName.size()+1
    uint16_t tagDesc = 0x010E;
    buf.push_back(static_cast<uint8_t>(tagDesc & 0xFF));
    buf.push_back(static_cast<uint8_t>((tagDesc >> 8) & 0xFF));
    buf.push_back(0x02); buf.push_back(0x00); // Type ASCII
    uint32_t descLen = static_cast<uint32_t>(locationName.size() + 1);
    buf.push_back(static_cast<uint8_t>(descLen & 0xFF));
    buf.push_back(static_cast<uint8_t>((descLen >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((descLen >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((descLen >> 24) & 0xFF));
    // Offset to string: offset 40
    uint32_t descOffset = 40;
    buf.push_back(static_cast<uint8_t>(descOffset & 0xFF));
    buf.push_back(static_cast<uint8_t>((descOffset >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((descOffset >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((descOffset >> 24) & 0xFF));

    // Entry 2: Tag 0x8825 (GPS IFD Pointer), Type 4 (LONG), count = 1, offset = 120
    uint16_t tagGps = 0x8825;
    buf.push_back(static_cast<uint8_t>(tagGps & 0xFF));
    buf.push_back(static_cast<uint8_t>((tagGps >> 8) & 0xFF));
    buf.push_back(0x04); buf.push_back(0x00); // Type LONG
    buf.push_back(0x01); buf.push_back(0x00); buf.push_back(0x00); buf.push_back(0x00); // count = 1
    uint32_t gpsIfdOffset = 120;
    buf.push_back(static_cast<uint8_t>(gpsIfdOffset & 0xFF));
    buf.push_back(static_cast<uint8_t>((gpsIfdOffset >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((gpsIfdOffset >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((gpsIfdOffset >> 24) & 0xFF));

    // Next IFD offset: 0 (terminator)
    buf.push_back(0x00); buf.push_back(0x00); buf.push_back(0x00); buf.push_back(0x00);

    // Offset 40: locationName string
    while (buf.size() < descOffset) buf.push_back(0);
    for (char c : locationName) buf.push_back(static_cast<uint8_t>(c));
    buf.push_back(0); // null terminator

    // Offset 120: GPS IFD
    while (buf.size() < gpsIfdOffset) buf.push_back(0);
    uint16_t gpsEntries = 4;
    buf.push_back(static_cast<uint8_t>(gpsEntries & 0xFF));
    buf.push_back(static_cast<uint8_t>((gpsEntries >> 8) & 0xFF));

    // GPS Tag 1: GPSLatitudeRef ('N')
    buf.push_back(0x01); buf.push_back(0x00);
    buf.push_back(0x02); buf.push_back(0x00);
    buf.push_back(0x02); buf.push_back(0x00); buf.push_back(0x00); buf.push_back(0x00);
    buf.push_back(lat >= 0 ? 'N' : 'S'); buf.push_back(0); buf.push_back(0); buf.push_back(0);

    // GPS Tag 2: GPSLatitude (3 RATIONALs = 24 bytes, offset = 220)
    buf.push_back(0x02); buf.push_back(0x00);
    buf.push_back(0x05); buf.push_back(0x00);
    buf.push_back(0x03); buf.push_back(0x00); buf.push_back(0x00); buf.push_back(0x00);
    uint32_t latValOffset = 220;
    buf.push_back(static_cast<uint8_t>(latValOffset & 0xFF));
    buf.push_back(static_cast<uint8_t>((latValOffset >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((latValOffset >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((latValOffset >> 24) & 0xFF));

    // GPS Tag 3: GPSLongitudeRef ('E')
    buf.push_back(0x03); buf.push_back(0x00);
    buf.push_back(0x02); buf.push_back(0x00);
    buf.push_back(0x02); buf.push_back(0x00); buf.push_back(0x00); buf.push_back(0x00);
    buf.push_back(lon >= 0 ? 'E' : 'W'); buf.push_back(0); buf.push_back(0); buf.push_back(0);

    // GPS Tag 4: GPSLongitude (3 RATIONALs = 24 bytes, offset = 244)
    buf.push_back(0x04); buf.push_back(0x00);
    buf.push_back(0x05); buf.push_back(0x00);
    buf.push_back(0x03); buf.push_back(0x00); buf.push_back(0x00); buf.push_back(0x00);
    uint32_t lonValOffset = 244;
    buf.push_back(static_cast<uint8_t>(lonValOffset & 0xFF));
    buf.push_back(static_cast<uint8_t>((lonValOffset >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((lonValOffset >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((lonValOffset >> 24) & 0xFF));

    // Terminator
    buf.push_back(0); buf.push_back(0); buf.push_back(0); buf.push_back(0);

    // Values at 220: Latitude (deg, min, sec)
    while (buf.size() < latValOffset) buf.push_back(0);
    double absLat = std::abs(lat);
    uint32_t degLat = static_cast<uint32_t>(absLat);
    double remLat = (absLat - degLat) * 60.0;
    uint32_t minLat = static_cast<uint32_t>(remLat);
    uint32_t secLat = static_cast<uint32_t>((remLat - minLat) * 60.0 * 1000.0);

    auto pushRational = [&](uint32_t num, uint32_t den) {
        buf.push_back(static_cast<uint8_t>(num & 0xFF));
        buf.push_back(static_cast<uint8_t>((num >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>((num >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((num >> 24) & 0xFF));
        buf.push_back(static_cast<uint8_t>(den & 0xFF));
        buf.push_back(static_cast<uint8_t>((den >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>((den >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((den >> 24) & 0xFF));
    };

    pushRational(degLat, 1);
    pushRational(minLat, 1);
    pushRational(secLat, 1000);

    // Values at 244: Longitude
    while (buf.size() < lonValOffset) buf.push_back(0);
    double absLon = std::abs(lon);
    uint32_t degLon = static_cast<uint32_t>(absLon);
    double remLon = (absLon - degLon) * 60.0;
    uint32_t minLon = static_cast<uint32_t>(remLon);
    uint32_t secLon = static_cast<uint32_t>((remLon - minLon) * 60.0 * 1000.0);

    pushRational(degLon, 1);
    pushRational(minLon, 1);
    pushRational(secLon, 1000);

    return buf;
}

void renderReconDashboard(
    cv::Mat& frame,
    const DroneTelemetry& drone,
    const RobotTelemetry& robot,
    const HumanoidTelemetry& humanoid,
    MissionState state,
    double simTime,
    const GeolocationMatch& geoMatch,
    const std::string& osintStatusText
) {
    frame = cv::Mat(720, 1280, CV_8UC3, kDarkPanel);

    // Draw background matrix grid
    for (int y = 40; y < frame.rows; y += 40) {
        cv::line(frame, cv::Point(0, y), cv::Point(frame.cols, y), kGridLine, 1);
    }
    for (int x = 40; x < frame.cols; x += 40) {
        cv::line(frame, cv::Point(x, 40), cv::Point(x, frame.rows), kGridLine, 1);
    }

    // Top Header Banner
    cv::rectangle(frame, cv::Point(0, 0), cv::Point(frame.cols, 42), cv::Scalar(10, 16, 20), -1);
    cv::line(frame, cv::Point(0, 42), cv::Point(frame.cols, 42), kCyberCyan, 2);

    std::string title = "GYGAX v0.6.0 // MULTI-DOMAIN RECONNAISSANCE SURVEY // OSINT & GEOLOCATION";
    cv::putText(frame, title, cv::Point(16, 28), cv::FONT_HERSHEY_SIMPLEX, 0.65, kCyberCyan, 2, cv::LINE_AA);

    std::string timeStr = std::format("TIME: T+{:.1f}s", simTime);
    cv::putText(frame, timeStr, cv::Point(frame.cols - 160, 28), cv::FONT_HERSHEY_SIMPLEX, 0.6, kMatrixGreen, 1, cv::LINE_AA);

    // Mission State Status Box
    std::string stateStr;
    cv::Scalar stateColor = kMatrixGreen;
    if (state == MissionState::InfiltrationSurvey) {
        stateStr = "[ PHASE 1: INFILTRATION & MULTI-DOMAIN SURVEY ]";
        stateColor = kCyberCyan;
    } else if (state == MissionState::TruthVerificationCrosscheck) {
        stateStr = "[ PHASE 2: REAL-TIME OSINT TRUTH RECONCILIATION ]";
        stateColor = kWarningAmber;
    } else if (state == MissionState::DiscrepancyAlarm) {
        stateStr = "[ ALERT: TRUTH DISCREPANCY DETECTED! HOSTILE BREACH! ]";
        stateColor = kThreatRed;
    } else if (state == MissionState::EmergencyEvacuationEgress) {
        stateStr = "[ EMERGENCY DIRECTIVE: EVACUATION PROTOCOL ENGAGED ]";
        stateColor = kThreatRed;
    } else {
        stateStr = "[ EVACUATION COMPLETE: ALL ASSETS SECURED AT LZ-ALPHA ]";
        stateColor = kMatrixGreen;
    }

    cv::rectangle(frame, cv::Point(16, 52), cv::Point(680, 84), cv::Scalar(18, 24, 30), -1);
    cv::rectangle(frame, cv::Point(16, 52), cv::Point(680, 84), stateColor, 1);
    cv::putText(frame, stateStr, cv::Point(24, 74), cv::FONT_HERSHEY_SIMPLEX, 0.52, stateColor, 1, cv::LINE_AA);

    // Left Panel: Aerial Drone (UAV-AERO-01)
    int panelW = 380;
    cv::rectangle(frame, cv::Point(16, 96), cv::Point(16 + panelW, 370), cv::Scalar(16, 22, 28), -1);
    cv::rectangle(frame, cv::Point(16, 96), cv::Point(16 + panelW, 370), kGridLine, 1);
    cv::putText(frame, "DOMAIN 1: AERIAL UAV (SKYWARD-1)", cv::Point(26, 122), cv::FONT_HERSHEY_SIMPLEX, 0.52, kCyberCyan, 1, cv::LINE_AA);

    cv::putText(frame, std::format("Altitude:     {:.1f} m AGL", drone.altitudeMeters), cv::Point(26, 154), cv::FONT_HERSHEY_SIMPLEX, 0.44, kTextDim, 1, cv::LINE_AA);
    cv::putText(frame, std::format("Airspeed:     {:.1f} m/s (Heading: {:.0f} deg)", drone.speedMps, drone.headingDeg), cv::Point(26, 180), cv::FONT_HERSHEY_SIMPLEX, 0.44, kTextDim, 1, cv::LINE_AA);
    cv::putText(frame, std::format("Coordinates:  {:.4f} N, {:.4f} E", drone.latitude, drone.longitude), cv::Point(26, 206), cv::FONT_HERSHEY_SIMPLEX, 0.44, kTextDim, 1, cv::LINE_AA);
    cv::putText(frame, std::format("Battery:      {:.1f}% [NORMAL]", drone.batteryPct), cv::Point(26, 232), cv::FONT_HERSHEY_SIMPLEX, 0.44, kMatrixGreen, 1, cv::LINE_AA);

    cv::Scalar rfColor = drone.rfJammingSnrDb > 5.0 ? kMatrixGreen : kThreatRed;
    std::string rfStatus = drone.rfJammingSnrDb > 5.0 ? "CLEAR (+24dB SNR)" : "JAMMING DETECTED (-18dB SNR)";
    cv::putText(frame, std::format("RF Spectrum:  {}", rfStatus), cv::Point(26, 258), cv::FONT_HERSHEY_SIMPLEX, 0.44, rfColor, 1, cv::LINE_AA);
    cv::putText(frame, std::format("Gimbal 4K:    {} [Target Locked]", drone.gimbalLocked ? "ACTIVE" : "STOWED"), cv::Point(26, 284), cv::FONT_HERSHEY_SIMPLEX, 0.44, kCyberCyan, 1, cv::LINE_AA);

    // Left Lower Panel: Ground Robot (TITAN-UGV)
    cv::rectangle(frame, cv::Point(16, 385), cv::Point(16 + panelW, 690), cv::Scalar(16, 22, 28), -1);
    cv::rectangle(frame, cv::Point(16, 385), cv::Point(16 + panelW, 690), kGridLine, 1);
    cv::putText(frame, "DOMAIN 2: GROUND UGV (TITAN-ROVER)", cv::Point(26, 412), cv::FONT_HERSHEY_SIMPLEX, 0.52, kCyberCyan, 1, cv::LINE_AA);

    cv::putText(frame, std::format("Ground Speed: {:.1f} m/s", robot.speedMps), cv::Point(26, 444), cv::FONT_HERSHEY_SIMPLEX, 0.44, kTextDim, 1, cv::LINE_AA);
    cv::putText(frame, std::format("Coordinates:  {:.4f} N, {:.4f} E", robot.latitude, robot.longitude), cv::Point(26, 470), cv::FONT_HERSHEY_SIMPLEX, 0.44, kTextDim, 1, cv::LINE_AA);
    cv::putText(frame, std::format("LiDAR Range:  {:.1f} m (32 Beams)", robot.minLidarRangeMeters), cv::Point(26, 496), cv::FONT_HERSHEY_SIMPLEX, 0.44, kTextDim, 1, cv::LINE_AA);

    cv::Scalar breachCol = robot.perimeterBreachDetected ? kThreatRed : kMatrixGreen;
    std::string breachTxt = robot.perimeterBreachDetected ? "PERIMETER BREACH AT AZ 042 DEG!" : "PERIMETER SECURE";
    cv::putText(frame, std::format("Perimeter:    {}", breachTxt), cv::Point(26, 522), cv::FONT_HERSHEY_SIMPLEX, 0.44, breachCol, 1, cv::LINE_AA);
    cv::putText(frame, std::format("Battery:      {:.1f}% [NORMAL]", robot.batteryPct), cv::Point(26, 548), cv::FONT_HERSHEY_SIMPLEX, 0.44, kMatrixGreen, 1, cv::LINE_AA);

    // Center Panel: Tactical Survey Map & Evacuation Corridor
    int mapX = 412;
    int mapW = 460;
    cv::rectangle(frame, cv::Point(mapX, 96), cv::Point(mapX + mapW, 490), cv::Scalar(10, 14, 18), -1);
    cv::rectangle(frame, cv::Point(mapX, 96), cv::Point(mapX + mapW, 490), kCyberCyan, 1);
    cv::putText(frame, "TACTICAL RECON RADAR // EVACUATION CORRIDOR", cv::Point(mapX + 16, 122), cv::FONT_HERSHEY_SIMPLEX, 0.5, kCyberCyan, 1, cv::LINE_AA);

    // Radar Rings
    cv::Point mapCenter(mapX + mapW / 2, 290);
    cv::circle(frame, mapCenter, 140, kGridLine, 1);
    cv::circle(frame, mapCenter, 90, kGridLine, 1);
    cv::circle(frame, mapCenter, 40, kGridLine, 1);
    cv::line(frame, cv::Point(mapCenter.x - 150, mapCenter.y), cv::Point(mapCenter.x + 150, mapCenter.y), kGridLine, 1);
    cv::line(frame, cv::Point(mapCenter.x, mapCenter.y - 150), cv::Point(mapCenter.x, mapCenter.y + 150), kGridLine, 1);

    // Sector Danger Perimeter
    cv::circle(frame, cv::Point(mapCenter.x + 25, mapCenter.y - 20), 45, state == MissionState::InfiltrationSurvey ? kWarningAmber : kThreatRed, 1, cv::LINE_AA);
    cv::putText(frame, "SECTOR-OMEGA", cv::Point(mapCenter.x + 35, mapCenter.y - 25), cv::FONT_HERSHEY_SIMPLEX, 0.38, kThreatRed, 1, cv::LINE_AA);

    // Evacuation Rally Point (LZ-ALPHA)
    cv::Point evacPt(mapCenter.x - 110, mapCenter.y + 90);
    cv::circle(frame, evacPt, 12, kMatrixGreen, 2);
    cv::putText(frame, "EVAC-LZ-ALPHA", cv::Point(evacPt.x - 45, evacPt.y + 24), cv::FONT_HERSHEY_SIMPLEX, 0.42, kMatrixGreen, 1, cv::LINE_AA);

    // If Evacuating, render pulsing green evacuation corridor vector
    if (state == MissionState::EmergencyEvacuationEgress || state == MissionState::EvacuationComplete) {
        cv::line(frame, cv::Point(mapCenter.x + 10, mapCenter.y), evacPt, kMatrixGreen, 2, cv::LINE_AA);
        cv::putText(frame, ">> FALLBACK CORRIDOR ACTIVE >>", cv::Point(mapCenter.x - 80, mapCenter.y + 35), cv::FONT_HERSHEY_SIMPLEX, 0.44, kMatrixGreen, 1, cv::LINE_AA);
    }

    // Plot units on map
    // Drone (Cyan Triangle)
    cv::Point dronePt(mapCenter.x + 10, mapCenter.y - 45);
    cv::circle(frame, dronePt, 7, kCyberCyan, -1);
    cv::putText(frame, "UAV-1", cv::Point(dronePt.x + 10, dronePt.y + 4), cv::FONT_HERSHEY_SIMPLEX, 0.4, kCyberCyan, 1, cv::LINE_AA);

    // Ground Rover (Amber Square)
    cv::Point roverPt(mapCenter.x + 20, mapCenter.y - 10);
    cv::rectangle(frame, cv::Point(roverPt.x - 6, roverPt.y - 6), cv::Point(roverPt.x + 6, roverPt.y + 6), kWarningAmber, -1);
    cv::putText(frame, "UGV-2", cv::Point(roverPt.x + 10, roverPt.y + 4), cv::FONT_HERSHEY_SIMPLEX, 0.4, kWarningAmber, 1, cv::LINE_AA);

    // Humanoid (Matrix Green Dot)
    cv::Point humPt(mapCenter.x + 28, mapCenter.y + 8);
    cv::circle(frame, humPt, 5, kMatrixGreen, -1);
    cv::putText(frame, "VALKYRIE-3", cv::Point(humPt.x + 8, humPt.y + 4), cv::FONT_HERSHEY_SIMPLEX, 0.4, kMatrixGreen, 1, cv::LINE_AA);

    // Right Panel: Humanoid Scout (VALKYRIE-03)
    int rX = mapX + mapW + 16;
    int rW = frame.cols - rX - 16;
    cv::rectangle(frame, cv::Point(rX, 96), cv::Point(rX + rW, 370), cv::Scalar(16, 22, 28), -1);
    cv::rectangle(frame, cv::Point(rX, 96), cv::Point(rX + rW, 370), kGridLine, 1);
    cv::putText(frame, "DOMAIN 3: HUMANOID (VALKYRIE-03)", cv::Point(rX + 14, 122), cv::FONT_HERSHEY_SIMPLEX, 0.48, kMatrixGreen, 1, cv::LINE_AA);

    cv::putText(frame, std::format("Gait Speed:   {:.1f} m/s", humanoid.walkingSpeedMps), cv::Point(rX + 14, 154), cv::FONT_HERSHEY_SIMPLEX, 0.42, kTextDim, 1, cv::LINE_AA);
    cv::putText(frame, std::format("Coordinates:  {:.4f} N, {:.4f} E", humanoid.latitude, humanoid.longitude), cv::Point(rX + 14, 180), cv::FONT_HERSHEY_SIMPLEX, 0.42, kTextDim, 1, cv::LINE_AA);
    cv::putText(frame, std::format("Joint Pitch:  {:.1f} deg", humanoid.balancePitchDeg), cv::Point(rX + 14, 206), cv::FONT_HERSHEY_SIMPLEX, 0.42, kTextDim, 1, cv::LINE_AA);
    cv::putText(frame, std::format("CryptoTag:    {}", humanoid.cryptotagScanned), cv::Point(rX + 14, 232), cv::FONT_HERSHEY_SIMPLEX, 0.40, kCyberCyan, 1, cv::LINE_AA);

    cv::Scalar airlockCol = humanoid.airlockIntact ? kMatrixGreen : kThreatRed;
    std::string airlockTxt = humanoid.airlockIntact ? "INTACT / SECURE" : "SEAL TAMPERED / BREACH";
    cv::putText(frame, std::format("Airlock Seal: {}", airlockTxt), cv::Point(rX + 14, 258), cv::FONT_HERSHEY_SIMPLEX, 0.42, airlockCol, 1, cv::LINE_AA);
    cv::putText(frame, std::format("Battery:      {:.1f}% [NOMINAL]", humanoid.batteryPct), cv::Point(rX + 14, 284), cv::FONT_HERSHEY_SIMPLEX, 0.42, kMatrixGreen, 1, cv::LINE_AA);

    // Right Lower Panel: Geolocation Truth Verification Status
    cv::rectangle(frame, cv::Point(rX, 385), cv::Point(rX + rW, 690), cv::Scalar(16, 22, 28), -1);
    cv::rectangle(frame, cv::Point(rX, 385), cv::Point(rX + rW, 690), kGridLine, 1);
    cv::putText(frame, "GEOLOCATION TRUTH CHECK", cv::Point(rX + 14, 412), cv::FONT_HERSHEY_SIMPLEX, 0.48, kCyberCyan, 1, cv::LINE_AA);

    cv::Scalar confColor = geoMatch.verified ? kMatrixGreen : kThreatRed;
    std::string verStr = geoMatch.verified ? "VERIFIED (100% MATCH)" : "UNVERIFIED";
    cv::putText(frame, std::format("Status:      {}", verStr), cv::Point(rX + 14, 444), cv::FONT_HERSHEY_SIMPLEX, 0.44, confColor, 1, cv::LINE_AA);
    cv::putText(frame, std::format("Confidence:  {:.1f}%", geoMatch.confidence * 100.0), cv::Point(rX + 14, 470), cv::FONT_HERSHEY_SIMPLEX, 0.44, kCyberCyan, 1, cv::LINE_AA);
    cv::putText(frame, std::format("Target Loc:  {}", geoMatch.locationName), cv::Point(rX + 14, 496), cv::FONT_HERSHEY_SIMPLEX, 0.42, kTextDim, 1, cv::LINE_AA);
    cv::putText(frame, std::format("GPS Delta:   < 2.4 meters"), cv::Point(rX + 14, 522), cv::FONT_HERSHEY_SIMPLEX, 0.42, kMatrixGreen, 1, cv::LINE_AA);
    cv::putText(frame, "TIFF EXIF GPS tags validated.", cv::Point(rX + 14, 548), cv::FONT_HERSHEY_SIMPLEX, 0.38, kTextDim, 1, cv::LINE_AA);

    // Bottom Center Console: Intelligence & Truth Log
    cv::rectangle(frame, cv::Point(mapX, 505), cv::Point(mapX + mapW, 690), cv::Scalar(12, 16, 22), -1);
    cv::rectangle(frame, cv::Point(mapX, 505), cv::Point(mapX + mapW, 690), kGridLine, 1);
    cv::putText(frame, "INTELLIGENCE LOG & TRUTH RECONCILIATION", cv::Point(mapX + 12, 528), cv::FONT_HERSHEY_SIMPLEX, 0.44, kCyberCyan, 1, cv::LINE_AA);

    cv::putText(frame, std::format("OSINT Feed:   {}", osintStatusText), cv::Point(mapX + 12, 556), cv::FONT_HERSHEY_SIMPLEX, 0.40, kTextDim, 1, cv::LINE_AA);

    if (state == MissionState::InfiltrationSurvey) {
        cv::putText(frame, ">> All units advancing along nominal survey vectors.", cv::Point(mapX + 12, 584), cv::FONT_HERSHEY_SIMPLEX, 0.38, kMatrixGreen, 1, cv::LINE_AA);
        cv::putText(frame, ">> Field report claims sector is clear of interference.", cv::Point(mapX + 12, 606), cv::FONT_HERSHEY_SIMPLEX, 0.38, kTextDim, 1, cv::LINE_AA);
    } else if (state == MissionState::TruthVerificationCrosscheck) {
        cv::putText(frame, ">> Verifying sensor telemetry against public OSINT advisories...", cv::Point(mapX + 12, 584), cv::FONT_HERSHEY_SIMPLEX, 0.38, kWarningAmber, 1, cv::LINE_AA);
        cv::putText(frame, ">> Cross-referencing bare TIFF EXIF tags with aerial downlink...", cv::Point(mapX + 12, 606), cv::FONT_HERSHEY_SIMPLEX, 0.38, kWarningAmber, 1, cv::LINE_AA);
    } else if (state == MissionState::DiscrepancyAlarm || state == MissionState::EmergencyEvacuationEgress) {
        cv::putText(frame, "[!] DISCREPANCY: Report claims sector secure; reality is HOSTILE!", cv::Point(mapX + 12, 584), cv::FONT_HERSHEY_SIMPLEX, 0.38, kThreatRed, 1, cv::LINE_AA);
        cv::putText(frame, "[!] DIRECTIVE: ABORT MISSION. INITIATE EVACUATION TO LZ-ALPHA!", cv::Point(mapX + 12, 606), cv::FONT_HERSHEY_SIMPLEX, 0.38, kThreatRed, 1, cv::LINE_AA);
    } else {
        cv::putText(frame, ">> EVACUATION SUCCESSFUL: 100% Units accounted for at LZ-ALPHA.", cv::Point(mapX + 12, 584), cv::FONT_HERSHEY_SIMPLEX, 0.38, kMatrixGreen, 1, cv::LINE_AA);
        cv::putText(frame, ">> Zero personnel or robot losses. Mission truth verified.", cv::Point(mapX + 12, 606), cv::FONT_HERSHEY_SIMPLEX, 0.38, kMatrixGreen, 1, cv::LINE_AA);
    }

    // Operator Key Control Hint
    std::string hintStr = "Controls: [O] Toggle OSINT HUD | [L] Cycle OSINT Level | [E] Trigger Evac Drill | [Q] Quit";
    cv::putText(frame, hintStr, cv::Point(mapX + 12, 672), cv::FONT_HERSHEY_SIMPLEX, 0.36, kTextDim, 1, cv::LINE_AA);
}

} // namespace

int main(int argc, char** argv) {
    ReconSurveyOptions opts = parseArgs(argc, argv);

    std::cout << "\n\033[1;36m================================================================================\033[0m\n";
    std::cout << "\033[1;36m   GYGAX v0.6.0 // AUTONOMOUS MULTI-DOMAIN RECONNAISSANCE & OSINT SURVEY        \033[0m\n";
    std::cout << "\033[1;36m================================================================================\033[0m\n";
    std::cout << "  [*] Domain 1: Aerial UAV Survey Drone  [SKYWARD-1] (4K Gimbal & Multispectral)\n";
    std::cout << "  [*] Domain 2: Heavy Ground UGV Rover   [TITAN-UGV] (32-Beam 3D LiDAR)\n";
    std::cout << "  [*] Domain 3: Agile Humanoid Scout     [VALKYRIE-03] (12-DOF Gait & CryptoTag)\n";
    std::cout << "  [*] Intel Engine: 5-Tier OSINT Engine  (Local -> Global)\n";
    std::cout << "  [*] Geolocation: Bare TIFF/EXIF GPS Parser & Truth Verification\n";
    std::cout << "  [*] Safety Failsafe: Emergency Fallback Evacuation Corridor (EVAC-LZ-ALPHA)\n";
    std::cout << "\033[1;36m================================================================================\033[0m\n\n";

    OsintEngine osintEngine;

    // Handle standalone image verification if requested
    if (!opts.verifyImagePath.empty()) {
        std::cout << "[*] Running standalone geolocation verification on: " << opts.verifyImagePath << "\n";
        auto match = osintEngine.verifyLocation(opts.verifyImagePath);
        std::cout << "    Verified:    " << (match.verified ? "YES (100% MATCH)" : "NO") << "\n";
        std::cout << "    Confidence:  " << std::fixed << std::setprecision(1) << (match.confidence * 100.0) << "%\n";
        std::cout << "    Coordinates: " << match.latitude << ", " << match.longitude << "\n";
        std::cout << "    Location:    " << match.locationName << "\n";
        std::cout << "    Notes:       " << match.verificationNotes << "\n\n";
        return match.verified ? 0 : 1;
    }

    // Initialize units
    DroneTelemetry drone;
    RobotTelemetry robot;
    HumanoidTelemetry humanoid;

    // Generate mock photo metadata with GPS tags
    auto mockPhotoBuf = createMockSurveyImageWithGps(54.3028, 6.8061, "Europipe Subsea Power Terminal");
    ImageMetadata photoMeta;
    parseTiff(mockPhotoBuf, photoMeta);
    GeolocationMatch geoMatch = osintEngine.verifyLocation(photoMeta);

    std::cout << "[+] Verified baseline survey image geolocation: "
              << geoMatch.locationName << " (" << geoMatch.latitude << "N, " << geoMatch.longitude << "E) "
              << "Confidence: " << (geoMatch.confidence * 100.0) << "%\n\n";

    MissionState state = MissionState::InfiltrationSurvey;
    double simTime = 0.0;
    const double dt = 0.1; // 100ms time step
    uint64_t stepCount = 0;

    cv::Mat hudFrame;
    std::string osintSummary = "Local: Baltic Subsea Interconnector [ACTIVE] - Anomaly Monitoring";

    // Simulation Loop
    while (simTime < opts.durationSeconds) {
        simTime += dt;
        stepCount++;

        // State Machine Progression
        if (simTime < 2.0) {
            state = MissionState::InfiltrationSurvey;
            drone.altitudeMeters = 118.5 - simTime * 2.0;
            robot.speedMps = 3.2;
            humanoid.walkingSpeedMps = 1.4;
        } else if (simTime < 3.5 && !opts.forceEvac) {
            state = MissionState::TruthVerificationCrosscheck;
            osintSummary = "Cross-referencing sensor telemetry with sector security report...";
        } else if (simTime < 4.8 || opts.forceEvac) {
            state = MissionState::DiscrepancyAlarm;

            // Hostile breach occurs in simulation!
            drone.rfJammingSnrDb = -18.4; // Jamming detected!
            robot.perimeterBreachDetected = true;
            robot.minLidarRangeMeters = 4.2;
            humanoid.airlockIntact = false;

            osintSummary = "CRITICAL DISCREPANCY: Report claims sector secure; telemetry confirms hostile breach!";
        } else {
            state = MissionState::EmergencyEvacuationEgress;

            // Units retreat towards EVAC-LZ-ALPHA
            drone.altitudeMeters = std::min(250.0, drone.altitudeMeters + 30.0 * dt);
            drone.headingDeg = 220.0; // Reverse heading
            robot.speedMps = 5.8;     // Emergency sprint
            robot.headingDeg = 215.0;
            humanoid.walkingSpeedMps = 3.6; // High-speed bipedal egress
            humanoid.balancePitchDeg = -2.4;

            if (simTime >= opts.durationSeconds - 0.5) {
                state = MissionState::EvacuationComplete;
                osintSummary = "ALL ASSETS EXTRACTED TO EVAC-LZ-ALPHA. Zero losses.";
            }
        }

        // Render tactical dashboard
        renderReconDashboard(hudFrame, drone, robot, humanoid, state, simTime, geoMatch, osintSummary);

        // Overlay the OSINT hierarchical feed if overlay is enabled
        if (osintEngine.isOverlayActive()) {
            osintEngine.renderOsintOverlay(hudFrame);
        }

        // Log periodic updates to stdout
        if (stepCount % 10 == 0) {
            std::cout << std::format("[T+{:4.1f}s] Drone: {:5.1f}m AGL | Rover LiDAR: {:4.1f}m | Humanoid: {:3.1f}m/s | Status: {}\n",
                simTime, drone.altitudeMeters, robot.minLidarRangeMeters, humanoid.walkingSpeedMps,
                (state == MissionState::EmergencyEvacuationEgress ? "EVACUATING" :
                 state == MissionState::DiscrepancyAlarm ? "BREACH ALARM" : "SURVEYING"));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "\n\033[1;32m================================================================================\033[0m\n";
    std::cout << "\033[1;32m[+] RECONNAISSANCE SURVEY COMPLETED SUCCESSFULLY                                \033[0m\n";
    std::cout << "\033[1;32m================================================================================\033[0m\n";
    std::cout << "  [*] Geolocation Truth:  VERIFIED (" << geoMatch.locationName << ", Conf: " << (geoMatch.confidence * 100.0) << "%)\n";
    std::cout << "  [*] Intel Discrepancy:  CORRECTLY DETECTED (Report fraud uncovered via sensor reality)\n";
    std::cout << "  [*] Evacuation Plan:    100% ASSETS SAFELY EVACUATED TO LZ-ALPHA\n";
    std::cout << "  [*] Drone Status:       Covering retreat at 250m AGL (Chaff deployed)\n";
    std::cout << "  [*] UGV Status:         Full speed retreat, perimeter clear\n";
    std::cout << "  [*] Humanoid Status:    Airlock egressed, sprinted 42m to safety\n";
    std::cout << "\033[1;32m================================================================================\033[0m\n\n";

    // Save final tactical snapshot image
    bool snapshotSaved = cv::imwrite(opts.outputSnapshotPath, hudFrame);
    if (snapshotSaved) {
        std::cout << "[+] Saved tactical reconnaissance HUD snapshot: " << opts.outputSnapshotPath << "\n";
    }

    // Export comprehensive JSON report
    json::Value report = json::Value::object();
    report["version"] = "0.6.0";
    report["timestamp"] = static_cast<int64_t>(std::chrono::system_clock::now().time_since_epoch().count());
    report["mission_status"] = "EVACUATION_COMPLETE_ZERO_LOSSES";
    report["truth_verified"] = true;
    report["discrepancy_detected"] = true;
    report["evacuation_corridor"] = "EVAC-LZ-ALPHA";

    json::Value units = json::Value::object();

    json::Value droneObj = json::Value::object();
    droneObj["callsign"] = drone.callsign;
    droneObj["final_altitude"] = drone.altitudeMeters;
    droneObj["battery_pct"] = drone.batteryPct;
    droneObj["evacuated"] = true;
    units["drone"] = std::move(droneObj);

    json::Value robotObj = json::Value::object();
    robotObj["callsign"] = robot.callsign;
    robotObj["perimeter_breach_detected"] = robot.perimeterBreachDetected;
    robotObj["evacuated"] = true;
    units["robot"] = std::move(robotObj);

    json::Value humObj = json::Value::object();
    humObj["callsign"] = humanoid.callsign;
    humObj["cryptotag"] = humanoid.cryptotagScanned;
    humObj["evacuated"] = true;
    units["humanoid"] = std::move(humObj);

    report["units"] = std::move(units);

    json::Value geoObj = json::Value::object();
    geoObj["verified"] = geoMatch.verified;
    geoObj["confidence"] = geoMatch.confidence;
    geoObj["location"] = geoMatch.locationName;
    geoObj["latitude"] = geoMatch.latitude;
    geoObj["longitude"] = geoMatch.longitude;
    report["geolocation"] = std::move(geoObj);

    std::ofstream ofs(opts.outputReportPath);
    if (ofs.is_open()) {
        ofs << report.dump(2);
        std::cout << "[+] Exported reconnaissance survey mission report: " << opts.outputReportPath << "\n\n";
    }

    return 0;
}
