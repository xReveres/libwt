#include "ServerImpl.hpp"
#include <algorithm>
#include <cstring>

namespace libwt
{
using namespace detail;

Session::Session(std::shared_ptr<Impl> impl) : impl_(std::move(impl))
{
}
Session::~Session() = default;
uint64_t Session::Id() const
{
    return impl_->id;
}
bool Session::SendDatagram(const void *data, size_t size)
{
    return TrySendDatagram(data, size) == DatagramResult::Accepted;
}
SessionStatistics Session::GetStatistics() const
{
    return impl_->counters->Snapshot();
}
void Session::SetDatagramHandler(DatagramHandler handler)
{
    DatagramHandler previous;
    {
        std::lock_guard<std::mutex> lock(impl_->handler_mutex);
        if (!impl_->closed)
        {
            previous = std::move(impl_->datagram_handler);
            impl_->datagram_handler = std::move(handler);
        }
    }
}
const Request &Session::GetRequest() const
{
    return impl_->request;
}
DatagramResult Session::TrySendDatagram(const void *data, size_t size)
{
    const auto result = impl_->send(data, size);
    impl_->counters->RecordSend(result, size);
    impl_->total->RecordSend(result, size);
    return result;
}
TransportStatistics Session::GetTransportStatistics() const
{
    return impl_->transport();
}
void Session::Close()
{
    impl_->close();
}

bool Server::Impl::Connection::ShutdownLocked(uint64_t error)
{
    if (handle && !closing)
    {
        closing = true;
        if (error != H3NoError)
        {
            ++owner.counters->protocol_errors;
        }
        owner.api->ConnectionShutdown(handle, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, error);
        return error != H3NoError;
    }
    return false;
}

void Server::Impl::Connection::Shutdown(uint64_t error)
{
    bool report = false;
    std::optional<uint64_t> session_id;
    {
        std::lock_guard<std::mutex> lock(send_mutex);
        report = ShutdownLocked(error);
        if (report && session)
            session_id = session->Id();
    }
    if (report)
        owner.Log(LogLevel::Error, "Connection closed with HTTP/3 error code", session_id, error);
}

void Server::Impl::Connection::CloseSession(uint64_t id)
{
    std::lock_guard<std::mutex> lock(send_mutex);
    if (session_active && session && session->Id() == id)
        ShutdownLocked(H3NoError);
}

TransportStatistics Server::Impl::Connection::GetTransportStatistics(uint64_t id)
{
    std::lock_guard<std::mutex> lock(send_mutex);
    TransportStatistics result;
    if (!handle || closing || !session_active || !session || session->Id() != id)
        return result;
    QUIC_STATISTICS_V2 stats{};
    uint32_t size = sizeof(stats);
    if (QUIC_FAILED(owner.api->GetParam(handle, QUIC_PARAM_CONN_STATISTICS_V2, &size, &stats)))
        return result;
    result.available = true;
    result.rtt_us = stats.Rtt;
    result.min_rtt_us = stats.MinRtt;
    result.max_rtt_us = stats.MaxRtt;
    result.congestion_window_bytes = stats.SendCongestionWindow;
    result.path_mtu = stats.SendPathMtu;
    result.packets_sent = stats.SendTotalPackets;
    result.packets_received = stats.RecvTotalPackets;
    result.suspected_lost_packets = stats.SendSuspectedLostPackets;
    result.spurious_lost_packets = stats.SendSpuriousLostPackets;
    result.udp_bytes_sent = stats.SendTotalBytes;
    result.udp_bytes_received = stats.RecvTotalBytes;
    result.receive_dropped_packets = stats.RecvDroppedPackets;
    result.receive_decryption_failures = stats.RecvDecryptionFailures;
    return result;
}

DatagramResult
Server::Impl::Connection::SendDatagram(uint64_t id, const void *data, size_t size,
                                       const std::shared_ptr<detail::SessionCounters> &counters)
{
    if (!data && size)
        return DatagramResult::Error;
    if (size > MaxDatagramPayloadBytes)
        return DatagramResult::Size;
    std::lock_guard<std::mutex> lock(send_mutex);
    if (!handle || closing || !session_active || !session || session->Id() != id)
        return DatagramResult::State;
    if (pending_sends >= MaxPendingSends)
        return DatagramResult::Queue;
    try
    {
        Bytes bytes;
        WriteVarInt(bytes, session_stream_id / 4);
        const auto maximum = max_datagram.load();
        if (!maximum)
            return DatagramResult::State;
        if (bytes.size() + size > maximum)
            return DatagramResult::Size;
        if (size)
        {
            auto begin = static_cast<const uint8_t *>(data);
            bytes.insert(bytes.end(), begin, begin + size);
        }
        auto buffer = std::make_unique<SendBuffer>(std::move(bytes), counters);
        ++pending_sends;
        const auto status =
            owner.api->DatagramSend(handle, &buffer->buffer, 1, QUIC_SEND_FLAG_NONE, buffer.get());
        if (QUIC_FAILED(status))
        {
            --pending_sends;
            // Buffers/count/flags above are valid. MsQuic reports its
            // current datagram size limit as INVALID_PARAMETER, including
            // when DATAGRAM_STATE_CHANGED preceded callback installation.
            if (status == QUIC_STATUS_INVALID_PARAMETER)
                return DatagramResult::Size;
            if (status == QUIC_STATUS_INVALID_STATE || status == QUIC_STATUS_ABORTED)
                return DatagramResult::State;
            if (status == QUIC_STATUS_OUT_OF_MEMORY)
                return DatagramResult::Queue;
            return DatagramResult::Error;
        }
        buffer.release();
        return DatagramResult::Accepted;
    }
    catch (...)
    {
        return DatagramResult::Error;
    }
}

bool Server::Impl::Connection::Send(Stream &stream, Bytes bytes, QUIC_SEND_FLAGS flags)
{
    if (pending_sends >= MaxPendingSends)
    {
        Shutdown(H3ExcessiveLoad);
        return false;
    }
    auto buffer = std::make_unique<SendBuffer>(std::move(bytes));
    ++pending_sends;
    const auto status =
        owner.api->StreamSend(stream.handle, &buffer->buffer, 1, flags, buffer.get());
    if (QUIC_FAILED(status))
    {
        --pending_sends;
        Shutdown(H3GeneralError);
        return false;
    }
    buffer.release();
    return true;
}

void Server::Impl::Connection::FinishSession()
{
    SessionPtr ended;
    bool published = false;
    {
        std::lock_guard<std::mutex> lock(send_mutex);
        session_active = false;
        ended = std::move(session);
        published = std::exchange(session_published, false);
    }
    if (ended)
    {
        Session::DatagramHandler previous;
        {
            std::lock_guard<std::mutex> lock(ended->impl_->handler_mutex);
            ended->impl_->closed = true;
            previous = std::move(ended->impl_->datagram_handler);
        }
        ++owner.counters->sessions_closed;
        --owner.counters->active_sessions;
    }
    if (ended && published && owner.config.on_close)
    {
        try
        {
            owner.config.on_close(ended);
        }
        catch (...)
        {
            detail::RecordCallbackError(*owner.counters, *ended->impl_->counters);
            owner.Log(LogLevel::Error, "on_close callback threw", ended->Id());
        }
    }
}

bool Server::Impl::Connection::StartControl()
{
    auto stream = std::make_shared<Stream>();
    stream->connection = this;
    stream->local = true;
    stream->kind = ControlStream;
    if (QUIC_FAILED(owner.api->StreamOpen(handle, QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
                                          StreamCallback, stream.get(), &stream->handle)))
        return false;
    streams.emplace(stream->handle, stream);
    if (QUIC_FAILED(owner.api->StreamStart(stream->handle, QUIC_STREAM_START_FLAG_IMMEDIATE)))
        return false;
    return Send(*stream, Settings());
}

void Server::Impl::Connection::Reject(Stream &stream, unsigned status)
{
    ++owner.counters->sessions_rejected;
    owner.Log(LogLevel::Warning, "CONNECT rejected with HTTP status", {}, status);
    stream.finished = true;
    Send(stream, Response(status), QUIC_SEND_FLAG_FIN);
    owner.api->StreamShutdown(stream.handle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE,
                              H3RequestRejected);
}

void Server::Impl::Connection::TryOpen(Stream &stream)
{
    if (!stream.pending_connect || !settings_received || stream.finished)
        return;
    stream.pending_connect = false;
    if (!peer_settings.datagrams || !peer_settings.webtransport)
    {
        Reject(stream, 400);
        return;
    }
    if (session_used)
    {
        ++owner.counters->sessions_rejected;
        owner.Log(LogLevel::Warning,
                  "CONNECT rejected because the connection already has a session");
        stream.finished = true;
        owner.api->StreamShutdown(stream.handle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT,
                                  H3RequestRejected);
        return;
    }
    Request incoming = request;
    incoming.authority = stream.headers.authority;
    incoming.path = stream.headers.path;
    incoming.origin = stream.headers.origin;
    bool accepted = false;
    try
    {
        accepted = owner.config.on_request && owner.config.on_request(incoming);
    }
    catch (...)
    {
        ++owner.counters->callback_errors;
        owner.Log(LogLevel::Error, "on_request callback threw");
    }
    if (!accepted)
    {
        Reject(stream, 403);
        return;
    }
    auto state = std::make_shared<Session::Impl>();
    state->id = ++owner.next_session;
    state->request = std::move(incoming);
    state->total = owner.counters;
    std::weak_ptr<Connection> weak = shared_from_this();
    const auto id = state->id;
    const auto session_counters = state->counters;
    state->send = [weak, id, session_counters](const void *data, size_t size)
    {
        auto connection = weak.lock();
        return connection ? connection->SendDatagram(id, data, size, session_counters)
                          : DatagramResult::State;
    };
    state->transport = [weak, id]
    {
        auto connection = weak.lock();
        return connection ? connection->GetTransportStatistics(id) : TransportStatistics{};
    };
    state->close = [weak, id]
    {
        if (auto connection = weak.lock())
            connection->CloseSession(id);
    };
    auto opened = SessionPtr(new Session(std::move(state)));
    bool can_open = false;
    {
        std::lock_guard<std::mutex> lock(send_mutex);
        can_open = handle && !closing;
        if (can_open)
        {
            session = opened;
            session_stream_id = stream.id;
            session_active = true;
        }
    }
    if (!can_open)
    {
        Reject(stream, 403);
        return;
    }
    ++owner.counters->sessions_accepted;
    ++owner.counters->active_sessions;
    session_used = true;
    stream.session_stream = true;
    if (!Send(stream, Response(200)))
        FinishSession();
    else
    {
        {
            std::lock_guard<std::mutex> lock(send_mutex);
            session_published = true;
        }
        if (owner.config.on_session)
        {
            try
            {
                owner.config.on_session(opened);
            }
            catch (...)
            {
                detail::RecordCallbackError(*owner.counters, *session_counters);
                owner.Log(LogLevel::Error, "on_session callback threw", opened->Id());
                Shutdown(H3GeneralError);
            }
        }
    }
}

QUIC_STATUS Server::Impl::Connection::OnStream(Stream &stream, QUIC_STREAM_EVENT &event)
{
    switch (event.Type)
    {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        if (QUIC_FAILED(event.START_COMPLETE.Status))
            Shutdown(H3StreamCreationError);
        break;
    case QUIC_STREAM_EVENT_RECEIVE:
        if (stream.finished)
            break;
        for (uint32_t i = 0; i < event.RECEIVE.BufferCount; ++i)
        {
            const auto &buffer = event.RECEIVE.Buffers[i];
            if (buffer.Length > MaxBufferedBytes - stream.input.size())
            {
                Shutdown(H3ExcessiveLoad);
                break;
            }
            stream.input.insert(stream.input.end(), buffer.Buffer, buffer.Buffer + buffer.Length);
            Process(stream);
        }
        break;
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        delete static_cast<SendBuffer *>(event.SEND_COMPLETE.ClientContext);
        --pending_sends;
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        if (stream.kind == ControlStream || stream.kind == QpackEncoderStream ||
            stream.kind == QpackDecoderStream)
            Shutdown(H3ClosedCriticalStream);
        else
        {
            if (stream.session_stream)
                FinishSession();
            if (!stream.input.empty() || !stream.capsules.empty())
                Shutdown(H3FrameError);
            owner.api->StreamShutdown(stream.handle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, H3NoError);
        }
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
    {
        if (stream.session_stream)
            FinishSession();
        HQUIC quic = stream.handle;
        stream.handle = nullptr;
        owner.api->StreamClose(quic);
        streams.erase(quic);
        break;
    }
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API Server::Impl::Connection::StreamCallback(HQUIC, void *context,
                                                              QUIC_STREAM_EVENT *event)
{
    auto *stream = static_cast<Stream *>(context);
    auto connection = stream->connection->shared_from_this();
    const auto keep = connection->streams.at(stream->handle);
    try
    {
        return connection->OnStream(*stream, *event);
    }
    catch (...)
    {
        connection->Shutdown(H3GeneralError);
        return QUIC_STATUS_SUCCESS;
    }
}

QUIC_STATUS Server::Impl::Connection::OnEvent(QUIC_CONNECTION_EVENT &event)
{
    switch (event.Type)
    {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        ++owner.counters->connections_connected;
        if (!StartControl())
            Shutdown(H3GeneralError);
        break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
    {
        auto stream = std::make_shared<Stream>();
        stream->connection = this;
        stream->handle = event.PEER_STREAM_STARTED.Stream;
        if (!(event.PEER_STREAM_STARTED.Flags & QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL))
            stream->kind = RequestStream;
        uint32_t size = sizeof(stream->id);
        if (QUIC_FAILED(
                owner.api->GetParam(stream->handle, QUIC_PARAM_STREAM_ID, &size, &stream->id)))
        {
            owner.api->StreamClose(stream->handle);
            Shutdown(H3GeneralError);
            break;
        }
        streams.emplace(stream->handle, stream);
        owner.api->SetCallbackHandler(stream->handle, reinterpret_cast<void *>(StreamCallback),
                                      stream.get());
        if (streams.size() > MaxStreamsPerConnection)
            Shutdown(H3ExcessiveLoad);
        break;
    }
    case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:
        max_datagram = event.DATAGRAM_STATE_CHANGED.SendEnabled
                           ? event.DATAGRAM_STATE_CHANGED.MaxSendLength
                           : 0;
        break;
    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
    {
        const auto &buffer = *event.DATAGRAM_RECEIVED.Buffer;
        size_t offset = 0;
        uint64_t quarter;
        if (!ReadVarInt(buffer.Buffer, buffer.Length, offset, quarter))
        {
            ++owner.counters->datagrams_ignored;
            break;
        }
        SessionPtr current;
        {
            std::lock_guard<std::mutex> lock(send_mutex);
            if (session_active && quarter == session_stream_id / 4)
                current = session;
        }
        if (current)
        {
            detail::RecordReceive(*owner.counters, *current->impl_->counters,
                                  buffer.Length - offset);
            try
            {
                Session::DatagramHandler handler;
                {
                    std::lock_guard<std::mutex> lock(current->impl_->handler_mutex);
                    if (!current->impl_->closed)
                        handler = current->impl_->datagram_handler;
                }
                if (handler)
                    handler(*current, buffer.Buffer + offset, buffer.Length - offset);
            }
            catch (...)
            {
                detail::RecordCallbackError(*owner.counters, *current->impl_->counters);
                owner.Log(LogLevel::Error, "datagram handler threw", current->Id());
                Shutdown(H3GeneralError);
            }
        }
        else
        {
            ++owner.counters->datagrams_ignored;
        }
        break;
    }
    case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
        if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(event.DATAGRAM_SEND_STATE_CHANGED.State))
        {
            auto *buffer =
                static_cast<SendBuffer *>(event.DATAGRAM_SEND_STATE_CHANGED.ClientContext);
            switch (event.DATAGRAM_SEND_STATE_CHANGED.State)
            {
            case QUIC_DATAGRAM_SEND_ACKNOWLEDGED:
            case QUIC_DATAGRAM_SEND_ACKNOWLEDGED_SPURIOUS:
                detail::RecordFinalSend(*owner.counters, *buffer->session_counters,
                                        FinalSendOutcome::Acknowledged);
                break;
            case QUIC_DATAGRAM_SEND_LOST_DISCARDED:
                detail::RecordFinalSend(*owner.counters, *buffer->session_counters,
                                        FinalSendOutcome::Lost);
                break;
            case QUIC_DATAGRAM_SEND_CANCELED:
                detail::RecordFinalSend(*owner.counters, *buffer->session_counters,
                                        FinalSendOutcome::Canceled);
                break;
            default:
                break;
            }
            delete buffer;
            --pending_sends;
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
    {
        std::lock_guard<std::mutex> lock(send_mutex);
        closing = true;
    }
        FinishSession();
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
    {
        FinishSession();
        for (auto &entry : streams)
            if (entry.second->handle)
                owner.api->StreamClose(entry.second->handle);
        streams.clear();
        HQUIC old;
        {
            std::lock_guard<std::mutex> lock(send_mutex);
            old = handle;
            handle = nullptr;
            closing = true;
        }
        owner.api->ConnectionClose(old);
        std::lock_guard<std::mutex> lock(owner.connections_mutex);
        owner.connections.erase(old);
        ++owner.counters->connections_closed;
        --owner.counters->active_connections;
        owner.connections_changed.notify_all();
        break;
    }
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API Server::Impl::Connection::Callback(HQUIC, void *context,
                                                        QUIC_CONNECTION_EVENT *event)
{
    auto connection = static_cast<Connection *>(context)->shared_from_this();
    try
    {
        return connection->OnEvent(*event);
    }
    catch (...)
    {
        connection->Shutdown(H3GeneralError);
        return QUIC_STATUS_SUCCESS;
    }
}
} // namespace libwt
