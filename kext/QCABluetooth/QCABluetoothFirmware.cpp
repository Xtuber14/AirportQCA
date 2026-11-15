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
    fFirmwareUploaded = false;
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

    /* Register for power management to handle suspend/resume.
     * QCA Rome firmware is stored in RAM and lost when USB power is cut during sleep.
     * We need to re-upload firmware on wake (D3->D0 transition). */
    PMinit();
    provider->joinPMtree(this);

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

    bool success = uploadFirmware(false);
    publishProperties(success);

    /* Clean up controller instance - but keep device open for power management */
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

    fFirmwareUploaded = true;

    /* Handoff: close device so Apple's native Bluetooth stack can claim it.
     * We keep the driver attached for power management (setPowerState will be called on wake). */
    fDevice->close(this);
    IOLog("QCABluetoothFirmware: firmware upload reported complete; device handed off to native macOS Bluetooth\n");
    return true;
}

void QCABluetoothFirmware::stop(IOService *provider)
{
    PMstop();
    super::stop(provider);
}

/* Rampatch file version structure (matches Linux btqca.h) */
struct qca_rampatch_version {
    uint16_t rom_version_high;
    uint16_t rom_version_low;
    uint16_t patch_version;
} __attribute__((packed));

/* NVM tag IDs (matches Linux btqca.h) */
#define EDL_TAG_ID_BD_ADDR      2
#define EDL_TAG_ID_HCI          17
#define EDL_TAG_ID_DEEP_SLEEP   27

/* Rome 3.2 version offset in rampatch file */
#define ROME_3_2_VER_OFFSET     18

