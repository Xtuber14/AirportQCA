//
//  qca_regs.h
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//
//  Hardware register map, bitmasks, and offsets for QCA9377 / Rome PCIe.
//

#ifndef _QCA_REGS_H_
#define _QCA_REGS_H_

#include "qca_compat.h"

/* PCIe Local Bus / Core Registers */
#define SOC_CORE_BASE_ADDRESS               0x00000000
#define PCIE_INTR_ENABLE_ADDRESS            0x00000008
#define PCIE_INTR_CAUSE_ADDRESS             0x0000000C
#define PCIE_INTR_CLR_ADDRESS               0x00000014
#define CORE_CTRL_ADDRESS                   0x00000020

#define CORE_CTRL_PCIE_REG_31_MASK          0x00000800U

/* Interrupt bits */
#define PCIE_INTR_FIRMWARE_MASK             0x00000400
#define PCIE_INTR_CE_MASK_ALL               0x0007F800
#define PCIE_INTR_CE_MASK(pipe)             (0x00000800 << (pipe))

/* Diagnostic / RTC / Reset Registers */
#define RTC_SOC_BASE_ADDRESS                0x00000000
#define SOC_GLOBAL_RESET_ADDRESS            0x00000008
#define RTC_POWER_REG_ADDRESS               0x0000000C
#define RTC_POWER_REG_V_ON                  0x00000001

#define SOC_CHIP_ID_ADDRESS                 0x000000EC
#define SOC_CHIP_ID_REV_MASK                0x0000000F
#define SOC_CHIP_ID_REV_LSB                 0

/* Target Firmware Indicator Register */
#define FW_INDICATOR_ADDRESS                0x0000180C
#define FW_IND_INITIALIZED                  0x00000001
#define FW_IND_EVENT_PENDING                0x00000002

/* Target Window Mapping */
#define WINDOW_ENABLE_BIT                   0x80000000
#define WINDOW_REG_ADDRESS                  0x00000010
#define WINDOW_VALUE_MASK                   0x3FFFFFFF

/* Copy Engine Base Offsets (QCA6174 / QCA9377) */
#define CE0_BASE_ADDRESS                    0x00000C00
#define CE1_BASE_ADDRESS                    0x00001000
#define CE2_BASE_ADDRESS                    0x00001400
#define CE3_BASE_ADDRESS                    0x00001800
#define CE4_BASE_ADDRESS                    0x00001C00
#define CE5_BASE_ADDRESS                    0x00002000
#define CE6_BASE_ADDRESS                    0x00002400
#define CE7_BASE_ADDRESS                    0x00002800

/* Copy Engine Register Offsets within CE Window */
#define CE_SRC_RING_BASE_ADDR_OFFSET        0x00000000
#define CE_SRC_RING_SZ_OFFSET               0x00000004
#define CE_SRC_RING_DMAX_OFFSET             0x00000008
#define CE_SRC_WATERMARK_OFFSET             0x0000000C
#define CE_SRC_WR_INDEX_OFFSET              0x00000010
#define CE_SRC_RD_INDEX_OFFSET              0x00000014

#define CE_DST_RING_BASE_ADDR_OFFSET        0x00000018
#define CE_DST_RING_SZ_OFFSET               0x0000001C
#define CE_DST_RING_DMAX_OFFSET             0x00000020
#define CE_DST_WATERMARK_OFFSET             0x00000024
#define CE_DST_WR_INDEX_OFFSET              0x00000028
#define CE_DST_RD_INDEX_OFFSET              0x0000002C

#define CE_CTRL1_OFFSET                     0x00000030
#define CE_CTRL1_DMAX_MASK                  0x0000FFFF
#define CE_CTRL1_COMPL_INTR_EN              0x00010000
#define CE_CTRL1_WATERMARK_INTR_EN          0x00020000
#define CE_CTRL1_ERR_INTR_EN                0x00040000

#endif /* _QCA_REGS_H_ */
