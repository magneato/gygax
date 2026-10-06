#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <gygax/core/log.hpp>

namespace gygax {

#if defined(__clang__)
#define GYGAX_FLAG_ENUM [[clang::flag_enum]]
#else
#define GYGAX_FLAG_ENUM
#endif

enum class GYGAX_FLAG_ENUM Capability : uint64_t {
    None = 0,
    Perception = 1ULL << 0,
    Cognition = 1ULL << 1,
    Actuation = 1ULL << 2,
    Manipulation = 1ULL << 3,
    Locomotion = 1ULL << 4,
    Communication = 1ULL << 5,
    QuantumCompute = 1ULL << 6,
    PneumaticControl = 1ULL << 7,
    Neuromorphic = 1ULL << 8
};

inline Capability operator|(Capability a, Capability b) {
    return static_cast<Capability>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}

inline bool hasCapability(Capability set, Capability required) {
    return (static_cast<uint64_t>(set) & static_cast<uint64_t>(required)) == static_cast<uint64_t>(required);
}

class Object {
public:
    virtual ~Object() = default;
    virtual void initialize() = 0;
    virtual void freeze(bool frozen) = 0;
    virtual void shutdown() = 0;
    virtual void pre_delete() = 0;
    virtual void reset() = 0;
    virtual std::string diff(const Object& other) const = 0;
    virtual bool compatible(Capability required) const = 0;
    virtual bool compatible(const Object& other) const = 0;
    virtual Capability capabilities() const = 0;
    virtual const std::string& name() const = 0;

    virtual void sendMessage(const std::string& target, const std::string& msg) = 0;
    virtual void onMessage(const std::string& sender, const std::string& msg) = 0;
    virtual void triggerEvent(const std::string& type, const std::string& data) = 0;
    virtual void onEvent(const std::string& type, const std::string& sender, const std::string& data) = 0;
};

constexpr size_t DEFAULT_MAX_THOUGHTS = 32;

class BaseObject : virtual public Object {
public:
    explicit BaseObject(std::string name, Capability caps = Capability::None);

    void initialize() override;
    void freeze(bool f) override {
        frozen_ = f;
        log::debug(name_, "frozen={}", frozen_);
    }
    void shutdown() override { log::debug(name_, "shutdown"); }
    void pre_delete() override;
    void reset() override {
        frozen_ = false;
        log::debug(name_, "reset");
    }

    Capability capabilities() const override { return caps_; }
    const std::string& name() const override { return name_; }
    bool compatible(Capability req) const override { return hasCapability(caps_, req); }
    bool compatible(const Object& other) const override {
        return (static_cast<uint64_t>(caps_) & static_cast<uint64_t>(other.capabilities())) != 0;
    }
    std::string diff(const Object& other) const override {
        return std::format("{} <-> {}: capability delta 0x{:X}", name_, other.name(),
                           static_cast<uint64_t>(caps_) ^ static_cast<uint64_t>(other.capabilities()));
    }

    void sendMessage(const std::string& target, const std::string& msg) override;
    void onMessage(const std::string& sender, const std::string& msg) override { log::debug(name_, "message from {}: {}", sender, msg); }
    void triggerEvent(const std::string& type, const std::string& data) override;
    void onEvent(const std::string& type, const std::string& sender, const std::string& data) override {
        log::debug(name_, "event '{}' from {}: {}", type, sender, data);
    }

    virtual void addGoal(const std::string& goal) { goals_.push_back(goal); }
    virtual const std::vector<std::string>& goals() const { return goals_; }
    virtual void pushThought(const std::string& context);
    virtual std::string summarizeThoughts() const;
    const std::vector<std::string>& thoughts() const { return thoughts_; }
    size_t maxThoughts() const { return maxThoughts_; }
    void setMaxThoughts(size_t limit) { maxThoughts_ = limit; }

    uint32_t sid() const { return sid_; }
    void setSid(uint32_t id) { sid_ = id; }

    using Parameter = std::variant<int, float, std::string, bool>;
    void setParameter(const std::string& key, Parameter val);
    const Parameter* getParameter(const std::string& key) const;
    bool frozen() const { return frozen_; }

protected:
    std::string name_;
    Capability caps_;
    bool frozen_;
    uint32_t sid_;
    size_t maxThoughts_;
    std::vector<std::string> goals_;
    std::vector<std::string> thoughts_;
    std::unordered_map<std::string, Parameter> params_;
};

}
