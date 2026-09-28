#include <libwt/Server.hpp>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>
#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            std::cerr << "Failed: " #x << '\n';                                                    \
            return EXIT_FAILURE;                                                                   \
        }                                                                                          \
    } while (false)
int main()
{
    libwt::Server server({});
    std::string error;
    CHECK(!server.Start(error));
    CHECK(!error.empty());
    CHECK(!server.ReloadCertificate("missing", "missing", error));
    server.Stop();
    server.Stop();
    CHECK(server.GetStatistics().connections_accepted == 0);
    CHECK(server.GetStatistics().datagrams_sent == 0);
    libwt::ServerConfig config;
    config.certificate_file = "missing";
    config.private_key_file = "missing";
    config.bind_address = "invalid address";
    std::vector<libwt::LogRecord> records;
    config.on_log = [&](const libwt::LogRecord &record) { records.push_back(record); };
    libwt::Server invalid(config);
    CHECK(!invalid.Start(error));
    CHECK(error == "Invalid bind address");
    CHECK(records.size() == 1);
    CHECK(records.front().level == libwt::LogLevel::Error);
    CHECK(records.front().message == error);
    CHECK(!records.front().session_id);

    config.on_log = [](const libwt::LogRecord &) { throw std::runtime_error("logger test"); };
    libwt::Server throwing_logger(config);
    CHECK(!throwing_logger.Start(error));
    CHECK(error == "Invalid bind address");
    CHECK(throwing_logger.GetStatistics().callback_errors == 1);
    CHECK(!throwing_logger.ReloadCertificate("missing", "missing", error));
    CHECK(error == "Server is not running");
    CHECK(throwing_logger.GetStatistics().callback_errors == 2);
}
