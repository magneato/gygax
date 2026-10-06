#include <gtest/gtest.h>

#include <cmath>

#include <gygax/robotics/transform.hpp>
#include <gygax/robotics/urdf.hpp>

using namespace gygax::robotics;

namespace {

constexpr double kPi = 3.14159265358979323846;

void expectNear(Vec3 a, Vec3 b, double tol = 1e-9) {
    EXPECT_NEAR(a.x, b.x, tol);
    EXPECT_NEAR(a.y, b.y, tol);
    EXPECT_NEAR(a.z, b.z, tol);
}

const char* kArm = R"(<?xml version="1.0"?>
<robot name="arm">
  <!-- two link planar arm -->
  <link name="base"/>
  <link name="upper"/>
  <link name="fore"/>
  <link name="tool"/>
  <joint name="shoulder" type="revolute">
    <parent link="base"/><child link="upper"/>
    <origin xyz="0 0 0.1" rpy="0 0 0"/>
    <axis xyz="0 0 1"/>
    <limit lower="-3.0" upper="3.0" effort="10" velocity="2"/>
  </joint>
  <joint name="elbow" type="revolute">
    <parent link="upper"/><child link="fore"/>
    <origin xyz="1 0 0"/>
    <axis xyz="0 0 1"/>
    <limit lower="-2.5" upper="2.5" effort="10" velocity="2"/>
  </joint>
  <joint name="flange" type="fixed">
    <parent link="fore"/><child link="tool"/>
    <origin xyz="1 0 0"/>
  </joint>
</robot>)";

}

TEST(Quat, RotatesAndRoundTripsRpy) {
    const auto q = Quat::fromAxisAngle({0, 0, 1}, kPi / 2);
    expectNear(q.rotate({1, 0, 0}), {0, 1, 0});
    const auto r = Quat::fromRpy(0.3, -0.4, 1.2).toRpy();
    EXPECT_NEAR(r.x, 0.3, 1e-9);
    EXPECT_NEAR(r.y, -0.4, 1e-9);
    EXPECT_NEAR(r.z, 1.2, 1e-9);
}

