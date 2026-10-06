#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace gygax::math {

class Matrix {
public:
    Matrix() = default;
    Matrix(std::size_t rows, std::size_t cols, double fill = 0.0) : rows_(rows), cols_(cols), data_(rows * cols, fill) {}
    Matrix(std::size_t rows, std::size_t cols, std::vector<double> rowMajor) : rows_(rows), cols_(cols), data_(std::move(rowMajor)) {}

    [[nodiscard]] std::size_t rows() const { return rows_; }
    [[nodiscard]] std::size_t cols() const { return cols_; }
    [[nodiscard]] double& operator()(std::size_t r, std::size_t c) { return data_[r * cols_ + c]; }
    [[nodiscard]] double operator()(std::size_t r, std::size_t c) const { return data_[r * cols_ + c]; }
    [[nodiscard]] const std::vector<double>& data() const { return data_; }
    [[nodiscard]] std::vector<double>& data() { return data_; }

private:
    std::size_t rows_ = 0;
    std::size_t cols_ = 0;
    std::vector<double> data_;
};

class LinearBackend {
public:
    virtual ~LinearBackend() = default;
    [[nodiscard]] virtual std::string name() const = 0;
    virtual bool solve(const Matrix& a, const std::vector<double>& b, std::vector<double>& x) const = 0;
    [[nodiscard]] virtual Matrix multiply(const Matrix& a, const Matrix& b) const = 0;
    virtual bool dampedLeastSquares(const Matrix& jacobian, const std::vector<double>& error, double damping,
                                    std::vector<double>& step) const = 0;
};

std::shared_ptr<LinearBackend> makeEigenBackend();

LinearBackend& linalg();
void setLinearBackend(std::shared_ptr<LinearBackend> backend);

}
