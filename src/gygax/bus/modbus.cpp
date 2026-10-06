#include <gygax/bus/modbus.hpp>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstring>
#include <thread>

namespace gygax::bus::modbus {

namespace {

constexpr std::uint8_t kReadCoilsFunction = 0x01;
constexpr std::uint8_t kReadDiscreteInputsFunction = 0x02;
constexpr std::uint8_t kReadHoldingRegistersFunction = 0x03;
constexpr std::uint8_t kReadInputRegistersFunction = 0x04;
constexpr std::uint8_t kWriteSingleCoilFunction = 0x05;
constexpr std::uint8_t kWriteSingleRegisterFunction = 0x06;
constexpr std::uint8_t kWriteMultipleCoilsFunction = 0x0F;
constexpr std::uint8_t kWriteMultipleRegistersFunction = 0x10;
constexpr std::uint8_t kMaskWriteRegisterFunction = 0x16;
constexpr std::uint16_t kMaxReadBitsPerRequest = 2000;
constexpr std::uint16_t kMaxReadRegistersPerRequest = 125;
constexpr std::size_t kMaxWriteCoilsPerRequest = 1968;
constexpr std::size_t kMaxWriteRegistersPerRequest = 123;
constexpr std::uint16_t kModbusCrcInitialValue = 0xFFFF;
constexpr std::uint16_t kModbusCrcPolynomial = 0xA001;
constexpr std::size_t kModbusTcpHeaderBytes = 6;
constexpr std::size_t kModbusTcpRequestBytes = 8;
constexpr std::size_t kMaxRtuBufferBytes = 256;

std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

void putBe16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

std::size_t expectedRtuResponse(std::span<const std::uint8_t> partial) {
    if (partial.size() < 2) return 0;
    const std::uint8_t fn = partial[1];
    if ((fn & 0x80) != 0) return 5;
    switch (fn) {
    case kReadCoilsFunction:
    case kReadDiscreteInputsFunction:
    case kReadHoldingRegistersFunction:
    case kReadInputRegistersFunction:
    case 0x17: return partial.size() < 3 ? 0 : 3 + static_cast<std::size_t>(partial[2]) + 2;
    case kWriteSingleCoilFunction:
    case kWriteSingleRegisterFunction:
    case kWriteMultipleCoilsFunction:
    case kWriteMultipleRegistersFunction: return kModbusTcpRequestBytes;
    case kMaskWriteRegisterFunction: return 10;
    default: return 0;
    }
}

} // namespace

std::uint16_t crc16(std::span<const std::uint8_t> data) {
    std::uint16_t crc = kModbusCrcInitialValue;
    for (const std::uint8_t byte : data) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i)
            crc = (crc & 1U) != 0 ? static_cast<std::uint16_t>((crc >> 1) ^ kModbusCrcPolynomial)
                                  : static_cast<std::uint16_t>(crc >> 1);
    }
    return crc;
}

std::vector<std::uint8_t> frameRtu(std::uint8_t unit, std::uint8_t function, std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> out{unit, function};
    out.insert(out.end(), data.begin(), data.end());
    const auto crc = crc16(out);
    out.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    out.push_back(static_cast<std::uint8_t>(crc >> 8));
    return out;
}

