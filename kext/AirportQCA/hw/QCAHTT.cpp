//
//  QCAHTT.cpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//

#include "QCAHTT.hpp"
#include "QCAPCIDevice.hpp"
#include "QCACopyEngine.hpp"
#include "QCAHTC.hpp"

QCAHTT::QCAHTT(QCAPCIDevice *pci)
    : fPci(pci), fNextMsduId(0)
{
}

QCAHTT::~QCAHTT()
{
}

bool QCAHTT::init()
{
    qca_info("Initializing HTT data plane");
    return true;
}

UInt32 QCAHTT::transmitPacket(mbuf_t m)
{
    if (!m) return 0;

    QCACopyEngine *ce = fPci->getCopyEngine();
    if (!ce) return 0;

    /* Populate HTT Tx Descriptor */
    struct htt_tx_desc desc;
    desc.info0 = cpu_to_le32(0x01); /* 802.3 format */
    desc.info1 = cpu_to_le32(fNextMsduId++);
    desc.frags_paddr = 0;
    desc.frags_len = cpu_to_le32((u32)mbuf_len(m));

    /* Submit descriptor over CE3 */
    ce->send(3, &desc, sizeof(desc));
    return 1;
}

int QCAHTT::transmitRaw80211(mbuf_t m)
{
    if (!m) return 0;

    QCACopyEngine *ce = fPci->getCopyEngine();
    if (!ce) return 0;

    struct htt_tx_desc desc;
    desc.info0 = cpu_to_le32(0x02); /* Raw 802.11 format */
    desc.info1 = cpu_to_le32(fNextMsduId++);
    desc.frags_paddr = 0;
    desc.frags_len = cpu_to_le32((u32)mbuf_len(m));

    ce->send(3, &desc, sizeof(desc));
    return 0;
}

void QCAHTT::receivePackets()
{
    /* Service incoming packets from CE1 */
}
