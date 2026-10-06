#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <gygax/bus/can.hpp>
#include <gygax/core/json.hpp>

namespace gygax::robotics {

struct CommandResult {
    int status = 200;
    json::Value body = json::Value::object();

    static CommandResult ok(json::Value body = json::Value::object());
    static CommandResult error(int status, std::string_view code, std::string_view message);
};

class Device {
public:
    virtual ~Device() = default;
    [[nodiscard]] virtual std::string kind() const = 0;
    [[nodiscard]] virtual std::string endpoint() const = 0;
    [[nodiscard]] virtual bool connected() const = 0;
    [[nodiscard]] virtual json::Value state() = 0;
    [[nodiscard]] virtual std::vector<std::string> commands() const = 0;
    virtual CommandResult command(const json::Value& request) = 0;
};

std::shared_ptr<bus::LoopbackCanNetwork> virtualCanNetwork(const std::string& name);

std::unique_ptr<Device> openDevice(const json::Value& config, std::string* error);
std::vector<std::string> deviceKinds();
bool serialPathAllowed(const std::string& path);

class DeviceRegistry {
public:
    bool add(const std::string& id, std::unique_ptr<Device> device, std::string* error = nullptr);
    bool remove(const std::string& id);
    [[nodiscard]] std::shared_ptr<Device> get(const std::string& id) const;
    [[nodiscard]] std::vector<std::string> ids() const;
    [[nodiscard]] json::Value list() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Device>> devices_;
};

} // namespace gygax::robotics
