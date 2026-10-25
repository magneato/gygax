#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gygax::intel {

// What an image says about itself: EXIF/TIFF tags (including the GPS IFD) and IPTC-IIM records.
struct ImageMetadata {
    std::string format; // "tiff", "jpeg" or "png"
    std::string description;
    std::string make;
    std::string model;
    std::string software;
    std::string dateTime;
    std::optional<double> latitude;  // decimal degrees, south negative
    std::optional<double> longitude; // decimal degrees, west negative
    std::optional<double> altitude;  // metres above sea level
    std::optional<double> heading;   // GPSImgDirection, degrees
    std::string iptcTitle;
    std::string iptcCaption;
    std::string iptcCity;
    std::string iptcCountry;
    std::vector<std::string> iptcKeywords;

    [[nodiscard]] bool hasGps() const { return latitude.has_value() && longitude.has_value(); }
};

// Parsers return false when the buffer is not that format or is truncated beyond use. Fields found before a
// problem are kept. Every read is bounds-checked, so hostile files cannot read past the buffer.
bool parseTiff(std::span<const std::uint8_t> data, ImageMetadata& out);
bool parseJpeg(std::span<const std::uint8_t> data, ImageMetadata& out);
bool parsePng(std::span<const std::uint8_t> data, ImageMetadata& out);

// Detects the format from the leading bytes.
bool parseImage(std::span<const std::uint8_t> data, ImageMetadata& out);
bool parseImageFile(const std::string& path, ImageMetadata& out, std::string* error = nullptr);

// Writers, used for fixtures and for stamping survey frames: a little-endian TIFF carrying description,
// software, make, model, date and GPS, and a JPEG wrapper with that block as its EXIF segment.
std::vector<std::uint8_t> buildTiff(const ImageMetadata& meta);
std::vector<std::uint8_t> buildJpeg(const ImageMetadata& meta);

} // namespace gygax::intel
