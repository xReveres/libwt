#include "ServerImpl.hpp"
#include <arpa/inet.h>

namespace libwt
{
using namespace detail;

void Server::Impl::Log(LogLevel level, std::string_view message, std::optional<uint64_t> session_id,
                       std::optional<uint64_t> error_code) noexcept
{
    if (!config.on_log)
        return;
    try
    {
        LogRecord record{level, std::string(message), session_id};
        if (error_code)
            record.message += ": " + std::to_string(*error_code);
        config.on_log(record);
    }
    catch (...)
    {
        ++counters->callback_errors;
    }
}

HQUIC Server::Impl::CreateConfiguration(const std::string &cert, const std::string &key,
                                        std::string &error)
{
    QUIC_SETTINGS settings{};
    settings.IdleTimeoutMs = config.idle_timeout_ms;
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.HandshakeIdleTimeoutMs = config.handshake_timeout_ms;
    settings.IsSet.HandshakeIdleTimeoutMs = TRUE;
    settings.PeerBidiStreamCount = QuicStreamLimit;
    settings.IsSet.PeerBidiStreamCount = TRUE;
    settings.PeerUnidiStreamCount = QuicStreamLimit;
    settings.IsSet.PeerUnidiStreamCount = TRUE;
    settings.DatagramReceiveEnabled = TRUE;
    settings.IsSet.DatagramReceiveEnabled = TRUE;
    settings.MigrationEnabled = FALSE;
    settings.IsSet.MigrationEnabled = TRUE;
    settings.ServerResumptionLevel = QUIC_SERVER_NO_RESUME;
    settings.IsSet.ServerResumptionLevel = TRUE;
    settings.ConnFlowControlWindow = ConnectionFlowControlWindowBytes;
    settings.IsSet.ConnFlowControlWindow = TRUE;
    settings.StreamRecvWindowDefault = StreamReceiveWindowBytes;
    settings.IsSet.StreamRecvWindowDefault = TRUE;
    QUIC_BUFFER alpn{2, reinterpret_cast<uint8_t *>(const_cast<char *>("h3"))};
    HQUIC candidate = nullptr;
    auto status = api->ConfigurationOpen(registration, &alpn, 1, &settings, sizeof(settings),
                                         nullptr, &candidate);
    if (QUIC_SUCCEEDED(status))
    {
        QUIC_CERTIFICATE_FILE files{key.c_str(), cert.c_str()};
        QUIC_CREDENTIAL_CONFIG credentials{};
        credentials.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
        credentials.CertificateFile = &files;
        status = api->ConfigurationLoadCredential(candidate, &credentials);
    }
    if (QUIC_FAILED(status))
    {
        if (candidate)
            api->ConfigurationClose(candidate);
        error = "MsQuic TLS configuration failed: " + std::to_string(status);
        return nullptr;
    }
    return candidate;
}

QUIC_STATUS QUIC_API Server::Impl::ListenerCallback(HQUIC, void *context,
                                                    QUIC_LISTENER_EVENT *event)
{
    auto &server = *static_cast<Impl *>(context);
    if (event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION)
        return QUIC_STATUS_SUCCESS;
    try
    {
        auto connection = std::make_shared<Connection>(server, event->NEW_CONNECTION.Connection);
        const auto *address = event->NEW_CONNECTION.Info->RemoteAddress;
        char ip[INET6_ADDRSTRLEN]{};
        if (QuicAddrGetFamily(address) == QUIC_ADDRESS_FAMILY_INET)
            inet_ntop(AF_INET, &address->Ipv4.sin_addr, ip, sizeof(ip));
        else
            inet_ntop(AF_INET6, &address->Ipv6.sin6_addr, ip, sizeof(ip));
        connection->request.peer_address = ip;
        connection->request.peer_port = QuicAddrGetPort(address);
        {
            std::lock_guard<std::mutex> lock(server.connections_mutex);
            if (!server.accepting || server.connections.size() >= server.config.max_connections)
            {
                ++server.counters->connections_rejected;
                return QUIC_STATUS_CONNECTION_REFUSED;
            }
            server.connections.emplace(connection->handle, connection);
            ++server.counters->connections_accepted;
            ++server.counters->active_connections;
        }
        server.api->SetCallbackHandler(
            connection->handle, reinterpret_cast<void *>(Connection::Callback), connection.get());
        QUIC_STATUS status;
        {
            std::lock_guard<std::mutex> lock(server.configuration_mutex);
            status =
                server.api->ConnectionSetConfiguration(connection->handle, server.configuration);
        }
        if (QUIC_FAILED(status))
            connection->Shutdown(H3GeneralError);
        return QUIC_STATUS_SUCCESS;
    }
    catch (...)
    {
        return QUIC_STATUS_OUT_OF_MEMORY;
    }
}

void Server::Impl::Stop()
{
    {
        std::lock_guard<std::mutex> lock(connections_mutex);
        accepting = false;
    }
    if (listener)
    {
        api->ListenerStop(listener);
        api->ListenerClose(listener);
        listener = nullptr;
    }
    std::vector<std::shared_ptr<Connection>> active;
    {
        std::lock_guard<std::mutex> lock(connections_mutex);
        for (auto &entry : connections)
            active.push_back(entry.second);
    }
    for (auto &connection : active)
        connection->Shutdown(H3NoError);
    {
        std::unique_lock<std::mutex> lock(connections_mutex);
        connections_changed.wait(lock, [this] { return connections.empty(); });
    }
    active.clear();
    if (configuration)
    {
        api->ConfigurationClose(configuration);
        configuration = nullptr;
    }
    if (registration)
    {
        api->RegistrationClose(registration);
        registration = nullptr;
    }
    if (api)
    {
        MsQuicClose(api);
        api = nullptr;
    }
}
Server::Server(ServerConfig config) : impl_(std::make_unique<Impl>(std::move(config)))
{
}
Server::~Server()
{
    Stop();
}
void Server::Stop()
{
    impl_->Stop();
}
ServerStatistics Server::GetStatistics() const
{
    return impl_->counters->Snapshot();
}

bool Server::Start(std::string &error)
{
    auto &s = *impl_;
    if (s.listener)
    {
        error.clear();
        return true;
    }
    error.clear();
    if (s.config.certificate_file.empty() || s.config.private_key_file.empty() ||
        !s.config.max_connections)
    {
        error = "Certificate, private key and a positive connection limit are required";
        s.Log(LogLevel::Error, error);
        return false;
    }
    QUIC_ADDR address{};
    if (!QuicAddrFromString(s.config.bind_address.c_str(), s.config.port, &address))
    {
        error = "Invalid bind address";
        s.Log(LogLevel::Error, error);
        return false;
    }
    auto status = MsQuicOpen2(&s.api);
    if (QUIC_FAILED(status))
    {
        error = "MsQuicOpen2 failed: " + std::to_string(status);
        s.Log(LogLevel::Error, error);
        return false;
    }
    QUIC_REGISTRATION_CONFIG registration{"libwt", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    status = s.api->RegistrationOpen(&registration, &s.registration);
    if (QUIC_SUCCEEDED(status))
        s.configuration =
            s.CreateConfiguration(s.config.certificate_file, s.config.private_key_file, error);
    if (QUIC_FAILED(status) || !s.configuration)
    {
        if (error.empty())
            error = "MsQuic registration failed: " + std::to_string(status);
        s.Stop();
        s.Log(LogLevel::Error, error);
        return false;
    }
    status = s.api->ListenerOpen(s.registration, Impl::ListenerCallback, &s, &s.listener);
    if (QUIC_SUCCEEDED(status))
    {
        {
            std::lock_guard<std::mutex> lock(s.connections_mutex);
            s.accepting = true;
        }
        QUIC_BUFFER alpn{2, reinterpret_cast<uint8_t *>(const_cast<char *>("h3"))};
        status = s.api->ListenerStart(s.listener, &alpn, 1, &address);
    }
    if (QUIC_FAILED(status))
    {
        error = "MsQuic listener failed: " + std::to_string(status);
        s.Stop();
        s.Log(LogLevel::Error, error);
        return false;
    }
    error.clear();
    return true;
}

bool Server::ReloadCertificate(const std::string &cert, const std::string &key, std::string &error)
{
    auto &s = *impl_;
    if (!s.listener)
    {
        ++s.counters->certificate_reload_errors;
        error = "Server is not running";
        s.Log(LogLevel::Error, error);
        return false;
    }
    HQUIC next = s.CreateConfiguration(cert, key, error);
    if (!next)
    {
        ++s.counters->certificate_reload_errors;
        s.Log(LogLevel::Error, error);
        return false;
    }
    HQUIC previous;
    {
        std::lock_guard<std::mutex> lock(s.configuration_mutex);
        previous = std::exchange(s.configuration, next);
    }
    s.api->ConfigurationClose(previous);
    ++s.counters->certificate_reloads;
    s.config.certificate_file = cert;
    s.config.private_key_file = key;
    error.clear();
    return true;
}
} // namespace libwt
