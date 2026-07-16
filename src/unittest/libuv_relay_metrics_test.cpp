#include "libuv_allocator.h"
#include "libuv_relay_worker.h"
#include "memory_stats.h"
#include "relay_metrics.h"
#include "stream_lifetime.h"
#include "tuning.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <array>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace {

void CheckAt(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "metrics check failed at line %d\n", line);
        std::abort();
    }
}

#define Check(condition) CheckAt((condition), __LINE__)

bool AcceptAllocatorInstall() noexcept {
    return true;
}

std::atomic<void*> gSendContext{nullptr};
struct PendingWrite {
    uv_write_t* Request{nullptr};
    uv_write_cb Complete{nullptr};
};
std::mutex gWriteMutex;
std::vector<PendingWrite> gPendingWrites;

void QUIC_API FakeReceiveComplete(HQUIC, std::uint64_t) {}

QUIC_STATUS QUIC_API FakeReceiveSetEnabled(HQUIC, BOOLEAN) {
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API FakeStreamShutdown(
    HQUIC, QUIC_STREAM_SHUTDOWN_FLAGS, QUIC_UINT62) {
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS CaptureSend(
    MsQuicStream*,
    const QUIC_BUFFER*,
    std::uint32_t,
    QUIC_SEND_FLAGS,
    void* context) {
    gSendContext.store(context, std::memory_order_release);
    return QUIC_STATUS_SUCCESS;
}

#if TCPQUIC_USE_MIMALLOC
std::mutex gAllocatorMutex;
std::condition_variable gAllocatorCondition;
bool gAllocatorEntered = false;
bool gAllocatorReleased = false;

int BlockingFailedAllocatorReplacement(
    uv_malloc_func,
    uv_realloc_func,
    uv_calloc_func,
    uv_free_func) {
    std::unique_lock<std::mutex> lock(gAllocatorMutex);
    gAllocatorEntered = true;
    gAllocatorCondition.notify_all();
    gAllocatorCondition.wait(lock, [] { return gAllocatorReleased; });
    return UV_EPERM;
}
#endif

int FakeTcpOpen(uv_tcp_t*, uv_os_sock_t) {
    return 0;
}

int FakeReadStart(uv_stream_t*, uv_alloc_cb, uv_read_cb) {
    return 0;
}

int FakeWrite(
    uv_write_t* request,
    uv_stream_t*,
    const uv_buf_t[],
    unsigned,
    uv_write_cb complete) {
    std::lock_guard<std::mutex> guard(gWriteMutex);
    gPendingWrites.push_back({request, complete});
    return 0;
}

TqUvRelayRegistration Registration(
    const std::shared_ptr<TqStreamLifetime>& owner,
    MsQuicStream* stream) {
    TqUvRelayRegistration registration{};
    registration.TcpSocket = static_cast<TqSocketHandle>(71);
    registration.Stream = stream;
    registration.StreamOwner = owner;
    registration.StopControl = std::make_shared<TqRelayStopControl>();
    registration.ControlGeneration = registration.StopControl->Generation;
    registration.TcpReadChunkSize = 4096;
    registration.MaxPendingBufferBytes = 32768;
    registration.MaxBufferedQuicSendBytes = 16384;
    registration.ResumeBufferedQuicSendBytes = 8192;
    return registration;
}

#if TCPQUIC_USE_MIMALLOC
int AcceptAllocatorReplacement(
    uv_malloc_func,
    uv_realloc_func,
    uv_calloc_func,
    uv_free_func) {
    return 0;
}
#endif

void CheckWorkerSchema() {
    auto& runtime = TqUvRelayRuntime::Instance();
    runtime.SetAllocatorInstallerForTest(&AcceptAllocatorInstall);
    auto calls = TqUvProductionCalls();
    calls.TcpOpen = &FakeTcpOpen;
    calls.ReadStart = &FakeReadStart;
    calls.Write = &FakeWrite;
    runtime.SetCallAdapterForTest(&calls);

    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 2;
    tuning.RelayEventQueueCapacity = 32;
    Check(runtime.Start(tuning));

    auto* first = runtime.PickWorker();
    auto* second = runtime.PickWorker();
    Check(first != nullptr && second != nullptr && first != second);
    std::vector<std::shared_ptr<TqStreamLifetime>> owners;
    std::vector<std::shared_ptr<TqUvRelayState>> relays;
    std::vector<std::unique_ptr<ITqCompressor>> compressors;
    std::vector<std::unique_ptr<ITqDecompressor>> decompressors;
    alignas(MsQuicStream)
    std::array<std::array<unsigned char, sizeof(MsQuicStream)>, 2>
        streamStorage{};
    std::array<MsQuicStream*, 2> streams{};
    static QUIC_API_TABLE api{};
    api.StreamReceiveComplete = &FakeReceiveComplete;
    api.StreamReceiveSetEnabled = &FakeReceiveSetEnabled;
    api.StreamShutdown = &FakeStreamShutdown;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&api);
    for (std::size_t index = 0; index < streams.size(); ++index) {
        streams[index] = reinterpret_cast<MsQuicStream*>(
            streamStorage[index].data());
        streams[index]->Handle = reinterpret_cast<HQUIC>(index + 1);
        streams[index]->CleanUpMode = CleanUpManual;
        streams[index]->Callback = MsQuicStream::NoOpCallback;
        streams[index]->Context = nullptr;
    }
    TqUvSetStreamSendHookForTest(&CaptureSend);
    std::size_t registrationIndex = 0;
    for (auto* worker : {first, second}) {
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Started);
        Check(owner->InstallDetachedStreamForTest(streams[registrationIndex]));
        const auto registered = worker->RegisterRelayWithId(
            Registration(owner, streams[registrationIndex]));
        Check(registered.Ok);
        auto relay = worker->RelayForTest(registered.RelayId);
        Check(relay != nullptr);
        relays.push_back(std::move(relay));
        owners.push_back(std::move(owner));
        ++registrationIndex;
    }
    for (std::size_t index = 0; index < relays.size(); ++index) {
        compressors.push_back(TqCreateCompressor(TqCompressAlgo::Zstd, 1));
        Check(compressors.back() != nullptr);
        std::atomic<bool> completed{false};
        Check((index == 0 ? first : second)->Post(
            [&, index](TqUvRelayWorker& worker) {
                auto& relay = *relays[index];
                relay.Compressor = compressors[index].get();
                relay.CompressAlgo = TqCompressAlgo::Zstd;
                auto buffer = TqUvStageTcpReadBufferForTest(relay, 256);
                Check(buffer.base != nullptr && buffer.len >= 256);
                std::memset(buffer.base, static_cast<int>('a' + index), 256);
                gSendContext.store(nullptr, std::memory_order_release);
                TqUvHandleTcpRead(worker, relay, 256, buffer);
                Check(relay.QuicSends.size() == 1);
                void* context = gSendContext.load(std::memory_order_acquire);
                Check(context != nullptr);
                Check(relay.StreamOwner->CancelSendCompletion(context));
                auto* operation = relay.QuicSends.begin()->second.get();
                TqUvHandleSendComplete(worker, *operation, false);
                completed.store(true, std::memory_order_release);
            }));
        while (!completed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    for (std::size_t index = 0; index < relays.size(); ++index) {
        std::atomic<bool> accounted{false};
        Check((index == 0 ? first : second)->Post(
            [&, index](TqUvRelayWorker& worker) {
                Check(worker.AddPendingBytes(
                    *relays[index], TqUvPendingDirection::QuicToTcp, 4));
                accounted.store(true, std::memory_order_release);
            }));
        while (!accounted.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    std::uint64_t expectedCompressedInput = 0;
    for (std::size_t index = 0; index < relays.size(); ++index) {
        auto encoder = TqCreateCompressor(TqCompressAlgo::Zstd, 1);
        Check(encoder != nullptr);
        std::vector<std::uint8_t> plain(256, static_cast<std::uint8_t>('x' + index));
        std::vector<std::uint8_t> compressed;
        Check(encoder->Compress(
            plain.data(), plain.size(), compressed, true));
        Check(!compressed.empty());
        expectedCompressedInput += compressed.size();
        decompressors.push_back(TqCreateDecompressor(TqCompressAlgo::Zstd));
        Check(decompressors.back() != nullptr);
        relays[index]->Decompressor = decompressors[index].get();
        relays[index]->CompressAlgo = TqCompressAlgo::Zstd;
        QUIC_BUFFER buffer{
            static_cast<std::uint32_t>(compressed.size()), compressed.data()};
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        event.RECEIVE.TotalBufferLength = buffer.Length;
        event.RECEIVE.BufferCount = 1;
        event.RECEIVE.Buffers = &buffer;
        Check(relays[index]->Binding->OnStreamEvent(
            streams[index], &event, relays[index]->RouteGeneration) ==
            QUIC_STATUS_PENDING);
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        PendingWrite write{};
        for (;;) {
            {
                std::lock_guard<std::mutex> guard(gWriteMutex);
                if (!gPendingWrites.empty()) {
                    write = gPendingWrites.front();
                    gPendingWrites.erase(gPendingWrites.begin());
                }
            }
            if (write.Request != nullptr ||
                std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            std::this_thread::yield();
        }
        Check(write.Request != nullptr && write.Complete != nullptr);
        std::atomic<bool> completed{false};
        Check((index == 0 ? first : second)->Post(
            [&, write](TqUvRelayWorker&) {
                write.Complete(write.Request, 0);
                completed.store(true, std::memory_order_release);
            }));
        while (!completed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    std::mutex mutex;
    std::condition_variable condition;
    bool loopBlocked = false;
    bool loopReleased = false;
    Check(first->Post([&](TqUvRelayWorker&) {
        {
            std::lock_guard<std::mutex> guard(mutex);
            loopBlocked = true;
        }
        condition.notify_one();
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        {
            std::lock_guard<std::mutex> guard(mutex);
            loopReleased = true;
        }
        condition.notify_one();
    }));
    {
        std::unique_lock<std::mutex> lock(mutex);
        Check(condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return loopBlocked;
        }));
    }
    Check(first->Post([](TqUvRelayWorker&) {}));
    Check(first->Post([](TqUvRelayWorker&) {}));
    {
        std::unique_lock<std::mutex> lock(mutex);
        Check(condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return loopReleased;
        }));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    const auto body = nlohmann::json::parse(TqRelayWorkersJson());
    Check(body.at("compiled_relay_backend") == "libuv");
    Check(body.at("snapshot_complete") == true);
    Check(body.at("workers").size() == 3);

    const auto& aggregate = body.at("workers").at(0);
    Check(aggregate.at("backend") == "libuv");
    Check(aggregate.at("worker_id") == "aggregate");
    Check(aggregate.at("active_relays") == 2);
    Check(aggregate.at("pending_bytes") == 8);
    Check(aggregate.at("tcp_read_bytes") == 512);
    Check(aggregate.at("quic_to_tcp_pending_bytes") == 8);
    Check(aggregate.at("tcp_to_quic_pending_bytes") == 0);
    Check(aggregate.at("loop_alive") == true);
    Check(aggregate.at("alive_workers") == 2);
    Check(aggregate.at("expected_workers") == 2);
    Check(aggregate.contains("queue_depth"));
    Check(aggregate.contains("wake_attempts"));
    Check(aggregate.contains("wake_coalesced"));
    Check(aggregate.contains("loop_lag_us"));
    Check(aggregate.contains("errors"));

    const auto& detail = body.at("workers").at(1);
    Check(detail.at("backend") == "libuv");
    Check(detail.at("worker_id") == "libuv-0");
    Check(detail.at("worker_index") == 0);
    Check(detail.at("loop_alive") == true);
    Check(detail.at("event_queue_capacity") == 32);
    Check(detail.at("loop_lag_us").get<std::uint64_t>() >= 20000);
    Check(detail.at("wake_coalesced").get<std::uint64_t>() >= 1);
    Check(detail.at("wake_attempts") ==
          detail.at("wake_successes").get<std::uint64_t>() +
          detail.at("wake_failures").get<std::uint64_t>());
    Check(detail.at("loop_iterations").get<std::uint64_t>() >= 1);
    Check(detail.at("tcp_read_bytes") == 256);
    Check(detail.at("pending_bytes") == 4);
    Check(detail.at("quic_to_tcp_pending_bytes") == 4);
    Check(body.at("workers").at(2).at("tcp_read_bytes") == 256);
    Check(body.at("workers").at(2).at("pending_bytes") == 4);

    const auto firstMetrics = first->Snapshot();
    const auto secondMetrics = second->Snapshot();
    Check(firstMetrics.CompressedTcpBytes > 0);
    Check(secondMetrics.CompressedTcpBytes > 0);
    Check(firstMetrics.DecompressedTcpBytes == 256);
    Check(secondMetrics.DecompressedTcpBytes == 256);
    const auto aggregateMetrics = nlohmann::json::parse(
        TqRelayMetricsFieldsJson(TqSnapshotRelayMetrics()));
    Check(aggregateMetrics.at("compiled_relay_backend") == "libuv");
    Check(aggregateMetrics.at("relay_backend") == "libuv");
    Check(aggregateMetrics.contains("relay_pending_events"));
    Check(aggregateMetrics.contains("relay_pending_bytes"));
    Check(aggregateMetrics.contains("relay_errors"));
    Check(aggregateMetrics.at("relay_tcp_read_bytes") == 512);
    Check(aggregateMetrics.at("linux_relay_compressed_tcp_bytes") ==
          firstMetrics.CompressedTcpBytes + secondMetrics.CompressedTcpBytes);
    Check(aggregateMetrics.at("relay_tcp_write_bytes") == 512);
    Check(aggregateMetrics.at("linux_relay_decompressed_tcp_bytes") == 512);
    Check(aggregateMetrics.at("linux_relay_zstd_decompress_input_bytes") ==
          expectedCompressedInput);
    Check(aggregateMetrics.at("linux_relay_zstd_decompress_output_bytes") == 512);
    Check(aggregateMetrics.at("linux_relay_zstd_decompress_calls") >= 2);
    Check(aggregateMetrics.at("linux_relay_tcp_to_quic_compress_failures") == 0);
    Check(aggregateMetrics.at("linux_relay_quic_receive_decompress_failures") == 0);

    std::atomic<bool> firstOwnershipCleared{false};
    Check(first->Post([&](TqUvRelayWorker& worker) {
        relays[0]->PrecommitReceives.clear();
        relays[0]->PendingQuicReceiveBytes = 0;
        Check(worker.CompletePendingBytes(
            *relays[0], TqUvPendingDirection::QuicToTcp, 4));
        firstOwnershipCleared.store(true, std::memory_order_release);
    }));
    while (!firstOwnershipCleared.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    Check(owners[0]->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    Check(first->StopForTest());
    const auto degraded = nlohmann::json::parse(TqRelayWorkersJson());
    const auto& degradedAggregate = degraded.at("workers").at(0);
    Check(degradedAggregate.at("loop_alive") == false);
    Check(degradedAggregate.at("alive_workers") == 1);
    Check(degradedAggregate.at("expected_workers") == 2);
    Check(degradedAggregate.at("active_relays") == 1);
    Check(degradedAggregate.at("pending_bytes") == 4);

    std::atomic<bool> secondOwnershipCleared{false};
    Check(second->Post([&](TqUvRelayWorker& worker) {
        relays[1]->PrecommitReceives.clear();
        relays[1]->PendingQuicReceiveBytes = 0;
        Check(worker.CompletePendingBytes(
            *relays[1], TqUvPendingDirection::QuicToTcp, 4));
        secondOwnershipCleared.store(true, std::memory_order_release);
    }));
    while (!secondOwnershipCleared.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    Check(owners[1]->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    Check(second->StopForTest());

    runtime.StopForTest();
    runtime.ResetTestHooksForTest();
    TqUvSetStreamSendHookForTest(nullptr);
    for (const auto& owner : owners) {
        owner->ReleaseStreamForTest();
    }
    MsQuic = nullptr;
}

void CheckAllocatorSchema() {
    TqUvResetAllocatorStateForTest();
#if TCPQUIC_USE_MIMALLOC
    {
        std::lock_guard<std::mutex> guard(gAllocatorMutex);
        gAllocatorEntered = false;
        gAllocatorReleased = false;
    }
    TqUvSetReplaceAllocatorForTest(&BlockingFailedAllocatorReplacement);
    std::thread installer([] { Check(!TqUvInstallAllocator()); });
    {
        std::unique_lock<std::mutex> lock(gAllocatorMutex);
        Check(gAllocatorCondition.wait_for(lock, std::chrono::seconds(2), [] {
            return gAllocatorEntered;
        }));
    }
    auto body = nlohmann::json::parse(
        TqMemoryAllocatorStatsJson(TqSnapshotMemoryAllocatorStats()));
    Check(body.at("libuv_allocator_mode") == "mimalloc");
    Check(body.at("libuv_allocator_attempted") == true);
    Check(body.at("libuv_allocator_in_progress") == true);
    Check(body.at("libuv_allocator_installed") == false);
    Check(body.at("libuv_allocator_status") == 0);
    {
        std::lock_guard<std::mutex> guard(gAllocatorMutex);
        gAllocatorReleased = true;
    }
    gAllocatorCondition.notify_all();
    installer.join();
    body = nlohmann::json::parse(
        TqMemoryAllocatorStatsJson(TqSnapshotMemoryAllocatorStats()));
    Check(body.at("libuv_allocator_in_progress") == false);
    Check(body.at("libuv_allocator_installed") == false);
    Check(body.at("libuv_allocator_status") == UV_EPERM);

    TqUvResetAllocatorStateForTest();
    TqUvSetReplaceAllocatorForTest(&AcceptAllocatorReplacement);
    Check(TqUvInstallAllocator());

    body = nlohmann::json::parse(
        TqMemoryAllocatorStatsJson(TqSnapshotMemoryAllocatorStats()));
    Check(body.at("compiled_relay_backend") == "libuv");
    Check(body.at("libuv_allocator_mode") == "mimalloc");
    Check(body.at("libuv_allocator_attempted") == true);
    Check(body.at("libuv_allocator_installed") == true);
    Check(body.at("libuv_allocator_status") == 0);
#else
    Check(TqUvInstallAllocator());
    const auto body = nlohmann::json::parse(
        TqMemoryAllocatorStatsJson(TqSnapshotMemoryAllocatorStats()));
    Check(body.at("compiled_relay_backend") == "libuv");
    Check(body.at("libuv_allocator_mode") == "system");
    Check(body.at("libuv_allocator_attempted") == false);
    Check(body.at("libuv_allocator_in_progress") == false);
    Check(body.at("libuv_allocator_installed") == false);
    Check(body.at("libuv_allocator_status") == 0);
#endif
}

} // namespace

int main() {
    CheckAllocatorSchema();
    CheckWorkerSchema();
    return 0;
}
