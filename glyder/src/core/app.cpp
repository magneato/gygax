#include <glyder/app.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>

#include <glyder/demo.hpp>
#include <glyder/rxserver.hpp>
#include <glyder/version.hpp>

namespace glyder {

namespace {

using render::Align;
namespace pal = kit::palette;

constexpr Rgb8 kInk = hex(0x0E1116), kPaper = hex(0xF4F4F1), kGraphite = hex(0x2B2F36), kDim = hex(0x8A929C), kCyan = hex(0x2EE6D6),
               kAmber = hex(0xFFB000), kRed = hex(0xFF5A36), kGreen = hex(0x3DFF7A), kGold = hex(0xFFD54A);
constexpr std::uint16_t kRxPort = 1998;

float F(int v) {
    return static_cast<float>(v);
}
float Fd(double v) {
    return static_cast<float>(v);
}

Quat yawOnly(Quat q) {
    // Keep the heading of an attitude, drop its pitch and roll (for the render frame, y up).
    const Vec3 f = rotate(q, {0, 0, -1});
    const float yaw = std::atan2(-f.x, -f.z);
    return axisAngle({0, 1, 0}, yaw);
}

float yawOf(Quat q) {
    const Vec3 f = rotate(q, {0, 0, -1});
    return std::atan2(-f.x, -f.z);
}

// ENU attitude to heading (radians clockwise from north).
float headingOf(Quat q) {
    const Vec3 f = rotate(q, {1, 0, 0});
    return std::atan2(f.x, f.y);
}

Quat enuYawOnly(Quat q) {
    return attitude(headingOf(q));
}

std::optional<float> raySphere(Vec3 o, Vec3 d, Vec3 c, float r) {
    const Vec3 m = o - c;
    const float b = dot(m, d), cc = dot(m, m) - r * r;
    if (cc > 0 && b > 0) return std::nullopt;
    const float disc = b * b - cc;
    if (disc < 0) return std::nullopt;
    const float t = -b - std::sqrt(disc);
    return std::max(t, 0.0f);
}

std::optional<float> rayPlane(Vec3 o, Vec3 d, Vec3 p, Vec3 n) {
    const float den = dot(d, n);
    if (std::fabs(den) < 1e-6f) return std::nullopt;
    const float t = dot(p - o, n) / den;
    return t > 0 ? std::optional(t) : std::nullopt;
}

Rgb8 toneColour(wire::Tone t) {
    switch (t) {
    case wire::Tone::Good: return kGreen;
    case wire::Tone::Warn: return kAmber;
    case wire::Tone::Alert: return kRed;
    default: return kPaper;
    }
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::uint32_t hash32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

struct ShapeInfo {
    Vec3 centre;
    float radius{1};
};
const std::vector<ShapeInfo>& shapeInfo() {
    static const std::vector<ShapeInfo> info = [] {
        std::vector<ShapeInfo> v;
        for (kit::Shape s : kit::allShapes()) {
            const kit::Mesh m = kit::build(s);
            v.push_back({m.bounds.centre(), std::max(m.bounds.radius(), 0.05f)});
        }
        return v;
    }();
    return info;
}
ShapeInfo infoOf(std::uint16_t shape) {
    const auto& v = shapeInfo();
    return shape < v.size() ? v[shape] : ShapeInfo{};
}

// The camera basis in the model (FLU) frame: camera x right = -y, camera y up = +z, camera -z = +x.
Mat4 cameraInModel() {
    Mat4 m;
    m.m[0][0] = 0, m.m[0][1] = -1, m.m[0][2] = 0;
    m.m[1][0] = 0, m.m[1][1] = 0, m.m[1][2] = 1;
    m.m[2][0] = -1, m.m[2][1] = 0, m.m[2][2] = 0;
    return m;
}

} // namespace

App::App(AppOptions options) : options_(std::move(options)) {
    client_ = std::make_unique<Client>(options_.clientName);
    std::string error;
    if (!client_->open(&error)) toast("NETWORK: " + upper(error), 8);
    loadState();
    applyRx();
    std::vector<net::Address> also;
    for (const auto& h : options_.hosts)
        if (auto a = net::Address::parse(h)) also.push_back(*a);
    client_->discover(true, also);
    if (options_.demo) {
        demo_ = std::make_unique<DemoWorld>();
        if (demo_->start(&error)) connectTo({0x7F000001u, demo_->port()}, "", "THE CONSTRUCT: CALM");
    } else if (!options_.connect.empty()) {
        if (auto a = net::Address::parse(options_.connect)) connectTo(*a, options_.secret, options_.connect);
    }
}

App::~App() {
    if (rxServer_) rxServer_->stop();
    client_.reset();
    if (demo_) demo_->stop();
}

std::string App::mode() const {
    switch (mode_) {
    case Mode::Lobby: return "lobby";
    case Mode::Keypad: return "keypad";
    case Mode::Connecting: return "connecting";
    case Mode::Overview: return "overview";
    case Mode::Ride: return ride_.control ? "control" : "riding";
    }
    return "";
}

std::string App::caption() const {
    std::string s = std::string("glyder ") + kVersion + " \"" + kReleaseName + "\"";
    if (world_) s += "  |  " + world_->title;
    s += "  |  " + mode();
    return s;
}

// ---------------------------------------------------------------------------------------------
// State

void App::loadState() {
    if (options_.storageDir.empty()) return;
    std::ifstream rx(options_.storageDir + "/prescription.txt");
    if (rx) {
        std::stringstream ss;
        ss << rx.rdbuf();
        std::string text = ss.str();
        // Settings lines live with the prescription: "#glyder strength 0.7 on".
        if (const auto pos = text.find("#glyder "); pos != std::string::npos) {
            std::istringstream st(text.substr(pos + 8));
            std::string key, onoff;
            float strength = 0.7f;
            st >> key >> strength >> onoff;
            rxStrength_ = clamp(strength, 0.0f, 1.0f);
            rxEnabled_ = onoff != "off";
        }
        if (auto p = rx::parse(text)) rx_ = *p;
    }
    std::ifstream hosts(options_.storageDir + "/hosts.txt");
    std::string addr, code;
    while (hosts >> addr >> code) paired_[addr] = code;
    // Accessibility: "colour tritan", "cues on", "blind off".
    if (!options_.rememberAccess) return;
    std::ifstream access(options_.storageDir + "/access.txt");
    std::string key, value;
    while (access >> key >> value) {
        if (key == "colour") colour_ = colour::fromToken(value);
        if (key == "cues") cues_ = value == "on";
        if (key == "blind") blind_ = value == "on";
        if (key == "base") base_ = value == "on";
        if (key == "ar") ar_ = value == "on";
    }
}

void App::saveState() const {
    if (options_.storageDir.empty()) return;
    std::ofstream rx(options_.storageDir + "/prescription.txt");
    rx << rx::format(rx_) << "#glyder strength " << rxStrength_ << (rxEnabled_ ? " on" : " off") << "\n";
    std::ofstream hosts(options_.storageDir + "/hosts.txt");
    for (const auto& [a, c] : paired_) hosts << a << " " << c << "\n";
    if (!options_.rememberAccess) return;
    std::ofstream access(options_.storageDir + "/access.txt");
    access << "colour " << colour::token(colour_) << "\ncues " << (cues_ ? "on" : "off") << "\nblind " << (blind_ ? "on" : "off")
           << "\nbase " << (base_ ? "on" : "off") << "\nar " << (ar_ ? "on" : "off") << "\n";
}

void App::applyRx() {
    rx::Display d;
    correction_ = rx::compute(rx_, d, rxStrength_);
    if (!rxEnabled_) {
        // Off: keep the real PD (it is not a correction) and drop the rest.
        const float ipd = correction_.ipd;
        correction_ = {};
        correction_.ipd = ipd;
    }
}

void App::toast(std::string text, double seconds) {
    toasts_.push_back({std::move(text), (startTime_ < 0 ? 0.0 : net::now()) + seconds});
    while (toasts_.size() > 3) toasts_.pop_front();
}

void App::connectTo(const net::Address& a, const std::string& secret, const std::string& labelText) {
    connectingTo_ = labelText;
    pendingHost_ = a;
    client_->connect(a, secret);
    mode_ = Mode::Connecting;
    world_.reset();
    haveTerrain_ = false;
    terrain_.reset();
    trails_.clear();
    events_.clear();
    oliphauntCards_.clear();
}

// ---------------------------------------------------------------------------------------------
// Input helpers

Pose App::pointer(const Input& in, int hand) const {
    if (hand == 1 && in.pointer) return *in.pointer;
    return in.hands[hand].aim;
}

bool App::pressed(const Input& in, int hand) const {
    const bool down = in.hands[hand].trigger > 0.6f;
    return down && !triggerWas_[hand];
}

void App::label(const std::string& text, Vec3 origin, Vec3 right, Vec3 up, float height, Rgb8 colour, Align align, std::uint8_t alpha) {
    render::text(solidOut(), upper(text), origin, right, up, height, colour, align, 0.11f, alpha);
}

void App::card(Vec3 c, Vec3 right, Vec3 up, Vec2 half, Rgb8 fill, std::uint8_t alpha, Rgb8 border, float glowWidth) {
    render::PanelStyle st;
    st.radius = std::min(half.x, half.y) * 0.18f;
    st.fill = fill;
    st.fillAlpha = alpha;
    st.border = 0.0035f;
    st.borderColour = border;
    st.borderAlpha = 255;
    st.glow = glowWidth;
    render::addPanel(panelsOut(), c, right, up, half, st);
}

bool App::button(const Input& in, const std::string& id, Vec3 c, Vec3 right, Vec3 up, Vec2 half, const std::string& text, bool active,
                 bool enabled) {
    const Vec3 n = normalize(cross(right, up));
    bool hit = false;
    for (int h = 0; h < 2; ++h) {
        const bool desktopPointer = h == 1 && in.pointer.has_value();
        if (!in.hands[h].active && !desktopPointer) continue;
        const Pose p = pointer(in, h);
        if (auto t = rayPlane(p.pos, p.forward(), c, n)) {
            const Vec3 q = p.pos + p.forward() * *t - c;
            if (std::fabs(dot(q, right)) <= half.x && std::fabs(dot(q, up)) <= half.y) {
                hit = true;
                hot_ = id;
            }
        }
    }
    const bool isHot = hit && enabled;
    const Rgb8 fill = active ? hex(0x10302E) : (isHot ? hex(0x1B2129) : kInk);
    const Rgb8 border = !enabled ? hex(0x2A3038) : (isHot || active ? kCyan : hex(0x39414B));
    card(c, right, up, half, fill, 235, border, isHot ? 0.004f : 0.0f);
    // The label fits the button: as tall as it can be without running over the edges.
    const float th = std::min({half.y * 0.55f, 0.022f, half.x * 1.7f / std::max(render::textWidth(upper(text), 1.0f), 1e-3f)});
    label(text, c - up * (th * 0.5f) + n * 0.001f, right, up, th, enabled ? (active ? kCyan : kPaper) : kDim, Align::Centre);
    if (!enabled) return false;
    if (isHot && hotWas_ != id) haptics_[1] = {0.15f, 0.01f};
    const bool click = clicked_ == id;
    if (click) haptics_[1] = {0.5f, 0.03f};
    return click;
}

// ---------------------------------------------------------------------------------------------
// Frame

render::FrameData App::frame(const Input& in, std::span<const EyeView> eyes, std::uint32_t width, std::uint32_t height) {
    if (startTime_ < 0) startTime_ = in.time;
    haveHeard_ = false;
    items_.clear();
    panels_.clear();
    solid_.clear();
    glow_.clear();
    uiPanels_.clear();
    uiSolid_.clear();
    uiGlow_.clear();
    haptics_[0] = haptics_[1] = {};
    // A click lands on whatever was hot when the trigger went down.
    clicked_.clear();
    for (int h = 0; h < 2; ++h)
        if (pressed(in, h) && !hot_.empty()) clicked_ = hot_;
    hotWas_ = hot_;
    hot_.clear();

    poll(in);

    // The menu: Y on the left controller (Tab on a desktop).
    const bool menuDown = in.hands[0].secondary;
    if (menuDown && !menuWas_ && (mode_ == Mode::Overview || mode_ == Mode::Ride || mode_ == Mode::Lobby))
        overlay_ = overlay_ == Overlay::Menu ? Overlay::None : Overlay::Menu;
    menuWas_ = menuDown;

    switch (mode_) {
    case Mode::Lobby: lobby(in); break;
    case Mode::Keypad: keypad(in); break;
    case Mode::Connecting: lobby(in); break;
    case Mode::Overview: overview(in); break;
    case Mode::Ride: ride(in); break;
    }
    if (overlay_ == Overlay::Menu) menu(in);
    if (overlay_ == Overlay::Vision) visionPanel(in);
    if (ar_ && !base_ && world_) oliphauntPanel(in);
    hud(in);
    controllers(in);
    rain(in);
    listen(in);

    for (int h = 0; h < 2; ++h) {
        triggerWas_[h] = in.hands[h].trigger > 0.6f;
        primaryWas_[h] = in.hands[h].primary;
        secondaryWas_[h] = in.hands[h].secondary;
    }

    // Long lines that pass near the eye are split, so the shader's minimum on-screen width (which it
    // works out at each end of a line) follows the real distance along it.
    subdivide(solid_, in.head.pos);
    subdivide(glow_, in.head.pos);
    subdivide(uiSolid_, in.head.pos);
    subdivide(uiGlow_, in.head.pos);

    // Scene constants for the eyes, with the pilot's PD and prism applied.
    render::FrameData f;
    scene_.groundColor.w = Fd(in.time - startTime_);
    const std::size_t n = std::min<std::size_t>(eyes.size(), 2);
    Vec3 centre;
    for (std::size_t i = 0; i < n; ++i) centre += eyes[i].pose.pos;
    centre = centre / F(static_cast<int>(std::max<std::size_t>(n, 1)));
    for (std::size_t i = 0; i < n; ++i) {
        EyeView e = eyes[i];
        if (n == 2 && correction_.ipd > 0.04f) {
            const Vec3 off = eyes[i].pose.pos - centre;
            const float half = length(off);
            if (half > 1e-4f) e.pose.pos = centre + off * (correction_.ipd * 0.5f / half);
        }
        const rx::EyeCorrection& ec = correction_.eye[n == 2 ? i : 1];
        e.pose.q = e.pose.q * axisAngle({0, 1, 0}, ec.yaw) * axisAngle({1, 0, 0}, ec.pitch);
        const Mat4 view = viewFromPose(e.pose.pos, e.pose.q);
        const Mat4 proj = projection(e.tanLeft, e.tanRight, e.tanUp, e.tanDown, options_.nearZ);
        scene_.views[i] = render::viewData(view, proj);
    }
    if (n == 1) scene_.views[1] = scene_.views[0];
    // Vision pre-compensation, in this eye buffer's pixels.
    if (visionFilter() && width > 0 && height > 0 && n > 0) {
        for (std::size_t i = 0; i < 2; ++i) {
            const EyeView& e = eyes[std::min(i, n - 1)];
            const rx::EyeCorrection& ec = correction_.eye[n == 2 ? i : 1];
            const float tanW = e.tanRight - e.tanLeft, tanH = e.tanUp - e.tanDown;
            const float pxPerRad = static_cast<float>(width) / std::max(tanW, 1e-3f);
            const float sMajor = ec.blurMajor * pxPerRad / 4.0f, sMinor = ec.blurMinor * pxPerRad / 4.0f;
            render::PostEye& pe = post_.eye[i];
            pe.axes = {std::cos(ec.axis), std::sin(ec.axis), sMajor, sMinor};
            pe.gains = {rx::gainFor(sMajor, rxStrength_), rx::gainFor(sMinor, rxStrength_), std::tan(28.0f * kDeg), 1.0f};
            pe.frustum = {e.tanLeft, tanW, e.tanUp, tanH};
            pe.texel = {1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height), 0, 0};
        }
        f.post = &post_;
    } else {
        for (auto& pe : post_.eye) pe.gains.w = 0;
    }
    // Colour vision: a matrix in the final pass, over everything.
    const colour::Mat3 cm = colour::display(colour_, colourCorrect_, colourPreview_);
    const bool colourOn = colour_ != colour::Vision::Typical && (colourCorrect_ || colourPreview_);
    for (std::size_t row = 0; row < 3; ++row) {
        const std::size_t k = row * 3;
        post_.colour[row] = {cm.m[k], cm.m[k + 1], cm.m[k + 2], row == 0 && colourOn ? 1.0f : 0.0f};
    }
    if (colourOn) f.post = &post_;
    if (ar_ && !base_ && in.passthroughAvailable) {
        f.drawSky = false;
        f.clear = {0, 0, 0, 0};
    }
    f.scene = scene_;
    f.items = items_;
    f.panels = panels_;
    f.solid = solid_;
    f.glow = glow_;
    f.uiPanels = uiPanels_;
    f.uiSolid = uiSolid_;
    f.uiGlow = uiGlow_;
    return f;
}

void App::subdivide(std::vector<render::Line>& lines, Vec3 eye) {
    const std::size_t n = lines.size();
    for (std::size_t i = 0; i < n; ++i) {
        const render::Line l = lines[i];
        const Vec3 d = l.b - l.a;
        const float len = length(d);
        if (len < 0.5f) continue;
        const float t = clamp(dot(eye - l.a, d) / (len * len), 0.0f, 1.0f);
        const float near = length(l.a + d * t - eye);
        if (len < near * 0.6f + 0.5f) continue;
        const int pieces = std::min(64, static_cast<int>(std::ceil(len / std::max(near * 0.5f, 0.5f))));
        lines[i].b = l.a + d * (1.0f / F(pieces));
        for (int k = 1; k < pieces; ++k) {
            render::Line part = l;
            part.a = l.a + d * (F(k) / F(pieces));
            part.b = l.a + d * (F(k + 1) / F(pieces));
            lines.push_back(part);
        }
    }
}

std::optional<kit::Mesh> App::takeTerrainMesh() {
    return std::exchange(terrainMesh_, std::nullopt);
}

void App::poll(const Input& in) {
    const auto st = client_->state();
    if (mode_ == Mode::Connecting) {
        if (st == Client::State::Connected && client_->world()) {
            world_ = client_->world();
            mode_ = Mode::Overview;
            focus_ = {0, 0, world_->groundZ};
            tableScale_ = (in.desktop ? 0.75f : 1.1f) / std::max(world_->radius, 1.0f);
            tableYaw_ = 0;
            // Put the table in front of where the pilot is looking.
            const float yaw = yawOf(in.head.q);
            const Vec3 fwd = rotate(axisAngle({0, 1, 0}, yaw), {0, 0, -1});
            tableCentre_ = Vec3{in.head.pos.x, std::max(in.head.pos.y - 0.55f, 0.6f), in.head.pos.z} + fwd * 1.0f;
            if (world_->environment == static_cast<std::uint8_t>(render::Environment::Construct))
                tableCentre_.y = std::max(in.head.pos.y - 0.7f, 0.4f);
            if (in.desktop) tableCentre_ = in.head.pos + fwd * 1.15f - Vec3{0, 0.55f, 0};
            worldRevision_ = client_->worldRevision();
            toast(world_->title.empty() ? "CONNECTED" : world_->title, 3);
            const auto key = pendingHost_.str();
            if (!code_.empty()) paired_[key] = code_;
            saveState();
        } else if (st == Client::State::Failed) {
            const std::string err = client_->error();
            if (err.find("pairing") != std::string::npos) {
                mode_ = Mode::Keypad;
                toast(code_.empty() ? "ENTER THE PAIRING CODE SHOWN ON THE HOST" : "THAT CODE DID NOT MATCH", 4);
                code_.clear();
            } else {
                mode_ = Mode::Lobby;
                toast(upper(err), 5);
                oliphauntCards_.clear();
            }
        }
    } else if ((mode_ == Mode::Overview || mode_ == Mode::Ride) && st == Client::State::Failed) {
        toast(upper(client_->error()), 5);
        if (mode_ == Mode::Ride) leaveRide("");
        mode_ = Mode::Lobby;
        world_.reset();
        oliphauntCards_.clear();
    }
    // The host switched scenes: take in the new world where we stand.
    if ((mode_ == Mode::Overview || mode_ == Mode::Ride) && client_->worldRevision() != worldRevision_) {
        worldRevision_ = client_->worldRevision();
        if (auto w = client_->world()) {
            if (mode_ == Mode::Ride) leaveRide("");
            world_ = std::move(w);
            terrain_.reset();
            haveTerrain_ = false;
            focus_ = {0, 0, world_->groundZ};
            tableScale_ = (in.desktop ? 0.75f : 1.1f) / std::max(world_->radius, 1.0f);
            toast(world_->title.empty() ? "SCENE CHANGED" : world_->title, 3);
        }
    }
    if (auto t = client_->takeTerrain()) {
        terrain_ = std::move(*t);
        const kit::HeightSamples hs{terrain_->w, terrain_->h, terrain_->cell, terrain_->x0, terrain_->y0, terrain_->z.data()};
        const int step = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(terrain_->w) * terrain_->h / 40000.0))));
        terrainMesh_ = kit::terrain(hs, step, world_ ? world_->radius * 1.7f : 0.0f);
        haveTerrain_ = true;
    }
    for (auto& e : client_->takeEvents()) {
        if (blind_) {
            // Each kind of event has its own interval, so it is recognisable without reading it.
            switch (e.kind) {
            case wire::EventKind::Opfor: earcon(392, 349.23f, 0.35f); break;
            case wire::EventKind::Outcome: earcon(659.25f, 783.99f, 0.35f); break;
            case wire::EventKind::Order:
            case wire::EventKind::Pilot: earcon(523.25f, 659.25f, 0.35f); break;
            default: earcon(587.33f, 587.33f, 0.2f); break;
            }
        }
        constexpr std::string_view marker = "[OLIPHAUNT] ";
        if (e.text.rfind(marker, 0) == 0) {
            const std::string message = e.text.substr(marker.size());
            const std::size_t separator = message.find(" | ");
            if (separator != std::string::npos) {
                const std::size_t idStart = message.rfind(' ', separator);
                const std::string id = idStart == std::string::npos ? std::string{} : message.substr(idStart + 1, separator - idStart - 1);
                if (id.size() == 12) {
                    auto found = std::find_if(oliphauntCards_.begin(), oliphauntCards_.end(),
                                              [&](const OliphauntCard& card) { return card.id == id; });
                    if (found == oliphauntCards_.end()) oliphauntCards_.push_front({id, message});
                    else {
                        found->text = message;
                        std::rotate(oliphauntCards_.begin(), found, std::next(found));
                    }
                    while (oliphauntCards_.size() > 5) oliphauntCards_.pop_back();
                }
            } else if (message.rfind("forgot ", 0) == 0) {
                const std::string id = message.substr(7);
                std::erase_if(oliphauntCards_, [&](const OliphauntCard& card) { return card.id == id; });
            } else if (message.rfind("trusted ", 0) == 0) {
                const std::string id = message.substr(8);
                std::erase_if(oliphauntCards_, [&](const OliphauntCard& card) { return card.id == id; });
            }
        }
        events_.push_back(std::move(e));
        eventAt_ = in.time;
        while (events_.size() > 6) events_.pop_front();
    }
    for (const auto& d : client_->takeDropStates()) {
        if (d.entity != ride_.entity || mode_ != Mode::Ride) continue;
        switch (d.state) {
        case wire::Drop::Riding:
            ride_.control = false;
            ride_.status = upper(d.reason);
            ride_.eta = d.eta;
            break;
        case wire::Drop::Control:
            if (audioCues()) earcon(523.25f, 1046.5f, 0.6f); // rising: yours
            ride_.control = true;
            ride_.controlAt = in.time;
            ride_.status = "YOU HAVE CONTROL";
            ride_.handling = {static_cast<Handling::Model>(d.model), d.minSpeed, d.maxSpeed, d.yawRate, d.climbRate, d.accel};
            ride_.havePrediction = false;
            haptics_[0] = haptics_[1] = {0.8f, 0.08f};
            break;
        case wire::Drop::Denied:
            if (audioCues()) earcon(392, 261.63f);
            leaveRide(upper(d.reason));
            break;
        case wire::Drop::Released:
            if (audioCues()) earcon(1046.5f, 523.25f); // falling: autonomy has it again
            leaveRide(upper(d.reason));
            break;
        case wire::Drop::Lost:
            if (audioCues()) earcon(392, 196);
            leaveRide("NODE LOST: " + upper(d.reason));
            break;
        }
    }
    client_->sample(in.time, entities_);
}

