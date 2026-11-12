//
//  QCAWMI.hpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//
//  WMI-TLV control plane engine for QCA9377.
//

#ifndef _QCA_WMI_HPP_
#define _QCA_WMI_HPP_

#include "../include/qca_compat.h"
#include "../include/qca_hw.h"
#include "../include/Apple80211/Apple80211.h"

class QCAPCIDevice;

/* WMI Command IDs */
enum wmi_cmd_id {
    WMI_INIT_CMDID                      = 0x0001,
    WMI_START_SCAN_CMDID                = 0x0002,
    WMI_STOP_SCAN_CMDID                 = 0x0003,
    WMI_VDEV_CREATE_CMDID               = 0x0004,
    WMI_VDEV_DELETE_CMDID               = 0x0005,
    WMI_VDEV_START_REQUEST_CMDID        = 0x0006,
    WMI_VDEV_RESTART_REQUEST_CMDID      = 0x0007,
    WMI_VDEV_STOP_CMDID                 = 0x0008,
    WMI_PEER_CREATE_CMDID               = 0x0009,
    WMI_PEER_DELETE_CMDID               = 0x000A,
    WMI_PEER_ASSOC_CMDID                = 0x000B,
    WMI_VDEV_INSTALL_KEY_CMDID          = 0x000C,
    WMI_PDEV_SET_REGDOMAIN_CMDID        = 0x000D,
};

/* WMI-TLV Header */
struct wmi_tlv {
    __le16 tag;
    __le16 len;
} __attribute__((packed));

class QCAWMI {
public:
    QCAWMI(QCAPCIDevice *pci);
    ~QCAWMI();

    bool init();

    int sendInitCmd();
    int createVdev(u32 vdevId, const u8 *mac);
    int startVdev(u32 vdevId, u32 channel, u32 chWidth);
    int stopVdev(u32 vdevId);
    int startScan(u32 scanType);
    int createPeer(u32 vdevId, const u8 *peerMac);
    int associatePeer(u32 vdevId, const u8 *peerMac, apple80211_assoc_data *assoc);
    int installKey(u32 vdevId, apple80211_key *key);

private:
    int sendWmiCmd(u32 cmdId, const void *payload, u32 len);

    QCAPCIDevice *fPci;
};

#endif /* _QCA_WMI_HPP_ */
