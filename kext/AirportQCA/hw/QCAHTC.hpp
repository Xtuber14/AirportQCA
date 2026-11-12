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
};

#endif /* _QCA_HTC_HPP_ */
