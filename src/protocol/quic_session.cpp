#include "quic_session.h"
#include "relay.h"
#include "trace.h"
#include "tunnel_registry.h"
#include "tuning.h"
#if defined(_WIN32) && !defined(TCPQUIC_WINDOWS_TLS_QUICTLS)
#include "quic_credentials_win.h"
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <unordered_map>

extern const MsQuicApi* MsQuic;

namespace {

constexpr char TqAlpn[] = "tcpquic-tunnel/1";
constexpr uint64_t TqIdleTimeoutMs = 60 * 1000;

struct TqEndpoint {
    std::string Host;
    uint16_t Port{0};
};

struct TqCredentialConfig {
    QUIC_CREDENTIAL_CONFIG Config{};
    QUIC_CERTIFICATE_FILE CertFile{};

    TqCredentialConfig(const TqConfig& cfg, QUIC_CREDENTIAL_FLAGS flags) {
        CertFile.PrivateKeyFile = cfg.QuicKey.c_str();
        CertFile.CertificateFile = cfg.QuicCert.c_str();
        Config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
        Config.Flags = flags | QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE;
        Config.CertificateFile = &CertFile;
        Config.CaCertificateFile = cfg.QuicCa.c_str();
    }
};

bool ParsePort(const std::string& value, uint16_t& port) {
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' ||
        parsed == 0 || parsed > std::numeric_limits<uint16_t>::max()) {
        return false;
    }
    port = static_cast<uint16_t>(parsed);
    return true;
}

bool ParseEndpoint(const std::string& value, TqEndpoint& endpoint) {
    if (value.empty()) {
        return false;
    }

    std::string host;
    std::string portText;
    if (value[0] == '[') {
        const size_t close = value.find(']');
        if (close == std::string::npos ||
            close + 1 >= value.size() ||
            value[close + 1] != ':') {
            return false;
        }
        host = value.substr(1, close - 1);
        portText = value.substr(close + 2);
    } else {
        const size_t colon = value.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size()) {
            return false;
        }
        host = value.substr(0, colon);
        portText = value.substr(colon + 1);
    }

    uint16_t port = 0;
    if (host.empty() || !ParsePort(portText, port)) {
        return false;
    }

    endpoint.Host = host;
    endpoint.Port = port;
    return true;
}

bool ConvertListenAddress(const TqEndpoint& endpoint, QUIC_ADDR& address) {
    if (endpoint.Host == "*") {
        std::memset(&address, 0, sizeof(address));
        QuicAddrSetFamily(&address, QUIC_ADDRESS_FAMILY_UNSPEC);
        QuicAddrSetPort(&address, endpoint.Port);
        return true;
    }
    return QuicAddrFromString(endpoint.Host.c_str(), endpoint.Port, &address);
}

std::string TqFormatConnectionAddr(MsQuicConnection* connection, uint32_t param) {
    if (connection == nullptr || !connection->IsValid()) {
        return "?";
    }
    QUIC_ADDR addr{};
    uint32_t len = sizeof(addr);
    if (QUIC_FAILED(connection->GetParam(param, &len, &addr))) {
        return "?";
    }
    QUIC_ADDR_STR addrStr{};
    if (!QuicAddrToString(&addr, &addrStr)) {
        return "?";
    }
    return addrStr.Address;
}

}

MsQuicSettings TqMakeMsQuicSettings(const TqConfig& cfg, bool server) {
    MsQuicSettings settings;
    settings.SetCongestionControlAlgorithm(QUIC_CONGESTION_CONTROL_ALGORITHM_BBR);
    settings.SetKeepAlive(cfg.QuicKeepAliveIntervalMs);
    settings.SetIdleTimeoutMs(TqIdleTimeoutMs);
    settings.SetPeerBidiStreamCount(static_cast<uint16_t>(cfg.QuicConnectionStreamCount));
    settings.SetStreamRecvWindowDefault(cfg.Tuning.StreamRecvWindow);
    settings.SetConnFlowControlWindow(cfg.Tuning.ConnFlowControlWindow);
    settings.InitialWindowPackets = cfg.Tuning.InitialWindowPackets;
    settings.IsSet.InitialWindowPackets = TRUE;
    settings.SetInitialRttMs(cfg.Tuning.InitialRttMs);
    settings.SetSendBufferingEnabled(false);
    settings.SetPacingEnabled(true);
    settings.SetStreamMultiReceiveEnabled(true);
    settings.SetNetStatsEventEnabled(true);
    if (server) {
        settings.SetServerResumptionLevel(QUIC_SERVER_RESUME_ONLY);
    }
    return settings;
}

