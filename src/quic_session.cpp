#include "quic_session.h"
#include "tuning.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif
#include <unordered_map>

extern const MsQuicApi* MsQuic;

namespace {

constexpr char TqAlpn[] = "tcpquic-tunnel/1";
constexpr uint32_t TqKeepAliveMs = 15 * 1000;
constexpr uint64_t TqIdleTimeoutMs = 60 * 1000;
constexpr uint16_t TqPeerBidiStreamCount = 1000;

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

MsQuicSettings MakeSettings(const TqConfig& cfg, bool server) {
    MsQuicSettings settings;
    settings.SetCongestionControlAlgorithm(QUIC_CONGESTION_CONTROL_ALGORITHM_BBR);
    settings.SetKeepAlive(TqKeepAliveMs);
    settings.SetIdleTimeoutMs(TqIdleTimeoutMs);
    settings.SetPeerBidiStreamCount(TqPeerBidiStreamCount);
    settings.SetStreamRecvWindowDefault(cfg.Tuning.StreamRecvWindow);
    settings.SetConnFlowControlWindow(cfg.Tuning.ConnFlowControlWindow);
    settings.InitialWindowPackets = cfg.Tuning.InitialWindowPackets;
    settings.IsSet.InitialWindowPackets = TRUE;
    settings.SetInitialRttMs(cfg.Tuning.InitialRttMs);
    settings.SetSendBufferingEnabled(false);
    settings.SetPacingEnabled(true);
    if (server) {
        settings.SetServerResumptionLevel(QUIC_SERVER_RESUME_ONLY);
    }
    return settings;
}

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
    std::unique_ptr<MsQuicConfiguration>& configuration) {
    MsQuicAlpn alpn(TqAlpn);
    MsQuicSettings settings = MakeSettings(cfg, server);
    const auto flags = server
        ? QUIC_CREDENTIAL_FLAG_REQUIRE_CLIENT_AUTHENTICATION
        : QUIC_CREDENTIAL_FLAG_CLIENT;
    TqCredentialConfig credential(cfg, flags);
    configuration = std::make_unique<MsQuicConfiguration>(
        registration,
        alpn,
        settings,
        MsQuicCredentialConfig(credential.Config));
    if (!configuration || !configuration->IsValid()) {
        std::fprintf(stderr, "ConfigurationOpen/LoadCredential failed, 0x%x\n",
            configuration ? configuration->GetInitStatus() : QUIC_STATUS_OUT_OF_MEMORY);
        return false;
    }
    return true;
}

std::mutex g_serverConnIdLock;
std::unordered_map<HQUIC, uint32_t> g_serverConnIds;
std::atomic<uint32_t> g_nextServerConnId{1};

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

bool QuicClientSession::Start(const TqConfig& cfg) {
    StreamHandler handler;
    {
        std::lock_guard<std::mutex> guard(State->Lock);
        handler = State->PeerStreamHandler;
    }
    Stop();
    State = std::make_shared<ClientSharedState>();
    {
        std::lock_guard<std::mutex> guard(State->Lock);
        State->PeerStreamHandler = std::move(handler);
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
        !InitConfiguration(Config, *Registration, false, Configuration)) {
        Stop();
        return false;
    }

    std::lock_guard<std::mutex> guard(State->Lock);
    State->Started = true;
    State->Stopping = false;
    State->Api = Api;
    State->Slots.clear();
    State->Slots.resize(Config.QuicConnections);
    PickIndex.store(0);
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
    std::shared_ptr<MsQuicApi> apiLocal;
    std::unique_ptr<MsQuicRegistration> registrationLocal;
    std::unique_ptr<MsQuicConfiguration> configurationLocal;
    std::shared_ptr<ClientSharedState> state = State;

    {
        std::lock_guard<std::mutex> guard(state->Lock);
        state->Started = false;
        state->Stopping = true;
        state->PeerStreamHandler = nullptr;
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

uint32_t QuicClientSession::ConnectionCount() const {
    return static_cast<uint32_t>(Config.QuicConnections);
}

void QuicClientSession::EnsureConnected() {
    std::unique_lock<std::mutex> guard(State->Lock);
    while (State->Started && !State->Stopping) {
        bool allConnected = !State->Slots.empty();
        for (size_t i = 0; i < State->Slots.size(); ++i) {
            auto& slot = State->Slots[i];
            if (!slot.Connected) {
                allConnected = false;
                if (slot.ReconnectNeeded || !slot.Connection) {
                    guard.unlock();
                    StartSlotLocked(i);
                    guard.lock();
                }
            }
        }
        if (allConnected) {
            return;
        }
        State->StateChanged.wait_for(guard, std::chrono::seconds(1));
    }
}

void QuicClientSession::SetPeerStreamHandler(StreamHandler h) {
    std::lock_guard<std::mutex> guard(State->Lock);
    State->PeerStreamHandler = std::move(h);
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
        oldConnection = std::move(slot.Connection);
        slot.Context = nullptr;
        slot.Connected = false;
        slot.ReconnectNeeded = false;
    }

    if (oldConnection) {
        oldConnection->Shutdown(0, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
        std::lock_guard<std::mutex> guard(state->Lock);
        state->OrphanedConnections.push_back(std::move(oldConnection));
    }

    MsQuicConnection* connectionToStart = nullptr;
    ClientConnContext* newContext = nullptr;

    if (TqRuntimeTuningEnabled(Config)) {
        TqApplyRuntimeObservations(Config);
        if (!InitConfiguration(Config, *Registration, false, Configuration)) {
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
            return false;
        }

        slot.Context = newContext;
        connectionToStart = slot.Connection.get();
    }

    const QUIC_STATUS startStatus =
        connectionToStart->Start(*Configuration, PeerHost.c_str(), PeerPort);

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
            return false;
        }
    }

    return true;
}