// ---------------------------------------------------------------------------------------------
// Lobby: the Construct

void App::lobby(const Input& in) {
    render::applyEnvironment(scene_, render::Environment::Construct);
    scene_.worldUp = {0, 1, 0, 0};
    const float t = Fd(in.time - startTime_);
    // The floor: concentric rings and radials, faint, so there is somewhere to stand.
    for (int r = 1; r <= 12; ++r) {
        const float radius = F(r) * 1.5f;
        const int seg = 24 + r * 6;
        for (int k = 0; k < seg; ++k) {
            const float a0 = 2 * kPi * F(k) / F(seg), a1 = 2 * kPi * F(k + 1) / F(seg);
            solid_.push_back({{radius * std::cos(a0), 0.001f, radius * std::sin(a0)},
                              {radius * std::cos(a1), 0.001f, radius * std::sin(a1)},
                              0.006f,
                              1.0f,
                              hex(0xC9CCD1),
                              static_cast<std::uint8_t>(std::max(30, 150 - r * 10))});
        }
    }
    // The kit drifts in a slow ring around you: everything glyder can show you.
    const int count = 12;
    const kit::Shape show[count] = {kit::Shape::Kite,  kit::Shape::Quadcopter, kit::Shape::RelaySat,   kit::Shape::Rover,
                                    kit::Shape::Truck, kit::Shape::Fighter,    kit::Shape::Node,       kit::Shape::Helicopter,
                                    kit::Shape::Boat,  kit::Shape::RobotArm,   kit::Shape::ImagingSat, kit::Shape::FixedWing};
    for (int i = 0; i < count; ++i) {
        const float a = F(i) * 2 * kPi / F(count) + t * 0.025f;
        const ShapeInfo si = infoOf(static_cast<std::uint16_t>(show[i]));
        const float s = 1.1f / si.radius;
        const Vec3 p{7.5f * std::sin(a), 1.6f + 0.25f * std::sin(t * 0.4f + F(i)), -7.5f * std::cos(a)};
        render::DrawItem d;
        d.mesh = static_cast<render::MeshId>(show[i]);
        const Quat spin = axisAngle({0, 1, 0}, -a + kPi * 0.5f + 0.3f * std::sin(t * 0.2f + F(i)));
        d.model = compose(p, spin, {s, s, s}) * worldToRender() * translation(-si.centre);
        d.tint = pal::teams[1];
        d.emissive = 1.6f;
        items_.push_back(d);
    }

    // Everything else is laid out facing where you first looked.
    const Vec3 fwd{0, 0, -1}, right{1, 0, 0}, up{0, 1, 0};
    const float eye = std::clamp(in.head.pos.y, 1.0f, 1.9f);
    label("GLYDER", Vec3{0, eye + 0.55f, -3.2f}, right, up, 0.34f, kGraphite, Align::Centre);
    label(std::string("GYGAX ") + kVersion + "  \"" + kReleaseName + "\"", Vec3{0, eye + 0.36f, -3.2f}, right, up, 0.075f, hex(0x5C636D),
          Align::Centre);

    const auto found = client_->found();
    const float cardY = eye - 0.12f;
    if (mode_ == Mode::Keypad) {
        // Only the Construct itself behind the keypad.
    } else if (mode_ == Mode::Connecting) {
        card(Vec3{0, cardY, -1.6f}, right, up, {0.34f, 0.12f}, kInk, 240, kCyan, 0.006f);
        label("CONNECTING", Vec3{0, cardY + 0.035f, -1.598f}, right, up, 0.03f, kCyan, Align::Centre);
        label(connectingTo_, Vec3{0, cardY - 0.03f, -1.598f}, right, up, 0.022f, kPaper, Align::Centre);
        const float p = client_->terrainProgress();
        if (client_->state() == Client::State::Connected && p < 1.0f) {
            card(Vec3{-0.25f + 0.25f * p, cardY - 0.085f, -1.597f}, right, up, {0.25f * p, 0.004f}, kCyan, 255, kCyan);
        }
    } else if (found.empty()) {
        label("LOOKING FOR GYGAX HOSTS ON THIS NETWORK", Vec3{0, cardY + 0.05f, -1.8f}, right, up, 0.036f, kGraphite, Align::Centre);
        label("ON YOUR COMPUTER RUN   OHFLOCK --GLYDER   OR   GYGAX SERVE --GLYDER", Vec3{0, cardY - 0.02f, -1.8f}, right, up, 0.022f,
              hex(0x5C636D), Align::Centre);
    } else {
        const int nCards = static_cast<int>(std::min<std::size_t>(found.size(), 5));
        for (int i = 0; i < nCards; ++i) {
            const Found& h = found[static_cast<std::size_t>(i)];
            // An arc of cards, 0.55 m apart.
            const float a = (F(i) - F(nCards - 1) * 0.5f) * 0.36f;
            const Vec3 c{1.6f * std::sin(a), cardY, -1.6f * std::cos(a)};
            const Vec3 r{std::cos(a), 0, std::sin(a)};
            const std::string id = "host:" + h.addr.str();
            if (button(in, id, c, r, up, {0.24f, 0.14f}, "", false)) openHost(h);
            const Vec3 nrm = normalize(cross(r, up)) * 0.002f;
            label(h.beacon.title.empty() ? h.beacon.name : h.beacon.title, c + up * 0.06f - r * 0.2f + nrm, r, up, 0.034f, kPaper);
            label(h.beacon.name + "  " + h.addr.str(), c + up * 0.005f - r * 0.2f + nrm, r, up, 0.016f, kDim);
            label(std::to_string(h.beacon.entities) + " NODES", c - up * 0.06f - r * 0.2f + nrm, r, up, 0.02f, kCyan);
            label(h.beacon.pairing ? (paired_.count(h.addr.str()) ? "PAIRED" : "PAIRING CODE") : "OPEN", c - up * 0.06f + r * 0.2f + nrm, r,
                  up, 0.016f, h.beacon.pairing ? kAmber : kGreen, Align::Right);
        }
    }
    // Always available: the Construct demo and the vision settings.
    if (mode_ != Mode::Connecting && mode_ != Mode::Keypad) {
        label("SCENES", Vec3{-0.62f, eye - 0.37f, -1.5f}, right, up, 0.024f, hex(0x5C636D));
        sceneRow(in, Vec3{-0.14f, eye - 0.45f, -1.5f}, right, up, 0.96f, 0.045f);
        if (button(in, "vision", Vec3{0.5f, eye - 0.45f, -1.5f}, right, up, {0.12f, 0.045f}, "VISION", overlay_ == Overlay::Vision))
            overlay_ = overlay_ == Overlay::Vision ? Overlay::None : Overlay::Vision;
    }
    (void)fwd;
}

