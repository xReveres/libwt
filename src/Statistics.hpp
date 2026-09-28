#pragma once
#include <libwt/Server.hpp>
#include <atomic>

namespace libwt::detail
{
enum class FinalSendOutcome
{
    Acknowledged,
    Lost,
    Canceled
};

struct DatagramCounters
{
    std::atomic<uint64_t> datagrams_received{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> datagrams_sent{0};
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> send_rejected_size{0};
    std::atomic<uint64_t> send_rejected_queue{0};
    std::atomic<uint64_t> send_rejected_state{0};
    std::atomic<uint64_t> send_errors{0};
    std::atomic<uint64_t> datagrams_acknowledged{0};
    std::atomic<uint64_t> datagrams_lost{0};
    std::atomic<uint64_t> datagrams_canceled{0};
    std::atomic<uint64_t> callback_errors{0};

    void RecordReceive(size_t size)
    {
        ++datagrams_received;
        bytes_received += size;
    }

    void RecordSend(DatagramResult result, size_t size)
    {
        switch (result)
        {
        case DatagramResult::Accepted:
            ++datagrams_sent;
            bytes_sent += size;
            break;
        case DatagramResult::Size:
            ++send_rejected_size;
            break;
        case DatagramResult::Queue:
            ++send_rejected_queue;
            break;
        case DatagramResult::State:
            ++send_rejected_state;
            break;
        case DatagramResult::Error:
            ++send_errors;
            break;
        }
    }

    void RecordFinalSend(FinalSendOutcome outcome)
    {
        switch (outcome)
        {
        case FinalSendOutcome::Acknowledged:
            ++datagrams_acknowledged;
            break;
        case FinalSendOutcome::Lost:
            ++datagrams_lost;
            break;
        case FinalSendOutcome::Canceled:
            ++datagrams_canceled;
            break;
        }
    }

    void RecordCallbackError() { ++callback_errors; }

    template <typename Snapshot> void Fill(Snapshot &result) const
    {
        result.datagrams_received = datagrams_received.load(std::memory_order_relaxed);
        result.bytes_received = bytes_received.load(std::memory_order_relaxed);
        result.datagrams_sent = datagrams_sent.load(std::memory_order_relaxed);
        result.bytes_sent = bytes_sent.load(std::memory_order_relaxed);
        result.send_rejected_size = send_rejected_size.load(std::memory_order_relaxed);
        result.send_rejected_queue = send_rejected_queue.load(std::memory_order_relaxed);
        result.send_rejected_state = send_rejected_state.load(std::memory_order_relaxed);
        result.send_errors = send_errors.load(std::memory_order_relaxed);
        result.datagrams_acknowledged = datagrams_acknowledged.load(std::memory_order_relaxed);
        result.datagrams_lost = datagrams_lost.load(std::memory_order_relaxed);
        result.datagrams_canceled = datagrams_canceled.load(std::memory_order_relaxed);
        result.callback_errors = callback_errors.load(std::memory_order_relaxed);
    }
};

struct ServerCounters : DatagramCounters
{
    std::atomic<uint64_t> connections_accepted{0};
    std::atomic<uint64_t> connections_rejected{0};
    std::atomic<uint64_t> connections_connected{0};
    std::atomic<uint64_t> connections_closed{0};
    std::atomic<uint64_t> active_connections{0};
    std::atomic<uint64_t> sessions_accepted{0};
    std::atomic<uint64_t> sessions_rejected{0};
    std::atomic<uint64_t> sessions_closed{0};
    std::atomic<uint64_t> active_sessions{0};
    std::atomic<uint64_t> datagrams_ignored{0};
    std::atomic<uint64_t> protocol_errors{0};
    std::atomic<uint64_t> certificate_reloads{0};
    std::atomic<uint64_t> certificate_reload_errors{0};

    ServerStatistics Snapshot() const
    {
        ServerStatistics result;
        result.connections_accepted = connections_accepted.load(std::memory_order_relaxed);
        result.connections_rejected = connections_rejected.load(std::memory_order_relaxed);
        result.connections_connected = connections_connected.load(std::memory_order_relaxed);
        result.connections_closed = connections_closed.load(std::memory_order_relaxed);
        result.active_connections = active_connections.load(std::memory_order_relaxed);
        result.sessions_accepted = sessions_accepted.load(std::memory_order_relaxed);
        result.sessions_rejected = sessions_rejected.load(std::memory_order_relaxed);
        result.sessions_closed = sessions_closed.load(std::memory_order_relaxed);
        result.active_sessions = active_sessions.load(std::memory_order_relaxed);
        result.datagrams_ignored = datagrams_ignored.load(std::memory_order_relaxed);
        result.protocol_errors = protocol_errors.load(std::memory_order_relaxed);
        result.certificate_reloads = certificate_reloads.load(std::memory_order_relaxed);
        result.certificate_reload_errors =
            certificate_reload_errors.load(std::memory_order_relaxed);
        Fill(result);
        return result;
    }
};

struct SessionCounters : DatagramCounters
{
    SessionStatistics Snapshot() const
    {
        SessionStatistics result;
        Fill(result);
        return result;
    }
};

inline void RecordReceive(ServerCounters &total, SessionCounters &session, size_t size)
{
    total.RecordReceive(size);
    session.RecordReceive(size);
}

inline void RecordFinalSend(ServerCounters &total, SessionCounters &session,
                            FinalSendOutcome outcome)
{
    total.RecordFinalSend(outcome);
    session.RecordFinalSend(outcome);
}

inline void RecordCallbackError(ServerCounters &total, SessionCounters &session)
{
    total.RecordCallbackError();
    session.RecordCallbackError();
}
} // namespace libwt::detail
