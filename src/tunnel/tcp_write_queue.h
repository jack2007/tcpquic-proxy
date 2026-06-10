// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "platform_socket.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

struct TqTcpWriteChunk {
    std::vector<uint8_t> Data;
    bool Fin{false};
};

// Bounded async TCP write queue for the QUIC receive path.
// Producer: MsQuic RECEIVE callback (Enqueue copies payload and returns immediately).
// Consumer: dedicated writer thread (blocking send via WriteAll).
// Enqueue returning false means the caller should abort the stream.
class TqTcpWriteQueue {
public:
    TqTcpWriteQueue(TqSocketHandle tcpFd, std::atomic<bool>* stopFlag, size_t maxChunks, size_t maxBytes);
    ~TqTcpWriteQueue();

    TqTcpWriteQueue(const TqTcpWriteQueue&) = delete;
    TqTcpWriteQueue& operator=(const TqTcpWriteQueue&) = delete;

    bool Start();
    // Best-effort abort: signals the writer and joins it without draining pending chunks.
    void Stop();
    bool Enqueue(const uint8_t* data, size_t len, bool fin);
    bool WaitUntilDrainedOrStopped(int timeoutMs);

private:
    void WriterLoop();
    bool WriteAll(const uint8_t* data, size_t len);

    TqSocketHandle TcpFd;
    std::atomic<bool>* StopFlag;
    size_t MaxChunks;
    size_t MaxBytes;
    std::thread Writer;
    std::mutex Lock;
    std::condition_variable Cv;
    std::deque<TqTcpWriteChunk> Queue;
    size_t QueuedBytes{0};
    bool WriterRunning{false};
    bool StopRequested{false};
};
