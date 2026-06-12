#include "platform_socket.h"

#include <mstcpip.h>

TqSocketStartup::TqSocketStartup() {
    WSADATA data{};
    Ok_ = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

TqSocketStartup::~TqSocketStartup() {
    if (Ok_) {
        ::WSACleanup();
    }
}

bool TqSocketValid(TqSocketHandle socket) {
    return socket != INVALID_SOCKET;
}

void TqCloseSocket(TqSocketHandle socket) {
    if (TqSocketValid(socket)) {
        (void)::closesocket(socket);
    }
}

bool TqSetNonBlocking(TqSocketHandle socket) {
    u_long mode = 1;
    return ::ioctlsocket(socket, FIONBIO, &mode) == 0;
}

int TqLastSocketError() {
    return ::WSAGetLastError();
}

bool TqSocketWouldBlock(int error) {
    return error == WSAEWOULDBLOCK;
}

bool TqSocketInProgress(int error) {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}

bool TqSocketInterrupted(int error) {
    return error == WSAEINTR;
}

int TqSend(TqSocketHandle socket, const void* data, size_t length, TqSendFlags) {
    return ::send(socket, static_cast<const char*>(data), static_cast<int>(length), 0);
}

int TqRecv(TqSocketHandle socket, void* data, size_t length, TqRecvFlags) {
    return ::recv(socket, static_cast<char*>(data), static_cast<int>(length), 0);
}

bool TqShutdownSend(TqSocketHandle socket) {
    return ::shutdown(socket, SD_SEND) == 0;
}

bool TqShutdownBoth(TqSocketHandle socket) {
    return ::shutdown(socket, SD_BOTH) == 0;
}

bool TqInetPton(int family, const char* text, void* out) {
    return ::InetPtonA(family, text, out) == 1;
}

const char* TqInetNtop(int family, const void* addr, char* text, size_t textLength) {
    return ::InetNtopA(family, const_cast<void*>(addr), text, static_cast<DWORD>(textLength));
}

bool TqSetReuseAddr(TqSocketHandle socket) {
    BOOL enabled = TRUE;
    return ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == 0;
}

bool TqSetNoDelay(TqSocketHandle socket) {
    BOOL enabled = TRUE;
    return ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
        reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == 0;
}

bool TqSetSocketBuffer(TqSocketHandle socket, int option, int bytes) {
    return bytes <= 0 || ::setsockopt(socket, SOL_SOCKET, option,
        reinterpret_cast<const char*>(&bytes), sizeof(bytes)) == 0;
}

int TqGetSocketBuffer(TqSocketHandle socket, int option) {
    int bytes = 0;
    int length = sizeof(bytes);
    if (::getsockopt(socket, SOL_SOCKET, option,
            reinterpret_cast<char*>(&bytes), &length) != 0) {
        return -1;
    }
    return bytes;
}

bool TqSocketPair(TqSocketHandle out[2]) {
    out[0] = TqInvalidSocket;
    out[1] = TqInvalidSocket;

    TqSocketHandle listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!TqSocketValid(listener)) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener, 1) != 0) {
        TqCloseSocket(listener);
        return false;
    }

    int addrLen = sizeof(addr);
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) {
        TqCloseSocket(listener);
        return false;
    }

    TqSocketHandle client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!TqSocketValid(client)) {
        TqCloseSocket(listener);
        return false;
    }

    if (::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        TqCloseSocket(client);
        TqCloseSocket(listener);
        return false;
    }

    TqSocketHandle server = ::accept(listener, nullptr, nullptr);
    TqCloseSocket(listener);
    if (!TqSocketValid(server)) {
        TqCloseSocket(client);
        return false;
    }

    out[0] = client;
    out[1] = server;
    return true;
}
