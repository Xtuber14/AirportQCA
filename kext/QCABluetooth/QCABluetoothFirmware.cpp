//
//  QCABluetoothFirmware.cpp
//  QCABluetoothFirmware - Qualcomm Atheros QCA9377 Bluetooth Firmware Injector
//

#include "QCABluetoothFirmware.hpp"
#include "QCAUSBController.hpp"
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/OSByteOrder.h>

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
        super::stop(provider);
        return false;
    }

    fUsbController = new QCAUSBController();
    if (!fUsbController || !fUsbController->initWithDevice(this, fDevice)) {
        IOLog("QCABluetoothFirmware: failed to init USB controller\n");
        if (fUsbController) {
            fUsbController->release();
            fUsbController = nullptr;
        }
        fDevice->close(this);
        super::stop(provider);
        return false;
    }

    bool success = uploadFirmware();
    publishProperties(success);

    /* Clean up controller instance */
    if (fUsbController) {
        fUsbController->release();
        fUsbController = nullptr;
    }

    if (!success) {
        fDevice->close(this);
        super::stop(provider);
        IOLog("QCABluetoothFirmware: firmware upload failed; refusing native Bluetooth handoff\n");
        return false;
    }

    /* Handoff: close device so Apple's native Bluetooth stack can claim it. */
    fDevice->close(this);
    IOLog("QCABluetoothFirmware: firmware upload reported complete; device handed off to native macOS Bluetooth\n");
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

    const uint32_t romVersion = OSSwapLittleToHostInt32(ver.rom_version);
    IOLog("QCABluetoothFirmware: detected ROM version 0x%08x, patch 0x%08x\n",
          romVersion, OSSwapLittleToHostInt32(ver.patch_version));

    if (romVersion != 0x00000302) {
        IOLog("QCABluetoothFirmware: unsupported Rome ROM version 0x%08x\n", romVersion);
        return false;
    }
    IOLog("QCABluetoothFirmware: verified QCA Rome 3.2 / QCA9377 hardware\n");

    /* Query status to determine if patch is required */
    uint8_t status = 0;
    ret = fUsbController->sendVendorRequestIn(QCA_CHECK_STATUS, &status, sizeof(status));
    if (ret != kIOReturnSuccess) {
        IOLog("QCABluetoothFirmware: failed to query status (0x%08x)\n", ret);
        return false;
    }

    if (!(status & QCA_PATCH_UPDATED)) {
        const uint8_t *patch = qca_rampatch_usb_00000302_bin;
        const uint32_t patchSize = qca_rampatch_usb_00000302_bin_len;
        if (patchSize < QCA_DFU_HEADER_SIZE + 6 || patch[16] != 0x08 ||
            patch[17] != 0x00 || patch[18] != 0x02 || patch[19] != 0x03) {
            IOLog("QCABluetoothFirmware: bundled rampatch header/version is invalid\n");
            return false;
        }
        const uint16_t patchVersion = static_cast<uint16_t>(
            static_cast<uint16_t>(patch[20]) | (static_cast<uint16_t>(patch[21]) << 8));
        const uint32_t currentPatch = OSSwapLittleToHostInt32(ver.patch_version);
        if (patchVersion < currentPatch) {
            IOLog("QCABluetoothFirmware: bundled rampatch 0x%04x is older than controller patch 0x%08x\n",
                  patchVersion, currentPatch);
            return false;
        }

        IOLog("QCABluetoothFirmware: uploading Rome 3.2 rampatch (%u bytes)\n", patchSize);
        if (fUsbController->sendVendorRequestOut(QCA_DFU_DOWNLOAD, patch, QCA_DFU_HEADER_SIZE) != kIOReturnSuccess) {
            IOLog("QCABluetoothFirmware: failed to send rampatch header\n");
            return false;
        }
        IODelay(20000);
        for (uint32_t offset = QCA_DFU_HEADER_SIZE; offset < patchSize;) {
            const uint32_t chunk = (patchSize - offset > QCA_DFU_PACKET_SIZE)
                ? QCA_DFU_PACKET_SIZE : patchSize - offset;
            IOReturn result = fUsbController->bulkWrite(patch + offset, chunk, 3000);
            if (result != kIOReturnSuccess) {
                IOLog("QCABluetoothFirmware: rampatch transfer failed at %u/%u (0x%08x)\n",
                      offset, patchSize, result);
                return false;
            }
            offset += chunk;
        }

        /* ROM updates its patch version after accepting the bulk stream. */
        IODelay(20000);
        memset(&ver, 0, sizeof(ver));
        if (fUsbController->sendVendorRequestIn(QCA_GET_TARGET_VERSION, &ver, sizeof(ver)) != kIOReturnSuccess ||
            OSSwapLittleToHostInt32(ver.rom_version) != romVersion ||
            OSSwapLittleToHostInt32(ver.patch_version) < patchVersion) {
            IOLog("QCABluetoothFirmware: controller did not report the uploaded rampatch\n");
            return false;
        }
    }

    if (!(status & QCA_SYSCFG_UPDATED)) {
        const uint8_t *nvm = qca_nvm_usb_00000302_bin;
        const uint32_t nvmSize = qca_nvm_usb_00000302_bin_len;
        if (nvmSize <= QCA_NVM_HEADER_SIZE) {
            IOLog("QCABluetoothFirmware: bundled NVM image is invalid\n");
            return false;
        }
        IOLog("QCABluetoothFirmware: uploading Rome 3.2 NVM (%u bytes)\n", nvmSize);
        if (fUsbController->sendVendorRequestOut(QCA_DFU_DOWNLOAD, nvm, QCA_NVM_HEADER_SIZE) != kIOReturnSuccess) {
            IOLog("QCABluetoothFirmware: failed to send NVM header\n");
            return false;
        }
        IODelay(20000);
        for (uint32_t offset = QCA_NVM_HEADER_SIZE; offset < nvmSize;) {
            const uint32_t chunk = (nvmSize - offset > QCA_DFU_PACKET_SIZE)
                ? QCA_DFU_PACKET_SIZE : nvmSize - offset;
            IOReturn result = fUsbController->bulkWrite(nvm + offset, chunk, 3000);
            if (result != kIOReturnSuccess) {
                IOLog("QCABluetoothFirmware: NVM transfer failed at %u/%u (0x%08x)\n",
                      offset, nvmSize, result);
                return false;
            }
            offset += chunk;
        }
    }

    uint8_t finalStatus = 0;
    IODelay(20000);
    ret = fUsbController->sendVendorRequestIn(QCA_CHECK_STATUS, &finalStatus, sizeof(finalStatus));
    if (ret != kIOReturnSuccess ||
        (finalStatus & (QCA_PATCH_UPDATED | QCA_SYSCFG_UPDATED)) !=
            (QCA_PATCH_UPDATED | QCA_SYSCFG_UPDATED)) {
        IOLog("QCABluetoothFirmware: firmware status verification failed (status=0x%02x, result=0x%08x)\n",
              finalStatus, ret);
        return false;
    }

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

    if (!success) {
        return;
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
