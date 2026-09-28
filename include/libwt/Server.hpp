#pragma once

#include <libwt/Statistics.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace libwt
{
struct ServerTestAccess;
class Session;
using SessionPtr = std::shared_ptr<Session>;
enum class DatagramResult
{
    Accepted,
    Size,
    Queue,
    State,
    Error
};

struct Request
{
    std::string authority;
    std::string path;
    std::string origin;
    std::string peer_address;
    uint16_t peer_port = 0;
};

enum class LogLevel
{
    Warning,
    Error
};

struct LogRecord
{
    LogLevel level;
    std::string message;
    std::optional<uint64_t> session_id;
};

struct ServerConfig
{
    std::string bind_address = "127.0.0.1";
    uint16_t port = 4433;
    std::string certificate_file;
    std::string private_key_file;
    uint32_t idle_timeout_ms = 30000;
    uint32_t handshake_timeout_ms = 5000;
    size_t max_connections = 1024;

    // Callbacks are serialized per connection, but different connections may run
    // concurrently. Do not call Server::Stop/destruct the server from a callback.
    // Return false to reject CONNECT with HTTP 403. No Session is exposed yet.
    std::function<bool(const Request &)> on_request;
    // Called after HTTP 200 is queued and sending is enabled.
    std::function<void(const SessionPtr &)> on_session;
    std::function<void(const SessionPtr &)> on_close;
    std::function<void(const LogRecord &)> on_log;
};

class Session
{
  public:
    using DatagramHandler = std::function<void(Session &, const uint8_t *, size_t)>;
    ~Session();
    // Thread-safe, nonblocking. False means closed, too large, or queue full.
    // True means queued, not acknowledged. The library copies the payload.
    bool SendDatagram(const void *data, size_t size);
    // Same operation with a reason for immediate rejection. Accepted is not an ACK.
    DatagramResult TrySendDatagram(const void *data, size_t size);
    void Close();
    uint64_t Id() const;
    TransportStatistics GetTransportStatistics() const;
    SessionStatistics GetStatistics() const;
    // Replacing a handler does not cancel a callback already in progress.
    // Data is borrowed only for the duration of the callback.
    void SetDatagramHandler(DatagramHandler handler);
    const Request &GetRequest() const;

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

  private:
    struct Impl;
    explicit Session(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
    friend class Server;
};

class Server
{
  public:
    explicit Server(ServerConfig config);
    ~Server();
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    // Lifecycle calls must be serialized by the application. Stop waits for all
    // callbacks; application callback state must outlive Stop/the destructor.
    bool Start(std::string &error);
    void Stop();
    ServerStatistics GetStatistics() const;
    // Transactional TLS rotation for new connections; live sessions stay open.
    bool ReloadCertificate(const std::string &certificate_file, const std::string &private_key_file,
                           std::string &error);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend struct ServerTestAccess;
};
} // namespace libwt
