#include <gygax/robotics/transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace gygax::robotics {

Vec3 operator+(const Vec3& a, const Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 operator-(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 operator*(const Vec3& a, double s) {
    return {a.x * s, a.y * s, a.z * s};
}
double dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double norm(const Vec3& a) {
    return std::sqrt(dot(a, a));
}

Quat Quat::fromAxisAngle(const Vec3& axis, double angle) {
    const double n = norm(axis);
    if (n < 1e-12) return {};
    const double s = std::sin(angle / 2.0) / n;
    return {std::cos(angle / 2.0), axis.x * s, axis.y * s, axis.z * s};
}

Quat Quat::fromRpy(double roll, double pitch, double yaw) {
    const double cr = std::cos(roll / 2), sr = std::sin(roll / 2);
    const double cp = std::cos(pitch / 2), sp = std::sin(pitch / 2);
    const double cy = std::cos(yaw / 2), sy = std::sin(yaw / 2);
    return {cr * cp * cy + sr * sp * sy, sr * cp * cy - cr * sp * sy, cr * sp * cy + sr * cp * sy, cr * cp * sy - sr * sp * cy};
}

Vec3 Quat::toRpy() const {
    const double sinr = 2.0 * (w * x + y * z);
    const double cosr = 1.0 - 2.0 * (x * x + y * y);
    const double sinp = std::clamp(2.0 * (w * y - z * x), -1.0, 1.0);
    const double siny = 2.0 * (w * z + x * y);
    const double cosy = 1.0 - 2.0 * (y * y + z * z);
    return {std::atan2(sinr, cosr), std::asin(sinp), std::atan2(siny, cosy)};
}

Quat Quat::conjugate() const {
    return {w, -x, -y, -z};
}

Quat Quat::normalized() const {
    const double n = std::sqrt(w * w + x * x + y * y + z * z);
    if (n < 1e-12) return {};
    return {w / n, x / n, y / n, z / n};
}

Vec3 Quat::rotate(const Vec3& v) const {
    const Vec3 u{x, y, z};
    const Vec3 t = cross(u, v) * 2.0;
    return v + t * w + cross(u, t);
}

double Quat::angleTo(const Quat& other) const {
    const double d = std::min(1.0, std::fabs(w * other.w + x * other.x + y * other.y + z * other.z));
    return 2.0 * std::acos(d);
}

Quat operator*(const Quat& a, const Quat& b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

Quat slerp(const Quat& a, const Quat& b, double t) {
    double d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    Quat bb = b;
    if (d < 0.0) {
        d = -d;
        bb = {-b.w, -b.x, -b.y, -b.z};
    }
    if (d > 0.9995) {
        return Quat{a.w + t * (bb.w - a.w), a.x + t * (bb.x - a.x), a.y + t * (bb.y - a.y), a.z + t * (bb.z - a.z)}.normalized();
    }
    const double theta = std::acos(d);
    const double s = std::sin(theta);
    const double wa = std::sin((1.0 - t) * theta) / s;
    const double wb = std::sin(t * theta) / s;
    return {wa * a.w + wb * bb.w, wa * a.x + wb * bb.x, wa * a.y + wb * bb.y, wa * a.z + wb * bb.z};
}

Transform Transform::inverse() const {
    const Quat qi = rotation.conjugate();
    return {qi, qi.rotate(translation) * -1.0};
}

Vec3 Transform::apply(const Vec3& point) const {
    return rotation.rotate(point) + translation;
}

Mat4 Transform::matrix() const {
    const Quat q = rotation.normalized();
    const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    return {1 - 2 * (yy + zz),
            2 * (xy - wz),
            2 * (xz + wy),
            translation.x,
            2 * (xy + wz),
            1 - 2 * (xx + zz),
            2 * (yz - wx),
            translation.y,
            2 * (xz - wy),
            2 * (yz + wx),
            1 - 2 * (xx + yy),
            translation.z,
            0,
            0,
            0,
            1};
}

Transform Transform::fromMatrix(const Mat4& m) {
    Quat q;
    const double trace = m[0] + m[5] + m[10];
    if (trace > 0.0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        q = {0.25 * s, (m[9] - m[6]) / s, (m[2] - m[8]) / s, (m[4] - m[1]) / s};
    } else if (m[0] > m[5] && m[0] > m[10]) {
        const double s = std::sqrt(1.0 + m[0] - m[5] - m[10]) * 2.0;
        q = {(m[9] - m[6]) / s, 0.25 * s, (m[1] + m[4]) / s, (m[2] + m[8]) / s};
    } else if (m[5] > m[10]) {
        const double s = std::sqrt(1.0 + m[5] - m[0] - m[10]) * 2.0;
        q = {(m[2] - m[8]) / s, (m[1] + m[4]) / s, 0.25 * s, (m[6] + m[9]) / s};
    } else {
        const double s = std::sqrt(1.0 + m[10] - m[0] - m[5]) * 2.0;
        q = {(m[4] - m[1]) / s, (m[2] + m[8]) / s, (m[6] + m[9]) / s, 0.25 * s};
    }
    return {q.normalized(), {m[3], m[7], m[11]}};
}

Transform Transform::planar(double x, double y, double theta) {
    return {Quat::fromAxisAngle({0, 0, 1}, theta), {x, y, 0.0}};
}

Transform operator*(const Transform& a, const Transform& b) {
    return {(a.rotation * b.rotation).normalized(), a.rotation.rotate(b.translation) + a.translation};
}

Transform interpolate(const Transform& a, const Transform& b, double t) {
    return {slerp(a.rotation, b.rotation, t), a.translation + (b.translation - a.translation) * t};
}

TfBuffer::TfBuffer(double cacheSeconds, double maxExtrapolationSeconds)
    : cache_(cacheSeconds), maxExtrapolation_(maxExtrapolationSeconds) {}

bool TfBuffer::set(const std::string& parent, const std::string& child, const Transform& transform, double stamp) {
    if (parent.empty() || child.empty() || parent == child) return false;
    std::lock_guard lock(mutex_);
    auto it = edges_.find(child);
    if (it != edges_.end() && (it->second.parent != parent || it->second.isStatic)) return false;
    if (it == edges_.end()) {
        std::set<std::string> seen{child};
        for (std::string f = parent;;) {
            if (!seen.insert(f).second) return false;
            auto p = edges_.find(f);
            if (p == edges_.end()) break;
            f = p->second.parent;
        }
        it = edges_.emplace(child, Edge{parent, false, {}}).first;
    }
    auto& h = it->second.history;
    const auto pos = std::lower_bound(h.begin(), h.end(), stamp, [](const auto& e, double s) { return e.first < s; });
    if (pos != h.end() && pos->first == stamp) {
        pos->second = transform;
    } else {
        h.insert(pos, {stamp, transform});
    }
    const double newest = h.back().first;
    while (h.size() > 1 && h.front().first < newest - cache_) h.erase(h.begin());
    return true;
}

bool TfBuffer::setStatic(const std::string& parent, const std::string& child, const Transform& transform) {
    if (parent.empty() || child.empty() || parent == child) return false;
    std::lock_guard lock(mutex_);
    auto it = edges_.find(child);
    if (it != edges_.end() && it->second.parent != parent) return false;
    if (it == edges_.end()) {
        std::set<std::string> seen{child};
        for (std::string f = parent;;) {
            if (!seen.insert(f).second) return false;
            auto p = edges_.find(f);
            if (p == edges_.end()) break;
            f = p->second.parent;
        }
        it = edges_.emplace(child, Edge{parent, true, {}}).first;
    }
    it->second.isStatic = true;
    it->second.history.assign(1, {0.0, transform});
    return true;
}

std::optional<Transform> TfBuffer::sample(const Edge& edge, double stamp) const {
    const auto& h = edge.history;
    if (h.empty()) return std::nullopt;
    if (edge.isStatic) return h.front().second;
    if (stamp < h.front().first) {
        if (h.front().first - stamp > maxExtrapolation_) return std::nullopt;
        return h.front().second;
    }
    if (stamp > h.back().first) {
        if (stamp - h.back().first > maxExtrapolation_) return std::nullopt;
        return h.back().second;
    }
    const auto hi = std::lower_bound(h.begin(), h.end(), stamp, [](const auto& e, double s) { return e.first < s; });
    if (hi->first == stamp || hi == h.begin()) return hi->second;
    const auto lo = hi - 1;
    return interpolate(lo->second, hi->second, (stamp - lo->first) / (hi->first - lo->first));
}

std::optional<std::vector<std::string>> TfBuffer::chainToRoot(const std::string& frame) const {
    std::vector<std::string> chain{frame};
    for (std::string f = frame;;) {
        auto it = edges_.find(f);
        if (it == edges_.end()) break;
        f = it->second.parent;
        chain.push_back(f);
        if (chain.size() > edges_.size() + 1) return std::nullopt;
    }
    return chain;
}

double TfBuffer::latestOf(const std::vector<std::string>& chain, std::size_t count) const {
    double latest = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < count; ++i) {
        const auto& e = edges_.at(chain[i]);
        if (!e.isStatic && !e.history.empty()) latest = std::min(latest, e.history.back().first);
    }
    return latest;
}

std::optional<Transform> TfBuffer::upTo(const std::vector<std::string>& chain, std::size_t count, const double* stamp) const {
    double t = stamp ? *stamp : latestOf(chain, count);
    Transform acc;
    for (std::size_t i = count; i-- > 0;) {
        const auto s = sample(edges_.at(chain[i]), std::isfinite(t) ? t : 0.0);
        if (!s) return std::nullopt;
        acc = acc * *s;
    }
    return acc;
}

std::optional<Transform> TfBuffer::resolve(const std::string& target, const std::string& source, const double* stamp) const {
    if (target == source) return Transform{};
    const auto srcChain = chainToRoot(source);
    const auto dstChain = chainToRoot(target);
    if (!srcChain || !dstChain) return std::nullopt;
    for (std::size_t i = 0; i < srcChain->size(); ++i) {
        const auto match = std::find(dstChain->begin(), dstChain->end(), (*srcChain)[i]);
        if (match == dstChain->end()) continue;
        const auto j = static_cast<std::size_t>(match - dstChain->begin());
        double common = 0.0;
        const double* use = stamp;
        if (!stamp) {
            common = std::min(latestOf(*srcChain, i), latestOf(*dstChain, j));
            if (std::isfinite(common)) use = &common;
        }
        const auto fromSrc = upTo(*srcChain, i, use);
        const auto fromDst = upTo(*dstChain, j, use);
        if (!fromSrc || !fromDst) return std::nullopt;
        return fromDst->inverse() * *fromSrc;
    }
    return std::nullopt;
}

std::optional<Transform> TfBuffer::lookup(const std::string& target, const std::string& source, double stamp) const {
    std::lock_guard lock(mutex_);
    return resolve(target, source, &stamp);
}

std::optional<Transform> TfBuffer::lookupLatest(const std::string& target, const std::string& source) const {
    std::lock_guard lock(mutex_);
    return resolve(target, source, nullptr);
}

bool TfBuffer::canTransform(const std::string& target, const std::string& source, double stamp) const {
    return lookup(target, source, stamp).has_value();
}

std::optional<Vec3> TfBuffer::transformPoint(const std::string& target, const std::string& source, const Vec3& point, double stamp) const {
    const auto t = lookup(target, source, stamp);
    if (!t) return std::nullopt;
    return t->apply(point);
}

std::vector<std::string> TfBuffer::frames() const {
    std::lock_guard lock(mutex_);
    std::set<std::string> all;
    for (const auto& [child, edge] : edges_) {
        all.insert(child);
        all.insert(edge.parent);
    }
    return {all.begin(), all.end()};
}

std::optional<std::string> TfBuffer::parentOf(const std::string& frame) const {
    std::lock_guard lock(mutex_);
    const auto it = edges_.find(frame);
    if (it == edges_.end()) return std::nullopt;
    return it->second.parent;
}

void TfBuffer::clear() {
    std::lock_guard lock(mutex_);
    edges_.clear();
}

}
