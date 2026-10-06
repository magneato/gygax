module;
#include <cstdint>
#include <string>

export module gygax.messaging;

export namespace gygax::messaging {

struct Message {
    uint32_t senderId;
    uint32_t receiverId;
    std::string payload;
};

class MessageObserver {
public:
    virtual ~MessageObserver() = default;
    virtual void onMessage(const Message& msg) = 0;
};

}