void App::keypad(const Input& in) {
    lobby(in); // the Construct behind it (its cards and buttons stand aside while the keypad is up)
    const UiLayer layer(*this);
    const Vec3 right{1, 0, 0}, up{0, 1, 0};
    const float eye = std::clamp(in.head.pos.y, 1.0f, 1.9f);
    const Vec3 c{0, eye - 0.15f, -1.2f};
    card(c, right, up, {0.2f, 0.27f}, kInk, 245, kCyan, 0.005f);
    label("PAIRING CODE", c + up * 0.22f + Vec3{0, 0, 0.002f}, right, up, 0.022f, kCyan, Align::Centre);
    label(connectingTo_, c + up * 0.185f + Vec3{0, 0, 0.002f}, right, up, 0.014f, kDim, Align::Centre);
    std::string shown;
    for (std::size_t i = 0; i < 6; ++i) shown += i < code_.size() ? code_[i] : '_';
    label(shown, c + up * 0.12f + Vec3{0, 0, 0.002f}, right, up, 0.04f, kPaper, Align::Centre);
    const char* keys = "123456789<0>";
    for (int k = 0; k < 12; ++k) {
        const int row = k / 3, col = k % 3;
        const Vec3 bc = c + right * (F(col - 1) * 0.11f) + up * (0.03f - F(row) * 0.075f) + Vec3{0, 0, 0.004f};
        const char ch = keys[k];
        const std::string text = ch == '<' ? "DEL" : ch == '>' ? "OK" : std::string(1, ch);
        if (button(in, std::string("key") + ch, bc, right, up, {0.048f, 0.032f}, text, false, ch != '>' || code_.size() >= 4)) {
            if (ch == '<') {
                if (!code_.empty()) code_.pop_back();
            } else if (ch == '>') {
                connectTo(pendingHost_, code_, connectingTo_);
            } else if (code_.size() < 12) {
                code_ += ch;
            }
        }
    }
    for (char ch : in.typed) {
        if (ch >= '0' && ch <= '9' && code_.size() < 12) code_ += ch;
        if ((ch == '\b' || ch == 127) && !code_.empty()) code_.pop_back();
        if ((ch == '\r' || ch == '\n') && code_.size() >= 4) connectTo(pendingHost_, code_, connectingTo_);
    }
    if (in.hands[1].secondary && !secondaryWas_[1]) mode_ = Mode::Lobby;
}

// ---------------------------------------------------------------------------------------------
// The world

Mat4 App::tableTransform() const {
    return translation(tableCentre_) * rotation(axisAngle({0, 1, 0}, tableYaw_)) * scaling({tableScale_, tableScale_, tableScale_}) *
           worldToRender() * translation(-focus_);
}

void App::tableGestures(const Input& in) {
    // Grip with one hand: drag and turn the table. Both hands: scale it too.
    const int held = (in.hands[0].squeeze > 0.6f ? 1 : 0) + (in.hands[1].squeeze > 0.6f ? 2 : 0);
    const bool both = held == 3;
    for (int h = 0; h < 2; ++h) {
        const bool g = in.hands[h].squeeze > 0.6f;
        if (g && !grabbing_[h]) {
            grabStart_[0] = in.hands[0].grip;
            grabStart_[1] = in.hands[1].grip;
            grabFocus_ = focus_;
            grabScale_ = tableScale_;
            grabYaw_ = tableYaw_;
            grabDistance_ = std::max(length(in.hands[0].grip.pos - in.hands[1].grip.pos), 0.05f);
        }
        grabbing_[h] = g;
    }
    if (held == 0) return;
    auto worldDelta = [&](Vec3 stageDelta) {
        // A stage-space displacement as a world (ENU) one at the current table scale and yaw.
        const Vec3 local = rotate(axisAngle({0, 1, 0}, -tableYaw_), stageDelta) / tableScale_;
        return fromRender(local);
    };
    if (both) {
        const Vec3 a0 = grabStart_[0].pos, b0 = grabStart_[1].pos, a1 = in.hands[0].grip.pos, b1 = in.hands[1].grip.pos;
        const float d1 = std::max(length(a1 - b1), 0.05f);
        tableScale_ = std::clamp(grabScale_ * d1 / grabDistance_, 1e-6f, 0.5f);
        const float yaw0 = std::atan2(b0.x - a0.x, b0.z - a0.z), yaw1 = std::atan2(b1.x - a1.x, b1.z - a1.z);
        tableYaw_ = grabYaw_ + (yaw1 - yaw0);
        const Vec3 mid0 = (a0 + b0) * 0.5f, mid1 = (a1 + b1) * 0.5f;
        Vec3 d = worldDelta(mid1 - mid0);
        d.z = 0;
        focus_ = grabFocus_ - d;
    } else {
        const int h = held == 1 ? 0 : 1;
        Vec3 d = worldDelta(in.hands[h].grip.pos - grabStart_[h].pos);
        d.z = 0;
        focus_ = grabFocus_ - d;
    }
}