std::vector<std::uint8_t> frameTcp(std::uint16_t transaction, std::uint8_t unit, std::uint8_t function,
                                   std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> out;
    putBe16(out, transaction);
    putBe16(out, 0);
    putBe16(out, static_cast<std::uint16_t>(data.size() + 2));
    out.push_back(unit);
    out.push_back(function);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::optional<Request> parseRtu(std::span<const std::uint8_t> frame) {
    if (frame.size() < 4) return std::nullopt;
    const auto crc = crc16(frame.first(frame.size() - 2));
    if ((crc & 0xFF) != frame[frame.size() - 2] || (crc >> 8) != frame[frame.size() - 1]) return std::nullopt;
    Request r;
    r.unit = frame[0];
    r.function = frame[1];
    r.data.assign(frame.begin() + 2, frame.end() - 2);
    return r;
}

std::optional<Request> parseTcp(std::span<const std::uint8_t> frame, std::uint16_t* transaction) {
    if (frame.size() < kModbusTcpRequestBytes || be16(&frame[2]) != 0) return std::nullopt;
    const std::size_t length = be16(&frame[4]);
    if (length < 2 || frame.size() < kModbusTcpHeaderBytes + length) return std::nullopt;
    if (transaction != nullptr) *transaction = be16(&frame[0]);
    Request r;
    r.unit = frame[6];
    r.function = frame[7];
    r.data.assign(frame.begin() + kModbusTcpRequestBytes,
                  frame.begin() + kModbusTcpHeaderBytes + static_cast<std::ptrdiff_t>(length));
    return r;
}

const char* exceptionText(std::uint8_t code) {
    switch (code) {
    case 1: return "illegal function";
    case 2: return "illegal data address";
    case 3: return "illegal data value";
    case 4: return "server device failure";
    case 5: return "acknowledge";
    case 6: return "server device busy";
    case 8: return "memory parity error";
    case 10: return "gateway path unavailable";
    case 11: return "gateway target device failed to respond";
    default: return "unknown exception";
    }
}

float Client::registersToFloat(std::uint16_t high, std::uint16_t low) {
    return std::bit_cast<float>((static_cast<std::uint32_t>(high) << 16) | low);
}

std::pair<std::uint16_t, std::uint16_t> Client::floatToRegisters(float value) {
    const auto raw = std::bit_cast<std::uint32_t>(value);
    return {static_cast<std::uint16_t>(raw >> 16), static_cast<std::uint16_t>(raw & 0xFFFF)};
}

Client::Client(std::shared_ptr<net::ByteLink> link, Mode mode, std::chrono::milliseconds timeout)
    : link_(std::move(link)), mode_(mode), timeout_(timeout) {}

IOResult Client::call(std::uint8_t unit, std::uint8_t function, std::span<const std::uint8_t> data, std::vector<std::uint8_t>& response) {
    exception_ = 0;
    response.clear();
    const std::uint16_t transaction = ++transaction_;
    const auto request = mode_ == Mode::Rtu ? frameRtu(unit, function, data) : frameTcp(transaction, unit, function, data);
    pending_.clear();
    std::vector<std::uint8_t> chunk;
    while (link_->read(chunk, std::chrono::milliseconds(0)) == 0) {
    }
    if (const auto rc = link_->write(request); rc != 0) return rc;

    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    while (true) {
        std::optional<Request> parsed;
        if (mode_ == Mode::Rtu) {
            const auto need = expectedRtuResponse(pending_);
            if (need != 0 && pending_.size() >= need) {
                parsed = parseRtu(std::span<const std::uint8_t>(pending_).first(need));
                if (!parsed) return -EBADMSG;
                if (parsed->unit != unit) {
                    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(need));
                    parsed.reset();
                }
            }
        } else if (pending_.size() >= 8) {
            const std::size_t length = be16(&pending_[4]);
            if (pending_.size() >= 6 + length) {
                std::uint16_t tid = 0;
                parsed = parseTcp(pending_, &tid);
                if (!parsed) return -EBADMSG;
                if (tid != transaction) {
                    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(6 + length));
                    parsed.reset();
                }
            }
        }
        if (parsed) {
            if ((parsed->function & 0x80) != 0) {
                exception_ = parsed->data.empty() ? 0 : parsed->data[0];
                return -EREMOTEIO;
            }
            if (parsed->function != function) return -EPROTO;
            response = std::move(parsed->data);
            return 0;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return -ETIMEDOUT;
        const auto rc =
            link_->read(chunk, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) + std::chrono::milliseconds(1));
        if (rc == 0)
            pending_.insert(pending_.end(), chunk.begin(), chunk.end());
        else if (rc != -ETIMEDOUT)
            return rc;
    }
}

