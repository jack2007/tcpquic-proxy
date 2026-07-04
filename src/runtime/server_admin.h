#pragma once

#include "admin_http.h"
#include "acl.h"
#include "config.h"
#include "runtime_config_file_store.h"
#include "server_metrics.h"
#include "server_runtime_config.h"

#include <cstdint>
#include <functional>

using TqServerAdminUpdateAcl = std::function<bool(const TqAcl&)>;

std::string TqHandleServerAdmin(
    const TqHttpRequest& req,
    TqServerMetrics& metrics,
    uint64_t uptimeSeconds,
    TqServerRuntimeConfigState& runtimeConfigState,
    TqRuntimeConfigFileStore* runtimeConfigStore,
    TqServerAdminUpdateAcl updateAcl);

std::string TqHandleServerAdmin(
    const TqHttpRequest& req,
    TqServerMetrics& metrics,
    uint64_t uptimeSeconds,
    const TqConfig& runtimeConfig);

std::string TqHandleServerAdmin(
    const TqHttpRequest& req,
    TqServerMetrics& metrics,
    uint64_t uptimeSeconds);
