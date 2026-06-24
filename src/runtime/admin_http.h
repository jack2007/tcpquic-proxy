#pragma once

#include "platform_socket.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct TqHttpRequest {
    std::string Method;
    std::string Path;
    std::string Body;
    std::map<std::string, std::string> Headers;
};

using TqHttpHandler = std::function<std::string(const TqHttpRequest&)>;

bool TqParseHttpRequest(const std::string& raw, TqHttpRequest& req, std::string& err);
std::string TqJsonResponse(int status, const std::string& json);
bool TqValidateAdminListen(const std::string& listen, std::string& err);

struct TqAdminHttpServerOptions {
    uint32_t AdminThreads{2};
    size_t MaxBodyBytes{1024 * 1024};
    std::string TokenFile;
    bool EnableTokenAuth{false};
    bool AllowUnauthenticatedLegacy{false};
};

namespace httplib {
class Server;
}

class TqAdminAuth;

class TqAdminHttpServer {
public:
    TqAdminHttpServer(std::string listen, TqHttpHandler handler);
    TqAdminHttpServer(std::string listen, TqHttpHandler handler, TqAdminHttpServerOptions options);
    ~TqAdminHttpServer();
    bool Start(std::string& err);
    void Stop();
    std::string ListenAddress() const;
    std::string AuthTokenForTesting() const;

private:
    void ConfigureRoutes();
    void Run();
    std::string Listen;
    std::string BoundListen;
    TqHttpHandler Handler;
    TqAdminHttpServerOptions Options;
    std::unique_ptr<httplib::Server> Server;
    std::unique_ptr<TqAdminAuth> Auth;
    std::string TokenFilePath;
    std::atomic<bool> Started{false};
    std::thread Thread;
    mutable std::mutex Lock;
};
