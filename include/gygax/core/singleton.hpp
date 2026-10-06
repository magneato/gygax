#pragma once

namespace gygax {

template <typename DerivedType> class Singleton {
public:
    static DerivedType& getInstance() noexcept {
        static DerivedType instance;
        return instance;
    }

    Singleton(const Singleton&) = delete;
    Singleton& operator=(const Singleton&) = delete;
    Singleton(Singleton&&) = delete;
    Singleton& operator=(Singleton&&) = delete;

protected:
    Singleton() = default;
    ~Singleton() = default;
};

}
