#pragma once

#include "platform_socket.h"

#include <cstdint>
#include <functional>

namespace TqReactorEvents {
constexpr uint32_t Read = 0x1;
constexpr uint32_t Write = 0x2;
constexpr uint32_t Error = 0x4;
} // namespace TqReactorEvents

class ITqSocketReactor {
public:
    using Handler = std::function<void(TqSocketHandle fd, uint32_t events)>;

    virtual ~ITqSocketReactor() = default;
    virtual bool Start() = 0;
    virtual void Stop() = 0;
    virtual bool Add(TqSocketHandle fd, uint32_t events, Handler handler) = 0;
    virtual bool Modify(TqSocketHandle fd, uint32_t events) = 0;
    virtual bool Remove(TqSocketHandle fd) = 0;
    virtual bool Wake() = 0;
    virtual bool RunOnce(int timeoutMs) = 0;
};
