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

static void WriteU64BE(std::vector<uint8_t>& out, uint64_t v) {
    out.push_back(static_cast<uint8_t>(v >> 56));
    out.push_back(static_cast<uint8_t>((v >> 48) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 40) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 32) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
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

static uint64_t ReadU64BE(const uint8_t* p) {
    return (static_cast<uint64_t>(p[0]) << 56) |
           (static_cast<uint64_t>(p[1]) << 48) |
           (static_cast<uint64_t>(p[2]) << 40) |
           (static_cast<uint64_t>(p[3]) << 32) |
           (static_cast<uint64_t>(p[4]) << 24) |
           (static_cast<uint64_t>(p[5]) << 16) |
           (static_cast<uint64_t>(p[6]) << 8) |
           static_cast<uint64_t>(p[7]);
}

static bool IsValidSpeedDirection(uint8_t v) {
    return v == static_cast<uint8_t>(TqSpeedDirection::Download) ||
           v == static_cast<uint8_t>(TqSpeedDirection::Upload);
}

static bool IsValidSpeedErrorFrameCode(uint8_t v) {
    return v != static_cast<uint8_t>(TqSpeedError::Ok);
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

static bool ValidateSpeedStart(const TqSpeedStart& msg) {
    return IsValidSpeedDirection(static_cast<uint8_t>(msg.Direction)) &&
           msg.DurationSec >= 1 && msg.DurationSec <= 86400 &&
           msg.Parallel >= 1 && msg.Parallel <= 128 &&
           msg.Flags == 0;
}

static bool ValidateSpeedReady(const TqSpeedReady& msg) {
    switch (msg.AddrType) {
    case TQ_ADDR_IPV4:
        return msg.Addr.size() == 4;
    case TQ_ADDR_IPV6:
        return msg.Addr.size() == 16;
    default:
        return false;
    }
}

static bool ValidateSpeedError(const TqSpeedErrorMessage& msg) {
    return IsValidSpeedErrorFrameCode(static_cast<uint8_t>(msg.Error)) &&
           msg.Message.size() <= TQ_SPEED_ERROR_MAX_MESSAGE_LEN;
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

bool TqEncodeSpeedStart(const TqSpeedStart& msg, std::vector<uint8_t>& out) {
    if (!ValidateSpeedStart(msg)) {
        return false;
    }

    out.clear();
    out.reserve(TQ_SPEED_START_SIZE);

    out.push_back(TQ_MAGIC_0);
    out.push_back(TQ_MAGIC_1);
    out.push_back(TQ_VERSION);
    out.push_back(TQ_CMD_SPEED_START);
    WriteU32BE(out, msg.SessionId);
    out.push_back(static_cast<uint8_t>(msg.Direction));
    WriteU32BE(out, msg.DurationSec);
    WriteU16BE(out, msg.Parallel);
    out.push_back(msg.Flags);

    return out.size() == TQ_SPEED_START_SIZE;
}

bool TqDecodeSpeedStart(const uint8_t* data, size_t len, TqSpeedStart& out) {
    if (data == nullptr || len != TQ_SPEED_START_SIZE) {
        return false;
    }
    if (data[0] != TQ_MAGIC_0 || data[1] != TQ_MAGIC_1) {
        return false;
    }
    if (data[2] != TQ_VERSION || data[3] != TQ_CMD_SPEED_START) {
        return false;
    }
    if (!IsValidSpeedDirection(data[8])) {
        return false;
    }
    if (data[15] != 0) {
        return false;
    }

    TqSpeedStart msg{};
    msg.SessionId = ReadU32BE(data + 4);
    msg.Direction = static_cast<TqSpeedDirection>(data[8]);
    msg.DurationSec = ReadU32BE(data + 9);
    msg.Parallel = ReadU16BE(data + 13);
    msg.Flags = data[15];

    if (!ValidateSpeedStart(msg)) {
        return false;
    }

    out = msg;
    return true;
}

bool TqEncodeSpeedReady(const TqSpeedReady& msg, std::vector<uint8_t>& out) {
    if (!ValidateSpeedReady(msg)) {
        return false;
    }

    out.clear();
    out.reserve(TQ_SPEED_READY_MIN_SIZE + msg.Addr.size());

    out.push_back(TQ_MAGIC_0);
    out.push_back(TQ_MAGIC_1);
    out.push_back(TQ_VERSION);
    out.push_back(TQ_CMD_SPEED_READY);
    WriteU32BE(out, msg.SessionId);
    out.push_back(msg.AddrType);
    WriteU16BE(out, msg.Port);
    WriteU16BE(out, static_cast<uint16_t>(msg.Addr.size()));
    out.insert(out.end(), msg.Addr.begin(), msg.Addr.end());

    return out.size() == TQ_SPEED_READY_MIN_SIZE + msg.Addr.size();
}

bool TqDecodeSpeedReady(const uint8_t* data, size_t len, TqSpeedReady& out) {
    if (data == nullptr || len < TQ_SPEED_READY_MIN_SIZE) {
        return false;
    }
    if (data[0] != TQ_MAGIC_0 || data[1] != TQ_MAGIC_1) {
        return false;
    }
    if (data[2] != TQ_VERSION || data[3] != TQ_CMD_SPEED_READY) {
        return false;
    }

    const uint16_t addrLen = ReadU16BE(data + 11);
    const size_t expectedLen = TQ_SPEED_READY_MIN_SIZE + addrLen;
    if (len != expectedLen) {
        return false;
    }

    TqSpeedReady msg{};
    msg.SessionId = ReadU32BE(data + 4);
    msg.AddrType = data[8];
    msg.Port = ReadU16BE(data + 9);
    msg.Addr.assign(data + 13, data + 13 + addrLen);

    if (!ValidateSpeedReady(msg)) {
        return false;
    }

    out = msg;
    return true;
}

bool TqEncodeSpeedFinish(const TqSpeedFinish& msg, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(TQ_SPEED_FINISH_SIZE);

    out.push_back(TQ_MAGIC_0);
    out.push_back(TQ_MAGIC_1);
    out.push_back(TQ_VERSION);
    out.push_back(TQ_CMD_SPEED_FINISH);
    WriteU32BE(out, msg.SessionId);
    WriteU64BE(out, msg.ClientBytes);
    WriteU64BE(out, msg.ClientElapsedUs);

    return out.size() == TQ_SPEED_FINISH_SIZE;
}

bool TqDecodeSpeedFinish(const uint8_t* data, size_t len, TqSpeedFinish& out) {
    if (data == nullptr || len != TQ_SPEED_FINISH_SIZE) {
        return false;
    }
    if (data[0] != TQ_MAGIC_0 || data[1] != TQ_MAGIC_1) {
        return false;
    }
    if (data[2] != TQ_VERSION || data[3] != TQ_CMD_SPEED_FINISH) {
        return false;
    }

    TqSpeedFinish msg{};
    msg.SessionId = ReadU32BE(data + 4);
    msg.ClientBytes = ReadU64BE(data + 8);
    msg.ClientElapsedUs = ReadU64BE(data + 16);

    out = msg;
    return true;
}

bool TqEncodeSpeedResult(const TqSpeedResult& msg, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(TQ_SPEED_RESULT_SIZE);

    out.push_back(TQ_MAGIC_0);
    out.push_back(TQ_MAGIC_1);
    out.push_back(TQ_VERSION);
    out.push_back(TQ_CMD_SPEED_RESULT);
    WriteU32BE(out, msg.SessionId);
    WriteU64BE(out, msg.ServerBytes);
    WriteU64BE(out, msg.ServerElapsedUs);
    WriteU32BE(out, msg.AcceptedConnections);
    WriteU32BE(out, msg.ClosedConnections);
    out.push_back(msg.Status);

    return out.size() == TQ_SPEED_RESULT_SIZE;
}

bool TqDecodeSpeedResult(const uint8_t* data, size_t len, TqSpeedResult& out) {
    if (data == nullptr || len != TQ_SPEED_RESULT_SIZE) {
        return false;
    }
    if (data[0] != TQ_MAGIC_0 || data[1] != TQ_MAGIC_1) {
        return false;
    }
    if (data[2] != TQ_VERSION || data[3] != TQ_CMD_SPEED_RESULT) {
        return false;
    }

    TqSpeedResult msg{};
    msg.SessionId = ReadU32BE(data + 4);
    msg.ServerBytes = ReadU64BE(data + 8);
    msg.ServerElapsedUs = ReadU64BE(data + 16);
    msg.AcceptedConnections = ReadU32BE(data + 24);
    msg.ClosedConnections = ReadU32BE(data + 28);
    msg.Status = data[32];

    out = msg;
    return true;
}

bool TqEncodeSpeedError(const TqSpeedErrorMessage& msg, std::vector<uint8_t>& out) {
    if (!ValidateSpeedError(msg)) {
        return false;
    }

    out.clear();
    out.reserve(TQ_SPEED_ERROR_MIN_SIZE + msg.Message.size());

    out.push_back(TQ_MAGIC_0);
    out.push_back(TQ_MAGIC_1);
    out.push_back(TQ_VERSION);
    out.push_back(TQ_CMD_SPEED_ERROR);
    WriteU32BE(out, msg.SessionId);
    out.push_back(static_cast<uint8_t>(msg.Error));
    WriteU16BE(out, static_cast<uint16_t>(msg.Message.size()));
    out.insert(out.end(), msg.Message.begin(), msg.Message.end());

    return out.size() == TQ_SPEED_ERROR_MIN_SIZE + msg.Message.size();
}

bool TqDecodeSpeedError(const uint8_t* data, size_t len, TqSpeedErrorMessage& out) {
    if (data == nullptr || len < TQ_SPEED_ERROR_MIN_SIZE) {
        return false;
    }
    if (data[0] != TQ_MAGIC_0 || data[1] != TQ_MAGIC_1) {
        return false;
    }
    if (data[2] != TQ_VERSION || data[3] != TQ_CMD_SPEED_ERROR) {
        return false;
    }

    const uint16_t messageLen = ReadU16BE(data + 9);
    const size_t expectedLen = TQ_SPEED_ERROR_MIN_SIZE + messageLen;
    if (len != expectedLen || messageLen > TQ_SPEED_ERROR_MAX_MESSAGE_LEN) {
        return false;
    }
    if (!IsValidSpeedErrorFrameCode(data[8])) {
        return false;
    }

    TqSpeedErrorMessage msg{};
    msg.SessionId = ReadU32BE(data + 4);
    msg.Error = static_cast<TqSpeedError>(data[8]);
    msg.Message.assign(reinterpret_cast<const char*>(data + 11), messageLen);

    out = msg;
    return true;
}
