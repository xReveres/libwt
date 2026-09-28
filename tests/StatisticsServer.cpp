#include <libwt/Server.hpp>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>

namespace
{
volatile std::sig_atomic_t stopped = 0;

void Signal(int)
{
    stopped = 1;
}

bool Settled(const libwt::SessionStatistics &stats)
{
    return stats.datagrams_sent ==
           stats.datagrams_acknowledged + stats.datagrams_lost + stats.datagrams_canceled;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 4 && argc != 5)
        return 2;
    const bool missing_request_handler = argc == 5 && std::string_view(argv[4]) == "no-request";

    libwt::ServerConfig config;
    config.port = static_cast<uint16_t>(std::stoi(argv[1]));
    config.certificate_file = argv[2];
    config.private_key_file = argv[3];
    std::mutex mutex;
    libwt::SessionPtr first;
    libwt::SessionPtr second;
    libwt::SessionPtr no_handler;
    libwt::SessionPtr replaced;
    std::weak_ptr<int> released_handler;
    std::atomic<unsigned> session_count{0};
    std::atomic<unsigned> close_count{0};
    std::atomic<unsigned> warning_logs{0};
    std::atomic<unsigned> error_logs{0};
    std::atomic<unsigned> reentrant_log_closes{0};
    std::atomic<bool> logger_threw{false};
    config.on_log = [&](const libwt::LogRecord &record)
    {
        if (record.level == libwt::LogLevel::Warning)
        {
            ++warning_logs;
            if (!logger_threw.exchange(true))
                throw std::runtime_error("logger test");
        }
        else
            ++error_logs;
        if (record.message.find("secret-payload") != std::string::npos)
            std::abort();
        if (record.message.find("Connection closed with HTTP/3 error code") == 0 &&
            record.session_id)
        {
            libwt::SessionPtr active;
            {
                std::lock_guard<std::mutex> lock(mutex);
                active = second;
            }
            if (active && active->Id() == *record.session_id)
            {
                active->Close();
                ++reentrant_log_closes;
            }
        }
    };
    config.on_request = [](const libwt::Request &request)
    {
        if (request.path == "/throw-request")
            throw std::runtime_error("admission test");
        return request.path == "/first" || request.path == "/second" ||
               request.path == "/close-true" || request.path == "/none" ||
               request.path == "/replace" || request.path == "/throw-session";
    };
    config.on_session = [&](const auto &session)
    {
        ++session_count;
        const auto &request = session->GetRequest();
        if (request.path == "/close-true")
        {
            session->Close();
            return;
        }
        if (request.path == "/throw-session")
            throw std::runtime_error("publication test");
        if (request.path == "/none")
        {
            std::lock_guard<std::mutex> lock(mutex);
            no_handler = session;
            return;
        }
        if (request.path == "/replace")
        {
            session->SetDatagramHandler([](libwt::Session &, const uint8_t *, size_t)
                                        { std::abort(); });
            session->SetDatagramHandler(
                [](libwt::Session &active, const uint8_t *data, size_t size)
                {
                    if (!active.SendDatagram(data, size))
                        std::abort();
                });
            {
                std::lock_guard<std::mutex> lock(mutex);
                replaced = session;
            }
            if (!session->SendDatagram("ready", 5))
                std::abort();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (request.path == "/first")
                first = session;
            else if (request.path == "/second")
                second = session;
            else
                std::abort();
        }
        auto marker = std::make_shared<int>(1);
        if (request.path == "/first")
            released_handler = marker;
        session->SetDatagramHandler(
            [marker](libwt::Session &active, const uint8_t *data, size_t size)
            {
                (void)marker;
                const std::string_view payload(reinterpret_cast<const char *>(data), size);
                if (payload == "throw")
                    throw std::runtime_error("callback test");
                if (payload == "burst")
                {
                    for (int i = 0; i < 32; ++i)
                        if (!active.SendDatagram(data, size))
                            std::abort();
                    active.Close();
                    return;
                }
                if (!active.SendDatagram(data, size))
                    std::abort();
            });
    };
    config.on_close = [&](const auto &) { ++close_count; };
    if (missing_request_handler)
        config.on_request = {};
    libwt::Server server(config);
    std::string error;
    if (!server.Start(error))
    {
        std::cerr << error << '\n';
        return 1;
    }
    if (server.ReloadCertificate("missing", "missing", error) || error.empty())
        return 10;
    std::signal(SIGTERM, Signal);
    std::cout << "READY\n" << std::flush;
    while (!stopped)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    server.Stop();

    if (missing_request_handler)
    {
        const auto statistics = server.GetStatistics();
        return statistics.sessions_rejected == 1 && statistics.sessions_accepted == 0 &&
                       session_count == 0 && close_count == 0 && warning_logs == 1 &&
                       logger_threw && statistics.callback_errors == 1
                   ? 0
                   : 9;
    }

    if (!first || !second || !no_handler || !replaced || !released_handler.expired())
        return 3;
    auto late_marker = std::make_shared<int>(2);
    std::weak_ptr<int> late_handler = late_marker;
    first->SetDatagramHandler([late_marker](libwt::Session &, const uint8_t *, size_t) {});
    late_marker.reset();
    if (!late_handler.expired())
        return 8;
    const auto a = first->GetStatistics();
    const auto b = second->GetStatistics();
    const auto total = server.GetStatistics();
    if (a.datagrams_received != 2 || a.bytes_received != 10 || a.datagrams_sent != 33 ||
        a.bytes_sent != 165 || a.callback_errors != 0 || !Settled(a) || b.datagrams_received != 2 ||
        b.bytes_received != 10 || b.datagrams_sent != 1 || b.bytes_sent != 5 ||
        b.callback_errors != 1 || !Settled(b) ||
        no_handler->GetStatistics().datagrams_received != 1 ||
        replaced->GetStatistics().datagrams_received != 1 ||
        replaced->GetStatistics().datagrams_sent != 2 || !Settled(replaced->GetStatistics()) ||
        total.sessions_accepted != 6 || total.sessions_rejected != 3 ||
        total.sessions_closed != 6 || total.active_sessions != 0 || total.active_connections != 0 ||
        total.datagrams_received != 6 || total.datagrams_sent != 36 ||
        total.datagrams_acknowledged != a.datagrams_acknowledged + b.datagrams_acknowledged +
                                            replaced->GetStatistics().datagrams_acknowledged ||
        total.datagrams_lost !=
            a.datagrams_lost + b.datagrams_lost + replaced->GetStatistics().datagrams_lost ||
        total.datagrams_canceled != a.datagrams_canceled + b.datagrams_canceled +
                                        replaced->GetStatistics().datagrams_canceled ||
        total.callback_errors != 4 || session_count != 6 || close_count != 6 || warning_logs < 3 ||
        error_logs < 5 || !logger_threw || reentrant_log_closes != 1 ||
        first->GetTransportStatistics().available)
        return 4;
    if (first->TrySendDatagram("x", 1) != libwt::DatagramResult::State ||
        first->GetStatistics().send_rejected_state != 1)
        return 5;
    const auto before_restart = server.GetStatistics();
    if (!server.Start(error))
        return 6;
    server.Stop();
    const auto after_restart = server.GetStatistics();
    if (after_restart.sessions_accepted != before_restart.sessions_accepted ||
        after_restart.datagrams_sent != before_restart.datagrams_sent ||
        after_restart.send_rejected_state != before_restart.send_rejected_state ||
        after_restart.active_connections != 0)
        return 7;
    std::cout << "PASS session statistics, retained counters, restart\n";
}