void App::drawWorld(const Input& in, const Mat4& w, float worldScale, bool table) {
    heard_ = w;
    heardScale_ = worldScale;
    haveHeard_ = true;
    const Vec3 head = in.head.pos;
    if (haveTerrain_) {
        render::DrawItem t;
        t.mesh = options_.terrainMesh;
        t.model = w;
        t.contour = true;
        items_.push_back(t);
    }
    const float R = world_ ? world_->radius : 1000.0f;
    const float groundZ = world_ ? world_->groundZ : 0.0f;
    auto groundAt = [&](float x, float y) { return terrain_ ? terrain_->sample(x, y) : groundZ; };
    // Satellites and anything far out are pulled onto a shell over the table so they stay in view.
    auto place = [&](Vec3 p) {
        if (!table) return p;
        Vec3 q = p - Vec3{focus_.x, focus_.y, 0};
        const float hr = std::sqrt(q.x * q.x + q.y * q.y);
        if (hr > R * 1.1f) {
            const float k = (R * 1.1f + (hr - R * 1.1f) * 0.004f) / hr;
            q.x *= k;
            q.y *= k;
        }
        const float ceiling = groundZ + R * 0.3f;
        if (p.z > ceiling) q.z = ceiling + (p.z - ceiling) * 0.003f;
        return q + Vec3{focus_.x, focus_.y, 0};
    };

    float bestT = 1e9f;
    std::uint32_t best = 0;
    const Pose ray = pointer(in, 1);
    const bool rayOn = (in.hands[1].active || in.pointer.has_value()) && overlay_ == Overlay::None;
    for (const auto& e : entities_) {
        if (mode_ == Mode::Ride && e.id == ride_.entity) continue; // you are inside it
        const ShapeInfo si = infoOf(e.shape);
        const Vec3 p = place(e.pos);
        float s = e.scale;
        // Map-table icons: never smaller than about two-thirds of a degree.
        const Vec3 stagePos = w.transformPoint(p);
        const float dist = std::max(length(stagePos - head), 0.05f);
        const float shown = si.radius * e.scale * worldScale;
        const kit::Domain dom =
            kit::domain(static_cast<kit::Shape>(std::min<std::uint16_t>(e.shape, static_cast<std::uint16_t>(kit::Shape::Count) - 1)));
        const bool shot =
            e.shape == static_cast<std::uint16_t>(kit::Shape::Missile) || e.shape == static_cast<std::uint16_t>(kit::Shape::Bomb);
        const float minShown = dist * (table ? (shot ? 0.006f : (in.desktop ? 0.02f : 0.012f)) : 0.0f);
        if (shown < minShown) s *= minShown / std::max(shown, 1e-9f);
        render::DrawItem d;
        d.mesh = e.shape;
        d.model = w * compose(p, e.q, {s, s, s});
        d.tint = e.tint;
        d.emissive = (e.flags & wire::kFiring) ? 3.5f : 1.6f;
        if (e.id == hovered_) d.highlight = 0.9f;
        if ((e.flags & wire::kDriven) && e.id != ride_.entity) d.highlight = std::max(d.highlight, 0.35f);
        items_.push_back(d);

        // Blob shadow on the ground for things near it.
        if (dom == kit::Domain::Air || dom == kit::Domain::Ground || dom == kit::Domain::Sea) {
            const float gz = groundAt(p.x, p.y);
            const float height = p.z - gz;
            const float r = si.radius * s * 0.8f;
            if (height < r * 40.0f) {
                render::PanelStyle st;
                st.fill = {0, 0, 0};
                st.fillAlpha = static_cast<std::uint8_t>(std::clamp(140.0f - height / std::max(r, 0.01f) * 3.0f, 20.0f, 140.0f));
                st.radius = r;
                st.softness = r * 0.6f * worldScale;
                // Lifted about a millimetre in the rendered scene, whatever the scale.
                const Vec3 c = w.transformPoint({p.x, p.y, gz + 0.002f / std::max(worldScale, 1e-6f)});
                const Vec3 ex = normalize(w.transformDir({1, 0, 0})), ny = normalize(w.transformDir({0, 1, 0}));
                render::addPanel(panels_, c, ex, ny, {r * worldScale, r * worldScale}, st);
            }
        }
        // A real machine stands on a gold hexagon.
        if (e.flags & wire::kPhysical) {
            const float r = si.radius * s * 1.3f;
            const float gz = dom == kit::Domain::Ground ? p.z : groundAt(p.x, p.y);
            for (int k = 0; k < 6; ++k) {
                const float a0 = F(k) * kPi / 3, a1 = F(k + 1) * kPi / 3;
                glow_.push_back({w.transformPoint({p.x + r * std::cos(a0), p.y + r * std::sin(a0), gz + 0.05f}),
                                 w.transformPoint({p.x + r * std::cos(a1), p.y + r * std::sin(a1), gz + 0.05f}), r * worldScale * 0.06f,
                                 1.2f, kGold});
            }
        }
        // Another pilot is in this node: show their presence riding on it.
        if ((e.flags & wire::kDriven) && e.id != ride_.entity) {
            render::DrawItem op;
            op.mesh = static_cast<render::MeshId>(kit::Shape::Operator);
            const float os = std::max(si.radius * s * 0.6f, 0.5f / std::max(worldScale, 1e-6f) * 0.02f);
            op.model = w * compose(p + Vec3{0, 0, si.radius * s * 1.4f}, e.q, {os, os, os});
            op.tint = kCyan;
            items_.push_back(op);
        }
        // Trails behind shots.
        if (shot) {
            auto& tr = trails_[e.id];
            if (tr.empty() || in.time - tr.back().first > 0.05) tr.emplace_back(in.time, p);
            while (!tr.empty() && in.time - tr.front().first > 2.5) tr.pop_front();
        }
        // Picking.
        if (rayOn && (e.flags & wire::kAlive)) {
            const Vec3 c = w.transformPoint(p + rotate(e.q, si.centre * s));
            const float r = std::max(si.radius * s * worldScale, dist * 0.02f);
            if (auto t = raySphere(ray.pos, ray.forward(), c, r))
                if (*t < bestT) {
                    bestT = *t;
                    best = e.id;
                }
        }
    }
    for (auto it = trails_.begin(); it != trails_.end();) {
        if (it->second.empty() || in.time - it->second.back().first > 2.5) {
            it = trails_.erase(it);
            continue;
        }
        const auto& tr = it->second;
        for (std::size_t k = 1; k < tr.size(); ++k) {
            const float age = Fd(in.time - tr[k].first);
            glow_.push_back({w.transformPoint(tr[k - 1].second), w.transformPoint(tr[k].second), 0.6f * worldScale * 4.0f,
                             0.5f * (1.0f - age / 2.5f), hex(0xFFE0B0)});
        }
        ++it;
    }
    // Effects from the host.
    for (const auto& fx : client_->effects()) {
        const float width = std::max(fx.width, 0.01f) * worldScale;
        const float inten = F(fx.intensity) / 255.0f;
        switch (fx.kind) {
        case wire::EffectKind::Ring: {
            const int seg = 40;
            for (int k = 0; k < seg; ++k) {
                const float a0 = 2 * kPi * F(k) / F(seg), a1 = 2 * kPi * F(k + 1) / F(seg);
                glow_.push_back({w.transformPoint(place(fx.a + Vec3{fx.b.x * std::cos(a0), fx.b.x * std::sin(a0), 0})),
                                 w.transformPoint(place(fx.a + Vec3{fx.b.x * std::cos(a1), fx.b.x * std::sin(a1), 0})), width, inten,
                                 fx.colour});
            }
            break;
        }
        case wire::EffectKind::Spot: {
            const float r = std::max(fx.width, 1.0f);
            for (int k = 0; k < 12; ++k) {
                const float a0 = 2 * kPi * F(k) / 12, a1 = 2 * kPi * F(k + 1) / 12;
                glow_.push_back({w.transformPoint(fx.a + Vec3{r * std::cos(a0), r * std::sin(a0), 0.2f}),
                                 w.transformPoint(fx.a + Vec3{r * std::cos(a1), r * std::sin(a1), 0.2f}), r * 0.3f * worldScale,
                                 inten * 1.5f, fx.colour});
            }
            break;
        }
        case wire::EffectKind::Link:
            glow_.push_back({w.transformPoint(place(fx.a)), w.transformPoint(place(fx.b)), width, inten * 0.35f, fx.colour});
            break;
        default:
            glow_.push_back({w.transformPoint(place(fx.a)), w.transformPoint(place(fx.b)), width,
                             inten * (fx.kind == wire::EffectKind::Laser ? 2.0f : 1.0f), fx.colour});
            break;
        }
    }
    hovered_ = best;
    if (best && hot_.empty()) {
        // The ray stops at what it hits.
        glow_.push_back({ray.pos, ray.pos + ray.forward() * bestT, 0.002f, 1.0f, kCyan});
    }
}

void App::overview(const Input& in) {
    const auto env = world_ ? static_cast<render::Environment>(std::min<std::uint8_t>(world_->environment, 4)) : render::Environment::Night;
    tableGestures(in);
    // Sticks: right turns (x) and zooms (y); left pans.
    const float dt = std::min(in.dt, 0.1f);
    const Vec2 rs = in.hands[1].stick, ls = in.hands[0].stick;
    if (std::fabs(rs.y) > 0.15f) tableScale_ = std::clamp(tableScale_ * std::exp(rs.y * dt * 1.6f), 1e-6f, 0.5f);
    if (std::fabs(rs.x) > 0.15f) tableYaw_ -= rs.x * dt * 1.2f;
    if (std::fabs(ls.x) > 0.15f || std::fabs(ls.y) > 0.15f) {
        const Vec3 local = rotate(axisAngle({0, 1, 0}, -tableYaw_), Vec3{ls.x, 0, -ls.y}) * (dt * 0.6f / tableScale_);
        const Vec3 d = fromRender(local);
        focus_ += Vec3{d.x, d.y, 0};
    }
    if (terrain_) focus_.z = terrain_->sample(focus_.x, focus_.y);
    const Mat4 w = tableTransform();
    render::applyEnvironment(scene_, env, tableScale_, w.transformPoint(focus_).y);
    // Directions in the scene are the world's, turned with the table.
    const Vec3 sun = normalize(w.transformDir(fromRender({scene_.sunDir.x, scene_.sunDir.y, scene_.sunDir.z})));
    const Vec3 moon = normalize(w.transformDir(fromRender({scene_.moon.x, scene_.moon.y, scene_.moon.z})));
    scene_.sunDir = {sun.x, sun.y, sun.z, scene_.sunDir.w};
    scene_.moon = {moon.x, moon.y, moon.z, scene_.moon.w};
    scene_.worldUp = {0, 1, 0, 0};
    // No fog on a map table: it is all within arm's reach.
    scene_.skyHorizon.w = 0;

    drawWorld(in, w, tableScale_, true);

    // The table's rim: a soft ring at the world's edge.
    const float R = world_ ? world_->radius : 1000.0f;
    const int seg = 96;
    for (int k = 0; k < seg; ++k) {
        const float a0 = 2 * kPi * F(k) / F(seg), a1 = 2 * kPi * F(k + 1) / F(seg);
        const Vec3 p0 = w.transformPoint({focus_.x + R * std::cos(a0), focus_.y + R * std::sin(a0), focus_.z});
        const Vec3 p1 = w.transformPoint({focus_.x + R * std::cos(a1), focus_.y + R * std::sin(a1), focus_.z});
        glow_.push_back({p0, p1, 0.004f, 0.6f, kCyan});
    }

    // The hovered node: its card (on the interface layer, so terrain never hides it), and the way in.
    if (hovered_) {
        const UiLayer layer(*this);
        const auto it = std::find_if(entities_.begin(), entities_.end(), [&](const auto& e) { return e.id == hovered_; });
        if (it != entities_.end()) {
            const ShapeInfo si = infoOf(it->shape);
            const Vec3 at = w.transformPoint(it->pos) + Vec3{0, std::max(si.radius * it->scale * tableScale_, 0.02f) + 0.07f, 0};
            const Vec3 toHead = normalize(Vec3{in.head.pos.x - at.x, 0, in.head.pos.z - at.z});
            const Vec3 right = normalize(cross(Vec3{0, 1, 0}, toHead));
            const Vec3 up{0, 1, 0};
            const auto inf = client_->info(it->id);
            const bool drivable = it->flags & wire::kDrivable;
            const bool physical = it->flags & wire::kPhysical;
            const float scale = std::clamp(length(at - in.head.pos) * 0.6f, 0.4f, 2.5f);
            card(at, right, up, Vec2{0.13f * scale, 0.045f * scale}, kInk, 230, physical ? kGold : kCyan, 0.002f);
            const Vec3 o = at - right * (0.118f * scale) + toHead * 0.002f;
            label(inf ? inf->name : "NODE " + std::to_string(it->id), o + up * (0.012f * scale), right, up, 0.018f * scale, kPaper);
            label(inf ? inf->task : std::string(kit::name(static_cast<kit::Shape>(it->shape))), o - up * (0.014f * scale), right, up,
                  0.011f * scale, kDim);
            label(drivable ? (physical ? "REAL MACHINE  TRIGGER TO DROP IN" : "TRIGGER TO DROP IN") : "", o - up * (0.034f * scale), right,
                  up, 0.009f * scale, physical ? kGold : kCyan);
            if (hot_.empty() && pressed(in, 1)) {
                if (drivable) {
                    enterRide(it->id, in);
                } else {
                    focus_ = {it->pos.x, it->pos.y, focus_.z};
                    haptics_[1] = {0.3f, 0.02f};
                }
            }
        }
    }
}

