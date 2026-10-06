#include <gygax/bus/canopen.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace gygax::bus::canopen {

namespace {

constexpr std::uint32_t kSdoTx = 0x600;
constexpr std::uint32_t kSdoRx = 0x580;
constexpr std::uint32_t kHeartbeat = 0x700;
constexpr std::uint32_t kNmtIdentifier = 0x000;
constexpr std::uint32_t kSyncIdentifier = 0x080;
constexpr std::uint32_t kAbortTimeout = 0x05040000;
constexpr std::uint32_t kAbortNoObject = 0x06020000;
constexpr std::uint32_t kAbortReadOnly = 0x06010002;
constexpr std::uint32_t kAbortCommand = 0x05040001;
constexpr std::uint32_t kAbortToggleNotAlternated = 0x05030000;
constexpr std::uint8_t kMaxNodeId = 127;
constexpr std::size_t kCanOpenFrameBytes = 8;

CanFrame sdoFrame(std::uint32_t id, const std::uint8_t (&bytes)[8]) {
    return CanFrame::make(id, bytes);
}

} // namespace

CanFrame nmtFrame(NmtCommand command, std::uint8_t nodeId) {
    const std::uint8_t bytes[2] = {static_cast<std::uint8_t>(command), nodeId};
    return CanFrame::make(kNmtIdentifier, bytes);
}

CanFrame syncFrame() {
    return CanFrame::make(kSyncIdentifier, {});
}

std::optional<Heartbeat> parseHeartbeat(const CanFrame& frame) {
    if (frame.extended || frame.remote || frame.length != 1 || frame.id <= kHeartbeat ||
        frame.id > kHeartbeat + kMaxNodeId)
        return std::nullopt;
    const auto state = frame.data[0] & 0x7F;
    if (state != 0x00 && state != 0x04 && state != 0x05 && state != 0x7F) return std::nullopt;
    return Heartbeat{static_cast<std::uint8_t>(frame.id - kHeartbeat), static_cast<NmtState>(state)};
}

const char* abortText(std::uint32_t code) {
    switch (code) {
    case 0x05030000: return "toggle bit not alternated";
    case kAbortTimeout: return "SDO protocol timed out";
    case kAbortCommand: return "command specifier not valid or unknown";
    case 0x06010000: return "unsupported access to an object";
    case 0x06010001: return "attempt to read a write-only object";
    case kAbortReadOnly: return "attempt to write a read-only object";
    case kAbortNoObject: return "object does not exist in the dictionary";
    case 0x06070010: return "data type does not match, length of service parameter does not match";
    case 0x06090011: return "sub-index does not exist";
    case 0x08000000: return "general error";
    default: return "unknown abort code";
    }
}

IOResult SdoClient::awaitReply(std::uint8_t nodeId, CanFrame& reply) {
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()) +
                               std::chrono::milliseconds(1);
        const auto rc = bus_->receive(reply, remaining);
        if (rc != 0) return rc;
        if (!reply.extended && reply.id == kSdoRx + nodeId && reply.length == 8) return 0;
    }
    return -ETIMEDOUT;
}

IOResult SdoClient::exchange(std::uint8_t nodeId, const std::uint8_t (&request)[8], CanFrame& reply) {
    if (nodeId == 0 || nodeId > kMaxNodeId) return -EINVAL;
    if (const auto rc = bus_->send(sdoFrame(kSdoTx + nodeId, request)); rc != 0) return rc;
    if (const auto rc = awaitReply(nodeId, reply); rc != 0) return rc;
    if ((reply.data[0] >> 5) == 4) {
        std::memcpy(&abort_, &reply.data[4], 4);
        return -EREMOTEIO;
    }
    return 0;
}

