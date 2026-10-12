// The experience without a GPU or a headset: prescriptions, and the app's whole journey from the
// lobby into a node and out again, driven the way a headset drives it.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <thread>

#include <glyder/app.hpp>
#include <glyder/rx.hpp>
#include <glyder/rxserver.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace glyder;

TEST(GlyderRx, ParsesPrescriptionsAsWritten) {
    const auto p = rx::parse("OD -1.25 -0.50 x 180 1.0 BI\nOS: SPH -1.00 CYL -0.75 AXIS 90 PRISM 0.5 BU\nPD 63\nADD +2.00\nAGE 52\n");
    ASSERT_TRUE(p);
    EXPECT_FLOAT_EQ(p->right.sphere, -1.25f);
    EXPECT_FLOAT_EQ(p->right.cylinder, -0.5f);
    EXPECT_FLOAT_EQ(p->right.axis, 180);
    EXPECT_FLOAT_EQ(p->right.prismOut, -1.0f); // base in
    EXPECT_FLOAT_EQ(p->left.axis, 90);
    EXPECT_FLOAT_EQ(p->left.prismUp, 0.5f);
    EXPECT_FLOAT_EQ(p->pd, 63);
    EXPECT_FLOAT_EQ(p->add, 2);
    EXPECT_EQ(p->age, 52);
    // Plus cylinder is transposed to minus cylinder: +1.00 +0.50 x 90 is +1.50 -0.50 x 180.
    const auto plus = rx::parse("R +1.00 +0.50 x 90\nL PLANO");
    ASSERT_TRUE(plus);
    EXPECT_FLOAT_EQ(plus->right.sphere, 1.5f);
    EXPECT_FLOAT_EQ(plus->right.cylinder, -0.5f);
    EXPECT_FLOAT_EQ(plus->right.axis, 180);
    // Compact forms.
    const auto compact = rx::parse("od -2.00-0.75X45\nos -2.25 DS\nPD 31.5 32");
    ASSERT_TRUE(compact);
    EXPECT_FLOAT_EQ(compact->right.cylinder, -0.75f);
    EXPECT_FLOAT_EQ(compact->right.axis, 45);
    EXPECT_FLOAT_EQ(compact->pd, 63.5f);
    // A round trip through the canonical form.
    const auto again = rx::parse(rx::format(*p));
    ASSERT_TRUE(again);
    EXPECT_FLOAT_EQ(again->left.prismUp, 0.5f);
    EXPECT_FLOAT_EQ(again->right.prismOut, -1.0f);
    // And refusals that say why.
    std::string why;
    EXPECT_FALSE(rx::parse("OD -1.00 x 400", &why));
    EXPECT_NE(why.find("axis"), std::string::npos);
    EXPECT_FALSE(rx::parse("hello", &why));
    EXPECT_FALSE(rx::parse("OD -1.00 1.0", &why));
    EXPECT_FALSE(rx::parse("OD -40", &why));
}

TEST(GlyderRx, PrismTurnsEachEyeTowardTheApex) {
    rx::Prescription p;
    p.right.prismOut = 2.0f; // base out on the right eye: base to the right, image moves left
    p.left.prismOut = 2.0f;  // and on the left eye the image moves right
    p.right.prismUp = 1.0f;  // base up: image moves down
    const auto c = rx::compute(p);
    const float a = std::atan(0.02f);
    // Content moves right when the view turns left (positive yaw).
    EXPECT_NEAR(c.eye[1].yaw, -a, 1e-6f);
    EXPECT_NEAR(c.eye[0].yaw, a, 1e-6f);
    // Content moves down when the view turns up (positive pitch).
    EXPECT_NEAR(c.eye[1].pitch, std::atan(0.01f), 1e-6f);
    EXPECT_NEAR(c.eye[0].pitch, 0.0f, 1e-9f);
}

