#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include <gygax/transport/gcode.hpp>
#include <gygax/transport/gpio.hpp>
#include <gygax/transport/modem.hpp>
#include <gygax/transport/printer.hpp>
#include <gygax/transport/serial.hpp>

#include "support.hpp"

using namespace gygax::transport;
using gygax::support::Pty;
using gygax::support::waitUntil;

namespace {

class SerialTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(pty.valid());
        ASSERT_EQ(serial.initialize(), 0);
        ASSERT_EQ(serial.open(pty.slavePath, handle), 0);
    }

    void TearDown() override { (void)serial.shutdown(); }

    Pty pty;
    SerialTransport serial;
    TransportHandle handle = 0;
    std::string received;
};

class PtyPeer {
public:
    using Responder = std::function<std::string(const std::string& line)>;

    PtyPeer(Pty& pty, Responder responder) : pty_(pty), responder_(std::move(responder)) {
        thread_ = std::thread([this] {
            std::string pending;
            while (running_.load()) {
                pending += pty_.readAvailable();
                if (const auto esc = pending.find("+++"); esc != std::string::npos) {
                    pending.erase(esc, 3);
                    record("+++");
                    pty_.write("\r\nOK\r\n");
                }
                std::size_t pos;
                while ((pos = pending.find_first_of("\r\n")) != std::string::npos) {
                    const std::string line = pending.substr(0, pos);
                    pending.erase(0, pos + 1);
                    if (line.empty()) continue;
                    record(line);
                    const auto reply = responder_(line);
                    if (!reply.empty()) pty_.write(reply);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    ~PtyPeer() {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
    }

    std::vector<std::string> lines() const {
        std::lock_guard lock(mutex_);
        return lines_;
    }

private:
    void record(const std::string& line) {
        std::lock_guard lock(mutex_);
        lines_.push_back(line);
    }

    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
    Pty& pty_;
    Responder responder_;
    std::atomic<bool> running_{true};
    std::thread thread_;
};

}

TEST_F(SerialTest, ReadsBufferedBytesAndReportsAvailability) {
    pty.write("hello");
    ASSERT_EQ(serial.waitReadable(handle, 5, std::chrono::seconds(2)), 0);
    std::size_t n = 0;
    ASSERT_EQ(serial.available(handle, n), 0);
    EXPECT_EQ(n, 5U);
    std::uint8_t buf[16];
    ASSERT_EQ(serial.read(handle, buf, sizeof(buf), n), 0);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(buf), n), "hello");
    ASSERT_EQ(serial.available(handle, n), 0);
    EXPECT_EQ(n, 0U);
}

TEST_F(SerialTest, WritesReachThePeer) {
    const std::string msg = "ping\n";
    std::size_t written = 0;
    ASSERT_EQ(serial.write(handle, reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size(), written), 0);
    EXPECT_EQ(written, msg.size());
    ASSERT_TRUE(waitUntil([&] {
        received += pty.readAvailable();
        return received.find("ping") != std::string::npos;
    }));
}

TEST_F(SerialTest, CallbackReceivesDataInsteadOfBuffering) {
    std::string seen;
    std::mutex m;
    ASSERT_EQ(serial.setCallback(handle,
                                 [&](TransportHandle, const std::uint8_t* d, std::size_t n) {
                                     std::lock_guard lock(m);
                                     seen.append(reinterpret_cast<const char*>(d), n);
                                 }),
              0);
    pty.write("abc");
    ASSERT_TRUE(waitUntil([&] {
        std::lock_guard lock(m);
        return seen == "abc";
    }));
    std::size_t n = 99;
    ASSERT_EQ(serial.available(handle, n), 0);
    EXPECT_EQ(n, 0U);
}

TEST_F(SerialTest, ConfigureLinesAndErrors) {
    EXPECT_EQ(serial.configure(handle, BaudRate::Baud9600, DataBits::Bits7, StopBits::Bits2, Parity::Even, FlowControl::RtsCts), 0);
    EXPECT_EQ(serial.configure(handle, BaudRate::Baud115200), 0);
    EXPECT_EQ(serial.configure(9999, BaudRate::Baud9600), -EBADF);
    EXPECT_EQ(serial.setLine(handle, "bogus", true), -EINVAL);
    EXPECT_EQ(serial.setLine(handle, "cts", true), -EINVAL);
    std::size_t n = 0;
    std::uint8_t b = 0;
    EXPECT_EQ(serial.read(9999, &b, 1, n), -EBADF);
    EXPECT_EQ(serial.close(9999), -EBADF);
    EXPECT_EQ(serial.close(handle), 0);
    EXPECT_EQ(serial.available(handle, n), -EBADF);
}

TEST_F(SerialTest, OpeningMissingDevicesFails) {
    TransportHandle h = 0;
    EXPECT_EQ(serial.open("/dev/gygax-does-not-exist", h), -ENOENT);
}

TEST_F(SerialTest, FlushDiscardsBufferedInput) {
    pty.write("junk");
    ASSERT_EQ(serial.waitReadable(handle, 4, std::chrono::seconds(2)), 0);
    ASSERT_EQ(serial.flushInput(handle), 0);
    std::size_t n = 1;
    ASSERT_EQ(serial.available(handle, n), 0);
    EXPECT_EQ(n, 0U);
    EXPECT_EQ(serial.waitReadable(handle, 1, std::chrono::milliseconds(20)), -ETIMEDOUT);
}

TEST_F(SerialTest, LargeWritesAreDeliveredCompletely) {
    const std::string payload(64 * 1024, 'z');
    std::atomic<std::size_t> total{0};
    std::atomic<bool> drain{true};
    std::thread reader([&] {
        while (drain.load() || total.load() < payload.size()) {
            total += pty.readAvailable().size();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (total.load() >= payload.size()) break;
        }
    });
    EXPECT_EQ(serial.writeAll(handle, payload, std::chrono::seconds(10)), 0);
    ASSERT_TRUE(waitUntil([&] { return total.load() >= payload.size(); }, std::chrono::seconds(10)));
    drain = false;
    reader.join();
}

TEST_F(SerialTest, EnumerateDevicesReturnsSortedPaths) {
    const auto devices = serial.enumerateDevices();
    EXPECT_TRUE(std::is_sorted(devices.begin(), devices.end()));
}

TEST(SerialLifecycle, RequiresInitializeAndCleansUp) {
    SerialTransport s;
    TransportHandle h = 0;
    Pty pty;
    ASSERT_TRUE(pty.valid());
    EXPECT_EQ(s.open(pty.slavePath, h), -EINVAL);
    EXPECT_EQ(s.initialize(), 0);
    EXPECT_EQ(s.initialize(), 0);
    EXPECT_EQ(s.open(pty.slavePath, h), 0);
    EXPECT_EQ(s.shutdown(), 0);
    EXPECT_EQ(s.shutdown(), 0);
}

TEST_F(SerialTest, HayesModemDialsAndTransfersData) {
    PtyPeer peer(pty, [](const std::string& line) -> std::string {
        if (line == "ATZ") return "\r\nOK\r\n";
        if (line == "ATE0V1Q0") return "\r\nOK\r\n";
        if (line.rfind("ATDT", 0) == 0) return "\r\nCONNECT 2400\r\n";
        if (line == "ATH0") return "\r\nOK\r\n";
        return "\r\nERROR\r\n";
    });
    HayesModem modem(serial, handle);
    EXPECT_EQ(modem.initialize().code, ModemCode::Ok);
    const auto dial = modem.dial("5551234");
    EXPECT_EQ(dial.code, ModemCode::Connect);
    EXPECT_EQ(dial.connectRate, 2400);
    EXPECT_TRUE(modem.online());
    EXPECT_EQ(modem.sendData("data over the wire"), 0);
    EXPECT_EQ(modem.command("AT+BOGUS").code, ModemCode::Error);
    EXPECT_EQ(modem.dial("12 34").code, ModemCode::Error);
    EXPECT_EQ(modem.hangup(std::chrono::milliseconds(20)).code, ModemCode::Ok);
    EXPECT_FALSE(modem.online());
    EXPECT_EQ(modem.sendData("x"), -ENOTCONN);
    const auto seen = peer.lines();
    ASSERT_GE(seen.size(), 3U);
    EXPECT_EQ(seen[0], "ATZ");
}

TEST_F(SerialTest, HayesModemTimesOutWhenSilent) {
    HayesModem modem(serial, handle);
    const auto r = modem.command("AT", std::chrono::milliseconds(80));
    EXPECT_EQ(r.code, ModemCode::Timeout);
}

TEST(HayesParse, RecognizesResultCodes) {
    EXPECT_EQ(HayesModem::parse("\r\nNO CARRIER\r\n").code, ModemCode::NoCarrier);
    EXPECT_EQ(HayesModem::parse("\r\nBUSY\r\n").code, ModemCode::Busy);
    EXPECT_EQ(HayesModem::parse("\r\nNO DIALTONE\r\n").code, ModemCode::NoDialtone);
    EXPECT_EQ(HayesModem::parse("\r\nCONNECT 14400/ARQ\r\n").connectRate, 14400);
    EXPECT_EQ(HayesModem::parse("garbage").code, ModemCode::Timeout);
    EXPECT_STREQ(toString(ModemCode::NoAnswer), "NO ANSWER");
}

namespace {

std::string marlinReply(const std::string& raw, std::atomic<int>& expectedLine, std::atomic<int>& resends) {
    std::string cmd = raw;
    int lineNo = -1;
    if (!cmd.empty() && cmd[0] == 'N') {
        const auto star = cmd.rfind('*');
        if (star == std::string::npos) return "Error:missing checksum\n";
        const int cs = std::stoi(cmd.substr(star + 1));
        if (cs != GcodePrinter::checksum(cmd.substr(0, star))) {
            ++resends;
            return "Error:checksum mismatch\nResend: " + std::to_string(expectedLine.load()) + "\nok\n";
        }
        const auto space = cmd.find(' ');
        lineNo = std::stoi(cmd.substr(1, space - 1));
        cmd = cmd.substr(space + 1, star - space - 1);
        if (cmd.rfind("M110", 0) == 0) {
            expectedLine = lineNo + 1;
            return "ok\n";
        }
        if (lineNo != expectedLine) {
            ++resends;
            return "Error:Line Number is not Last Line Number+1\nResend: " + std::to_string(expectedLine.load()) + "\n";
        }
        ++expectedLine;
    }
    if (cmd == "M105") return "ok T:201.5 /210.0 B:59.8 /60.0\n";
    if (cmd == "M114") return "X:10.00 Y:20.50 Z:0.30 E:1.25 Count X:800 Y:1640 Z:120\nok\n";
    if (cmd == "G28") return "ok\n";
    if (cmd == "M999") return "Error:Printer halted. kill() called!\n";
    return "ok\n";
}

}

TEST_F(SerialTest, GcodePrinterStreamsWithLineNumbersAndChecksums) {
    std::atomic<int> expected{1};
    std::atomic<int> resends{0};
    PtyPeer peer(pty, [&](const std::string& line) { return marlinReply(line, expected, resends); });
    GcodePrinter printer(serial, handle);
    ASSERT_EQ(printer.connect(std::chrono::milliseconds(10)), 0);
    EXPECT_EQ(printer.home(), 0);
    EXPECT_EQ(printer.setHotend(210), 0);
    EXPECT_EQ(printer.setBed(60), 0);
    Temperatures t;
    ASSERT_EQ(printer.readTemperatures(t), 0);
    EXPECT_DOUBLE_EQ(t.hotend, 201.5);
    EXPECT_DOUBLE_EQ(t.hotendTarget, 210.0);
    EXPECT_DOUBLE_EQ(t.bed, 59.8);
    EXPECT_DOUBLE_EQ(t.bedTarget, 60.0);
    Position p;
    ASSERT_EQ(printer.readPosition(p), 0);
    EXPECT_DOUBLE_EQ(p.x, 10.0);
    EXPECT_DOUBLE_EQ(p.y, 20.5);
    EXPECT_DOUBLE_EQ(p.e, 1.25);

    std::istringstream program("; header\nG1 X10 Y10 ; move\n\nG1 X20\nM104 S0\n");
    std::size_t lastSent = 0;
    EXPECT_EQ(printer.stream(program,
                             [&](std::size_t sent, std::size_t total) {
                                 lastSent = sent;
                                 EXPECT_EQ(total, 3U);
                             }),
              0);
    EXPECT_EQ(lastSent, 3U);
    EXPECT_EQ(resends.load(), 0);
    EXPECT_EQ(printer.setHotend(9000), -EINVAL);
    EXPECT_EQ(printer.send("G1\nX5"), -EINVAL);
}

TEST_F(SerialTest, GcodePrinterRecoversFromResendRequests) {
    std::atomic<int> expected{1};
    std::atomic<int> resends{0};
    std::atomic<bool> corruptedOnce{false};
    PtyPeer peer(pty, [&](const std::string& line) {
        if (!corruptedOnce.load() && line.find("G1 X1") != std::string::npos) {
            corruptedOnce = true;
            ++resends;
            return std::string("Resend: ") + std::to_string(expected.load()) + "\nok\n";
        }
        return marlinReply(line, expected, resends);
    });
    GcodePrinter printer(serial, handle);
    ASSERT_EQ(printer.connect(std::chrono::milliseconds(10)), 0);
    EXPECT_EQ(printer.send("G1 X1"), 0);
    EXPECT_EQ(resends.load(), 1);
    EXPECT_EQ(printer.send("G1 X2"), 0);
}

TEST_F(SerialTest, GcodePrinterSurfacesFirmwareErrorsAndTimeouts) {
    std::atomic<int> expected{1};
    std::atomic<int> resends{0};
    PtyPeer peer(pty, [&](const std::string& line) {
        if (line.find("M999") != std::string::npos) return std::string("Error:Printer halted. kill() called!\n");
        if (line.find("G4") != std::string::npos) return std::string();
        return marlinReply(line, expected, resends);
    });
    GcodeOptions opts;
    opts.commandTimeout = std::chrono::milliseconds(150);
    GcodePrinter printer(serial, handle, opts);
    ASSERT_EQ(printer.connect(std::chrono::milliseconds(10)), 0);
    EXPECT_EQ(printer.send("M999"), -EIO);
    EXPECT_EQ(printer.send("G4 P100"), -ETIMEDOUT);
}

TEST(GcodeParse, TemperaturesPositionsAndChecksums) {
    const auto t = GcodePrinter::parseTemperatures("ok T:25.3 /0.0 B:24.1 /0.0 @:0 B@:0");
    ASSERT_TRUE(t);
    EXPECT_DOUBLE_EQ(t->hotend, 25.3);
    EXPECT_DOUBLE_EQ(t->bed, 24.1);
    EXPECT_FALSE(GcodePrinter::parseTemperatures("nothing here"));
    EXPECT_FALSE(GcodePrinter::parsePosition("X:1 Y:2"));
    EXPECT_EQ(GcodePrinter::checksum("N1 M105"), 38);
    EXPECT_EQ(GcodePrinter::stripComment("  G1 X1 ; comment"), "G1 X1");
    EXPECT_EQ(GcodePrinter::stripComment("; only comment"), "");
}

namespace {

class TcpSink {
public:
    TcpSink() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) throw std::runtime_error("bind failed");
        socklen_t len = sizeof(addr);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        ::listen(fd_, 4);
        thread_ = std::thread([this] {
            while (true) {
                const int c = ::accept(fd_, nullptr, nullptr);
                if (c < 0) return;
                std::string data;
                char buf[1024];
                while (true) {
                    const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    data.append(buf, static_cast<std::size_t>(n));
                    if (!reply.empty()) {
                        ::send(c, reply.data(), reply.size(), MSG_NOSIGNAL);
                        break;
                    }
                }
                {
                    std::lock_guard lock(mutex);
                    received = data;
                    ++jobs;
                }
                ::close(c);
            }
        });
    }

    ~TcpSink() {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] std::uint16_t port() const { return port_; }
    std::string get() {
        std::lock_guard lock(mutex);
        return received;
    }

    std::string reply;
    std::mutex mutex;
    std::string received;
    std::atomic<int> jobs{0};

private:
    int fd_ = -1;
    std::uint16_t port_ = 0;
    std::thread thread_;
};

}

