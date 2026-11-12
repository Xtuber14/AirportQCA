# Qualcomm Atheros QCA9377 Native macOS Driver Architecture & Research Specification

## 1. Executive Summary

This documentation repository contains the complete systems engineering analysis and architectural blueprint to develop a production-quality, native macOS driver suite for the **Qualcomm Atheros QCA9377** 802.11ac Wi-Fi and Bluetooth combo card mounted in this laptop.

The analysis is directly synthesized from the reference codebases provided in `source/`:
- **`source/ath/drivers/net/wireless/ath/ath10k`**: Linux kernel core driver for Atheros/Qualcomm 802.11ac chips (BMI, Copy Engine, HTC, WMI-TLV, HTT).
- **`source/ath/drivers/bluetooth`**: Linux Bluetooth subsystem (`btusb_main.c`, `btusb_qcom.c`, `btqca.c`) implementing Qualcomm Rome firmware upload.
- **`source/itlwm`**: OpenIntelWireless native macOS Wi-Fi driver (`AirportItlwm`, `AirportItlwmV2`, `AirportItlwmSkywalkInterface`), establishing the standard for native Apple AirPort emulation on third-party silicon.
- **`source/IntelBluetoothFirmware`**: OpenIntelWireless macOS USB firmware injector for third-party Bluetooth controllers.
- **`source/linux-firmware`**: Upstream binary firmware images (`ath10k/QCA9377/hw1.0/` and `qca/`).
- **`source/xnu`**: Apple Darwin XNU kernel and IOKit framework (`IOPCIDevice`, `IOWorkLoop`, `IOCommandGate`, `IOBufferMemoryDescriptor`, `IOMapper`).

---

## 2. Master Documentation Index

The technical research and engineering specifications are structured into six dedicated engineering reports located in this directory:

