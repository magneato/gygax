#include <gygax/service/expression.hpp>

#include <cctype>
#include <charconv>

#include <gygax/core/charconv.hpp>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace gygax::service {

namespace {

constexpr int kMaximumExpressionNestingDepth = 64;
constexpr std::size_t kMaximumExpressionLengthBytes = 2048;
constexpr double kPi = 3.14159265358979323846;
constexpr double kEulerNumber = 2.71828182845904523536;

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    bool run(double& out, std::string& error) {
        try {
            out = expression();
            skip();
            if (pos_ != text_.size()) throw std::runtime_error("unexpected character '" + std::string(1, text_[pos_]) + "'");
            if (!std::isfinite(out)) throw std::runtime_error("result is not finite");
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }

private:
    void skip() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_])) != 0) ++pos_;
    }

    bool eat(char c) {
        skip();
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    double expression() {
        double value = term();
        while (true) {
            if (eat('+'))
                value += term();
            else if (eat('-'))
                value -= term();
            else
                return value;
        }
    }

    double term() {
        double value = unary();
        while (true) {
            if (eat('*'))
                value *= unary();
            else if (eat('/')) {
                const double divisor = unary();
                if (divisor == 0.0) throw std::runtime_error("division by zero");
                value /= divisor;
            } else if (eat('%')) {
                const double divisor = unary();
                if (divisor == 0.0) throw std::runtime_error("modulo by zero");
                value = std::fmod(value, divisor);
            } else
                return value;
        }
    }

    double unary() {
        if (eat('-')) return -unary();
        if (eat('+')) return unary();
        return power();
    }

    double power() {
        const double base = primary();
        if (eat('^')) return std::pow(base, unary());
        return base;
    }

    double primary() {
        skip();
        if (++depth_ > kMaximumExpressionNestingDepth) throw std::runtime_error("expression nested too deeply");
        struct Guard {
            int& d;
            ~Guard() { --d; }
        } guard{depth_};
        if (eat('(')) {
            const double v = expression();
            if (!eat(')')) throw std::runtime_error("missing ')'");
            return v;
        }
        if (pos_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0 || text_[pos_] == '.')) {
            double v = 0.0;
            auto [ptr, ec] = gygax::fromChars(text_.data() + pos_, text_.data() + text_.size(), v);
            if (ec != std::errc()) throw std::runtime_error("invalid number");
            pos_ = static_cast<std::size_t>(ptr - text_.data());
            return v;
        }
        std::string name;
        while (pos_ < text_.size() && (std::isalpha(static_cast<unsigned char>(text_[pos_])) != 0 || text_[pos_] == '_'))
            name.push_back(text_[pos_++]);
        if (name.empty())
            throw std::runtime_error(pos_ >= text_.size() ? "unexpected end of expression"
                                                          : "unexpected character '" + std::string(1, text_[pos_]) + "'");
        if (name == "pi") return kPi;
        if (name == "e") return kEulerNumber;
        if (!eat('(')) throw std::runtime_error("unknown constant '" + name + "'");
        std::vector<double> args;
        if (!eat(')')) {
            do {
                args.push_back(expression());
            } while (eat(','));
            if (!eat(')')) throw std::runtime_error("missing ')' after arguments");
        }
        return call(name, args);
    }

    static double call(const std::string& name, const std::vector<double>& a) {
        auto need = [&](std::size_t n) {
            if (a.size() != n) throw std::runtime_error(name + " expects " + std::to_string(n) + " argument(s)");
        };
        if (name == "sqrt") {
            need(1);
            if (a[0] < 0.0) throw std::runtime_error("sqrt of a negative number");
            return std::sqrt(a[0]);
        }
        if (name == "abs") {
            need(1);
            return std::fabs(a[0]);
        }
        if (name == "sin") {
            need(1);
            return std::sin(a[0]);
        }
        if (name == "cos") {
            need(1);
            return std::cos(a[0]);
        }
        if (name == "tan") {
            need(1);
            return std::tan(a[0]);
        }
        if (name == "exp") {
            need(1);
            return std::exp(a[0]);
        }
        if (name == "ln") {
            need(1);
            if (a[0] <= 0.0) throw std::runtime_error("ln of a non-positive number");
            return std::log(a[0]);
        }
        if (name == "log10") {
            need(1);
            if (a[0] <= 0.0) throw std::runtime_error("log10 of a non-positive number");
            return std::log10(a[0]);
        }
        if (name == "floor") {
            need(1);
            return std::floor(a[0]);
        }
        if (name == "ceil") {
            need(1);
            return std::ceil(a[0]);
        }
        if (name == "round") {
            need(1);
            return std::round(a[0]);
        }
        if (name == "min") {
            need(2);
            return std::min(a[0], a[1]);
        }
        if (name == "max") {
            need(2);
            return std::max(a[0], a[1]);
        }
        if (name == "pow") {
            need(2);
            return std::pow(a[0], a[1]);
        }
        throw std::runtime_error("unknown function '" + name + "'");
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    int depth_ = 0;
};

}

bool evaluateExpression(std::string_view text, double& result, std::string& error) {
    if (text.size() > kMaximumExpressionLengthBytes) {
        error = "expression too long";
        return false;
    }
    return Parser(text).run(result, error);
}

}
