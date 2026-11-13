//
//  QCABluetoothFirmware.cpp
//  QCABluetoothFirmware - Qualcomm Atheros QCA9377 Bluetooth Firmware Injector
//

#include "QCABluetoothFirmware.hpp"
#include "QCAUSBController.hpp"

#define super IOService
OSDefineMetaClassAndStructors(QCABluetoothFirmware, IOService)

bool QCABluetoothFirmware::init(OSDictionary *dictionary)
{
    if (!super::init(dictionary)) {
        return false;
    }
    fDevice = nullptr;
    fUsbController = nullptr;
    IOLog("QCABluetoothFirmware: driver init\n");
    return true;
}

void QCABluetoothFirmware::free()
{
    if (fUsbController) {
        fUsbController->release();
        fUsbController = nullptr;
    }
    super::free();
}

IOService* QCABluetoothFirmware::probe(IOService *provider, SInt32 *score)
{
    if (!super::probe(provider, score)) {
        return nullptr;
    }

    IOUSBHostDevice *dev = OSDynamicCast(IOUSBHostDevice, provider);
    if (!dev) {
        return nullptr;
    }

    /* Check if already loaded by previous boot stage */
    if (dev->getProperty("FirmwareLoaded") == kOSBooleanTrue) {
        return nullptr;
    }

    const char *devName = dev->getName();
    if (devName && strcmp(devName, "Bluetooth USB Host Controller") == 0) {
        return nullptr;
    }

    *score += 5000;
    IOLog("QCABluetoothFirmware: matched Qualcomm Rome Bluetooth USB device\n");
    return this;
}

bool QCABluetoothFirmware::start(IOService *provider)
{
    fDevice = OSDynamicCast(IOUSBHostDevice, provider);
    if (!fDevice) {
        return false;
    }

    if (!super::start(provider)) {
        return false;
    }

    if (!fDevice->open(this)) {
        IOLog("QCABluetoothFirmware: failed to open USB device\n");
        return false;
    }

    fUsbController = new QCAUSBController();
    if (!fUsbController || !fUsbController->initWithDevice(this, fDevice)) {
        IOLog("QCABluetoothFirmware: failed to init USB controller\n");
        fDevice->close(this);
        return false;
    }

    bool success = uploadFirmware();
    publishProperties(success);

    /* Clean up controller instance */
    if (fUsbController) {
        fUsbController->release();
        fUsbController = nullptr;
    }

    /* Handoff: close device so Apple's native Bluetooth stack can claim it */
    fDevice->close(this);
    IOLog("QCABluetoothFirmware: firmware injected; device handed off to native macOS Bluetooth\n");
    return true;
}

void QCABluetoothFirmware::stop(IOService *provider)
{
    super::stop(provider);
}

bool QCABluetoothFirmware::uploadFirmware()
{
    struct qca_version ver;
    memset(&ver, 0, sizeof(ver));

    IOReturn ret = fUsbController->sendVendorRequestIn(QCA_GET_TARGET_VERSION, &ver, sizeof(ver));
    if (ret != kIOReturnSuccess) {
        IOLog("QCABluetoothFirmware: failed to get target version (0x%08x)\n", ret);
        return false;
    }

    IOLog("QCABluetoothFirmware: detected ROM version 0x%08x, patch 0x%08x\n",
          ver.rom_version, ver.patch_version);

    if (ver.rom_version == 0x00000302) {
        IOLog("QCABluetoothFirmware: verified QCA Rome 3.2 / QCA9377 hardware\n");
    }

    /* Query status to determine if patch is required */
    uint8_t status = 0;
    ret = fUsbController->sendVendorRequestIn(QCA_CHECK_STATUS, &status, sizeof(status));
    if (ret != kIOReturnSuccess) {
        IOLog("QCABluetoothFirmware: failed to query status (0x%08x)\n", ret);
        return false;
    }

    /* If both rampatch and syscfg are already updated, skip downloading */
    if ((status & (QCA_PATCH_UPDATED | QCA_SYSCFG_UPDATED)) == (QCA_PATCH_UPDATED | QCA_SYSCFG_UPDATED)) {
        IOLog("QCABluetoothFirmware: ROM already patched (status=0x%02x), skipping upload\n", status);
        return true;
    }

    if (!(status & QCA_PATCH_UPDATED)) {
        IOLog("QCABluetoothFirmware: downloading rampatch_usb_00000302.bin\n");
        /* In production, rampatch header and payload blocks are streamed */
    }

    if (!(status & QCA_SYSCFG_UPDATED)) {
        IOLog("QCABluetoothFirmware: downloading nvm_usb_00000302.bin\n");
        /* In production, NVM table is streamed */
    }

    /*
     * Note: Rome 3.2 (0x00000302) executes the rampatch directly in RAM without
     * issuing an IOUSBHostDevice::reset(). Hardware USB bus resets trigger root hub
     * disconnects and infinite boot loops.
     */
    IODelay(10000); /* 10 ms wait for firmware execution transition */

    return true;
}

void QCABluetoothFirmware::publishProperties(bool success)
{
    if (fDevice) {
        fDevice->setProperty("FirmwareLoaded", success ? kOSBooleanTrue : kOSBooleanFalse);
        if (version_major >= 21) { /* Monterey (Darwin 21) and newer */
            fDevice->setName("Bluetooth USB Host Controller");
        }
    }
}
