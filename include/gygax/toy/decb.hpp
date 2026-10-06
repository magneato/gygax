#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace gygax::toy::decb {

constexpr int kTracks = 35;
constexpr int kSectorsPerTrack = 18;
constexpr int kSectorSize = 256;
constexpr int kGranuleSectors = 9;
constexpr int kGranulesPerDisk = (kTracks - 1) * 2;
constexpr int kDiskBytes = kTracks * kSectorsPerTrack * kSectorSize;

inline std::vector<uint8_t> makeBinaryPayload(const std::vector<uint8_t>& code, uint16_t loadAddr, uint16_t execAddr) {
    std::vector<uint8_t> out;
    out.push_back(0x00);
    out.push_back(static_cast<uint8_t>(code.size() >> 8));
    out.push_back(static_cast<uint8_t>(code.size() & 0xFF));
    out.push_back(static_cast<uint8_t>(loadAddr >> 8));
    out.push_back(static_cast<uint8_t>(loadAddr & 0xFF));
    out.insert(out.end(), code.begin(), code.end());
    out.push_back(0xFF);
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(static_cast<uint8_t>(execAddr >> 8));
    out.push_back(static_cast<uint8_t>(execAddr & 0xFF));
    return out;
}

struct LoadedProgram {
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> segments;
    uint16_t execAddr = 0;
};

inline LoadedProgram parseBinaryPayload(const std::vector<uint8_t>& blob) {
    LoadedProgram prog;
    size_t i = 0;
    while (i + 5 <= blob.size()) {
        uint8_t marker = blob[i];
        if (marker == 0xFF) {
            prog.execAddr = static_cast<uint16_t>((blob[i + 3] << 8) | blob[i + 4]);
            break;
        }
        if (marker != 0x00) break;
        uint16_t len = static_cast<uint16_t>((blob[i + 1] << 8) | blob[i + 2]);
        uint16_t addr = static_cast<uint16_t>((blob[i + 3] << 8) | blob[i + 4]);
        i += 5;
        if (i + len > blob.size()) break;
        std::vector<uint8_t> data(blob.begin() + i, blob.begin() + i + len);
        prog.segments.emplace_back(addr, std::move(data));
        i += len;
    }
    return prog;
}

