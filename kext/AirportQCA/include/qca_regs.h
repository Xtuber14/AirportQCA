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

/* SOC core registers — offsets relative to SOC_CORE_BASE */
#define CORE_CTRL_ADDRESS                   0x00000000
#define PCIE_INTR_ENABLE_ADDRESS            0x00000008
#define PCIE_INTR_CAUSE_ADDRESS             0x0000000C
#define PCIE_INTR_CLR_ADDRESS               0x00000014

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

/* Scratch register 3 (ath10k qca6174_regs.scratch_3_address): the target
   uses it for firmware indications, so it is sampled as a diagnostic. */
#define SCRATCH_3_ADDRESS                   0x00000028
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

/* Target Firmware Indicator Register (absolute target address).
   ath10k maps the QCA9377 (device id 0x0042) onto qca6174_regs, whose
   fw_indicator_address is 0x3a028. */
#define FW_INDICATOR_ADDRESS                0x0003a028

/* Alternate indicator observed on this chip: it holds the QCA "alive"
   magic 0xdeadc0de. Only used for diagnostics until the authoritative
   indicator has been confirmed. */
#define FW_INDICATOR_ALT_ADDRESS            0x0000180C

#define FW_IND_EVENT_PENDING                0x00000001
#define FW_IND_INITIALIZED                  0x00000002

/* PCIe extended capability space. The Advanced Error Reporting (AER)
   capability describes every PCIe error the endpoint saw; its header log
   even holds the address of the offending transaction, which is the
   quickest way to find out what killed the target ROM. */
#define PCI_EXT_CAP_START                  0x00000100
#define PCI_EXT_CAP_END                    0x00000400
#define PCI_EXT_CAP_ID_AER                 0x0001
#define PCI_EXT_CAP_ID_L1PM_SUBSTATES      0x001e
#define PCI_EXT_CAP_NEXT_SHIFT             20
#define PCI_EXT_CAP_NEXT_MASK              0x00000FFF

/* AER capability register offsets (PCIe base specification 2.0). */
#define PCI_EXT_AER_UNCOR_STATUS           0x00000004
#define PCI_EXT_AER_UNCOR_MASK             0x00000008
#define PCI_EXT_AER_UNCOR_SEVERITY         0x0000000C
#define PCI_EXT_AER_COR_STATUS             0x00000010
#define PCI_EXT_AER_COR_MASK               0x00000014
#define PCI_EXT_AER_COR_SEVERITY           0x00000018
#define PCI_EXT_AER_STATUS_UPPER           0x0000001C
#define PCI_EXT_AER_ROOT_ERROR_COMMAND     0x0000002C
#define PCI_EXT_AER_ROOT_ERROR_STATUS      0x00000030
#define PCI_EXT_AER_HEADER_LOG             0x00000038

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
#define CE_WM_STATUS_OFFSET                 0x00000030
#define CE_MISC_IE_OFFSET                   0x00000034
#define CE_SRC_WR_INDEX_OFFSET              0x0000003C
#define CE_DST_WR_INDEX_OFFSET              0x00000040
#define CE_SRC_RD_INDEX_OFFSET              0x00000044
#define CE_DST_RD_INDEX_OFFSET              0x00000048

/* Copy Engine control register 1 (offset 0x10 inside a CE block).
   Bits [15:0] hold dmax (maximum transfer size), bit 16 enables
   source-ring byte swap and bit 17 destination-ring byte swap. ath10k
   clears both byte-swap bits; the register's reset value (0x80) has to
   survive the read-modify-write sequence. */
#define CE_CTRL1_RESET_VALUE                0x00000080
#define CE_CTRL1_DMAX_MASK                  0x0000FFFF
#define CE_CTRL1_SRC_BYTE_SWAP              0x00010000
#define CE_CTRL1_DST_BYTE_SWAP              0x00020000

/* Ring watermark registers: lowmark lives in bits [31:16], highmark in
   bits [15:0]. ath10k programs lowmark = 0 and highmark = the number of
   ring entries. */
#define CE_SRC_WM_OFFSET                    0x0000004C
#define CE_DST_WM_OFFSET                    0x00000050
#define CE_WM_HIGH_MASK                     0x0000FFFF
#define CE_WM_LOW_MASK                      0xFFFF0000

/* CE descriptor metadata field (ath10k qca6174_values: mask 0xFFFC at
   bit 2). BMI transfers use transfer id -1, i.e. all metadata bits set. */
