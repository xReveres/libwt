#include <node_api.h>
#include <libwt/Server.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
struct SessionHandle
{
    libwt::SessionPtr session;
    std::atomic<uint64_t> dropped_datagrams{0};
    std::atomic<bool> closed{false};
};
struct Event
{
    std::string type;
    uint64_t id = 0;
    std::vector<uint8_t> data;
    std::shared_ptr<SessionHandle> handle;
    std::optional<libwt::LogRecord> record;
    Event(std::string kind, uint64_t session_id, std::shared_ptr<SessionHandle> session)
        : type(std::move(kind)), id(session_id), handle(std::move(session))
    {
    }
};
struct Entry
{
    std::shared_ptr<SessionHandle> handle;
};
struct Native
{
    std::mutex mutex;
    std::deque<Event> events;
    std::unordered_map<uint64_t, Entry> sessions;
    size_t lifecycle_used = 0;
    size_t lifecycle_limit = 0;
    size_t dropped_datagrams = 0;
    size_t queued_datagrams = 0;
    size_t datagram_limit = 1024;
    size_t queued_logs = 0;
    size_t dropped_logs = 0;
    static constexpr size_t LogLimit = 256;
    std::unique_ptr<libwt::Server> server;
    ~Native()
    {
        if (server)
            server->Stop();
    }
};

void Check(napi_env env, napi_status status)
{
    if (status != napi_ok)
    {
        const napi_extended_error_info *info = nullptr;
        napi_get_last_error_info(env, &info);
        throw std::runtime_error(info && info->error_message ? info->error_message
                                                             : "Node-API failure");
    }
}
napi_value String(napi_env env, const std::string &value)
{
    napi_value result;
    Check(env, napi_create_string_utf8(env, value.c_str(), value.size(), &result));
    return result;
}
napi_value Big(napi_env env, uint64_t value)
{
    napi_value result;
    Check(env, napi_create_bigint_uint64(env, value, &result));
    return result;
}
std::string Text(napi_env env, napi_value value)
{
    napi_valuetype type;
    Check(env, napi_typeof(env, value, &type));
    if (type != napi_string)
        throw std::invalid_argument("Expected string");
    size_t length;
    Check(env, napi_get_value_string_utf8(env, value, nullptr, 0, &length));
    std::string result(length + 1, '\0');
    Check(env, napi_get_value_string_utf8(env, value, result.data(), result.size(), &length));
    result.resize(length);
    return result;
}
void Set(napi_env env, napi_value object, const char *key, napi_value value)
{
    Check(env, napi_set_named_property(env, object, key, value));
}
void SetBig(napi_env env, napi_value object, const char *key, uint64_t value)
{
    Set(env, object, key, Big(env, value));
}
void SetBool(napi_env env, napi_value object, const char *key, bool value)
{
    napi_value result;
    Check(env, napi_get_boolean(env, value, &result));
    Set(env, object, key, result);
}
napi_value Object(napi_env env);
napi_value RequestValue(napi_env env, const libwt::Request &request)
{
    auto value = Object(env);
    Set(env, value, "authority", String(env, request.authority));
    Set(env, value, "path", String(env, request.path));
    Set(env, value, "origin", String(env, request.origin));
    Set(env, value, "peerAddress", String(env, request.peer_address));
    napi_value port;
    Check(env, napi_create_uint32(env, request.peer_port, &port));
    Set(env, value, "peerPort", port);
    return value;
}
napi_value Object(napi_env env)
{
    napi_value result;
    Check(env, napi_create_object(env, &result));
    return result;
}
napi_value Undefined(napi_env env)
{
    napi_value result;
    Check(env, napi_get_undefined(env, &result));
    return result;
}
std::vector<napi_value> Args(napi_env env, napi_callback_info info, size_t count)
{
    std::vector<napi_value> result(count);
    size_t actual = count;
    Check(env, napi_get_cb_info(env, info, &actual, result.data(), nullptr, nullptr));
    if (actual != count)
        throw std::invalid_argument("Wrong argument count");
    return result;
}
napi_value Property(napi_env env, napi_value object, const char *name)
{
    napi_value result;
    Check(env, napi_get_named_property(env, object, name, &result));
    return result;
}
bool Has(napi_env env, napi_value object, const char *name)
{
    bool result;
    Check(env, napi_has_named_property(env, object, name, &result));
    return result;
}
std::string OptionalText(napi_env env, napi_value object, const char *name,
                         const std::string &fallback = {})
{
    return Has(env, object, name) ? Text(env, Property(env, object, name)) : fallback;
}
uint32_t OptionalUint(napi_env env, napi_value object, const char *name, uint32_t fallback)
{
    if (!Has(env, object, name))
        return fallback;
    napi_value value = Property(env, object, name);
    napi_valuetype type;
    Check(env, napi_typeof(env, value, &type));
    if (type != napi_number)
        throw std::invalid_argument(std::string(name) + " must be a number");
    double number;
    Check(env, napi_get_value_double(env, value, &number));
    if (!std::isfinite(number) || number < 0 || number > UINT32_MAX || std::floor(number) != number)
        throw std::invalid_argument(std::string(name) + " must be an unsigned 32-bit integer");
    return static_cast<uint32_t>(number);
}
Native *Unwrap(napi_env env, napi_value object)
{
    Native *result = nullptr;
    Check(env, napi_unwrap(env, object, reinterpret_cast<void **>(&result)));
    if (!result)
        throw std::invalid_argument("Invalid server handle");
    return result;
}
void Finalize(napi_env, void *data, void *)
{
    delete static_cast<Native *>(data);
}
std::shared_ptr<SessionHandle> UnwrapSession(napi_env env, napi_value object)
{
    std::shared_ptr<SessionHandle> *result = nullptr;
    Check(env, napi_unwrap(env, object, reinterpret_cast<void **>(&result)));
    if (!result)
        throw std::invalid_argument("Invalid session handle");
    return *result;
}
void FinalizeSession(napi_env, void *data, void *)
{
    delete static_cast<std::shared_ptr<SessionHandle> *>(data);
}
napi_value WrapSession(napi_env env, const std::shared_ptr<SessionHandle> &handle)
{
    auto value = Object(env);
    auto pointer = std::make_unique<std::shared_ptr<SessionHandle>>(handle);
    Check(env, napi_wrap(env, value, pointer.get(), FinalizeSession, nullptr, nullptr));
    pointer.release();
    return value;
}

