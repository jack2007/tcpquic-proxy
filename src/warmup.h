#pragma once

#include "config.h"
#include "quic_session.h"

bool TqRunClientWarmup(QuicClientSession& quic, const TqConfig& cfg);
