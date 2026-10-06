#include <gygax/robotics/urdf.hpp>

#include <gygax/math/linalg.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <set>
#include <sstream>

namespace gygax::robotics {

namespace {

constexpr int kMaximumXmlNestingDepth = 64;
constexpr double kJointAxisZeroTolerance = 1e-9;
constexpr double kInverseKinematicsFiniteDifferenceStep = 1e-6;
constexpr std::size_t kSpatialJacobianRows = 6;
constexpr std::size_t kPositionJacobianRows = 3;

struct XmlNode {
    std::string tag;
    std::map<std::string, std::string> attrs;
    std::vector<std::unique_ptr<XmlNode>> children;

    [[nodiscard]] const XmlNode* child(std::string_view name) const {
        for (const auto& c : children)
            if (c->tag == name) return c.get();
        return nullptr;
    }
    [[nodiscard]] std::string attr(const std::string& name, const std::string& fallback = {}) const {
        const auto it = attrs.find(name);
        return it == attrs.end() ? fallback : it->second;
    }
};

class XmlParser {
public:
    XmlParser(std::string_view text, std::string* error) : s_(text), error_(error) {}

    std::unique_ptr<XmlNode> parse() {
        std::unique_ptr<XmlNode> root;
        for (;;) {
            skipText();
            if (pos_ >= s_.size()) break;
            if (!skipMisc()) {
                if (failed_) return nullptr;
                if (root) return fail("multiple root elements");
                root = element();
                if (!root) return nullptr;
            }
        }
        if (!root) return fail("no root element");
        return root;
    }

private:
    std::unique_ptr<XmlNode> fail(const std::string& message) {
        if (!failed_ && error_) *error_ = "xml: " + message;
        failed_ = true;
        return nullptr;
    }

    void skipText() {
        while (pos_ < s_.size() && s_[pos_] != '<') ++pos_;
    }

    void skipSpace() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }

    bool startsWith(std::string_view p) const { return s_.substr(pos_, p.size()) == p; }

    bool skipUntil(std::string_view end) {
        const auto at = s_.find(end, pos_);
        if (at == std::string_view::npos) {
            fail("unterminated construct");
            return true;
        }
        pos_ = at + end.size();
        return true;
    }

    bool skipMisc() {
        if (startsWith("<!--")) return skipUntil("-->");
        if (startsWith("<?")) return skipUntil("?>");
        if (startsWith("<![CDATA[")) return skipUntil("]]>");
        if (startsWith("<!")) return skipUntil(">");
        return false;
    }

    static bool nameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ':' || c == '.'; }

    std::string name() {
        const auto start = pos_;
        while (pos_ < s_.size() && nameChar(s_[pos_])) ++pos_;
        return std::string(s_.substr(start, pos_ - start));
    }

