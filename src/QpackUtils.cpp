#include "Http3Utils.hpp"

extern "C"
{
#include "lsqpack.h"
#include "lsxpack_header.h"
}

#include <array>
#include <string_view>
#include <unordered_set>

namespace libwt::detail
{
namespace
{
struct Decoder
{
    lsqpack_dec decoder{};
    lsxpack_header header{};
    std::array<char, MaxHeadersBytes> buffer{};
    ConnectHeaders headers;
    std::unordered_set<std::string> pseudo;
    size_t total = 0;
    bool regular = false;
    ~Decoder() { lsqpack_dec_cleanup(&decoder); }
};

lsxpack_header *Prepare(void *context, lsxpack_header *header, size_t space)
{
    auto &d = *static_cast<Decoder *>(context);
    if (space > d.buffer.size())
        return nullptr;
    if (!header)
    {
        header = &d.header;
        lsxpack_header_prepare_decode(header, d.buffer.data(), 0, space);
    }
    else
    {
        header->buf = d.buffer.data();
        header->val_len = static_cast<lsxpack_strlen_t>(space);
    }
    return header;
}

int Header(void *context, lsxpack_header *header)
{
    try
    {
        auto &d = *static_cast<Decoder *>(context);
        const std::string name(header->buf + header->name_offset, header->name_len);
        const std::string value(header->buf + header->val_offset, header->val_len);
        d.total += name.size() + value.size() + 32;
        if (name.empty() || d.total > MaxHeadersBytes ||
            value.find_first_of(std::string("\0\r\n", 3)) != std::string::npos)
            return -1;
        for (size_t i = 0; i < name.size(); ++i)
        {
            const auto c = static_cast<unsigned char>(name[i]);
            if (i == 0 && c == ':')
                continue;
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  std::string_view("!#$%&'*+-.^_`|~").find(char(c)) != std::string_view::npos))
                return -1;
        }
        if (name[0] == ':')
        {
            if (d.regular || !d.pseudo.insert(name).second)
                return -1;
            if (name == ":method")
                d.headers.method = value;
            else if (name == ":scheme")
                d.headers.scheme = value;
            else if (name == ":protocol")
                d.headers.protocol = value;
            else if (name == ":authority")
                d.headers.authority = value;
            else if (name == ":path")
                d.headers.path = value;
            else
                return -1;
        }
        else
        {
            d.regular = true;
            if (name == "connection" || name == "upgrade" || name == "transfer-encoding" ||
                name == "keep-alive" || name == "proxy-connection" ||
                (name == "te" && value != "trailers"))
                return -1;
            if (name == "origin")
            {
                if (value.empty() || !d.headers.origin.empty())
                    return -1;
                d.headers.origin = value;
            }
        }
        return 0;
    }
    catch (...)
    {
        return -1;
    }
}
void Unblocked(void *)
{
}
} // namespace

bool DecodeConnectHeaders(const uint8_t *data, size_t size, uint64_t stream_id,
                          ConnectHeaders &headers)
{
    if (size < 2 || size > MaxHeadersBytes)
        return false;
    static const lsqpack_dec_hset_if callbacks{Unblocked, Prepare, Header};
    Decoder decoder;
    // Advertise zero capacity: no dynamic state, blocked header blocks or ACKs.
    lsqpack_dec_init(&decoder.decoder, nullptr, 0, 0, &callbacks, lsqpack_dec_opts(0));
    const uint8_t *cursor = data;
    const auto result = lsqpack_dec_header_in(&decoder.decoder, &decoder, stream_id, size, &cursor,
                                              size, nullptr, nullptr);
    if (result != LQRHS_DONE || cursor != data + size)
        return false;
    headers = std::move(decoder.headers);
    return headers.method == "CONNECT" && headers.protocol == "webtransport" &&
           headers.scheme == "https" && !headers.authority.empty() && !headers.path.empty() &&
           headers.path[0] == '/';
}
} // namespace libwt::detail
