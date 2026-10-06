#pragma once
#include <gygax/core/agent.hpp>
#include <gygax/transport/streams.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <type_traits>

namespace gygax::taxonomy {

class Brain : public gygax::AbstractAgent {
public:
    virtual ~Brain() = default;

    virtual void attachStream(std::shared_ptr<comm::Stream> stream) = 0;
    [[nodiscard]] virtual std::shared_ptr<comm::Stream> getStream(const std::string& name) const = 0;
    [[nodiscard]] virtual std::string telemetry() const { return "{}"; }
    virtual void advance(double dtSeconds) { (void)dtSeconds; }
};

class Effector : public Brain {
public:
    virtual ~Effector() = default;
    virtual void action(float x, float y, float z) = 0;
};

namespace Computer {
class Capability : public gygax::taxonomy::Brain {
public:
    virtual ~Capability() = default;
    virtual void runTask(const std::string& script) = 0;
};
}

namespace Mobility {
class Vehicle : public gygax::taxonomy::Effector {
public:
    virtual ~Vehicle() = default;
    virtual void steer(float angle) = 0;
    virtual void setThrottle(float percentage) = 0;
    virtual void applyBraking(float force) = 0;
};
}

namespace Marine {
class Vessel : public gygax::taxonomy::Effector {
public:
    virtual ~Vessel() = default;
    virtual void setRudder(float angle) = 0;
    virtual void setPropulsion(float thrust) = 0;
};
}

namespace Aerospace {
class UAV : public gygax::taxonomy::Effector {
public:
    virtual ~UAV() = default;
    virtual void setFlightPath(float pitch, float roll, float yaw) = 0;
    virtual void setAltitude(float meters) = 0;
};

class Drone : public UAV {
public:
    virtual ~Drone() = default;
    virtual void takeoff() = 0;
    virtual void land() = 0;
    virtual void hover() = 0;
};

class HighAltitudePlatform : public gygax::taxonomy::Effector {
public:
    virtual ~HighAltitudePlatform() = default;
    virtual void adjustBallast(float mass) = 0;
    virtual void ventGas(float volume) = 0;
};
}

namespace Space {
class OrbitalConstellation : public gygax::taxonomy::Effector {
public:
    virtual ~OrbitalConstellation() = default;
    virtual void orientSolarPanels(float angle) = 0;
    virtual void pulseThrusters(float duration, float magnitude) = 0;
};
}

namespace Infrastructure {
class StationaryTrafficMonitor : public gygax::taxonomy::Effector {
public:
    virtual ~StationaryTrafficMonitor() = default;
    virtual void panTiltZoom(float pan, float tilt, float zoom) = 0;
    virtual void encodeVideo(int bitrate) = 0;
};
}

namespace Edge {
class MobileEdgeDevice : public gygax::taxonomy::Effector {
public:
    virtual ~MobileEdgeDevice() = default;
    virtual void requestComputeOffload(size_t loadBytes) = 0;
    virtual void vibrate(float intensity) = 0;
};
}

enum class Type {
    Capability,
    Vehicle,
    Vessel,
    UAV,
    Drone,
    HighAltitudePlatform,
    OrbitalConstellation,
    StationaryTrafficMonitor,
    MobileEdgeDevice
};

inline constexpr Type kCapability = Type::Capability;
inline constexpr Type kVehicle = Type::Vehicle;
inline constexpr Type kVessel = Type::Vessel;
inline constexpr Type kUAV = Type::UAV;
inline constexpr Type kDrone = Type::Drone;
inline constexpr Type kHighAltitudePlatform = Type::HighAltitudePlatform;
inline constexpr Type kOrbitalConstellation = Type::OrbitalConstellation;
inline constexpr Type kStationaryTrafficMonitor = Type::StationaryTrafficMonitor;
inline constexpr Type kMobileEdgeDevice = Type::MobileEdgeDevice;

inline std::string normalizeName(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    for (char c : str) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            if (c >= 'A' && c <= 'Z') {
                result.push_back(static_cast<char>(c - 'A' + 'a'));
            } else {
                result.push_back(c);
            }
        }
    }
    return result;
}

void ensure_linked() noexcept;

class TaxonomyRegistry {
public:
    using CreatorFunc = std::unique_ptr<Brain> (*)(uint32_t);

    static TaxonomyRegistry& getInstance() {
        ensure_linked();
        static TaxonomyRegistry instance;
        return instance;
    }

    void registerCreator(Type type, const std::string& name, CreatorFunc creator) {
        creatorsByName_[normalizeName(name)] = creator;
        creatorsByType_[type] = creator;
    }

