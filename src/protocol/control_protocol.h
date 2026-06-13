#pragma once
#include <cstdint>
#include <vector>
#include <optional>
#include <string>

static constexpr uint8_t TQ_MAGIC_0 = 0x54;
static constexpr uint8_t TQ_MAGIC_1 = 0x51;
static constexpr uint8_t TQ_VERSION = 0x01;
static constexpr uint8_t TQ_CMD_OPEN = 0x01;
static constexpr uint8_t TQ_CMD_OPEN_OK = 0x02;
static constexpr uint8_t TQ_CMD_OPEN_FAIL = 0x03;
static constexpr uint8_t TQ_CMD_SPEED_START = 0x10;
static constexpr uint8_t TQ_CMD_SPEED_READY = 0x11;
static constexpr uint8_t TQ_CMD_SPEED_FINISH = 0x12;
static constexpr uint8_t TQ_CMD_SPEED_RESULT = 0x13;
static constexpr uint8_t TQ_CMD_SPEED_ERROR = 0x14;

static constexpr uint8_t TQ_ADDR_IPV4 = 0x01;
static constexpr uint8_t TQ_ADDR_IPV6 = 0x02;
static constexpr uint8_t TQ_ADDR_DOMAIN = 0x03;

static constexpr uint8_t TQ_FLAG_COMPRESS = 0x01;
static constexpr uint8_t TQ_FLAG_DNS_REMOTE = 0x04;

static constexpr size_t TQ_OPEN_RESPONSE_SIZE = 9;
static constexpr size_t TQ_SPEED_START_SIZE = 16;
static constexpr size_t TQ_SPEED_READY_MIN_SIZE = 13;
static constexpr size_t TQ_SPEED_FINISH_SIZE = 24;
static constexpr size_t TQ_SPEED_RESULT_SIZE = 33;
static constexpr size_t TQ_SPEED_ERROR_MIN_SIZE = 11;
static constexpr size_t TQ_SPEED_ERROR_MAX_MESSAGE_LEN = 1024;

enum class TqSpeedDirection : uint8_t {
    Download = 1,
    Upload = 2,
};

enum class TqSpeedError : uint8_t {
    Ok = 0x00,
    InvalidRequest = 0x01,
    BindFailed = 0x02,
    ListenFailed = 0x03,
    Internal = 0x04,
    Timeout = 0x05,
    Unsupported = 0x06,
};

enum class TqOpenError : uint8_t {
    Ok = 0x00,
    AclDenied = 0x01,
    DnsFailed = 0x02,
    TcpTimeout = 0x03,
    TcpRefused = 0x04,
    Internal = 0x05,
};

struct TqOpenRequest {
    uint8_t Flags{};
    uint8_t AddrType{};
    uint16_t Port{};
    std::vector<uint8_t> Addr; // raw IP bytes or domain octets
};

struct TqOpenResponse {
    bool Ok{};
    TqOpenError Error{TqOpenError::Ok};
    uint32_t ConnId{};
};

struct TqSpeedStart {
    uint32_t SessionId{};
    TqSpeedDirection Direction{TqSpeedDirection::Download};
    uint32_t DurationSec{};
    uint16_t Parallel{};
    uint8_t Flags{};
};

struct TqSpeedReady {
    uint32_t SessionId{};
    uint8_t AddrType{};
    uint16_t Port{};
    std::vector<uint8_t> Addr;
};

struct TqSpeedFinish {
    uint32_t SessionId{};
    uint64_t ClientBytes{};
    uint64_t ClientElapsedUs{};
};

struct TqSpeedResult {
    uint32_t SessionId{};
    uint64_t ServerBytes{};
    uint64_t ServerElapsedUs{};
    uint32_t AcceptedConnections{};
    uint32_t ClosedConnections{};
    uint8_t Status{};
};

struct TqSpeedErrorMessage {
    uint32_t SessionId{};
    TqSpeedError Error{TqSpeedError::Ok};
    std::string Message;
};

bool TqEncodeOpenRequest(const TqOpenRequest& req, std::vector<uint8_t>& out);
bool TqDecodeOpenRequest(const uint8_t* data, size_t len, TqOpenRequest& out);
bool TqEncodeOpenResponse(const TqOpenResponse& resp, std::vector<uint8_t>& out);
bool TqDecodeOpenResponse(const uint8_t* data, size_t len, TqOpenResponse& out);

bool TqEncodeSpeedStart(const TqSpeedStart& msg, std::vector<uint8_t>& out);
bool TqDecodeSpeedStart(const uint8_t* data, size_t len, TqSpeedStart& out);
bool TqEncodeSpeedReady(const TqSpeedReady& msg, std::vector<uint8_t>& out);
bool TqDecodeSpeedReady(const uint8_t* data, size_t len, TqSpeedReady& out);
bool TqEncodeSpeedFinish(const TqSpeedFinish& msg, std::vector<uint8_t>& out);
bool TqDecodeSpeedFinish(const uint8_t* data, size_t len, TqSpeedFinish& out);
bool TqEncodeSpeedResult(const TqSpeedResult& msg, std::vector<uint8_t>& out);
bool TqDecodeSpeedResult(const uint8_t* data, size_t len, TqSpeedResult& out);
bool TqEncodeSpeedError(const TqSpeedErrorMessage& msg, std::vector<uint8_t>& out);
bool TqDecodeSpeedError(const uint8_t* data, size_t len, TqSpeedErrorMessage& out);