void App::enterRide(std::uint32_t id, const Input& in) {
    // The switch is immediate; control follows when the node is ready.
    const auto it = std::find_if(entities_.begin(), entities_.end(), [&](const auto& e) { return e.id == id; });
    ride_ = {};
    ride_.entity = id;
    ride_.since = in.time;
    ride_.status = "DROPPING IN";
    ride_.shape = it != entities_.end() ? static_cast<kit::Shape>(it->shape) : kit::Shape::Marker;
    ride_.anchor = {in.head.pos, yawOnly(in.head.q)};
    mode_ = Mode::Ride;
    overlay_ = Overlay::None;
    rainAt_ = in.time;
    rainOut_ = false;
    haptics_[0] = haptics_[1] = {0.6f, 0.05f};
    client_->dropIn(id);
}

void App::select(std::uint32_t entity) {
    Input in;
    in.time = net::now();
    in.head.pos = {0, 1.6f, 0};
    enterRide(entity, in);
}

void App::jackOut() {
    leaveRide("JACKED OUT");
}

std::uint32_t App::find(kit::Shape shape) const {
    for (const auto& e : entities_)
        if (e.shape == static_cast<std::uint16_t>(shape) && (e.flags & wire::kDrivable)) return e.id;
    return 0;
}

void App::leaveRide(const std::string& why) {
    if (mode_ != Mode::Ride) return;
    if (ride_.entity) client_->release();
    ride_ = {};
    mode_ = world_ ? Mode::Overview : Mode::Lobby;
    rainAt_ = net::now();
    rainOut_ = true;
    if (!why.empty()) toast(why, 4);
}

Mat4 App::rideTransform(const Input& in) {
    (void)in;
    const auto it = std::find_if(entities_.begin(), entities_.end(), [&](const auto& e) { return e.id == ride_.entity; });
    Vec3 pos;
    Quat q;
    float scale = 1;
    if (it != entities_.end()) {
        pos = it->pos;
        q = it->q;
        scale = it->scale;
        ride_.shape = static_cast<kit::Shape>(it->shape);
    }
    // While in control, show the node where the pilot's own inputs put it, reconciled softly with
    // the host's authoritative state.
    if (ride_.control && ride_.handling.model != Handling::Model::None && it != entities_.end()) {
        if (!ride_.havePrediction) {
            ride_.predicted.pos = pos;
            ride_.predicted.heading = headingOf(q);
            const Vec3 v = it->vel;
            const float hsp = std::sqrt(v.x * v.x + v.y * v.y);
            ride_.predicted.speed = ride_.handling.model == Handling::Model::Hover ? 0.0f : hsp;
            ride_.predicted.climb = v.z;
            ride_.havePrediction = true;
        }
        const Vec3 err = pos - ride_.predicted.pos;
        const float k = 1.0f - std::exp(-std::min(in.dt, 0.1f) / 0.3f);
        ride_.predicted.pos += err * (length(err) > 50.0f ? 1.0f : k);
        pos = ride_.predicted.pos;
        q = ride_.predicted.attitude(ride_.handling.model);
    }
    const Quat shown = horizonLock_ ? enuYawOnly(q) : q;
    const Mat4 eye = worldToRender() * compose(pos, shown) * translation(kit::eyepoint(ride_.shape) * scale) * cameraInModel();
    const Mat4 anchor = compose(ride_.anchor.pos, ride_.anchor.q);
    return anchor * rigidInverse(eye) * worldToRender();
}

void App::ride(const Input& in) {
    const auto env = world_ ? static_cast<render::Environment>(std::min<std::uint8_t>(world_->environment, 4)) : render::Environment::Night;
    // Controls first, so the prediction this frame includes them.
    if (ride_.control) {
        wire::Control c;
        c.entity = ride_.entity;
        c.clientTime = in.time;
        c.axes = {in.hands[0].stick.y, in.hands[0].stick.x, 0, in.hands[1].stick.x, in.hands[1].stick.y, in.hands[1].trigger};
        for (float& a : c.axes)
            if (std::fabs(a) < 0.08f) a = 0; // dead zone
        c.buttons = static_cast<std::uint16_t>((in.hands[1].primary ? 1 : 0) | (in.hands[0].primary ? 2 : 0));
        const Vec3 f = rotate(conjugate(ride_.anchor.q) * in.head.q, {0, 0, -1});
        c.lookYaw = std::atan2(-f.x, -f.z);
        c.lookPitch = std::asin(clamp(f.y, -1, 1));
        client_->control(c);
        if (ride_.havePrediction) {
            const float axes[6] = {c.axes[0], c.axes[1], c.axes[2], c.axes[3], c.axes[4], c.axes[5]};
            const float gz = terrain_ ? terrain_->sample(ride_.predicted.pos.x, ride_.predicted.pos.y) : (world_ ? world_->groundZ : 0.0f);
            step(ride_.predicted, ride_.handling, axes, std::min(in.dt, 0.1f), gz);
        }
    }
    // B: jack out, at once.
    if (in.hands[1].secondary && !secondaryWas_[1]) {
        leaveRide("JACKED OUT");
        overview(in);
        return;
    }
    const Mat4 w = rideTransform(in);
    render::applyEnvironment(scene_, env, 1.0f, 0.0f);
    const Vec3 sun = normalize(w.transformDir(fromRender({scene_.sunDir.x, scene_.sunDir.y, scene_.sunDir.z})));
    const Vec3 moon = normalize(w.transformDir(fromRender({scene_.moon.x, scene_.moon.y, scene_.moon.z})));
    const Vec3 up = normalize(w.transformDir({0, 0, 1}));
    scene_.sunDir = {sun.x, sun.y, sun.z, scene_.sunDir.w};
    scene_.moon = {moon.x, moon.y, moon.z, scene_.moon.w};
    scene_.worldUp = {up.x, up.y, up.z, 0};
    const float groundZ = world_ ? world_->groundZ : 0.0f;
    scene_.contour.z = dot(w.transformPoint({0, 0, 0}), up); // contour heights are world z

    drawWorld(in, w, 1.0f, false);

    // A ground grid for motion cues where there is no terrain.
    if (!haveTerrain_) {
        const auto it = std::find_if(entities_.begin(), entities_.end(), [&](const auto& e) { return e.id == ride_.entity; });
        const Vec3 c = it != entities_.end() ? it->pos : Vec3{};
        const float cell = 5.0f, extent = 120.0f;
        const float gx = std::floor(c.x / cell) * cell, gy = std::floor(c.y / cell) * cell;
        const Rgb8 gridC = env == render::Environment::Construct ? hex(0xC4C8CE) : hex(0x3A6A90);
        const int lines = static_cast<int>(extent / cell);
        for (int k = -lines; k <= lines; ++k) {
            const float d = F(k) * cell;
            const auto alpha = static_cast<std::uint8_t>(std::clamp(110.0f * (1.0f - std::fabs(d) / extent), 0.0f, 110.0f));
            solid_.push_back({w.transformPoint({gx + d, gy - extent, groundZ + 0.01f}),
                              w.transformPoint({gx + d, gy + extent, groundZ + 0.01f}), 0.012f, 1, gridC, alpha});
            solid_.push_back({w.transformPoint({gx - extent, gy + d, groundZ + 0.01f}),
                              w.transformPoint({gx + extent, gy + d, groundZ + 0.01f}), 0.012f, 1, gridC, alpha});
        }
    }

    // Comfort: narrow the view while the node turns hard, unless the pilot turned it off.
    float turn = 0;
    if (ride_.control && ride_.havePrediction) turn = std::fabs(ride_.predicted.yawRate) + std::fabs(ride_.predicted.climb) * 0.02f;
    const float target = vignette_ ? clamp((turn - 0.25f) * 0.9f, 0.0f, 0.55f) : 0.0f;
    vignetteAmount_ += (target - vignetteAmount_) * (1.0f - std::exp(-std::min(in.dt, 0.1f) * 6.0f));
    if (vignetteAmount_ > 0.01f) {
        render::PanelStyle st;
        st.fill = {0, 0, 0};
        st.fillAlpha = 255;
        st.radius = 0.45f * (1.0f - vignetteAmount_);
        st.glow = -0.12f; // hole mode: softness
        const Pose& h = in.head;
        render::addPanel(uiPanels_, h.pos + h.forward() * 0.35f, h.right(), h.up(), {0.9f, 0.9f}, st);
    }
}

// ---------------------------------------------------------------------------------------------
// HUD, menus, vision