    std::unique_ptr<Brain> create(Type type, uint32_t id) {
        auto it = creatorsByType_.find(type);
        if (it != creatorsByType_.end()) {
            return it->second(id);
        }
        return nullptr;
    }

    std::unique_ptr<Brain> create(const std::string& name, uint32_t id) {
        std::string norm = normalizeName(name);

        auto it = creatorsByName_.find(norm);
        if (it != creatorsByName_.end()) {
            return it->second(id);
        }

        for (const auto& [regName, creator] : creatorsByName_) {
            if (norm.size() >= regName.size() && norm.compare(norm.size() - regName.size(), regName.size(), regName) == 0) {
                return creator(id);
            }
        }

        return nullptr;
    }

private:
    std::unordered_map<std::string, CreatorFunc> creatorsByName_;
    std::unordered_map<Type, CreatorFunc> creatorsByType_;
};

template <typename ImplClass> class FactoryRegisterer {
public:
    FactoryRegisterer(Type type, const std::string& name) {
        static_assert(!std::is_copy_constructible_v<ImplClass>, "Taxonomy embodiment classes must not be copy constructible!");
        static_assert(!std::is_copy_assignable_v<ImplClass>, "Taxonomy embodiment classes must not be copy assignable!");

        TaxonomyRegistry::getInstance().registerCreator(
            type, name, [](uint32_t id) -> std::unique_ptr<Brain> { return std::make_unique<ImplClass>(id); });
    }
};

#define FACTORY_REGISTER_CONCAT(a, b) a##b
#define FACTORY_REGISTER_UNIQUE(line) FACTORY_REGISTER_CONCAT(reg_var_, line)
#define FACTORY_REGISTER(ClassName)                                                                                                        \
    static inline const ::gygax::taxonomy::FactoryRegisterer<ClassName> FACTORY_REGISTER_UNIQUE(__LINE__)(ClassName::type, ClassName::name)

template <typename T> struct TypeMap;

template <> struct TypeMap<Computer::Capability> {
    static constexpr Type value = Type::Capability;
};
template <> struct TypeMap<Mobility::Vehicle> {
    static constexpr Type value = Type::Vehicle;
};
template <> struct TypeMap<Marine::Vessel> {
    static constexpr Type value = Type::Vessel;
};
template <> struct TypeMap<Aerospace::UAV> {
    static constexpr Type value = Type::UAV;
};
template <> struct TypeMap<Aerospace::Drone> {
    static constexpr Type value = Type::Drone;
};
template <> struct TypeMap<Aerospace::HighAltitudePlatform> {
    static constexpr Type value = Type::HighAltitudePlatform;
};
template <> struct TypeMap<Space::OrbitalConstellation> {
    static constexpr Type value = Type::OrbitalConstellation;
};
template <> struct TypeMap<Infrastructure::StationaryTrafficMonitor> {
    static constexpr Type value = Type::StationaryTrafficMonitor;
};
template <> struct TypeMap<Edge::MobileEdgeDevice> {
    static constexpr Type value = Type::MobileEdgeDevice;
};

template <typename T> inline std::unique_ptr<T> Create(uint32_t id) {
    Type type = TypeMap<T>::value;
    auto obj = TaxonomyRegistry::getInstance().create(type, id);
    return std::unique_ptr<T>(static_cast<T*>(obj.release()));
}

inline std::unique_ptr<Brain> Create(Type type, uint32_t id) {
    return TaxonomyRegistry::getInstance().create(type, id);
}

inline std::unique_ptr<Brain> Create(const std::string& name, uint32_t id) {
    return TaxonomyRegistry::getInstance().create(name, id);
}

std::unique_ptr<Computer::Capability> CreateCapabilityModule(uint32_t id);
std::unique_ptr<Mobility::Vehicle> CreateVehicleModule(uint32_t id);
std::unique_ptr<Marine::Vessel> CreateVesselModule(uint32_t id);
std::unique_ptr<Aerospace::UAV> CreateUAVModule(uint32_t id);
std::unique_ptr<Aerospace::Drone> CreateDroneModule(uint32_t id);
std::unique_ptr<Aerospace::HighAltitudePlatform> CreateHighAltitudePlatformModule(uint32_t id);
std::unique_ptr<Space::OrbitalConstellation> CreateOrbitalConstellationModule(uint32_t id);
std::unique_ptr<Infrastructure::StationaryTrafficMonitor> CreateStationaryTrafficMonitorModule(uint32_t id);
std::unique_ptr<Edge::MobileEdgeDevice> CreateMobileEdgeDeviceModule(uint32_t id);

}