napi_value Stats(napi_env env, const libwt::ServerStatistics &s)
{
    auto value = Object(env);
#define FIELD(name) SetBig(env, value, #name, s.name)
    FIELD(connections_accepted);
    FIELD(connections_rejected);
    FIELD(connections_connected);
    FIELD(connections_closed);
    FIELD(active_connections);
    FIELD(sessions_accepted);
    FIELD(sessions_rejected);
    FIELD(sessions_closed);
    FIELD(active_sessions);
    FIELD(datagrams_received);
    FIELD(bytes_received);
    FIELD(datagrams_ignored);
    FIELD(datagrams_sent);
    FIELD(bytes_sent);
    FIELD(send_rejected_size);
    FIELD(send_rejected_queue);
    FIELD(send_rejected_state);
    FIELD(send_errors);
    FIELD(datagrams_acknowledged);
    FIELD(datagrams_lost);
    FIELD(datagrams_canceled);
    FIELD(callback_errors);
    FIELD(protocol_errors);
    FIELD(certificate_reloads);
    FIELD(certificate_reload_errors);
#undef FIELD
    return value;
}
napi_value SessionStatsValue(napi_env env, const libwt::SessionStatistics &s)
{
    auto value = Object(env);
#define FIELD(name) SetBig(env, value, #name, s.name)
    FIELD(datagrams_received);
    FIELD(bytes_received);
    FIELD(datagrams_sent);
    FIELD(bytes_sent);
    FIELD(send_rejected_size);
    FIELD(send_rejected_queue);
    FIELD(send_rejected_state);
    FIELD(send_errors);
    FIELD(datagrams_acknowledged);
    FIELD(datagrams_lost);
    FIELD(datagrams_canceled);
    FIELD(callback_errors);
#undef FIELD
    return value;
}
napi_value Transport(napi_env env, const libwt::TransportStatistics &s)
{
    auto value = Object(env);
    SetBool(env, value, "available", s.available);
#define FIELD(name) SetBig(env, value, #name, s.name)
    FIELD(rtt_us);
    FIELD(min_rtt_us);
    FIELD(max_rtt_us);
    FIELD(congestion_window_bytes);
    FIELD(path_mtu);
    FIELD(packets_sent);
    FIELD(packets_received);
    FIELD(suspected_lost_packets);
    FIELD(spurious_lost_packets);
    FIELD(udp_bytes_sent);
    FIELD(udp_bytes_received);
    FIELD(receive_dropped_packets);
    FIELD(receive_decryption_failures);
#undef FIELD
    return value;
}
napi_value Guard(napi_env env, const std::function<napi_value()> &run)
{
    try
    {
        return run();
    }
    catch (const std::exception &error)
    {
        napi_throw_error(env, nullptr, error.what());
        return nullptr;
    }
}
napi_value Create(napi_env env, napi_callback_info info)
{
    return Guard(
        env,
        [&]
        {
            auto args = Args(env, info, 1);
            auto config_value = args[0];
            napi_valuetype type;
            Check(env, napi_typeof(env, config_value, &type));
            if (type != napi_object)
                throw std::invalid_argument("Configuration must be an object");
            auto native = std::make_unique<Native>();
            libwt::ServerConfig config;
            config.bind_address =
                OptionalText(env, config_value, "bindAddress", config.bind_address);
            const auto port = OptionalUint(env, config_value, "port", config.port);
            if (port > UINT16_MAX)
                throw std::invalid_argument("port exceeds 65535");
            config.port = static_cast<uint16_t>(port);
            config.certificate_file = OptionalText(env, config_value, "certificateFile");
            config.private_key_file = OptionalText(env, config_value, "privateKeyFile");
            config.idle_timeout_ms =
                OptionalUint(env, config_value, "idleTimeoutMs", config.idle_timeout_ms);
            config.handshake_timeout_ms =
                OptionalUint(env, config_value, "handshakeTimeoutMs", config.handshake_timeout_ms);
            config.max_connections = OptionalUint(env, config_value, "maxConnections", 1024);
            if (!config.max_connections || config.max_connections > 65536)
                throw std::invalid_argument("maxConnections must be 1..65536");
            native->datagram_limit = OptionalUint(env, config_value, "maxPendingDatagrams", 1024);
            if (!native->datagram_limit || native->datagram_limit > 65536)
                throw std::invalid_argument("maxPendingDatagrams must be 1..65536");
            native->lifecycle_limit = config.max_connections * 2 + 64;
            const auto path = OptionalText(env, config_value, "path", "/echo");
            const auto origin = OptionalText(env, config_value, "origin");
            const auto authority = OptionalText(env, config_value, "authority");
            Native *state = native.get();
            config.on_request = [path, origin, authority](const libwt::Request &request)
            {
                return request.path == path && (origin.empty() || request.origin == origin) &&
                       (authority.empty() || request.authority == authority);
            };
            config.on_session = [state](const libwt::SessionPtr &session)
            {
                auto handle = std::make_shared<SessionHandle>();
                handle->session = session;
                bool exposed = false;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (state->lifecycle_used + 2 <= state->lifecycle_limit)
                    {
                        state->sessions.emplace(session->Id(), Entry{handle});
                        try
                        {
                            state->events.emplace_back("session", session->Id(), handle);
                        }
                        catch (...)
                        {
                            state->sessions.erase(session->Id());
                            throw;
                        }
                        state->lifecycle_used += 2;
                        exposed = true;
                    }
                }
                if (!exposed)
                {
                    session->Close();
                    return;
                }
                session->SetDatagramHandler(
                    [state, id = session->Id()](libwt::Session &, const uint8_t *data, size_t size)
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        auto found = state->sessions.find(id);
                        if (found == state->sessions.end())
                            return;
                        if (state->queued_datagrams >= state->datagram_limit)
                        {
                            ++state->dropped_datagrams;
                            ++found->second.handle->dropped_datagrams;
                            return;
                        }
                        Event event{"datagram", id, found->second.handle};
                        if (size)
                            event.data.assign(data, data + size);
                        state->events.push_back(std::move(event));
                        ++state->queued_datagrams;
                    });
            };
            config.on_close = [state](const libwt::SessionPtr &session)
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                auto found = state->sessions.find(session->Id());
                if (found == state->sessions.end())
                    return;
                found->second.handle->closed = true;
                state->events.emplace_back("close", session->Id(), found->second.handle);
                state->sessions.erase(found);
            };
            config.on_log = [state](const libwt::LogRecord &record)
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->queued_logs >= Native::LogLimit)
                {
                    ++state->dropped_logs;
                    return;
                }
                Event event{"log", 0, {}};
                event.record = record;
                state->events.push_back(std::move(event));
                ++state->queued_logs;
            };
            native->server = std::make_unique<libwt::Server>(std::move(config));
            auto object = Object(env);
            Check(env, napi_wrap(env, object, native.get(), Finalize, nullptr, nullptr));
            native.release();
            return object;
        });
}
napi_value Start(napi_env env, napi_callback_info info)
{
    return Guard(env,
                 [&]
                 {
                     auto args = Args(env, info, 1);
                     auto *state = Unwrap(env, args[0]);
                     std::string error;
                     if (!state->server->Start(error))
                         throw std::runtime_error(error);
                     return Undefined(env);
                 });
}
napi_value Stop(napi_env env, napi_callback_info info)
{
    return Guard(env,
                 [&]
                 {
                     auto args = Args(env, info, 1);
                     Unwrap(env, args[0])->server->Stop();
                     return Undefined(env);
                 });
}
napi_value Reload(napi_env env, napi_callback_info info)
{
    return Guard(
        env,
        [&]
        {
            auto args = Args(env, info, 3);
            std::string error;
            if (!Unwrap(env, args[0])
                     ->server->ReloadCertificate(Text(env, args[1]), Text(env, args[2]), error))
                throw std::runtime_error(error);
            return Undefined(env);
        });
}
napi_value Poll(napi_env env, napi_callback_info info)
{
    return Guard(env,
                 [&]
                 {
                     auto args = Args(env, info, 1);
                     auto *state = Unwrap(env, args[0]);
                     std::deque<Event> events;
                     {
                         std::lock_guard<std::mutex> lock(state->mutex);
                         events.swap(state->events);
                         for (const auto &event : events)
                         {
                             if (event.type == "datagram")
                                 --state->queued_datagrams;
                             else if (event.type == "log")
                                 --state->queued_logs;
                             else
                                 --state->lifecycle_used;
                         }
                     }
                     napi_value array;
                     Check(env, napi_create_array_with_length(env, events.size(), &array));
                     uint32_t index = 0;
                     for (const auto &event : events)
                     {
                         auto item = Object(env);
                         Set(env, item, "type", String(env, event.type));
                         if (event.type == "log")
                         {
                             const auto &record = *event.record;
                             Set(env, item, "level",
                                 String(env, record.level == libwt::LogLevel::Warning ? "warning"
                                                                                      : "error"));
                             Set(env, item, "message", String(env, record.message));
                             if (record.session_id)
                                 SetBig(env, item, "sessionId", *record.session_id);
                         }
                         else
                         {
                             SetBig(env, item, "id", event.id);
                             if (event.type == "datagram")
                             {
                                 napi_value buffer;
                                 void *copied;
                                 Check(env, napi_create_buffer_copy(
                                                env, event.data.size(),
                                                event.data.empty() ? nullptr : event.data.data(),
                                                &copied, &buffer));
                                 Set(env, item, "data", buffer);
                             }
                             if (event.type == "session")
                             {
                                 Set(env, item, "handle", WrapSession(env, event.handle));
                                 Set(env, item, "request",
                                     RequestValue(env, event.handle->session->GetRequest()));
                             }
                         }
                         Check(env, napi_set_element(env, array, index++, item));
                     }
                     return array;
                 });
}
napi_value Send(napi_env env, napi_callback_info info)
{
    return Guard(env,
                 [&]
                 {
                     auto args = Args(env, info, 2);
                     auto handle = UnwrapSession(env, args[0]);
                     bool is_buffer;
                     Check(env, napi_is_buffer(env, args[1], &is_buffer));
                     if (!is_buffer)
                         throw std::invalid_argument("Datagram must be a Buffer");
                     void *data;
                     size_t size;
                     Check(env, napi_get_buffer_info(env, args[1], &data, &size));
                     auto result = handle->session->TrySendDatagram(data, size);
                     const char *names[] = {"accepted", "size", "queue", "state", "error"};
                     return String(env, names[static_cast<unsigned>(result)]);
                 });
}
napi_value Close(napi_env env, napi_callback_info info)
{
    return Guard(env,
                 [&]
                 {
                     auto args = Args(env, info, 1);
                     auto handle = UnwrapSession(env, args[0]);
                     if (!handle->closed)
                         handle->session->Close();
                     return Undefined(env);
                 });
}
napi_value ServerStats(napi_env env, napi_callback_info info)
{
    return Guard(env,
                 [&]
                 {
                     auto args = Args(env, info, 1);
                     auto *state = Unwrap(env, args[0]);
                     auto value = Stats(env, state->server->GetStatistics());
                     {
                         std::lock_guard<std::mutex> lock(state->mutex);
                         SetBig(env, value, "bridge_dropped_datagrams", state->dropped_datagrams);
                         SetBig(env, value, "bridge_dropped_logs", state->dropped_logs);
                     }
                     return value;
                 });
}
napi_value SessionStats(napi_env env, napi_callback_info info)
{
    return Guard(env,
                 [&]
                 {
                     auto args = Args(env, info, 1);
                     auto handle = UnwrapSession(env, args[0]);
                     auto value = SessionStatsValue(env, handle->session->GetStatistics());
                     SetBig(env, value, "bridge_dropped_datagrams", handle->dropped_datagrams);
                     return value;
                 });
}
napi_value TransportStats(napi_env env, napi_callback_info info)
{
    return Guard(env,
                 [&]
                 {
                     auto args = Args(env, info, 1);
                     auto handle = UnwrapSession(env, args[0]);
                     return Transport(env, handle->closed
                                               ? libwt::TransportStatistics{}
                                               : handle->session->GetTransportStatistics());
                 });
}
napi_value SessionClosed(napi_env env, napi_callback_info info)
{
    return Guard(env,
                 [&]
                 {
                     auto args = Args(env, info, 1);
                     napi_value value;
                     Check(env, napi_get_boolean(env, UnwrapSession(env, args[0])->closed, &value));
                     return value;
                 });
}
napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor methods[] = {
        {"create", nullptr, Create, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"start", nullptr, Start, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stop", nullptr, Stop, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"reload", nullptr, Reload, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"poll", nullptr, Poll, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"send", nullptr, Send, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"close", nullptr, Close, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"statistics", nullptr, ServerStats, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sessionStatistics", nullptr, SessionStats, nullptr, nullptr, nullptr, napi_default,
         nullptr},
        {"transportStatistics", nullptr, TransportStats, nullptr, nullptr, nullptr, napi_default,
         nullptr},
        {"sessionClosed", nullptr, SessionClosed, nullptr, nullptr, nullptr, napi_default,
         nullptr}};
    Check(env, napi_define_properties(env, exports, sizeof(methods) / sizeof(methods[0]), methods));
    return exports;
}
} // namespace
NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
