#include "client_ingress_state.h"

#include "http_connect_server.h"
#include "socks5_server.h"

#include <algorithm>
#include <utility>

namespace {

constexpr uint8_t TqSocks5Version = 0x05;
constexpr uint8_t TqSocks5NoAuth = 0x00;
constexpr uint8_t TqSocks5UserPass = 0x02;
constexpr uint8_t TqSocks5NoAcceptable = 0xFF;
constexpr uint8_t TqSocks5UserPassVersion = 0x01;
constexpr uint8_t TqSocks5AuthSuccess = 0x00;
constexpr uint8_t TqSocks5AuthFailure = 0x01;
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

TqClientIngressState::TqClientIngressState(
    TqClientIngressProto proto,
    std::shared_ptr<const TqProxyAuthTable> auth) :
    Proto(proto),
    Auth(std::move(auth)) {
    if (!Auth) {
        Auth = std::make_shared<TqProxyAuthTable>();
    }
}

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
    if (!WriteBuffer.empty()) {
        return TqClientIngressResult::NeedWrite;
    }
    if (CloseAfterWrite) {
        return TqClientIngressResult::Close;
    }
    if (!SocksGreetingDone) {
        if (SocksAuthPending) {
            if (ReadBuffer.size() < 2) {
                return TqClientIngressResult::NeedRead;
            }
            if (ReadBuffer[0] != TqSocks5UserPassVersion) {
                return TqClientIngressResult::Close;
            }
            const size_t usernameLen = ReadBuffer[1];
            if (usernameLen == 0 || usernameLen > TqProxyAuthTable::kMaxFieldBytes) {
                return TqClientIngressResult::Close;
            }
            if (ReadBuffer.size() < 2 + usernameLen + 1) {
                return TqClientIngressResult::NeedRead;
            }
            const size_t passwordLenOffset = 2 + usernameLen;
            const size_t passwordLen = ReadBuffer[passwordLenOffset];
            if (passwordLen == 0 || passwordLen > TqProxyAuthTable::kMaxFieldBytes) {
                return TqClientIngressResult::Close;
            }
            const size_t authLen = 2 + usernameLen + 1 + passwordLen;
            if (ReadBuffer.size() < authLen) {
                return TqClientIngressResult::NeedRead;
            }
            const std::string username(
                ReadBuffer.begin() + 2,
                ReadBuffer.begin() + static_cast<std::ptrdiff_t>(2 + usernameLen));
            const std::string password(
                ReadBuffer.begin() + static_cast<std::ptrdiff_t>(passwordLenOffset + 1),
                ReadBuffer.begin() + static_cast<std::ptrdiff_t>(passwordLenOffset + 1 + passwordLen));
            const bool valid = Auth->Validate(username, password);
            ReadBuffer.erase(
                ReadBuffer.begin(),
                ReadBuffer.begin() + static_cast<std::ptrdiff_t>(authLen));
            WriteBuffer.clear();
            WriteBuffer.push_back(static_cast<char>(TqSocks5UserPassVersion));
            WriteBuffer.push_back(static_cast<char>(valid ? TqSocks5AuthSuccess : TqSocks5AuthFailure));
            CloseAfterWrite = !valid;
            SocksAuthPending = false;
            SocksGreetingDone = valid;
            return TqClientIngressResult::NeedWrite;
        }
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
        bool hasUserPass = false;
        for (size_t i = 2; i < greetingLen; ++i) {
            hasNoAuth = hasNoAuth || ReadBuffer[i] == TqSocks5NoAuth;
            hasUserPass = hasUserPass || ReadBuffer[i] == TqSocks5UserPass;
        }

        const bool authEnabled = Auth && Auth->Enabled();
        if (!authEnabled) {
            if (!hasNoAuth) {
                WriteBuffer.assign("\x05\xFF", 2);
                CloseAfterWrite = true;
                ReadBuffer.erase(ReadBuffer.begin(), ReadBuffer.begin() + static_cast<std::ptrdiff_t>(greetingLen));
                return TqClientIngressResult::NeedWrite;
            }
            ReadBuffer.erase(ReadBuffer.begin(), ReadBuffer.begin() + static_cast<std::ptrdiff_t>(greetingLen));
            WriteBuffer.assign("\x05\x00", 2);
            SocksGreetingDone = true;
            return TqClientIngressResult::NeedWrite;
        }

        if (!hasUserPass) {
            WriteBuffer.assign("\x05\xFF", 2);
            CloseAfterWrite = true;
            ReadBuffer.erase(ReadBuffer.begin(), ReadBuffer.begin() + static_cast<std::ptrdiff_t>(greetingLen));
            return TqClientIngressResult::NeedWrite;
        }

        WriteBuffer.assign("\x05\x02", 2);
        ReadBuffer.erase(ReadBuffer.begin(), ReadBuffer.begin() + static_cast<std::ptrdiff_t>(greetingLen));
        SocksAuthPending = true;
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
    if (!WriteBuffer.empty()) {
        return TqClientIngressResult::NeedWrite;
    }
    if (CloseAfterWrite) {
        return TqClientIngressResult::Close;
    }

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
    const std::string header = request.substr(0, headerSize);
    if (!TqParseHttpConnectRequest(header, parsed)) {
        return TqClientIngressResult::Close;
    }
    if (!TqHttpConnectRequestAuthorized(header, *Auth)) {
        WriteBuffer = "HTTP/1.1 407 Proxy Authentication Required\r\n"
            "Proxy-Authenticate: Basic realm=\"tcpquic-proxy\"\r\n\r\n";
        CloseAfterWrite = true;
        return TqClientIngressResult::NeedWrite;
    }
    parsed.IngressTraceProto = 2;
    ParsedRequest = parsed;
    ReadBuffer.erase(ReadBuffer.begin(), ReadBuffer.begin() + static_cast<std::ptrdiff_t>(headerSize));
    Ready = true;
    return TqClientIngressResult::ReadyToOpen;
}
