#include <gygax/math/bigint.hpp>

#include <gmpxx.h>

#include <limits>
#include <mutex>
#include <typeinfo>

namespace gygax::math {

namespace {

constexpr unsigned kMaximumFactorialInput = 20000;

class GmpRep final : public IntegerRep {
public:
    explicit GmpRep(mpz_class v) : v_(std::move(v)) {}

    [[nodiscard]] const mpz_class& value() const { return v_; }

    [[nodiscard]] std::shared_ptr<const IntegerRep> add(const IntegerRep& o) const override { return make(v_ + cast(o)); }
    [[nodiscard]] std::shared_ptr<const IntegerRep> sub(const IntegerRep& o) const override { return make(v_ - cast(o)); }
    [[nodiscard]] std::shared_ptr<const IntegerRep> mul(const IntegerRep& o) const override { return make(v_ * cast(o)); }
    [[nodiscard]] std::shared_ptr<const IntegerRep> divide(const IntegerRep& o) const override {
        if (cast(o) == 0) return nullptr;
        return make(mpz_class(v_ / cast(o)));
    }
    [[nodiscard]] std::shared_ptr<const IntegerRep> modulo(const IntegerRep& o) const override {
        if (cast(o) == 0) return nullptr;
        return make(mpz_class(v_ % cast(o)));
    }
    [[nodiscard]] std::shared_ptr<const IntegerRep> pow(unsigned e) const override {
        mpz_class out;
        mpz_pow_ui(out.get_mpz_t(), v_.get_mpz_t(), e);
        return make(std::move(out));
    }
    [[nodiscard]] std::shared_ptr<const IntegerRep> powMod(const IntegerRep& e, const IntegerRep& m) const override {
        const auto& exp = cast(e);
        const auto& mod = cast(m);
        if (mod == 0) return nullptr;
        mpz_class out;
        if (exp < 0) {
            mpz_class inv;
            if (mpz_invert(inv.get_mpz_t(), v_.get_mpz_t(), mod.get_mpz_t()) == 0) return nullptr;
            mpz_class pos = -exp;
            mpz_powm(out.get_mpz_t(), inv.get_mpz_t(), pos.get_mpz_t(), mod.get_mpz_t());
        } else {
            mpz_powm(out.get_mpz_t(), v_.get_mpz_t(), exp.get_mpz_t(), mod.get_mpz_t());
        }
        return make(std::move(out));
    }
    [[nodiscard]] std::shared_ptr<const IntegerRep> gcd(const IntegerRep& o) const override {
        mpz_class out;
        mpz_gcd(out.get_mpz_t(), v_.get_mpz_t(), cast(o).get_mpz_t());
        return make(std::move(out));
    }
    [[nodiscard]] std::shared_ptr<const IntegerRep> negate() const override { return make(-v_); }
    [[nodiscard]] int compare(const IntegerRep& o) const override {
        const int c = cmp(v_, cast(o));
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    [[nodiscard]] int sign() const override { return sgn(v_); }
    [[nodiscard]] std::size_t bitLength() const override { return v_ == 0 ? 0 : mpz_sizeinbase(v_.get_mpz_t(), 2); }
    [[nodiscard]] bool isProbablePrime(int rounds) const override { return mpz_probab_prime_p(v_.get_mpz_t(), rounds) > 0; }
    [[nodiscard]] std::string toString() const override { return v_.get_str(10); }
    [[nodiscard]] std::optional<std::int64_t> toInt64() const override {
        if (!v_.fits_slong_p()) return std::nullopt;
        return static_cast<std::int64_t>(v_.get_si());
    }

private:
    static std::shared_ptr<const IntegerRep> make(mpz_class v) { return std::make_shared<GmpRep>(std::move(v)); }
    static const mpz_class& cast(const IntegerRep& r) { return static_cast<const GmpRep&>(r).value(); }

    mpz_class v_;
};

class GmpBackend final : public IntegerBackend {
public:
    [[nodiscard]] std::string name() const override { return "gmp"; }
    [[nodiscard]] std::shared_ptr<const IntegerRep> fromInt64(std::int64_t value) const override {
        return std::make_shared<GmpRep>(mpz_class(static_cast<long>(value)));
    }
    [[nodiscard]] std::shared_ptr<const IntegerRep> parse(std::string_view text) const override {
        if (text.empty()) return nullptr;
        std::size_t start = (text.front() == '-' || text.front() == '+') ? 1 : 0;
        if (start == text.size()) return nullptr;
        for (std::size_t i = start; i < text.size(); ++i)
            if (text[i] < '0' || text[i] > '9') return nullptr;
        mpz_class v;
        if (v.set_str(std::string(text.front() == '+' ? text.substr(1) : text), 10) != 0) return nullptr;
        return std::make_shared<GmpRep>(std::move(v));
    }
};

std::mutex& backendMutex() {
    static std::mutex m;
    return m;
}

std::shared_ptr<IntegerBackend>& current() {
    static std::shared_ptr<IntegerBackend> b = makeGmpBackend();
    return b;
}

std::shared_ptr<IntegerBackend> currentShared() {
    std::lock_guard lock(backendMutex());
    return current();
}

}

std::shared_ptr<IntegerBackend> makeGmpBackend() {
    return std::make_shared<GmpBackend>();
}

IntegerBackend& integerBackend() {
    return *currentShared();
}

void setIntegerBackend(std::shared_ptr<IntegerBackend> backend) {
    std::lock_guard lock(backendMutex());
    current() = backend ? std::move(backend) : makeGmpBackend();
}

BigInt::BigInt() : rep_(integerBackend().fromInt64(0)) {}

BigInt::BigInt(std::int64_t value) : rep_(integerBackend().fromInt64(value)) {}

std::optional<BigInt> BigInt::parse(std::string_view decimal) {
    auto rep = integerBackend().parse(decimal);
    if (!rep) return std::nullopt;
    return BigInt(std::move(rep));
}

std::optional<BigInt> BigInt::factorial(unsigned n) {
    if (n > kMaximumFactorialInput) return std::nullopt;
    BigInt acc(1);
    for (unsigned i = 2; i <= n; ++i) acc = acc * BigInt(static_cast<std::int64_t>(i));
    return acc;
}

std::shared_ptr<const IntegerRep> BigInt::adopt(const BigInt& o) const {
    const IntegerRep& mine = *rep_;
    const IntegerRep& theirs = *o.rep_;
    if (typeid(mine) == typeid(theirs)) return o.rep_;
    auto converted = integerBackend().parse(o.toString());
    return converted ? converted : o.rep_;
}

BigInt BigInt::operator+(const BigInt& o) const {
    return BigInt(rep_->add(*adopt(o)));
}
BigInt BigInt::operator-(const BigInt& o) const {
    return BigInt(rep_->sub(*adopt(o)));
}
BigInt BigInt::operator*(const BigInt& o) const {
    return BigInt(rep_->mul(*adopt(o)));
}
BigInt BigInt::operator-() const {
    return BigInt(rep_->negate());
}

std::optional<BigInt> BigInt::divide(const BigInt& o) const {
    auto r = rep_->divide(*adopt(o));
    if (!r) return std::nullopt;
    return BigInt(std::move(r));
}

std::optional<BigInt> BigInt::modulo(const BigInt& o) const {
    auto r = rep_->modulo(*adopt(o));
    if (!r) return std::nullopt;
    return BigInt(std::move(r));
}

BigInt BigInt::pow(unsigned exponent) const {
    return BigInt(rep_->pow(exponent));
}

std::optional<BigInt> BigInt::powMod(const BigInt& exponent, const BigInt& modulus) const {
    auto r = rep_->powMod(*adopt(exponent), *adopt(modulus));
    if (!r) return std::nullopt;
    return BigInt(std::move(r));
}

BigInt BigInt::gcd(const BigInt& o) const {
    return BigInt(rep_->gcd(*adopt(o)));
}
int BigInt::compare(const BigInt& o) const {
    return rep_->compare(*adopt(o));
}
int BigInt::sign() const {
    return rep_->sign();
}
std::size_t BigInt::bitLength() const {
    return rep_->bitLength();
}
bool BigInt::isProbablePrime(int rounds) const {
    return rep_->isProbablePrime(rounds);
}
std::string BigInt::toString() const {
    return rep_->toString();
}
std::optional<std::int64_t> BigInt::toInt64() const {
    return rep_->toInt64();
}

}
