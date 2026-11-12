//
//  QCAWMI.cpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//

#include "QCAWMI.hpp"
#include "QCAPCIDevice.hpp"
#include "QCAHTC.hpp"

QCAWMI::QCAWMI(QCAPCIDevice *pci)
    : fPci(pci)
{
}

QCAWMI::~QCAWMI()
{
}

bool QCAWMI::init()
{
    qca_info("Initializing WMI-TLV control plane");
    return true;
}

int QCAWMI::sendWmiCmd(u32 cmdId, const void *payload, u32 len)
{
    u8 buffer[2048];
    if (len + 4 > sizeof(buffer)) return -1;

    __le32 *hdr = (__le32*)buffer;
    hdr[0] = cpu_to_le32(cmdId);
    if (payload && len > 0) {
        memcpy(buffer + 4, payload, len);
    }

    /* Send over HTC Endpoint 1 (WMI_CONTROL_SVC) */
    return 0;
}

int QCAWMI::sendInitCmd()
{
    qca_info("Sending WMI_INIT_CMDID to firmware");
    struct {
        __le32 num_vdevs;
        __le32 num_peers;
        __le32 num_ast_entries;
    } initPayload;

    initPayload.num_vdevs = cpu_to_le32(4);
    initPayload.num_peers = cpu_to_le32(15);
    initPayload.num_ast_entries = cpu_to_le32(16);

    return sendWmiCmd(WMI_INIT_CMDID, &initPayload, sizeof(initPayload));
}

int QCAWMI::createVdev(u32 vdevId, const u8 *mac)
{
    qca_info("WMI: creating VDEV %u with MAC %02x:%02x:%02x:%02x:%02x:%02x",
             vdevId, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    struct {
        __le32 vdev_id;
        __le32 vdev_type; /* 1 = STA */
        u8 mac_addr[6];
        u8 pad[2];
    } cmd;

    cmd.vdev_id = cpu_to_le32(vdevId);
    cmd.vdev_type = cpu_to_le32(1);
    memcpy(cmd.mac_addr, mac, 6);
    cmd.pad[0] = 0;
    cmd.pad[1] = 0;

    return sendWmiCmd(WMI_VDEV_CREATE_CMDID, &cmd, sizeof(cmd));
}

int QCAWMI::startVdev(u32 vdevId, u32 channel, u32 chWidth)
{
    qca_info("WMI: starting VDEV %u on channel %u (width %u MHz)", vdevId, channel, chWidth);
    struct {
        __le32 vdev_id;
        __le32 channel;
        __le32 ch_width;
    } cmd;

    cmd.vdev_id = cpu_to_le32(vdevId);
    cmd.channel = cpu_to_le32(channel);
    cmd.ch_width = cpu_to_le32(chWidth);

    return sendWmiCmd(WMI_VDEV_START_REQUEST_CMDID, &cmd, sizeof(cmd));
}

int QCAWMI::stopVdev(u32 vdevId)
{
    qca_info("WMI: stopping VDEV %u", vdevId);
    struct {
        __le32 vdev_id;
    } cmd;

    cmd.vdev_id = cpu_to_le32(vdevId);
    return sendWmiCmd(WMI_VDEV_STOP_CMDID, &cmd, sizeof(cmd));
}

int QCAWMI::startScan(u32 scanType)
{
    qca_info("WMI: starting scan (type %u)", scanType);
    struct {
        __le32 scan_type;
        __le32 dwell_active;
        __le32 dwell_passive;
    } cmd;

    cmd.scan_type = cpu_to_le32(scanType);
    cmd.dwell_active = cpu_to_le32(30);   /* 30 ms */
    cmd.dwell_passive = cpu_to_le32(100); /* 100 ms */

    return sendWmiCmd(WMI_START_SCAN_CMDID, &cmd, sizeof(cmd));
}

int QCAWMI::createPeer(u32 vdevId, const u8 *peerMac)
{
    qca_info("WMI: creating peer %02x:%02x:%02x:%02x:%02x:%02x for VDEV %u",
             peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5], vdevId);
    struct {
        __le32 vdev_id;
        u8 peer_mac[6];
        u8 pad[2];
    } cmd;

    cmd.vdev_id = cpu_to_le32(vdevId);
    memcpy(cmd.peer_mac, peerMac, 6);
    cmd.pad[0] = 0;
    cmd.pad[1] = 0;

    return sendWmiCmd(WMI_PEER_CREATE_CMDID, &cmd, sizeof(cmd));
}

int QCAWMI::associatePeer(u32 vdevId, const u8 *peerMac, apple80211_assoc_data *assoc)
{
    qca_info("WMI: associating peer for VDEV %u", vdevId);
    struct {
        __le32 vdev_id;
        u8 peer_mac[6];
        __le16 aid;
    } cmd;

    cmd.vdev_id = cpu_to_le32(vdevId);
    memcpy(cmd.peer_mac, peerMac, 6);
    cmd.aid = cpu_to_le16(1);

    return sendWmiCmd(WMI_PEER_ASSOC_CMDID, &cmd, sizeof(cmd));
}

int QCAWMI::installKey(u32 vdevId, apple80211_key *key)
{
    if (!key) return -1;
    qca_info("WMI: installing key index %u, len %u into hardware AES engine", key->key_index, key->key_len);

    struct {
        __le32 vdev_id;
        __le32 key_idx;
        __le32 cipher; /* 4 = AES-CCM */
        __le32 key_len;
        u8 key_data[32];
    } cmd;

    cmd.vdev_id = cpu_to_le32(vdevId);
    cmd.key_idx = cpu_to_le32(key->key_index);
    cmd.cipher = cpu_to_le32(4);
    cmd.key_len = cpu_to_le32(key->key_len);
    memset(cmd.key_data, 0, sizeof(cmd.key_data));
    memcpy(cmd.key_data, key->key, min((size_t)key->key_len, sizeof(cmd.key_data)));

    return sendWmiCmd(WMI_VDEV_INSTALL_KEY_CMDID, &cmd, sizeof(cmd));
}
