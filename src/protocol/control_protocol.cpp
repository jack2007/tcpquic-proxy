#include "control_protocol.h"

#include <cstring>

namespace {

static void WriteU16BE(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

static void WriteU32BE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

static uint16_t ReadU16BE(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

static uint32_t ReadU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

static bool ValidateAddr(const TqOpenRequest& req) {
    switch (req.AddrType) {
    case TQ_ADDR_IPV4:
        return req.Addr.size() == 4;
    case TQ_ADDR_IPV6:
        return req.Addr.size() == 16;
    case TQ_ADDR_DOMAIN:
        if (req.Addr.empty() || req.Addr.size() > 255) {
            return false;
        }
        return (req.Flags & TQ_FLAG_DNS_REMOTE) != 0;
    default:
        return false;
    }
}

} // namespace

bool TqEncodeOpenRequest(const TqOpenRequest& req, std::vector<uint8_t>& out) {
    if (!ValidateAddr(req)) {
        return false;
    }

    out.clear();
    out.reserve(11 + req.Addr.size());

    out.push_back(TQ_MAGIC_0);
    out.push_back(TQ_MAGIC_1);
    out.push_back(TQ_VERSION);
    out.push_back(TQ_CMD_OPEN);
    out.push_back(req.Flags);
    out.push_back(req.AddrType);
    WriteU16BE(out, req.Port);
    WriteU16BE(out, static_cast<uint16_t>(req.Addr.size()));
    out.insert(out.end(), req.Addr.begin(), req.Addr.end());
    out.push_back(0); // auth_present: v1 fixed 0

    return true;
}

bool TqDecodeOpenRequest(const uint8_t* data, size_t len, TqOpenRequest& out) {
    if (data == nullptr || len < 11) {
        return false;
    }

    if (data[0] != TQ_MAGIC_0 || data[1] != TQ_MAGIC_1) {
        return false;
    }
    if (data[2] != TQ_VERSION) {
        return false;
    }
    if (data[3] != TQ_CMD_OPEN) {
        return false;
    }

    const uint16_t addrLen = ReadU16BE(data + 8);
    const size_t expectedLen = 11 + addrLen;
    if (len != expectedLen) {
        return false;
    }

    out.Flags = data[4];
    out.AddrType = data[5];
    out.Port = ReadU16BE(data + 6);
    out.Addr.assign(data + 10, data + 10 + addrLen);

    if (data[10 + addrLen] != 0) {
        return false; // auth_present must be 0 in v1
    }

    return ValidateAddr(out);
}

bool TqEncodeOpenResponse(const TqOpenResponse& resp, std::vector<uint8_t>& out) {
    if (resp.Ok) {
        if (resp.Error != TqOpenError::Ok) {
            return false;
        }
    } else if (resp.Error == TqOpenError::Ok) {
        return false;
    }

    out.clear();
    out.reserve(TQ_OPEN_RESPONSE_SIZE);

    out.push_back(TQ_MAGIC_0);
    out.push_back(TQ_MAGIC_1);
    out.push_back(TQ_VERSION);
    out.push_back(resp.Ok ? TQ_CMD_OPEN_OK : TQ_CMD_OPEN_FAIL);
    out.push_back(resp.Ok ? static_cast<uint8_t>(TqOpenError::Ok)
                          : static_cast<uint8_t>(resp.Error));
    WriteU32BE(out, resp.ConnId);

    return out.size() == TQ_OPEN_RESPONSE_SIZE;
}

bool TqDecodeOpenResponse(const uint8_t* data, size_t len, TqOpenResponse& out) {
    if (data == nullptr || len != TQ_OPEN_RESPONSE_SIZE) {
        return false;
    }

    if (data[0] != TQ_MAGIC_0 || data[1] != TQ_MAGIC_1) {
        return false;
    }
    if (data[2] != TQ_VERSION) {
        return false;
    }

    const uint8_t cmd = data[3];
    if (cmd == TQ_CMD_OPEN_OK) {
        out.Ok = true;
    } else if (cmd == TQ_CMD_OPEN_FAIL) {
        out.Ok = false;
    } else {
        return false;
    }

    const uint8_t errorCode = data[4];
    out.Error = static_cast<TqOpenError>(errorCode);
    out.ConnId = ReadU32BE(data + 5);

    if (out.Ok) {
        return errorCode == static_cast<uint8_t>(TqOpenError::Ok);
    }

    return errorCode != static_cast<uint8_t>(TqOpenError::Ok);
}