void App::hud(const Input& in) {
    const UiLayer layer(*this);
    // The visor follows your gaze lazily in yaw (a quarter-second lag) and sits level: it never
    // swims with every small head movement, and it is always there when you look for it.
    const Pose& h = in.head;
    const float target = yawOf(h.q);
    if (!hudYawSet_ || in.desktop) {
        hudYaw_ = target;
        hudYawSet_ = true;
    } else {
        float d = target - hudYaw_;
        while (d > kPi) d -= 2 * kPi;
        while (d < -kPi) d += 2 * kPi;
        hudYaw_ += d * (1.0f - std::exp(-std::min(in.dt, 0.1f) / 0.25f));
    }
    const Quat visor = in.desktop ? h.q : axisAngle({0, 1, 0}, hudYaw_) * axisAngle({1, 0, 0}, -0.06f);
    const Vec3 f = rotate(visor, {0, 0, -1}), r = rotate(visor, {1, 0, 0}), u = rotate(visor, {0, 1, 0});
    const float d = 1.4f;
    const Vec3 c = h.pos + f * d;
    // Toasts: a lower third.
    float row = 0;
    for (auto it = toasts_.begin(); it != toasts_.end();) {
        if (in.time > it->until) {
            it = toasts_.erase(it);
            continue;
        }
        const float a = clamp(Fd(it->until - in.time), 0.0f, 1.0f);
        const Vec3 tc = c - u * (0.36f - row);
        const float w = render::textWidth(it->text, 0.024f) * 0.5f + 0.035f;
        card(tc, r, u, {w, 0.028f}, kInk, static_cast<std::uint8_t>(215 * a), kCyan);
        label(it->text, tc - u * 0.012f - f * 0.002f, r, u, 0.024f, kPaper, Align::Centre, static_cast<std::uint8_t>(255 * a));
        row += 0.07f;
        ++it;
    }
    if (mode_ == Mode::Ride) {
        const auto it = std::find_if(entities_.begin(), entities_.end(), [&](const auto& e) { return e.id == ride_.entity; });
        const auto inf = client_->info(ride_.entity);
        const bool physical = it != entities_.end() && (it->flags & wire::kPhysical);
        const Rgb8 frame = physical ? kGold : kCyan;
        // Visor brackets: four corners, thin, so the HUD has a shape without a box.
        const float bx = 0.56f, by = 0.36f, arm = 0.05f;
        for (int sx : {-1, 1})
            for (int sy : {-1, 1}) {
                const Vec3 k = c + r * (F(sx) * bx) + u * (F(sy) * by);
                glowOut().push_back({k, k - r * (F(sx) * arm), 0.0016f, 0.55f, frame});
                glowOut().push_back({k, k - u * (F(sy) * arm), 0.0016f, 0.55f, frame});
            }
        // Top: who you are in, and whether it is yours.
        const std::string who = inf ? inf->name : std::string(kit::name(ride_.shape));
        const Rgb8 chipC = ride_.control ? kGreen : kAmber;
        const std::string chip = ride_.control ? "CONTROL" : "RIDING";
        const float whoW = render::textWidth(upper(who), 0.032f), chipW = render::textWidth(chip, 0.016f);
        const float total = whoW + 0.03f + chipW;
        const Vec3 top = c + u * 0.3f - r * (total * 0.5f);
        label(who, top, r, u, 0.032f, kPaper);
        card(top + r * (whoW + 0.03f + chipW * 0.5f) + u * 0.008f, r, u, {chipW * 0.5f + 0.012f, 0.016f}, chipC, 60, chipC);
        label(chip, top + r * (whoW + 0.03f) + u * 0.0f, r, u, 0.016f, chipC);
        if (physical) label("REAL MACHINE", c + u * 0.25f, r, u, 0.016f, kGold, Align::Centre);
        // Centre: the handover, while it happens.
        const bool fresh = ride_.control && in.time - ride_.controlAt < 2.2;
        if (fresh) {
            const float a = clamp(Fd(2.2 - (in.time - ride_.controlAt)) * 1.5f, 0.0f, 1.0f);
            label("YOU HAVE CONTROL", c + u * 0.1f, r, u, 0.05f, kGreen, Align::Centre, static_cast<std::uint8_t>(255 * a));
        } else if (!ride_.control) {
            std::string state = ride_.status;
            if (ride_.eta > 0) {
                char eta[32];
                std::snprintf(eta, sizeof eta, "   %.1f S", static_cast<double>(ride_.eta));
                state += eta;
            }
            label("HANDOVER AT THE NEXT STOPPING POINT", c + u * 0.16f, r, u, 0.016f, kDim, Align::Centre);
            label(state, c + u * 0.11f, r, u, 0.026f, kAmber, Align::Centre);
        }
        // Bottom: telemetry, what the node is doing, how to leave.
        if (it != entities_.end()) {
            const float speed = length(it->vel);
            const float hdg = std::fmod(headingOf(it->q) / kDeg + 360.0f, 360.0f);
            const float alt = it->pos.z - (terrain_ ? terrain_->sample(it->pos.x, it->pos.y) : (world_ ? world_->groundZ : 0.0f));
            char line[96];
            std::snprintf(line, sizeof line, "HDG %03.0f    %4.0f KM/H    AGL %4.0f M", static_cast<double>(hdg),
                          static_cast<double>(speed * 3.6f), static_cast<double>(alt));
            label(line, c - u * 0.25f, r, u, 0.022f, frame, Align::Centre);
            if (inf && !inf->task.empty()) label(inf->task, c - u * 0.29f, r, u, 0.015f, hex(0xA9B0B9), Align::Centre);
        }
        label(ride_.control ? "LEFT STICK  SPEED    RIGHT STICK  TURN AND CLIMB    B  JACK OUT" : "B  JACK OUT", c - u * 0.34f, r, u,
              0.013f, kDim, Align::Centre);
        // Boresight: where the node's nose points, fixed in the world, so you can look around and
        // still know which way you are going.
        if (it != entities_.end()) {
            const Vec3 nose = rotate(it->q, {1, 0, 0});
            const Mat4 w = rideTransform(in);
            const Vec3 p0 = w.transformPoint(it->pos), p1 = w.transformPoint(it->pos + nose * 60.0f);
            const Vec3 dir = normalize(p1 - p0);
            const Vec3 at = h.pos + dir * 3.0f;
            const Vec3 br = normalize(cross(dir, Vec3{0, 1, 0})), bu = normalize(cross(br, dir));
            const int seg = 24;
            for (int k = 0; k < seg; ++k) {
                const float a0 = 2 * kPi * F(k) / F(seg), a1 = 2 * kPi * F(k + 1) / F(seg);
                glowOut().push_back({at + (br * std::cos(a0) + bu * std::sin(a0)) * 0.035f,
                                     at + (br * std::cos(a1) + bu * std::sin(a1)) * 0.035f, 0.003f, 0.8f, frame});
            }
            for (const Vec3 tick : {br, -br, bu}) glowOut().push_back({at + tick * 0.035f, at + tick * 0.06f, 0.003f, 0.8f, frame});
        }
    }
    if (mode_ == Mode::Overview || mode_ == Mode::Ride) {
        // Status: on the left wrist in a headset (look at it), at the lower left on a desktop.
        Vec3 sc, rr, uu;
        float s = 1.0f;
        if (in.hands[0].active && !in.desktop) {
            const Pose& g = in.hands[0].grip;
            uu = g.forward();
            rr = normalize(cross(uu, Vec3{0, 1, 0}) * -1.0f);
            if (length(rr) < 0.5f) rr = g.right();
            sc = g.pos + g.up() * 0.06f + uu * 0.02f;
            const Vec3 face = normalize(cross(rr, uu));
            s = dot(face, normalize(in.head.pos - sc)) < 0.3f ? 0.0f : 0.45f;
        } else {
            sc = c - r * 0.78f - u * 0.02f;
            rr = r;
            uu = u;
            s = 1.15f;
        }
        const auto st = client_->status();
        if (s > 0) {
            const float rowH = 0.024f * s;
            const float rows = F(static_cast<int>(st.items.size()) + 1);
            const Vec2 half{0.19f * s, (0.05f * s + rows * rowH) * 0.5f};
            const Vec3 nrm = normalize(cross(rr, uu)) * 0.002f;
            card(sc, rr, uu, half, kInk, 200, hex(0x2A3038));
            Vec3 o = sc - rr * (half.x - 0.016f * s) + uu * (half.y - 0.032f * s) + nrm;
            label(world_ ? world_->title : "", o, rr, uu, 0.019f * s, kCyan);
            o -= uu * (0.034f * s);
            for (const auto& item : st.items) {
                label(item.label, o, rr, uu, 0.0125f * s, hex(0xA9B0B9));
                label(item.value, o + rr * (half.x * 2 - 0.032f * s), rr, uu, 0.0125f * s, toneColour(item.tone), Align::Right);
                o -= uu * rowH;
            }
            char net[48];
            std::snprintf(net, sizeof net, "%.0f MS", client_->rtt() * 1000.0);
            label("LINK", o, rr, uu, 0.0115f * s, kDim);
            label(net, o + rr * (half.x * 2 - 0.032f * s), rr, uu, 0.0115f * s, client_->rtt() < 0.03 ? kGreen : kAmber, Align::Right);
        }
        // The newest event, briefly, as a subtitle (not over an open panel).
        if (!base_ && !events_.empty() && in.time - eventAt_ < 7.0 && overlay_ == Overlay::None) {
            const float a = clamp(Fd(7.0 - (in.time - eventAt_)), 0.0f, 1.0f);
            const std::string text = events_.back().text.size() > 96 ? events_.back().text.substr(0, 94) + ".." : events_.back().text;
            label(text, c - u * (mode_ == Mode::Ride ? 0.2f : 0.3f), r, u, 0.017f, kPaper, Align::Centre,
                  static_cast<std::uint8_t>(235 * a));
        }
    }
}

void App::menu(const Input& in) {
    const UiLayer layer(*this);
    // Opened where you look, then fixed in the world while you use it.
    Pose& at = menuAt_;
    bool& placed = menuPlaced_;
    if (!placed) {
        const Quat y = yawOnly(in.head.q);
        at = {in.head.pos + rotate(y, {0, -0.15f, -0.75f}), y};
        placed = true;
    }
    const Vec3 r = at.right(), u = at.up(), c = at.pos;
    const Vec3 nrm = normalize(cross(r, u)) * 0.003f;
    std::vector<std::string> orders;
    if (world_) {
        std::istringstream ss(world_->help);
        std::string line;
        while (std::getline(ss, line))
            if (line.rfind("order:", 0) == 0) orders.push_back(line.substr(6));
    }
    // Scenes in one row, orders in two columns, then the settings.
    const int orderRows = (static_cast<int>(orders.size()) + 1) / 2;
    const float scenesH = 0.1f;
    const float rowsH = 0.05f * F(9 + orderRows) + (orders.empty() ? 0.0f : 0.03f) + scenesH;
    card(c, r, u, {0.24f, rowsH * 0.5f + 0.04f}, kInk, 235, kCyan, 0.004f);
    Vec3 o = c + u * (rowsH * 0.5f) + nrm;
    label("MENU", o - r * 0.21f, r, u, 0.022f, kCyan);
    label("SCENES", o + r * 0.21f, r, u, 0.012f, kDim, Align::Right);
    o -= u * 0.055f;
    if (sceneRow(in, o, r, u, 0.44f)) placed = false;
    o -= u * (scenesH - 0.055f);
    if (!orders.empty()) label("ORDERS TO THE HOST", o + r * 0.21f, r, u, 0.012f, kDim, Align::Right);
    o -= u * 0.055f;
    for (std::size_t k = 0; k < orders.size(); ++k) {
        const Vec3 bc = o + r * (k % 2 ? 0.11f : -0.11f) - u * (0.05f * F(static_cast<int>(k / 2)));
        if (button(in, "order:" + orders[k], bc, r, u, {0.1f, 0.021f}, orders[k])) {
            client_->order(orders[k]);
            toast("ORDER SENT: " + upper(orders[k]), 3);
        }
    }
    o -= u * (0.05f * F(orderRows) + (orders.empty() ? 0.0f : 0.03f));
    if (button(in, "horizon", o, r, u, {0.2f, 0.021f}, horizonLock_ ? "HORIZON LOCK  ON" : "HORIZON LOCK  OFF", horizonLock_))
        horizonLock_ = !horizonLock_;
    o -= u * 0.05f;
    if (button(in, "vignette", o, r, u, {0.2f, 0.021f}, vignette_ ? "COMFORT VIGNETTE  ON" : "COMFORT VIGNETTE  OFF", vignette_))
        vignette_ = !vignette_;
    o -= u * 0.05f;
    if (button(in, "colourbtn", o, r, u, {0.2f, 0.021f}, "COLOUR VISION  " + std::string(colour::name(colour_)),
               colour_ != colour::Vision::Typical)) {
        setColourVision(colour::next(colour_), true, colourPreview_);
        toast("COLOUR VISION: " + std::string(colour::name(colour_)), 3);
    }
    o -= u * 0.05f;
    if (button(in, "cuesbtn", o, r, u, {0.2f, 0.021f}, cues_ ? "AUDIO CUES  ON" : "AUDIO CUES  OFF", cues_)) setAudioCues(!cues_);
    o -= u * 0.05f;
    if (button(in, "blindbtn", o, r, u, {0.2f, 0.021f}, blind_ ? "BLIND MODE  ON" : "BLIND MODE  OFF", blind_)) setBlindMode(!blind_);
    o -= u * 0.05f;
    if (button(in, "basebtn", o, r, u, {0.2f, 0.021f}, base_ ? "BASE MODE  ON" : "BASE MODE  OFF", base_)) setBaseMode(!base_);
    o -= u * 0.05f;
    if (button(in, "arbtn", o, r, u, {0.2f, 0.021f}, ar_ ? "AR MODE  ON" : "AR MODE  OFF", ar_)) setArMode(!ar_);
    o -= u * 0.05f;
    if (button(in, "visionbtn", o, r, u, {0.2f, 0.021f}, "VISION")) {
        overlay_ = Overlay::Vision;
        placed = false;
    }
    o -= u * 0.05f;
    if (button(in, "leave", o, r, u, {0.2f, 0.021f}, world_ ? "LEAVE THIS HOST" : "CLOSE")) {
        if (mode_ == Mode::Ride) leaveRide("");
        client_->disconnect();
        world_.reset();
        oliphauntCards_.clear();
        mode_ = Mode::Lobby;
        overlay_ = Overlay::None;
        placed = false;
    }
    if (overlay_ != Overlay::Menu) placed = false;
}

