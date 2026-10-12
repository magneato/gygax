#include <glyder/rx.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <vector>

namespace glyder::rx {

namespace {

constexpr float kPi = 3.14159265358979f;

std::string upper(std::string_view s) {
    std::string u(s);
    for (char& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return u;
}

std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) out.push_back(cur);
        cur.clear();
    };
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (std::isspace(static_cast<unsigned char>(c)) || c == ':' || c == ',' || c == '/' || c == '=') {
            flush();
        } else if ((c == '-' || c == '+') && !cur.empty() && (std::isdigit(static_cast<unsigned char>(cur.back())) || cur.back() == '.')) {
            flush(); // "-2.00-0.75": a sign after a number starts the next one
            cur += c;
        } else if (c == 'X' && !cur.empty() && (std::isdigit(static_cast<unsigned char>(cur.back())) || cur.back() == '.')) {
            flush(); // "-0.50X180"
            out.push_back("X");
        } else {
            cur += c;
        }
    }
    flush();
    return out;
}

bool number(const std::string& t, float& v) {
    if (t == "PL" || t == "PLANO" || t == "DS" || t == "SPH" || t.empty()) return false;
    char* end = nullptr;
    v = std::strtof(t.c_str(), &end);
    return end && *end == '\0';
}

void normalise(Eye& e) {
    // Plus cylinder to minus cylinder: sphere + cyl, -cyl, axis +- 90.
    if (e.cylinder > 0) {
        e.sphere += e.cylinder;
        e.cylinder = -e.cylinder;
        e.axis = e.axis > 90 ? e.axis - 90 : e.axis + 90;
    }
    if (e.axis <= 0) e.axis += 180;
    if (e.axis > 180) e.axis -= 180;
}

} // namespace

bool Prescription::empty() const {
    auto zero = [](const Eye& e) { return e.sphere == 0 && e.cylinder == 0 && e.prismOut == 0 && e.prismUp == 0; };
    return zero(right) && zero(left);
}

