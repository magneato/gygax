#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gygax/robotics/transform.hpp>

namespace gygax::robotics {

inline constexpr int kDefaultIkMaxIterations = 200;
inline constexpr double kDefaultIkPositionTolerance = 1e-4;
inline constexpr double kDefaultIkOrientationTolerance = 1e-3;
inline constexpr double kDefaultIkDamping = 0.05;
inline constexpr double kDefaultIkMaxStep = 0.3;

enum class JointType { Fixed, Revolute, Continuous, Prismatic };

struct Joint {
    std::string name;
    JointType type = JointType::Fixed;
    std::string parent;
    std::string child;
    Transform origin;
    Vec3 axis{1.0, 0.0, 0.0};
    double lower = 0.0;
    double upper = 0.0;
    double effort = 0.0;
    double velocity = 0.0;

    [[nodiscard]] bool actuated() const { return type != JointType::Fixed; }
    [[nodiscard]] bool limited() const { return type == JointType::Revolute || type == JointType::Prismatic; }
};

using JointValues = std::map<std::string, double>;

struct IkOptions {
    int maxIterations = kDefaultIkMaxIterations;
    double positionTolerance = kDefaultIkPositionTolerance;
    double orientationTolerance = kDefaultIkOrientationTolerance;
    double damping = kDefaultIkDamping;
    double maxStep = kDefaultIkMaxStep;
    bool useOrientation = true;
};

struct IkResult {
    bool converged = false;
    int iterations = 0;
    double positionError = 0.0;
    double orientationError = 0.0;
    std::vector<double> q;
};

class Robot {
public:
    static std::optional<Robot> parse(std::string_view urdf, std::string* error);

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] const std::vector<std::string>& links() const { return links_; }
    [[nodiscard]] const std::vector<Joint>& joints() const { return joints_; }
    [[nodiscard]] const std::string& rootLink() const { return root_; }
    [[nodiscard]] const Joint* joint(std::string_view name) const;

    [[nodiscard]] std::optional<std::vector<const Joint*>> chain(const std::string& tip) const;
    [[nodiscard]] std::vector<std::string> actuatedChain(const std::string& tip) const;

    [[nodiscard]] std::optional<Transform> forwardKinematics(const std::string& tip, const JointValues& q) const;
    [[nodiscard]] std::optional<std::vector<double>> jacobian(const std::string& tip, const std::vector<std::string>& joints,
                                                              const JointValues& q) const;
    [[nodiscard]] std::optional<IkResult> inverseKinematics(const std::string& tip, const Transform& target, const JointValues& seed,
                                                            const IkOptions& options = {}) const;

    [[nodiscard]] double clampToLimits(const std::string& joint, double value) const;
    void publish(TfBuffer& buffer, const JointValues& q, double stamp) const;

private:
    std::string name_;
    std::string root_;
    std::vector<std::string> links_;
    std::vector<Joint> joints_;
    std::map<std::string, std::size_t> byChild_;
};

}
