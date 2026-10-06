#include <gtest/gtest.h>

#include <gygax/math/bigint.hpp>
#include <gygax/math/linalg.hpp>

using namespace gygax::math;

TEST(Linalg, SolvesAndMultipliesThroughTheDefaultBackend) {
    EXPECT_EQ(linalg().name(), "eigen");
    const Matrix a(2, 2, {2, 1, 1, 3});
    std::vector<double> x;
    ASSERT_TRUE(linalg().solve(a, {3, 5}, x));
    EXPECT_NEAR(x[0], 0.8, 1e-12);
    EXPECT_NEAR(x[1], 1.4, 1e-12);
    const auto p = linalg().multiply(Matrix(2, 3, {1, 2, 3, 4, 5, 6}), Matrix(3, 1, {1, 0, -1}));
    ASSERT_EQ(p.rows(), 2U);
    ASSERT_EQ(p.cols(), 1U);
    EXPECT_EQ(p(0, 0), -2.0);
    EXPECT_EQ(p(1, 0), -2.0);
}

TEST(Linalg, RejectsSingularAndMismatchedInputs) {
    std::vector<double> x;
    EXPECT_FALSE(linalg().solve(Matrix(2, 2, {1, 2, 2, 4}), {1, 2}, x));
    EXPECT_FALSE(linalg().solve(Matrix(2, 3), {1, 2}, x));
    EXPECT_FALSE(linalg().solve(Matrix(2, 2, {1, 0, 0, 1}), {1}, x));
    EXPECT_EQ(linalg().multiply(Matrix(2, 2), Matrix(3, 1)).rows(), 0U);
    EXPECT_FALSE(linalg().dampedLeastSquares(Matrix(2, 2), {1}, 0.1, x));
}

TEST(Linalg, DampedLeastSquaresApproachesThePseudoInverse) {
    std::vector<double> dq;
    ASSERT_TRUE(linalg().dampedLeastSquares(Matrix(1, 2, {1, 1}), {2.0}, 1e-6, dq));
    EXPECT_NEAR(dq[0], 1.0, 1e-6);
    EXPECT_NEAR(dq[1], 1.0, 1e-6);
}

namespace {

class CountingBackend final : public LinearBackend {
public:
    explicit CountingBackend(std::shared_ptr<LinearBackend> inner, int* calls) : inner_(std::move(inner)), calls_(calls) {}
    [[nodiscard]] std::string name() const override { return "counting"; }
    bool solve(const Matrix& a, const std::vector<double>& b, std::vector<double>& x) const override {
        ++*calls_;
        return inner_->solve(a, b, x);
    }
    [[nodiscard]] Matrix multiply(const Matrix& a, const Matrix& b) const override {
        ++*calls_;
        return inner_->multiply(a, b);
    }
    bool dampedLeastSquares(const Matrix& j, const std::vector<double>& e, double d, std::vector<double>& s) const override {
        ++*calls_;
        return inner_->dampedLeastSquares(j, e, d, s);
    }

private:
    std::shared_ptr<LinearBackend> inner_;
    int* calls_;
};

}

TEST(Linalg, BackendsAreInterchangeable) {
    int calls = 0;
    setLinearBackend(std::make_shared<CountingBackend>(makeEigenBackend(), &calls));
    EXPECT_EQ(linalg().name(), "counting");
    std::vector<double> x;
    EXPECT_TRUE(linalg().solve(Matrix(1, 1, std::vector<double>{2}), {4}, x));
    EXPECT_EQ(calls, 1);
    setLinearBackend(nullptr);
    EXPECT_EQ(linalg().name(), "eigen");
}

TEST(BigInt, ArithmeticBeyondSixtyFourBits) {
    const auto big = *BigInt::parse("123456789012345678901234567890");
    EXPECT_EQ((big + BigInt(10)).toString(), "123456789012345678901234567900");
    EXPECT_EQ((big * big).toString(), "15241578753238836750495351562536198787501905199875019052100");
    EXPECT_EQ((big - big - BigInt(1)).toString(), "-1");
    EXPECT_EQ(big.divide(BigInt(10))->toString(), "12345678901234567890123456789");
    EXPECT_EQ(big.modulo(BigInt(97))->toString(), "52");
    EXPECT_FALSE(big.toInt64());
    EXPECT_EQ(BigInt(-42).toInt64().value(), -42);
    EXPECT_GT(big.bitLength(), 64U);
    EXPECT_EQ(BigInt(0).bitLength(), 0U);
}

TEST(BigInt, PowFactorialGcdAndModularExponent) {
    EXPECT_EQ(BigInt(2).pow(100).toString(), "1267650600228229401496703205376");
    EXPECT_EQ(BigInt::factorial(25)->toString(), "15511210043330985984000000");
    EXPECT_EQ(BigInt(48).gcd(BigInt(-180)).toString(), "12");
    EXPECT_EQ(BigInt(4).powMod(BigInt(13), BigInt(497))->toString(), "445");
    EXPECT_EQ(BigInt(3).powMod(BigInt(-1), BigInt(7))->toString(), "5");
    EXPECT_FALSE(BigInt(2).powMod(BigInt(3), BigInt(0)));
    EXPECT_FALSE(BigInt::factorial(1000000));
    EXPECT_TRUE(BigInt::parse("170141183460469231731687303715884105727")->isProbablePrime());
    EXPECT_FALSE(BigInt(91).isProbablePrime());
}

TEST(BigInt, RejectsBadInputAndDivisionByZero) {
    for (const char* bad : {"", "-", "+", "12a", "1.5", " 1", "0x10"}) EXPECT_FALSE(BigInt::parse(bad)) << bad;
    EXPECT_FALSE(BigInt(5).divide(BigInt(0)));
    EXPECT_FALSE(BigInt(5).modulo(BigInt(0)));
    EXPECT_EQ(BigInt::parse("+7")->toString(), "7");
    EXPECT_EQ(BigInt::parse("-0")->toString(), "0");
}

TEST(BigInt, ComparesAndOrders) {
    const auto a = *BigInt::parse("100000000000000000000");
    EXPECT_TRUE(BigInt(5) < a);
    EXPECT_TRUE(-a < BigInt(0));
    EXPECT_EQ(a.compare(a), 0);
    EXPECT_EQ(a.sign(), 1);
    EXPECT_EQ((-a).sign(), -1);
}
