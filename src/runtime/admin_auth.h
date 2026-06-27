#pragma once

#include "admin_http.h"

#include <string>

class TqAdminAuth {
public:
    bool InitializeToken();
    bool Authorize(const TqHttpRequest& req) const;
    bool WriteTokenFile(const std::string& path, const std::string& listen, std::string& err);
    bool CleanupTokenFile(const std::string& path) const;
    const std::string& Token() const { return TokenValue; }

    static void SetRuntimeBinaryName(const char* argv0);
    static std::string DefaultTokenFilePath();
    static std::string DefaultTokenFilePath(const std::string& role);

private:
    std::string TokenValue;
};
