module;
#include <cstdint>
#include <string>

export module gygax.messaging;

// extern "C++" attaches these to the global module, so they are the same entities that
// include/gygax/core/agent.hpp forward-declares outside any module (newer clang rejects
// a module-attached definition of a type declared outside the module).
export extern "C++" {
    namespace gygax::messaging {

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

    } // namespace gygax::messaging
}
