#pragma once

#include "admin_http.h"
#include "server_metrics.h"

#include <cstdint>

std::string TqHandleServerAdmin(
    const TqHttpRequest& req,
    TqServerMetrics& metrics,
    uint64_t uptimeSeconds);