TEST(Printer, SendsRawJobsOverTcp) {
    TcpSink sink;
    PrinterTarget target;
    std::string err;
    ASSERT_TRUE(PrinterTarget::parse("tcp://127.0.0.1:" + std::to_string(sink.port()), target, err)) << err;
    RawPrinter printer(target);
    const std::string job(20000, 'P');
    EXPECT_EQ(printer.print(job), 0);
    ASSERT_TRUE(waitUntil([&] { return sink.jobs.load() == 1; }));
    EXPECT_EQ(sink.get(), job);
}

TEST(Printer, WrapsJobsInPjlAndSanitizesNames) {
    TcpSink sink;
    PrinterTarget target;
    std::string err;
    ASSERT_TRUE(PrinterTarget::parse("tcp://127.0.0.1:" + std::to_string(sink.port()), target, err));
    RawPrinter printer(target);
    EXPECT_EQ(printer.printPjl("my \"job\"\r\n@PJL INJECT", "BODY"), 0);
    ASSERT_TRUE(waitUntil([&] { return sink.jobs.load() == 1; }));
    const auto got = sink.get();
    EXPECT_NE(got.find("@PJL JOB NAME=\"my job@PJL INJECT\"\r\nBODY\r\n@PJL EOJ"), std::string::npos);
    EXPECT_EQ(got.find("\"\r\n@PJL INJECT"), std::string::npos);
}