TEST(Transform, InverseAndMatrixAreConsistent) {
    const Transform t{Quat::fromRpy(0.2, 0.5, -0.9), {1, -2, 3}};
    const Vec3 p{0.5, 0.25, -1.0};
    expectNear(t.inverse().apply(t.apply(p)), p);
    const auto m = t.matrix();
    const Vec3 viaMatrix{m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                         m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
    expectNear(viaMatrix, t.apply(p));
    const auto back = Transform::fromMatrix(m);
    EXPECT_NEAR(back.rotation.angleTo(t.rotation), 0.0, 1e-7);
    expectNear(back.translation, t.translation);
}

TEST(Transform, FromMatrixHandlesLargeRotations) {
    for (double angle : {kPi * 0.99, kPi, -kPi * 0.75}) {
        for (Vec3 axis : {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}) {
            const Transform t{Quat::fromAxisAngle(axis, angle), {0, 0, 0}};
            EXPECT_NEAR(Transform::fromMatrix(t.matrix()).rotation.angleTo(t.rotation), 0.0, 1e-6);
        }
    }
}

TEST(Slerp, HitsEndpointsAndMidpoint) {
    const Quat a;
    const Quat b = Quat::fromAxisAngle({0, 0, 1}, kPi / 2);
    EXPECT_NEAR(slerp(a, b, 0.0).angleTo(a), 0.0, 1e-9);
    EXPECT_NEAR(slerp(a, b, 1.0).angleTo(b), 0.0, 1e-9);
    EXPECT_NEAR(slerp(a, b, 0.5).angleTo(Quat::fromAxisAngle({0, 0, 1}, kPi / 4)), 0.0, 1e-9);
}

TEST(TfBuffer, ChainsFramesAcrossBranches) {
    TfBuffer tf;
    ASSERT_TRUE(tf.setStatic("map", "odom", Transform::planar(10, 0, 0)));
    ASSERT_TRUE(tf.set("odom", "base_link", Transform::planar(1, 2, kPi / 2), 1.0));
    ASSERT_TRUE(tf.setStatic("base_link", "lidar_link", Transform{{}, {0.5, 0, 0.2}}));
    ASSERT_TRUE(tf.setStatic("base_link", "camera_link", Transform{{}, {0, 0.3, 0.4}}));

    const auto lidarInMap = tf.lookup("map", "lidar_link", 1.0);
    ASSERT_TRUE(lidarInMap);
    expectNear(lidarInMap->translation, {10 + 1 - 0.0, 2 + 0.5, 0.2});

    const auto camInLidar = tf.lookup("lidar_link", "camera_link", 1.0);
    ASSERT_TRUE(camInLidar);
    expectNear(camInLidar->translation, {-0.5, 0.3, 0.2});
    expectNear(tf.lookup("camera_link", "lidar_link", 1.0)->apply(camInLidar->translation), {0, 0, 0});
    EXPECT_TRUE(tf.lookup("map", "map", 1.0));
}

TEST(TfBuffer, InterpolatesBetweenSamplesAndRejectsExtrapolation) {
    TfBuffer tf(10.0, 0.0);
    tf.set("odom", "base", Transform::planar(0, 0, 0), 1.0);
    tf.set("odom", "base", Transform::planar(2, 0, kPi / 2), 2.0);
    const auto mid = tf.lookup("odom", "base", 1.5);
    ASSERT_TRUE(mid);
    expectNear(mid->translation, {1, 0, 0});
    EXPECT_NEAR(mid->rotation.toRpy().z, kPi / 4, 1e-9);
    EXPECT_FALSE(tf.lookup("odom", "base", 0.5));
    EXPECT_FALSE(tf.lookup("odom", "base", 2.5));
    EXPECT_TRUE(TfBuffer(10.0, 1.0).lookup("odom", "base", 0.5) == std::nullopt);

    TfBuffer tolerant(10.0, 1.0);
    tolerant.set("odom", "base", Transform::planar(3, 0, 0), 1.0);
    EXPECT_TRUE(tolerant.lookup("odom", "base", 1.8));
    EXPECT_TRUE(tolerant.lookupLatest("odom", "base"));
}

TEST(TfBuffer, PrunesOldSamplesAndRejectsCyclesAndReparenting) {
    TfBuffer tf(1.0);
    for (int i = 0; i < 10; ++i) tf.set("a", "b", Transform::planar(i, 0, 0), i);
    EXPECT_FALSE(tf.lookup("a", "b", 2.0));
    EXPECT_TRUE(tf.lookup("a", "b", 9.0));
    EXPECT_FALSE(tf.set("b", "a", {}, 1.0));
    EXPECT_FALSE(tf.set("c", "b", {}, 1.0));
    EXPECT_FALSE(tf.set("a", "a", {}, 1.0));
    EXPECT_TRUE(tf.setStatic("b", "c", {}));
    EXPECT_FALSE(tf.set("c", "a", {}, 1.0));
    EXPECT_FALSE(tf.lookup("a", "unknown", 9.0));
    EXPECT_EQ(tf.parentOf("b").value(), "a");
    EXPECT_EQ(tf.frames().size(), 3U);
}

TEST(TfBuffer, LookupLatestUsesTheCommonTime) {
    TfBuffer tf;
    tf.set("map", "odom", Transform::planar(0, 0, 0), 1.0);
    tf.set("map", "odom", Transform::planar(1, 0, 0), 3.0);
    tf.set("odom", "base", Transform::planar(0, 0, 0), 1.0);
    tf.set("odom", "base", Transform::planar(0, 4, 0), 2.0);
    const auto latest = tf.lookupLatest("map", "base");
    ASSERT_TRUE(latest);
    expectNear(latest->translation, {0.5, 4, 0}, 1e-9);
}

TEST(Urdf, ParsesAndComputesForwardKinematics) {
    std::string err;
    const auto robot = Robot::parse(kArm, &err);
    ASSERT_TRUE(robot) << err;
    EXPECT_EQ(robot->name(), "arm");
    EXPECT_EQ(robot->rootLink(), "base");
    EXPECT_EQ(robot->joints().size(), 3U);
    const auto names = robot->actuatedChain("tool");
    ASSERT_EQ(names.size(), 2U);
    EXPECT_EQ(names[0], "shoulder");

    const auto zero = robot->forwardKinematics("tool", {});
    ASSERT_TRUE(zero);
    expectNear(zero->translation, {2, 0, 0.1});

    const auto bent = robot->forwardKinematics("tool", {{"shoulder", kPi / 2}, {"elbow", -kPi / 2}});
    ASSERT_TRUE(bent);
    expectNear(bent->translation, {1, 1, 0.1}, 1e-9);
    EXPECT_NEAR(bent->rotation.toRpy().z, 0.0, 1e-9);
    EXPECT_FALSE(robot->forwardKinematics("nope", {}));
}

TEST(Urdf, InverseKinematicsReachesReachableTargets) {
    const auto robot = Robot::parse(kArm, nullptr);
    ASSERT_TRUE(robot);
    const Transform target = *robot->forwardKinematics("tool", {{"shoulder", 0.7}, {"elbow", 1.1}});
    IkOptions options;
    options.useOrientation = false;
    const auto result = robot->inverseKinematics("tool", target, {{"shoulder", 0.1}, {"elbow", 0.2}}, options);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->converged);
    ASSERT_EQ(result->q.size(), 2U);
    const auto reached = robot->forwardKinematics("tool", {{"shoulder", result->q[0]}, {"elbow", result->q[1]}});
    expectNear(reached->translation, target.translation, 1e-3);

    const auto far = robot->inverseKinematics("tool", Transform{{}, {5, 0, 0.1}}, {}, options);
    ASSERT_TRUE(far);
    EXPECT_FALSE(far->converged);
    for (double v : far->q) EXPECT_LE(std::fabs(v), 3.0 + 1e-12);
}

