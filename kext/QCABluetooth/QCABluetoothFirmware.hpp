//
//  QCABluetoothFirmware.hpp
//  QCABluetoothFirmware - Qualcomm Atheros QCA9377 Bluetooth Firmware Injector
//
//  Main IOService driver injecting rampatch and NVM firmware into QCA Rome Bluetooth.
//

#ifndef _QCA_BLUETOOTH_FIRMWARE_HPP_
#define _QCA_BLUETOOTH_FIRMWARE_HPP_

#include <IOKit/IOService.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <libkern/version.h>

class QCAUSBController;

class QCABluetoothFirmware : public IOService {
    OSDeclareDefaultStructors(QCABluetoothFirmware)

public:
    virtual bool init(OSDictionary *dictionary = nullptr) APPLE_KEXT_OVERRIDE;
    virtual void free() APPLE_KEXT_OVERRIDE;

    virtual IOService* probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

private:
    bool uploadFirmware();
    void publishProperties(bool success);

    IOUSBHostDevice   *fDevice;
    QCAUSBController  *fUsbController;
};

#endif /* _QCA_BLUETOOTH_FIRMWARE_HPP_ */
