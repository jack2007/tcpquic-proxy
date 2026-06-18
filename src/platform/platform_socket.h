#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using TqSocketHandle = SOCKET;
static constexpr TqSocketHandle TqInvalidSocket = INVALID_SOCKET;
#else
#include <netinet/in.h>
#include <sys/socket.h>
using TqSocketHandle = int;
static constexpr TqSocketHandle TqInvalidSocket = -1;
#endif

enum class TqSendFlags : int {
    None = 0,
    NoSignal = 1,
};

enum class TqRecvFlags : int {
    None = 0,
    DontWait = 1,
};

class TqSocketStartup {
public:
    TqSocketStartup();
    ~TqSocketStartup();
    TqSocketStartup(const TqSocketStartup&) = delete;
    TqSocketStartup& operator=(const TqSocketStartup&) = delete;

    bool Ok() const { return Ok_; }

private:
    bool Ok_{false};
};

bool TqSocketValid(TqSocketHandle socket);
void TqCloseSocket(TqSocketHandle socket);
void TqResetSocket(TqSocketHandle socket);
bool TqSetNonBlocking(TqSocketHandle socket);
bool TqSetSocketBlocking(TqSocketHandle socket);
int TqLastSocketError();
bool TqSocketWouldBlock(int error);
bool TqSocketInProgress(int error);
bool TqSocketInterrupted(int error);
bool TqSocketConnectionRefused(int error);
bool TqSocketTimeoutLike(int error);
int TqConnect(TqSocketHandle socket, const sockaddr* address, socklen_t addressLength);
bool TqGetSocketError(TqSocketHandle socket, int& error);
int TqSend(TqSocketHandle socket, const void* data, size_t length, TqSendFlags flags);
int TqRecv(TqSocketHandle socket, void* data, size_t length, TqRecvFlags flags);
bool TqShutdownSend(TqSocketHandle socket);
bool TqShutdownBoth(TqSocketHandle socket);
bool TqInetPton(int family, const char* text, void* out);
const char* TqInetNtop(int family, const void* addr, char* text, size_t textLength);
bool TqSetReuseAddr(TqSocketHandle socket);
bool TqSetNoDelay(TqSocketHandle socket);
bool TqSetSocketBuffer(TqSocketHandle socket, int option, int bytes);
int TqGetSocketBuffer(TqSocketHandle socket, int option);
bool TqSocketPair(TqSocketHandle out[2]);
