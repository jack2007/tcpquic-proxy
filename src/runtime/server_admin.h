#pragma once

#include "admin_http.h"
#include "config.h"
#include "server_metrics.h"

#include <cstdint>

std::string TqHandleServerAdmin(
    const TqHttpRequest& req,
    TqServerMetrics& metrics,
    uint64_t uptimeSeconds,
    const TqConfig& runtimeConfig);

std::string TqHandleServerAdmin(
    const TqHttpRequest& req,
    TqServerMetrics& metrics,
    uint64_t uptimeSeconds);
