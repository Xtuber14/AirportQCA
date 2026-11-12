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

/* Qualcomm Vendor USB Control Requests */
#define QCA_DFU_DOWNLOAD                    0x01
#define QCA_CHECK_STATUS                    0x04
#define QCA_GET_TARGET_VERSION              0x05

#define QCA_PATCH_UPDATED                   0x01
#define QCA_SYSCFG_UPDATED                  0x02

struct qca_version {
    uint32_t rom_version;
    uint32_t patch_version;
    uint32_t ram_version;
    uint32_t ref_clock;
};

class QCAUSBController : public OSObject {
    OSDeclareDefaultStructors(QCAUSBController)

public:
    virtual bool initWithDevice(IOService *client, IOUSBHostDevice *dev);
    virtual void free() APPLE_KEXT_OVERRIDE;

    IOReturn sendVendorRequestIn(uint8_t request, void *data, uint16_t size);
    IOReturn sendVendorRequestOut(uint8_t request, const void *data, uint16_t size);
    IOReturn bulkWrite(const void *data, uint32_t length, uint32_t timeoutMs = 5000);

    bool resetController();

private:
    IOService           *fClient;
    IOUSBHostDevice     *fDevice;
    IOUSBHostInterface  *fInterface;
    IOUSBHostPipe       *fBulkOutPipe;
};

#endif /* _QCA_USB_CONTROLLER_HPP_ */
