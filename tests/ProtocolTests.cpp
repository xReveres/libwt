#include "Http3Utils.hpp"
#include <cstdlib>
#include <iostream>

using namespace libwt::detail;
#define REQUIRE(condition)                                                                         \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            std::cerr << __LINE__ << ": " #condition "\n";                                         \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)

static void Literal(Bytes &block, const std::string &name, const std::string &value)
{
    if (name.size() < 7)
        block.push_back(0x20 | static_cast<uint8_t>(name.size()));
    else
    {
        block.push_back(0x27);
        block.push_back(static_cast<uint8_t>(name.size() - 7));
    }
    block.insert(block.end(), name.begin(), name.end());
    block.push_back(static_cast<uint8_t>(value.size()));
    block.insert(block.end(), value.begin(), value.end());
}

int main()
{
    for (uint64_t value :
         {uint64_t(0), uint64_t(63), uint64_t(64), uint64_t(16383), uint64_t(16384),
          (uint64_t(1) << 30) - 1, uint64_t(1) << 30, (uint64_t(1) << 62) - 1})
    {
        Bytes encoded;
        WriteVarInt(encoded, value);
        for (size_t truncated = 0; truncated < encoded.size(); ++truncated)
        {
            size_t offset = 0;
            uint64_t decoded = 0;
            REQUIRE(!ReadVarInt(encoded.data(), truncated, offset, decoded));
            REQUIRE(offset == 0);
        }
        size_t offset = 0;
        uint64_t decoded = 0;
        REQUIRE(ReadVarInt(encoded.data(), encoded.size(), offset, decoded));
        REQUIRE(decoded == value && offset == encoded.size());
    }
    PeerSettings settings;
    Bytes valid{0x33, 1};
    WriteVarInt(valid, 0x2b603743);
    WriteVarInt(valid, 1);
    REQUIRE(ParseSettings(valid.data(), valid.size(), settings));
    REQUIRE(settings.datagrams && settings.webtransport);
    for (const Bytes &invalid :
         {Bytes{0x33, 1, 0x33, 1}, Bytes{2, 0}, Bytes{8, 2}, Bytes{0x33}, Bytes{0x33, 2}})
        REQUIRE(!ParseSettings(invalid.data(), invalid.size(), settings));

    Bytes block{0, 0};
    Literal(block, ":method", "CONNECT");
    Literal(block, ":scheme", "https");
    Literal(block, ":authority", "localhost");
    Literal(block, ":path", "/echo");
    Literal(block, ":protocol", "webtransport");
    auto empty_origin = block;
    Literal(empty_origin, "origin", "");
    Literal(block, "origin", "https://localhost");
    ConnectHeaders headers;
    REQUIRE(!DecodeConnectHeaders(empty_origin.data(), empty_origin.size(), 0, headers));
    auto duplicate_origin = block;
    Literal(duplicate_origin, "origin", "https://localhost");
    REQUIRE(!DecodeConnectHeaders(duplicate_origin.data(), duplicate_origin.size(), 0, headers));
    REQUIRE(DecodeConnectHeaders(block.data(), block.size(), 0, headers));
    REQUIRE(headers.path == "/echo" && headers.authority == "localhost");
    for (size_t i = 0; i < block.size(); ++i)
    {
        // Truncations before all required pseudo-headers must not create a session.
        if (i < block.size() - 26)
            REQUIRE(!DecodeConnectHeaders(block.data(), i, 0, headers));
    }
    auto duplicate = block;
    Literal(duplicate, ":path", "/other");
    REQUIRE(!DecodeConnectHeaders(duplicate.data(), duplicate.size(), 0, headers));
    auto uppercase = block;
    Literal(uppercase, "Origin", "https://localhost");
    REQUIRE(!DecodeConnectHeaders(uppercase.data(), uppercase.size(), 0, headers));
    auto dynamic = block;
    dynamic[0] = 1;
    REQUIRE(!DecodeConnectHeaders(dynamic.data(), dynamic.size(), 0, headers));
    const Bytes large(MaxHeadersBytes + 1, 0);
    REQUIRE(!DecodeConnectHeaders(large.data(), large.size(), 0, headers));
    std::cout << "libwt protocol tests passed\n";
}