TEST(GlyderRx, BlurFollowsTheOpticsAtTheHeadsetsFocus) {
    rx::Display d;
    // An emmetrope with accommodation to spare sees the 1.3 m image sharply.
    rx::Prescription none;
    none.age = 30;
    EXPECT_NEAR(rx::compute(none, d).eye[1].blurMajor, 0.0f, 1e-6f);
    EXPECT_FALSE(rx::compute(none, d).filter);
    // A -0.75 D myope's far point is 1.33 m: sharp without help.
    rx::Prescription m075;
    m075.right.sphere = m075.left.sphere = -0.75f;
    EXPECT_LT(rx::compute(m075, d).eye[1].blurMajor, 0.0002f);
    // A -2.00 D myope cannot relax beyond 0.5 m: 1.23 D of defocus, a 5.5 mrad blur with a 4.5 mm pupil.
    rx::Prescription m2;
    m2.right.sphere = m2.left.sphere = -2.0f;
    const auto c2 = rx::compute(m2, d);
    EXPECT_NEAR(c2.eye[1].blurMajor, 0.0045f * (2.0f - 1.0f / 1.3f), 1e-5f);
    EXPECT_TRUE(c2.filter);
    EXPECT_FALSE(c2.beyondSoftware);
    // A presbyope with an ADD cannot accommodate to 1.3 m.
    rx::Prescription presby;
    presby.add = 2.5f;
    EXPECT_GT(rx::compute(presby, d).eye[1].blurMajor, 0.003f);
    // Myopic astigmatism: the eye cannot relax further, so the stronger meridian (S + C, at axis +
    // 90) stays out of focus and the image blurs along it.
    rx::Prescription astig;
    astig.right.sphere = -0.75f;
    astig.right.cylinder = -1.5f;
    astig.right.axis = 30;
    const auto ca = rx::compute(astig, d);
    EXPECT_GT(ca.eye[1].blurMajor, ca.eye[1].blurMinor * 10.0f);
    EXPECT_NEAR(ca.eye[1].axis, 120.0f * kDeg, 1e-5f);
    // Hyperopic astigmatism with accommodation to spare: the eye focuses between the meridians.
    rx::Prescription hyper;
    hyper.age = 25;
    hyper.right.sphere = 1.0f;
    hyper.right.cylinder = -1.0f;
    const auto ch = rx::compute(hyper, d);
    EXPECT_NEAR(ch.eye[1].blurMajor, ch.eye[1].blurMinor, 1e-6f);
    EXPECT_NEAR(ch.eye[1].blurMajor, 0.0045f * 0.5f, 1e-5f);
    // -4 D is beyond software: the advice says so.
    rx::Prescription strong;
    strong.right.sphere = strong.left.sphere = -4.0f;
    const auto cs = rx::compute(strong, d);
    EXPECT_TRUE(cs.beyondSoftware);
    EXPECT_NE(cs.advice.find("spacer"), std::string::npos);
    // The PD is applied exactly.
    rx::Prescription pd;
    pd.pd = 58;
    EXPECT_FLOAT_EQ(rx::compute(pd).ipd, 0.058f);
}

namespace {
Input headsetInput(double t, float dt) {
    Input in;
    in.time = t;
    in.dt = dt;
    in.head = {{0, 1.6f, 0}, Quat{}};
    for (int h = 0; h < 2; ++h) {
        in.hands[h].active = true;
        in.hands[h].aim = {{h == 0 ? -0.2f : 0.2f, 1.3f, -0.3f}, Quat{}};
        in.hands[h].grip = in.hands[h].aim;
    }
    return in;
}
} // namespace

