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
#include <libkern/OSByteOrder.h>

/* Controller version structure (matches Linux btusb_main.c) */
struct qca_version {
    uint32_t rom_version;
    uint32_t patch_version;
    uint32_t ram_version;
    uint8_t  chip_id;
    uint8_t  platform_id;
    uint16_t flag;
    uint8_t  reserved[4];
} __attribute__((packed));

/* Rampatch version structure (matches Linux btusb_main.c) */
struct qca_rampatch_version {
    uint16_t rom_version_high;
    uint16_t rom_version_low;
    uint16_t patch_version;
} __attribute__((packed));

/* Version flags (matches Linux btusb_main.c) */
#define QCA_FLAG_MULTI_NVM      0x80

/* NVM tag IDs (matches Linux btqca.h) */
#define EDL_TAG_ID_BD_ADDR      2
#define EDL_TAG_ID_HCI          17
#define EDL_TAG_ID_DEEP_SLEEP   27

#define QCA_DFU_PACKET_SIZE     4096
#define QCA_DFU_HEADER_SIZE     28
#define QCA_NVM_HEADER_SIZE     4

#define QCA_GET_TARGET_VERSION  0x09
#define QCA_CHECK_STATUS        0x05
#define QCA_DFU_DOWNLOAD        0x01

#define QCA_SYSCFG_UPDATED      0x40
#define QCA_PATCH_UPDATED       0x80

/* Device info table (matches Linux btusb_main.c qca_devices_table) */
struct qca_device_info {
    uint32_t rom_version;
    uint8_t  rampatch_hdr;    /* length of header in rampatch */
    uint8_t  nvm_hdr;         /* length of header in NVM */
    uint8_t  ver_offset;      /* offset of version structure in rampatch */
};

/* Supported device ROM versions */
#define QCA_ROME_1_0      0x00000100
#define QCA_ROME_1_1      0x00000101
#define QCA_ROME_2_0      0x00000200
#define QCA_ROME_2_1      0x00000201
#define QCA_ROME_3_0      0x00000300
#define QCA_ROME_3_2      0x00000302
#define QCA_WCN6855_1_0   0x00130100
#define QCA_WCN6855_2_0   0x00130200
#define QCA_WCN6855_2_1   0x00130201
#define QCA_WCN785X_2_0   0x00190200

class QCAUSBController;

class QCABluetoothFirmware : public IOService {
    OSDeclareDefaultStructors(QCABluetoothFirmware)

public:
    virtual bool init(OSDictionary *dictionary = nullptr) APPLE_KEXT_OVERRIDE;
    virtual void free() APPLE_KEXT_OVERRIDE;

    virtual IOService* probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice) APPLE_KEXT_OVERRIDE;

private:
    bool uploadFirmware(bool isResume = false);
    void publishProperties(bool success);
    void writeBluetoothNVRAM();
    bool checkFirmwareNeeded();

    /* Board-ID based NVM loading helpers */
    uint16_t extractBoardId(const struct qca_version *ver);
    void generateNvmFileName(char *buf, size_t maxSize, uint32_t romVersion, uint16_t boardId);

    const struct qca_device_info* lookupDeviceInfo(uint32_t romVersion);

    IOUSBHostDevice   *fDevice;
    QCAUSBController  *fUsbController;
    uint16_t           fVendorID;
    uint16_t           fProductID;
    bool               fFirmwareUploaded;
};

#endif /* _QCA_BLUETOOTH_FIRMWARE_HPP_ */