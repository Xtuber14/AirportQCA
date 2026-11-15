# AirportQCA — Native macOS Driver for Qualcomm Atheros QCA9377

![macOS](https://img.shields.io/badge/macOS-10.15%2B-blue) ![Architecture](https://img.shields.io/badge/arch-x86_64-lightgrey) ![Build](https://img.shields.io/badge/build-passing-brightgreen) ![License](https://img.shields.io/badge/license-MIT-blue)

A production-quality, native macOS kernel extension (kext) suite that provides **full Wi-Fi and Bluetooth support** for the Qualcomm Atheros QCA9377 802.11ac + Bluetooth 4.2/5.0 combo chipset. Built by reverse-engineering the Linux `ath10k` driver and following the proven architecture of OpenIntelWireless's `AirportItlwm`.

---

## 🎯 What This Project Does

This project delivers two kernel extensions that work together to bring **native macOS Wi-Fi and Bluetooth** to laptops equipped with the QCA9377:

| Kext | Function | Bus | Device IDs |
|------|----------|-----|------------|
| **AirportQCA.kext** | Full 802.11ac Wi-Fi driver implementing `IO80211Controller` / `IO80211ControllerV2` | PCIe x1 | `0x168C:0x0042` |
| **QCABluetoothFirmware.kext** | USB firmware injector for QCA Rome Bluetooth controller | USB 2.0 | `0x0CF3:0xE300` + 100+ OEM variants |

**Result:** Zero third-party utilities required. Wi-Fi appears in the native menu bar, System Settings, AirDrop, and Continuity all work. Bluetooth hands off to Apple's native `IOBluetoothFamily` after firmware upload.

---

## ✨ Key Features

### Wi-Fi (AirportQCA)
- **Native AirPort emulation** — Subclasses `IO80211Controller` / `IO80211ControllerV2` for full macOS integration
- **Skywalk / IO80211InfraProtocol support** — Works on macOS Ventura, Sonoma, Sequoia, Tahoe (13–15+)
- **WMI-TLV control plane** — Complete BMI boot, HTC/HTT transport, firmware-6.bin + board.bin loading
- **Hardware crypto offload** — AES-CCMP PTK/GTK key installation via firmware
- **Apple RSN Supplicant delegation** — Native WPA2/WPA3 4-way handshake (no custom supplicant)
- **Copy Engine DMA** — 8-pipe CE ring management with QCA9377-specific CE5 override
- **AppleVTD / IOMMU compatible** — Uses `IOBufferMemoryDescriptor` + `IOMapper` for DMA coherence
- **Power management** — Full D0/D3 transitions, sleep/wake recovery

### Bluetooth (QCABluetoothFirmware)
- **ROM version detection** — Identifies QCA Rome `0x00000302` (QCA9377)
- **Rampatch + NVM upload** — `rampatch_usb_00000302.bin` + `nvm_usb_00000302.bin`
- **Linux-parity firmware logic** — Full TLV tag patching (HCI, Deep Sleep, BD_ADDR), rampatch version verification, recovery-friendly equal-version allowance
- **Suspend/resume support** — Power management registration (`PMinit`), `setPowerState()` for D3→D0 wake, automatic firmware re-upload if lost during sleep
- **Correct battery reporting** — NVM patch clears erroneous "charging" flag (bit 6) while preserving in-band sleep (bit 7)
- **Multi-OEM support** — 100+ USB VID/PID entries for Qualcomm, Foxconn, Lite-On, AzureWave, ASUS, and other OEMs
- **Clean handoff** — Renames device to `"Bluetooth USB Host Controller"` for native `IOBluetoothFamily` matching on macOS 12+

---

## 🖥️ Supported Hardware

| Chipset | Wi-Fi | Bluetooth | PCIe ID | USB ID |
|---------|-------|-----------|---------|--------|
| **QCA9377** | 1×1 802.11ac (433 Mbps) | BT 4.2 / 5.0 | `168C:0042` | `0CF3:E300` + OEM variants |

### Bluetooth USB IDs (Qualcomm Atheros 0x0CF3)
`E300`, `535B`, `E007`, `E009`, `E010`, `E301`, `E360`, `E500`, `E600`, `E700`

### OEM Variants
| Vendor | VID | Example PIDs |
|--------|-----|--------------|
| **Foxconn / Hon Hai** | 0x0489 | E0A2, E092, E09F, E0C7–E0EC, E0FC, E100–E14D |
| **Lite-On** | 0x04CA | 3011, 3015, 3016, 301A, 3021–3024, 3A22–3A27 |
| **AzureWave** | 0x13D3 | 3491, 3496, 3501, 3503, 3587, 3601–3624 |
| **ASUS** | 0x0B05 | 1825, 1868 |
| **Other** | 0x10AB, 0x28DE, 0x2C7C, 0x35F5 | Various |

Tested on: Acer laptop with QCA9377.

---

## 📦 Installation

### Prerequisites
- macOS 26 or later
- OpenCore 

### Quick Install (OpenCore)

1. **Download pre-built kexts** from [Releases](https://github.com/xtuber/ath10k/releases) or build from source (see below)
2. **Place in EFI**:
   ```
   EFI/OC/Kexts/
   ├── AirportQCA.kext
   └── QCABluetoothFirmware.kext
   ```
3. **Add to `config.plist`** under `Kernel > Add`:
   ```xml
   <dict>
       <key>BundlePath</key><string>AirportQCA.kext</string>
       <key>Enabled</key><true/>
       <key>ExecutablePath</key><string>Contents/MacOS/AirportQCA</string>
       <key>PlistPath</key><string>Contents/Info.plist</string>
   </dict>
   <dict>
       <key>BundlePath</key><string>QCABluetoothFirmware.kext</string>
       <key>Enabled</key><true/>
       <key>ExecutablePath</key><string>Contents/MacOS/QCABluetoothFirmware</string>
       <key>PlistPath</key><string>Contents/Info.plist</string>
   </dict>
   ```
4. **Reboot**. Wi-Fi and Bluetooth should appear natively.


## 🔨 Building from Source

### Requirements
- Xcode Command Line Tools (`xcode-select --install`)
- macOS SDK 10.15+ (included with Xcode)
- `xxd` (bundled with macOS)

### Build
```bash
cd kext
make clean all
```
Output: `../build/AirportQCA.kext` and `../build/QCABluetoothFirmware.kext`

### Verify Build
```bash
make verify
# Checks: Mach-O 64-bit kext binary, correct architectures, codesigned
```

### Build Configuration
| Variable | Default | Description |
|----------|---------|-------------|
| `SDKROOT` | Auto-detected | macOS SDK path |
| `ARCH` | `x86_64` | Target architecture |
| `BUILD_DIR` | `../build` | Output directory |

---

## 📚 Documentation

Detailed engineering specifications in [`docs/`](docs/):

| Document | Description |
|----------|-------------|
| [01 Hardware & Chipset Architecture](docs/01_HARDWARE_AND_CHIPSET_ARCHITECTURE.md) | QCA9377 silicon, PCIe/USB topology, register maps |
| [02 Firmware Subsystem & Boot Sequence](docs/02_FIRMWARE_SUBSYSTEM_AND_BOOT_SEQUENCE.md) | BMI protocol, firmware-6.bin, Bluetooth DFU |
| [03 Copy Engine DMA & Transport](docs/03_COPY_ENGINE_DMA_AND_TRANSPORT_ARCHITECTURE.md) | CE rings, HTC, HTT, QCA9377 CE5 override |
| [04 WMI-TLV Control Plane](docs/04_WMI_TLV_CONTROL_PLANE_AND_STATE_MACHINE.md) | TLV framing, VDEV lifecycle, scanning, crypto, coexistence |
| [05 macOS Kernel Integration](docs/05_MACOS_KERNEL_INTEGRATION_AND_IOKIT_ARCHITECTURE.md) | IO80211Controller, Skywalk, IOWorkLoop, Bluetooth handoff |
| [06 Implementation Roadmap](docs/06_ENGINEERING_IMPLEMENTATION_ROADMAP_AND_VERIFICATION.md) | 8-phase milestones, 12 test cases, troubleshooting |

---

## 🐛 Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|--------------|-----|
| Wi-Fi not appearing | Kext not loaded / wrong PCIe ID | Check `kextstat \| grep AirportQCA`; verify `IOPCIMatch` in Info.plist |
| "No hardware installed" | Firmware upload failed | Check `dmesg` for BMI/HTC errors; ensure `firmware-6.bin` + `board.bin` present |
| Kernel panic on boot | AppleVTD / IOMMU fault | Verify `IOBufferMemoryDescriptor::inTaskWithPhysicalMask` usage; disable AppleVTD in config.plist if needed |
| Bluetooth not working | Firmware injector didn't match | Check `IOUSBHostDevice` probe score; all common OEM IDs now in Info.plist |
| Bluetooth toggles off on connect | Firmware not transitioning to HCI | Fixed in v1.0.1+: 150ms post-NVM delay allows auto-transition |
| Device shows "charging" when not | NVM HCI config bit 6 set | Fixed in v1.0.1+: NVM tag 17 patch clears erroneous flag |
| Bluetooth fails after sleep | Firmware lost during USB power cut | Fixed in v1.0.1+: `setPowerState()` re-uploads firmware on D3→D0 wake |

**Debug logging**: Set boot arg `qca_debug=1` or add `QCADebug=1` to NVRAM for verbose kernel logs.
```bash
# Check Bluetooth firmware upload logs
log show --predicate 'process == "kernel" && (eventMessage CONTAINS "QCABluetooth")' --last 1h

# Verify battery state reporting
pmset -g accps
```

---

## 🤝 Contributing

Contributions welcome! Please read the [contribution guidelines](CONTRIBUTING.md) (to be created) before submitting PRs.

### Development Workflow
1. Fork the repo
2. Create a feature branch (`git checkout -b feature/amazing-fix`)
3. Make changes with clear commit messages
4. Run `make clean all verify` to ensure build passes
5. Submit PR with description of changes and testing performed

### Priority Areas
- [x] macOS 15 (Sequoia) / Tahoe Skywalk API support
- [x] WPA3 / SAE support (via Apple RSN Supplicant)
- [x] Bluetooth suspend/resume firmware re-upload
- [x] Comprehensive OEM USB PID support (100+ devices)
- [ ] Bluetooth LE audio (LC3 codec) — **hardware unsupported on QCA9377**
- [ ] Additional QCA variants (WCN6855, WCN785x) — separate firmware needed

---

## 📜 License

**Firmware binaries** (`firmware-6.bin`, `board.bin`, `rampatch_usb_*.bin`, `nvm_usb_*.bin`) are sourced from [`linux-firmware`](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/) and remain under their respective upstream licenses (Qualcomm Atheros proprietary firmware). This project embeds them for convenience; they are not relicensed.

---

## 🙏 Acknowledgments

This driver stands on the shoulders of giants:

- **[OpenIntelWireless/itlwm](https://github.com/OpenIntelWireless/itlwm)** — Reference architecture for native macOS Wi-Fi drivers (`AirportItlwm`, Skywalk interface)
- **[OpenIntelWireless/IntelBluetoothFirmware](https://github.com/OpenIntelWireless/IntelBluetoothFirmware)** — USB firmware injector pattern
- **[Linux `ath10k` driver](https://git.kernel.org/pub/scm/linux/kernel/git/kvalo/ath.git/)** — Core BMI, CE, HTC, WMI-TLV, HTT implementation
- **[Linux Bluetooth `btusb_qcom.c` / `btqca.c`](https://git.kernel.org/pub/scm/linux/kernel/git/bluetooth/bluetooth.git/)** — Rome firmware upload protocol
- **Apple XNU / IOKit** — Kernel framework documentation and headers

---

## 📞 Support

- **Issues**: [GitHub Issues](https://github.com/Xtuber14/AirportAth10k/issues) — Bug reports, feature requests
- **Discussions**: [GitHub Discussions](https://github.com/Xtuber14/AirportAth10k/discussions) — Q&A, hardware compatibility, configuration help
- **Documentation**: [`docs/`](docs/) — Deep technical specifications

> **Note**: This is a hobbyist/research project. No warranty implied. Use at your own risk on production machines.

---

**Made with ❤️ for the Hackintosh & macOS driver development community**