namespace {

QUIC_EXECUTION_PROFILE TqToMsQuicProfile(TqQuicProfile profile) {
    switch (profile) {
    case TqQuicProfile::LowLatency:
        return QUIC_EXECUTION_PROFILE_LOW_LATENCY;
    default:
        return QUIC_EXECUTION_PROFILE_TYPE_MAX_THROUGHPUT;
    }
}

std::mutex g_msquicApiLock;
std::weak_ptr<MsQuicApi> g_msquicApi;

void ReleaseSharedMsQuicApi(MsQuicApi* api) {
    {
        std::lock_guard<std::mutex> guard(g_msquicApiLock);
        if (MsQuic == api) {
            MsQuic = nullptr;
        }
    }
    delete api;
}

bool AcquireSharedMsQuicApi(std::shared_ptr<MsQuicApi>& api) {
    {
        std::lock_guard<std::mutex> guard(g_msquicApiLock);
        if (auto existing = g_msquicApi.lock()) {
            api = std::move(existing);
            return true;
        }
    }

    std::shared_ptr<MsQuicApi> created(new (std::nothrow) MsQuicApi(), ReleaseSharedMsQuicApi);
    if (!created || !created->IsValid()) {
        std::fprintf(stderr, "MsQuicOpenVersion failed, 0x%x\n",
            created ? created->GetInitStatus() : QUIC_STATUS_OUT_OF_MEMORY);
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(g_msquicApiLock);
        if (auto existing = g_msquicApi.lock()) {
            api = std::move(existing);
            return true;
        }
        MsQuic = created.get();
        g_msquicApi = created;
        api = std::move(created);
    }
    return true;
}

bool InitApiAndRegistration(
    std::shared_ptr<MsQuicApi>& api,
    std::unique_ptr<MsQuicRegistration>& registration,
    QUIC_EXECUTION_PROFILE profile) {
    if (!AcquireSharedMsQuicApi(api)) {
        return false;
    }

    registration = std::make_unique<MsQuicRegistration>(
        "tcpquic-proxy",
        profile,
        true);
    if (!registration || !registration->IsValid()) {
        std::fprintf(stderr, "RegistrationOpen failed, 0x%x\n",
            registration ? registration->GetInitStatus() : QUIC_STATUS_OUT_OF_MEMORY);
        return false;
    }
    return true;
}

bool InitConfiguration(
    const TqConfig& cfg,
    const MsQuicRegistration& registration,
    bool server,
    std::unique_ptr<MsQuicConfiguration>& configuration
#if defined(_WIN32) && !defined(TCPQUIC_WINDOWS_TLS_QUICTLS)
    ,
    std::unique_ptr<TqQuicCredentialHolder>& credentials
#endif
    ) {
    MsQuicAlpn alpn(TqAlpn);
    MsQuicSettings settings = TqMakeMsQuicSettings(cfg, server);
#if defined(_WIN32) && defined(TCPQUIC_WINDOWS_TLS_QUICTLS)
    const auto flags = server
        ? QUIC_CREDENTIAL_FLAG_NONE
        : static_cast<QUIC_CREDENTIAL_FLAGS>(QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION);
#else
#if defined(_WIN32)
    const auto flags = server
        ? QUIC_CREDENTIAL_FLAG_NONE
        : static_cast<QUIC_CREDENTIAL_FLAGS>(QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION);
#else
    const auto flags = server
        ? QUIC_CREDENTIAL_FLAG_REQUIRE_CLIENT_AUTHENTICATION
        : QUIC_CREDENTIAL_FLAG_CLIENT;
#endif
#endif

    const QUIC_CREDENTIAL_CONFIG* credentialConfig = nullptr;
#if defined(_WIN32) && !defined(TCPQUIC_WINDOWS_TLS_QUICTLS)
    credentials = std::make_unique<TqQuicCredentialHolder>();
    if (!credentials->Build(cfg, flags)) {
        credentials.reset();
        return false;
    }
    credentialConfig = &credentials->Config();
#else
    TqCredentialConfig credential(cfg, flags);
    credentialConfig = &credential.Config;
#endif

    configuration = std::make_unique<MsQuicConfiguration>(
        registration,
        alpn,
        settings,
        MsQuicCredentialConfig(*credentialConfig));
    if (!configuration || !configuration->IsValid()) {
        std::fprintf(stderr, "ConfigurationOpen/LoadCredential failed, 0x%x\n",
            configuration ? configuration->GetInitStatus() : QUIC_STATUS_OUT_OF_MEMORY);
#if defined(_WIN32) && !defined(TCPQUIC_WINDOWS_TLS_QUICTLS)
        credentials.reset();
#endif
        return false;
    }
    return true;
}

bool TqSetDisable1RttEncryption(MsQuicConnection* connection, const char* role) {
    const uint8_t value = TRUE;
    const QUIC_STATUS status = connection->SetParam(
        QUIC_PARAM_CONN_DISABLE_1RTT_ENCRYPTION,
        sizeof(value),
        &value);
    if (QUIC_FAILED(status)) {
        std::fprintf(stderr,
            "tcpquic-proxy: failed to disable QUIC 1-RTT encryption on %s connection, 0x%x\n",
            role,
            status);
        return false;
    }
    return true;
}

std::mutex g_serverConnIdLock;
std::unordered_map<HQUIC, uint32_t> g_serverConnIds;
std::atomic<uint32_t> g_nextServerConnId{1};

std::mutex g_clientTraceConnLock;
std::unordered_map<HQUIC, uint32_t> g_clientTraceConnIds;

void TqSampleConnectionRtt(MsQuicConnection* connection) {
    if (connection == nullptr || !connection->IsValid()) {
        return;
    }

    QUIC_STATISTICS_V2 stats{};
    if (QUIC_FAILED(connection->GetStatistics(&stats))) {
        return;
    }

    const uint32_t sampleUs = stats.MinRtt > 0 ? stats.MinRtt : stats.Rtt;
    if (sampleUs == 0) {
        return;
    }

    TqRecordMeasuredRtt(std::max(1u, sampleUs / 1000));
}

} // namespace

uint32_t TqRegisterServerConnection(MsQuicConnection* connection) {
    if (connection == nullptr || connection->Handle == nullptr) {
        return 0;
    }
    const uint32_t id = g_nextServerConnId.fetch_add(1);
    std::lock_guard<std::mutex> guard(g_serverConnIdLock);
    g_serverConnIds[connection->Handle] = id;
    return id;
}