1. [**01. Hardware & Chipset Architecture**](file:///Users/xtuber/ath10k/docs/01_HARDWARE_AND_CHIPSET_ARCHITECTURE.md)
   - QCA9377 silicon overview: Single-die 1x1 802.11ac + BT 4.2/5.0 module.
   - Dual-bus topology: **PCIe x1** for Wi-Fi (`0x168C:0x0042`) and **USB 2.0** for Bluetooth (`0x0CF3:0xE300` / OEM variants).
   - Tensilica Xtensa LX4 32-bit RISC core operating at 176 MHz.
   - Register mappings (BAR0 MMIO, Local PCIe config, Reset registers, Diagnostic windows).
   - Architectural comparison between Broadcom, Intel, and Qualcomm Atheros.

2. [**02. Firmware Subsystem & Boot Sequence**](file:///Users/xtuber/ath10k/docs/02_FIRMWARE_SUBSYSTEM_AND_BOOT_SEQUENCE.md)
   - Wi-Fi binary assets: `firmware-6.bin` (TLV container), `board-2.bin`, and `board.bin` (8,124 bytes calibration BDF).
   - Wi-Fi Bootloader Messaging Interface (BMI) protocol over PCIe CE0/CE1.
   - Complete Wi-Fi target boot progression from cold reset to `FW_IND_INITIALIZED`.
   - Bluetooth DFU protocol: ROM version identification (`0x00000302`), Rampatch loading (`rampatch_usb_00000302.bin`), NVM parameter injection (`nvm_usb_00000302.bin`), and device handoff.

3. [**03. Copy Engine (CE) DMA, HTC and HTT Transport**](file:///Users/xtuber/ath10k/docs/03_COPY_ENGINE_DMA_AND_TRANSPORT_ARCHITECTURE.md)
   - Hardware Copy Engine circular DMA descriptor rings (`ce_desc`).
   - Complete 8-pipe allocation table and critical **QCA9377 CE5 override** (`ath10k_pci_override_ce_config`).
   - Host-Target Communication (HTC) framing and credit-based flow control.
   - Host-Target Transport (HTT) data plane framing (`htt_tx_desc`, `htt_rx_desc`, in-order indications).
   - AppleVTD (IOMMU) DMA coherence and mapping via `ItlDmaArena` pattern.

4. [**04. WMI-TLV Control Plane and 802.11 State Machine**](file:///Users/xtuber/ath10k/docs/04_WMI_TLV_CONTROL_PLANE_AND_STATE_MACHINE.md)
   - WMI-TLV message framing (Tag-Length-Value parser and serializer).
   - Driver-Firmware initialization handshake (`SERVICE_READY` -> `INIT_CMD` -> `READY_EVENT`).
   - Virtual Device (VDEV) lifecycle management (Create, Start, Stop, Delete).
   - Active/Passive scanning engine and Information Element (IE) ingestion.
   - Hardware AES-CCMP cryptographic key installation (PTK & GTK).
   - Hardware Wi-Fi/Bluetooth coexistence arbiter (`WMI_SERVICE_COEX_GPIO`).

5. [**05. macOS Kernel Integration and IOKit Architecture**](file:///Users/xtuber/ath10k/docs/05_MACOS_KERNEL_INTEGRATION_AND_IOKIT_ARCHITECTURE.md)
   - Native AirPort emulation strategy: Subclassing `IO80211Controller` / `IO80211ControllerV2`.
   - Full implementation of `apple80211Request` IOCTL dispatch table.
   - Delegating 802.1X / WPA2-PSK 4-way handshake to native Apple RSN Supplicant (`useAppleRSNSupplicant = true`).
   - Supporting modern macOS (Ventura, Sonoma, Sequoia) via Skywalk (`IO80211InfraProtocol`).
   - `IOWorkLoop`, `IOCommandGate`, and single-vector MSI interrupt synchronization.
   - Bluetooth companion driver (`QCABluetoothFirmware`) matching `IOUSBHostDevice` and handing off to native `IOBluetoothFamily`.

6. [**06. Engineering Implementation Roadmap and Verification Matrix**](file:///Users/xtuber/ath10k/docs/06_ENGINEERING_IMPLEMENTATION_ROADMAP_AND_VERIFICATION.md)
   - 8-phase developmental milestones from hardware bringup to sleep/wake recovery.
   - 12 comprehensive test cases (PCIe probe, BMI upload, HTC/WMI handshake, scan, association, data throughput, BT audio, coexistence, S3 sleep/wake).
   - Diagnostic and panic troubleshooting guide (AppleVTD faults, firmware crashes, HTC hangs).

---

## 3. High-Level Architectural Flowchart

```
+-------------------------------------------------------------------------------------------------+
|                                         macOS Userland                                          |
|                Wi-Fi Menu Bar | System Settings | AirDrop | CoreLocation | bluetoothd           |
+------------------------------------------------+------------------------------------------------+
                                                 |
                                                 v
+------------------------------------------------+------------------------------------------------+
|                                      macOS Kernel Frameworks                                    |
|                 IO80211Family.kext             |              IOBluetoothFamily.kext            |
+------------------------------------------------+------------------------------------------------+
                                                 |
                    +----------------------------+----------------------------+
                    |                                                         |
                    v (Wi-Fi: PCIe)                                           v (Bluetooth: USB)
+-------------------+----------------------------+      +---------------------+-------------------+
|               AirportQCA.kext                  |      |         QCABluetoothFirmware.kext       |
|                                                |      |                                         |
|  - Subclasses IO80211Controller / ControllerV2 |      |  - Matches IOUSBHostDevice              |
|  - Translates apple80211Request IOCTLs         |      |  - Checks ROM ver (0x00000302)          |
|  - Delegates crypto to Apple RSN Supplicant    |      |  - Uploads rampatch_usb_00000302.bin    |
|  - Manages Copy Engine DMA Rings (CE0 - CE7)   |      |  - Injects nvm_usb_00000302.bin         |
|  - Manages HTC & WMI-TLV State Machine         |      |  - Performs USB controller soft reset   |
|  - Boots firmware-6.bin & BDF via BMI          |      |  - Hands off to native Apple Bluetooth  |
+-------------------+----------------------------+      +---------------------+-------------------+
                    |                                                         |
                    v (PCIe BAR0 MMIO + MSI)                                  v (USB Control/Bulk)
+-------------------+---------------------------------------------------------+-------------------+
|                                  Qualcomm Atheros QCA9377 Chipset                               |
|                     Tensilica Xtensa LX4 CPU @ 176 MHz + Hardware Coexistence                   |
+-------------------------------------------------------------------------------------------------+
```

---

## 4. Key Engineering Discoveries & Conclusions

1. **Hardware Feasibility**:
   - The QCA9377 is fundamentally capable of delivering a 100% native macOS user experience without third-party connection helper applications (like HeliPort).
   - By inheriting the proven `AirportItlwm` architecture, the driver leverages macOS's native `corewland` for scanning, UI display, network selection, and WPA2/WPA3 4-way handshakes.

2. **Crucial Hardware Quirk Solved**:
   - In `ath10k`, QCA9377 requires a mandatory Copy Engine override: **CE5 must be disabled**, and **Service 15 (HTT Rx) must be routed to CE1**. Implementing standard QCA9880 pipe settings causes a fatal target lockup.

3. **AppleVTD Compatibility**:
   - Modern hackintoshes running macOS Ventura, Sonoma, or Sequoia with AppleVTD enabled require all DMA buffers to be allocated via `IOBufferMemoryDescriptor::inTaskWithPhysicalMask` and registered with `IOMapper::copyMapperForDevice`. This prevents DMAR IOMMU page faults.

4. **Bluetooth Independence**:
   - Because the Bluetooth core is completely separated on the USB bus, it does not require complex kernel networking code. A lightweight USB firmware loader modeled on `IntelBluetoothFirmware` brings the chip out of ROM mode, allowing Apple's native `IOBluetoothFamily` to drive it seamlessly.

All specifications, register offsets, TLV tags, descriptor layouts, and sequencing requirements are documented in the respective files linked above.