inline void writeDsk(const std::string& path, const std::string& filename, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> disk(kDiskBytes, 0xFF);

    std::array<uint8_t, kSectorSize> fat{};
    fat.fill(0xFF);
    auto lsnOffset = [](int track, int sector) { return (track * kSectorsPerTrack + sector) * kSectorSize; };

    size_t granulesNeeded = (payload.size() + (kGranuleSectors * kSectorSize) - 1) / (kGranuleSectors * kSectorSize);
    if (granulesNeeded == 0) granulesNeeded = 1;
    if (static_cast<int>(granulesNeeded) > kGranulesPerDisk) {
        throw std::runtime_error("program too large for a 35-track DECB disk");
    }

    std::vector<int> granuleTrack, granuleHalf;
    for (int t = 0; t < kTracks; ++t) {
        if (t == 17) continue;
        for (int h = 0; h < 2; ++h) {
            granuleTrack.push_back(t);
            granuleHalf.push_back(h);
        }
    }

    size_t bytesLeft = payload.size();
    size_t srcOff = 0;
    int lastSectorsUsed = kGranuleSectors;
    for (size_t g = 0; g < granulesNeeded; ++g) {
        int track = granuleTrack[g];
        int startSector = granuleHalf[g] * kGranuleSectors;
        size_t bytesThisGranule = std::min(bytesLeft, static_cast<size_t>(kGranuleSectors * kSectorSize));
        int sectorsUsed = static_cast<int>((bytesThisGranule + kSectorSize - 1) / kSectorSize);
        if (sectorsUsed == 0) sectorsUsed = 1;

        size_t written = 0;
        for (int s = 0; s < sectorsUsed; ++s) {
            size_t chunk = std::min(bytesThisGranule - written, static_cast<size_t>(kSectorSize));
            size_t off = lsnOffset(track, startSector + s);
            std::memcpy(&disk[off], payload.data() + srcOff + written, chunk);
            written += chunk;
        }

        bool isLast = (g + 1 == granulesNeeded);
        if (isLast) {
            fat[g] = static_cast<uint8_t>(0xC0 | sectorsUsed);
            lastSectorsUsed = sectorsUsed;
        } else {
            fat[g] = static_cast<uint8_t>(g + 1);
        }

        srcOff += bytesThisGranule;
        bytesLeft -= bytesThisGranule;
    }
    std::memcpy(&disk[lsnOffset(17, 1)], fat.data(), kSectorSize);

    std::array<uint8_t, 32> entry{};
    entry.fill(' ');
    std::string base = filename, ext = "BIN";
    auto dot = filename.find('.');
    if (dot != std::string::npos) {
        base = filename.substr(0, dot);
        ext = filename.substr(dot + 1);
    }
    base.resize(8, ' ');
    ext.resize(3, ' ');
    std::memcpy(entry.data(), base.data(), 8);
    std::memcpy(entry.data() + 8, ext.data(), 3);
    entry[11] = 2;
    entry[12] = 0xFF;
    entry[13] = 0;
    entry[14] = static_cast<uint8_t>(lastSectorsUsed == kGranuleSectors ? 0 : (payload.size() % kSectorSize) >> 8);
    entry[15] = static_cast<uint8_t>(payload.size() % kSectorSize);

    std::memcpy(&disk[lsnOffset(17, 3)], entry.data(), 32);
    for (int s = 3; s <= 11; ++s) {
        size_t off = lsnOffset(17, s);
        for (int e = (s == 3 ? 1 : 0); e < 8; ++e) {
            disk[off + e * 32] = 0xFF;
        }
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot open " + path + " for writing");
    out.write(reinterpret_cast<const char*>(disk.data()), disk.size());
}

inline std::vector<uint8_t> readDsk(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::vector<uint8_t> disk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (disk.size() < static_cast<size_t>(kDiskBytes)) {
        throw std::runtime_error(path + " is not a valid 35-track DECB disk image");
    }

    auto lsnOffset = [](int track, int sector) { return (track * kSectorsPerTrack + sector) * kSectorSize; };

    int firstGranule = -1;
    for (int s = 3; s <= 11 && firstGranule < 0; ++s) {
        size_t off = lsnOffset(17, s);
        for (int e = 0; e < 8; ++e) {
            uint8_t nameByte = disk[off + e * 32];
            if (nameByte != 0xFF && nameByte != 0x00) {
                firstGranule = disk[off + e * 32 + 13];
                break;
            }
        }
    }
    if (firstGranule < 0) throw std::runtime_error(path + ": no files found in directory");

    std::array<uint8_t, kSectorSize> fat{};
    std::memcpy(fat.data(), &disk[lsnOffset(17, 1)], kSectorSize);

    std::vector<int> granuleTrack, granuleHalf;
    for (int t = 0; t < kTracks; ++t) {
        if (t == 17) continue;
        for (int h = 0; h < 2; ++h) {
            granuleTrack.push_back(t);
            granuleHalf.push_back(h);
        }
    }

    std::vector<uint8_t> payload;
    int g = firstGranule;
    while (g >= 0 && g < kGranulesPerDisk) {
        uint8_t marker = fat[g];
        int track = granuleTrack[g];
        int startSector = granuleHalf[g] * kGranuleSectors;
        int sectorsUsed = kGranuleSectors;
        bool isLast = (marker & 0xC0) == 0xC0;
        if (isLast) sectorsUsed = marker & 0x3F;
        for (int s = 0; s < sectorsUsed; ++s) {
            size_t off = lsnOffset(track, startSector + s);
            payload.insert(payload.end(), disk.begin() + off, disk.begin() + off + kSectorSize);
        }
        if (isLast) break;
        g = marker;
    }
    return payload;
}

}