TEST(Urdf, JacobianMatchesPlanarGeometry) {
    const auto robot = Robot::parse(kArm, nullptr);
    const auto jac = robot->jacobian("tool", {"shoulder", "elbow"}, {});
    ASSERT_TRUE(jac);
    EXPECT_NEAR((*jac)[1 * 2 + 0], 2.0, 1e-4);
    EXPECT_NEAR((*jac)[1 * 2 + 1], 1.0, 1e-4);
    EXPECT_NEAR((*jac)[5 * 2 + 0], 1.0, 1e-4);
}

TEST(Urdf, PublishesJointStatesIntoTheTfBuffer) {
    const auto robot = Robot::parse(kArm, nullptr);
    TfBuffer tf;
    robot->publish(tf, {{"shoulder", kPi / 2}}, 1.0);
    const auto pose = tf.lookup("base", "tool", 1.0);
    ASSERT_TRUE(pose);
    expectNear(pose->translation, {0, 2, 0.1}, 1e-9);
}

TEST(Urdf, RejectsMalformedModelsWithUsefulErrors) {
    auto expectError = [](const char* text, const char* needle) {
        std::string err;
        EXPECT_FALSE(Robot::parse(text, &err)) << text;
        EXPECT_NE(err.find(needle), std::string::npos) << err;
    };
    expectError("", "root");
    expectError("<robot><link name='a'></robot>", "mismatched");
    expectError("<model/>", "<robot>");
    expectError("<robot/>", "no links");
    expectError("<robot><link name='a'/><link name='a'/></robot>", "duplicate link");
    expectError("<robot><link name='a'/><link name='b'/></robot>", "2 root");
    expectError(
        "<robot><link name='a'/><link name='b'/><joint name='j' type='floating'><parent link='a'/><child link='b'/></joint></robot>",
        "unsupported");
    expectError(
        "<robot><link name='a'/><link name='b'/><joint name='j' type='revolute'><parent link='a'/><child link='b'/></joint></robot>",
        "<limit>");
    expectError("<robot><link name='a'/><joint name='j' type='fixed'><parent link='a'/><child link='z'/></joint></robot>", "unknown link");
    expectError("<robot><link name='a'/><link name='b'/><joint name='j' type='fixed'><parent link='a'/><child link='b'/>"
                "<origin xyz='1 2'/></joint></robot>",
                "xyz");
    expectError(
        "<robot><link name='a'/><link name='b'/><link name='c'/><joint name='j1' type='fixed'><parent link='a'/><child link='c'/></joint>"
        "<joint name='j2' type='fixed'><parent link='b'/><child link='c'/></joint></robot>",
        "two parents");
}
