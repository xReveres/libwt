#include <libwt/Server.hpp>
#include <type_traits>

static_assert(std::is_same_v<libwt::Statistics, libwt::ServerStatistics>);
static_assert(std::is_same_v<decltype(std::declval<libwt::Session>().GetStatistics()),
                             libwt::SessionStatistics>);

int main()
{
    libwt::ServerConfig config;
    config.on_request = [](const libwt::Request &request) { return request.path == "/echo"; };
    config.on_session = [](const libwt::SessionPtr &session)
    {
        (void)session->GetRequest();
        session->SetDatagramHandler([](libwt::Session &active, const uint8_t *data, size_t size)
                                    { active.SendDatagram(data, size); });
    };
    config.on_close = [](const libwt::SessionPtr &) {};
    config.on_log = [](const libwt::LogRecord &) {};
    libwt::Server server(config);
    return server.GetStatistics().connections_accepted == 0 ? 0 : 1;
}
