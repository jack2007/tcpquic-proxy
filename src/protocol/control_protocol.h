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

static constexpr uint8_t TQ_ADDR_IPV4 = 0x01;
static constexpr uint8_t TQ_ADDR_IPV6 = 0x02;
static constexpr uint8_t TQ_ADDR_DOMAIN = 0x03;

static constexpr uint8_t TQ_FLAG_COMPRESS = 0x01;
static constexpr uint8_t TQ_FLAG_COMPRESS_LZ4 = 0x02;
static constexpr uint8_t TQ_FLAG_DNS_REMOTE = 0x04;

static constexpr size_t TQ_OPEN_RESPONSE_SIZE = 9;

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

bool TqEncodeOpenRequest(const TqOpenRequest& req, std::vector<uint8_t>& out);
bool TqDecodeOpenRequest(const uint8_t* data, size_t len, TqOpenRequest& out);
bool TqEncodeOpenResponse(const TqOpenResponse& resp, std::vector<uint8_t>& out);
bool TqDecodeOpenResponse(const uint8_t* data, size_t len, TqOpenResponse& out);
