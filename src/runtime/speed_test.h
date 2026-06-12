#pragma once

#include <cstdint>
#include <string>

#include "control_protocol.h"

class TqEphemeralTargetAuthorizer {
public:
    virtual ~TqEphemeralTargetAuthorizer() = default;
    virtual bool IsAllowedEphemeralTarget(const std::string& host, uint16_t port) const = 0;
};

class TqServerSpeedTestController final : public TqEphemeralTargetAuthorizer {
public:
    TqServerSpeedTestController();
    ~TqServerSpeedTestController();

    bool StartSession(const TqSpeedStart& start, TqSpeedReady& ready);
    bool FinishSession(uint32_t sessionId, uint64_t clientBytes, uint64_t clientElapsedUs, TqSpeedResult& result);
    void StopAll();
    bool IsAllowedEphemeralTarget(const std::string& host, uint16_t port) const override;

private:
    struct Impl;
    Impl* Impl_;
};
