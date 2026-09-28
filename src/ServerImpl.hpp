#pragma once
#include <libwt/Server.hpp>
#include "Http3Utils.hpp"
#include "Statistics.hpp"
#include <msquic.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace libwt
{
using namespace detail;

struct Session::Impl
{
    std::shared_ptr<detail::SessionCounters> counters = std::make_shared<detail::SessionCounters>();
    std::shared_ptr<detail::ServerCounters> total;
    uint64_t id = 0;
    Request request;
    std::mutex handler_mutex;
    Session::DatagramHandler datagram_handler;
    bool closed = false;
    std::function<DatagramResult(const void *, size_t)> send;
    std::function<void()> close;
    std::function<TransportStatistics()> transport;
};

struct Server::Impl
{
    struct Connection;
    struct Stream
    {
        Connection *connection = nullptr;
        HQUIC handle = nullptr;
        uint64_t id = 0;
        uint64_t kind = UINT64_MAX;
        bool local = false;
        bool headers_seen = false;
        bool pending_connect = false;
        bool session_stream = false;
        bool finished = false;
        Bytes input;
        Bytes capsules;
        ConnectHeaders headers;
    };

    struct SendBuffer
    {
        Bytes bytes;
        QUIC_BUFFER buffer;
        std::shared_ptr<detail::SessionCounters> session_counters;
        explicit SendBuffer(Bytes data, std::shared_ptr<detail::SessionCounters> counters = {})
            : bytes(std::move(data)), buffer{static_cast<uint32_t>(bytes.size()), bytes.data()},
              session_counters(std::move(counters))
        {
        }
    };

    struct Connection : std::enable_shared_from_this<Connection>
    {
        Impl &owner;
        HQUIC handle;
        std::mutex send_mutex;
        bool closing = false;
        bool session_active = false;
        bool session_used = false;
        bool settings_received = false;
        unsigned critical_streams = 0;
        PeerSettings peer_settings;
        // MsQuic 2.4 can negotiate this before assigning the application callback.
        // Until a size event arrives, DatagramSend itself enforces the peer/MTU limit.
        std::atomic<uint16_t> max_datagram{UINT16_MAX};
        std::atomic<unsigned> pending_sends{0};
        std::unordered_map<HQUIC, std::shared_ptr<Stream>> streams;
        SessionPtr session;
        bool session_published = false;
        uint64_t session_stream_id = 0;
        Request request;
        Connection(Impl &server, HQUIC quic) : owner(server), handle(quic) {}

        bool ShutdownLocked(uint64_t error);
        void Shutdown(uint64_t error);
        void CloseSession(uint64_t id);
        TransportStatistics GetTransportStatistics(uint64_t id);
        DatagramResult SendDatagram(uint64_t id, const void *data, size_t size,
                                    const std::shared_ptr<detail::SessionCounters> &counters);
        bool Send(Stream &stream, Bytes bytes, QUIC_SEND_FLAGS flags = QUIC_SEND_FLAG_NONE);
        void FinishSession();
        bool StartControl();
        void Reject(Stream &stream, unsigned status);
        void TryOpen(Stream &stream);
        bool Capsules(Stream &stream, const uint8_t *data, size_t size);
        void Process(Stream &stream);
        QUIC_STATUS OnStream(Stream &stream, QUIC_STREAM_EVENT &event);
        static QUIC_STATUS QUIC_API StreamCallback(HQUIC, void *context, QUIC_STREAM_EVENT *event);
        QUIC_STATUS OnEvent(QUIC_CONNECTION_EVENT &event);
        static QUIC_STATUS QUIC_API Callback(HQUIC, void *context, QUIC_CONNECTION_EVENT *event);
    };

    std::shared_ptr<detail::ServerCounters> counters = std::make_shared<detail::ServerCounters>();
    ServerConfig config;
    const QUIC_API_TABLE *api = nullptr;
    HQUIC registration = nullptr;
    HQUIC configuration = nullptr;
    HQUIC listener = nullptr;
    std::mutex configuration_mutex;
    std::mutex connections_mutex;
    std::condition_variable connections_changed;
    std::unordered_map<HQUIC, std::shared_ptr<Connection>> connections;
    std::atomic<uint64_t> next_session{0};
    bool accepting = false;

    explicit Impl(ServerConfig value) : config(std::move(value)) {}

    void Log(LogLevel level, std::string_view message, std::optional<uint64_t> session_id = {},
             std::optional<uint64_t> error_code = {}) noexcept;

    HQUIC CreateConfiguration(const std::string &cert, const std::string &key, std::string &error);
    static QUIC_STATUS QUIC_API ListenerCallback(HQUIC, void *context, QUIC_LISTENER_EVENT *event);
    void Stop();
};
} // namespace libwt
