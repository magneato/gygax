#include <gygax/bus/isotp.hpp>

#include <algorithm>
#include <cerrno>
#include <thread>

namespace gygax::bus {

namespace {

constexpr std::uint8_t kSingle = 0x0;
constexpr std::uint8_t kFirst = 0x1;
constexpr std::uint8_t kConsecutive = 0x2;
constexpr std::uint8_t kFlow = 0x3;
constexpr std::size_t kSingleFrameHeaderBytes = 1;
constexpr std::size_t kFirstFrameHeaderBytes = 2;
constexpr std::size_t kConsecutiveFrameHeaderBytes = 1;
constexpr std::size_t kFlowControlFrameBytes = 3;
constexpr std::size_t kMaxClassicSingleFramePayloadBytes = kClassicCanPayloadBytes - kSingleFrameHeaderBytes;
constexpr std::int64_t kMicrosecondsPerMillisecond = 1000;
constexpr std::int64_t kStMinResolutionMicroseconds = 100;
constexpr std::uint8_t kStMinSubMillisecondBase = 0xF0;
constexpr std::uint8_t kStMinSubMillisecondFirst = 0xF1;
constexpr std::uint8_t kStMinSubMillisecondLast = 0xF9;

std::chrono::microseconds decodeStMin(std::uint8_t raw) {
    if (raw <= 0x7F) return std::chrono::milliseconds(raw);
    if (raw >= kStMinSubMillisecondFirst && raw <= kStMinSubMillisecondLast)
        return std::chrono::microseconds(kStMinResolutionMicroseconds * (raw - kStMinSubMillisecondBase));
    return std::chrono::milliseconds(127);
}

std::uint8_t encodeStMin(std::chrono::microseconds t) {
    if (t.count() <= 0) return 0;
    if (t.count() < kMicrosecondsPerMillisecond)
        return static_cast<std::uint8_t>(
            kStMinSubMillisecondBase +
            std::clamp<std::int64_t>((t.count() + kStMinResolutionMicroseconds - 1) / kStMinResolutionMicroseconds, 1, 9));
    return static_cast<std::uint8_t>(
        std::min<std::int64_t>((t.count() + kMicrosecondsPerMillisecond - 1) / kMicrosecondsPerMillisecond, 127));
}

} // namespace

IOResult IsoTpChannel::sendFrame(std::span<const std::uint8_t> bytes) {
    const std::size_t frameMax = options_.fd ? kMaxCanFdPayloadBytes : kClassicCanPayloadBytes;
    if (bytes.size() > frameMax) return -EMSGSIZE;
    CanFrame frame = CanFrame::make(options_.txId, bytes, options_.extended, options_.fd);
    std::size_t target = frame.length;
    if (!options_.fd) {
        if (options_.padding) target = kClassicCanPayloadBytes;
    } else if (frame.length > 8) {
        target = dlcToLength(lengthToDlc(frame.length));
    } else if (options_.padding) {
        target = kClassicCanPayloadBytes;
    }
    for (std::size_t i = frame.length; i < target; ++i) frame.data[i] = options_.paddingByte;
    frame.length = static_cast<std::uint8_t>(target);
    return bus_->send(frame);
}

bool IsoTpChannel::nextFrame(CanFrame& frame, std::chrono::steady_clock::time_point deadline) {
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) + std::chrono::milliseconds(1);
        if (bus_->receive(frame, remaining) != 0) return false;
        if (frame.id == options_.rxId && frame.extended == options_.extended && !frame.remote) return true;
    }
}

IOResult IsoTpChannel::sendFlowControl(std::uint8_t status) {
    const std::uint8_t bytes[kFlowControlFrameBytes] = {static_cast<std::uint8_t>((kFlow << 4) | status), options_.blockSize,
                                                        encodeStMin(options_.separationTime)};
    return sendFrame(bytes);
}

IOResult IsoTpChannel::awaitFlowControl(std::uint8_t& blockSize, std::chrono::microseconds& stMin) {
    const auto deadline = std::chrono::steady_clock::now() + options_.timeout;
    for (int waits = 0; waits <= options_.maxWaitFrames;) {
        CanFrame f;
        if (!nextFrame(f, deadline)) return -ETIMEDOUT;
        if (f.length < 3 || (f.data[0] >> 4) != kFlow) continue;
        switch (f.data[0] & 0x0F) {
        case 0:
            blockSize = f.data[1];
            stMin = decodeStMin(f.data[2]);
            return 0;
        case 1: ++waits; continue;
        case 2: return -EMSGSIZE;
        default: return -EPROTO;
        }
    }
    return -ETIMEDOUT;
}