// ---------------------------------------------------------------------------------------------
// Scenes

namespace {
constexpr const char* kScenes[] = {"CALM", "CHAOS", "OHFLOCK", "LIVE"};
}

std::string App::sceneNow() const {
    if (!world_ || (mode_ != Mode::Overview && mode_ != Mode::Ride)) return "";
    if (demo_ && pendingHost_ == net::Address{0x7F000001u, demo_->port()}) return demo_->scene() == DemoScene::Chaos ? "CHAOS" : "CALM";
    for (const auto& f : client_->found())
        if (f.addr == pendingHost_ && f.beacon.name == "ohflock") return "OHFLOCK";
    return "LIVE";
}

void App::openHost(const Found& h) {
    if (mode_ == Mode::Ride) leaveRide("");
    overlay_ = Overlay::None;
    const std::string title = h.beacon.title.empty() ? h.beacon.name : h.beacon.title;
    const auto known = paired_.find(h.addr.str());
    if (h.beacon.pairing && known == paired_.end()) {
        client_->disconnect();
        world_.reset();
        pendingHost_ = h.addr;
        connectingTo_ = title;
        code_.clear();
        mode_ = Mode::Keypad;
    } else {
        code_ = known != paired_.end() ? known->second : "";
        connectTo(h.addr, code_, title);
    }
}

void App::goScene(const std::string& name) {
    if (name == sceneNow()) return;
    if (name == "CALM" || name == "CHAOS") {
        const DemoScene want = name == "CHAOS" ? DemoScene::Chaos : DemoScene::Calm;
        if (!demo_) {
            demo_ = std::make_unique<DemoWorld>();
            std::string error;
            if (!demo_->start(&error, want)) {
                toast(upper(error));
                demo_.reset();
                return;
            }
        }
        demo_->setScene(want);
        const net::Address at{0x7F000001u, demo_->port()};
        if (pendingHost_ == at && (mode_ == Mode::Overview || mode_ == Mode::Ride)) {
            toast("SWITCHING TO " + name, 2); // the host re-sends the world; poll() takes it in
        } else {
            if (mode_ == Mode::Ride) leaveRide("");
            overlay_ = Overlay::None;
            connectTo(at, "", "THE CONSTRUCT: " + name);
        }
        return;
    }
    // OHFLOCK and LIVE run on a computer on the network.
    const auto found = client_->found();
    const Found* pick = nullptr;
    for (const auto& f : found) {
        const bool flock = f.beacon.name == "ohflock";
        if (name == "OHFLOCK" ? flock : (!flock && f.beacon.name != "construct")) {
            if (!pick || (name == "LIVE" && f.beacon.name == "gygax" && pick->beacon.name != "gygax")) pick = &f;
        }
    }
    if (!pick) {
        toast(name == "OHFLOCK" ? "NO OHFLOCK HOST FOUND: RUN  OHFLOCK --GLYDER  ON YOUR COMPUTER"
                                : "NO LIVE HOST FOUND: RUN  GYGAX SERVE --GLYDER  ON YOUR COMPUTER",
              6);
        return;
    }
    openHost(*pick);
}

// The four scenes side by side; true when one was picked.
bool App::sceneRow(const Input& in, Vec3 centre, Vec3 right, Vec3 up, float width, float halfHeight) {
    const std::string now = sceneNow();
    const float w = width / 4.0f;
    for (int k = 0; k < 4; ++k) {
        const std::string name = kScenes[k];
        const Vec3 bc = centre + right * (-width * 0.5f + w * (F(k) + 0.5f));
        if (button(in, "scene:" + name, bc, right, up, {w * 0.5f - 0.008f, halfHeight}, name, name == now) && name != now) {
            goScene(name);
            return true;
        }
    }
    return false;
}

void App::visionPanel(const Input& in) {
    const UiLayer layer(*this);
    Pose& at = visionAt_;
    bool& placed = visionPlaced_;
    if (!placed) {
        const Quat y = yawOnly(in.head.q);
        at = {in.head.pos + rotate(y, {0.0f, -0.1f, -0.8f}), y};
        placed = true;
    }
    if (!rxServer_) {
        rxServer_ = std::make_unique<RxServer>();
        std::string error;
        const bool started = rxServer_->start(
            kRxPort,
            [this](const std::string& text, bool& ok) {
                std::string err;
                const auto p = rx::parse(text, &err);
                ok = p.has_value();
                if (!p) return "Not understood: " + err;
                rx_ = *p;
                rxEnabled_ = true;
                applyRx();
                saveState();
                rxMessage_ = "PRESCRIPTION RECEIVED";
                return "Received. " + correction_.advice;
            },
            &error);
        if (!started && !rxServer_->start(0, nullptr, &error)) rxServer_.reset();
    }
    const Vec3 r = at.right(), u = at.up(), c = at.pos;
    const Vec3 nrm = normalize(cross(r, u)) * 0.003f;
    card(c, r, u, {0.32f, 0.22f}, kInk, 240, kCyan, 0.004f);
    Vec3 o = c - r * 0.3f + u * 0.18f + nrm;
    label("VISION", o, r, u, 0.024f, kCyan);
    label(rxEnabled_ ? "CORRECTION ON" : "CORRECTION OFF", o + r * 0.6f, r, u, 0.016f, rxEnabled_ ? kGreen : kDim, Align::Right);
    o -= u * 0.045f;
    std::istringstream lines(rx::format(rx_));
    std::string line;
    while (std::getline(lines, line)) {
        label(line, o, r, u, 0.016f, kPaper);
        o -= u * 0.026f;
    }
    // The advice, wrapped to the panel.
    std::string advice = upper(correction_.advice);
    while (!advice.empty()) {
        std::size_t cut = std::min<std::size_t>(advice.size(), 52);
        if (cut < advice.size()) {
            const std::size_t sp = advice.rfind(' ', cut);
            if (sp != std::string::npos && sp > 20) cut = sp;
        }
        label(advice.substr(0, cut), o, r, u, 0.012f, correction_.beyondSoftware ? kAmber : kDim);
        advice = advice.substr(std::min(advice.size(), cut + 1));
        o -= u * 0.02f;
    }
    if (rxServer_ && rxServer_->running()) {
        label("ON YOUR PHONE  " + rxServer_->url(), c - r * 0.3f - u * 0.1f + nrm, r, u, 0.014f, kCyan);
        label("PIN  " + rxServer_->pin(), c - r * 0.3f - u * 0.13f + nrm, r, u, 0.02f, kPaper);
    }
    const Vec3 b = c - u * 0.185f + nrm;
    if (button(in, "rx-onoff", b - r * 0.24f, r, u, {0.07f, 0.022f}, rxEnabled_ ? "ON" : "OFF", rxEnabled_)) {
        rxEnabled_ = !rxEnabled_;
        applyRx();
        saveState();
    }
    if (button(in, "rx-less", b - r * 0.12f, r, u, {0.04f, 0.022f}, "-")) {
        rxStrength_ = clamp(rxStrength_ - 0.1f, 0.0f, 1.0f);
        applyRx();
        saveState();
    }
    char sbuf[32];
    std::snprintf(sbuf, sizeof sbuf, "%.0f%%", static_cast<double>(rxStrength_ * 100));
    label(sbuf, b - r * 0.04f - u * 0.008f, r, u, 0.016f, kPaper, Align::Centre);
    if (button(in, "rx-more", b + r * 0.04f, r, u, {0.04f, 0.022f}, "+")) {
        rxStrength_ = clamp(rxStrength_ + 0.1f, 0.0f, 1.0f);
        applyRx();
        saveState();
    }
    if (button(in, "rx-chart", b + r * 0.15f, r, u, {0.06f, 0.022f}, "CHART", chart_)) chart_ = !chart_;
    if (button(in, "rx-close", b + r * 0.26f, r, u, {0.05f, 0.022f}, "DONE")) {
        overlay_ = Overlay::None;
        placed = false;
        chart_ = false;
        if (rxServer_) rxServer_->stop();
        rxServer_.reset();
    }
    if (chart_) {
        // A tumbling-E acuity chart 3 m away: lines from 20/80 down to 20/16 at the panel's distance.
        const Vec3 cc = at.pos + at.forward() * 2.4f + u * 0.25f;
        card(cc, r, u, {0.42f, 0.5f}, kPaper, 255, kPaper);
        const float dist = length(cc - in.head.pos);
        const float sizes[6] = {80, 50, 40, 30, 20, 16}; // 20/x: letter height 5 arcmin at 20/20
        float y = 0.42f;
        std::uint32_t seed = 7;
        for (float sz : sizes) {
            const float letter = dist * std::tan((5.0f / 60.0f) * kDeg * sz / 20.0f);
            const int n = sz >= 50 ? 3 : 5;
            for (int k = 0; k < n; ++k) {
                seed = hash32(seed + static_cast<std::uint32_t>(k));
                const int rot = static_cast<int>(seed % 4);
                const Vec3 ec = cc + r * ((F(k) - F(n - 1) * 0.5f) * letter * 1.9f) + u * y + normalize(cross(r, u)) * 0.002f;
                // An E of five bars: three arms and a spine, each a fifth of the letter.
                const Quat spin = axisAngle(normalize(cross(r, u)), F(rot) * kPi * 0.5f);
                const Vec3 rr = rotate(spin, r), uu = rotate(spin, u);
                render::PanelStyle st;
                st.fill = {0, 0, 0};
                st.fillAlpha = 255;
                st.radius = 0;
                const float bar = letter / 5.0f;
                render::addPanel(panelsOut(), ec - rr * (letter * 0.4f), rr, uu, {bar * 0.5f, letter * 0.5f}, st);
                for (int arm = 0; arm < 3; ++arm)
                    render::addPanel(panelsOut(), ec + uu * (F(arm - 1) * bar * 2.0f) + rr * (bar * 0.5f), rr, uu,
                                     {letter * 0.4f, bar * 0.5f}, st);
            }
            label("20/" + std::to_string(static_cast<int>(sz)), cc - r * 0.4f + u * (y - 0.01f) + normalize(cross(r, u)) * 0.002f, r, u,
                  0.018f, kDim);
            y -= std::max(letter * 1.9f, 0.08f);
        }
    }
    if (!rxMessage_.empty()) {
        toast(rxMessage_, 4);
        rxMessage_.clear();
    }
}

