#include "../control_protocol.h"

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

    return 0;
}