#define CE_DESC_META_DATA_MASK              0x0000FFFC
#define CE_DESC_META_DATA_LSB               2
#define CE_DESC_META_DATA_BMI               0x0000FFFC

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

/* Additional host-interest fields used by the early configuration
   (ath10k targaddrs.h, struct host_interest). */
#define HI_OPTION_FLAG2_OFFSET              0x000000CC
#define HI_INTERCONNECT_STATE_OFFSET        0x000000F8
#define HI_EARLY_ALLOC_OFFSET               0x00000100

/* hi_option_flag2 options (ath10k targaddrs.h). */
#define HI_OPTION_EARLY_CFG_DONE            0x00000010

/* Early allocation (hi_early_alloc): the host tells the target that it
   supports the early allocation and how many IRAM banks it may use. */
#define HI_EARLY_ALLOC_MAGIC                0x00006D8A
#define HI_EARLY_ALLOC_MAGIC_MASK           0xFFFF0000
#define HI_EARLY_ALLOC_MAGIC_SHIFT          16
#define HI_EARLY_ALLOC_IRAM_BANKS_MASK      0x0000000F
#define HI_EARLY_ALLOC_IRAM_BANKS_SHIFT     0

/* Number of IRAM banks the QCA9377 target has
   (ath10k_pci_get_num_banks for QCA9377_1_0_DEVICE_ID). */
#define QCA9377_IRAM_BANKS                  9

/* struct pcie_state (ath10k pci.h): the target hands the host the
   addresses of the pipe configuration and of the service-to-pipe map. */
#define PCIE_STATE_PIPE_CFG_ADDR_OFFSET     0x00000000
#define PCIE_STATE_SVC_TO_PIPE_MAP_OFFSET   0x00000004
#define PCIE_STATE_CONFIG_FLAGS_OFFSET      0x00000020
#define PCIE_CONFIG_FLAG_ENABLE_L1          0x00000001

/* Pipe directions (ath10k ce.h). The directions are relative to the
   host, so PIPEDIR_OUT means host -> target. */
#define PIPEDIR_NONE                        0
#define PIPEDIR_IN                          1
#define PIPEDIR_OUT                         2
#define PIPEDIR_INOUT                       3

/* Target-side Copy Engine configuration (struct ce_pipe_config). The
   structure is shared between the host driver and the target
   firmware, so the field order must not change. */
struct qca_ce_pipe_config {
    u32 pipenum;
    u32 pipedir;
    u32 nentries;
    u32 nbytes_max;
    u32 flags;
    u32 reserved;
};

/* Service-to-pipe map (struct ce_service_to_pipe), also shared with
   the target firmware. */
struct qca_ce_service_to_pipe {
    u32 service_id;
    u32 pipedir;
    u32 pipenum;
};

/* HTC service ids (ath10k htc.h, SVC(group, idx) = group << 8 | idx). */
#define ATH10K_HTC_SVC_ID_RSVD_CTRL         0x0001
#define ATH10K_HTC_SVC_ID_WMI_CONTROL       0x0100
#define ATH10K_HTC_SVC_ID_WMI_DATA_BE       0x0101
#define ATH10K_HTC_SVC_ID_WMI_DATA_BK       0x0102
#define ATH10K_HTC_SVC_ID_WMI_DATA_VI       0x0103
#define ATH10K_HTC_SVC_ID_WMI_DATA_VO       0x0104
#define ATH10K_HTC_SVC_ID_HTT_DATA_MSG      0x0300
#define ATH10K_HTC_SVC_ID_TEST_RAW_STREAMS  0xFE00

/* Number of target Copy Engine configuration entries the QCA6174
   family expects (ath10k qca6174_values.num_target_ce_config_wlan). */
#define NUM_TARGET_CE_CONFIG_WLAN           7

/* Diagnostic window (CE7) transfer limit and pipe number
   (ath10k pci.h / pci.c). */
#define DIAG_TRANSFER_LIMIT                 2048
#define CE_DIAG_PIPE                        7

/* ath10k waits at most 10 ms for the diagnostic window to complete a
   transfer, polling every 50 us. */
#define DIAG_ACCESS_CE_WAIT_US              50
#define DIAG_ACCESS_CE_TIMEOUT_US           10000

#define HTC_PROTOCOL_VERSION                0x00000002
#define HI_OPTION_NUM_DEV_SHIFT             0x9
#define HI_OPTION_FW_MODE_SHIFT             0xC
#define HI_OPTION_FW_MODE_AP                0x2
#define HI_OPTION_MAC_ADDR_METHOD_SHIFT     0x3

#endif /* _QCA_REGS_H_ */