    static std::string unescape(std::string v) {
        static const std::pair<const char*, char> table[] = {
            {"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}};
        for (const auto& [from, to] : table) {
            for (std::size_t at = v.find(from); at != std::string::npos; at = v.find(from, at + 1)) v.replace(at, std::strlen(from), 1, to);
        }
        return v;
    }

    std::unique_ptr<XmlNode> element() {
        if (++depth_ > kMaximumXmlNestingDepth) return fail("nesting too deep");
        ++pos_;
        auto node = std::make_unique<XmlNode>();
        node->tag = name();
        if (node->tag.empty()) return fail("missing tag name");
        for (;;) {
            skipSpace();
            if (pos_ >= s_.size()) return fail("unexpected end inside <" + node->tag + ">");
            if (startsWith("/>")) {
                pos_ += 2;
                --depth_;
                return node;
            }
            if (s_[pos_] == '>') {
                ++pos_;
                break;
            }
            const auto key = name();
            if (key.empty()) return fail("bad attribute in <" + node->tag + ">");
            skipSpace();
            if (pos_ >= s_.size() || s_[pos_] != '=') return fail("attribute '" + key + "' has no value");
            ++pos_;
            skipSpace();
            if (pos_ >= s_.size() || (s_[pos_] != '"' && s_[pos_] != '\'')) return fail("attribute '" + key + "' is not quoted");
            const char quote = s_[pos_++];
            const auto end = s_.find(quote, pos_);
            if (end == std::string_view::npos) return fail("unterminated attribute value");
            node->attrs[key] = unescape(std::string(s_.substr(pos_, end - pos_)));
            pos_ = end + 1;
        }
        for (;;) {
            skipText();
            if (pos_ >= s_.size()) return fail("missing </" + node->tag + ">");
            if (startsWith("</")) {
                pos_ += 2;
                const auto closing = name();
                skipSpace();
                if (closing != node->tag || pos_ >= s_.size() || s_[pos_] != '>')
                    return fail("mismatched </" + closing + "> for <" + node->tag + ">");
                ++pos_;
                --depth_;
                return node;
            }
            if (skipMisc()) {
                if (failed_) return nullptr;
                continue;
            }
            auto c = element();
            if (!c) return nullptr;
            node->children.push_back(std::move(c));
        }
    }

    std::string_view s_;
    std::string* error_;
    std::size_t pos_ = 0;
    int depth_ = 0;
    bool failed_ = false;
};

bool parseDoubles(const std::string& text, double* out, int count) {
    std::istringstream in(text);
    for (int i = 0; i < count; ++i) {
        std::string tok;
        if (!(in >> tok)) return false;
        char* end = nullptr;
        out[i] = std::strtod(tok.c_str(), &end);
        if (end == tok.c_str() || *end != '\0' || !std::isfinite(out[i])) return false;
    }
    std::string extra;
    return !(in >> extra);
}

bool parseScalar(const std::string& text, double* out) {
    return parseDoubles(text, out, 1);
}

Vec3 vec(const double* v) {
    return {v[0], v[1], v[2]};
}

}

std::optional<Robot> Robot::parse(std::string_view urdf, std::string* error) {
    auto fail = [&](const std::string& m) -> std::optional<Robot> {
        if (error) *error = m;
        return std::nullopt;
    };
    std::string xmlError;
    XmlParser parser(urdf, &xmlError);
    const auto root = parser.parse();
    if (!root) return fail(xmlError);
    if (root->tag != "robot") return fail("urdf: root element must be <robot>");

    Robot robot;
    robot.name_ = root->attr("name");
    std::set<std::string> linkSet;
    for (const auto& c : root->children) {
        if (c->tag != "link") continue;
        const auto n = c->attr("name");
        if (n.empty()) return fail("urdf: link without a name");
        if (!linkSet.insert(n).second) return fail("urdf: duplicate link '" + n + "'");
        robot.links_.push_back(n);
    }
    if (robot.links_.empty()) return fail("urdf: no links");

    std::set<std::string> jointNames;
    for (const auto& c : root->children) {
        if (c->tag != "joint") continue;
        Joint j;
        j.name = c->attr("name");
        if (j.name.empty()) return fail("urdf: joint without a name");
        if (!jointNames.insert(j.name).second) return fail("urdf: duplicate joint '" + j.name + "'");
        const auto type = c->attr("type");
        if (type == "fixed")
            j.type = JointType::Fixed;
        else if (type == "revolute")
            j.type = JointType::Revolute;
        else if (type == "continuous")
            j.type = JointType::Continuous;
        else if (type == "prismatic")
            j.type = JointType::Prismatic;
        else
            return fail("urdf: joint '" + j.name + "' has unsupported type '" + type + "'");
        const auto* parent = c->child("parent");
        const auto* child = c->child("child");
        if (!parent || !child) return fail("urdf: joint '" + j.name + "' needs <parent> and <child>");
        j.parent = parent->attr("link");
        j.child = child->attr("link");
        if (!linkSet.count(j.parent) || !linkSet.count(j.child)) return fail("urdf: joint '" + j.name + "' references an unknown link");
        if (const auto* o = c->child("origin")) {
            double xyz[3] = {0, 0, 0};
            double rpy[3] = {0, 0, 0};
            if (o->attrs.count("xyz") && !parseDoubles(o->attr("xyz"), xyz, 3)) return fail("urdf: bad xyz on joint '" + j.name + "'");
            if (o->attrs.count("rpy") && !parseDoubles(o->attr("rpy"), rpy, 3)) return fail("urdf: bad rpy on joint '" + j.name + "'");
            j.origin = {Quat::fromRpy(rpy[0], rpy[1], rpy[2]), vec(xyz)};
        }
        if (const auto* a = c->child("axis")) {
            double xyz[3] = {1, 0, 0};
            if (!parseDoubles(a->attr("xyz"), xyz, 3)) return fail("urdf: bad axis on joint '" + j.name + "'");
            const double n = norm(vec(xyz));
            if (n < kJointAxisZeroTolerance) return fail("urdf: zero axis on joint '" + j.name + "'");
            j.axis = vec(xyz) * (1.0 / n);
        }
        if (const auto* l = c->child("limit")) {
            double v = 0;
            if (l->attrs.count("lower")) {
                if (!parseScalar(l->attr("lower"), &v)) return fail("urdf: bad lower limit on joint '" + j.name + "'");
                j.lower = v;
            }
            if (l->attrs.count("upper")) {
                if (!parseScalar(l->attr("upper"), &v)) return fail("urdf: bad upper limit on joint '" + j.name + "'");
                j.upper = v;
            }
            if (l->attrs.count("effort") && parseScalar(l->attr("effort"), &v)) j.effort = v;
            if (l->attrs.count("velocity") && parseScalar(l->attr("velocity"), &v)) j.velocity = v;
        } else if (j.limited()) {
            return fail("urdf: joint '" + j.name + "' needs <limit>");
        }
        if (j.limited() && j.lower > j.upper) return fail("urdf: joint '" + j.name + "' has lower > upper");
        if (robot.byChild_.count(j.child)) return fail("urdf: link '" + j.child + "' has two parents");
        robot.byChild_[j.child] = robot.joints_.size();
        robot.joints_.push_back(std::move(j));
    }

    std::vector<std::string> roots;
    for (const auto& l : robot.links_)
        if (!robot.byChild_.count(l)) roots.push_back(l);
    if (roots.size() != 1)
        return fail(roots.empty() ? "urdf: kinematic loop" : "urdf: robot has " + std::to_string(roots.size()) + " root links");
    robot.root_ = roots.front();
    for (const auto& l : robot.links_) {
        std::string f = l;
        for (std::size_t hops = 0; f != robot.root_; ++hops) {
            if (hops > robot.links_.size()) return fail("urdf: kinematic loop");
            f = robot.joints_[robot.byChild_.at(f)].parent;
        }
    }
    return robot;
}

const Joint* Robot::joint(std::string_view name) const {
    for (const auto& j : joints_)
        if (j.name == name) return &j;
    return nullptr;
}

std::optional<std::vector<const Joint*>> Robot::chain(const std::string& tip) const {
    if (std::find(links_.begin(), links_.end(), tip) == links_.end()) return std::nullopt;
    std::vector<const Joint*> out;
    for (std::string f = tip; f != root_;) {
        const Joint& j = joints_[byChild_.at(f)];
        out.push_back(&j);
        f = j.parent;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<std::string> Robot::actuatedChain(const std::string& tip) const {
    std::vector<std::string> names;
    if (const auto c = chain(tip))
        for (const auto* j : *c)
            if (j->actuated()) names.push_back(j->name);
    return names;
}

namespace {

Transform jointMotion(const Joint& j, double value) {
    switch (j.type) {
    case JointType::Revolute:
    case JointType::Continuous: return {Quat::fromAxisAngle(j.axis, value), {}};
    case JointType::Prismatic: return {{}, j.axis * value};
    case JointType::Fixed: break;
    }
    return {};
}

}

std::optional<Transform> Robot::forwardKinematics(const std::string& tip, const JointValues& q) const {
    const auto c = chain(tip);
    if (!c) return std::nullopt;
    Transform acc;
    for (const auto* j : *c) {
        double v = 0.0;
        if (j->actuated()) {
            const auto it = q.find(j->name);
            if (it != q.end()) v = it->second;
        }
        acc = acc * j->origin * jointMotion(*j, v);
    }
    return acc;
}

double Robot::clampToLimits(const std::string& name, double value) const {
    const Joint* j = joint(name);
    if (!j || !j->limited()) return value;
    return std::clamp(value, j->lower, j->upper);
}

namespace {

Vec3 rotationVector(const Quat& qin) {
    Quat q = qin.normalized();
    if (q.w < 0) q = {-q.w, -q.x, -q.y, -q.z};
    const double s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    if (s < 1e-12) return {0, 0, 0};
    const double angle = 2.0 * std::atan2(s, q.w);
    return Vec3{q.x, q.y, q.z} * (angle / s);
}

}

std::optional<std::vector<double>> Robot::jacobian(const std::string& tip, const std::vector<std::string>& names,
                                                   const JointValues& q) const {
    const auto base = forwardKinematics(tip, q);
    if (!base) return std::nullopt;
    const std::size_t n = names.size();
    std::vector<double> jac(kSpatialJacobianRows * n, 0.0);
    constexpr double h = kInverseKinematicsFiniteDifferenceStep;
    for (std::size_t i = 0; i < n; ++i) {
        if (!joint(names[i])) return std::nullopt;
        JointValues qp = q;
        qp[names[i]] += h;
        const auto moved = forwardKinematics(tip, qp);
        if (!moved) return std::nullopt;
        const Vec3 dp = (moved->translation - base->translation) * (1.0 / h);
        const Vec3 dw = rotationVector(moved->rotation * base->rotation.conjugate()) * (1.0 / h);
        jac[0 * n + i] = dp.x;
        jac[1 * n + i] = dp.y;
        jac[2 * n + i] = dp.z;
        jac[kPositionJacobianRows * n + i] = dw.x;
        jac[(kPositionJacobianRows + 1) * n + i] = dw.y;
        jac[(kPositionJacobianRows + 2) * n + i] = dw.z;
    }
    return jac;
}

std::optional<IkResult> Robot::inverseKinematics(const std::string& tip, const Transform& target, const JointValues& seed,
                                                 const IkOptions& options) const {
    const auto names = actuatedChain(tip);
    if (!chain(tip)) return std::nullopt;
    const std::size_t n = names.size();
    JointValues q = seed;
    for (const auto& name : names) q[name] = clampToLimits(name, q.count(name) ? q[name] : 0.0);
    IkResult result;
    const auto rows = options.useOrientation ? kSpatialJacobianRows : kPositionJacobianRows;
    for (int it = 0; it <= options.maxIterations; ++it) {
        const auto pose = forwardKinematics(tip, q);
        if (!pose) return std::nullopt;
        const Vec3 ep = target.translation - pose->translation;
        const Vec3 eo = rotationVector(target.rotation * pose->rotation.conjugate());
        result.iterations = it;
        result.positionError = norm(ep);
        result.orientationError = norm(eo);
        if (result.positionError <= options.positionTolerance &&
            (!options.useOrientation || result.orientationError <= options.orientationTolerance)) {
            result.converged = true;
            break;
        }
        if (it == options.maxIterations || n == 0) break;
        const auto jac = jacobian(tip, names, q);
        if (!jac) break;
        const auto r = rows;
        const math::Matrix j(r, n, std::vector<double>(jac->begin(), jac->begin() + static_cast<std::ptrdiff_t>(r * n)));
        const double err[kSpatialJacobianRows] = {ep.x, ep.y, ep.z, eo.x, eo.y, eo.z};
        std::vector<double> dq;
        if (!math::linalg().dampedLeastSquares(j, std::vector<double>(err, err + r), options.damping, dq)) break;
        double stepNorm = 0.0;
        for (const double d : dq) stepNorm += d * d;
        stepNorm = std::sqrt(stepNorm);
        const double scale = stepNorm > options.maxStep ? options.maxStep / stepNorm : 1.0;
        for (std::size_t c = 0; c < n; ++c) q[names[c]] = clampToLimits(names[c], q[names[c]] + dq[c] * scale);
    }
    for (const auto& name : names) result.q.push_back(q[name]);
    return result;
}

void Robot::publish(TfBuffer& buffer, const JointValues& q, double stamp) const {
    for (const auto& j : joints_) {
        double v = 0.0;
        if (j.actuated()) {
            const auto it = q.find(j.name);
            if (it != q.end()) v = it->second;
        }
        const Transform t = j.origin * jointMotion(j, v);
        if (j.actuated())
            buffer.set(j.parent, j.child, t, stamp);
        else
            buffer.setStatic(j.parent, j.child, t);
    }
}

}
