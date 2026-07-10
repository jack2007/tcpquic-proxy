#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "control_protocol.h"
#include "msquic.hpp"
#include "platform_socket.h"

struct TqConfig;
struct MsQuicStream;
struct MsQuicConnection;
class TqStreamLifetime;

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

bool TqRunIngressClientSpeedTest(MsQuicConnection& controlConn, const TqConfig& cfg);
bool TqOpenIngressSpeedTestConnection(
    const TqConfig& cfg,
    const TqSpeedReady& ready,
    TqSocketHandle& outSocket,
    std::vector<uint8_t>& leftover,
    std::string& err);
void TqTuneSpeedTestLocalSocket(TqSocketHandle socket);
uint64_t TqSpeedByteMismatchLimit(uint64_t highBytes);
bool TqSpeedByteCountsCloseEnough(uint64_t localBytes, uint64_t serverBytes, uint64_t& diffOut, uint64_t& limitOut);
bool TqSpeedConnectWaitShouldStop(uint32_t connected, uint32_t needed, bool deadlineReached);
void TqHandleServerSpeedControlStream(
    TqServerSpeedTestController& controller,
    MsQuicConnection* conn,
    HQUIC rawStream);

bool TqAttachServerSpeedControlStreamManaged(
    TqServerSpeedTestController& controller,
    MsQuicConnection* conn,
    std::shared_ptr<TqStreamLifetime> owner,
    std::vector<uint8_t> initialBytes,
    std::function<void()> onComplete = {});