std::optional<Prescription> parse(std::string_view text, std::string* error) {
    Prescription p;
    bool any = false;
    std::istringstream in{std::string(text)};
    std::string raw;
    int lineNo = 0;
    while (std::getline(in, raw)) {
        ++lineNo;
        if (const auto hash = raw.find('#'); hash != std::string::npos) raw.resize(hash);
        const auto t = tokens(upper(raw));
        if (t.empty()) continue;
        std::size_t i = 0;
        const std::string& head = t[0];
        auto fail = [&](const std::string& why) {
            if (error) *error = "line " + std::to_string(lineNo) + ": " + why;
            return std::nullopt;
        };
        float v = 0;
        if (head == "PD") {
            if (t.size() < 2 || !number(t[1], v)) return fail("PD needs a distance in mm");
            float v2 = 0;
            if (v > 20 && v < 45 && t.size() >= 3 && number(t[2], v2) && v2 > 20 && v2 < 45) v += v2; // "PD 31.5 32": per eye
            if (v < 45 || v > 80) return fail("PD should be 45-80 mm (or two per-eye halves)");
            p.pd = v;
            any = true;
            continue;
        }
        if (head == "ADD") {
            if (t.size() < 2 || !number(t[1], v) || v < 0 || v > 4) return fail("ADD should be 0 to +4.00");
            p.add = v;
            continue;
        }
        if (head == "AGE") {
            if (t.size() < 2 || !number(t[1], v) || v < 5 || v > 110) return fail("AGE should be in years");
            p.age = static_cast<int>(v);
            continue;
        }
        Eye* eye = nullptr;
        if (head == "OD" || head == "R" || head == "RIGHT" || head == "RE") eye = &p.right;
        if (head == "OS" || head == "L" || head == "LEFT" || head == "LE") eye = &p.left;
        if (head == "OU" || head == "BOTH") eye = &p.right;
        if (!eye) return fail("expected OD, OS, OU, PD, ADD or AGE, not '" + head + "'");
        const bool both = head == "OU" || head == "BOTH";
        Eye e;
        int positional = 0;
        bool axisGiven = false;
        for (i = 1; i < t.size(); ++i) {
            const std::string& k = t[i];
            auto next = [&](float& out) { return i + 1 < t.size() && number(t[i + 1], out) ? (++i, true) : false; };
            if (k == "SPH" || k == "S") {
                if (!next(e.sphere)) return fail("SPH needs a value");
                positional = std::max(positional, 1);
            } else if (k == "CYL" || k == "C") {
                if (!next(e.cylinder)) return fail("CYL needs a value");
                positional = std::max(positional, 2);
            } else if (k == "X" || k == "AXIS" || k == "AX") {
                if (!next(e.axis) || e.axis < 0 || e.axis > 180) return fail("axis must be 0-180 degrees");
                positional = 3;
                axisGiven = true;
            } else if (k == "PRISM" || k == "P" || k == "PR") {
                float amount = 0;
                if (!next(amount)) return fail("PRISM needs a value");
                if (i + 1 >= t.size()) return fail("PRISM needs a base: BI, BO, BU or BD");
                const std::string& base = t[++i];
                if (base == "BI" || base == "IN")
                    e.prismOut -= amount;
                else if (base == "BO" || base == "OUT")
                    e.prismOut += amount;
                else if (base == "BU" || base == "UP")
                    e.prismUp += amount;
                else if (base == "BD" || base == "DOWN")
                    e.prismUp -= amount;
                else
                    return fail("unknown prism base '" + base + "'");
            } else if (k == "BI" || k == "BO" || k == "BU" || k == "BD") {
                return fail("a prism base needs an amount before it (e.g. 1.0 " + k + ")");
            } else if (k == "PL" || k == "PLANO" || k == "DS" || k == "SPH.") {
                positional = std::max(positional, 1);
            } else if (number(k, v)) {
                // Positional: sphere, cylinder, axis, then a prism amount followed by its base.
                if (i + 1 < t.size() && (t[i + 1] == "BI" || t[i + 1] == "BO" || t[i + 1] == "BU" || t[i + 1] == "BD")) {
                    const std::string& base = t[++i];
                    if (base == "BI") e.prismOut -= v;
                    if (base == "BO") e.prismOut += v;
                    if (base == "BU") e.prismUp += v;
                    if (base == "BD") e.prismUp -= v;
                } else if (positional == 0) {
                    e.sphere = v;
                    positional = 1;
                } else if (positional == 1) {
                    e.cylinder = v;
                    positional = 2;
                } else if (positional == 2) {
                    if (v < 0 || v > 180) return fail("axis must be 0-180 degrees");
                    e.axis = v;
                    positional = 3;
                    axisGiven = true;
                } else {
                    return fail("unexpected number '" + k + "'");
                }
            } else {
                return fail("unexpected '" + k + "'");
            }
        }
        if (e.cylinder != 0 && !axisGiven) return fail("a cylinder needs its axis (e.g. -0.50 x 180)");
        if (std::fabs(e.sphere) > 20 || std::fabs(e.cylinder) > 8 || std::fabs(e.prismOut) > 15 || std::fabs(e.prismUp) > 15)
            return fail("values out of range for a spectacle prescription");
        normalise(e);
        *eye = e;
        if (both) p.left = e;
        any = true;
    }
    if (!any) {
        if (error) *error = "no prescription found (expected lines like 'OD -1.25 -0.50 x 180')";
        return std::nullopt;
    }
    return p;
}

std::string format(const Prescription& p) {
    auto eye = [](const char* name, const Eye& e) {
        char buf[160];
        int n = std::snprintf(buf, sizeof buf, "%s %+.2f %+.2f x %03.0f", name, static_cast<double>(e.sphere),
                              static_cast<double>(e.cylinder), static_cast<double>(e.axis));
        if (e.prismOut != 0)
            n += std::snprintf(buf + n, sizeof buf - static_cast<std::size_t>(n), " %.2f %s", static_cast<double>(std::fabs(e.prismOut)),
                               e.prismOut > 0 ? "BO" : "BI");
        if (e.prismUp != 0)
            std::snprintf(buf + n, sizeof buf - static_cast<std::size_t>(n), " %.2f %s", static_cast<double>(std::fabs(e.prismUp)),
                          e.prismUp > 0 ? "BU" : "BD");
        return std::string(buf);
    };
    std::string s = eye("OD", p.right) + "\n" + eye("OS", p.left) + "\n";
    char buf[64];
    std::snprintf(buf, sizeof buf, "PD %.1f\n", static_cast<double>(p.pd));
    s += buf;
    if (p.add > 0) {
        std::snprintf(buf, sizeof buf, "ADD %+.2f\n", static_cast<double>(p.add));
        s += buf;
    }
    if (p.age > 0) s += "AGE " + std::to_string(p.age) + "\n";
    return s;
}

