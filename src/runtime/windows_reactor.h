#pragma once

#include "socket_reactor.h"

#if defined(_WIN32)
#include <cstddef>
#include <unordered_map>
#endif

class TqWindowsReactor final : public ITqSocketReactor {
public:
    TqWindowsReactor() = default;
    ~TqWindowsReactor() override;

    TqWindowsReactor(const TqWindowsReactor&) = delete;
    TqWindowsReactor& operator=(const TqWindowsReactor&) = delete;

    bool Start() override;
    void Stop() override;
    bool Add(TqSocketHandle fd, uint32_t events, Handler handler) override;
    bool Modify(TqSocketHandle fd, uint32_t events) override;
    bool Remove(TqSocketHandle fd) override;
    bool Wake() override;
    bool RunOnce(int timeoutMs) override;

private:
#if defined(_WIN32)
    struct Entry {
        WSAEVENT Event{WSA_INVALID_EVENT};
        uint32_t Events{0};
        Handler Callback;
    };

    WSAEVENT WakeEvent_{WSA_INVALID_EVENT};
    std::unordered_map<TqSocketHandle, Entry> Entries_;
    size_t NextWaitOffset_{0};
#else
    bool Started_{false};
#endif
};