TEST(Printer, QueryReadsTheStatusResponse) {
    TcpSink sink;
    sink.reply = "@PJL INFO STATUS\r\nCODE=10001\r\nDISPLAY=\"Ready\"\r\n\f";
    PrinterTarget target;
    std::string err;
    ASSERT_TRUE(PrinterTarget::parse("tcp://127.0.0.1:" + std::to_string(sink.port()), target, err));
    RawPrinter printer(target, std::chrono::milliseconds(2000));
    std::string response;
    ASSERT_EQ(printer.query("\x1b%-12345X@PJL INFO STATUS\r\n\x1b%-12345X", response), 0);
    EXPECT_NE(response.find("CODE=10001"), std::string::npos);
}

TEST(Printer, DeviceFilesAndTargetParsing) {
    const auto path = std::filesystem::temp_directory_path() / "gygax-printer-test.bin";
    std::filesystem::remove(path);
    { std::ofstream touch(path); }
    PrinterTarget target;
    std::string err;
    ASSERT_TRUE(PrinterTarget::parse(path.string(), target, err));
    EXPECT_EQ(RawPrinter(target).print("device data"), 0);
    std::ifstream in(path);
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(contents, "device data");
    std::filesystem::remove(path);

    EXPECT_FALSE(PrinterTarget::parse("lpt1", target, err));
    EXPECT_FALSE(PrinterTarget::parse("tcp://", target, err));
    EXPECT_FALSE(PrinterTarget::parse("tcp://h:0", target, err));
    ASSERT_TRUE(PrinterTarget::parse("tcp://printer.local", target, err));
    EXPECT_EQ(target.port, 9100);
    EXPECT_LT(RawPrinter(PrinterTarget{PrinterTarget::Kind::Tcp, "127.0.0.1", 1, ""}, std::chrono::milliseconds(300)).print("x"), 0);
}

