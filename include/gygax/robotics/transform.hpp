#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace gygax::robotics {

inline constexpr double kDefaultTransformCacheSeconds = 10.0;
inline constexpr double kDefaultTransformMaxExtrapolationSeconds = 0.0;

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

Vec3 operator+(const Vec3& a, const Vec3& b);
Vec3 operator-(const Vec3& a, const Vec3& b);
Vec3 operator*(const Vec3& a, double s);
double dot(const Vec3& a, const Vec3& b);
Vec3 cross(const Vec3& a, const Vec3& b);
double norm(const Vec3& a);

struct Quat {
    double w = 1.0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    static Quat fromAxisAngle(const Vec3& axis, double angle);
    static Quat fromRpy(double roll, double pitch, double yaw);
    [[nodiscard]] Vec3 toRpy() const;
    [[nodiscard]] Quat conjugate() const;
    [[nodiscard]] Quat normalized() const;
    [[nodiscard]] Vec3 rotate(const Vec3& v) const;
    [[nodiscard]] double angleTo(const Quat& other) const;
};

Quat operator*(const Quat& a, const Quat& b);
Quat slerp(const Quat& a, const Quat& b, double t);

using Mat4 = std::array<double, 16>;

struct Transform {
    Quat rotation;
    Vec3 translation;

    [[nodiscard]] Transform inverse() const;
    [[nodiscard]] Vec3 apply(const Vec3& point) const;
    [[nodiscard]] Mat4 matrix() const;
    static Transform fromMatrix(const Mat4& m);
    static Transform planar(double x, double y, double theta);
};

Transform operator*(const Transform& a, const Transform& b);
Transform interpolate(const Transform& a, const Transform& b, double t);

struct TransformStamped {
    std::string parent;
    std::string child;
    double stamp = 0.0;
    Transform transform;
};

class TfBuffer {
public:
    explicit TfBuffer(double cacheSeconds = kDefaultTransformCacheSeconds,
                      double maxExtrapolationSeconds = kDefaultTransformMaxExtrapolationSeconds);

    bool set(const std::string& parent, const std::string& child, const Transform& transform, double stamp);
    bool setStatic(const std::string& parent, const std::string& child, const Transform& transform);

    [[nodiscard]] std::optional<Transform> lookup(const std::string& target, const std::string& source, double stamp) const;
    [[nodiscard]] std::optional<Transform> lookupLatest(const std::string& target, const std::string& source) const;
    [[nodiscard]] bool canTransform(const std::string& target, const std::string& source, double stamp) const;
    [[nodiscard]] std::optional<Vec3> transformPoint(const std::string& target, const std::string& source, const Vec3& point,
                                                     double stamp) const;
    [[nodiscard]] std::vector<std::string> frames() const;
    [[nodiscard]] std::optional<std::string> parentOf(const std::string& frame) const;
    void clear();

private:
    struct Edge {
        std::string parent;
        bool isStatic = false;
        std::vector<std::pair<double, Transform>> history;
    };

    [[nodiscard]] std::optional<Transform> sample(const Edge& edge, double stamp) const;
    [[nodiscard]] std::optional<std::vector<std::string>> chainToRoot(const std::string& frame) const;
    [[nodiscard]] std::optional<Transform> resolve(const std::string& target, const std::string& source, const double* stamp) const;
    [[nodiscard]] std::optional<Transform> upTo(const std::vector<std::string>& chain, std::size_t count, const double* stamp) const;
    [[nodiscard]] double latestOf(const std::vector<std::string>& chain, std::size_t count) const;

    double cache_;
    double maxExtrapolation_;
    mutable std::mutex mutex_;
    std::map<std::string, Edge> edges_;
};

}
