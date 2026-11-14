//
//  qca_regs.h
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//
//  Hardware register map, bitmasks, and offsets for QCA9377.
//
//  The QCA9377 (Rome) belongs to the QCA6174 register family. All
//  offsets below mirror ath10k's qca6174_regs table and the qca6174
//  PLL/CPU-clock register definitions (source/ath ath10k hw.h/hw.c).
//  Host access to target space is direct BAR0 addressing without the
//  legacy QCA988x window scheme.
//

#ifndef _QCA_REGS_H_
#define _QCA_REGS_H_

#include "qca_compat.h"

/* Per-family register space bases (ath10k qca6174_regs) */
#define RTC_SOC_BASE_ADDRESS                0x00000800
#define RTC_WMAC_BASE_ADDRESS               0x00001000
#define PCIE_LOCAL_BASE_ADDRESS             0x00080000
#define SOC_CORE_BASE_ADDRESS               0x0003a000

/* PCIe local (host) registers — offsets relative to PCIE_LOCAL_BASE */
#define SOC_GLOBAL_RESET_ADDRESS            0x00000008
#define PCIE_INTR_ENABLE_ADDRESS            0x00000008
#define PCIE_INTR_CAUSE_ADDRESS             0x0000000C
#define PCIE_INTR_CLR_ADDRESS               0x00000014
#define CORE_CTRL_ADDRESS                   0x00000020

#define PCIE_INTR_FIRMWARE_MASK             0x00000400
#define PCIE_INTR_CE_MASK_ALL               0x0007F800
#define PCIE_INTR_CE_MASK(pipe)             (0x00000800 << (pipe))

/* CORE_CTRL bits */
#define CORE_CTRL_CPU_INTR_MASK             0x00002000

/* SOC reset registers — offsets relative to RTC_SOC_BASE */
#define SOC_RESET_CONTROL_ADDRESS           0x00000000
#define SOC_RESET_CONTROL_CE_RST_MASK       0x00000001
#define SOC_RESET_CONTROL_CPU_WARM_RST_MASK 0x00000040
#define SOC_LF_TIMER_CONTROL0_ADDRESS       0x00000050
#define SOC_LF_TIMER_CONTROL0_ENABLE_MASK   0x00000004

#define SOC_CHIP_ID_ADDRESS                 0x000000F0
#define SOC_CHIP_ID_REV_MASK                0x0000000F

/* PCIe SoC wake and RTC power state (qca6174/qca9377 pci_ps support).
   The SoC register file is unpowered while the target is asleep, so
   every MMIO access must be preceded by a wake and an RTC state poll
   (ath10k __ath10k_pci_wake / ath10k_pci_wake_wait). */
#define PCIE_SOC_WAKE_ADDRESS              0x00000004
#define PCIE_SOC_WAKE_V_MASK               0x00000001
#define PCIE_SOC_WAKE_RESET                0x00000000
#define RTC_STATE_ADDRESS                  0x00000000
#define RTC_STATE_V_MASK                   0x00000007
#define RTC_STATE_V_ON                     0x00000003

/* Target Firmware Indicator Register (absolute target address) */
#define FW_INDICATOR_ADDRESS                0x0003a028
#define FW_IND_EVENT_PENDING                0x00000001
#define FW_IND_INITIALIZED                  0x00000002

/* Copy Engine Base Offsets (QCA6174 / QCA9377, stride 0x400) */
#define CE0_BASE_ADDRESS                    0x00034400
#define CE1_BASE_ADDRESS                    0x00034800
#define CE2_BASE_ADDRESS                    0x00034C00
#define CE3_BASE_ADDRESS                    0x00035000
#define CE4_BASE_ADDRESS                    0x00035400
#define CE5_BASE_ADDRESS                    0x00035800
#define CE6_BASE_ADDRESS                    0x00035C00
#define CE7_BASE_ADDRESS                    0x00036000

/* Copy Engine Register Offsets within CE Window */
#define CE_SRC_RING_BASE_ADDR_OFFSET        0x00000000
#define CE_SRC_RING_SZ_OFFSET               0x00000004
#define CE_DST_RING_BASE_ADDR_OFFSET        0x00000008
#define CE_DST_RING_SZ_OFFSET               0x0000000C
#define CE_CTRL1_OFFSET                     0x00000010
#define CE_CMD_OFFSET                       0x00000018
#define CE_HOST_IE_OFFSET                   0x0000002C
#define CE_MISC_IE_OFFSET                   0x00000034
#define CE_SRC_WR_INDEX_OFFSET              0x0000003C
#define CE_DST_WR_INDEX_OFFSET              0x00000040
#define CE_SRC_RD_INDEX_OFFSET              0x00000044
#define CE_DST_RD_INDEX_OFFSET              0x00000048
#define CE_CTRL1_DMAX_MASK                  0x0000FFFF
#define CE_CTRL1_SRC_RING_ENABLE            0x00010000
#define CE_CTRL1_DST_RING_ENABLE            0x00020000