IOResult Client::readBits(std::uint8_t function, std::uint8_t unit, std::uint16_t address, std::uint16_t count, std::vector<bool>& out) {
    if (count == 0 || count > kMaxReadBitsPerRequest) return -EINVAL;
    std::vector<std::uint8_t> req;
    putBe16(req, address);
    putBe16(req, count);
    std::vector<std::uint8_t> resp;
    if (const auto rc = call(unit, function, req, resp); rc != 0) return rc;
    const std::size_t bytes = (static_cast<std::size_t>(count) + 7) / 8;
    if (resp.size() != bytes + 1 || resp[0] != bytes) return -EPROTO;
    out.assign(count, false);
    for (std::size_t i = 0; i < count; ++i) out[i] = ((resp[1 + i / 8] >> (i % 8)) & 1U) != 0;
    return 0;
}

IOResult Client::readWords(std::uint8_t function, std::uint8_t unit, std::uint16_t address, std::uint16_t count,
                           std::vector<std::uint16_t>& out) {
    if (count == 0 || count > kMaxReadRegistersPerRequest) return -EINVAL;
    std::vector<std::uint8_t> req;
    putBe16(req, address);
    putBe16(req, count);
    std::vector<std::uint8_t> resp;
    if (const auto rc = call(unit, function, req, resp); rc != 0) return rc;
    if (resp.size() != static_cast<std::size_t>(count) * 2 + 1 || resp[0] != count * 2) return -EPROTO;
    out.resize(count);
    for (std::size_t i = 0; i < count; ++i) out[i] = be16(&resp[1 + 2 * i]);
    return 0;
}

IOResult Client::readCoils(std::uint8_t unit, std::uint16_t address, std::uint16_t count, std::vector<bool>& out) {
    return readBits(kReadCoilsFunction, unit, address, count, out);
}
IOResult Client::readDiscreteInputs(std::uint8_t unit, std::uint16_t address, std::uint16_t count, std::vector<bool>& out) {
    return readBits(kReadDiscreteInputsFunction, unit, address, count, out);
}
IOResult Client::readHoldingRegisters(std::uint8_t unit, std::uint16_t address, std::uint16_t count, std::vector<std::uint16_t>& out) {
    return readWords(kReadHoldingRegistersFunction, unit, address, count, out);
}
IOResult Client::readInputRegisters(std::uint8_t unit, std::uint16_t address, std::uint16_t count, std::vector<std::uint16_t>& out) {
    return readWords(kReadInputRegistersFunction, unit, address, count, out);
}

IOResult Client::writeCoil(std::uint8_t unit, std::uint16_t address, bool value) {
    std::vector<std::uint8_t> req;
    putBe16(req, address);
    putBe16(req, value ? 0xFF00 : 0x0000);
    std::vector<std::uint8_t> resp;
    if (const auto rc = call(unit, kWriteSingleCoilFunction, req, resp); rc != 0) return rc;
    return resp == req ? 0 : -EPROTO;
}

IOResult Client::writeRegister(std::uint8_t unit, std::uint16_t address, std::uint16_t value) {
    std::vector<std::uint8_t> req;
    putBe16(req, address);
    putBe16(req, value);
    std::vector<std::uint8_t> resp;
    if (const auto rc = call(unit, kWriteSingleRegisterFunction, req, resp); rc != 0) return rc;
    return resp == req ? 0 : -EPROTO;
}

IOResult Client::writeCoils(std::uint8_t unit, std::uint16_t address, const std::vector<bool>& values) {
    if (values.empty() || values.size() > kMaxWriteCoilsPerRequest) return -EINVAL;
    std::vector<std::uint8_t> req;
    putBe16(req, address);
    putBe16(req, static_cast<std::uint16_t>(values.size()));
    const std::size_t bytes = (values.size() + 7) / 8;
    req.push_back(static_cast<std::uint8_t>(bytes));
    std::vector<std::uint8_t> packed(bytes, 0);
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i]) packed[i / 8] = static_cast<std::uint8_t>(packed[i / 8] | (1U << (i % 8)));
    }
    req.insert(req.end(), packed.begin(), packed.end());
    std::vector<std::uint8_t> resp;
    if (const auto rc = call(unit, kWriteMultipleCoilsFunction, req, resp); rc != 0) return rc;
    return resp.size() == 4 && be16(&resp[0]) == address && be16(&resp[2]) == values.size() ? 0 : -EPROTO;
}

