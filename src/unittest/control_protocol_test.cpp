#include "control_protocol.h"

#include <cstring>

int main() {
    TqOpenRequest req{};
    req.Flags = TQ_FLAG_COMPRESS | TQ_FLAG_DNS_REMOTE;
    req.AddrType = TQ_ADDR_DOMAIN;
    req.Port = 3306;
    const char* host = "db.internal";
    req.Addr.assign(host, host + strlen(host));

    std::vector<uint8_t> buf;
    if (!TqEncodeOpenRequest(req, buf)) {
        return 1;
    }

    TqOpenRequest decoded{};
    if (!TqDecodeOpenRequest(buf.data(), buf.size(), decoded)) {
        return 2;
    }
    if (decoded.Port != 3306) {
        return 3;
    }
    if (decoded.Flags != (TQ_FLAG_COMPRESS | TQ_FLAG_DNS_REMOTE)) {
        return 4;
    }
    if (decoded.AddrType != TQ_ADDR_DOMAIN) {
        return 5;
    }
    if (decoded.Addr.size() != strlen(host)) {
        return 6;
    }
    if (memcmp(decoded.Addr.data(), host, strlen(host)) != 0) {
        return 7;
    }

    TqOpenResponse ok{true, TqOpenError::Ok, 0};
    std::vector<uint8_t> rbuf;
    if (!TqEncodeOpenResponse(ok, rbuf)) {
        return 8;
    }
    if (rbuf.size() != TQ_OPEN_RESPONSE_SIZE) {
        return 9;
    }

    TqOpenResponse rdec{};
    if (!TqDecodeOpenResponse(rbuf.data(), rbuf.size(), rdec)) {
        return 10;
    }
    if (!rdec.Ok) {
        return 11;
    }
    if (rdec.Error != TqOpenError::Ok) {
        return 12;
    }
    if (rdec.ConnId != 0) {
        return 13;
    }

    TqOpenResponse fail{false, TqOpenError::AclDenied, 0};
    std::vector<uint8_t> fbuf;
    if (!TqEncodeOpenResponse(fail, fbuf)) {
        return 14;
    }
    if (fbuf.size() != TQ_OPEN_RESPONSE_SIZE) {
        return 15;
    }

    TqOpenResponse fdec{};
    if (!TqDecodeOpenResponse(fbuf.data(), fbuf.size(), fdec)) {
        return 16;
    }
    if (fdec.Ok) {
        return 17;
    }
    if (fdec.Error != TqOpenError::AclDenied) {
        return 18;
    }

    // DOMAIN without DNS_REMOTE must be rejected.
    TqOpenRequest badDomain = req;
    badDomain.Flags = TQ_FLAG_COMPRESS;
    if (TqEncodeOpenRequest(badDomain, buf)) {
        return 19;
    }

    TqSpeedStart start{};
    start.SessionId = 7;
    start.Direction = TqSpeedDirection::Download;
    start.DurationSec = 10;
    start.Parallel = 4;
    if (!TqEncodeSpeedStart(start, buf)) {
        return 1;
    }

    TqSpeedStart startDecoded{};
    if (!TqDecodeSpeedStart(buf.data(), buf.size(), startDecoded)) {
        return 1;
    }
    if (startDecoded.SessionId != 7 ||
        startDecoded.Direction != TqSpeedDirection::Download ||
        startDecoded.DurationSec != 10 ||
        startDecoded.Parallel != 4 ||
        startDecoded.Flags != 0) {
        return 1;
    }

    std::vector<uint8_t> startMut = buf;
    startMut[8] = 0;
    if (TqDecodeSpeedStart(startMut.data(), startMut.size(), startDecoded)) {
        return 1;
    }
    startMut = buf;
    startMut[15] = 1;
    if (TqDecodeSpeedStart(startMut.data(), startMut.size(), startDecoded)) {
        return 1;
    }
    if (TqDecodeSpeedStart(buf.data(), buf.size() - 1, startDecoded)) {
        return 1;
    }

    TqSpeedReady ready{};
    ready.SessionId = 7;
    ready.AddrType = TQ_ADDR_IPV4;
    ready.Port = 54321;
    ready.Addr = {127, 0, 0, 1};
    if (!TqEncodeSpeedReady(ready, buf)) {
        return 1;
    }

    TqSpeedReady readyDecoded{};
    if (!TqDecodeSpeedReady(buf.data(), buf.size(), readyDecoded)) {
        return 1;
    }
    if (readyDecoded.SessionId != 7 ||
        readyDecoded.AddrType != TQ_ADDR_IPV4 ||
        readyDecoded.Port != 54321 ||
        readyDecoded.Addr != ready.Addr) {
        return 1;
    }

    std::vector<uint8_t> readyMut = buf;
    readyMut[11] = 0;
    readyMut[12] = 3;
    if (TqDecodeSpeedReady(readyMut.data(), readyMut.size(), readyDecoded)) {
        return 1;
    }
    if (TqDecodeSpeedReady(buf.data(), buf.size() - 1, readyDecoded)) {
        return 1;
    }

    TqSpeedFinish finish{};
    finish.SessionId = 7;
    finish.ClientBytes = 123456789ULL;
    finish.ClientElapsedUs = 10000001ULL;
    if (!TqEncodeSpeedFinish(finish, buf)) {
        return 1;
    }

    TqSpeedFinish finishDecoded{};
    if (!TqDecodeSpeedFinish(buf.data(), buf.size(), finishDecoded)) {
        return 1;
    }
    if (finishDecoded.SessionId != 7 ||
        finishDecoded.ClientBytes != 123456789ULL ||
        finishDecoded.ClientElapsedUs != 10000001ULL) {
        return 1;
    }
    if (TqDecodeSpeedFinish(buf.data(), buf.size() - 1, finishDecoded)) {
        return 1;
    }

    TqSpeedResult result{};
    result.SessionId = 7;
    result.ServerBytes = 123456000ULL;
    result.ServerElapsedUs = 10000002ULL;
    result.AcceptedConnections = 4;
    result.ClosedConnections = 4;
    result.Status = 0;
    if (!TqEncodeSpeedResult(result, buf)) {
        return 1;
    }

    TqSpeedResult resultDecoded{};
    if (!TqDecodeSpeedResult(buf.data(), buf.size(), resultDecoded)) {
        return 1;
    }
    if (resultDecoded.SessionId != 7 ||
        resultDecoded.ServerBytes != 123456000ULL ||
        resultDecoded.ServerElapsedUs != 10000002ULL ||
        resultDecoded.AcceptedConnections != 4 ||
        resultDecoded.ClosedConnections != 4 ||
        resultDecoded.Status != 0) {
        return 1;
    }
    if (TqDecodeSpeedResult(buf.data(), buf.size() - 1, resultDecoded)) {
        return 1;
    }

    TqSpeedErrorMessage error{};
    error.SessionId = 7;
    error.Error = TqSpeedError::InvalidRequest;
    error.Message = "bad speed request";
    if (!TqEncodeSpeedError(error, buf)) {
        return 1;
    }

    TqSpeedErrorMessage errorDecoded{};
    if (!TqDecodeSpeedError(buf.data(), buf.size(), errorDecoded)) {
        return 1;
    }
    if (errorDecoded.SessionId != 7 ||
        errorDecoded.Error != TqSpeedError::InvalidRequest ||
        errorDecoded.Message != "bad speed request") {
        return 1;
    }

    std::vector<uint8_t> futureError = buf;
    futureError[8] = 99;
    if (!TqDecodeSpeedError(futureError.data(), futureError.size(), errorDecoded)) {
        return 1;
    }
    if (errorDecoded.SessionId != 7 ||
        static_cast<uint8_t>(errorDecoded.Error) != 99 ||
        errorDecoded.Message != "bad speed request") {
        return 1;
    }

    std::vector<uint8_t> errorMut = buf;
    errorMut[8] = 0x00;
    if (TqDecodeSpeedError(errorMut.data(), errorMut.size(), errorDecoded)) {
        return 1;
    }
    errorMut = buf;
    errorMut[9] = 0x04;
    errorMut[10] = 0x01;
    if (TqDecodeSpeedError(errorMut.data(), errorMut.size(), errorDecoded)) {
        return 1;
    }
    if (TqDecodeSpeedError(buf.data(), buf.size() - 1, errorDecoded)) {
        return 1;
    }

    TqSpeedStart badDirection = start;
    badDirection.Direction = static_cast<TqSpeedDirection>(0);
    if (TqEncodeSpeedStart(badDirection, buf)) {
        return 1;
    }

    TqSpeedStart badDuration = start;
    badDuration.DurationSec = 0;
    if (TqEncodeSpeedStart(badDuration, buf)) {
        return 1;
    }

    TqSpeedStart badParallel = start;
    badParallel.Parallel = 0;
    if (TqEncodeSpeedStart(badParallel, buf)) {
        return 1;
    }

    TqSpeedStart badFlags = start;
    badFlags.Flags = 1;
    if (TqEncodeSpeedStart(badFlags, buf)) {
        return 1;
    }

    TqSpeedReady badReady = ready;
    badReady.Addr = {127, 0, 0};
    if (TqEncodeSpeedReady(badReady, buf)) {
        return 1;
    }

    return 0;
}