TEST(GlyderExperience, FromTheLobbyIntoANodeAndOut) {
    AppOptions o;
    o.demo = true; // the Construct: a real host on loopback, so this is the true path end to end
    App app(o);
    EyeView eyes[2];
    eyes[0].pose = {{-0.032f, 1.6f, 0}, Quat{}};
    eyes[1].pose = {{0.032f, 1.6f, 0}, Quat{}};
    const double t0 = net::now();
    auto run = [&](double seconds, const std::function<bool()>& until) {
        const double end = net::now() + seconds;
        while (net::now() < end) {
            const double t = net::now();
            const auto f = app.frame(headsetInput(t, 0.011f), eyes, 1832, 1920);
            EXPECT_EQ(f.scene.views[0].eye.x, f.scene.views[0].eye.x); // finite
            if (until()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        return until();
    };
    ASSERT_TRUE(run(5.0, [&] { return app.mode() == "overview"; })) << app.mode();
    // The world arrives: nodes, scenery, the effects.
    std::size_t items = 0, glow = 0;
    ASSERT_TRUE(run(5.0,
                    [&] {
                        const auto f = app.frame(headsetInput(net::now(), 0.011f), eyes, 1832, 1920);
                        items = f.items.size();
                        glow = f.glow.size();
                        return items > 15 && glow > 0;
                    }))
        << items << " items, " << glow << " glow";

    // Drop into the rover (node 1): the view switches at once, control follows at a stopping point.
    app.select(1);
    EXPECT_EQ(app.riding(), 1u);
    EXPECT_TRUE(app.mode() == "riding" || app.mode() == "control");
    ASSERT_TRUE(run(6.0, [&] { return app.mode() == "control"; })) << app.mode();

    // B jacks out instantly.
    Input b = headsetInput(net::now(), 0.011f);
    b.hands[1].secondary = true;
    app.frame(b, eyes);
    EXPECT_EQ(app.mode(), "overview");
    EXPECT_EQ(app.riding(), 0u);
    (void)t0;
}

namespace {
std::string httpPost(std::uint16_t port, const std::string& body) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        ::close(fd);
        return {};
    }
    const std::string req = "POST /rx HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body;
    ::send(fd, req.data(), req.size(), 0);
    std::string out;
    char buf[4096];
    for (ssize_t n; (n = ::recv(fd, buf, sizeof buf, 0)) > 0;) out.append(buf, static_cast<std::size_t>(n));
    ::close(fd);
    return out;
}
} // namespace

TEST(GlyderRx, PhonePageTakesAPrescriptionOnlyWithThePin) {
    RxServer server;
    std::string received;
    std::string error;
    ASSERT_TRUE(server.start(
        0,
        [&](const std::string& text, bool& ok) {
            received = text;
            ok = rx::parse(text).has_value();
            return ok ? std::string("Received.") : std::string("Not understood.");
        },
        &error))
        << error;
    const auto url = server.url();
    const std::uint16_t port = static_cast<std::uint16_t>(std::stoi(url.substr(url.rfind(':') + 1)));
    // The wrong PIN is refused and nothing reaches the headset.
    const auto wrong = httpPost(port, "pin=0000&od_sph=-1.25");
    EXPECT_NE(wrong.find("403"), std::string::npos);
    EXPECT_TRUE(received.empty());
    // The form, decoded: + for spaces, %XX escapes, per-field and pasted forms.
    const auto ok = httpPost(port, "pin=" + server.pin() + "&od_sph=-1.25&od_cyl=-0.50&od_ax=180&od_pr=1&od_base=BI&os_sph=%2D1.00&pd=63");
    EXPECT_NE(ok.find("200 OK"), std::string::npos);
    EXPECT_NE(ok.find("Received."), std::string::npos);
    const auto p = rx::parse(received);
    ASSERT_TRUE(p) << received;
    EXPECT_FLOAT_EQ(p->right.cylinder, -0.5f);
    EXPECT_FLOAT_EQ(p->right.prismOut, -1.0f);
    EXPECT_FLOAT_EQ(p->left.sphere, -1.0f);
    const auto pasted = httpPost(port, "pin=" + server.pin() + "&text=OD+-2.00+-0.75+x+45%0AOS+-2.25+DS");
    EXPECT_NE(pasted.find("200 OK"), std::string::npos);
    EXPECT_EQ(received, "OD -2.00 -0.75 x 45\nOS -2.25 DS");
    // The page escapes what it echoes.
    EXPECT_EQ(htmlEscape("<b>&\"'"), "&lt;b&gt;&amp;&quot;&#39;");
    server.stop();
}