IOResult IsoTpChannel::send(std::span<const std::uint8_t> payload) {
    if (payload.empty() || payload.size() > kMaxPayload) return -EMSGSIZE;
    const std::size_t frameMax = options_.fd ? kMaxCanFdPayloadBytes : kClassicCanPayloadBytes;
    const std::size_t singleMax = options_.fd ? frameMax - kFirstFrameHeaderBytes : kMaxClassicSingleFramePayloadBytes;

    if (payload.size() <= singleMax) {
        std::vector<std::uint8_t> bytes;
        if (payload.size() <= kMaxClassicSingleFramePayloadBytes) {
            bytes.push_back(static_cast<std::uint8_t>((kSingle << 4) | payload.size()));
        } else {
            bytes.push_back(kSingle << 4);
            bytes.push_back(static_cast<std::uint8_t>(payload.size()));
        }
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        return sendFrame(bytes);
    }

    const std::size_t firstData = frameMax - kFirstFrameHeaderBytes;
    std::vector<std::uint8_t> first;
    first.push_back(static_cast<std::uint8_t>((kFirst << 4) | (payload.size() >> 8)));
    first.push_back(static_cast<std::uint8_t>(payload.size() & 0xFF));
    first.insert(first.end(), payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(firstData));
    if (const auto rc = sendFrame(first); rc != 0) return rc;

    std::size_t offset = firstData;
    std::uint8_t sequence = 1;
    std::uint8_t blockSize = 0;
    std::chrono::microseconds stMin{0};
    std::uint8_t sentInBlock = 0;
    if (const auto rc = awaitFlowControl(blockSize, stMin); rc != 0) return rc;

    const std::size_t consecutiveData = frameMax - kConsecutiveFrameHeaderBytes;
    while (offset < payload.size()) {
        const std::size_t take = std::min(consecutiveData, payload.size() - offset);
        std::vector<std::uint8_t> bytes;
        bytes.push_back(static_cast<std::uint8_t>((kConsecutive << 4) | (sequence & 0x0F)));
        bytes.insert(bytes.end(), payload.begin() + static_cast<std::ptrdiff_t>(offset),
                     payload.begin() + static_cast<std::ptrdiff_t>(offset + take));
        if (const auto rc = sendFrame(bytes); rc != 0) return rc;
        offset += take;
        sequence = static_cast<std::uint8_t>((sequence + 1) & 0x0F);
        ++sentInBlock;
        if (offset >= payload.size()) break;
        if (blockSize != 0 && sentInBlock >= blockSize) {
            sentInBlock = 0;
            if (const auto rc = awaitFlowControl(blockSize, stMin); rc != 0) return rc;
        } else if (stMin.count() > 0) {
            std::this_thread::sleep_for(stMin);
        }
    }
    return 0;
}

IOResult IsoTpChannel::receive(std::vector<std::uint8_t>& payload, std::chrono::milliseconds timeout) {
    payload.clear();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    CanFrame f;
    while (true) {
        if (!nextFrame(f, deadline)) return -ETIMEDOUT;
        if (f.length == 0) continue;
        const std::uint8_t type = f.data[0] >> 4;
        if (type == kSingle) {
            std::size_t len = f.data[0] & 0x0F;
            std::size_t start = 1;
            if (len == 0 && f.length > kClassicCanPayloadBytes) {
                len = f.data[1];
                start = 2;
            }
            if (len == 0 || start + len > f.length) return -EPROTO;
            payload.assign(f.data.begin() + static_cast<std::ptrdiff_t>(start), f.data.begin() + static_cast<std::ptrdiff_t>(start + len));
            return 0;
        }
        if (type == kFirst) break;
    }

    const std::size_t total = (static_cast<std::size_t>(f.data[0] & 0x0F) << 8) | f.data[1];
    if (total == 0 || total > kMaxPayload) {
        (void)sendFlowControl(2);
        return -EMSGSIZE;
    }
    payload.insert(payload.end(), f.data.begin() + 2, f.data.begin() + f.length);
    if (payload.size() > total) payload.resize(total);
    if (const auto rc = sendFlowControl(0); rc != 0) return rc;

    std::uint8_t expected = 1;
    std::uint8_t inBlock = 0;
    while (payload.size() < total) {
        const auto cfDeadline = std::chrono::steady_clock::now() + options_.timeout;
        if (!nextFrame(f, cfDeadline)) return -ETIMEDOUT;
        if (f.length == 0 || (f.data[0] >> 4) != kConsecutive) continue;
        if ((f.data[0] & 0x0F) != expected) return -EILSEQ;
        expected = static_cast<std::uint8_t>((expected + 1) & 0x0F);
        const std::size_t take = std::min<std::size_t>(f.length - 1, total - payload.size());
        payload.insert(payload.end(), f.data.begin() + 1, f.data.begin() + 1 + static_cast<std::ptrdiff_t>(take));
        if (options_.blockSize != 0 && ++inBlock >= options_.blockSize && payload.size() < total) {
            inBlock = 0;
            if (const auto rc = sendFlowControl(0); rc != 0) return rc;
        }
    }
    return 0;
}

} // namespace gygax::bus
