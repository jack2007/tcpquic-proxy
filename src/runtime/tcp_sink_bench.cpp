#include "tcp_sink_bench.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr size_t kBufferSize = 1024 * 1024;

struct BenchConfig {
    std::string Mode;
    TqTcpBenchEndpoint Listen;
    TqTcpBenchEndpoint Connect;
    TqTcpBenchEndpoint Proxy;
    bool UseHttpProxy{false};
    uint32_t Streams{1};
    uint32_t DurationSec{30};
    int SocketBufferBytes{64 * 1024 * 1024};
};

struct BenchCounters {
    std::atomic<uint64_t> Bytes{0};
    std::atomic<uint64_t> Ops{0};
    std::atomic<uint64_t> Errors{0};
    std::atomic<uint64_t> Connections{0};
};

bool ParseU32(const char* value, uint32_t& out) {
    if (value == nullptr || *value == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    out = static_cast<uint32_t>(parsed);
    return true;
}

bool ParseI32(const char* value, int& out) {
    uint32_t parsed = 0;
    if (!ParseU32(value, parsed) || parsed > static_cast<uint32_t>(INT32_MAX)) {
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

void TuneSocket(int fd, int socketBufferBytes) {
    if (socketBufferBytes > 0) {
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &socketBufferBytes, sizeof(socketBufferBytes));
        (void)::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &socketBufferBytes, sizeof(socketBufferBytes));
    }
    int one = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

int ConnectTcp(const TqTcpBenchEndpoint& endpoint, int socketBufferBytes, std::string& err) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const std::string port = std::to_string(endpoint.Port);
    addrinfo* results = nullptr;
    const int gai = ::getaddrinfo(endpoint.Host.c_str(), port.c_str(), &hints, &results);
    if (gai != 0) {
        err = ::gai_strerror(gai);
        return -1;
    }

    int fd = -1;
    for (addrinfo* item = results; item != nullptr; item = item->ai_next) {
        fd = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd < 0) {
            continue;
        }
        TuneSocket(fd, socketBufferBytes);
        if (::connect(fd, item->ai_addr, item->ai_addrlen) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(results);
    if (fd < 0) {
        err = std::strerror(errno);
    }
    return fd;
}

bool SendAll(int fd, const uint8_t* data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        const ssize_t sent = ::send(fd, data + offset, length - offset, MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool DoHttpConnect(int fd, const TqTcpBenchEndpoint& target, std::string& err) {
    const std::string request = TqTcpBenchBuildHttpConnectRequest(target);
    if (!SendAll(fd, reinterpret_cast<const uint8_t*>(request.data()), request.size())) {
        err = "failed to send HTTP CONNECT";
        return false;
    }

    std::string response;
    response.reserve(4096);
    char ch = 0;
    while (response.size() < 8192) {
        const ssize_t got = ::recv(fd, &ch, 1, 0);
        if (got == 1) {
            response.push_back(ch);
            if (response.size() >= 4 &&
                response.compare(response.size() - 4, 4, "\r\n\r\n") == 0) {
                break;
            }
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        err = "failed to read HTTP CONNECT response";
        return false;
    }

    if (!TqTcpBenchHttpConnectSucceeded(response)) {
        err = "HTTP CONNECT rejected: " + response.substr(0, response.find("\r\n"));
        return false;
    }
    return true;
}

int ListenTcp(const TqTcpBenchEndpoint& endpoint, int socketBufferBytes, std::string& err) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    const std::string port = std::to_string(endpoint.Port);
    addrinfo* results = nullptr;
    const int gai = ::getaddrinfo(endpoint.Host.c_str(), port.c_str(), &hints, &results);
    if (gai != 0) {
        err = ::gai_strerror(gai);
        return -1;
    }

    int fd = -1;
    for (addrinfo* item = results; item != nullptr; item = item->ai_next) {
        fd = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int one = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        TuneSocket(fd, socketBufferBytes);
        if (::bind(fd, item->ai_addr, item->ai_addrlen) == 0 && ::listen(fd, 1024) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(results);
    if (fd < 0) {
        err = std::strerror(errno);
    }
    return fd;
}

void PrintJson(
    const char* mode,
    uint32_t streams,
    uint32_t durationSec,
    const BenchCounters& counters,
    int exitCode) {
    const uint64_t bytes = counters.Bytes.load();
    const double gbps = durationSec == 0
        ? 0.0
        : static_cast<double>(bytes) * 8.0 / static_cast<double>(durationSec) / 1e9;
    const std::string body = nlohmann::json{
        {"mode", mode},
        {"streams", streams},
        {"duration_sec", durationSec},
        {"bytes", bytes},
        {"gbps", gbps},
        {"ops", counters.Ops.load()},
        {"connections", counters.Connections.load()},
        {"errors", counters.Errors.load()},
        {"exit", exitCode},
    }.dump();
    std::printf("%s\n", body.c_str());
}

int RunServer(const BenchConfig& cfg) {
    std::string err;
    const int listenFd = ListenTcp(cfg.Listen, cfg.SocketBufferBytes, err);
    if (listenFd < 0) {
        std::fprintf(stderr, "listen failed: %s\n", err.c_str());
        return 2;
    }

    BenchCounters counters;
    std::atomic<bool> stop{false};
    std::vector<std::thread> writers;
    writers.reserve(cfg.Streams);
    std::vector<uint8_t> buffer(kBufferSize, 0x5a);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(cfg.DurationSec);
    while (writers.size() < cfg.Streams && std::chrono::steady_clock::now() < deadline) {
        sockaddr_storage addr{};
        socklen_t addrLen = sizeof(addr);
        const int fd = ::accept(listenFd, reinterpret_cast<sockaddr*>(&addr), &addrLen);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            counters.Errors.fetch_add(1);
            break;
        }
        TuneSocket(fd, cfg.SocketBufferBytes);
        counters.Connections.fetch_add(1);
        writers.emplace_back([fd, &stop, &counters, buffer]() mutable {
            while (!stop.load(std::memory_order_relaxed)) {
                const ssize_t sent = ::send(fd, buffer.data(), buffer.size(), MSG_NOSIGNAL);
                if (sent > 0) {
                    counters.Bytes.fetch_add(static_cast<uint64_t>(sent));
                    counters.Ops.fetch_add(1);
                    continue;
                }
                if (sent < 0 && errno == EINTR) {
                    continue;
                }
                counters.Errors.fetch_add(1);
                break;
            }
            ::close(fd);
        });
    }

    std::this_thread::sleep_until(deadline);
    stop.store(true);
    ::close(listenFd);
    for (auto& writer : writers) {
        if (writer.joinable()) {
            writer.join();
        }
    }
    PrintJson("server", cfg.Streams, cfg.DurationSec, counters, 0);
    return 0;
}

int RunClient(const BenchConfig& cfg) {
    BenchCounters counters;
    std::vector<int> fds;
    fds.reserve(cfg.Streams);
    for (uint32_t i = 0; i < cfg.Streams; ++i) {
        std::string err;
        const int fd = ConnectTcp(
            cfg.UseHttpProxy ? cfg.Proxy : cfg.Connect,
            cfg.SocketBufferBytes,
            err);
        if (fd < 0) {
            std::fprintf(stderr, "connect failed: %s\n", err.c_str());
            counters.Errors.fetch_add(1);
            continue;
        }
        if (cfg.UseHttpProxy && !DoHttpConnect(fd, cfg.Connect, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            ::close(fd);
            counters.Errors.fetch_add(1);
            continue;
        }
        counters.Connections.fetch_add(1);
        fds.push_back(fd);
    }

    if (fds.empty()) {
        PrintJson("client", cfg.Streams, cfg.DurationSec, counters, 1);
        return 1;
    }

    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    readers.reserve(fds.size());
    for (int fd : fds) {
        readers.emplace_back([fd, &stop, &counters]() {
            std::vector<uint8_t> buffer(kBufferSize);
            while (!stop.load(std::memory_order_relaxed)) {
                const ssize_t got = ::recv(fd, buffer.data(), buffer.size(), 0);
                if (got > 0) {
                    counters.Bytes.fetch_add(static_cast<uint64_t>(got));
                    counters.Ops.fetch_add(1);
                    continue;
                }
                if (got < 0 && errno == EINTR) {
                    continue;
                }
                if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    continue;
                }
                if (stop.load(std::memory_order_relaxed)) {
                    break;
                }
                if (got < 0) {
                    counters.Errors.fetch_add(1);
                }
                break;
            }
            ::close(fd);
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(cfg.DurationSec));
    stop.store(true);
    for (int fd : fds) {
        (void)::shutdown(fd, SHUT_RDWR);
    }
    for (auto& reader : readers) {
        if (reader.joinable()) {
            reader.join();
        }
    }
    PrintJson("client", cfg.Streams, cfg.DurationSec, counters, counters.Errors.load() == 0 ? 0 : 1);
    return counters.Errors.load() == 0 ? 0 : 1;
}

void Usage() {
    std::fprintf(stderr,
        "usage:\n"
        "  tcpquic_tcp_sink_bench server --listen <host:port> [--streams n] [--duration sec]\n"
        "  tcpquic_tcp_sink_bench client --connect <host:port> [--proxy http://host:port] [--streams n] [--duration sec]\n");
}

bool ParseArgs(int argc, char** argv, BenchConfig& cfg, std::string& err) {
    if (argc < 2) {
        err = "missing mode";
        return false;
    }
    cfg.Mode = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                err = "missing value for " + arg;
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--listen") {
            const char* value = next();
            if (value == nullptr || !TqTcpBenchParseEndpoint(value, cfg.Listen, err)) {
                return false;
            }
        } else if (arg == "--connect") {
            const char* value = next();
            if (value == nullptr || !TqTcpBenchParseEndpoint(value, cfg.Connect, err)) {
                return false;
            }
        } else if (arg == "--proxy") {
            const char* value = next();
            if (value == nullptr) {
                return false;
            }
            std::string proxy = value;
            constexpr const char* prefix = "http://";
            if (proxy.find(prefix) != 0 ||
                !TqTcpBenchParseEndpoint(proxy.substr(std::strlen(prefix)), cfg.Proxy, err)) {
                err = "only http://host:port proxy is supported";
                return false;
            }
            cfg.UseHttpProxy = true;
        } else if (arg == "--streams") {
            const char* value = next();
            if (value == nullptr || !ParseU32(value, cfg.Streams) || cfg.Streams == 0) {
                err = "invalid streams";
                return false;
            }
        } else if (arg == "--duration") {
            const char* value = next();
            if (value == nullptr || !ParseU32(value, cfg.DurationSec) || cfg.DurationSec == 0) {
                err = "invalid duration";
                return false;
            }
        } else if (arg == "--socket-buffer") {
            const char* value = next();
            if (value == nullptr || !ParseI32(value, cfg.SocketBufferBytes) ||
                cfg.SocketBufferBytes <= 0) {
                err = "invalid socket buffer";
                return false;
            }
        } else {
            err = "unknown argument: " + arg;
            return false;
        }
    }
    if (cfg.Mode == "server") {
        if (cfg.Listen.Host.empty() || cfg.Listen.Port == 0) {
            err = "server requires --listen";
            return false;
        }
        return true;
    }
    if (cfg.Mode == "client") {
        if (cfg.Connect.Host.empty() || cfg.Connect.Port == 0) {
            err = "client requires --connect";
            return false;
        }
        return true;
    }
    err = "mode must be server or client";
    return false;
}

} // namespace

bool TqTcpBenchParseEndpoint(
    const std::string& value,
    TqTcpBenchEndpoint& endpoint,
    std::string& err) {
    const size_t colon = value.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size()) {
        err = "endpoint must be host:port";
        return false;
    }
    uint32_t port = 0;
    if (!ParseU32(value.substr(colon + 1).c_str(), port) || port == 0 || port > 65535) {
        err = "invalid endpoint port";
        return false;
    }
    endpoint.Host = value.substr(0, colon);
    endpoint.Port = static_cast<uint16_t>(port);
    return true;
}

std::string TqTcpBenchBuildHttpConnectRequest(const TqTcpBenchEndpoint& endpoint) {
    const std::string authority = endpoint.Host + ":" + std::to_string(endpoint.Port);
    return "CONNECT " + authority + " HTTP/1.1\r\n"
        "Host: " + authority + "\r\n"
        "Proxy-Connection: Keep-Alive\r\n"
        "\r\n";
}

bool TqTcpBenchHttpConnectSucceeded(const std::string& response) {
    if (response.find("HTTP/1.") != 0) {
        return false;
    }
    const size_t firstSpace = response.find(' ');
    if (firstSpace == std::string::npos || firstSpace + 4 > response.size()) {
        return false;
    }
    return response.compare(firstSpace + 1, 3, "200") == 0;
}

int TqTcpSinkBenchMain(int argc, char** argv) {
    BenchConfig cfg;
    std::string err;
    if (!ParseArgs(argc, argv, cfg, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        Usage();
        return 2;
    }
    if (cfg.Mode == "server") {
        return RunServer(cfg);
    }
    return RunClient(cfg);
}
