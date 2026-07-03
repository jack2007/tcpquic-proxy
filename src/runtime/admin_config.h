#pragma once

#include "config.h"

#include <string>
#include <vector>

std::string TqRuntimeConfigJson(const TqConfig& cfg, bool redact);
std::string TqServerRuntimeConfigJson(const TqConfig& cfg, const std::vector<std::string>& resolvedListens, bool redact);
std::string TqDiagnosticsJson(const TqConfig& cfg);
bool TqApplyDiagnosticsPatch(const std::string& body, TqConfig& cfg, std::string& err);
bool TqApplyRuntimeConfigPatch(const std::string& body, TqConfig& cfg, std::string& err, bool& unsupported);
std::string TqPeerPublicConfigJson(const TqConfig& cfg, const TqPeerConfig& peer);
std::string TqClientPublicConfigJson(const TqConfig& cfg);
std::string TqStructuredErrorJson(const std::string& code, const std::string& message);
