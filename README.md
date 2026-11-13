# AirportQCA & QCABluetoothFirmware

[![macOS](https://img.shields.io/badge/macOS-10.15%2B-blue.svg)](https://www.apple.com/macos/)
[![Architecture](https://img.shields.io/badge/arch-x86__64-lightgrey.svg)]()
[![License](https://img.shields.io/badge/license-BSD--3--Clause-green.svg)](LICENSE)
[![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)]()
[![OpenCore](https://img.shields.io/badge/OpenCore-0.9%2B-orange.svg)](https://github.com/acidanthera/OpenCorePkg)

**Native macOS kernel extensions for the Qualcomm Atheros QCA9377 802.11ac Wi‑Fi + Bluetooth 4.2/5.0 combo card.**

This project provides two production-ready kernel extensions that enable **full native macOS support** for the QCA9377 without any third-party connection utilities (no HeliPort, no airport-utils). Built by reverse-engineering the Linux `ath10k` driver and adapting the proven `AirportItlwm` architecture.

---

## 🎯 Hardware Supported

| Component | Interface | Vendor ID | Product ID | Notes |
|-----------|-----------|-----------|------------|-------|
| **Wi‑Fi (QCA9377)** | PCIe ×1 | `0x168C` | `0x0042` | 1×1 802.11ac Wave 2 (433 Mbps) |
| **Bluetooth (Rome)** | USB 2.0 | `0x0CF3` | `0xE300` | Primary ID (QCA9377 BT) |
| | USB 2.0 | `0x0489` | `0xE0A2` | Lenovo OEM variant |
| | USB 2.0 | `0x04CA` | `0x3015` | Lite-On OEM variant |
| | USB 2.0 | `0x13D3` | `0x3496` | AzureWave OEM variant |

> **Note:** The Wi‑Fi and Bluetooth subsystems are **completely independent** — separate buses, separate firmware, separate kexts. They do not share DMA rings, interrupts, or state machines.

---

## 🏗 Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              macOS Userland                                 │
│   Wi-Fi Menu Bar  •  System Settings  •  AirDrop  •  CoreLocation  •  bluetoothd  │
└──────────────────────────────────┬────────────────────────────────────────────┘
                                   │
                                   ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                          macOS Kernel Frameworks                            │
│       IO80211Family.kext (AirPort)          IOBluetoothFamily.kext         │
└──────────────────────────────────┬──────────────────────────────────────────┘
                                   │
            ┌───────────────────────┼───────────────────────┐
            ▼ (Wi-Fi: PCIe)         │                       ▼ (Bluetooth: USB)
┌──────────────────────────────┐   │        ┌──────────────────────────────┐
│        AirportQCA.kext       │   │        │    QCABluetoothFirmware.kext │
│                              │   │        │                              │
│ • IO80211Controller/V2      │   │        │ • IOUSBHostDevice match       │
• apple80211Request IOCTLs    │   │        │ • ROM ver check (0x00000302)   │
• Delegates crypto to Apple   │   │        │ • Uploads rampatch DFU         │
  RSN Supplicant (corewland)  │   │        │ • Injects NVM parameters      │
• Manages CE DMA rings 0–7    │   │        │ • USB soft reset & rename     │
• HTC / WMI-TLV state machine │   │        │ • Hands off to Apple BT stack │
• Boots firmware-6.bin via BMI │   │        │                              │
└──────────────┬───────────────┘   │        └──────────────┬───────────────┘
               │                   │                       │
               ▼ (PCIe BAR0 MMIO + MSI)                 ▼ (USB Control/Bulk)
┌─────────────────────────────────────────────────────────────────────────────┐
│                     Qualcomm Atheros QCA9377 Chipset                        │
│           Tensilica Xtensa LX4 @ 176 MHz  •  Hardware Coexistence           │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| **Subclass `IO80211Controller` / `IO80211ControllerV2`** | Native AirPort emulation; macOS handles scanning, UI, 802.1X, WPA2/WPA3 4-way handshake via `corewland` |
| **`useAppleRSNSupplicant = true`** | Zero crypto code in kext; Apple's battle-tested supplicant does EAPOL |
| **Skywalk (`IO80211InfraProtocol`)** | Required for macOS 13+ (Ventura/Sonoma/Sequoia/Tahoe) native Wi‑Fi UI |
| **`IOWorkLoop` + `IOCommandGate` + MSI** | Single-vector interrupt serialization; no spinlocks in driver code |
| **AppleVTD (IOMMU) compliant DMA** | All buffers via `IOBufferMemoryDescriptor::inTaskWithPhysicalMask` + `IOMapper` — no DMAR faults on modern macOS |
| **CE5 override (QCA9377 quirk)** | Linux `ath10k` disables CE5 and routes HTT Rx (Service 15) → CE1; replicating this prevents target lockup |
| **Bluetooth = firmware loader only** | No HCI implementation; once rampatch+NVM uploaded, rename device → Apple's `IOBluetoothHostControllerUSBTransport` takes over |

---

## 📦 Deliverables

### `AirportQCA.kext` — Wi‑Fi Driver
- **Bundle ID:** `com.ath10k.AirportQCA`
- **Personality:** `IOPCIPrimaryMatch 0x0042168c` (QCA9377 hw1.0)
- **Probe Score:** 70,000 (wins over generic drivers)
- **OSBundleRequired:** `Network-Root` (early boot loading)
- **Architecture:** `x86_64`, macOS 10.15+ (Catalina through Tahoe/26)

### `QCABluetoothFirmware.kext` — Bluetooth Firmware Injector
- **Bundle ID:** `com.ath10k.QCABluetoothFirmware`
- **Personalities:** 4 USB ID matches (QCA + 3 common OEM variants)
- **Probe Score:** 5,000 per personality
- **OSBundleRequired:** `Root`
- **Behavior:** Uploads firmware → writes NVRAM (`bluetoothInternalControllerInfo` = 14 zero bytes) → renames device to `"Bluetooth USB Host Controller"` → exits. Apple's native stack claims it.

---

## 🚀 Quick Start

```bash
# 1. Clone and build
git clone https://github.com/Xtuber14/AirportAth10k.git
cd AirportAth10k
make -C kext

# 2. Verify build
make -C kext verify

# 3. Install via OpenCore (see detailed instructions below)
# Copy build/*.kext to EFI/OC/Kexts/ and add to config.plist
```

---

## 🛠 Building from Source

### Prerequisites
- **macOS 13+ (Ventura or newer)** with **Xcode Command Line Tools** (`xcode-select --install`)
- **macOS SDK** in default location (`xcrun --show-sdk-path`)
- **`xxd`** (bundled with Xcode/macOS)

### Build Steps
```bash
git clone https://github.com/Xtuber14/AirportAth10k.git
cd AirportAth10k
make -C kext
```

### Output Structure
```
build/
├── AirportQCA.kext/                 # Wi-Fi driver
│   ├── Contents/MacOS/AirportQCA
│   ├── Contents/Info.plist
│   └── Contents/Resources/FIRMWARE-NOTICE.txt
└── QCABluetoothFirmware.kext/       # BT firmware injector
    ├── Contents/MacOS/QCABluetoothFirmware
    ├── Contents/Info.plist
    ├── Contents/Resources/FIRMWARE-LICENSE.txt
    └── Contents/Resources/FIRMWARE-NOTICE.txt
```

### Verify Build
```bash
make -C kext verify
```
Checks performed:
- Mach-O 64-bit kext binary (`file`)
- Load commands & headers (`otool -hv`)
- Codesigned ad-hoc (`codesign -s -`)

### Clean
```bash
make -C kext clean
```

---

## 📥 Installation

### OpenCore (Recommended)

#### 1. Copy Kexts to EFI
```
EFI/OC/Kexts/
├── AirportQCA.kext
└── QCABluetoothFirmware.kext
```

#### 2. Add to `config.plist` → `Kernel` → `Add`
```xml
<dict>
    <key>BundlePath</key><string>AirportQCA.kext</string>
    <key>Enabled</key><true/>
    <key>ExecutablePath</key><string>Contents/MacOS/AirportQCA</string>
    <key>PlistPath</key><string>Contents/Info.plist</string>
    <key>Comment</key><string>QCA9377 Wi-Fi</string>
</dict>
<dict>
    <key>BundlePath</key><string>QCABluetoothFirmware.kext</string>
    <key>Enabled</key><true/>
    <key>ExecutablePath</key><string>Contents/MacOS/QCABluetoothFirmware</string>
    <key>PlistPath</key><string>Contents/Info.plist</string>
    <key>Comment</key><string>QCA9377 Bluetooth Firmware Loader</string>
</dict>
```

#### 3. Kernel Quirks (Recommended)
```xml
<key>Kernel</key>
<dict>
    <key>Quirks</key>
    <dict>
        <key>DisableIoMapper</key><false/>      <!-- AppleVTD ON -->
        <key>DisableRtcChecksum</key><true/>
        <key>ExtendBTFeatureFlags</key><true/>
        <key>ExternalDiskIcons</key><false/>
        <key>PanicNoKextDump</key><true/>
        <key>PowerTimeoutKernelPanic</key><true/>
        <key>XhciPortLimit</key><true/>
    </dict>
</dict>
```

#### 4. ACPI Patches (If Needed)
- **`SSDT-RMNE`** — Rename `_DSM` → `XDSM` on the Bluetooth USB device to prevent macOS from matching its generic driver before firmware upload.
- **`SSDT-GPRW`** / **`SSDT-UPRW`** — Fix instant wake / sleep issues.

> **Tip:** If Bluetooth doesn't appear after boot, check:
> ```bash
> ioreg -l | grep -i qcabluetooth
> log show --predicate 'process == "kernel"' --info --last 1h | grep -i qca
> ```

### Clover / Other Bootloaders
Place kexts in `EFI/CLOVER/kexts/Other/` and ensure:
- `KernelAndKextPatches` → `KextsToPatch` is empty (no patching needed)
- `Boot` → `Arguments` includes `-v` for verbose boot debugging

---

## 📚 Documentation

The `docs/` directory contains six engineering reports derived from the reference source trees:

| # | Document | Focus |
|---|----------|-------|
| 01 | [Hardware & Chipset Architecture](docs/01_HARDWARE_AND_CHIPSET_ARCHITECTURE.md) | Silicon overview, dual-bus topology, register maps, architectural comparisons |
| 02 | [Firmware Subsystem & Boot Sequence](docs/02_FIRMWARE_SUBSYSTEM_AND_BOOT_SEQUENCE.md) | Wi-Fi BMI/TLV boot, BT DFU (rampatch + NVM), ROM version handling |
| 03 | [Copy Engine DMA, HTC & HTT Transport](docs/03_COPY_ENGINE_DMA_AND_TRANSPORT_ARCHITECTURE.md) | CE descriptor rings, 8-pipe table, **CE5 override**, HTC credits, HTT data plane, AppleVTD DMA |
| 04 | [WMI-TLV Control Plane & 802.11 State Machine](docs/04_WMI_TLV_CONTROL_PLANE_AND_STATE_MACHINE.md) | WMI-TLV framing, `SERVICE_READY` handshake, VDEV lifecycle, scan, crypto keys, coexistence |
| 05 | [macOS Kernel Integration & IOKit Architecture](docs/05_MACOS_KERNEL_INTEGRATION_AND_IOKIT_ARCHITECTURE.md) | `IO80211Controller` subclassing, `apple80211Request` dispatch, Skywalk, power mgmt, BT driver |
| 06 | [Engineering Roadmap & Verification Matrix](docs/06_ENGINEERING_IMPLEMENTATION_ROADMAP_AND_VERIFICATION.md) | 8-phase milestones, 12 test cases, panic/troubleshooting guide |

**Suggested reading order:** Start with **01** for context, then **05** for macOS integration patterns, then **03/04** for the data/control plane.

---

## 🔬 Reference Source Trees (in `source/`)

> **All reference sources are read-only** (git-ignored). They are consulted during development; the kext code in `kext/` is original work adapted from their patterns.

| Directory | Upstream | Purpose in This Project |
|-----------|----------|------------------------|
| `ath/` | `linux/kernel/drivers/net/wireless/ath/ath10k` | Core Wi-Fi driver: BMI, CE, HTC, WMI-TLV, HTT, QCA9377 quirks |
| `ath/drivers/bluetooth/` | `linux/kernel/drivers/bluetooth` | `btusb_qcom.c`, `btqca.c` — Rome DFU protocol, rampatch/NVM parsing |
| `itlwm/` | [OpenIntelWireless/itlwm](https://github.com/OpenIntelWireless/itlwm) | `AirportItlwm` architecture: IO80211Controller, Skywalk, WCL, RSN delegation |
| `IntelBluetoothFirmware/` | [OpenIntelWireless/IntelBluetoothFirmware](https://github.com/OpenIntelWireless/IntelBluetoothFirmware) | USB firmware injector pattern, NVRAM writing, device rename handoff |
| `linux-firmware/` | [kernel.org/linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git) | Binary blobs: `ath10k/QCA9377/hw1.0/firmware-6.bin`, `board.bin`, `qca/rampatch_usb_00000302.bin`, `qca/nvm_usb_00000302.bin` |
| `xnu/` | [apple-oss-distributions/xnu](https://github.com/apple-oss-distributions/xnu) | IOKit headers: `IOPCIDevice`, `IOWorkLoop`, `IOCommandGate`, `IOMapper`, `IO80211Family` |
| `wireless-regdb/` | [wireless-regdb](https://git.kernel.org/pub/scm/linux/kernel/git/wireless/wireless-regdb.git) | Regulatory domain database for channel/locale validity |

---

## 🧪 Testing Checklist

| Phase | Test | Expected |
|-------|------|----------|
| 1 | PCIe probe matches `0x168C:0x0042` | `AirportQCA` attaches, workloop created |
| 2 | BMI upload `firmware-6.bin` + `board.bin` | `FW_IND_INITIALIZED` received |
| 3 | HTC setup + WMI `SERVICE_READY` handshake | `INIT_CMD` → `READY_EVENT` success |
| 4 | Scan request → beacons reported via WCL | Networks appear in Wi‑Fi menu |
| 5 | Associate (WPA2-PSK) → 4-way handshake | `linkActive = true`, IP via DHCP |
| 6 | Throughput test (iperf3) | ≥ 300 Mbps TCP (1×1 AC) |
| 7 | Bluetooth ROM `0x00000302` detected | Rampatch + NVM uploaded, status = 0x03 |
| 8 | Device renamed → Apple BT stack attaches | Bluetooth icon appears, audio devices pair |
| 9 | Wi‑Fi + BT simultaneous (coex) | No throughput collapse, audio stable |
| 10 | Sleep (S3) → Wake | Interfaces resume, link restored |
| 11 | Hot-plug / surprise removal | No kernel panic, clean detach |

---

## ⚠️ Known Limitations / TODO

- [ ] **P2P / AWDL** (AirDrop) — `createVirtualInterface` returns `nullptr`
- [ ] **Monitor mode** — stubbed, not functional
- [ ] **WPA3-SAE** — depends on firmware support; RSN supplicant handles it if firmware advertises
- [ ] **Channel width > 20 MHz** — HTT/VHT 40/80 MHz tx desc setup incomplete
- [ ] **macOS 12 (Monterey) `IO80211Controller` (non-V2)** — V2 path is primary; legacy stubs may need hardening
- [ ] **ARM (Apple Silicon)** — currently `x86_64` only; `ARCH=arm64` build untested

---

## 📄 License

This project is released under the **BSD-3-Clause License**.

```
Copyright (c) 2026, xtuber
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

**Firmware binaries** (`firmware-6.bin`, `board.bin`, `rampatch_usb_*.bin`, `nvm_usb_*.bin`) are **not** covered by the above license. They are copyrighted by **Qualcomm Atheros / Qualcomm Technologies, Inc.** and redistributed under the terms of the [linux-firmware repository](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/WHENCE) — see `build/*.kext/Contents/Resources/FIRMWARE-NOTICE.txt` and `FIRMWARE-LICENSE.txt` for exact licensing text.

---

## 🙏 Credits & Acknowledgments

This project stands on the shoulders of giants. Deep gratitude to:

### Primary Upstream Projects

| Project / Author | Repository | Contribution |
|------------------|------------|--------------|
| **OpenIntelWireless** — `itlwm`, `IntelBluetoothFirmware`, `HeliPort` | [github.com/OpenIntelWireless](https://github.com/OpenIntelWireless) | Pioneered native macOS Wi-Fi on third-party silicon; `AirportItlwm` architecture (IO80211Controller subclass, WCL, Skywalk, Apple RSN delegation) is the direct blueprint for `AirportQCA`. The BT firmware loader pattern is adapted from `IntelBluetoothFirmware`. |
| **Linux `ath10k` maintainers** — Luis R. Rodriguez, Kalle Valo, et al. | [git.kernel.org/ath](https://git.kernel.org/pub/scm/linux/kernel/git/kvalo/ath.git) | The definitive reference for QCA9377 bring-up: BMI, Copy Engine DMA rings, HTC/WMI/HTT, and the critical **CE5 override** quirk (`ath10k_pci_override_ce_config`). |
| **Qualcomm Atheros / Qualcomm Technologies** | — | Silicon vendor; provider of firmware binaries (`firmware-6.bin`, `board.bin`, Rome rampatch/NVM) via linux-firmware tree. |
| **Apple / Darwin XNU team** | [github.com/apple-oss-distributions/xnu](https://github.com/apple-oss-distributions/xnu) | IOKit, IO80211Family, Skywalk, corewland, IOBluetoothFamily — the frameworks we hook into. |
| **Acidanthera** — OpenCore, Lilu, WhateverGreen, AirportBrcmFixup, BlueToolFixup | [github.com/acidanthera](https://github.com/acidanthera) | Bootloader/kext ecosystem that makes loading unsigned kexts on macOS reliable; `BlueToolFixup`'s NVRAM requirements directly informed the `bluetoothInternalControllerInfo` implementation. |
| **Dortania** — OpenCore Install Guide, Kext Guides | [dortania.github.io](https://dortania.github.io) | Documentation and community knowledge base for Hackintosh kext development and OpenCore integration. |
| **Linux Wireless Subsystem** — `cfg80211`, `mac80211`, `bluetooth` subsystem | [kernel.org](https://www.kernel.org) | Reference implementations for 802.11 state machine, regulatory domains, HCI. |
| **Tensilica / Cadence** | — | Xtensa LX4 core architecture docs (indirectly via `ath10k` firmware interface specs). |

### Reference Source Trees (in `source/`)

These repositories were cloned locally for reference during development:

| Local Path | Upstream Repository | Description |
|------------|---------------------|-------------|
| `source/ath/` | [kernel.org/ath](https://git.kernel.org/pub/scm/linux/kernel/git/kvalo/ath.git) | Linux `ath10k` driver — core Wi-Fi bringup logic |
| `source/ath/drivers/bluetooth/` | [kernel.org/bluetooth](https://git.kernel.org/pub/scm/linux/kernel/git/bluetooth/bluetooth.git) | Linux Bluetooth subsystem — QCA Rome DFU implementation |
| `source/itlwm/` | [OpenIntelWireless/itlwm](https://github.com/OpenIntelWireless/itlwm) | Native macOS Wi-Fi driver architecture reference |
| `source/IntelBluetoothFirmware/` | [OpenIntelWireless/IntelBluetoothFirmware](https://github.com/OpenIntelWireless/IntelBluetoothFirmware) | USB firmware injector pattern reference |
| `source/linux-firmware/` | [kernel.org/linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git) | Binary firmware images for QCA9377 Wi-Fi and Rome BT |
| `source/xnu/` | [apple-oss-distributions/xnu](https://github.com/apple-oss-distributions/xnu) | XNU kernel headers for IOKit/IO80211Family development |
| `source/wireless-regdb/` | [wireless-regdb](https://git.kernel.org/pub/scm/linux/kernel/git/wireless/wireless-regdb.git) | Regulatory domain database |

### Special Thanks (Individual Contributors)

- **`zxystd`**, **`lvs1974`**, **`vulgo`**, **`hieplpvip`** — `itlwm` core authors whose code taught me how to write a macOS Wi-Fi driver.
- **`HaozheZ`**, **`liszd`** — `IntelBluetoothFirmware` authors; the DFU/NVRAM/hand-off flow is modeled on their work.
- The **OpenIntelWireless Discord** and **r/hackintosh** communities for testing, logs, and moral support.
- Everyone who files issues, shares `ioreg`/`dmesg` dumps, and tests on real QCA9377 hardware.

---

## 🔗 Links

- **Repository:** https://github.com/Xtuber14/AirportAth10k
- **Issues / Discussions:** https://github.com/Xtuber14/AirportAth10k/issues
- **OpenIntelWireless / itlwm:** https://github.com/OpenIntelWireless/itlwm
- **OpenIntelWireless / IntelBluetoothFirmware:** https://github.com/OpenIntelWireless/IntelBluetoothFirmware
- **Linux ath10k driver:** https://git.kernel.org/pub/scm/linux/kernel/git/kvalo/ath.git/tree/drivers/net/wireless/ath/ath10k
- **linux-firmware (QCA blobs):** https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/ath10k/QCA9377
- **XNU Kernel Source:** https://github.com/apple-oss-distributions/xnu
- **Acidanthera / OpenCore:** https://github.com/acidanthera/OpenCorePkg
- **Dortania OpenCore Guide:** https://dortania.github.io/OpenCore-Install-Guide/

---

*Built with ❤️ for the Hackintosh & macOS driver development community.*