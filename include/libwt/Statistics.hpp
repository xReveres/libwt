#pragma once
#include <cstdint>

namespace libwt
{
// Monotonic totals for one server lifetime, except active_* gauges.
// Byte totals count application payload only. Snapshots are race-free but are
// not atomic across fields. Accepted sends are not delivery confirmations.
struct ServerStatistics
{
    uint64_t connections_accepted = 0;
    uint64_t connections_rejected = 0;
    uint64_t connections_connected = 0;
    uint64_t connections_closed = 0;
    uint64_t active_connections = 0;
    uint64_t sessions_accepted = 0;
    uint64_t sessions_rejected = 0;
    uint64_t sessions_closed = 0;
    uint64_t active_sessions = 0;
    uint64_t datagrams_received = 0;
    uint64_t bytes_received = 0;
    uint64_t datagrams_ignored = 0;
    uint64_t datagrams_sent = 0;
    uint64_t bytes_sent = 0;
    uint64_t send_rejected_size = 0;
    uint64_t send_rejected_queue = 0;
    uint64_t send_rejected_state = 0;
    uint64_t send_errors = 0;
    uint64_t datagrams_acknowledged = 0;
    uint64_t datagrams_lost = 0;
    uint64_t datagrams_canceled = 0;
    uint64_t callback_errors = 0;
    uint64_t protocol_errors = 0;
    uint64_t certificate_reloads = 0;
    uint64_t certificate_reload_errors = 0;
};
using Statistics = ServerStatistics;

// Monotonic totals for one session, including pending send outcomes after close.
// Byte totals count application payload only. Snapshots are race-free but are
// not atomic across fields. Accepted sends are not delivery confirmations.
struct SessionStatistics
{
    uint64_t datagrams_received = 0;
    uint64_t bytes_received = 0;
    uint64_t datagrams_sent = 0;
    uint64_t bytes_sent = 0;
    uint64_t send_rejected_size = 0;
    uint64_t send_rejected_queue = 0;
    uint64_t send_rejected_state = 0;
    uint64_t send_errors = 0;
    uint64_t datagrams_acknowledged = 0;
    uint64_t datagrams_lost = 0;
    uint64_t datagrams_canceled = 0;
    uint64_t callback_errors = 0;
};
// Live MsQuic connection sample. Invalid after shutdown; not a cached final value.
struct TransportStatistics
{
    bool available = false;
    uint32_t rtt_us = 0;
    uint32_t min_rtt_us = 0;
    uint32_t max_rtt_us = 0;
    uint32_t congestion_window_bytes = 0;
    uint16_t path_mtu = 0;
    uint64_t packets_sent = 0;
    uint64_t packets_received = 0;
    uint64_t suspected_lost_packets = 0;
    uint64_t spurious_lost_packets = 0;
    uint64_t udp_bytes_sent = 0;
    uint64_t udp_bytes_received = 0;
    uint64_t receive_dropped_packets = 0;
    uint64_t receive_decryption_failures = 0;
};

} // namespace libwt
