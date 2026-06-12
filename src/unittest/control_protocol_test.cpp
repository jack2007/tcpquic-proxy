#include "control_protocol.h"

#include <cassert>
#include <cstring>

int main() {
    TqOpenRequest req{};
    req.Flags = TQ_FLAG_COMPRESS | TQ_FLAG_DNS_REMOTE;
    req.AddrType = TQ_ADDR_DOMAIN;
    req.Port = 3306;
    const char* host = "db.internal";
    req.Addr.assign(host, host + strlen(host));

    std::vector<uint8_t> buf;
    assert(TqEncodeOpenRequest(req, buf));

    TqOpenRequest decoded{};
    assert(TqDecodeOpenRequest(buf.data(), buf.size(), decoded));
    assert(decoded.Port == 3306);
    assert(decoded.Flags == (TQ_FLAG_COMPRESS | TQ_FLAG_DNS_REMOTE));
    assert(decoded.AddrType == TQ_ADDR_DOMAIN);
    assert(decoded.Addr.size() == strlen(host));
    assert(memcmp(decoded.Addr.data(), host, strlen(host)) == 0);

    TqOpenResponse ok{true, TqOpenError::Ok, 0};
    std::vector<uint8_t> rbuf;
    assert(TqEncodeOpenResponse(ok, rbuf));
    assert(rbuf.size() == TQ_OPEN_RESPONSE_SIZE);

    TqOpenResponse rdec{};
    assert(TqDecodeOpenResponse(rbuf.data(), rbuf.size(), rdec));
    assert(rdec.Ok);
    assert(rdec.Error == TqOpenError::Ok);
    assert(rdec.ConnId == 0);

    TqOpenResponse fail{false, TqOpenError::AclDenied, 0};
    std::vector<uint8_t> fbuf;
    assert(TqEncodeOpenResponse(fail, fbuf));
    assert(fbuf.size() == TQ_OPEN_RESPONSE_SIZE);

    TqOpenResponse fdec{};
    assert(TqDecodeOpenResponse(fbuf.data(), fbuf.size(), fdec));
    assert(!fdec.Ok);
    assert(fdec.Error == TqOpenError::AclDenied);

    // DOMAIN without DNS_REMOTE must be rejected.
    TqOpenRequest badDomain = req;
    badDomain.Flags = TQ_FLAG_COMPRESS;
    assert(!TqEncodeOpenRequest(badDomain, buf));

    TqSpeedStart start{};
    start.SessionId = 7;
    start.Direction = TqSpeedDirection::Download;
    start.DurationSec = 10;
    start.Parallel = 4;
    assert(TqEncodeSpeedStart(start, buf));

    TqSpeedStart startDecoded{};
    assert(TqDecodeSpeedStart(buf.data(), buf.size(), startDecoded));
    assert(startDecoded.SessionId == 7);
    assert(startDecoded.Direction == TqSpeedDirection::Download);
    assert(startDecoded.DurationSec == 10);
    assert(startDecoded.Parallel == 4);
    assert(startDecoded.Flags == 0);

    TqSpeedReady ready{};
    ready.SessionId = 7;
    ready.AddrType = TQ_ADDR_IPV4;
    ready.Port = 54321;
    ready.Addr = {127, 0, 0, 1};
    assert(TqEncodeSpeedReady(ready, buf));

    TqSpeedReady readyDecoded{};
    assert(TqDecodeSpeedReady(buf.data(), buf.size(), readyDecoded));
    assert(readyDecoded.SessionId == 7);
    assert(readyDecoded.AddrType == TQ_ADDR_IPV4);
    assert(readyDecoded.Port == 54321);
    assert(readyDecoded.Addr == ready.Addr);

    TqSpeedFinish finish{};
    finish.SessionId = 7;
    finish.ClientBytes = 123456789ULL;
    finish.ClientElapsedUs = 10000001ULL;
    assert(TqEncodeSpeedFinish(finish, buf));

    TqSpeedFinish finishDecoded{};
    assert(TqDecodeSpeedFinish(buf.data(), buf.size(), finishDecoded));
    assert(finishDecoded.SessionId == 7);
    assert(finishDecoded.ClientBytes == 123456789ULL);
    assert(finishDecoded.ClientElapsedUs == 10000001ULL);

    TqSpeedResult result{};
    result.SessionId = 7;
    result.ServerBytes = 123456000ULL;
    result.ServerElapsedUs = 10000002ULL;
    result.AcceptedConnections = 4;
    result.ClosedConnections = 4;
    result.Status = 0;
    assert(TqEncodeSpeedResult(result, buf));

    TqSpeedResult resultDecoded{};
    assert(TqDecodeSpeedResult(buf.data(), buf.size(), resultDecoded));
    assert(resultDecoded.SessionId == 7);
    assert(resultDecoded.ServerBytes == 123456000ULL);
    assert(resultDecoded.ServerElapsedUs == 10000002ULL);
    assert(resultDecoded.AcceptedConnections == 4);
    assert(resultDecoded.ClosedConnections == 4);
    assert(resultDecoded.Status == 0);

    TqSpeedErrorMessage error{};
    error.SessionId = 7;
    error.Error = TqSpeedError::InvalidRequest;
    error.Message = "bad speed request";
    assert(TqEncodeSpeedErrorMessage(error, buf));

    TqSpeedErrorMessage errorDecoded{};
    assert(TqDecodeSpeedErrorMessage(buf.data(), buf.size(), errorDecoded));
    assert(errorDecoded.SessionId == 7);
    assert(errorDecoded.Error == TqSpeedError::InvalidRequest);
    assert(errorDecoded.Message == "bad speed request");

    TqSpeedStart badDirection = start;
    badDirection.Direction = static_cast<TqSpeedDirection>(0);
    assert(!TqEncodeSpeedStart(badDirection, buf));

    TqSpeedStart badDuration = start;
    badDuration.DurationSec = 0;
    assert(!TqEncodeSpeedStart(badDuration, buf));

    TqSpeedStart badParallel = start;
    badParallel.Parallel = 0;
    assert(!TqEncodeSpeedStart(badParallel, buf));

    TqSpeedStart badFlags = start;
    badFlags.Flags = 1;
    assert(!TqEncodeSpeedStart(badFlags, buf));

    TqSpeedReady badReady = ready;
    badReady.Addr = {127, 0, 0};
    assert(!TqEncodeSpeedReady(badReady, buf));

    return 0;
}