/* qca6174 PLL / CPU clock registers (BMI SOC-register addressing:
   address = <base> | <offset>, e.g. RTC_SOC_BASE_ADDRESS | EFUSE_OFFSET) */
#define EFUSE_OFFSET                        0x0000032C
#define EFUSE_XTAL_SEL_LSB                  8
#define EFUSE_XTAL_SEL_MASK                 0x00000700

#define BB_PLL_CONFIG_OFFSET                0x000002F4
#define BB_PLL_CONFIG_FRAC_MASK             0x0003FFFF
#define BB_PLL_CONFIG_OUTDIV_LSB            18
#define BB_PLL_CONFIG_OUTDIV_MASK           0x001C0000

#define WLAN_PLL_SETTLE_OFFSET              0x00000018
#define WLAN_PLL_SETTLE_TIME_MASK           0x000007FF

#define SOC_CORE_CLK_CTRL_OFFSET            0x00000114
#define SOC_CORE_CLK_CTRL_DIV_MASK          0x00000007

#define SOC_CPU_CLOCK_OFFSET                0x00000020
#define SOC_CPU_CLOCK_STANDARD_MASK         0x00000003

#define WLAN_PLL_CONTROL_OFFSET             0x00000014
#define WLAN_PLL_CONTROL_DIV_MASK           0x000003FF
#define WLAN_PLL_CONTROL_REFDIV_LSB         10
#define WLAN_PLL_CONTROL_REFDIV_MASK        0x00003C00
#define WLAN_PLL_CONTROL_BYPASS_MASK        0x00010000
#define WLAN_PLL_CONTROL_NOPWD_MASK         0x00040000

#define RTC_SYNC_STATUS_OFFSET              0x00000244
#define RTC_SYNC_STATUS_PLL_CHANGING_MASK   0x00000020

/* Target RAM addresses of the PLL control block (qca6174_regs) */
#define QCA6174_CPU_PLL_INIT_ADDRESS        0x00404020
#define QCA6174_CPU_SPEED_ADDRESS           0x00404024
#define QCA6174_CORE_CLK_DIV_ADDRESS        0x00404028
#define QCA6174_TARGET_CPU_FREQ             176000000

/* Reference clock parameter table (ath10k qca6174_clk) */
#define QCA_REFCLK_COUNT                    8

struct qca_hw_clk_params {
    u32 refclk;
    u32 div;
    u32 rnfrac;
    u32 settleTime;
    u32 refdiv;
    u32 outdiv;
};

static const struct qca_hw_clk_params qca_refclk_params[QCA_REFCLK_COUNT] = {
    { 48000000, 0x0e, 0x2aaa8, 2400, 0, 1 },
    { 19200000, 0x24, 0x2aaa8,  960, 0, 1 },
    { 24000000, 0x1d, 0x15551, 1200, 0, 1 },
    { 26000000, 0x1b, 0x4ec4,  1300, 0, 1 },
    { 37400000, 0x12, 0x34b49, 1870, 0, 1 },
    { 38400000, 0x12, 0x15551, 1920, 0, 1 },
    { 40000000, 0x12, 0x26665, 2000, 0, 1 },
    { 52000000, 0x1b, 0x4ec4,  2600, 0, 1 },
};

/* Extract the field selected by a LSB/MASK pair (ath10k MS()). */
#define QCA_MS(x, mask) ((u32)((x) & (mask)) >> _qcaLsb(mask))

static inline int _qcaLsb(u32 mask)
{
    return mask ? __builtin_ctz(mask) : 0;
}

/* Host Interest structure (target RAM, addressed via BMI) */
#define QCA_HOST_INTEREST                   0x00400800
#define HI_APP_HOST_INTEREST_OFFSET         0x00000000
#define HI_OPTION_FLAG_OFFSET               0x00000010
#define HI_BE_OFFSET                        0x00000044
#define HI_BOARD_DATA_OFFSET                0x00000054
#define HI_BOARD_DATA_INITIALIZED_OFFSET    0x00000058
#define HI_FW_SWAP_OFFSET                   0x00000104

#define HTC_PROTOCOL_VERSION                0x00000002
#define HI_OPTION_NUM_DEV_SHIFT             0x9
#define HI_OPTION_FW_MODE_SHIFT             0xC
#define HI_OPTION_FW_MODE_AP                0x2
#define HI_OPTION_MAC_ADDR_METHOD_SHIFT     0x3

#endif /* _QCA_REGS_H_ */