IOResult SdoClient::read(std::uint8_t nodeId, std::uint16_t index, std::uint8_t subIndex, std::vector<std::uint8_t>& out) {
    abort_ = 0;
    out.clear();
    std::uint8_t req[kCanOpenFrameBytes] = {0x40, static_cast<std::uint8_t>(index & 0xFF),
                                            static_cast<std::uint8_t>(index >> 8), subIndex, 0, 0, 0, 0};
    CanFrame reply;
    if (const auto rc = exchange(nodeId, req, reply); rc != 0) return rc;
    const std::uint8_t cs = reply.data[0];
    if ((cs >> 5) != 2) return -EPROTO;
    if ((cs & 0x02) != 0) {
        const std::size_t size = (cs & 0x01) != 0 ? 4 - ((cs >> 2) & 0x03) : 4;
        out.assign(reply.data.begin() + 4, reply.data.begin() + 4 + static_cast<std::ptrdiff_t>(size));
        return 0;
    }
    std::uint32_t total = 0;
    if ((cs & 0x01) != 0) std::memcpy(&total, &reply.data[4], 4);
    bool toggle = false;
    while (true) {
        std::uint8_t seg[kCanOpenFrameBytes] = {static_cast<std::uint8_t>(0x60 | (toggle ? 0x10 : 0x00)), 0, 0, 0, 0, 0, 0, 0};
        if (const auto rc = exchange(nodeId, seg, reply); rc != 0) return rc;
        const std::uint8_t scs = reply.data[0];
        if ((scs >> 5) != 0 || ((scs & 0x10) != 0) != toggle) return -EPROTO;
        const std::size_t unused = (scs >> 1) & 0x07;
        const std::size_t count = 7 - unused;
        out.insert(out.end(), reply.data.begin() + 1, reply.data.begin() + 1 + static_cast<std::ptrdiff_t>(count));
        if ((scs & 0x01) != 0) break;
        toggle = !toggle;
        if (out.size() > (1U << 20)) return -EMSGSIZE;
    }
    if (total != 0 && out.size() != total) return -EPROTO;
    return 0;
}

IOResult SdoClient::write(std::uint8_t nodeId, std::uint16_t index, std::uint8_t subIndex, std::span<const std::uint8_t> data) {
    abort_ = 0;
    if (data.empty()) return -EINVAL;
    CanFrame reply;
    if (data.size() <= 4) {
        std::uint8_t req[kCanOpenFrameBytes] = {static_cast<std::uint8_t>(0x23 | ((4 - data.size()) << 2)),
                               static_cast<std::uint8_t>(index & 0xFF),
                               static_cast<std::uint8_t>(index >> 8),
                               subIndex,
                               0,
                               0,
                               0,
                               0};
        std::copy(data.begin(), data.end(), req + 4);
        if (const auto rc = exchange(nodeId, req, reply); rc != 0) return rc;
        return (reply.data[0] >> 5) == 3 ? 0 : -EPROTO;
    }
    const auto total = static_cast<std::uint32_t>(data.size());
    std::uint8_t init[kCanOpenFrameBytes] = {0x21, static_cast<std::uint8_t>(index & 0xFF),
                                             static_cast<std::uint8_t>(index >> 8), subIndex, 0, 0, 0, 0};
    std::memcpy(&init[4], &total, 4);
    if (const auto rc = exchange(nodeId, init, reply); rc != 0) return rc;
    if ((reply.data[0] >> 5) != 3) return -EPROTO;
    bool toggle = false;
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t take = std::min<std::size_t>(kClassicCanPayloadBytes - 1, data.size() - offset);
        std::uint8_t seg[kCanOpenFrameBytes] = {0};
        seg[0] = static_cast<std::uint8_t>((toggle ? 0x10 : 0x00) | ((7 - take) << 1) | (offset + take >= data.size() ? 0x01 : 0x00));
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(offset), take, seg + 1);
        if (const auto rc = exchange(nodeId, seg, reply); rc != 0) return rc;
        if ((reply.data[0] >> 5) != 1 || ((reply.data[0] & 0x10) != 0) != toggle) return -EPROTO;
        offset += take;
        toggle = !toggle;
    }
    return 0;
}

void SdoServer::abort(std::uint16_t index, std::uint8_t sub, std::uint32_t code) {
    std::uint8_t out[8] = {0x80, static_cast<std::uint8_t>(index & 0xFF), static_cast<std::uint8_t>(index >> 8), sub, 0, 0, 0, 0};
    std::memcpy(&out[4], &code, 4);
    (void)bus_->send(sdoFrame(kSdoRx + nodeId_, out));
    downloading_ = false;
    pendingUpload_.clear();
}

