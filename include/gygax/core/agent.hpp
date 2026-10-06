#pragma once
#include <gygax/core/object.hpp>
#include <memory>
#include <cstdint>

namespace gygax {

namespace messaging {
struct Message;
}

class AbstractAgent : virtual public gygax::Object {
public:
    virtual ~AbstractAgent() = default;

    [[nodiscard]] virtual bool receiveMessage(const messaging::Message& message) = 0;

    virtual void emitEvent(uint32_t eventId, const void* payload, size_t size) = 0;

    [[nodiscard]] virtual uint32_t sid() const = 0;
};

[[nodiscard]] std::unique_ptr<AbstractAgent> CreateDefaultAgent();

}