bool QCABluetoothFirmware::uploadFirmware(bool isResume)
{
    struct qca_version ver;
    memset(&ver, 0, sizeof(ver));

    IOReturn ret = fUsbController->sendVendorRequestIn(QCA_GET_TARGET_VERSION, &ver, sizeof(ver));
    if (ret != kIOReturnSuccess) {
        IOLog("QCABluetoothFirmware: failed to get target version (0x%08x)\n", ret);
        return false;
    }

    const uint32_t romVersion = OSSwapLittleToHostInt32(ver.rom_version);
    if (!isResume) {
        IOLog("QCABluetoothFirmware: detected ROM version 0x%08x, patch 0x%08x\n",
              romVersion, OSSwapLittleToHostInt32(ver.patch_version));
    } else {
        IOLog("QCABluetoothFirmware: [resume] detected ROM version 0x%08x, patch 0x%08x\n",
              romVersion, OSSwapLittleToHostInt32(ver.patch_version));
    }

    if (romVersion != 0x00000302) {
        IOLog("QCABluetoothFirmware: unsupported Rome ROM version 0x%08x\n", romVersion);
        return false;
    }
    if (!isResume) {
        IOLog("QCABluetoothFirmware: verified QCA Rome 3.2 / QCA9377 hardware\n");
    }

    /* Query status to determine if patch is required */
    uint8_t status = 0;
    ret = fUsbController->sendVendorRequestIn(QCA_CHECK_STATUS, &status, sizeof(status));
    if (ret != kIOReturnSuccess) {
        IOLog("QCABluetoothFirmware: failed to query status (0x%08x)\n", ret);
        return false;
    }

    /* If firmware is already loaded (e.g. survived sleep), skip upload */
    if ((status & (QCA_PATCH_UPDATED | QCA_SYSCFG_UPDATED)) == (QCA_PATCH_UPDATED | QCA_SYSCFG_UPDATED)) {
        IOLog("QCABluetoothFirmware: firmware already loaded (status=0x%02x), skipping upload\n", status);
        return true;
    }

    /* --- RAMPATCH UPLOAD --- */
    if (!(status & QCA_PATCH_UPDATED)) {
        const uint8_t *patch = qca_rampatch_usb_00000302_bin;
        const uint32_t patchSize = qca_rampatch_usb_00000302_bin_len;
        if (patchSize < QCA_DFU_HEADER_SIZE + 6 || patch[16] != 0x08 ||
            patch[17] != 0x00 || patch[18] != 0x02 || patch[19] != 0x03) {
            IOLog("QCABluetoothFirmware: bundled rampatch header/version is invalid\n");
            return false;
        }

        /* Verify rampatch version from file header (Linux-style: read at ver_offset) */
        const struct qca_rampatch_version *rver = reinterpret_cast<const struct qca_rampatch_version *>(patch + ROME_3_2_VER_OFFSET);
        uint16_t rver_rom_low = OSSwapLittleToHostInt16(rver->rom_version_low);
        uint16_t rver_patch = OSSwapLittleToHostInt16(rver->patch_version);
        uint32_t rver_rom = rver_rom_low;  // Rome 3.2 uses low 16 bits
        uint32_t currentPatch = OSSwapLittleToHostInt32(ver.patch_version);

        IOLog("QCABluetoothFirmware: rampatch file: rome 0x%x build 0x%x, controller: rome 0x%x build 0x%x\n",
              rver_rom, rver_patch, romVersion, currentPatch);

        /* Allow equal versions for recovery (Linux behavior) */
        if (rver_rom != romVersion || rver_patch < currentPatch) {
            IOLog("QCABluetoothFirmware: rampatch file version did not match with firmware\n");
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
            OSSwapLittleToHostInt32(ver.patch_version) < rver_patch) {
            IOLog("QCABluetoothFirmware: controller did not report the uploaded rampatch\n");
            return false;
        }
    }

    /* --- NVM UPLOAD --- */
    if (!(status & QCA_SYSCFG_UPDATED)) {
        const uint8_t *nvm = qca_nvm_usb_00000302_bin;
        const uint32_t nvmSize = qca_nvm_usb_00000302_bin_len;
        if (nvmSize <= QCA_NVM_HEADER_SIZE) {
            IOLog("QCABluetoothFirmware: bundled NVM image is invalid\n");
            return false;
        }

        /* Create mutable copy for NVM patching.
         * The NVM contains TLV tags; we patch:
         *  - EDL_TAG_ID_HCI (17): enable in-band sleep, clear erroneous charging flag
         *  - EDL_TAG_ID_DEEP_SLEEP (27): enable deep sleep feature
         *  - EDL_TAG_ID_BD_ADDR (2): would set custom BD_ADDR (optional) */
        uint8_t *nvmPatched = (uint8_t *)IOMalloc(nvmSize);
        if (!nvmPatched) {
            IOLog("QCABluetoothFirmware: failed to allocate NVM patch buffer\n");
            return false;
        }
        memcpy(nvmPatched, nvm, nvmSize);

        /* Parse TLV tags: [4-byte header][tag_id(2) tag_len(2) reserve1(4) reserve2(4) data[]]... */
        for (uint32_t offset = QCA_NVM_HEADER_SIZE; offset + 8 < nvmSize;) {
            uint16_t tag_id = *reinterpret_cast<uint16_t *>(nvmPatched + offset);
            uint16_t tag_len = *reinterpret_cast<uint16_t *>(nvmPatched + offset + 2);
            uint8_t *tag_data = nvmPatched + offset + 8;

            if (tag_id == EDL_TAG_ID_HCI && tag_len >= 3) {  /* tag 17: HCI transport config */
                /* data[0] bit 7 = enable in-band sleep (keep/set).
                 * Clear bit 6 which may be misinterpreted as "charging" by bluetoothd. */
                tag_data[0] |= 0x80;   /* enable in-band sleep */
                tag_data[0] &= ~0x40;  /* clear erroneous charging flag */
                IOLog("QCABluetoothFirmware: patched NVM tag 17 (HCI config): in-band sleep enabled, charging flag cleared\n");
            }
            else if (tag_id == EDL_TAG_ID_DEEP_SLEEP && tag_len >= 1) {  /* tag 27: deep sleep */
                /* data[0] bit 0 = enable deep sleep */
                tag_data[0] |= 0x01;
                IOLog("QCABluetoothFirmware: patched NVM tag 27 (DEEP_SLEEP): enabled\n");
            }
            /* BD_ADDR (tag 2) patching would go here if custom address needed */

            offset += 8 + tag_len;
            if (tag_len == 0) break;
        }

        IOLog("QCABluetoothFirmware: uploading Rome 3.2 NVM (%u bytes, patched)\n", nvmSize);
        if (fUsbController->sendVendorRequestOut(QCA_DFU_DOWNLOAD, nvmPatched, QCA_NVM_HEADER_SIZE) != kIOReturnSuccess) {
            IOFree(nvmPatched, nvmSize);
            IOLog("QCABluetoothFirmware: failed to send NVM header\n");
            return false;
        }
        IODelay(20000);
        for (uint32_t offset = QCA_NVM_HEADER_SIZE; offset < nvmSize;) {
            const uint32_t chunk = (nvmSize - offset > QCA_DFU_PACKET_SIZE)
                ? QCA_DFU_PACKET_SIZE : nvmSize - offset;
            IOReturn result = fUsbController->bulkWrite(nvmPatched + offset, chunk, 3000);
            if (result != kIOReturnSuccess) {
                IOFree(nvmPatched, nvmSize);
                IOLog("QCABluetoothFirmware: NVM transfer failed at %u/%u (0x%08x)\n",
                      offset, nvmSize, result);
                return false;
            }
            offset += chunk;
        }
        IOFree(nvmPatched, nvmSize);

        /* Wait for controller to apply NVM (Linux: 100ms for WCN6855 2.1+) */
        IODelay(100000);  /* 100ms */
    }

    uint8_t finalStatus = 0;
    /* Allow controller time to auto-transition from DFU to HCI mode after NVM upload. */
    IODelay(150000);  /* 150ms */

    ret = fUsbController->sendVendorRequestIn(QCA_CHECK_STATUS, &finalStatus, sizeof(finalStatus));
    if (ret != kIOReturnSuccess ||
        (finalStatus & (QCA_PATCH_UPDATED | QCA_SYSCFG_UPDATED)) !=
            (QCA_PATCH_UPDATED | QCA_SYSCFG_UPDATED)) {
        IOLog("QCABluetoothFirmware: firmware status verification failed (status=0x%02x, result=0x%08x)\n",
              finalStatus, ret);
        return false;
    }

    IOLog("QCABluetoothFirmware: firmware verified, controller should auto-transition to HCI mode\n");

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

/* Check if firmware upload is needed by querying controller status.
 * Returns true if rampatch and/or NVM need to be uploaded. */
bool QCABluetoothFirmware::checkFirmwareNeeded()
{
    if (!fUsbController) return false;

    struct qca_version ver;
    memset(&ver, 0, sizeof(ver));
    IOReturn ret = fUsbController->sendVendorRequestIn(QCA_GET_TARGET_VERSION, &ver, sizeof(ver));
    if (ret != kIOReturnSuccess) {
        IOLog("QCABluetoothFirmware: [check] failed to get target version (0x%08x)\n", ret);
        return false;
    }

    uint8_t status = 0;
    ret = fUsbController->sendVendorRequestIn(QCA_CHECK_STATUS, &status, sizeof(status));
    if (ret != kIOReturnSuccess) {
        IOLog("QCABluetoothFirmware: [check] failed to query status (0x%08x)\n", ret);
        return false;
    }

    bool needed = (status & (QCA_PATCH_UPDATED | QCA_SYSCFG_UPDATED)) != (QCA_PATCH_UPDATED | QCA_SYSCFG_UPDATED);
    IOLog("QCABluetoothFirmware: [check] status=0x%02x, firmware needed=%d\n", status, needed);
    return needed;
}

/* Power management: handle D3->D0 transition (wake from sleep).
 * QCA Rome firmware is in RAM and lost when USB power is cut during sleep.
 * On wake, the controller reverts to DFU mode and needs firmware re-upload. */
IOReturn QCABluetoothFirmware::setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice)
{
    IOLog("QCABluetoothFirmware: setPowerState(%lu)\n", powerStateOrdinal);

    /* powerStateOrdinal: 0 = off (D3), 1 = on (D0) */
    if (powerStateOrdinal == 1) {
        /* Wake from sleep (D3->D0) */
        if (fFirmwareUploaded && fDevice) {
            IOLog("QCABluetoothFirmware: wake detected, checking firmware status\n");

            /* Re-open device for firmware operations */
            if (!fDevice->open(this)) {
                IOLog("QCABluetoothFirmware: [wake] failed to re-open device\n");
                return IOPMAckImplied;
            }

            /* Re-init USB controller */
            if (fUsbController) {
                fUsbController->release();
                fUsbController = nullptr;
            }
            fUsbController = new QCAUSBController();
            if (!fUsbController || !fUsbController->initWithDevice(this, fDevice)) {
                IOLog("QCABluetoothFirmware: [wake] failed to init USB controller\n");
                if (fUsbController) {
                    fUsbController->release();
                    fUsbController = nullptr;
                }
                fDevice->close(this);
                return IOPMAckImplied;
            }

            /* Check if firmware re-upload is needed */
            if (checkFirmwareNeeded()) {
                IOLog("QCABluetoothFirmware: [wake] firmware lost, re-uploading\n");
                bool success = uploadFirmware(true);
                if (success) {
                    IOLog("QCABluetoothFirmware: [wake] firmware re-upload successful\n");
                } else {
                    IOLog("QCABluetoothFirmware: [wake] firmware re-upload FAILED\n");
                }
            } else {
                IOLog("QCABluetoothFirmware: [wake] firmware intact, no re-upload needed\n");
            }

            /* Clean up controller */
            if (fUsbController) {
                fUsbController->release();
                fUsbController = nullptr;
            }

            /* Close device - native stack will reclaim it */
            fDevice->close(this);
        }
    }

    return IOPMAckImplied;
}