void App::controllers(const Input& in) {
    if (in.desktop) return;
    for (int h = 0; h < 2; ++h) {
        if (!in.hands[h].active) continue;
        const Pose& g = in.hands[h].aim;
        render::DrawItem d;
        d.mesh = static_cast<render::MeshId>(kit::Shape::Wand);
        // The wand is modelled along +x (FLU); the aim pose looks down -z.
        d.model = compose(g.pos, g.q) * rigidInverse(cameraInModel());
        d.tint = h == 1 ? kCyan : kPaper;
        d.emissive = 2.0f;
        items_.push_back(d);
        // A short beam when there is something to point at.
        if (h == 1 && mode_ != Mode::Ride && hovered_ == 0)
            (hot_.empty() ? glow_ : uiGlow_).push_back({g.pos, g.pos + g.forward() * (hot_.empty() ? 0.4f : 1.2f), 0.0015f, 0.6f, kCyan});
    }
}

void App::rain(const Input& in) {
    const UiLayer layer(*this);
    // The drop-in transition: the world dissolves into falling glyphs for a moment.
    const double age = in.time - rainAt_;
    const double life = 0.75;
    if (age < 0 || age > life) return;
    const float k = Fd(age / life);
    const float fade = 1.0f - k;
    const Pose& h = in.head;
    const char glyphs[] = "0123456789ABCDEFZX<>=:";
    for (int col = 0; col < 110; ++col) {
        const std::uint32_t hc = hash32(static_cast<std::uint32_t>(col) * 977u + 13u);
        const float a = F(col) * (2 * kPi / 110.0f) + F(static_cast<int>(hc % 100)) * 0.003f;
        const float speed = 1.6f + F(static_cast<int>(hc % 7)) * 0.35f;
        const float radius = 0.75f + F(static_cast<int>((hc >> 8) % 30)) * 0.01f;
        const Vec3 dir{std::sin(a), 0, -std::cos(a)};
        const Vec3 right{std::cos(a), 0, std::sin(a)};
        const float headY = h.pos.y + 0.9f - (rainOut_ ? 1.0f - k : k) * speed * 1.2f + F(static_cast<int>((hc >> 4) % 50)) * 0.01f;
        for (int g = 0; g < 9; ++g) {
            const float y = headY + F(g) * 0.05f;
            const char ch = glyphs[hash32(hc + static_cast<std::uint32_t>(g) + static_cast<std::uint32_t>(age * 14)) % (sizeof glyphs - 1)];
            const float bright = (g == 0 ? 2.2f : 1.0f - F(g) / 9.0f) * fade;
            std::vector<render::Line> tmp;
            render::text(tmp, std::string(1, ch), Vec3{h.pos.x, 0, h.pos.z} + dir * radius + Vec3{0, y, 0}, -right, {0, 1, 0}, 0.035f,
                         kGreen, Align::Centre, 0.12f);
            for (auto& l : tmp) {
                l.intensity = bright;
                glowOut().push_back(l);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Accessibility

void App::setColourVision(colour::Vision v, bool corrected, bool preview) {
    colour_ = v;
    colourCorrect_ = corrected;
    colourPreview_ = preview;
    if (v == colour::Vision::Typical)
        toast("COLOUR VISION: TYPICAL", 3);
    else
        toast("COLOUR VISION: " + std::string(colour::name(v)) + (corrected ? "  CORRECTED" : "  UNCORRECTED") +
                  (preview ? "  (PREVIEW: AS THIS VIEWER SEES IT)" : ""),
              4);
    saveState();
}

void App::setAudioCues(bool on) {
    cues_ = on;
    if (on) earcon(659.25f, 987.77f);
    saveState();
}

void App::setBlindMode(bool on) {
    blind_ = on;
    if (on) earcon(523.25f, 1046.5f, 0.6f);
    toast(on ? "BLIND MODE: LISTEN. POINT TO HEAR A NODE; TRIGGER TO DROP IN" : "BLIND MODE OFF", 4);
    saveState();
}

void App::setBaseMode(bool on) {
    base_ = on;
    if (on) ar_ = false;
    earcon(on ? 440.0f : 330.0f, on ? 660.0f : 440.0f, 0.5f);
    toast(on ? "BASE MODE: PLAIN VIEW, SOUND FIRST. VISION CORRECTION AND COLOUR FILTERS STAY ON" : "BASE MODE OFF", 4);
    saveState();
}

void App::setArMode(bool on) {
    ar_ = on;
    if (on) base_ = false;
    toast(on ? "AR MODE: PASSTHROUGH AND OLIPHAUNT OVERLAY" : "AR MODE OFF", 4);
    saveState();
}

void App::oliphauntPanel(const Input& in) {
    const UiLayer layer(*this);
    const Quat yaw = yawOnly(in.head.q);
    const Vec3 right = rotate(yaw, {1, 0, 0});
    const Vec3 up = rotate(yaw, {0, 1, 0});
    const Vec3 centre = in.head.pos + rotate(yaw, {0, -0.16f, -0.82f});
    const Vec3 normal = normalize(cross(right, up)) * 0.003f;
    const float halfHeight = 0.12f + 0.045f * F(static_cast<int>(std::min<std::size_t>(oliphauntCards_.size(), 3)));
    card(centre, right, up, {0.31f, halfHeight}, kInk, 225, kCyan, 0.003f);
    Vec3 at = centre + up * (halfHeight - 0.035f) + normal;
    label("OLIPHAUNT  |  NEVER FORGETS", at - right * 0.28f, right, up, 0.016f, kCyan);
    at -= up * 0.045f;
    if (button(in, "oliphaunt-report", at, right, up, {0.28f, 0.017f}, "WHO'S FOLLOWING ME?  CHECK MEMORY")) {
        client_->order("oliphaunt report");
        toast("OLIPHAUNT REPORT REQUESTED", 3);
    }
    at -= up * 0.047f;
    const std::size_t count = std::min<std::size_t>(oliphauntCards_.size(), 3);
    for (std::size_t i = 0; i < count; ++i) {
        const OliphauntCard& finding = oliphauntCards_[i];
        const std::string summary = finding.text.size() > 58 ? finding.text.substr(0, 55) + "..." : finding.text;
        label(summary, at - right * 0.28f, right, up, 0.013f, kPaper);
        const Vec3 trustAt = at + right * 0.13f - up * 0.025f;
        if (button(in, "oliphaunt-trust:" + finding.id, trustAt, right, up, {0.07f, 0.014f}, "TRUST")) {
            client_->order("oliphaunt trust " + finding.id);
            toast("TRUST REQUESTED FOR " + finding.id, 3);
        }
        const Vec3 forgetAt = at + right * 0.24f - up * 0.025f;
        if (button(in, "oliphaunt-forget:" + finding.id, forgetAt, right, up, {0.065f, 0.014f}, "FORGET")) {
            client_->order("oliphaunt forget " + finding.id);
            toast("FORGET REQUESTED FOR " + finding.id, 3);
        }
        at -= up * 0.052f;
    }
}

void App::earcon(float hz, float hz2, float gain) {
    mixer_.note(audio::Cue::Event, hz, gain, 0, hz2);
}

void App::audio(std::span<float> out) {
    mixer_.render(out);
}

void App::listen(const Input& in) {
    if (!audioCues() || !haveHeard_ || !world_) {
        mixer_.update({});
        return;
    }
    const Pose& head = in.head;
    const Vec3 right = head.right(), up = head.up(), back = -head.forward();
    // Riding, you hear about 4 km around you; on the map table, the whole table (nearer is louder).
    const float audible = mode_ == Mode::Overview ? 1.6f : 4000.0f * heardScale_;
    const wire::EntityState* me = nullptr;
    if (mode_ == Mode::Ride)
        for (const auto& e : entities_)
            if (e.id == ride_.entity) me = &e;
    const Vec3 listenerW = me ? me->pos : focus_;
    const Vec3 listenerV = me ? me->vel : Vec3{};

    std::vector<audio::Source> sources;
    auto place = [&](audio::Source& s, Vec3 worldPos) {
        const Vec3 rel = heard_.transformPoint(worldPos) - head.pos;
        const float d = std::max(length(rel), 1e-6f);
        s.direction = {dot(rel, right) / d, dot(rel, up) / d, dot(rel, back) / d};
        s.distance = d;
        s.audible = audible;
        return d;
    };
    for (const auto& e : entities_) {
        if (me && e.id == me->id) continue;
        const auto shape = static_cast<kit::Shape>(std::min<std::uint16_t>(e.shape, static_cast<std::uint16_t>(kit::Shape::Count) - 1));
        const kit::Domain dom = kit::domain(shape);
        if (dom == kit::Domain::Glyph || dom == kit::Domain::Space) continue;
        audio::Source s;
        s.id = e.id;
        if (place(s, e.pos) > audible) continue;
        const Vec3 toW = e.pos - listenerW;
        const float worldDist = length(toW);
        const Vec3 dirW = worldDist > 1e-3f ? toW / worldDist : Vec3{0, 0, 1};
        s.closing = -dot(e.vel - listenerV, dirW);
        s.timeToGo = s.closing > 1 ? worldDist / s.closing : -1;
        const bool kinetic = shape == kit::Shape::Missile || shape == kit::Shape::Bomb;
        const bool ours = e.flags & wire::kDrivable;
        if (kinetic || (!ours && s.closing > 150))
            s.cue = audio::Cue::Threat;
        else if (ours)
            s.cue = audio::Cue::Friend;
        else if (dom == kit::Domain::Air)
            s.cue = audio::Cue::Air;
        else if (dom == kit::Domain::Structure) {
            if (worldDist > 60) continue;
            s.cue = audio::Cue::Structure;
        } else
            s.cue = audio::Cue::Ground;
        sources.push_back(s);
    }
    // Most salient first: what is coming soonest, then what is nearest. A dozen at most, so it
    // stays a scene and not a din; your own side is limited to the nearest few.
    std::sort(sources.begin(), sources.end(), [](const audio::Source& a, const audio::Source& b) {
        const bool at = a.cue == audio::Cue::Threat, bt = b.cue == audio::Cue::Threat;
        if (at != bt) return at;
        if (at) return (a.timeToGo < 0 ? 1e9f : a.timeToGo) < (b.timeToGo < 0 ? 1e9f : b.timeToGo);
        return a.distance < b.distance;
    });
    std::vector<audio::Source> kept;
    int friends = 0;
    for (const auto& s : sources) {
        if (kept.size() >= 12) break;
        if (s.cue == audio::Cue::Friend && ++friends > 3) continue;
        kept.push_back(s);
    }
    // The ground below an aircraft you ride: higher and more insistent as it gets closer.
    if (me && terrain_ && kit::domain(ride_.shape) == kit::Domain::Air) {
        const float agl = me->pos.z - terrain_->sample(me->pos.x, me->pos.y);
        if (agl < 300) {
            audio::Source t;
            t.id = 0xFFFFFFF0u;
            t.cue = audio::Cue::Terrain;
            t.direction = {0, -1, 0};
            t.distance = std::max(agl, 0.0f);
            t.audible = 400;
            t.pitch = 1.0f + 2.0f * (1.0f - std::max(agl, 0.0f) / 300.0f);
            kept.push_back(t);
        }
    }
    // Blind mode: the node you point at sounds, so you can scan by pointing.
    if (blind_ && hovered_) {
        for (const auto& e : entities_)
            if (e.id == hovered_) {
                audio::Source f;
                f.id = 0xFFFFFFF1u;
                f.cue = audio::Cue::Focus;
                place(f, e.pos);
                f.distance = 0;
                kept.push_back(f);
            }
        if (hovered_ != heardFocus_) haptics_[1] = {0.25f, 0.015f};
    }
    heardFocus_ = blind_ ? hovered_ : 0;
    // Anything arriving within two seconds: both controllers pulse.
    for (const auto& s : kept)
        if (s.cue == audio::Cue::Threat && s.timeToGo >= 0 && s.timeToGo < 2.0f) haptics_[0] = haptics_[1] = {0.6f, 0.05f};
    mixer_.update(kept);
}

} // namespace glyder
