#include "client_ingress_state.h"

#include "http_connect_server.h"
#include "socks5_server.h"

#include <algorithm>

namespace {

constexpr uint8_t TqSocks5Version = 0x05;
constexpr uint8_t TqSocks5NoAuth = 0x00;
constexpr uint8_t TqSocks5AtypIpv4 = 0x01;
constexpr uint8_t TqSocks5AtypDomain = 0x03;
constexpr uint8_t TqSocks5AtypIpv6 = 0x04;
constexpr size_t TqHttpConnectHeaderCap = 16 * 1024;

bool TqSocks5RequestLength(const std::vector<uint8_t>& data, size_t& length) {
    if (data.size() < 4) {
        return false;
    }

    switch (data[3]) {
    case TqSocks5AtypIpv4:
        length = 10;
        return true;
    case TqSocks5AtypDomain:
        if (data.size() < 5) {
            return false;
        }
        if (data[4] == 0) {
            length = 0;
            return true;
        }
        length = static_cast<size_t>(7) + data[4];
        return true;
    case TqSocks5AtypIpv6:
        length = 22;
        return true;
    default:
        length = 0;
        return true;
    }
}

} // namespace

TqClientIngressState::TqClientIngressState(TqClientIngressProto proto) : Proto(proto) {}

TqClientIngressResult TqClientIngressState::Feed(const uint8_t* data, size_t size) {
    if (data != nullptr && size > 0) {
        ReadBuffer.insert(ReadBuffer.end(), data, data + size);
    }

    if (Ready) {
        return TqClientIngressResult::ReadyToOpen;
    }

    if (Proto == TqClientIngressProto::Socks5) {
        return FeedSocks5();
    }
    return FeedHttpConnect();
}

const std::string& TqClientIngressState::PendingWrite() const {
    return WriteBuffer;
}

void TqClientIngressState::MarkWriteComplete(size_t bytes) {
    const size_t eraseBytes = std::min(bytes, WriteBuffer.size());
    WriteBuffer.erase(0, eraseBytes);
}

const TunnelRequest& TqClientIngressState::Request() const {
    return ParsedRequest;
}

std::vector<uint8_t> TqClientIngressState::TakeBufferedData() {
    std::vector<uint8_t> data;
    data.swap(ReadBuffer);
    return data;
}

TqClientIngressResult TqClientIngressState::FeedSocks5() {
    if (!SocksGreetingDone) {
        if (ReadBuffer.size() < 2) {
            return TqClientIngressResult::NeedRead;
        }
        if (ReadBuffer[0] != TqSocks5Version) {
            return TqClientIngressResult::Close;
        }

        const size_t methodsLen = ReadBuffer[1];
        const size_t greetingLen = 2 + methodsLen;
        if (ReadBuffer.size() < greetingLen) {
            return TqClientIngressResult::NeedRead;
        }

        bool hasNoAuth = false;
        for (size_t i = 2; i < greetingLen; ++i) {
            if (ReadBuffer[i] == TqSocks5NoAuth) {
                hasNoAuth = true;
                break;
            }
        }
        if (!hasNoAuth) {
            return TqClientIngressResult::Close;
        }

        ReadBuffer.erase(ReadBuffer.begin(), ReadBuffer.begin() + static_cast<std::ptrdiff_t>(greetingLen));
        WriteBuffer.assign("\x05\x00", 2);
        SocksGreetingDone = true;
        return TqClientIngressResult::NeedWrite;
    }

    if (!WriteBuffer.empty()) {
        return TqClientIngressResult::NeedWrite;
    }

    size_t requestLen = 0;
    if (!TqSocks5RequestLength(ReadBuffer, requestLen)) {
        return TqClientIngressResult::NeedRead;
    }
    if (requestLen == 0) {
        return TqClientIngressResult::Close;
    }
    if (ReadBuffer.size() < requestLen) {
        return TqClientIngressResult::NeedRead;
    }

    const std::vector<uint8_t> request(ReadBuffer.begin(), ReadBuffer.begin() + static_cast<std::ptrdiff_t>(requestLen));
    TunnelRequest parsed{};
    if (!TqParseSocks5ConnectRequest(request, parsed)) {
        return TqClientIngressResult::Close;
    }
    parsed.IngressTraceProto = 1;
    ParsedRequest = parsed;
    ReadBuffer.erase(ReadBuffer.begin(), ReadBuffer.begin() + static_cast<std::ptrdiff_t>(requestLen));
    Ready = true;
    return TqClientIngressResult::ReadyToOpen;
}

TqClientIngressResult TqClientIngressState::FeedHttpConnect() {
    const std::string request(ReadBuffer.begin(), ReadBuffer.end());
    const size_t headerEnd = request.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        if (ReadBuffer.size() >= TqHttpConnectHeaderCap) {
            return TqClientIngressResult::Close;
        }
        return TqClientIngressResult::NeedRead;
    }

    const size_t headerSize = headerEnd + 4;
    if (headerSize > TqHttpConnectHeaderCap) {
        return TqClientIngressResult::Close;
    }

    TunnelRequest parsed{};
    if (!TqParseHttpConnectRequest(request.substr(0, headerSize), parsed)) {
        return TqClientIngressResult::Close;
    }
    parsed.IngressTraceProto = 2;
    ParsedRequest = parsed;
    ReadBuffer.erase(ReadBuffer.begin(), ReadBuffer.begin() + static_cast<std::ptrdiff_t>(headerSize));
    Ready = true;
    return TqClientIngressResult::ReadyToOpen;
}
