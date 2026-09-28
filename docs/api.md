# C++ API

Public headers are [`Server.hpp`](../include/libwt/Server.hpp) and
[`Statistics.hpp`](../include/libwt/Statistics.hpp), in namespace `libwt`.
Link against `libwt::wt`. `Server` and `Session` are noncopyable; applications receive
sessions as `SessionPtr`, an alias for `std::shared_ptr<Session>`.

## Minimal echo server

Save this as `main.cpp` and use one of the [CMake consumer configurations](building.md#install-and-consume-from-cmake).
Run the resulting program with a PEM certificate and private key as arguments.
It listens on `127.0.0.1:4433` until Enter is pressed or standard input closes.

```cpp
#include <libwt/Server.hpp>
#include <iostream>

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        std::cerr << "Usage: my_server CERT KEY\n";
        return 2;
    }
    libwt::ServerConfig config;
    config.certificate_file = argv[1];
    config.private_key_file = argv[2];
    config.on_request = [](const libwt::Request &request) {
        return request.path == "/echo";
    };
    config.on_session = [](const libwt::SessionPtr &session) {
        session->SetDatagramHandler([](libwt::Session &active, const uint8_t *data, size_t size) {
            const auto result = active.TrySendDatagram(data, size);
            if (result != libwt::DatagramResult::Accepted)
                active.Close();
        });
    };
    libwt::Server server(config);
    std::string error;
    if (!server.Start(error))
    {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << "Listening on 127.0.0.1:4433; press Enter to stop.\n";
    std::cin.get();
    server.Stop();
}
```

This example accepts any origin and authority at `/echo`. Use those request fields
in `on_request` when your application needs stricter admission.

## Server configuration

`Server(ServerConfig config)` stores its configuration by value. Runtime settings
are supplied through this object; the native library reads no application-specific
environment variables or configuration files.

| Field                  | Default       | Meaning                                                                           |
| ---------------------- | ------------- | --------------------------------------------------------------------------------- |
| `bind_address`         | `"127.0.0.1"` | Numeric IP address used by the listener                                           |
| `port`                 | `4433`        | UDP port (`uint16_t`)                                                             |
| `certificate_file`     | Empty         | PEM certificate file, required to start                                           |
| `private_key_file`     | Empty         | PEM private key file, required to start                                           |
| `idle_timeout_ms`      | `30000`       | Passed to MsQuic as the connection idle timeout                                   |
| `handshake_timeout_ms` | `5000`        | Passed to MsQuic as the handshake idle timeout                                    |
| `max_connections`      | `1024`        | Maximum tracked connections; must be positive                                     |
| `on_request`           | Unset         | `bool(const Request&)`; unset or false rejects CONNECT with 403                   |
| `on_session`           | Unset         | `void(const SessionPtr&)`; called after HTTP 200 is queued and sending is enabled |
| `on_close`             | Unset         | `void(const SessionPtr&)`; called when a published session ends                   |
| `on_log`               | Unset         | `void(const LogRecord&)`; warnings and errors                                     |

`Request` contains `authority`, `path`, `origin`, `peer_address`, and `peer_port`.
An absent Origin header produces an empty `origin`. These are decoded request
values and the remote network endpoint, not an authenticated application identity.
Only these fields are exposed; there is no public arbitrary-header collection.

`LogRecord` contains `level` (`LogLevel::Warning` or `LogLevel::Error`), `message`,
and optional `session_id`. No default logger writes messages for the application.

## Server operations

| Method                                                                                        | Behavior                                                                                                 |
| --------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- |
| `bool Start(std::string &error)`                                                              | Starts listening; returns false with an error message on failure. Repeated start while running succeeds. |
| `void Stop()`                                                                                 | Stops admission and waits for connection shutdown and callbacks. Repeated calls are harmless.            |
| `ServerStatistics GetStatistics() const`                                                      | Returns a race-free snapshot of server counters.                                                         |
| `bool ReloadCertificate(const std::string &cert, const std::string &key, std::string &error)` | Loads replacement credentials transactionally for new connections. Requires a running server.            |
| Destructor                                                                                    | Calls `Stop()`.                                                                                          |

On successful start or reload, `error` is cleared. A failed reload preserves the
previous credentials. Existing sessions remain open after a successful reload.
Stopping and restarting the same server object preserves its statistics and
session ID sequence; successful rotation also updates the paths used on restart.

## Session operations

| Method                                                          | Behavior                                                                          |
| --------------------------------------------------------------- | --------------------------------------------------------------------------------- |
| `bool SendDatagram(const void *data, size_t size)`              | Convenience wrapper: true only for `Accepted`.                                    |
| `DatagramResult TrySendDatagram(const void *data, size_t size)` | Copies and queues a payload or returns an immediate rejection reason.             |
| `void Close()`                                                  | Initiates shutdown of this session's QUIC connection; completion is asynchronous. |
| `uint64_t Id() const`                                           | Session identifier assigned by its server; distinct from the QUIC stream ID.      |
| `const Request &GetRequest() const`                             | Immutable metadata valid while the session object remains alive.                  |
| `void SetDatagramHandler(DatagramHandler handler)`              | Replaces the receive handler; an empty handler disables application delivery.     |
| `SessionStatistics GetStatistics() const`                       | Session counters retained after close.                                            |
| `TransportStatistics GetTransportStatistics() const`            | Live transport sample, or `available=false` when unavailable.                     |

`DatagramHandler` is `void(Session&, const uint8_t*, size_t)`. Receive memory is
borrowed for that invocation only; copy it if another thread or later operation
needs it. Outgoing payloads are copied before the send call returns. Empty
datagrams are permitted; a null pointer with a nonzero size is an error.

| `DatagramResult` | Meaning                                                                                |
| ---------------- | -------------------------------------------------------------------------------------- |
| `Accepted`       | MsQuic accepted the send for processing; no delivery guarantee.                        |
| `Size`           | Absolute or negotiated datagram size limit exceeded.                                   |
| `Queue`          | Pending-send limit reached or MsQuic reports insufficient memory.                      |
| `State`          | Session/connection closed, closing, or sending unavailable.                            |
| `Error`          | Invalid payload pointer, unexpected transport failure, or allocation failure in libwt. |

Rejection checks have an implementation order: callers should not treat a return
value as a complete description of every invalid condition. For example, an
oversized payload can return `Size` even during shutdown.

Retaining a `SessionPtr` after shutdown is supported. Its counters and metadata
remain accessible, normal sends return `State`, transport samples are unavailable,
and `Close()` is harmless. Dropping an application handle does not itself close a
session because the active connection retains it.

## Callbacks and threading

- Serialize `Start()`, `Stop()`, destruction, and certificate rotation in the application.
- Transport callbacks are serialized per connection, but different connections can
  invoke callbacks concurrently. Protect shared application state.
- Logging can also occur synchronously from lifecycle operations; make the logger safe
  for calls from the application thread and transport threads.
- Do not call `Stop()` or destroy the server from a native callback: it waits for callbacks
  to complete. Signal an application thread to stop instead.
- Callback state must outlive `Stop()` or the server destructor. Declare captured
  state before the server, or explicitly stop before destroying that state.
- Datagram sends are thread-safe and nonblocking. Avoid lengthy work in receive callbacks.
- Replacing a handler does not cancel an invocation already in progress.
- Avoid capturing the same owning `SessionPtr` in its own datagram handler; use the
  provided `Session&` or a weak pointer when ownership is unnecessary.

Exceptions from `on_request` reject admission. Exceptions from `on_session` or a
datagram handler cause connection shutdown. Exceptions from `on_close` and `on_log`
are contained. Callback failures increment the relevant error counters; a failing
logger is not recursively logged.

## Statistics

All integer fields are `uint64_t` except the narrower fields declared in
`TransportStatistics`. `Statistics` is an alias for `ServerStatistics`.
Snapshots are race-free but not atomic across fields: concurrent activity can
make related counters temporarily differ.

### Server counters

| Fields                                                                         | Meaning                                                                          |
| ------------------------------------------------------------------------------ | -------------------------------------------------------------------------------- |
| `connections_accepted`, `connections_rejected`                                 | Connections admitted to tracking or refused at the listener limit/shutdown check |
| `connections_connected`, `connections_closed`                                  | Completed handshakes and completed connection shutdowns                          |
| `active_connections`                                                           | Currently tracked connections, including handshakes and shutdowns                |
| `sessions_accepted`, `sessions_rejected`, `sessions_closed`, `active_sessions` | Session admission, rejection, close totals and current gauge                     |
| `datagrams_ignored`                                                            | Incoming datagrams with an invalid identifier or no matching active session      |
| `protocol_errors`                                                              | Locally initiated connection shutdowns with a non-normal HTTP/3 error code       |
| `certificate_reloads`, `certificate_reload_errors`                             | Successful and failed rotation attempts                                          |

An accepted session is counted before the HTTP 200 send is attempted. Therefore
`sessions_accepted` can include a session that could not be published to a callback.
`protocol_errors` can also reflect callback/internal failures that close the connection.

### Datagram counters shared by server and session snapshots

| Fields                                                                            | Meaning                                                                         |
| --------------------------------------------------------------------------------- | ------------------------------------------------------------------------------- |
| `datagrams_received`, `bytes_received`                                            | Valid session datagrams received, even when no application handler is installed |
| `datagrams_sent`, `bytes_sent`                                                    | Sends accepted for processing, with application payload bytes only              |
| `send_rejected_size`, `send_rejected_queue`, `send_rejected_state`, `send_errors` | Immediate send outcomes                                                         |
| `datagrams_acknowledged`                                                          | Final acknowledged outcomes, including spurious acknowledgments                 |
| `datagrams_lost`                                                                  | Final lost/discarded outcomes                                                   |
| `datagrams_canceled`                                                              | Final canceled outcomes                                                         |
| `callback_errors`                                                                 | Contained application callback exceptions                                       |

Totals are monotonic for the server object's lifetime or the individual session.
Only `active_*` fields are gauges. Pending send outcomes may update session totals
after its close callback. An acknowledgment describes transport receipt, not
application processing. Application byte counters exclude HTTP/3 and QUIC overhead.

### Live transport sample

Check `available` before using any other field. Samples are not cached as final
connection statistics after shutdown.

- Timing: `rtt_us`, `min_rtt_us`, `max_rtt_us` (microseconds).
- Capacity: `congestion_window_bytes`, `path_mtu` (bytes).
- Packets: `packets_sent`, `packets_received`, `suspected_lost_packets`, `spurious_lost_packets`.
- Transport bytes: `udp_bytes_sent`, `udp_bytes_received` (MsQuic totals).
- Receive failures: `receive_dropped_packets`, `receive_decryption_failures`.
