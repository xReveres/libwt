#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace libwt::detail
{
using Bytes = std::vector<uint8_t>;
constexpr size_t MaxBufferedBytes = 32768;
constexpr size_t MaxHeadersBytes = 16384;
constexpr size_t MaxStreamsPerConnection = 32;
constexpr unsigned MaxPendingSends = 64;
constexpr size_t MaxDatagramPayloadBytes = 65535;
constexpr uint64_t ConnectionFlowControlWindowBytes = 256 * 1024;
constexpr uint32_t StreamReceiveWindowBytes = 32768;
constexpr uint16_t QuicStreamLimit = 8;
constexpr uint64_t ControlStream = 0;
constexpr uint64_t QpackEncoderStream = 2;
constexpr uint64_t QpackDecoderStream = 3;
constexpr uint64_t RequestStream = 4; // Internal marker for bidirectional streams.
constexpr uint64_t WebTransportUnidirectionalStream = 0x54;
constexpr uint64_t DataFrame = 0;
constexpr uint64_t HeadersFrame = 1;
constexpr uint64_t ReservedFrame2 = 2;
constexpr uint64_t CancelPushFrame = 3;
constexpr uint64_t SettingsFrame = 4;
constexpr uint64_t PushPromiseFrame = 5;
constexpr uint64_t ReservedFrame6 = 6;
constexpr uint64_t GoawayFrame = 7;
constexpr uint64_t ReservedFrame8 = 8;
constexpr uint64_t ReservedFrame9 = 9;
constexpr uint64_t MaxPushIdFrame = 0xd;
constexpr uint64_t WebTransportStreamFrame = 0x41;
constexpr uint64_t CloseWebTransportSessionCapsule = 0x2843;
constexpr uint64_t DrainWebTransportSessionCapsule = 0x78ae;
constexpr uint64_t MinCloseCapsuleBytes = 4;
constexpr uint64_t MaxCloseCapsuleBytes = 1028;
constexpr uint64_t QpackMaxTableCapacitySetting = 1;
constexpr uint64_t ReservedHttp2SettingFirst = 2;
constexpr uint64_t ReservedHttp2SettingLast = 5;
constexpr uint64_t MaxFieldSectionSizeSetting = 6;
constexpr uint64_t QpackBlockedStreamsSetting = 7;
constexpr uint64_t EnableConnectProtocolSetting = 8;
constexpr uint64_t DatagramSetting = 0x33;
constexpr uint64_t WebTransportDraft02Setting = 0x2b603742;
constexpr uint64_t WebTransportDraft07Setting = 0x2b603743;
constexpr uint8_t QpackSetCapacityZero = 0x20;
constexpr uint64_t H3NoError = 0x100;
constexpr uint64_t H3GeneralError = 0x101;
constexpr uint64_t H3StreamCreationError = 0x103;
constexpr uint64_t H3ClosedCriticalStream = 0x104;
constexpr uint64_t H3FrameUnexpected = 0x105;
constexpr uint64_t H3FrameError = 0x106;
constexpr uint64_t H3ExcessiveLoad = 0x107;
constexpr uint64_t H3SettingsError = 0x109;
constexpr uint64_t H3MissingSettings = 0x10a;
constexpr uint64_t H3RequestRejected = 0x10b;
constexpr uint64_t H3MessageError = 0x10e;
constexpr uint64_t QpackDecompressionFailed = 0x200;
constexpr uint64_t QpackEncoderStreamError = 0x201;
constexpr uint64_t QpackDecoderStreamError = 0x202;
constexpr uint64_t WebTransportStreamError = 0x170d7b68;

bool ReadVarInt(const uint8_t *data, size_t size, size_t &offset, uint64_t &value);
void WriteVarInt(Bytes &out, uint64_t value);
Bytes Frame(uint64_t type, const Bytes &payload);
Bytes Settings();
Bytes Response(unsigned status);

struct PeerSettings
{
    bool datagrams = false;
    bool webtransport = false;
};
bool ParseSettings(const uint8_t *data, size_t size, PeerSettings &settings);

struct ConnectHeaders
{
    std::string method, scheme, protocol, authority, path, origin;
};
bool DecodeConnectHeaders(const uint8_t *data, size_t size, uint64_t stream_id,
                          ConnectHeaders &headers);
} // namespace libwt::detail
