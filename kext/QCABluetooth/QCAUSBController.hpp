//
//  QCAUSBController.hpp
//  QCABluetoothFirmware - Qualcomm Atheros QCA9377 Bluetooth Firmware Injector
//
//  USB endpoint management and vendor transfer abstraction for QCA Rome.
//

#ifndef _QCA_USB_CONTROLLER_HPP_
#define _QCA_USB_CONTROLLER_HPP_

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>

#include "QCABluetoothFirmware.hpp"

extern unsigned char qca_rampatch_usb_00000302_bin[];
extern unsigned int qca_rampatch_usb_00000302_bin_len;
extern unsigned char qca_nvm_usb_00000302_bin[];
extern unsigned int qca_nvm_usb_00000302_bin_len;
extern unsigned char qca_rampatch_usb_00000100_bin[];
extern unsigned int qca_rampatch_usb_00000100_bin_len;
extern unsigned char qca_rampatch_usb_00000101_bin[];
extern unsigned int qca_rampatch_usb_00000101_bin_len;
extern unsigned char qca_rampatch_usb_00000200_bin[];
extern unsigned int qca_rampatch_usb_00000200_bin_len;
extern unsigned char qca_rampatch_usb_00000201_bin[];
extern unsigned int qca_rampatch_usb_00000201_bin_len;
extern unsigned char qca_rampatch_usb_00000300_bin[];
extern unsigned int qca_rampatch_usb_00000300_bin_len;
extern unsigned char qca_rampatch_usb_00130200_bin[];
extern unsigned int qca_rampatch_usb_00130200_bin_len;
extern unsigned char qca_rampatch_usb_00130201_bin[];
extern unsigned int qca_rampatch_usb_00130201_bin_len;
extern unsigned char qca_rampatch_usb_00190200_bin[];
extern unsigned int qca_rampatch_usb_00190200_bin_len;

extern unsigned char qca_nvm_usb_00000100_bin[];
extern unsigned int qca_nvm_usb_00000100_bin_len;
extern unsigned char qca_nvm_usb_00000101_bin[];
extern unsigned int qca_nvm_usb_00000101_bin_len;
extern unsigned char qca_nvm_usb_00000200_bin[];
extern unsigned int qca_nvm_usb_00000200_bin_len;
extern unsigned char qca_nvm_usb_00000201_bin[];
extern unsigned int qca_nvm_usb_00000201_bin_len;
extern unsigned char qca_nvm_usb_00000300_bin[];
extern unsigned int qca_nvm_usb_00000300_bin_len;
extern unsigned char qca_nvm_usb_00000302_bin[];
extern unsigned int qca_nvm_usb_00000302_bin_len;
extern unsigned char qca_nvm_usb_00000302_eu_bin[];
extern unsigned int qca_nvm_usb_00000302_eu_bin_len;
extern unsigned char qca_nvm_usb_00130200_bin[];
extern unsigned int qca_nvm_usb_00130200_bin_len;
extern unsigned char qca_nvm_usb_00130201_bin[];
extern unsigned int qca_nvm_usb_00130201_bin_len;
extern unsigned char qca_nvm_usb_00130201_gf_bin[];
extern unsigned int qca_nvm_usb_00130201_gf_bin_len;
extern unsigned char qca_nvm_usb_00190200_bin[];
extern unsigned int qca_nvm_usb_00190200_bin_len;

class QCAUSBController : public OSObject {
    OSDeclareDefaultStructors(QCAUSBController)

public:
    virtual bool initWithDevice(IOService *client, IOUSBHostDevice *dev);
    virtual void free() APPLE_KEXT_OVERRIDE;

    IOReturn sendVendorRequestIn(uint8_t request, void *data, uint16_t size);
    IOReturn sendVendorRequestOut(uint8_t request, const void *data, uint16_t size);
    IOReturn bulkWrite(const void *data, uint32_t length, uint32_t timeoutMs = 5000);
    bool openBulkOutInterface();

    bool resetController();

private:
    IOService           *fClient;
    IOUSBHostDevice     *fDevice;
    IOUSBHostInterface  *fInterface;
    IOUSBHostPipe       *fBulkOutPipe;
};

#endif /* _QCA_USB_CONTROLLER_HPP_ */