IOResult Client::writeRegisters(std::uint8_t unit, std::uint16_t address, const std::vector<std::uint16_t>& values) {
    if (values.empty() || values.size() > kMaxWriteRegistersPerRequest) return -EINVAL;
    std::vector<std::uint8_t> req;
    putBe16(req, address);
    putBe16(req, static_cast<std::uint16_t>(values.size()));
    req.push_back(static_cast<std::uint8_t>(values.size() * 2));
    for (const auto v : values) putBe16(req, v);
    std::vector<std::uint8_t> resp;
    if (const auto rc = call(unit, kWriteMultipleRegistersFunction, req, resp); rc != 0) return rc;
    return resp.size() == 4 && be16(&resp[0]) == address && be16(&resp[2]) == values.size() ? 0 : -EPROTO;
}

IOResult Client::maskWriteRegister(std::uint8_t unit, std::uint16_t address, std::uint16_t andMask, std::uint16_t orMask) {
    std::vector<std::uint8_t> req;
    putBe16(req, address);
    putBe16(req, andMask);
    putBe16(req, orMask);
    std::vector<std::uint8_t> resp;
    if (const auto rc = call(unit, kMaskWriteRegisterFunction, req, resp); rc != 0) return rc;
    return resp == req ? 0 : -EPROTO;
}

std::optional<std::vector<std::uint8_t>> Slave::process(const Request& r) {
    auto exception = [&](std::uint8_t code) { return std::vector<std::uint8_t>{code}; };
    auto reply = [&](std::uint8_t function, std::vector<std::uint8_t> data) {
        data.insert(data.begin(), function);
        return data;
    };
    const auto& d = r.data;
    switch (r.function) {
    case 1:
    case 2: {
        if (d.size() != 4) return reply(r.function | 0x80, exception(3));
        const std::uint16_t address = be16(&d[0]);
        const std::uint16_t count = be16(&d[2]);
        if (count == 0 || count > kMaxReadBitsPerRequest) return reply(r.function | 0x80, exception(3));
        const auto& table = r.function == kReadCoilsFunction ? coils : discrete;
        std::vector<std::uint8_t> out(1 + (count + 7) / 8, 0);
        out[0] = static_cast<std::uint8_t>(out.size() - 1);
        for (std::uint16_t i = 0; i < count; ++i) {
            auto it = table.find(static_cast<std::uint16_t>(address + i));
            if (it == table.end()) return reply(r.function | 0x80, exception(2));
            if (it->second) out[1 + i / 8] = static_cast<std::uint8_t>(out[1 + i / 8] | (1U << (i % 8)));
        }
        return reply(r.function, out);
    }
    case 3:
    case 4: {
        if (d.size() != 4) return reply(r.function | 0x80, exception(3));
        const std::uint16_t address = be16(&d[0]);
        const std::uint16_t count = be16(&d[2]);
        if (count == 0 || count > kMaxReadRegistersPerRequest) return reply(r.function | 0x80, exception(3));
        const auto& table = r.function == kReadHoldingRegistersFunction ? holding : input;
        std::vector<std::uint8_t> out{static_cast<std::uint8_t>(count * 2)};
        for (std::uint16_t i = 0; i < count; ++i) {
            auto it = table.find(static_cast<std::uint16_t>(address + i));
            if (it == table.end()) return reply(r.function | 0x80, exception(2));
            putBe16(out, it->second);
        }
        return reply(r.function, out);
    }
    case 5: {
        if (d.size() != 4) return reply(0x85, exception(3));
        const std::uint16_t address = be16(&d[0]);
        const std::uint16_t value = be16(&d[2]);
        if (value != 0xFF00 && value != 0x0000) return reply(0x85, exception(3));
        if (!coils.contains(address)) return reply(0x85, exception(2));
        coils[address] = value == 0xFF00;
        return reply(5, d);
    }
    case 6: {
        if (d.size() != 4) return reply(0x86, exception(3));
        const std::uint16_t address = be16(&d[0]);
        if (!holding.contains(address)) return reply(0x86, exception(2));
        holding[address] = be16(&d[2]);
        return reply(6, d);
    }
    case 0x0F: {
        if (d.size() < 5) return reply(0x8F, exception(3));
        const std::uint16_t address = be16(&d[0]);
        const std::uint16_t count = be16(&d[2]);
        if (count == 0 || d[4] != (count + 7) / 8 || d.size() != 5U + d[4]) return reply(0x8F, exception(3));
        for (std::uint16_t i = 0; i < count; ++i) {
            if (!coils.contains(static_cast<std::uint16_t>(address + i))) return reply(0x8F, exception(2));
        }
        for (std::uint16_t i = 0; i < count; ++i) coils[static_cast<std::uint16_t>(address + i)] = ((d[5 + i / 8] >> (i % 8)) & 1U) != 0;
        return reply(0x0F, std::vector<std::uint8_t>(d.begin(), d.begin() + 4));
    }
    case 0x10: {
        if (d.size() < 5) return reply(0x90, exception(3));
        const std::uint16_t address = be16(&d[0]);
        const std::uint16_t count = be16(&d[2]);
        if (count == 0 || d[4] != count * 2 || d.size() != 5U + d[4]) return reply(0x90, exception(3));
        for (std::uint16_t i = 0; i < count; ++i) {
            if (!holding.contains(static_cast<std::uint16_t>(address + i))) return reply(0x90, exception(2));
        }
        for (std::uint16_t i = 0; i < count; ++i) holding[static_cast<std::uint16_t>(address + i)] = be16(&d[5 + 2 * i]);
        return reply(0x10, std::vector<std::uint8_t>(d.begin(), d.begin() + 4));
    }
    case 0x16: {
        if (d.size() != 6) return reply(0x96, exception(3));
        const std::uint16_t address = be16(&d[0]);
        auto it = holding.find(address);
        if (it == holding.end()) return reply(0x96, exception(2));
        it->second = static_cast<std::uint16_t>((it->second & be16(&d[2])) | (be16(&d[4]) & ~be16(&d[2])));
        return reply(0x16, d);
    }
    default: return reply(r.function | 0x80, exception(1));
    }
}

