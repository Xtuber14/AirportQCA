//
//  QCAHTC.cpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//

#include "QCAHTC.hpp"
#include "QCAPCIDevice.hpp"
#include "QCACopyEngine.hpp"

QCAHTC::QCAHTC(QCAPCIDevice *pci)
    : fPci(pci)
{
    for (int i = 0; i < ATH10K_HTC_EP_COUNT; i++) {
        fCredits[i] = 100; /* Initial safe default credits */
        fConnected[i] = false;
    }
}

QCAHTC::~QCAHTC()
{
}

bool QCAHTC::init()
{
    qca_info("Initializing HTC layer");
    return true;
}

bool QCAHTC::connectService(u8 epId, u16 serviceId)
{
    if (epId >= ATH10K_HTC_EP_COUNT) return false;
    fConnected[epId] = true;
    qca_info("HTC endpoint %d connected to service 0x%04x", epId, serviceId);
    return true;
}

int QCAHTC::send(u8 epId, void *buf, u32 len)
{
    if (epId >= ATH10K_HTC_EP_COUNT || !fConnected[epId]) return -1;
    if (fCredits[epId] == 0) {
        qca_warn("HTC endpoint %d out of credits", epId);
        return -1;
    }

    /* Wrap in HTC header */
    u8 frame[2048];
    if (len + sizeof(struct ath10k_htc_hdr) > sizeof(frame)) return -1;

    struct ath10k_htc_hdr *hdr = (struct ath10k_htc_hdr*)frame;
    hdr->eid = epId;
    hdr->flags = 0;
    hdr->len = cpu_to_le16(len);
    memcpy(frame + sizeof(struct ath10k_htc_hdr), buf, len);

    QCACopyEngine *ce = fPci->getCopyEngine();
    if (!ce) return -1;

    u32 pipe = (epId == ATH10K_HTC_EP_1) ? 4 : 3; /* CE4 for WMI, CE3 for HTT */
    int ret = ce->send(pipe, frame, len + sizeof(struct ath10k_htc_hdr));
    if (ret == 0) {
        fCredits[epId]--;
    }
    return ret;
}

int QCAHTC::recv(u8 epId, void *buf, u32 *len)
{
    QCACopyEngine *ce = fPci->getCopyEngine();
    if (!ce) return -1;

    u32 pipe = (epId == ATH10K_HTC_EP_1) ? 2 : 1; /* CE2 for WMI, CE1 for HTT */
    u8 frame[2048];
    u32 frameLen = sizeof(frame);

    if (ce->recv(pipe, frame, &frameLen) == 0) {
        if (frameLen < sizeof(struct ath10k_htc_hdr)) return -1;

        struct ath10k_htc_hdr *hdr = (struct ath10k_htc_hdr*)frame;
        u32 payloadLen = le16_to_cpu(hdr->len);

        *len = min(*len, payloadLen);
        memcpy(buf, frame + sizeof(struct ath10k_htc_hdr), *len);

        /* Replenish credits */
        if (hdr->flags & ATH10K_HTC_FLAG_SEND_CREDIT_UPDATE) {
            fCredits[epId] += 10;
        }
        return 0;
    }

    return -1;
}
