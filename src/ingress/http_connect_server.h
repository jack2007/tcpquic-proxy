#pragma once

#include "control_protocol.h"
#include "tcp_tunnel.h"

#include <string>

int TqHttpStatusForOpenError(TqOpenError error);
bool TqParseHttpConnectRequest(const std::string& request, TunnelRequest& out);
