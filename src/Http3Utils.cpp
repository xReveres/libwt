#include "Http3Utils.hpp"

#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace libwt::detail
{
bool ReadVarInt(const uint8_t *data, size_t size, size_t &offset, uint64_t &value)
{
    if (offset >= size)
        return false;
    const size_t length = size_t(1) << (data[offset] >> 6);
    if (length > size - offset)
        return false;
    value = data[offset] & 0x3f;
    for (size_t i = 1; i < length; ++i)
        value = (value << 8) | data[offset + i];
    offset += length;
    return true;
}

void WriteVarInt(Bytes &out, uint64_t value)
{
    if (value >= (uint64_t(1) << 62))
        throw std::invalid_argument("QUIC variable integer exceeds 62 bits");
    size_t length = value < 64 ? 1 : value < 16384 ? 2 : value < (uint64_t(1) << 30) ? 4 : 8;
    const size_t begin = out.size();
    out.resize(begin + length);
    for (size_t i = length; i > 0; --i)
    {
        out[begin + i - 1] = static_cast<uint8_t>(value);
        value >>= 8;
    }
    out[begin] |= length == 1 ? 0 : length == 2 ? 0x40 : length == 4 ? 0x80 : 0xc0;
}

Bytes Frame(uint64_t type, const Bytes &payload)
{
    Bytes out;
    WriteVarInt(out, type);
    WriteVarInt(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Bytes Settings()
{
    Bytes payload;
    for (auto setting : {std::pair<uint64_t, uint64_t>{QpackMaxTableCapacitySetting, 0},
                         {QpackBlockedStreamsSetting, 0},
                         {MaxFieldSectionSizeSetting, MaxHeadersBytes},
                         {EnableConnectProtocolSetting, 1},
                         {DatagramSetting, 1},
                         {WebTransportDraft02Setting, 1},
                         {WebTransportDraft07Setting, 1}})
    {
        WriteVarInt(payload, setting.first);
        WriteVarInt(payload, setting.second);
    }
    Bytes out{ControlStream};
    const auto frame = Frame(SettingsFrame, payload);
    out.insert(out.end(), frame.begin(), frame.end());
    return out;
}

Bytes Response(unsigned status)
{
    // QPACK without dynamic entries: Required Insert Count=0, Delta Base=0.
    Bytes block{0, 0};
    if (status == 200)
        block.push_back(0xd9); // Static table index 25: :status=200.
    else
    {
        block.push_back(0x5f); // Literal value, static name index 24 (:status).
        block.push_back(9);
        const std::string value = std::to_string(status);
        block.push_back(static_cast<uint8_t>(value.size()));
        block.insert(block.end(), value.begin(), value.end());
    }
    // draft-02 compatibility; harmless for draft-07 clients.
    const std::string name = "sec-webtransport-http3-draft", value = "draft02";
    block.push_back(0x27);
    block.push_back(static_cast<uint8_t>(name.size() - 7));
    block.insert(block.end(), name.begin(), name.end());
    block.push_back(static_cast<uint8_t>(value.size()));
    block.insert(block.end(), value.begin(), value.end());
    return Frame(HeadersFrame, block);
}

bool ParseSettings(const uint8_t *data, size_t size, PeerSettings &settings)
{
    std::unordered_set<uint64_t> seen;
    size_t offset = 0;
    while (offset < size)
    {
        uint64_t id, value;
        if (!ReadVarInt(data, size, offset, id) || !ReadVarInt(data, size, offset, value) ||
            !seen.insert(id).second)
            return false;
        if (id >= ReservedHttp2SettingFirst && id <= ReservedHttp2SettingLast)
            return false;
        if ((id == EnableConnectProtocolSetting || id == DatagramSetting ||
             id == WebTransportDraft02Setting) &&
            value > 1)
            return false;
        if (id == DatagramSetting)
            settings.datagrams = value == 1;
        if (id == WebTransportDraft02Setting || id == WebTransportDraft07Setting)
            settings.webtransport |= value > 0;
    }
    return true;
}
} // namespace libwt::detail
