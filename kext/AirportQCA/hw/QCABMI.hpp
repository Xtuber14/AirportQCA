//
//  QCABMI.hpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//
//  Bootloader Messaging Interface (BMI) implementation for QCA9377.
//

#ifndef _QCA_BMI_HPP_
#define _QCA_BMI_HPP_

#include "../include/qca_compat.h"
#include "../include/qca_hw.h"

class QCAPCIDevice;

class QCABMI {
public:
    QCABMI(QCAPCIDevice *pci);
    ~QCABMI();

    int getTargetInfo(struct bmi_target_info *info);
    int writeMem(u32 address, const void *data, u32 length);
    int readMem(u32 address, void *data, u32 length);
    int execute(u32 address, u32 param);
    int setAppStart(u32 address);
    int writeSocRegister(u32 address, u32 value);
    int done();

private:
    int exchangeMessage(const void *req, u32 reqLen, void *resp, u32 *respLen);

    QCAPCIDevice *fPci;
    bool fDoneSent;
};

#endif /* _QCA_BMI_HPP_ */
