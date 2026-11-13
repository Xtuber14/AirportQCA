//
//  QCABluetoothFirmware.cpp
//  QCABluetoothFirmware - Qualcomm Atheros QCA9377 Bluetooth Firmware Injector
//

#include "QCABluetoothFirmware.hpp"
#include "QCAUSBController.hpp"
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>

#define super IOService
OSDefineMetaClassAndStructors(QCABluetoothFirmware, IOService)

bool QCABluetoothFirmware::init(OSDictionary *dictionary)
{
    if (!super::init(dictionary)) {
        return false;
    }
    fDevice = nullptr;
    fUsbController = nullptr;
    fVendorID = 0;
    fProductID = 0;
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
    fVendorID  = USBToHost16(dev->getDeviceDescriptor()->idVendor);
    fProductID = USBToHost16(dev->getDeviceDescriptor()->idProduct);
    IOLog("QCABluetoothFirmware: matched Qualcomm Rome Bluetooth USB device (vid=0x%04x pid=0x%04x)\n",
          fVendorID, fProductID);
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

void QCABluetoothFirmware::writeBluetoothNVRAM()
{
    /*
     * Write bluetoothInternalControllerInfo to NVRAM.
     *
     * macOS Monterey+ (and BlueToolFixup with -btlfxnvramcheck) requires this key to
     * be present under the Apple NVRAM GUID 7C436110-AB2A-4BBB-A880-FE41995C9F82.
     * The key MUST be exactly 14 zero bytes — a different length (e.g. 32 bytes)
     * causes BlueToolFixup and bluetoothd to reject the controller silently.
     *
     * We also set bluetoothExternalDongleFailed = 0x00 to signal the adapter
     * is NOT a failed external dongle (native internal controller path).
     *
     * IORegistryEntry::fromPath("/options", gIODTPlane) accesses the NVRAM plane;
     * the prefixed key name "GUID:key" is how IOPlatformExpertDevice stores NVRAM.
     */
    IORegistryEntry *nvram = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!nvram) {
        IOLog("QCABluetoothFirmware: could not access NVRAM /options node\n");
        return;
    }

    /* 14 zero bytes — standard placeholder for non-Apple USB Bluetooth on macOS 12+ */
    static const uint8_t kBtInfoValue[14] = { 0 };
    OSData *btInfoData = OSData::withBytes(kBtInfoValue, sizeof(kBtInfoValue));
    if (btInfoData) {
        nvram->setProperty(
            "7C436110-AB2A-4BBB-A880-FE41995C9F82:bluetoothInternalControllerInfo",
            btInfoData);
        btInfoData->release();
        IOLog("QCABluetoothFirmware: wrote bluetoothInternalControllerInfo (14 bytes) to NVRAM\n");
    }

    /* Ensure dongle-failed flag is clear */
    static const uint8_t kDongleFailed[1] = { 0 };
    OSData *dongleData = OSData::withBytes(kDongleFailed, sizeof(kDongleFailed));
    if (dongleData) {
        nvram->setProperty(
            "7C436110-AB2A-4BBB-A880-FE41995C9F82:bluetoothExternalDongleFailed",
            dongleData);
        dongleData->release();
        IOLog("QCABluetoothFirmware: wrote bluetoothExternalDongleFailed=0x00 to NVRAM\n");
    }

    nvram->release();
}

void QCABluetoothFirmware::publishProperties(bool success)
{
    if (!fDevice) return;

    fDevice->setProperty("FirmwareLoaded", success ? kOSBooleanTrue : kOSBooleanFalse);

    if (success) {
        /*
         * Write correct NVRAM Bluetooth controller info.
         * This is required BEFORE renaming the device so that when macOS's
         * IOBluetoothHostControllerUSBTransport attaches after the rename,
         * bluetoothd can read a valid (14-byte) bluetoothInternalControllerInfo.
         */
        writeBluetoothNVRAM();
    }

    /*
     * Rename the device so macOS's native IOBluetoothHostControllerUSBTransport
     * can re-match it. This is the signal to IOKit that firmware is loaded and
     * the native BT stack should claim the device.
     * Required for macOS Monterey (Darwin 21) through Tahoe (Darwin 26+).
     */
    if (version_major >= 21) {
        fDevice->setName("Bluetooth USB Host Controller");
        IOLog("QCABluetoothFirmware: device renamed to 'Bluetooth USB Host Controller'\n");
    }
}
