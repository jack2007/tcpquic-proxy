#pragma once

#include "platform_socket.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct TqHttpRequest {
    std::string Method;
    std::string Path;
    std::string Body;
};

using TqHttpHandler = std::function<std::string(const TqHttpRequest&)>;

bool TqParseHttpRequest(const std::string& raw, TqHttpRequest& req, std::string& err);
std::string TqJsonResponse(int status, const std::string& json);
bool TqValidateAdminListen(const std::string& listen, std::string& err);

class TqAdminHttpServer {
public:
    TqAdminHttpServer(std::string listen, TqHttpHandler handler);
    ~TqAdminHttpServer();
    bool Start(std::string& err);
    void Stop();
    std::string ListenAddress() const;

private:
    void Run(TqSocketHandle listenFd);
    void HandleClient(uint64_t clientId, TqSocketHandle clientFd, TqHttpHandler handler);
    void RemoveActiveClientLocked(uint64_t clientId);
    struct ActiveClient {
        uint64_t Id;
        TqSocketHandle Fd{TqInvalidSocket};
    };
    std::string Listen;
    std::string BoundListen;
    TqHttpHandler Handler;
    std::atomic<bool> Stopping{false};
    std::atomic<uint64_t> NextClientId{1};
    std::atomic<TqSocketHandle> ListenFd{TqInvalidSocket};
    std::thread Thread;
    mutable std::mutex Lock;
    std::condition_variable ActiveClientsDrained;
    std::condition_variable ClientThreadsDrained;
    std::vector<ActiveClient> ActiveClients;
    uint64_t ActiveClientThreads{0};
};
