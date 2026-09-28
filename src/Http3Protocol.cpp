#include "ServerImpl.hpp"

namespace libwt
{
using namespace detail;

bool Server::Impl::Connection::Capsules(Stream &stream, const uint8_t *data, size_t size)
{
    if (size > MaxBufferedBytes - stream.capsules.size())
        return false;
    stream.capsules.insert(stream.capsules.end(), data, data + size);
    size_t consumed = 0;
    while (consumed < stream.capsules.size())
    {
        size_t offset = consumed;
        uint64_t type, length;
        if (!ReadVarInt(stream.capsules.data(), stream.capsules.size(), offset, type) ||
            !ReadVarInt(stream.capsules.data(), stream.capsules.size(), offset, length))
            break;
        if (length > MaxBufferedBytes)
            return false;
        if (length > stream.capsules.size() - offset)
            break;
        consumed = offset + static_cast<size_t>(length);
        if (type == CloseWebTransportSessionCapsule)
        {
            if (length < MinCloseCapsuleBytes || length > MaxCloseCapsuleBytes)
                return false;
            stream.finished = true;
            FinishSession();
            Send(stream, {}, QUIC_SEND_FLAG_FIN);
            stream.capsules.clear();
            return true;
        }
        if (type == DrainWebTransportSessionCapsule && length != 0)
            return false; // DRAIN_WEBTRANSPORT_SESSION.
        // Unknown capsules and draft flow control capsules are ignored.
    }
    stream.capsules.erase(stream.capsules.begin(), stream.capsules.begin() + consumed);
    return true;
}

void Server::Impl::Connection::Process(Stream &stream)
{
    size_t consumed = 0;
    if (stream.kind == UINT64_MAX)
    {
        if (!ReadVarInt(stream.input.data(), stream.input.size(), consumed, stream.kind))
            return;
        if (stream.kind == ControlStream || stream.kind == QpackEncoderStream ||
            stream.kind == QpackDecoderStream)
        {
            const unsigned bit = 1u << stream.kind;
            if (critical_streams & bit)
            {
                Shutdown(H3StreamCreationError);
                return;
            }
            critical_streams |= bit;
        }
        else
        {
            // This datagram-only API does not expose WT streams or server push.
            stream.finished = true;
            owner.api->StreamShutdown(stream.handle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE,
                                      stream.kind == WebTransportUnidirectionalStream
                                          ? WebTransportStreamError
                                          : H3NoError);
            return;
        }
    }
    if (stream.kind == QpackEncoderStream || stream.kind == QpackDecoderStream)
    {
        for (size_t i = consumed; i < stream.input.size(); ++i)
        {
            // With advertised table capacity 0 the only valid encoder
            // instruction is Set Capacity 0; no decoder ACK is expected.
            if (stream.kind == QpackDecoderStream || stream.input[i] != QpackSetCapacityZero)
            {
                Shutdown(stream.kind == QpackEncoderStream ? QpackEncoderStreamError
                                                           : QpackDecoderStreamError);
                return;
            }
        }
        stream.input.clear();
        return;
    }
    while (consumed < stream.input.size() && !stream.finished)
    {
        size_t offset = consumed;
        uint64_t type, length;
        if (!ReadVarInt(stream.input.data(), stream.input.size(), offset, type))
            break;
        if (stream.kind == RequestStream && !stream.headers_seen && type == WebTransportStreamFrame)
        {
            stream.finished = true;
            owner.api->StreamShutdown(stream.handle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT,
                                      WebTransportStreamError);
            return;
        }
        if (!ReadVarInt(stream.input.data(), stream.input.size(), offset, length))
            break;
        if (length > MaxHeadersBytes)
        {
            Shutdown(H3ExcessiveLoad);
            return;
        }
        if (length > stream.input.size() - offset)
            break;
        const auto *payload = stream.input.data() + offset;
        if (stream.kind == ControlStream)
        {
            if (!settings_received)
            {
                if (type != SettingsFrame)
                {
                    Shutdown(H3MissingSettings);
                    return;
                }
                if (!ParseSettings(payload, length, peer_settings))
                {
                    Shutdown(H3SettingsError);
                    return;
                }
                settings_received = true;
                for (auto &entry : streams)
                {
                    if (entry.second->kind != RequestStream)
                        continue;
                    TryOpen(*entry.second);
                    if (!entry.second->pending_connect && !entry.second->finished)
                        Process(*entry.second);
                }
            }
            else if (type == SettingsFrame || type == DataFrame || type == HeadersFrame ||
                     type == PushPromiseFrame || type == ReservedFrame2 || type == ReservedFrame6 ||
                     type == ReservedFrame8 || type == ReservedFrame9)
            {
                Shutdown(H3FrameUnexpected);
                return;
            }
            else if (type == GoawayFrame || type == CancelPushFrame || type == MaxPushIdFrame)
            {
                size_t cursor = 0;
                uint64_t value;
                if (!ReadVarInt(payload, length, cursor, value) || cursor != length)
                {
                    Shutdown(H3FrameError);
                    return;
                }
            }
        }
        else if (type == HeadersFrame)
        {
            if (stream.headers_seen)
            {
                Shutdown(H3FrameUnexpected);
                return;
            }
            stream.headers_seen = true;
            if (!DecodeConnectHeaders(payload, length, stream.id, stream.headers))
            {
                Shutdown(QpackDecompressionFailed);
                return;
            }
            stream.pending_connect = true;
            TryOpen(stream);
        }
        else if (type == DataFrame)
        {
            if (!stream.headers_seen)
            {
                Shutdown(H3FrameUnexpected);
                return;
            }
            if (stream.pending_connect)
                break; // Wait for SETTINGS on another stream.
            if (!stream.session_stream || !Capsules(stream, payload, length))
            {
                Shutdown(H3MessageError);
                return;
            }
        }
        else if (type == SettingsFrame || type == PushPromiseFrame || type == CancelPushFrame ||
                 type == GoawayFrame || type == MaxPushIdFrame || type == ReservedFrame2 ||
                 type == ReservedFrame6 || type == ReservedFrame8 || type == ReservedFrame9)
        {
            Shutdown(H3FrameUnexpected);
            return;
        }
        consumed = offset + static_cast<size_t>(length);
    }
    stream.input.erase(stream.input.begin(), stream.input.begin() + consumed);
}
} // namespace libwt