float gainFor(float sigmaPixels, float strength) {
    return sigmaPixels < 0.35f ? 0.0f : std::min(2.4f, 1.6f * strength * std::min(sigmaPixels, 2.5f));
}

float accommodation(const Prescription& p) {
    if (p.add > 0) return std::max(0.0f, 2.5f - p.add); // a reading addition implies little left
    if (p.age > 0) return std::max(0.0f, 18.5f - 0.3f * static_cast<float>(p.age));
    return 4.0f; // unknown: assume a moderate amplitude
}

Correction compute(const Prescription& p, const Display& d, float strength) {
    Correction c;
    c.ipd = p.pd / 1000.0f;
    const float amp = accommodation(p);
    const float vergence = -1.0f / d.focusDistance; // light from the virtual image diverges
    const float pupil = d.pupil / 1000.0f;
    const float pxPerRad = d.pixelsPerDegree * 180.0f / kPi;
    float worst = 0;
    for (int k = 0; k < 2; ++k) {
        const Eye& e = k == 0 ? p.left : p.right;
        EyeCorrection& ec = c.eye[k];
        // Prism: image displaced toward the apex. Horizontal base out on the right eye is a base to
        // the right (temporal), so the image moves left; on the left eye it moves right.
        const float h = std::atan(e.prismOut / 100.0f), v = std::atan(e.prismUp / 100.0f);
        const float shiftRight = k == 1 ? -h : h;
        const float shiftUp = -v;
        // Content moves right when the view turns left (positive yaw); up when it turns down.
        ec.yaw = shiftRight;
        ec.pitch = -shiftUp;

        // Defocus in the two principal meridians: powers S (at the axis) and S + C (at axis + 90).
        // An eye with refraction P sees sharply at vergence P - A for accommodation A.
        const float p1 = e.sphere, p2 = e.sphere + e.cylinder;
        const float a = std::clamp(-vergence + (p1 + p2) * 0.5f, 0.0f, amp);
        const float d1 = vergence - (p1 - a), d2 = vergence - (p2 - a);
        // Rays in a meridian that is out of focus spread along that meridian: the axis meridian
        // (power S) blurs along the axis, the other (S + C) across it.
        const float b1 = pupil * std::fabs(d1), b2 = pupil * std::fabs(d2);
        const float axisRad = e.axis * kPi / 180.0f;
        if (b1 >= b2) {
            ec.blurMajor = b1;
            ec.blurMinor = b2;
            ec.axis = axisRad;
        } else {
            ec.blurMajor = b2;
            ec.blurMinor = b1;
            ec.axis = axisRad + kPi / 2;
        }
        // Pre-compensation: a uniform disc of diameter D has about the Gaussian sigma D/4. The
        // gain sharpens most where the eye's blur halves contrast; strength scales it, and it is
        // capped so the image keeps its tonal range.
        auto sigma = [&](float b) { return b * pxPerRad / 4.0f; };
        auto gain = [&](float s) { return gainFor(s, strength); };
        ec.sigmaMajor = sigma(ec.blurMajor);
        ec.sigmaMinor = sigma(ec.blurMinor);
        ec.gainMajor = gain(ec.sigmaMajor);
        ec.gainMinor = gain(ec.sigmaMinor);
        if (ec.gainMajor > 0 || ec.gainMinor > 0) c.filter = true;
        worst = std::max({worst, std::fabs(d1), std::fabs(d2)});
    }
    char buf[200];
    if (worst > 1.5f) {
        c.beyondSoftware = true;
        std::snprintf(buf, sizeof buf,
                      "%.1f D of blur is more than software can undo: wear your glasses with the headset's spacer or fit lens inserts. "
                      "PD and prism are still applied exactly.",
                      static_cast<double>(worst));
    } else if (c.filter) {
        std::snprintf(buf, sizeof buf, "Up to %.2f D of blur at the headset's focus: pre-compensated. Tune the strength on the chart.",
                      static_cast<double>(worst));
    } else {
        std::snprintf(buf, sizeof buf, "Your eyes focus the headset's image without help; PD and prism applied exactly.");
    }
    c.advice = buf;
    return c;
}

} // namespace glyder::rx