namespace {

class FakeSysfs {
public:
    FakeSysfs() : root(std::filesystem::temp_directory_path() / ("gygax-gpio-" + std::to_string(::getpid()))) {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        std::ofstream exportFile(root / "export");
        std::ofstream unexportFile(root / "unexport");
    }

    ~FakeSysfs() { std::filesystem::remove_all(root); }

    void addPin(unsigned pin, char value = '0') const {
        const auto dir = root / ("gpio" + std::to_string(pin));
        std::filesystem::create_directories(dir);
        std::ofstream(dir / "direction") << "in";
        std::ofstream(dir / "value") << value;
    }

    void setValue(unsigned pin, char value) const { std::ofstream(root / ("gpio" + std::to_string(pin)) / "value") << value; }

    [[nodiscard]] std::string readFile(unsigned pin, const char* file) const {
        std::ifstream in(root / ("gpio" + std::to_string(pin)) / file);
        std::string s;
        in >> s;
        return s;
    }

    std::filesystem::path root;
};

}

TEST(Gpio, ConfiguresReadsAndWritesPins) {
    FakeSysfs fs;
    fs.addPin(17, '1');
    fs.addPin(18);
    SysfsGpioPort port(fs.root.string());
    ASSERT_EQ(port.initialize(), 0);
    ASSERT_EQ(port.configure(17, GpioMode::Input), 0);
    ASSERT_EQ(port.configure(18, GpioMode::Output), 0);
    EXPECT_EQ(fs.readFile(18, "direction"), "out");
    GpioLevel level{};
    ASSERT_EQ(port.read(17, level), 0);
    EXPECT_EQ(level, GpioLevel::High);
    EXPECT_EQ(port.write(17, GpioLevel::High), -EPERM);
    ASSERT_EQ(port.write(18, GpioLevel::High), 0);
    EXPECT_EQ(fs.readFile(18, "value"), "1");
    ASSERT_EQ(port.read(18, level), 0);
    EXPECT_EQ(level, GpioLevel::High);
    EXPECT_EQ(port.read(99, level), -EINVAL);
    EXPECT_EQ(port.write(99, GpioLevel::Low), -EINVAL);
    EXPECT_EQ(port.mode(18), GpioMode::Output);
}

