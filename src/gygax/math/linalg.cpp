#include <gygax/math/linalg.hpp>

#include <Eigen/Dense>
#include <mutex>

namespace gygax::math {

namespace {

using RowMajor = Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;

Eigen::Map<const RowMajor> view(const Matrix& m) {
    return Eigen::Map<const RowMajor>(m.data().data(), static_cast<Eigen::Index>(m.rows()), static_cast<Eigen::Index>(m.cols()));
}

Eigen::Map<const Eigen::VectorXd> view(const std::vector<double>& v) {
    return Eigen::Map<const Eigen::VectorXd>(v.data(), static_cast<Eigen::Index>(v.size()));
}

std::vector<double> toVector(const Eigen::VectorXd& v) {
    return {v.data(), v.data() + v.size()};
}

class EigenBackend final : public LinearBackend {
public:
    [[nodiscard]] std::string name() const override { return "eigen"; }

    bool solve(const Matrix& a, const std::vector<double>& b, std::vector<double>& x) const override {
        if (a.rows() != a.cols() || a.rows() != b.size() || a.rows() == 0) return false;
        Eigen::FullPivLU<RowMajor> lu(view(a));
        if (!lu.isInvertible()) return false;
        x = toVector(lu.solve(view(b)));
        return true;
    }

    [[nodiscard]] Matrix multiply(const Matrix& a, const Matrix& b) const override {
        if (a.cols() != b.rows()) return {};
        Matrix out(a.rows(), b.cols());
        Eigen::Map<RowMajor>(out.data().data(), static_cast<Eigen::Index>(out.rows()), static_cast<Eigen::Index>(out.cols())) =
            view(a) * view(b);
        return out;
    }

    bool dampedLeastSquares(const Matrix& jacobian, const std::vector<double>& error, double damping,
                            std::vector<double>& step) const override {
        if (jacobian.rows() != error.size() || jacobian.rows() == 0 || jacobian.cols() == 0) return false;
        const auto j = view(jacobian);
        RowMajor gram = j * j.transpose();
        gram.diagonal().array() += damping * damping;
        Eigen::LDLT<RowMajor> ldlt(gram);
        if (ldlt.info() != Eigen::Success) return false;
        step = toVector(j.transpose() * ldlt.solve(view(error)));
        return true;
    }
};

std::mutex& backendMutex() {
    static std::mutex m;
    return m;
}

std::shared_ptr<LinearBackend>& current() {
    static std::shared_ptr<LinearBackend> b = makeEigenBackend();
    return b;
}

}

std::shared_ptr<LinearBackend> makeEigenBackend() {
    return std::make_shared<EigenBackend>();
}

LinearBackend& linalg() {
    std::lock_guard lock(backendMutex());
    return *current();
}

void setLinearBackend(std::shared_ptr<LinearBackend> backend) {
    std::lock_guard lock(backendMutex());
    current() = backend ? std::move(backend) : makeEigenBackend();
}

}
