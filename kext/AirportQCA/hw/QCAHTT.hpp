//
//  QCAHTT.hpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//
//  Host-Target Transport (HTT) data plane framing for QCA9377.
//

#ifndef _QCA_HTT_HPP_
#define _QCA_HTT_HPP_

#include "../include/qca_compat.h"
#include "../include/qca_hw.h"
#include <sys/kpi_mbuf.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>

class QCAPCIDevice;

struct htt_tx_fragment32 { __le32 address; __le32 length; } __attribute__((packed));
struct htt_tx_descriptor {
    u8 flags0;
    __le16 flags1;
    __le16 length;
    __le16 msduId;
    __le32 fragmentsAddress;
    __le32 peerId;
} __attribute__((packed));
struct htt_tx_buffer {
    htt_tx_fragment32 fragments[2];
    ath10k_htc_hdr htc;
    u8 commandType;
    htt_tx_descriptor descriptor;
    u8 packet[0];
} __attribute__((packed, aligned(4)));

class QCAHTT {
public:
    QCAHTT(QCAPCIDevice *pci);
    ~QCAHTT();

    bool init();
    UInt32 transmitPacket(mbuf_t m);
    int transmitRaw80211(mbuf_t m);
    void receivePackets();

private:
    int transmit(mbuf_t m, bool raw80211);
    void completeMsdu(u16 msduId);

    QCAPCIDevice *fPci;
    u16 fNextMsduId;
    IOLock *fLock;
    IOBufferMemoryDescriptor *fPending[256];
};

#endif /* _QCA_HTT_HPP_ */