TEST(Gpio, WatchDeliversEdgesWithTimestamps) {
    FakeSysfs fs;
    fs.addPin(4);
    SysfsGpioPort port(fs.root.string(), std::chrono::milliseconds(2));
    ASSERT_EQ(port.initialize(), 0);
    ASSERT_EQ(port.configure(4, GpioMode::Input), 0);
    std::mutex m;
    std::vector<std::pair<GpioLevel, std::uint64_t>> edges;
    ASSERT_EQ(port.watch(4,
                         [&](std::uint32_t pin, GpioLevel level, std::uint64_t ts) {
                             EXPECT_EQ(pin, 4U);
                             std::lock_guard lock(m);
                             edges.emplace_back(level, ts);
                         }),
              0);
    fs.setValue(4, '1');
    ASSERT_TRUE(waitUntil([&] {
        std::lock_guard lock(m);
        return edges.size() == 1;
    }));
    fs.setValue(4, '0');
    ASSERT_TRUE(waitUntil([&] {
        std::lock_guard lock(m);
        return edges.size() == 2;
    }));
    std::lock_guard lock(m);
    EXPECT_EQ(edges[0].first, GpioLevel::High);
    EXPECT_EQ(edges[1].first, GpioLevel::Low);
    EXPECT_GT(edges[1].second, edges[0].second);
    EXPECT_EQ(port.unwatch(4), 0);
}

TEST(Gpio, ExportsMissingPinsAndReportsUnavailableOnes) {
    FakeSysfs fs;
    SysfsGpioPort port(fs.root.string());
    ASSERT_EQ(port.initialize(), 0);
    EXPECT_EQ(port.configure(7, GpioMode::Input), -ENODEV);
    EXPECT_EQ(fs.readFile(0, "x"), "");
    SysfsGpioPort missing("/nonexistent/gygax/gpio");
    EXPECT_EQ(missing.initialize(), -ENOENT);
    SysfsGpioPort notStarted(fs.root.string());
    EXPECT_EQ(notStarted.configure(1, GpioMode::Input), -EINVAL);
}
