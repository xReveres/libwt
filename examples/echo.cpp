#include <libwt/Server.hpp>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

static volatile std::sig_atomic_t stopped = 0;
static volatile std::sig_atomic_t reload = 0;
static void Signal(int value)
{
    if (value == SIGHUP)
        reload = 1;
    else
        stopped = 1;
}

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        std::cerr << "Usage: wt_echo PORT CERT KEY\n";
        return 2;
    }
    libwt::ServerConfig config;
    config.port = static_cast<uint16_t>(std::stoi(argv[1]));
    config.certificate_file = argv[2];
    config.private_key_file = argv[3];
    std::mutex retained_mutex;
    std::vector<libwt::SessionPtr> retained;
    std::atomic<bool> sampled{false};
    config.on_request = [](const libwt::Request &request) { return request.path == "/echo"; };
    config.on_session = [&](const auto &session)
    {
        {
            std::lock_guard<std::mutex> lock(retained_mutex);
            retained.push_back(session);
        }
        session->SetDatagramHandler(
            [&sampled](libwt::Session &active, const uint8_t *data, size_t size)
            {
                if (!sampled.exchange(true))
                {
                    const auto transport = active.GetTransportStatistics();
                    const auto result = active.TrySendDatagram(data, 70000);
                    std::cout << "SAMPLE available=" << transport.available
                              << " received=" << active.GetStatistics().datagrams_received
                              << " oversize=" << (result == libwt::DatagramResult::Size)
                              << std::endl;
                }
                std::cout << "DATAGRAM " << size << " queued=" << active.SendDatagram(data, size)
                          << std::endl;
            });
    };
    config.on_close = [](const auto &) { std::cout << "CLOSED\n" << std::flush; };
    libwt::Server server(config);
    std::string error;
    if (!server.Start(error))
    {
        std::cerr << error << '\n';
        return 1;
    }
    std::signal(SIGTERM, Signal);
    std::signal(SIGINT, Signal);
    std::signal(SIGHUP, Signal);
    std::cout << "READY\n" << std::flush;
    while (!stopped)
    {
        if (reload)
        {
            reload = 0;
            std::cout << (server.ReloadCertificate(argv[2], argv[3], error)
                              ? "RELOADED"
                              : "RELOAD_FAILED: " + error)
                      << '\n'
                      << std::flush;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    server.Stop();
    const auto statistics = server.GetStatistics();
    std::cout << "STATS received=" << statistics.datagrams_received
              << " sent=" << statistics.datagrams_sent
              << " accepted=" << statistics.sessions_accepted
              << " rejected=" << statistics.sessions_rejected
              << " active_sessions=" << statistics.active_sessions
              << " active_connections=" << statistics.active_connections
              << " oversize=" << statistics.send_rejected_size << '\n';
    if (!sampled || !statistics.datagrams_received || !statistics.datagrams_sent ||
        !statistics.sessions_accepted || !statistics.sessions_rejected ||
        statistics.active_sessions || statistics.active_connections ||
        !statistics.send_rejected_size ||
        statistics.sessions_accepted != statistics.sessions_closed ||
        statistics.connections_accepted != statistics.connections_closed)
        return 4;
    // Repeated Stop and destruction must be harmless.
    server.Stop();
    for (const auto &session : retained)
    {
        if (session->SendDatagram("closed", 6))
            return 3;
        if (session->GetTransportStatistics().available)
            return 5;
        session->Close();
    }
}
