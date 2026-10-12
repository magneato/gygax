#include <glyder/rxserver.hpp>

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <glyder/crypto.hpp>
#include <glyder/net.hpp>

namespace glyder {

namespace {

const char* kPage = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>glyder vision</title>
<style>
:root{--ink:#0e1116;--panel:#161a21;--line:#262c35;--text:#e6e1d3;--dim:#8a929c;--accent:#2ee6d6;--warn:#ffb000}
*{box-sizing:border-box}body{margin:0;background:var(--ink);color:var(--text);font:16px/1.45 ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:560px;margin:0 auto;padding:28px 16px 48px}
h1{font:600 13px/1 ui-monospace,SFMono-Regular,Menlo,monospace;letter-spacing:.32em;color:var(--accent);margin:0 0 10px}
h2{font-size:26px;font-weight:600;margin:0 0 6px}p{color:var(--dim);margin:0 0 18px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:16px;margin:0 0 14px}
.grid{display:grid;grid-template-columns:52px repeat(5,1fr);gap:8px;align-items:center}
.grid b{font:600 12px ui-monospace,monospace;color:var(--dim);letter-spacing:.08em}
input,select,textarea{width:100%;background:#0b0e12;color:var(--text);border:1px solid var(--line);border-radius:9px;padding:9px 8px;font:15px ui-monospace,monospace}
input:focus,select:focus,textarea:focus{outline:2px solid var(--accent);border-color:transparent}
input::placeholder,textarea::placeholder{color:#3c444e}
.row{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px}label{display:block;font:600 12px ui-monospace,monospace;color:var(--dim);margin:0 0 6px;letter-spacing:.08em}
button{width:100%;padding:14px;border:0;border-radius:11px;background:var(--accent);color:#04201d;font:700 15px ui-sans-serif,system-ui;letter-spacing:.04em}
.note{font-size:13px;color:var(--dim)}.msg{border-left:3px solid var(--accent);padding:10px 12px;background:#0f1a1b;border-radius:8px;margin:0 0 14px}
.msg.bad{border-color:var(--warn);background:#1d1709}details summary{cursor:pointer;color:var(--dim);font-size:14px}
@media (max-width:420px){.grid{grid-template-columns:40px repeat(5,1fr);gap:5px}input,select{padding:8px 4px;font-size:14px}}
</style></head><body><main>
<h1>GLYDER &middot; VISION</h1>
<h2>Your prescription</h2>
<p>Typed here, kept on your headset. glyder places its cameras at your PD and turns each eye's view for prism exactly, and pre-sharpens for mild sphere and cylinder.</p>
%MESSAGE%
<form method="post" action="/rx">
<div class="card"><div class="grid">
<span></span><b>SPH</b><b>CYL</b><b>AXIS</b><b>PRISM</b><b>BASE</b>
<b>OD</b><input name="od_sph" inputmode="decimal" placeholder="-1.25"><input name="od_cyl" inputmode="decimal" placeholder="-0.50"><input name="od_ax" inputmode="numeric" placeholder="180"><input name="od_pr" inputmode="decimal" placeholder="0"><select name="od_base"><option>BI</option><option>BO</option><option>BU</option><option>BD</option></select>
<b>OS</b><input name="os_sph" inputmode="decimal" placeholder="-1.00"><input name="os_cyl" inputmode="decimal" placeholder="-0.75"><input name="os_ax" inputmode="numeric" placeholder="90"><input name="os_pr" inputmode="decimal" placeholder="0"><select name="os_base"><option>BI</option><option>BO</option><option>BU</option><option>BD</option></select>
</div></div>
<div class="card row"><div><label>PD (mm)</label><input name="pd" inputmode="decimal" placeholder="63"></div><div><label>ADD</label><input name="add" inputmode="decimal" placeholder="0"></div><div><label>AGE</label><input name="age" inputmode="numeric" placeholder=""></div></div>
<div class="card"><details><summary>Or paste it as written</summary><p class="note" style="margin-top:10px">One eye per line, e.g. <code>OD -1.25 -0.50 x 180 1.0 BI</code>, <code>OS -1.00 -0.75 x 90</code>, <code>PD 63</code>.</p><textarea name="text" rows="4"></textarea></details></div>
<div class="card"><label>PIN SHOWN IN THE HEADSET</label><input name="pin" inputmode="numeric" autocomplete="one-time-code" required></div>
<button type="submit">SEND TO HEADSET</button>
<p class="note" style="margin-top:14px">Software cannot refocus light: for more than about 1.5 dioptres of blur, glyder will ask you to wear your glasses with the headset's spacer or fit lens inserts. It is not a medical device.</p>
</form></main></body></html>
)HTML";

std::string fromForm(const std::string& body) {
    if (const std::string text = formValue(body, "text"); !text.empty()) return text;
    std::string out;
    for (const char* eye : {"od", "os"}) {
        const std::string e(eye);
        const std::string sph = formValue(body, e + "_sph"), cyl = formValue(body, e + "_cyl"), ax = formValue(body, e + "_ax"),
                          pr = formValue(body, e + "_pr"), base = formValue(body, e + "_base");
        if (sph.empty() && cyl.empty() && pr.empty()) continue;
        out += e == "od" ? "OD SPH " : "OS SPH ";
        out += sph.empty() ? std::string("0") : sph;
        if (!cyl.empty()) {
            out += " CYL ";
            out += cyl;
            out += " AXIS ";
            out += ax.empty() ? std::string("180") : ax;
        }
        if (!pr.empty() && std::strtof(pr.c_str(), nullptr) != 0.0f) {
            out += " PRISM ";
            out += pr;
            out += ' ';
            out += base;
        }
        out += "\n";
    }
    for (const char* k : {"pd", "add", "age"})
        if (const std::string v = formValue(body, k); !v.empty()) out += std::string(k) + " " + v + "\n";
    return out;
}

void reply(int fd, int status, const std::string& body) {
    const char* reason = status == 200 ? "OK" : status == 403 ? "Forbidden" : status == 429 ? "Too Many Requests" : "Bad Request";
    const std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                             "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nX-Frame-Options: DENY\r\n"
                             "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; form-action 'self'\r\n"
                             "Connection: close\r\nContent-Length: " +
                             std::to_string(body.size()) + "\r\n\r\n";
    const std::string all = head + body;
    std::size_t off = 0;
    while (off < all.size()) {
        const ssize_t n = ::send(fd, all.data() + off, all.size() - off, MSG_NOSIGNAL);
        if (n <= 0) break;
        off += static_cast<std::size_t>(n);
    }
}

std::string page(const std::string& message, bool ok) {
    std::string p = kPage;
    const std::string m =
        message.empty() ? "" : "<div class=\"msg" + std::string(ok ? "" : " bad") + "\">" + htmlEscape(message) + "</div>";
    p.replace(p.find("%MESSAGE%"), 9, m);
    return p;
}

} // namespace

std::string htmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        case '\'': o += "&#39;"; break;
        default: o += c;
        }
    }
    return o;
}

std::string formValue(const std::string& body, const std::string& key) {
    std::size_t pos = 0;
    while (pos <= body.size()) {
        const std::size_t amp = body.find('&', pos);
        const std::string pair = body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const std::size_t eq = pair.find('=');
        if (pair.substr(0, eq) == key && eq != std::string::npos) {
            std::string v;
            const std::string raw = pair.substr(eq + 1);
            for (std::size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '+')
                    v += ' ';
                else if (raw[i] == '%' && i + 2 < raw.size()) {
                    v += static_cast<char>(std::strtol(raw.substr(i + 1, 2).c_str(), nullptr, 16));
                    i += 2;
                } else
                    v += raw[i];
            }
            return v;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return {};
}

RxServer::~RxServer() {
    stop();
}

bool RxServer::start(std::uint16_t port, Submit submit, std::string* error) {
    stop();
    submit_ = std::move(submit);
    listen_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    const int one = 1;
    ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(listen_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 || ::listen(listen_, 8) != 0) {
        if (error) *error = std::string("vision page: ") + std::strerror(errno);
        ::close(listen_);
        listen_ = -1;
        return false;
    }
    socklen_t len = sizeof sa;
    ::getsockname(listen_, reinterpret_cast<sockaddr*>(&sa), &len);
    port_ = ntohs(sa.sin_port);
    std::array<std::uint8_t, 4> r{};
    crypto::randomBytes(r);
    pin_ = std::to_string(1000 + (r[0] | r[1] << 8) % 9000);
    failures_ = 0;
    running_ = true;
    thread_ = std::jthread([this] {
        while (running_) {
            pollfd p{listen_, POLLIN, 0};
            if (::poll(&p, 1, 100) <= 0) continue;
            const int fd = ::accept4(listen_, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd >= 0) {
                serve(fd);
                ::close(fd);
            }
        }
    });
    return true;
}

void RxServer::stop() {
    if (!running_) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
    ::close(listen_);
    listen_ = -1;
}

std::string RxServer::url() const {
    const auto addrs = net::localAddresses();
    std::string host = addrs.empty() ? "headset" : addrs.front().str();
    if (const auto colon = host.rfind(':'); colon != std::string::npos) host.resize(colon);
    return "http://" + host + ":" + std::to_string(port_);
}

void RxServer::serve(int fd) {
    timeval tv{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string req;
    char buf[4096];
    std::size_t headerEnd = std::string::npos, contentLength = 0;
    bool headersParsed = false;
    while (req.size() < 16384) {
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        req.append(buf, static_cast<std::size_t>(n));
        if (headerEnd == std::string::npos) headerEnd = req.find("\r\n\r\n");
        if (headerEnd != std::string::npos && !headersParsed) {
            headersParsed = true;
            const std::string head = req.substr(0, headerEnd);
            std::string lower = head;
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (const auto cl = lower.find("content-length:"); cl != std::string::npos)
                contentLength = std::strtoul(head.c_str() + cl + 15, nullptr, 10);
            if (contentLength > 8192) {
                reply(fd, 400, page("That is too long for a prescription.", false));
                return;
            }
        }
        if (headerEnd != std::string::npos && req.size() >= headerEnd + 4 + contentLength) break;
    }
    if (headerEnd == std::string::npos) return;
    const std::string line = req.substr(0, req.find("\r\n"));
    if (line.rfind("GET / ", 0) == 0 || line.rfind("GET /rx ", 0) == 0) {
        reply(fd, 200, page("", true));
        return;
    }
    if (line.rfind("POST /rx ", 0) != 0) {
        reply(fd, 400, page("Open the address shown in the headset.", false));
        return;
    }
    const std::string body = req.substr(headerEnd + 4, contentLength);
    std::lock_guard lock(mutex_);
    const double t = net::now();
    if (t - failuresSince_ > 60) {
        failures_ = 0;
        failuresSince_ = t;
    }
    if (failures_ >= 5) {
        reply(fd, 429, page("Too many wrong PINs. Wait a minute and try again.", false));
        return;
    }
    if (formValue(body, "pin") != pin_) {
        ++failures_;
        reply(fd, 403, page("That PIN does not match the one in the headset.", false));
        return;
    }
    bool ok = false;
    const std::string msg = submit_ ? submit_(fromForm(body), ok) : "not accepting prescriptions";
    reply(fd, ok ? 200 : 400, page(msg, ok));
}

} // namespace glyder
