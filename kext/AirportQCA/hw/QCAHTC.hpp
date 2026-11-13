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
    QCAPCIDevice *fPci;
    u32 fCredits[ATH10K_HTC_EP_COUNT];
    bool fConnected[ATH10K_HTC_EP_COUNT];
    u16 fService[ATH10K_HTC_EP_COUNT];
    u16 fMaxMessage[ATH10K_HTC_EP_COUNT];
    u8 fPipeOut[ATH10K_HTC_EP_COUNT];
    u8 fPipeIn[ATH10K_HTC_EP_COUNT];
    u8 fSequence[ATH10K_HTC_EP_COUNT];
    u16 fCreditSize;
};

#endif /* _QCA_HTC_HPP_ */
