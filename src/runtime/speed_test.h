#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "control_protocol.h"
#include "msquic.hpp"
#include "platform_socket.h"

struct TqConfig;
class QuicClientSession;
class MsQuicStream;
struct MsQuicConnection;

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

bool TqRunClientSpeedTest(QuicClientSession& quic, const TqConfig& cfg);
TqConfig TqMakeSpeedClientSessionConfig(const TqConfig& cfg);
void TqTuneSpeedTestLocalSocket(TqSocketHandle socket);
uint64_t TqSpeedByteMismatchLimit(uint64_t highBytes);
bool TqSpeedByteCountsCloseEnough(uint64_t localBytes, uint64_t serverBytes, uint64_t& diffOut, uint64_t& limitOut);
void TqHandleServerSpeedControlStream(
    TqServerSpeedTestController& controller,
    MsQuicConnection* conn,
    HQUIC rawStream);

// Internal handoff for the server incoming-stream dispatcher after it has already
// wrapped the raw stream and buffered initial bytes.
bool TqAttachServerSpeedControlStream(
    TqServerSpeedTestController& controller,
    MsQuicConnection* conn,
    MsQuicStream* stream,
    std::vector<uint8_t> initialBytes,
    std::function<void()> onComplete = {});
