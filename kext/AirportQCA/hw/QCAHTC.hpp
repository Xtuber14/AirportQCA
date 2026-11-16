//
//  QCAHTC.hpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//
//  Host-Target Communication (HTC) messaging and credit flow control.
//

#ifndef _QCA_HTC_HPP_
#define _QCA_HTC_HPP_

#include "../include/qca_compat.h"
#include "../include/qca_hw.h"

class QCAPCIDevice;

/* Maximum size of an HTC frame (header plus payload) and the number of
   frames that can be held for an endpoint while another endpoint is
   being served. On the QCA6174 family the HTC control endpoint and the
   HTT data endpoint share CE1, so a frame for one endpoint can arrive
   while the other endpoint is being polled. */
#define HTC_FRAME_MAX       4096
#define HTC_PENDING_SLOTS   4

class QCAHTC {
public:
    QCAHTC(QCAPCIDevice *pci);
    ~QCAHTC();

    bool init();
    bool connectService(u8 epId, u16 serviceId);
    bool start();

    int send(u8 epId, void *buf, u32 len);
    int recv(u8 epId, void *buf, u32 *len);

    u32 getCredits(u8 epId) const {
        if (epId < ATH10K_HTC_EP_COUNT) return fCredits[epId];
        return 0;
    }

private:
    /* A frame that arrived on an endpoint's Copy Engine but belongs to
       a different endpoint. */
    struct PendingFrame {
        u16 len;
        u8 data[HTC_FRAME_MAX];
    };

    bool queuePending(u8 eid, const u8 *frame, u32 frameLength);
    bool takePending(u8 eid, void *buf, u32 *len);

    QCAPCIDevice *fPci;
    u32 fCredits[ATH10K_HTC_EP_COUNT];
    bool fConnected[ATH10K_HTC_EP_COUNT];
    u16 fService[ATH10K_HTC_EP_COUNT];
    u16 fMaxMessage[ATH10K_HTC_EP_COUNT];
    u8 fPipeOut[ATH10K_HTC_EP_COUNT];
    u8 fPipeIn[ATH10K_HTC_EP_COUNT];
    u8 fSequence[ATH10K_HTC_EP_COUNT];
    u16 fCreditSize;

    PendingFrame fPending[ATH10K_HTC_EP_COUNT][HTC_PENDING_SLOTS];
    u8 fPendingCount[ATH10K_HTC_EP_COUNT];
};

#endif /* _QCA_HTC_HPP_ */
