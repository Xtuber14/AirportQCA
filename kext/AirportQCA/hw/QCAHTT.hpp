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

class QCAPCIDevice;

/* HTT Tx Descriptor */
struct htt_tx_desc {
    __le32 info0; /* Packet type, flags */
    __le32 info1; /* MSDU ID, peer ID */
    __le32 frags_paddr;
    __le32 frags_len;
} __attribute__((packed));

class QCAHTT {
public:
    QCAHTT(QCAPCIDevice *pci);
    ~QCAHTT();

    bool init();
    UInt32 transmitPacket(mbuf_t m);
    int transmitRaw80211(mbuf_t m);
    void receivePackets();

private:
    QCAPCIDevice *fPci;
    u16 fNextMsduId;
};

#endif /* _QCA_HTT_HPP_ */
