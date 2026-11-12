# 01. Qualcomm Atheros QCA9377 Hardware and Chipset Architecture

## 1. Executive Summary & Hardware Overview

The **Qualcomm Atheros QCA9377** (often designated in Linux and internal Atheros nomenclature as part of the **Rome** family, closely related to the QCA6174) is a highly integrated single-die solution combining:
- **Wi-Fi**: 1x1 IEEE 802.11a/b/g/n/ac Single-Stream Wave-2 MU-MIMO WLAN supporting 20/40/80 MHz channel bandwidths up to 433.3 Mbps PHY rate.
- **Bluetooth**: Bluetooth 4.2 / 5.0 Dual Mode (BR/EDR + BLE) subsystem with integrated Wideband Speech (eSCO) and coexistence engine.

In standard laptop implementations (such as the target laptop where this card is mounted in an M.2 2230 Key-E or solder-down module), the module exposes **two completely distinct hardware transport buses** to the host:
1. **PCIe (PCI Express 1.1 / 2.0 x1)**: Dedicated exclusively to the **Wi-Fi** subsystem.
2. **USB 2.0 (High-Speed / Full-Speed)**: Dedicated exclusively to the **Bluetooth** subsystem.

To achieve native-like operation in macOS (analogous to Apple's native Broadcom AirPort or OpenIntelWireless `AirportItlwm` + `IntelBluetoothFirmware`), two distinct kernel extensions or modular subsystems are required:
- An **IO80211 / IOPCIDevice** network kext for Wi-Fi (`AirportQCA` or equivalent).
- An **IOUSBHostDevice** firmware injection kext for Bluetooth (`QCABluetoothFirmware` or equivalent).

```
                      +-------------------------------------------------+
                      |              QCA9377 Combo Module               |
                      |                                                 |
  PCIe Link           |  +-------------------------------------------+  |
 (Host PCIe Root) ====>  |              Wi-Fi Subsystem              |  |
                      |  | - PCIe Endpoint (Device 0x0042)           |  |
                      |  | - Xtensa LX4 Microcontroller              |  |
                      |  | - Copy Engine (CE) DMA Ring Engine        |  |
                      |  | - 802.11ac MAC / Baseband / Radio (1T1R)  |  |
                      |  +---------------------+---------------------+  |
                      |                        |                        |
                      |              Hardware Coexistence               |
                      |                        |                        |
  USB D+/D-           |  +---------------------+---------------------+  |
 (Host USB Root) =====>  |            Bluetooth Subsystem            |  |
                      |  | - USB Device Controller (HCI over USB)    |  |
                      |  | - ROM Bootloader / Patch RAM Execution    |  |
                      |  | - BR/EDR / LE 4.2/5.0 Radio Subsystem     |  |
                      |  +-------------------------------------------+  |
                      +-------------------------------------------------+
```

---

## 2. Wi-Fi Subsystem: Hardware Specifications & Registers

### 2.1 Identification & Topology
- **PCI Vendor ID**: `0x168C` (Qualcomm Atheros)
- **PCI Device ID**: `0x0042` (`QCA9377_1_0_DEVICE_ID` in `ath10k`)
- **PCI Class / Subclass**: `0x028000` (Network Controller - Other / Wireless)
- **PCI Revision IDs**:
  - `QCA9377_HW_1_0_CHIP_ID_REV` = `0x0`
  - `QCA9377_HW_1_1_CHIP_ID_REV` = `0x1`
- **PCI Capabilities**:
  - Power Management Interface (`PCI_CAP_ID_PM = 0x01`): Supports D0, D3hot, D3cold.
  - MSI (Message Signalled Interrupts, `PCI_CAP_ID_MSI = 0x05`): Single vector MSI supported.
  - Express Endpoint (`PCI_CAP_ID_EXP = 0x10`): PCIe v2.0 endpoint.
  - MSI-X (`PCI_CAP_ID_MSIX = 0x11`): Present on some board revisions but disabled by driver in favor of standard MSI.

### 2.2 Memory Mapped I/O (MMIO) BAR Structure
The QCA9377 PCI interface exposes **BAR 0** (32-bit Memory mapped, non-prefetchable, typically 2 MB in size). Through BAR 0, the host accesses:
1. **PCIe Local Control Registers**:
   - `SOC_CORE_BASE_ADDRESS` (`0x00000000` - `0x00000FFF`): PCIe local config space, link control, interrupt enable.
   - `PCIE_INTR_ENABLE_ADDRESS`: Controls which hardware interrupts reach the host.
   - `PCIE_INTR_CAUSE_ADDRESS`: Hardware interrupt status register.
   - `CORE_CTRL_ADDRESS`: Controls PCIe configuration register 31 mask, clock gating, and warm reset triggers.
2. **Copy Engine (CE) Registers**:
   - Spanning from offset `0x00000C00` (CE0) up to CE7.
   - Controls ring base pointers, ring sizes, read/write ring indices, and CE interrupt masks.
3. **Diagnostic Window / RTC Window**:
   - AXI window used for direct SoC register access and SRAM reading/writing during the bootloader (BMI) phase.
   - `RTC_SOC_BASE_ADDRESS` (`0x00000000` on internal bus, mapped through window).
   - `SOC_CHIP_ID_ADDRESS`: Target Chip identification word.
   - `FW_INDICATOR_ADDRESS`: Status register written by firmware during initialization.

### 2.3 Internal SoC Core
- **Microcontroller**: Tensilica Xtensa LX4 32-bit RISC core operating at **176 MHz** (`target_cpu_freq = 176000000`).
- **Internal Memory**:
  - ROM: Contains the initial factory bootloader (implements BMI - Bootloader Messaging Interface).
  - SRAM: Fast on-chip instruction and data memory used to load firmware overlays, calibration tables, and execution buffers.
  - DRAM (Internal / Shared): Target system memory used by the firmware RTOS.

---

## 3. Bluetooth Subsystem: Hardware Specifications & USB IDs

The Bluetooth controller is an integrated Qualcomm Rome core wired internally to the laptop's USB 2.0 bus (routed through motherboard M.2 slot pin 36 / 38: USB D- / D+).

### 3.1 Known USB Vendor & Product IDs
The QCA9377 Bluetooth core appears under several OEM vendor IDs depending on module manufacturer:

| Vendor | VID | PID | Description |
| :--- | :--- | :--- | :--- |
| **Qualcomm Atheros** | `0x0CF3` | `0xE300` | Generic QCA9377 / QCA6174 Bluetooth |
| **Qualcomm Atheros** | `0x0CF3` | `0xE007` | QCA Rome Bluetooth 4.2 |
| **Qualcomm Atheros** | `0x0CF3` | `0xE009` | QCA Rome Bluetooth |
| **Qualcomm Atheros** | `0x0CF3` | `0xE360` | QCA9377 Bluetooth |
| **Qualcomm Atheros** | `0x0CF3` | `0xE500` | QCA9377 Bluetooth 5.0 |
| **Foxconn / Hon Hai**| `0x0489` | `0xE092` | Foxconn QCA9377 |
| **Foxconn / Hon Hai**| `0x0489` | `0xE09F` | Foxconn QCA9377 |
| **Foxconn / Hon Hai**| `0x0489` | `0xE0A2` | Foxconn QCA9377 |
| **Lite-On**          | `0x04CA` | `0x3015` | Lite-On QCA9377 |
| **Lite-On**          | `0x04CA` | `0x3016` | Lite-On QCA9377 |
| **Lite-On**          | `0x04CA` | `0x301A` | Lite-On QCA9377 |
| **Lite-On**          | `0x04CA` | `0x3021` | Lite-On QCA9377 |
| **AzureWave**        | `0x13D3` | `0x3491` | AzureWave AW-CM251NF |
| **AzureWave**        | `0x13D3` | `0x3496` | AzureWave AW-CM251NF (QCA9377) |
| **AzureWave**        | `0x13D3` | `0x3501` | AzureWave QCA9377 |
| **AzureWave**        | `0x13D3` | `0x3503` | AzureWave QCA9377 |

### 3.2 USB Interface & Endpoint Architecture
When plugged into USB, the unpatched hardware exposes standard USB device descriptors:
- **Device Class**: `0xE0` (Wireless Controller)
- **Device Subclass**: `0x01` (RF Controller)
- **Device Protocol**: `0x01` (Bluetooth Programming Interface)
- **Endpoints**:
  - `Endpoint 0` (Control): Default pipe for standard requests and vendor-specific firmware download commands (`QCA_DFU_DOWNLOAD = 0x01`, `QCA_GET_TARGET_VERSION = 0x05`, `QCA_CHECK_STATUS = 0x04`).
  - `Endpoint 1 IN` (Interrupt, EP1 IN): HCI Event delivery.
  - `Endpoint 2 OUT` (Bulk, EP2 OUT): HCI ACL Data transmission & Rampatch body streaming.
  - `Endpoint 2 IN` (Bulk, EP2 IN): HCI ACL Data reception.
  - `Endpoint 3 OUT/IN` (Isochronous, EP3): HCI SCO audio voice data.

Before firmware download, the controller has raw unpatched ROM. It cannot function as a reliable Bluetooth host controller under macOS until `rampatch` and `nvm` are injected into target memory.

---

## 4. Hardware Power States, Reset Sequences & Clocking

### 4.1 Clocking Architecture
- **Reference Clock**: 40 MHz external crystal oscillator.
- **Internal PLL**: Boosts internal core clock from 40 MHz reference clock to 176 MHz for the Xtensa processor.
- **Clock Skipping Feature**: During early bootloader initialization, `hi_skip_clock_init` can be set to allow SoC to run on `REF_CLK` until the target firmware initializes its internal clock tree, preventing PCIe bus dropouts on certain motherboard PLL configurations.

### 4.2 Reset Sequences in Hardware
The chip supports two reset topologies:
1. **PCIe Warm Reset (`ath10k_pci_warm_reset`)**:
   - Software writes to `CORE_CTRL_ADDRESS` via `SOC_CORE_BASE_ADDRESS` toggling `CORE_CTRL_PCIE_REG_31_MASK`.
   - Clears outstanding bus requests, flushes pending DMA, but keeps the PCIe link training alive with the host root port.
2. **PCIe Cold / Chip Reset (`ath10k_pci_qca6174_chip_reset`)**:
   - Writes to `SOC_GLOBAL_RESET_ADDRESS` (`val |= 1`).
   - Puts all internal digital logic (including CPU and internal buses) into hardware reset.
   - Requires a strict delay (`mdelay(10)`) followed by polling `FW_INDICATOR_ADDRESS` until the target ROM signals `FW_IND_INITIALIZED` (value `0x00000001` or `0x00000002`).

---

## 5. Architectural Comparison: QCA9377 vs. Intel vs. Broadcom

| Architectural Feature | Broadcom (Apple Native) | Intel (itlwm / AirportItlwm) | Qualcomm Atheros QCA9377 (Target) |
| :--- | :--- | :--- | :--- |
| **Host Interface** | PCIe x1 / x2 | PCIe x1 | PCIe x1 (Wi-Fi) + USB 2.0 (BT) |
| **On-Chip Processor** | ARM Cortex-R4 / proprietary | Tensilica Xtensa | Tensilica Xtensa LX4 @ 176 MHz |
| **Host-Target Messaging** | Direct register/ring queues | Ring queues + IWLM commands | **Copy Engine (CE) + HTC + WMI-TLV** |
| **Firmware Protocol** | Monolithic Apple driver firmware | iwlwifi API microcode | **Modular BMI (Boot) + WMI (Control) + HTT (Data)** |
| **Data Plane** | Native DMA Rings (D2H, H2D) | TX/RX Ring Buffers | **HTT (Host Target Transport) over CE** |
| **macOS Native Driver** | `AirPortBrcmNIC.kext` | None (OpenIntelWireless emulates) | None (Requires `AirportQCA` emulation) |
| **macOS Supplicant** | Native Apple RSN Supplicant | Native Apple RSN Supplicant | Can leverage Apple RSN Supplicant |
| **Bluetooth Architecture** | Broadcom UART / USB HCI | Intel USB HCI + Firmware Patch | **Qualcomm Rome USB HCI + Rampatch + NVM** |

The following documents establish the exact software and protocol engineering needed to realize this architecture under macOS.
