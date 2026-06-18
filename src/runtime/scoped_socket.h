#pragma once

#include "platform_socket.h"

class TqScopedSocket {
public:
    explicit TqScopedSocket(TqSocketHandle socket = TqInvalidSocket) : Socket(socket) {}
    ~TqScopedSocket() { Reset(); }

    TqScopedSocket(const TqScopedSocket&) = delete;
    TqScopedSocket& operator=(const TqScopedSocket&) = delete;

    TqSocketHandle Get() const { return Socket; }

    TqSocketHandle Release() {
        const TqSocketHandle socket = Socket;
        Socket = TqInvalidSocket;
        return socket;
    }

    void Reset(TqSocketHandle socket = TqInvalidSocket) {
        if (TqSocketValid(Socket)) {
            TqCloseSocket(Socket);
        }
        Socket = socket;
    }

private:
    TqSocketHandle Socket{TqInvalidSocket};
};
