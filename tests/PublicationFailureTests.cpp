#include "ServerImpl.hpp"
#include <cstdlib>
#include <iostream>

namespace
{
unsigned shutdowns = 0;

QUIC_STATUS QUIC_API FailResponseSend(HQUIC, const QUIC_BUFFER *, uint32_t, QUIC_SEND_FLAGS, void *)
{
    return QUIC_STATUS_INVALID_STATE;
}

void QUIC_API CountShutdown(HQUIC, QUIC_CONNECTION_SHUTDOWN_FLAGS, QUIC_UINT62)
{
    ++shutdowns;
}
} // namespace

namespace libwt
{
struct ServerTestAccess
{
    static bool FailedResponseDoesNotPublish()
    {
        unsigned requests = 0;
        unsigned sessions = 0;
        unsigned closes = 0;
        ServerConfig config;
        config.on_request = [&](const Request &)
        {
            ++requests;
            return true;
        };
        config.on_session = [&](const SessionPtr &) { ++sessions; };
        config.on_close = [&](const SessionPtr &) { ++closes; };
        Server::Impl owner(std::move(config));
        QUIC_API_TABLE api{};
        api.StreamSend = FailResponseSend;
        api.ConnectionShutdown = CountShutdown;
        owner.api = &api;
        auto connection =
            std::make_shared<Server::Impl::Connection>(owner, reinterpret_cast<HQUIC>(0x1));
        connection->settings_received = true;
        connection->peer_settings.datagrams = true;
        connection->peer_settings.webtransport = true;
        Server::Impl::Stream stream;
        stream.handle = reinterpret_cast<HQUIC>(0x2);
        stream.id = 0;
        stream.pending_connect = true;
        stream.headers.path = "/echo";
        connection->TryOpen(stream);
        const auto stats = owner.counters->Snapshot();
        return requests == 1 && sessions == 0 && closes == 0 && shutdowns == 1 &&
               stats.sessions_accepted == 1 && stats.sessions_closed == 1 &&
               stats.active_sessions == 0 && stats.protocol_errors == 1 && !connection->session &&
               !connection->session_published;
    }
};
} // namespace libwt

int main()
{
    if (!libwt::ServerTestAccess::FailedResponseDoesNotPublish())
    {
        std::cerr << "HTTP 200 send failure published a session\n";
        return EXIT_FAILURE;
    }
}
