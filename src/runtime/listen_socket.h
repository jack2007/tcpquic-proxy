#pragma once

#include "platform_socket.h"

#include <string>

struct TqListenSocket {
    TqSocketHandle Fd{TqInvalidSocket};
    std::string Address;
};

bool TqCreateNonBlockingListenSocket(const std::string& listen, TqListenSocket& out);
