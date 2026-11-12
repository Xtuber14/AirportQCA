//
//  qca_hw.h
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//
//  Hardware parameters, Copy Engine descriptors, and protocol constants.
//

#ifndef _QCA_HW_H_
#define _QCA_HW_H_

#include "qca_compat.h"

/* PCI Identification */
#define QCA9377_PCI_VENDOR_ID               0x168C
#define QCA9377_1_0_DEVICE_ID               0x0042

/* Chip ID & Revisions */
#define QCA9377_HW_1_0_CHIP_ID_REV          0x0
#define QCA9377_HW_1_1_CHIP_ID_REV          0x1

#define QCA9377_HW_1_0_DEV_VERSION          0x05020000
#define QCA9377_HW_1_1_DEV_VERSION          0x05020001

#define QCA9377_HW_1_0_PATCH_LOAD_ADDR      0x1234
#define QCA9377_TARGET_CPU_FREQ             176000000 /* 176 MHz */

#define QCA9377_BOARD_DATA_SZ               8124
#define QCA9377_BOARD_EXT_DATA_SZ           0

#define CE_COUNT                            8

/* Copy Engine Descriptor Flags */
#define CE_DESC_FLAGS_BYTE_SWAP             0x0001
#define CE_DESC_FLAGS_GATHER                0x0002
#define CE_DESC_FLAGS_META_INFO             0xFFFC

/* Copy Engine Hardware Descriptor */
struct ce_desc {
    __le32 addr;   /* DMA buffer physical address (low 32-bits) */
    __le16 nbytes; /* Transfer length in bytes */
    __le16 flags;  /* Descriptor flags */
} __attribute__((packed));

/* BMI (Bootloader Messaging Interface) Command IDs */
enum bmi_cmd_id {
    BMI_NO_COMMAND           = 0,
    BMI_DONE                 = 1,
    BMI_READ_MEM             = 2,
    BMI_WRITE_MEM            = 3,
    BMI_EXECUTE              = 4,
    BMI_SET_APP_START        = 5,
    BMI_READ_SOC_REGISTER    = 6,
    BMI_WRITE_SOC_REGISTER   = 7,
    BMI_GET_TARGET_INFO      = 8,
};

/* BMI Target Info Response */
struct bmi_target_info {
    u32 version;
    u32 type;
};

/* HTC (Host-Target Communication) Header */
struct ath10k_htc_hdr {
    u8 eid;          /* Endpoint ID */
    u8 flags;        /* Flags */
    __le16 len;      /* Payload length excluding header */
} __attribute__((packed));

#define ATH10K_HTC_FLAG_NEED_CREDIT_UPDATE  0x01
#define ATH10K_HTC_FLAG_SEND_CREDIT_UPDATE  0x02

/* HTC Endpoints */
enum ath10k_htc_ep_id {
    ATH10K_HTC_EP_0 = 0, /* Control */
    ATH10K_HTC_EP_1 = 1, /* WMI Control */
    ATH10K_HTC_EP_2 = 2, /* HTT Data */
    ATH10K_HTC_EP_3 = 3, /* Diagnostics */
    ATH10K_HTC_EP_COUNT
};

/* WMI Services */
enum ath10k_service_id {
    ATH10K_SERVICE_WMI_CONTROL = 1,
    ATH10K_SERVICE_HTT_DATA    = 2,
};

#endif /* _QCA_HW_H_ */
