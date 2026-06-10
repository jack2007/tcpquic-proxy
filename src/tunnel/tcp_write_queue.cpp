// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "tcp_write_queue.h"

#include "platform_socket.h"

TqTcpWriteQueue::TqTcpWriteQueue(
    TqSocketHandle tcpFd,
    std::atomic<bool>* stopFlag,
    size_t maxChunks,
    size_t maxBytes)
    : TcpFd(tcpFd),
      StopFlag(stopFlag),
      MaxChunks(maxChunks),
      MaxBytes(maxBytes) {}

TqTcpWriteQueue::~TqTcpWriteQueue() {
    Stop();
}

bool TqTcpWriteQueue::Start() {
    std::lock_guard<std::mutex> lock(Lock);
    if (WriterRunning) {
        return false;
    }
    StopRequested = false;
    Writer = std::thread(&TqTcpWriteQueue::WriterLoop, this);
    WriterRunning = true;
    return true;
}

void TqTcpWriteQueue::Stop() {
    {
        std::lock_guard<std::mutex> lock(Lock);
        if (!WriterRunning) {
            return;
        }
        StopRequested = true;
    }
    Cv.notify_all();
    if (Writer.joinable()) {
        Writer.join();
    }
    {
        std::lock_guard<std::mutex> lock(Lock);
        Queue.clear();
        QueuedBytes = 0;
        WriterRunning = false;
    }
}

bool TqTcpWriteQueue::Enqueue(const uint8_t* data, size_t len, bool fin) {
    if (StopFlag->load()) {
        return false;
    }
    if (len == 0 && !fin) {
        return false;
    }

    TqTcpWriteChunk chunk;
    if (len > 0) {
        chunk.Data.assign(data, data + len);
    }
    chunk.Fin = fin;

    std::unique_lock<std::mutex> lock(Lock);
    if (!WriterRunning || StopRequested) {
        return false;
    }

    const auto hasSpace = [&]() {
        return Queue.size() < MaxChunks && QueuedBytes + len <= MaxBytes;
    };

    while (!hasSpace()) {
        Cv.wait(lock, [&] {
            return hasSpace() || StopFlag->load() || !WriterRunning || StopRequested;
        });
        if (StopFlag->load() || !WriterRunning || StopRequested) {
            return false;
        }
    }

    QueuedBytes += len;
    Queue.push_back(std::move(chunk));
    lock.unlock();
    Cv.notify_one();
    return true;
}

bool TqTcpWriteQueue::WaitUntilDrainedOrStopped(int timeoutMs) {
    std::unique_lock<std::mutex> lock(Lock);
    const auto done = [&]() {
        return Queue.empty() || StopFlag->load();
    };
    return Cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), done);
}

bool TqTcpWriteQueue::WriteAll(const uint8_t* data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        const int sent = TqSend(TcpFd, data + offset, length - offset, TqSendFlags::NoSignal);
        if (sent > 0) {
            offset += static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && TqSocketInterrupted(TqLastSocketError())) {
            continue;
        }
        return false;
    }
    return true;
}

void TqTcpWriteQueue::WriterLoop() {
    while (!StopFlag->load() && !StopRequested) {
        TqTcpWriteChunk chunk;
        {
            std::unique_lock<std::mutex> lock(Lock);
            Cv.wait(lock, [&] {
                return !Queue.empty() || StopFlag->load() || StopRequested;
            });
            if (Queue.empty()) {
                continue;
            }
            chunk = std::move(Queue.front());
            QueuedBytes -= chunk.Data.size();
            Queue.pop_front();
        }

        if (!chunk.Data.empty() && !WriteAll(chunk.Data.data(), chunk.Data.size())) {
            StopFlag->store(true);
            (void)TqShutdownBoth(TcpFd);
            Cv.notify_all();
            break;
        }
        if (chunk.Fin) {
            StopFlag->store(true);
            (void)TqShutdownSend(TcpFd);
            Cv.notify_all();
            break;
        }
        Cv.notify_all();
    }
}