bool SdoServer::serveOne(std::chrono::milliseconds timeout) {
    CanFrame f;
    while (true) {
        if (bus_->receive(f, timeout) != 0) return false;
        if (!f.extended && f.id == kSdoTx + nodeId_ && f.length == 8) break;
    }
    const std::uint8_t ccs = f.data[0] >> 5;
    const auto index = static_cast<std::uint16_t>(f.data[1] | (f.data[2] << 8));
    const std::uint8_t sub = f.data[3];
    std::uint8_t out[8] = {0};

    if (ccs == 2) {
        auto value = reader_ ? reader_(index, sub) : std::nullopt;
        if (!value) {
            abort(index, sub, kAbortNoObject);
            return true;
        }
        out[1] = f.data[1];
        out[2] = f.data[2];
        out[3] = sub;
        if (value->size() <= 4) {
            out[0] = static_cast<std::uint8_t>(0x43 | ((4 - value->size()) << 2));
            std::copy(value->begin(), value->end(), out + 4);
        } else {
            out[0] = 0x41;
            const auto size = static_cast<std::uint32_t>(value->size());
            std::memcpy(&out[4], &size, 4);
            pendingUpload_ = std::move(*value);
            uploadOffset_ = 0;
            uploadToggle_ = false;
        }
    } else if (ccs == 3) {
        if (pendingUpload_.empty() || ((f.data[0] & 0x10) != 0) != uploadToggle_) {
            abort(index, sub, kAbortCommand);
            return true;
        }
        const std::size_t take = std::min<std::size_t>(7, pendingUpload_.size() - uploadOffset_);
        const bool last = uploadOffset_ + take >= pendingUpload_.size();
        out[0] = static_cast<std::uint8_t>((uploadToggle_ ? 0x10 : 0x00) | ((7 - take) << 1) | (last ? 0x01 : 0x00));
        std::copy_n(pendingUpload_.begin() + static_cast<std::ptrdiff_t>(uploadOffset_), take, out + 1);
        uploadOffset_ += take;
        uploadToggle_ = !uploadToggle_;
        if (last) pendingUpload_.clear();
    } else if (ccs == 1) {
        const std::uint8_t cs = f.data[0];
        if ((cs & 0x02) != 0) {
            const std::size_t size = (cs & 0x01) != 0 ? 4 - ((cs >> 2) & 0x03) : 4;
            if (!writer_ || !writer_(index, sub, std::span<const std::uint8_t>(f.data.data() + 4, size))) {
                abort(index, sub, kAbortReadOnly);
                return true;
            }
            out[0] = 0x60;
            out[1] = f.data[1];
            out[2] = f.data[2];
            out[3] = sub;
        } else {
            std::uint32_t size = 0;
            std::memcpy(&size, &f.data[4], 4);
            downloading_ = true;
            downloadSize_ = size;
            downloadIndex_ = index;
            downloadSub_ = sub;
            downloadToggle_ = false;
            pendingDownload_.clear();
            out[0] = 0x60;
            out[1] = f.data[1];
            out[2] = f.data[2];
            out[3] = sub;
        }
    } else if (ccs == 0 && downloading_) {
        if (((f.data[0] & 0x10) != 0) != downloadToggle_) {
            abort(downloadIndex_, downloadSub_, kAbortToggleNotAlternated);
            return true;
        }
        const std::size_t count = 7 - ((f.data[0] >> 1) & 0x07);
        pendingDownload_.insert(pendingDownload_.end(), f.data.begin() + 1, f.data.begin() + 1 + static_cast<std::ptrdiff_t>(count));
        out[0] = static_cast<std::uint8_t>(0x20 | (downloadToggle_ ? 0x10 : 0x00));
        downloadToggle_ = !downloadToggle_;
        if ((f.data[0] & 0x01) != 0) {
            downloading_ = false;
            if (!writer_ || (downloadSize_ != 0 && pendingDownload_.size() != downloadSize_) ||
                !writer_(downloadIndex_, downloadSub_, pendingDownload_)) {
                abort(downloadIndex_, downloadSub_, kAbortReadOnly);
                return true;
            }
        }
    } else {
        abort(index, sub, kAbortCommand);
        return true;
    }
    (void)bus_->send(sdoFrame(kSdoRx + nodeId_, out));
    return true;
}

} // namespace gygax::bus::canopen
