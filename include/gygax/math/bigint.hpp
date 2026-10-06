#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace gygax::math {

inline constexpr int kDefaultPrimalityTestRounds = 25;

class IntegerRep {
public:
    virtual ~IntegerRep() = default;
    [[nodiscard]] virtual std::shared_ptr<const IntegerRep> add(const IntegerRep& other) const = 0;
    [[nodiscard]] virtual std::shared_ptr<const IntegerRep> sub(const IntegerRep& other) const = 0;
    [[nodiscard]] virtual std::shared_ptr<const IntegerRep> mul(const IntegerRep& other) const = 0;
    [[nodiscard]] virtual std::shared_ptr<const IntegerRep> divide(const IntegerRep& other) const = 0;
    [[nodiscard]] virtual std::shared_ptr<const IntegerRep> modulo(const IntegerRep& other) const = 0;
    [[nodiscard]] virtual std::shared_ptr<const IntegerRep> pow(unsigned exponent) const = 0;
    [[nodiscard]] virtual std::shared_ptr<const IntegerRep> powMod(const IntegerRep& exponent, const IntegerRep& modulus) const = 0;
    [[nodiscard]] virtual std::shared_ptr<const IntegerRep> gcd(const IntegerRep& other) const = 0;
    [[nodiscard]] virtual std::shared_ptr<const IntegerRep> negate() const = 0;
    [[nodiscard]] virtual int compare(const IntegerRep& other) const = 0;
    [[nodiscard]] virtual int sign() const = 0;
    [[nodiscard]] virtual std::size_t bitLength() const = 0;
    [[nodiscard]] virtual bool isProbablePrime(int rounds) const = 0;
    [[nodiscard]] virtual std::string toString() const = 0;
    [[nodiscard]] virtual std::optional<std::int64_t> toInt64() const = 0;
};

class IntegerBackend {
public:
    virtual ~IntegerBackend() = default;
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual std::shared_ptr<const IntegerRep> fromInt64(std::int64_t value) const = 0;
    [[nodiscard]] virtual std::shared_ptr<const IntegerRep> parse(std::string_view decimal) const = 0;
};

std::shared_ptr<IntegerBackend> makeGmpBackend();
IntegerBackend& integerBackend();
void setIntegerBackend(std::shared_ptr<IntegerBackend> backend);

class BigInt {
public:
    BigInt();
    BigInt(std::int64_t value);
    static std::optional<BigInt> parse(std::string_view decimal);
    static std::optional<BigInt> factorial(unsigned n);

    [[nodiscard]] BigInt operator+(const BigInt& o) const;
    [[nodiscard]] BigInt operator-(const BigInt& o) const;
    [[nodiscard]] BigInt operator*(const BigInt& o) const;
    [[nodiscard]] BigInt operator-() const;
    [[nodiscard]] std::optional<BigInt> divide(const BigInt& o) const;
    [[nodiscard]] std::optional<BigInt> modulo(const BigInt& o) const;
    [[nodiscard]] BigInt pow(unsigned exponent) const;
    [[nodiscard]] std::optional<BigInt> powMod(const BigInt& exponent, const BigInt& modulus) const;
    [[nodiscard]] BigInt gcd(const BigInt& o) const;
    [[nodiscard]] int compare(const BigInt& o) const;
    [[nodiscard]] bool operator==(const BigInt& o) const { return compare(o) == 0; }
    [[nodiscard]] bool operator<(const BigInt& o) const { return compare(o) < 0; }
    [[nodiscard]] int sign() const;
    [[nodiscard]] std::size_t bitLength() const;
    [[nodiscard]] bool isProbablePrime(int rounds = kDefaultPrimalityTestRounds) const;
    [[nodiscard]] std::string toString() const;
    [[nodiscard]] std::optional<std::int64_t> toInt64() const;

private:
    explicit BigInt(std::shared_ptr<const IntegerRep> rep) : rep_(std::move(rep)) {}
    [[nodiscard]] std::shared_ptr<const IntegerRep> adopt(const BigInt& o) const;

    std::shared_ptr<const IntegerRep> rep_;
};

}