uint32_t TqLookupServerConnectionId(MsQuicConnection* connection) {
    if (connection == nullptr || connection->Handle == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> guard(g_serverConnIdLock);
    const auto it = g_serverConnIds.find(connection->Handle);
    return it == g_serverConnIds.end() ? 0 : it->second;
}

void TqUnregisterServerConnection(MsQuicConnection* connection) {
    if (connection == nullptr || connection->Handle == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_serverConnIdLock);
    g_serverConnIds.erase(connection->Handle);
}

uint32_t TqLookupClientTraceConnId(MsQuicConnection* connection) {
    if (connection == nullptr || connection->Handle == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> guard(g_clientTraceConnLock);
    const auto it = g_clientTraceConnIds.find(connection->Handle);
    return it == g_clientTraceConnIds.end() ? 0 : it->second;
}

static void TqRegisterClientTraceConnection(MsQuicConnection* connection, size_t slotIndex) {
    if (connection == nullptr || connection->Handle == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_clientTraceConnLock);
    g_clientTraceConnIds[connection->Handle] = static_cast<uint32_t>(slotIndex + 1);
}

static void TqUnregisterClientTraceConnection(MsQuicConnection* connection) {
    if (connection == nullptr || connection->Handle == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_clientTraceConnLock);
    g_clientTraceConnIds.erase(connection->Handle);
}

static bool TqEnvFlagEnabled(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
        return false;
    }
    const bool enabled = len > 1 && std::strcmp(value, "0") != 0;
    std::free(value);
    return enabled;
#else
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
#endif
}

static bool TqClientDebugEnabled() {
    static const bool enabled = TqEnvFlagEnabled("TQ_QUIC_CLIENT_DEBUG");
    return enabled;
}

static void TqClientDebugLog(const char* message, size_t slotIndex, const void* connection, QUIC_STATUS status = QUIC_STATUS_SUCCESS) {
    if (!TqClientDebugEnabled()) {
        return;
    }
    std::fprintf(stderr,
        "tcpquic-proxy quic-client-debug: %s slot=%zu conn=%p status=0x%x\n",
        message,
        slotIndex + 1,
        connection,
        status);
}

QuicClientSession::~QuicClientSession() {
    Stop();
}

bool QuicClientSession::Start(const TqConfig& cfg) {
    StreamHandler handler;
    ConnectionStateHandler stateHandler;
    {
        std::lock_guard<std::mutex> guard(State->Lock);
        handler = State->PeerStreamHandler;
        stateHandler = State->ConnectionStateChanged;
    }
    Stop(false);
    State = std::make_shared<ClientSharedState>();
    {
        std::lock_guard<std::mutex> guard(State->Lock);
        State->PeerStreamHandler = handler;
        State->ConnectionStateChanged = stateHandler;
    }

    TqEndpoint endpoint;
    if (!ParseEndpoint(cfg.QuicPeer, endpoint)) {
        std::fprintf(stderr, "Invalid --quic-peer endpoint: %s\n", cfg.QuicPeer.c_str());
        return false;
    }

    Config = cfg;
    PeerHost = endpoint.Host;
    PeerPort = endpoint.Port;

    if (!InitApiAndRegistration(Api, Registration, TqToMsQuicProfile(cfg.QuicProfile)) ||
        !InitConfiguration(Config, *Registration, false, Configuration
#if defined(_WIN32) && !defined(TCPQUIC_WINDOWS_TLS_QUICTLS)
            , Credentials
#endif
            )) {
        Stop(false);
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(State->Lock);
        State->Started = true;
        State->Stopping = false;
        State->Api = Api;
        State->Slots.clear();
        State->Slots.resize(Config.QuicConnections);
        PickIndex.store(0);
    }
    StartReconnectLoop();
    return true;
}

void QuicClientSession::DropOrphanedConnection(const std::shared_ptr<ClientSharedState>& state, MsQuicConnection* connection) {
    if (connection == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(state->Lock);
    for (auto it = state->OrphanedConnections.begin(); it != state->OrphanedConnections.end(); ++it) {
        if (it->get() == connection) {
            state->OrphanedConnections.erase(it);
            if (state->OrphanedConnections.empty()) {
                state->Api.reset();
            }
            state->OrphanDrained.notify_all();
            return;
        }
    }
}

void QuicClientSession::WaitForOrphanedConnectionsDrain(const std::shared_ptr<ClientSharedState>& state, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> guard(state->Lock);
    state->OrphanDrained.wait_for(guard, timeout, [&state] { return state->OrphanedConnections.empty(); });
}

void QuicClientSession::Stop() {
    Stop(true);
}

void QuicClientSession::Stop(bool clearHandlers) {
    std::shared_ptr<MsQuicApi> apiLocal;
    std::unique_ptr<MsQuicRegistration> registrationLocal;
    std::unique_ptr<MsQuicConfiguration> configurationLocal;
    std::shared_ptr<ClientSharedState> state = State;

    StopReconnectLoop(state);

    {
        std::lock_guard<std::mutex> guard(state->Lock);
        state->Started = false;
        state->Stopping = true;
        if (clearHandlers) {
            state->PeerStreamHandler = nullptr;
            state->ConnectionStateChanged = nullptr;
        }
        for (auto& slot : state->Slots) {
            if (slot.Connection) {
                slot.Connection->Shutdown(0, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
                state->OrphanedConnections.push_back(std::move(slot.Connection));
            }
            slot.Context = nullptr;
        }
        state->Slots.clear();
    }

    state->StateChanged.notify_all();
    WaitForOrphanedConnectionsDrain(state, std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> guard(state->Lock);
        if (state->OrphanedConnections.empty()) {
            state->Api.reset();
        }
    }

    {
        std::lock_guard<std::mutex> guard(state->Lock);
        apiLocal = std::move(Api);
        registrationLocal = std::move(Registration);
        configurationLocal = std::move(Configuration);
    }
}

MsQuicConnection* QuicClientSession::GetConnection() {
    return PickConnection();
}

MsQuicConnection* QuicClientSession::PickConnection() {
    std::lock_guard<std::mutex> guard(State->Lock);
    if (State->Slots.empty()) {
        return nullptr;
    }
    const size_t count = State->Slots.size();
    for (size_t attempt = 0; attempt < count; ++attempt) {
        const size_t index = PickIndex.fetch_add(1) % count;
        auto& slot = State->Slots[index];
        if (slot.Connected && slot.Connection && slot.Connection->IsValid()) {
            return slot.Connection.get();
        }
    }
    return nullptr;
}

MsQuicConnection* QuicClientSession::PickConnectionAt(size_t index) {
    std::lock_guard<std::mutex> guard(State->Lock);
    if (index >= State->Slots.size()) {
        return nullptr;
    }
    auto& slot = State->Slots[index];
    if (slot.Connected && slot.Connection && slot.Connection->IsValid()) {
        return slot.Connection.get();
    }
    return nullptr;
}

MsQuicConnection* QuicClientSession::PickConnectionFrom(size_t firstIndex) {
    std::lock_guard<std::mutex> guard(State->Lock);
    if (firstIndex >= State->Slots.size()) {
        return nullptr;
    }
    const size_t count = State->Slots.size() - firstIndex;
    for (size_t attempt = 0; attempt < count; ++attempt) {
        const size_t index = firstIndex + (PickIndex.fetch_add(1) % count);
        auto& slot = State->Slots[index];
        if (slot.Connected && slot.Connection && slot.Connection->IsValid()) {
            return slot.Connection.get();
        }
    }
    return nullptr;
}

uint32_t QuicClientSession::ConnectionCount() const {
    return static_cast<uint32_t>(Config.QuicConnections);
}

uint32_t QuicClientSession::ConnectedConnectionCount() const {
    std::lock_guard<std::mutex> guard(State->Lock);
    return ConnectedCountLocked(*State);
}

uint32_t QuicClientSession::ConnectedCountLocked(const ClientSharedState& state) {
    uint32_t count = 0;
    for (const auto& slot : state.Slots) {
        if (slot.Connected && slot.Connection && slot.Connection->IsValid()) {
            ++count;
        }
    }
    return count;
}

bool QuicClientSession::EnsureConnected(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> guard(State->Lock);
    while (State->Started && !State->Stopping) {
        bool anyConnected = false;
        auto nextWake = deadline;
        const auto now = std::chrono::steady_clock::now();
        for (size_t i = 0; i < State->Slots.size(); ++i) {
            if (ConnectedCountLocked(*State) > 0) {
                return true;
            }
            auto& slot = State->Slots[i];
            if (slot.Connected && slot.Connection && slot.Connection->IsValid()) {
                anyConnected = true;
                continue;
            }
            if (slot.ReconnectNeeded || !slot.Connection) {
                if (slot.ReconnectNeeded &&
                    slot.NextReconnectAt != std::chrono::steady_clock::time_point{} &&
                    now < slot.NextReconnectAt) {
                    nextWake = std::min(nextWake, slot.NextReconnectAt);
                    continue;
                }
                guard.unlock();
                StartSlotLocked(i);
                guard.lock();
                if (ConnectedCountLocked(*State) > 0) {
                    return true;
                }
                nextWake = std::min(nextWake, std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
                break;
            }
        }
        if (anyConnected) {
            return true;
        }
        if (State->Slots.empty() || std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        State->StateChanged.wait_until(guard, nextWake);
    }
    return false;
}

bool QuicClientSession::EnsureAnyConnected(std::chrono::milliseconds timeout) {
    return EnsureConnected(timeout);
}

void QuicClientSession::SetPeerStreamHandler(StreamHandler h) {
    std::lock_guard<std::mutex> guard(State->Lock);
    State->PeerStreamHandler = std::move(h);
}

void QuicClientSession::SetConnectionStateHandler(ConnectionStateHandler h) {
    std::lock_guard<std::mutex> guard(State->Lock);
    State->ConnectionStateChanged = std::move(h);
}

void QuicClientSession::AbortAllTunnels() {
    std::vector<MsQuicConnection*> connections;
    {
        std::lock_guard<std::mutex> guard(State->Lock);
        connections.reserve(State->Slots.size());
        for (auto& slot : State->Slots) {
            if (slot.Connection) {
                connections.push_back(slot.Connection.get());
            }
        }
    }

    for (MsQuicConnection* connection : connections) {
        (void)TqAbortConnectionTunnels(connection);
    }
}

void QuicClientSession::NotifyConnectionStateChanged(ConnectionStateNotification notification) {
    if (notification.Handler) {
        notification.Handler(notification.ConnectedCount);
    }
}

void QuicClientSession::StartReconnectLoop() {
    std::shared_ptr<ClientSharedState> state = State;
    auto gate = std::make_shared<ClientReconnectGate>();
    {
        std::lock_guard<std::mutex> guard(gate->Lock);
        gate->Session = this;
    }
    {
        std::lock_guard<std::mutex> guard(state->Lock);
        state->ReconnectInterval = std::chrono::milliseconds(Config.QuicReconnectIntervalMs);
        state->ReconnectGate = gate;
    }

    state->ReconnectThread = std::thread(&QuicClientSession::RunReconnectLoop, state, gate);
}

void QuicClientSession::StopReconnectLoop(const std::shared_ptr<ClientSharedState>& state) {
    if (!state) {
        return;
    }

    std::thread worker;
    std::shared_ptr<ClientReconnectGate> gate;
    {
        std::lock_guard<std::mutex> guard(state->Lock);
        state->Stopping = true;
        state->StateChanged.notify_all();
        worker = std::move(state->ReconnectThread);
        gate = std::move(state->ReconnectGate);
    }
    if (gate) {
        std::lock_guard<std::mutex> guard(gate->Lock);
        gate->Session = nullptr;
    }
    if (worker.joinable()) {
        worker.join();
    }
}

void QuicClientSession::RunReconnectLoop(
    std::shared_ptr<ClientSharedState> state,
    std::shared_ptr<ClientReconnectGate> gate) {
    for (;;) {
        {
            std::unique_lock<std::mutex> guard(state->Lock);
            if (state->Stopping || !state->Started) {
                return;
            }
            state->StateChanged.wait_for(guard, state->ReconnectInterval, [&state] {
                return state->Stopping || !state->Started;
            });
            if (state->Stopping || !state->Started) {
                return;
            }
        }

        // The gate serializes session dereference with Stop()/destructor. Stop
        // clears Session under this lock and joins before session members die.
        std::lock_guard<std::mutex> guard(gate->Lock);
        if (gate->Session == nullptr) {
            return;
        }
        (void)gate->Session->EnsureConnected(std::chrono::milliseconds(100));
        gate->Session->StartAllDueSlots();
    }
}

void QuicClientSession::StartAllDueSlots() {
    const size_t count = Config.QuicConnections;
    for (size_t i = 0; i < count; ++i) {
        {
            std::lock_guard<std::mutex> guard(State->Lock);
            if (!State->Started || State->Stopping) {
                return;
            }
        }
        (void)StartSlotLocked(i);
    }
}

bool QuicClientSession::StartSlotLocked(size_t index) {
    std::unique_ptr<MsQuicConnection> oldConnection;
    std::shared_ptr<ClientSharedState> state = State;

    {
        std::lock_guard<std::mutex> guard(state->Lock);
        if (index >= state->Slots.size()) {
            return false;
        }

        auto& slot = state->Slots[index];
        if (slot.Connected && slot.Connection && slot.Connection->IsValid()) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (slot.ReconnectNeeded &&
            slot.NextReconnectAt != std::chrono::steady_clock::time_point{} &&
            now < slot.NextReconnectAt) {
            return false;
        }
        oldConnection = std::move(slot.Connection);
        slot.Context = nullptr;
        slot.Connected = false;
        slot.ReconnectNeeded = false;
        slot.NextReconnectAt = {};
        TqClientDebugLog("slot-reset", index, oldConnection.get());
    }

    if (oldConnection) {
        TqClientDebugLog("old-shutdown", index, oldConnection.get());
        oldConnection->Shutdown(0, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
        std::lock_guard<std::mutex> guard(state->Lock);
        state->OrphanedConnections.push_back(std::move(oldConnection));
    }

    MsQuicConnection* connectionToStart = nullptr;
    ClientConnContext* newContext = nullptr;

    if (TqRuntimeTuningEnabled(Config)) {
        TqApplyRuntimeObservations(Config);
        if (!InitConfiguration(Config, *Registration, false, Configuration
#if defined(_WIN32) && !defined(TCPQUIC_WINDOWS_TLS_QUICTLS)
                , Credentials
#endif
                )) {
            std::fprintf(stderr, "Configuration refresh failed\n");
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> guard(state->Lock);
        if (index >= state->Slots.size()) {
            return false;
        }

        auto& slot = state->Slots[index];
        newContext = new (std::nothrow) ClientConnContext{state, index};
        if (newContext == nullptr) {
            return false;
        }

        slot.Connection = std::make_unique<MsQuicConnection>(
            *Registration,
            CleanUpManual,
            QuicClientSession::ConnectionCallback,
            newContext);
        if (!slot.Connection || !slot.Connection->IsValid()) {
            std::fprintf(stderr, "ConnectionOpen failed, 0x%x\n",
                slot.Connection ? slot.Connection->GetInitStatus() : QUIC_STATUS_OUT_OF_MEMORY);
            delete newContext;
            slot.Connection.reset();
            slot.ReconnectNeeded = true;
            slot.NextReconnectAt = std::chrono::steady_clock::now() + state->ReconnectInterval;
            return false;
        }

        slot.Context = newContext;
        connectionToStart = slot.Connection.get();
        if (Config.QuicDisable1RttEncryption &&
            !TqSetDisable1RttEncryption(connectionToStart, "client")) {
            slot.Context = nullptr;
            delete newContext;
            slot.Connection.reset();
            slot.ReconnectNeeded = true;
            slot.NextReconnectAt = std::chrono::steady_clock::now() + state->ReconnectInterval;
            return false;
        }
        TqClientDebugLog("connection-opened", index, connectionToStart,
            slot.Connection->GetInitStatus());
    }

    TqClientDebugLog("connection-start-before", index, connectionToStart);
    const QUIC_STATUS startStatus =
        connectionToStart->Start(*Configuration, PeerHost.c_str(), PeerPort);
    TqClientDebugLog("connection-start-after", index, connectionToStart, startStatus);

    if (TqTraceEnabled()) {
        const std::string peer = PeerHost + ":" + std::to_string(PeerPort);
        TqTraceQuicConnecting("client", static_cast<uint32_t>(index + 1), peer.c_str());
    }

    {
        std::lock_guard<std::mutex> guard(state->Lock);
        if (index >= state->Slots.size()) {
            return QUIC_SUCCEEDED(startStatus);
        }

        auto& slot = state->Slots[index];
        if (slot.Connection.get() != connectionToStart || slot.Context != newContext) {
            return QUIC_SUCCEEDED(startStatus);
        }

        if (QUIC_FAILED(startStatus)) {
            std::fprintf(stderr, "ConnectionStart failed, 0x%x\n", startStatus);
            slot.Context = nullptr;
            delete newContext;
            slot.Connection.reset();
            slot.ReconnectNeeded = true;
            slot.NextReconnectAt = std::chrono::steady_clock::now() + state->ReconnectInterval;
            return false;
        }
    }

    return true;
}

QuicClientSession::ConnectionStateNotification QuicClientSession::OnSlotConnected(
    const std::shared_ptr<ClientSharedState>& state,
    size_t index,
    MsQuicConnection* connection) {
    ConnectionStateNotification notification;
    std::lock_guard<std::mutex> guard(state->Lock);
    if (index >= state->Slots.size()) {
        return notification;
    }
    auto& slot = state->Slots[index];
    const bool wasConnected = slot.Connected && slot.Connection && slot.Connection->IsValid();
    if (slot.Connection.get() == connection) {
        slot.Connected = true;
        slot.ReconnectNeeded = false;
        slot.NextReconnectAt = {};
    }
    const bool isConnected = slot.Connected && slot.Connection && slot.Connection->IsValid();
    if (wasConnected != isConnected) {
        notification.Handler = state->ConnectionStateChanged;
        notification.ConnectedCount = ConnectedCountLocked(*state);
    }
    return notification;
}

QuicClientSession::ConnectionStateNotification QuicClientSession::OnSlotDisconnected(
    const std::shared_ptr<ClientSharedState>& state,
    size_t index,
    MsQuicConnection* connection) {
    ConnectionStateNotification notification;
    std::lock_guard<std::mutex> guard(state->Lock);
    if (index >= state->Slots.size()) {
        return notification;
    }
    auto& slot = state->Slots[index];
    const bool wasConnected = slot.Connected && slot.Connection && slot.Connection->IsValid();
    if (slot.Connection.get() == connection) {
        slot.Connected = false;
        slot.ReconnectNeeded = !state->Stopping;
        slot.NextReconnectAt = state->Stopping
            ? std::chrono::steady_clock::time_point{}
            : std::chrono::steady_clock::now() + state->ReconnectInterval;
    }
    const bool isConnected = slot.Connected && slot.Connection && slot.Connection->IsValid();
    if (wasConnected != isConnected) {
        notification.Handler = state->ConnectionStateChanged;
        notification.ConnectedCount = ConnectedCountLocked(*state);
    }
    return notification;
}

QUIC_STATUS QUIC_API QuicClientSession::ConnectionCallback(
    MsQuicConnection* connection,
    void* context,
    QUIC_CONNECTION_EVENT* event) noexcept {
    auto* slotContext = static_cast<ClientConnContext*>(context);
    if (slotContext == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }

    std::shared_ptr<QuicClientSession::ClientSharedState> state = slotContext->State;
    const size_t slotIndex = slotContext->SlotIndex;

    switch (event->Type) {
    case QUIC_CONNECTION_EVENT_NETWORK_STATISTICS:
        TqRelayUpdateQuicReadAheadFromNetworkStats(
            event->NETWORK_STATISTICS.Bandwidth,
            event->NETWORK_STATISTICS.SmoothedRTT);
        if (TqTraceEnabled()) {
            TqTraceQuicNetworkStats(
                connection,
                TqTraceNetworkStats{
                    event->NETWORK_STATISTICS.BytesInFlight,
                    event->NETWORK_STATISTICS.PostedBytes,
                    event->NETWORK_STATISTICS.IdealBytes,
                    event->NETWORK_STATISTICS.SmoothedRTT,
                    event->NETWORK_STATISTICS.CongestionWindow,
                    event->NETWORK_STATISTICS.Bandwidth});
        }
        break;
    case QUIC_CONNECTION_EVENT_CONNECTED:
        TqClientDebugLog("event-connected", slotIndex, connection);
        TqSampleConnectionRtt(connection);
        {
            char line[256];
            std::snprintf(
                line,
                sizeof(line),
                "tcpquic-proxy: QUIC client connected peer=%s slot=%zu",
                TqFormatConnectionAddr(connection, QUIC_PARAM_CONN_REMOTE_ADDRESS).c_str(),
                slotIndex + 1);
            std::fprintf(stderr, "%s\n", line);
            TqTraceLogLine(line);
        }
        if (state) {
            auto notification = QuicClientSession::OnSlotConnected(state, slotIndex, connection);
            state->StateChanged.notify_all();
            QuicClientSession::NotifyConnectionStateChanged(std::move(notification));
        }
        if (TqTraceEnabled()) {
            TqRegisterClientTraceConnection(connection, slotIndex);
            TqTraceQuicConnected(
                connection,
                static_cast<uint32_t>(slotIndex + 1),
                "client",
                static_cast<uint32_t>(slotIndex + 1));
        }
        break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        if (state) {
            QuicClientSession::StreamHandler handler;
            {
                std::lock_guard<std::mutex> guard(state->Lock);
                handler = state->PeerStreamHandler;
            }
            if (handler) {
                handler(connection, event->PEER_STREAM_STARTED.Stream);
            } else {
                MsQuic->StreamClose(event->PEER_STREAM_STARTED.Stream);
            }
        } else {
            MsQuic->StreamClose(event->PEER_STREAM_STARTED.Stream);
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        TqClientDebugLog("event-shutdown-transport", slotIndex, connection,
            event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
        (void)TqAbortConnectionTunnels(connection);
        TqSampleConnectionRtt(connection);
        if (TqTraceEnabled()) {
            TqTraceQuicShutdownTransport(
                connection,
                static_cast<uint32_t>(slotIndex + 1),
                "client",
                static_cast<uint32_t>(event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status),
                event->SHUTDOWN_INITIATED_BY_TRANSPORT.ErrorCode);
        }
        if (state) {
            auto notification = QuicClientSession::OnSlotDisconnected(state, slotIndex, connection);
            state->StateChanged.notify_all();
            QuicClientSession::NotifyConnectionStateChanged(std::move(notification));
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        TqClientDebugLog("event-shutdown-peer", slotIndex, connection);
        (void)TqAbortConnectionTunnels(connection);
        TqSampleConnectionRtt(connection);
        if (TqTraceEnabled()) {
            TqTraceQuicShutdownPeer(
                connection,
                static_cast<uint32_t>(slotIndex + 1),
                "client",
                event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
        }
        if (state) {
            auto notification = QuicClientSession::OnSlotDisconnected(state, slotIndex, connection);
            state->StateChanged.notify_all();
            QuicClientSession::NotifyConnectionStateChanged(std::move(notification));
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        TqClientDebugLog("event-shutdown-complete", slotIndex, connection);
        (void)TqAbortConnectionTunnels(connection);
        {
            char line[256];
            std::snprintf(
                line,
                sizeof(line),
                "tcpquic-proxy: QUIC client disconnected peer=%s slot=%zu",
                TqFormatConnectionAddr(connection, QUIC_PARAM_CONN_REMOTE_ADDRESS).c_str(),
                slotIndex + 1);
            std::fprintf(stderr, "%s\n", line);
            TqTraceLogLine(line);
        }
        if (TqTraceEnabled()) {
            TqUnregisterClientTraceConnection(connection);
            TqTraceQuicDisconnected(
                connection,
                static_cast<uint32_t>(slotIndex + 1),
                "client");
        }
        if (state) {
            auto notification = QuicClientSession::OnSlotDisconnected(state, slotIndex, connection);
            {
                std::lock_guard<std::mutex> guard(state->Lock);
                if (slotIndex < state->Slots.size() &&
                    state->Slots[slotIndex].Context == slotContext) {
                    state->Slots[slotIndex].Context = nullptr;
                }
            }
            state->StateChanged.notify_all();
            QuicClientSession::NotifyConnectionStateChanged(std::move(notification));
        }
        connection->Close();
        if (state) {
            QuicClientSession::DropOrphanedConnection(state, connection);
        }
        delete slotContext;
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

bool QuicServerSession::Start(const TqConfig& cfg) {
    Stop();

    TqEndpoint endpoint;
    if (!ParseEndpoint(cfg.QuicListen, endpoint)) {
        std::fprintf(stderr, "Invalid --quic-listen endpoint: %s\n", cfg.QuicListen.c_str());
        return false;
    }

    Config = cfg;
    if (!InitApiAndRegistration(Api, Registration, TqToMsQuicProfile(cfg.QuicProfile)) ||
        !InitConfiguration(Config, *Registration, true, Configuration
#if defined(_WIN32) && !defined(TCPQUIC_WINDOWS_TLS_QUICTLS)
            , Credentials
#endif
            )) {
        Stop();
        return false;
    }

    QUIC_ADDR address{};
    if (!ConvertListenAddress(endpoint, address)) {
        std::fprintf(stderr, "Invalid --quic-listen address: %s\n", cfg.QuicListen.c_str());
        Stop();
        return false;
    }

    Listener = std::make_unique<MsQuicListener>(
        *Registration,
        CleanUpManual,
        QuicServerSession::ListenerCallback,
        this);
    if (!Listener || !Listener->IsValid()) {
        std::fprintf(stderr, "ListenerOpen failed, 0x%x\n",
            Listener ? Listener->GetInitStatus() : QUIC_STATUS_OUT_OF_MEMORY);
        Stop();
        return false;
    }

    MsQuicAlpn alpn(TqAlpn);
    const QUIC_STATUS status = Listener->Start(alpn, &address);
    if (QUIC_FAILED(status)) {
        std::fprintf(stderr, "ListenerStart failed, 0x%x\n", status);
        Stop();
        return false;
    }

    std::lock_guard<std::mutex> guard(Lock);
    Started = true;
    Stopping = false;
    return true;
}

void QuicServerSession::Stop() {
    std::shared_ptr<MsQuicApi> apiLocal;
    std::unique_ptr<MsQuicRegistration> registrationLocal;
    std::unique_ptr<MsQuicConfiguration> configurationLocal;
    std::unique_ptr<MsQuicListener> listenerLocal;

    {
        std::lock_guard<std::mutex> guard(Lock);
        Started = false;
        Stopping = true;
        if (Listener && Listener->Handle) {
            MsQuic->ListenerStop(Listener->Handle);
        }
        apiLocal = std::move(Api);
        registrationLocal = std::move(Registration);
        configurationLocal = std::move(Configuration);
        listenerLocal = std::move(Listener);
    }

    StateChanged.notify_all();
}

void QuicServerSession::Run() {
    std::unique_lock<std::mutex> guard(Lock);
    StateChanged.wait(guard, [this] { return !Started || Stopping; });
    guard.unlock();
    Stop();
}

void QuicServerSession::SetPeerStreamHandler(StreamHandler h) {
    std::lock_guard<std::mutex> guard(Lock);
    PeerStreamHandler = std::move(h);
}

void QuicServerSession::SetConnectionHandler(ConnectionHandler h) {
    std::lock_guard<std::mutex> guard(Lock);
    PeerConnectionHandler = std::move(h);
}

QUIC_STATUS QUIC_API QuicServerSession::ListenerCallback(
    MsQuicListener*,
    void* context,
    QUIC_LISTENER_EVENT* event) noexcept {
    auto* session = static_cast<QuicServerSession*>(context);
    if (session == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) {
        return QUIC_STATUS_SUCCESS;
    }

    TqTraceQuicIncoming();

    auto* connection = new (std::nothrow) MsQuicConnection(
        event->NEW_CONNECTION.Connection,
        CleanUpAutoDelete,
        QuicServerSession::ConnectionCallback,
        session);
    if (connection == nullptr) {
        return QUIC_STATUS_OUT_OF_MEMORY;
    }

    if (session->Config.QuicDisable1RttEncryption &&
        !TqSetDisable1RttEncryption(connection, "server")) {
        connection->Handle = nullptr;
        delete connection;
        return QUIC_STATUS_INVALID_STATE;
    }

    if (QUIC_FAILED(connection->SetConfiguration(*session->Configuration))) {
        connection->Handle = nullptr;
        delete connection;
        return QUIC_STATUS_INVALID_STATE;
    }

    QuicServerSession::ConnectionHandler handler;
    {
        std::lock_guard<std::mutex> guard(session->Lock);
        handler = session->PeerConnectionHandler;
    }
    if (handler) {
        handler(connection);
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API QuicServerSession::ConnectionCallback(
    MsQuicConnection* connection,
    void* context,
    QUIC_CONNECTION_EVENT* event) noexcept {
    auto* session = static_cast<QuicServerSession*>(context);
    if (session == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }

    switch (event->Type) {
    case QUIC_CONNECTION_EVENT_NETWORK_STATISTICS:
        TqRelayUpdateQuicReadAheadFromNetworkStats(
            event->NETWORK_STATISTICS.Bandwidth,
            event->NETWORK_STATISTICS.SmoothedRTT);
        if (TqTraceEnabled()) {
            TqTraceQuicNetworkStats(
                connection,
                TqTraceNetworkStats{
                    event->NETWORK_STATISTICS.BytesInFlight,
                    event->NETWORK_STATISTICS.PostedBytes,
                    event->NETWORK_STATISTICS.IdealBytes,
                    event->NETWORK_STATISTICS.SmoothedRTT,
                    event->NETWORK_STATISTICS.CongestionWindow,
                    event->NETWORK_STATISTICS.Bandwidth});
        }
        break;
    case QUIC_CONNECTION_EVENT_CONNECTED:
        TqSampleConnectionRtt(connection);
        connection->SendResumptionTicket(QUIC_SEND_RESUMPTION_FLAG_FINAL);
        {
            const uint32_t connId = TqRegisterServerConnection(connection);
            char line[256];
            std::snprintf(
                line,
                sizeof(line),
                "tcpquic-proxy: QUIC server connection accepted from=%s conn=%u",
                TqFormatConnectionAddr(connection, QUIC_PARAM_CONN_REMOTE_ADDRESS).c_str(),
                connId);
            std::fprintf(stderr, "%s\n", line);
            TqTraceLogLine(line);
            TqTraceQuicConnected(connection, connId, "server", 0);
        }
        break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        {
            QuicServerSession::StreamHandler handler;
            {
                std::lock_guard<std::mutex> guard(session->Lock);
                handler = session->PeerStreamHandler;
            }
            if (handler) {
                handler(connection, event->PEER_STREAM_STARTED.Stream);
            } else {
                MsQuic->StreamClose(event->PEER_STREAM_STARTED.Stream);
            }
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        (void)TqAbortConnectionTunnels(connection);
        if (TqTraceEnabled()) {
            const uint32_t connId = TqLookupServerConnectionId(connection);
            TqTraceQuicShutdownTransport(
                connection,
                connId,
                "server",
                static_cast<uint32_t>(event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status),
                event->SHUTDOWN_INITIATED_BY_TRANSPORT.ErrorCode);
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        (void)TqAbortConnectionTunnels(connection);
        if (TqTraceEnabled()) {
            const uint32_t connId = TqLookupServerConnectionId(connection);
            TqTraceQuicShutdownPeer(
                connection,
                connId,
                "server",
                event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        (void)TqAbortConnectionTunnels(connection);
        {
            const uint32_t connId = TqLookupServerConnectionId(connection);
            char line[256];
            std::snprintf(
                line,
                sizeof(line),
                "tcpquic-proxy: QUIC server connection disconnected from=%s conn=%u",
                TqFormatConnectionAddr(connection, QUIC_PARAM_CONN_REMOTE_ADDRESS).c_str(),
                connId);
            std::fprintf(stderr, "%s\n", line);
            TqTraceLogLine(line);
        }
        if (TqTraceEnabled()) {
            const uint32_t connId = TqLookupServerConnectionId(connection);
            TqTraceQuicDisconnected(connection, connId, "server");
        }
        TqUnregisterServerConnection(connection);
        break;
    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}