void QuicClientSession::OnSlotConnected(const std::shared_ptr<ClientSharedState>& state, size_t index, MsQuicConnection* connection) {
    std::lock_guard<std::mutex> guard(state->Lock);
    if (index >= state->Slots.size()) {
        return;
    }
    auto& slot = state->Slots[index];
    if (slot.Connection.get() == connection) {
        slot.Connected = true;
        slot.ReconnectNeeded = false;
    }
}

void QuicClientSession::OnSlotDisconnected(const std::shared_ptr<ClientSharedState>& state, size_t index, MsQuicConnection* connection) {
    std::lock_guard<std::mutex> guard(state->Lock);
    if (index >= state->Slots.size()) {
        return;
    }
    auto& slot = state->Slots[index];
    if (slot.Connection.get() == connection) {
        slot.Connected = false;
        slot.ReconnectNeeded = !state->Stopping;
    }
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
    case QUIC_CONNECTION_EVENT_CONNECTED:
        TqSampleConnectionRtt(connection);
        if (state) {
            QuicClientSession::OnSlotConnected(state, slotIndex, connection);
            state->StateChanged.notify_all();
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
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        TqSampleConnectionRtt(connection);
        if (state) {
            QuicClientSession::OnSlotDisconnected(state, slotIndex, connection);
            state->StateChanged.notify_all();
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        if (state) {
            QuicClientSession::OnSlotDisconnected(state, slotIndex, connection);
            {
                std::lock_guard<std::mutex> guard(state->Lock);
                if (slotIndex < state->Slots.size() &&
                    state->Slots[slotIndex].Context == slotContext) {
                    state->Slots[slotIndex].Context = nullptr;
                }
            }
            state->StateChanged.notify_all();
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
        !InitConfiguration(Config, *Registration, true, Configuration)) {
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
#if defined(_WIN32)
    const bool interactive = _isatty(_fileno(stdin)) != 0;
#else
    const bool interactive = isatty(STDIN_FILENO) != 0;
#endif
    if (interactive) {
        std::fprintf(stderr, "Press Enter to exit.\n");
        (void)getchar();
    } else {
        std::fprintf(stderr, "Running (non-interactive).\n");
        std::unique_lock<std::mutex> guard(Lock);
        StateChanged.wait(guard, [this] { return !Started || Stopping; });
    }
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
    if (session == nullptr || event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) {
        return QUIC_STATUS_SUCCESS;
    }

    auto* connection = new (std::nothrow) MsQuicConnection(
        event->NEW_CONNECTION.Connection,
        CleanUpAutoDelete,
        QuicServerSession::ConnectionCallback,
        session);
    if (connection == nullptr) {
        return QUIC_STATUS_OUT_OF_MEMORY;
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
    case QUIC_CONNECTION_EVENT_CONNECTED:
        TqSampleConnectionRtt(connection);
        connection->SendResumptionTicket(QUIC_SEND_RESUMPTION_FLAG_FINAL);
        TqRegisterServerConnection(connection);
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
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        TqUnregisterServerConnection(connection);
        break;
    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}