bool Slave::serveOne(net::ByteLink& link, Mode mode, std::chrono::milliseconds timeout) {
    std::vector<std::uint8_t> chunk;
    if (link.read(chunk, timeout) != 0) return false;
    buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
    while (true) {
        std::optional<Request> request;
        std::size_t consumed = 0;
        std::uint16_t transaction = 0;
        if (mode == Mode::Tcp) {
                if (buffer_.size() < kModbusTcpRequestBytes) return true;
            const std::size_t length = be16(&buffer_[4]);
            if (buffer_.size() < kModbusTcpHeaderBytes + length) return true;
            request = parseTcp(buffer_, &transaction);
            consumed = kModbusTcpHeaderBytes + length;
        } else {
            if (buffer_.size() < 4) return true;
            const std::size_t limit = std::min<std::size_t>(buffer_.size(), kMaxRtuBufferBytes);
            for (std::size_t n = 4; n <= limit; ++n) {
                if (auto parsed = parseRtu(std::span<const std::uint8_t>(buffer_).first(n))) {
                    request = std::move(parsed);
                    consumed = n;
                    break;
                }
            }
            if (!request) {
                if (buffer_.size() >= kMaxRtuBufferBytes) buffer_.clear();
                return true;
            }
        }
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
        if (!request || (request->unit != unit_ && request->unit != 0)) continue;
        auto response = process(*request);
        if (!response || request->unit == 0) continue;
        const std::uint8_t function = (*response)[0];
        const std::span<const std::uint8_t> data(response->data() + 1, response->size() - 1);
        const auto wire = mode == Mode::Tcp ? frameTcp(transaction, unit_, function, data) : frameRtu(unit_, function, data);
        (void)link.write(wire);
    }
}

} // namespace gygax::bus::modbus
