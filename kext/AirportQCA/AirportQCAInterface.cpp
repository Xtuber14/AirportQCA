// Tahoe IO80211Family V2 interface implementation.
#include "AirportQCAInterface.hpp"
#include "AirportQCA.hpp"
#include "WCL/WCL.hpp"

#define super IO80211InfraProtocol
OSDefineMetaClassAndStructors(AirportQCAInterface, IO80211InfraProtocol)

bool AirportQCAInterface::init()
{
    if (!super::init()) return false;
    fDriver = nullptr;
    return true;
}

void AirportQCAInterface::free()
{
    fDriver = nullptr;
    super::free();
}

IOReturn AirportQCAInterface::setMacAddress(mloAddrArray &addresses)
{
    return fDriver ? fDriver->setHardwareAddress(&addresses, sizeof(ether_addr)) : kIOReturnNotReady;
}

IOReturn AirportQCAInterface::getHW_ADDR(apple80211_hw_mac_address *data)
{
    if (!data) return kIOReturnBadArgument;
    if (!fDriver) return kIOReturnNotReady;
    IOEthernetAddress address = {};
    if (fDriver->getHardwareAddress(&address) != kIOReturnSuccess) return kIOReturnNotReady;
    AirportWCL::HardwareAddress value;
    if (!AirportWCL::copyHardwareAddress(address.bytes, value)) return kIOReturnNotReady;
    memcpy(data, &value, sizeof(value));
    return kIOReturnSuccess;
}

IOReturn AirportQCAInterface::getWCL_CHANNELS_INFO(apple80211ChannelInfo *data)
{
    if (!data) return kIOReturnBadArgument;
    AirportWCL::ChannelInfo info = {};
    static const uint8_t channels[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
        36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112,
        116, 120, 124, 128, 132, 136, 140, 149, 153, 157, 161, 165
    };
    for (unsigned i = 0; i < sizeof(channels); ++i) {
        auto &entry = info.channels[info.count++];
        entry.channel = channels[i];
        entry.channelSpec = (uint16_t)(channels[i] | (channels[i] > 14 ? 0xd000 : 0x1000));
    }
    memcpy(data, &info, sizeof(info));
    return kIOReturnSuccess;
}

#define QCA_GET(name, type, cmd) \
IOReturn AirportQCAInterface::name(type *data) \
{ return fDriver ? fDriver->dispatchApple80211Request(SIOCGA80211, cmd, data) : kIOReturnNotReady; }
#define QCA_SET(name, type, cmd) \
IOReturn AirportQCAInterface::name(type *data) \
{ return fDriver ? fDriver->dispatchApple80211Request(SIOCSA80211, cmd, data) : kIOReturnNotReady; }
QCA_GET(getCHANNEL, apple80211_channel_data, APPLE80211_IOC_CHANNEL)
QCA_GET(getPOWERSAVE, apple80211_powersave_data, APPLE80211_IOC_POWERSAVE)
QCA_GET(getTXPOWER, apple80211_txpower_data, APPLE80211_IOC_TXPOWER)
QCA_GET(getRATE, apple80211_rate_data, APPLE80211_IOC_RATE)
QCA_GET(getOP_MODE, apple80211_opmode_data, APPLE80211_IOC_OP_MODE)
QCA_GET(getRSSI, apple80211_rssi_data, APPLE80211_IOC_RSSI)
QCA_GET(getSUPPORTED_CHANNELS, apple80211_sup_channel_data, APPLE80211_IOC_SUPPORTED_CHANNELS)
QCA_SET(setCIPHER_KEY, apple80211_key, APPLE80211_IOC_CIPHER_KEY)
QCA_SET(setCHANNEL, apple80211_channel_data, APPLE80211_IOC_CHANNEL)

IOReturn AirportQCAInterface::setWCL_SCAN_REQ(apple80211ScanRequest *request)
{
    AirportWCL::ScanRequest decoded;
    if (!AirportWCL::decodeScan(request, 0x1550, decoded)) return kIOReturnBadArgument;
    return fDriver ? fDriver->dispatchApple80211Request(SIOCSA80211, APPLE80211_IOC_SCAN_REQ, nullptr) : kIOReturnNotReady;
}

IOReturn AirportQCAInterface::setWCL_LEAVE_NETWORK(apple80211_leave_network *)
{
    return fDriver ? fDriver->dispatchApple80211Request(SIOCSA80211, APPLE80211_IOC_DISASSOCIATE, nullptr) : kIOReturnNotReady;
}

IOReturn AirportQCAInterface::setWCL_ASSOCIATE(apple80211AssocCandidates *request)
{
    AirportWCL::JoinRequest decoded;
    if (!AirportWCL::decodeJoin(request, 0x2cc, decoded)) return kIOReturnBadArgument;
    if (!fDriver) return kIOReturnNotReady;
    if (AirportWCL::classifyLegacyAuth(decoded) == AirportWCL::LegacyAuth::Invalid)
        return kIOReturnUnsupported;

    const UInt32 candidateChannel = decoded.channelSpec & 0x00ff;
    const UInt16 band = decoded.channelSpec & 0xf000;
    if (candidateChannel &&
        ((band != 0x1000 && band != 0xd000) ||
         (band == 0x1000 && candidateChannel > 14) ||
         (band == 0xd000 && (candidateChannel <= 14 || candidateChannel > 196))))
        return kIOReturnBadArgument;

    apple80211_assoc_data association = {};
    association.version = APPLE80211_VERSION;
    association.ad_mode = APPLE80211_M_STA;
    association.ad_auth_lower = static_cast<uint16_t>(decoded.lowerAuth);
    association.ad_auth_upper = static_cast<uint16_t>(decoded.upperAuth);
    association.ad_ssid_len = decoded.ssidLength;
    memcpy(association.ad_ssid, decoded.ssid, decoded.ssidLength);
    memcpy(association.ad_bssid.octet, decoded.bssid, sizeof(decoded.bssid));
    association.ad_key.version = APPLE80211_VERSION;
    association.ad_key.key_len = decoded.keyLength;
    association.ad_key.key_cipher_type = decoded.keyCipher;
    association.ad_key.key_index = decoded.keyIndex;
    memcpy(association.ad_key.key, decoded.key, decoded.keyLength);
    association.ad_rsn_ie_len = decoded.rsnLength;
    memcpy(association.ad_rsn_ie, decoded.rsn, decoded.rsnLength);
    return fDriver->dispatchApple80211Request(SIOCSA80211, APPLE80211_IOC_ASSOCIATE,
                                               &association, candidateChannel);
}
