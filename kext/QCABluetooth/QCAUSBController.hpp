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
#define QCA_CHECK_STATUS                    0x05
#define QCA_GET_TARGET_VERSION              0x09
#define QCA_DFU_PACKET_SIZE                 4096
#define QCA_DFU_HEADER_SIZE                 28
#define QCA_NVM_HEADER_SIZE                 4

#define QCA_PATCH_UPDATED                   0x80
#define QCA_SYSCFG_UPDATED                  0x40

struct qca_version {
    uint32_t rom_version;
    uint32_t patch_version;
    uint32_t ram_version;
    uint8_t chip_id;
    uint8_t platform_id;
    uint16_t flags;
    uint8_t reserved[4];
} __attribute__((packed));

extern unsigned char qca_rampatch_usb_00000302_bin[];
extern unsigned int qca_rampatch_usb_00000302_bin_len;
extern unsigned char qca_nvm_usb_00000302_bin[];
extern unsigned int qca_nvm_usb_00000302_bin_len;

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
