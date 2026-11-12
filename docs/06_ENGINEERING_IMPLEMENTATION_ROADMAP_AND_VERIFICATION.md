# 06. Engineering Implementation Roadmap and Verification Matrix

## 1. Executive Implementation Strategy

Building a production-grade macOS kernel extension for the Qualcomm Atheros QCA9377 requires a structured, phased engineering pipeline. Attempting to implement the entire 802.11 stack simultaneously makes isolating faults impossible.

By structuring development into **8 distinct milestones**, each phase can be independently validated against physical hardware registers, bus analyzers, and kernel logs.

```
+--------------------------------------------------------------------------+
| PHASE 1: Hardware Bringup & Firmware Boot (PCIe / BMI / BDF / FW-6)      |
+------------------------------------+-------------------------------------+
                                     v
+--------------------------------------------------------------------------+
| PHASE 2: Copy Engine DMA & HTC Transport (CE Rings / MSI / AppleVTD)     |
+------------------------------------+-------------------------------------+
                                     v
+--------------------------------------------------------------------------+
| PHASE 3: WMI-TLV Control Plane (Handshake / RegDomain / VDEV Creation)   |
+------------------------------------+-------------------------------------+
                                     v
+--------------------------------------------------------------------------+
| PHASE 4: Network Scanning & BSS Ingestion (Active/Passive / Menu Bar)    |
+------------------------------------+-------------------------------------+
                                     v
+--------------------------------------------------------------------------+
| PHASE 5: Association & Security (Apple RSN Supplicant / WPA2-PSK AES)    |
+------------------------------------+-------------------------------------+
                                     v
+--------------------------------------------------------------------------+
| PHASE 6: HTT Data Plane (Tx/Rx Rings / Decapsulation / mbuf Networking)  |
+------------------------------------+-------------------------------------+
                                     v
+--------------------------------------------------------------------------+
| PHASE 7: Bluetooth Firmware Injection (USB DFU / Rampatch / NVM Handoff) |
+------------------------------------+-------------------------------------+
                                     v
+--------------------------------------------------------------------------+
| PHASE 8: Power Management & System Sleep/Wake (S3 / D0 / D3hot Recovery) |
+--------------------------------------------------------------------------+
```

---

## 2. Detailed Milestone Breakdown

### Phase 1: Hardware Bringup & Wi-Fi Firmware Boot
- **Objective**: Establish communication with QCA9377 over PCIe BAR0, perform chip reset, execute BMI commands, upload BDF and `firmware-6.bin`, and verify firmware boot.
- **Key Engineering Steps**:
  1. Implement `AirportQCA::probe` matching Vendor `0x168C` and Device `0x0042`.
  2. Map BAR0 MMIO space using `IOPCIDevice::mapDeviceMemoryWithRegister(0)`.
  3. Execute warm/cold reset via `ath10k_pci_warm_reset` / `ath10k_pci_qca6174_chip_reset`.
  4. Poll `FW_INDICATOR_ADDRESS` until `FW_IND_INITIALIZED` (0x01) is read.
  5. Initialize primitive CE0/CE1 pipes for BMI messaging.
  6. Issue `BMI_GET_TARGET_INFO`; confirm Chip ID (`0x05020000` / `0x05020001`).
  7. Boost internal PLL to 176 MHz via `BMI_WRITE_SOC_REGISTER`.
  8. Extract BDF from `board-2.bin` (or fallback `board.bin`, 8124 bytes) and upload via `BMI_WRITE_MEM`.
  9. Upload `firmware-6.bin` executable segments to target RAM via `BMI_WRITE_MEM`.
  10. Issue `BMI_DONE`.
- **Pass Criteria**: Kernel log records `Target firmware booted successfully; FW_IND_INITIALIZED asserted`.

---

### Phase 2: Copy Engine DMA & HTC Transport Bringup
- **Objective**: Configure all 8 Copy Engine DMA channels, install MSI interrupt handlers, map DMA buffers via AppleVTD, and establish the HTC session.
- **Key Engineering Steps**:
  1. Allocate Source and Destination descriptor rings for CE0 through CE7 using `IOBufferMemoryDescriptor::inTaskWithPhysicalMask` (32-bit physical mask).
  2. Implement `ItlDmaArena` pattern to ensure all rings and packet buffers are mapped via `IOMapper::copyMapperForDevice(pciNub)`.
  3. Apply QCA9377 CE configuration overrides:
     - Disable CE5 (`src_sz_max = 0`, `dest_nentries = 0`).
     - Route Service 15 (HTT Rx) to CE1.
  4. Register single-vector MSI interrupt handler (`IOFilterInterruptEventSource`).
  5. Enable CE interrupts and start Copy Engine hardware.
  6. Exchange `HTC_MSG_READY` with firmware; extract target credit pool.
  7. Issue HTC Connect requests for:
     - `WMI_CONTROL_SVC` (Endpoint 1 -> CE4/CE2).
     - `HTT_DATA_SVC` (Endpoint 2 -> CE3/CE1).
- **Pass Criteria**: HTC endpoints connected; driver receives initial credits.

---

### Phase 3: WMI-TLV Control Plane Implementation
- **Objective**: Implement the WMI-TLV parser and serializer, complete initialization handshake, and create primary STA Virtual Device.
- **Key Engineering Steps**:
  1. Implement generic TLV framing engine (`wmi_tlv_parse` and `wmi_tlv_build`).
  2. Catch incoming `WMI_SERVICE_READY_EVENT`; parse `svc_map` and limits (`max_peers = 15`).
  3. Send `WMI_INIT_CMD` with host memory configuration and CE buffer profiles.
  4. Receive and parse `WMI_READY_EVENT`; extract target MAC address and firmware version.
  5. Send `WMI_PDEV_SET_REGDOMAIN_CMDID` with system country code (e.g. "US", "DE", "IT").
  6. Send `WMI_VDEV_CREATE_CMDID` for `vdev_id = 0`, type `WMI_VDEV_TYPE_STA`.
- **Pass Criteria**: Primary VDEV 0 created; MAC address populated in IOKit registry.

---

### Phase 4: Network Scanning & BSS Discovery
- **Objective**: Translate macOS scan requests into WMI commands and ingest discovered Access Points into macOS UI.
- **Key Engineering Steps**:
  1. Handle `APPLE80211_IOC_SCAN_REQ` in `apple80211Request`.
  2. Construct `WMI_START_SCAN_CMDID` specifying 2.4 GHz and 5 GHz channel frequencies, active dwell time (30 ms), and passive dwell time (100 ms).
  3. Handle `WMI_TLV_TAG_STRUCT_MGMT_RX_HDR` events containing Beacons and Probe Responses:
     - Parse 802.11 header to extract BSSID, SSID, Beacon Interval, Capabilities.
     - Parse Information Elements (IEs): Supported Rates, HT Capabilities, VHT Capabilities, RSN IE (WPA2 ciphers).
     - Calculate RSSI and Noise floor.
  4. Populate internal scan cache.
  5. Upon receiving `WMI_SCAN_EVENTID` (`WMI_SCAN_EVENT_COMPLETED`):
     - Signal macOS via `fNetIf->postMessage(APPLE80211_M_SCAN_DONE)`.
  6. Handle `APPLE80211_IOC_SCAN_RESULT` to return discovered BSS list to macOS.
- **Pass Criteria**: Available Wi-Fi networks appear in macOS menu bar with accurate RSSI bars.

---

### Phase 5: Association & Security (Apple RSN Supplicant)
- **Objective**: Associate to an Access Point, execute WPA2-PSK handshake via Apple's native supplicant, install keys, and bring the link up.
- **Key Engineering Steps**:
  1. Handle `APPLE80211_IOC_SSID` and `APPLE80211_IOC_ASSOCIATE`.
  2. Issue `WMI_VDEV_START_REQUEST_CMDID` configured for AP's channel frequency and channel width (20/40/80 MHz).
  3. Issue `WMI_PEER_CREATE_CMDID` for the AP MAC address.
  4. Issue `WMI_PEER_ASSOC_CMDID` setting AID, listen interval, and HT/VHT capabilities.
  5. Return `true` in `useAppleRSNSupplicant()`:
     - macOS `corewland` initiates 4-way handshake using EAPOL frames.
     - Outbound EAPOL frames are sent via `outputRaw80211Packet`.
     - Inbound EAPOL frames are delivered to macOS via `inputPacket`.
  6. Handle `APPLE80211_IOC_CIPHER_KEY`:
     - Extract PTK (16 bytes AES-CCMP) -> Issue `WMI_VDEV_INSTALL_KEY_CMDID` (Key Index 0, Pairwise).
     - Extract GTK (16 bytes AES-CCMP + 6 bytes RSC) -> Issue `WMI_VDEV_INSTALL_KEY_CMDID` (Key Index 1-3, Group).
  7. Call `setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive)`.
- **Pass Criteria**: Successful connection to WPA2-PSK network; IP address assigned via DHCP.

---

### Phase 6: HTT Data Plane & Packet Transmission
- **Objective**: Implement high-throughput transmit and receive paths between macOS networking and QCA9377 radio.
- **Key Engineering Steps**:
  1. **Transmit Path**:
     - Implement `outputPacket(mbuf_t m)`:
     - Allocate `htt_tx_desc` descriptor from DMA pool.
     - Map `mbuf_t` physical segments into CE3 DMA ring.
     - Fill descriptor flags (802.3 Ethernet format, MSDU ID, peer ID).
     - Trigger CE3 ring update.
     - Handle `HTT_T2H_MSG_TYPE_TX_COMPL_IND` to free `mbuf_t` back to kernel.
  2. **Receive Path**:
     - Handle `HTT_T2H_MSG_TYPE_RX_IN_ORD_IND` on CE1.
     - Strip HTT header, account for `decap_align_bytes = 4`.
     - Allocate fresh `mbuf_t`, copy/transfer frame payload.
     - Deliver packet to macOS TCP/IP stack via `fNetIf->inputPacket(m)`.
  3. Enable hardware TCP/UDP checksum offloading (`kIONetworkFeatureHardwareChecksumOffload`).
- **Pass Criteria**: Full TCP/UDP bidirectional throughput (>200-300 Mbps on 802.11ac 80 MHz channel).

---

### Phase 7: Bluetooth Firmware Injection (`QCABluetoothFirmware`)
- **Objective**: Match the USB Bluetooth controller, inject rampatch and NVM firmware, and hand off to macOS native Bluetooth stack.
- **Key Engineering Steps**:
  1. Create companion kext `QCABluetoothFirmware` matching `IOUSBHostDevice` with Vendor ID `0x0CF3` (and OEM variants `0x0489`, `0x04CA`, `0x13D3`) and Product ID `0xE300`.
  2. In `start(provider)`:
     - Open device: `provider->open(this)`.
     - Send Vendor Request `QCA_GET_TARGET_VERSION` (0x05) to verify ROM version `0x00000302`.
     - Send `rampatch_usb_00000302.bin` via EP0 control transfer (28-byte header) and EP2 OUT bulk transfers (payload).
     - Send `nvm_usb_00000302.bin` via EP0 and EP2 OUT.
     - Verify with `QCA_CHECK_STATUS` (0x04).
     - Issue USB reset and set property `"FirmwareLoaded" = true`.
     - Close device: `provider->close(this)`.
  3. Allow native macOS `BroadcomBluetoothHostControllerUSBTransport` / generic USB HCI driver to claim device.
- **Pass Criteria**: Bluetooth icon turns active in macOS menu bar; Bluetooth devices discoverable and pairable.

---

### Phase 8: Power Management & Sleep/Wake Lifecycle
- **Objective**: Ensure seamless operation across system sleep, wake, and warm reboot cycles.
- **Key Engineering Steps**:
  1. Implement `setPowerState(powerStateOrdinal, whatDevice)`:
     - On Sleep (`kPowerStateOff`):
       - Stop network queues.
       - Disassociate from AP (`WMI_VDEV_STOP_CMDID`).
       - Stop Copy Engine DMA rings.
       - Put PCIe device into D3hot power state.
     - On Wake (`kPowerStateOn`):
       - Transition PCIe device from D3hot to D0.
       - Re-execute warm reset and re-upload firmware via BMI.
       - Re-establish HTC and WMI sessions.
       - Trigger network scan to restore previous connection automatically.
- **Pass Criteria**: Laptop sleeps and wakes without kernel panics; Wi-Fi reconnects within 2-3 seconds of lid open.

---

## 3. Comprehensive Verification & Test Matrix

| Test Case | Procedure | Expected Result | Pass/Fail Criteria |
| :--- | :--- | :--- | :--- |
| **TC-01: PCIe Probe** | Boot macOS with kext loaded | Kext matches `pci168c,42`; logs device revision | Exit code 0; device attached |
| **TC-02: Target Init** | Read `FW_INDICATOR_ADDRESS` post-reset | Register returns `0x00000001` (`FW_IND_INITIALIZED`) | Timeout failure if != 0x01 |
| **TC-03: BMI Upload** | Upload BDF & `firmware-6.bin` via CE0/CE1 | Target completes execution without hang | Zero BMI transfer errors |
| **TC-04: HTC Handshake**| Connect HTC endpoints | Target sends `HTC_MSG_READY` with > 100 credits | Credits received > 0 |
| **TC-05: WMI Handshake**| Send `WMI_INIT_CMD` | Target returns `WMI_READY_EVENT` with MAC | Valid MAC address displayed |
| **TC-06: 2.4/5GHz Scan** | Trigger Wi-Fi scan from menu bar | AP list populates in UI within 3 seconds | Both 2.4 GHz & 5 GHz SSIDs visible |
| **TC-07: WPA2 Join** | Enter password for WPA2-PSK network | 4-way handshake succeeds, keys installed | Link state UP, DHCP IP received |
| **TC-08: Data TX/RX** | Run `iperf3 -c <host> -t 30` | Throughput > 200 Mbps on 80 MHz AC channel | Zero packet drops, zero kernel panics |
| **TC-09: BT Firmware** | Check USB enumeration on boot | Controller shows `0x00000302` patch loaded | BT preference pane shows "On" |
| **TC-10: BT Audio** | Connect Bluetooth headphones (A2DP) | Audio streams smoothly without glitching | Wideband speech & A2DP functional |
| **TC-11: Coexistence** | Stream BT audio while running Wi-Fi iperf | Both audio and Wi-Fi throughput maintain stability | Coex arbiter prevents audio drop |
| **TC-12: S3 Sleep/Wake** | Close laptop lid for 60s, open lid | System resumes; Wi-Fi reconnects within 3s | No crashdump, no kernel panic |

---

## 4. Troubleshooting & Panic Analysis Guide

### Issue 1: Target Firmware Crash (`FW_IND_EVENT_PENDING`)
- **Symptom**: `FW_INDICATOR_ADDRESS` has bit 1 set; CE7 generates continuous interrupts.
- **Root Cause**: BDF mismatch or corrupted firmware image.
- **Remedy**: Verify BDF length is exactly 8,124 bytes. Ensure `board-2.bin` matches the exact subsystem PCI ID of the card. Use `firmware-6.bin` from `source/linux-firmware`.

### Issue 2: Immediate Kernel Panic on Packet Reception (AppleVTD Fault)
- **Symptom**: System crashes with `AppleVTD::PageFault` pointing to a host DMA address.
- **Root Cause**: DMA descriptors allocated without IOMMU mapping.
- **Remedy**: Ensure `IOPCIUseDeviceMapper` property is set on the PCI nub, and all CE rings are mapped via `IOMapper::copyMapperForDevice(pciNub)`.

### Issue 3: HTC Connect Hangs / No Response from Firmware
- **Symptom**: Driver waits indefinitely in `ath10k_htc_wait_target`.
- **Root Cause**: CE5 override was omitted. On QCA9377, CE5 must be disabled and Service 15 redirected to CE1.
- **Remedy**: Verify `ath10k_pci_override_ce_config` logic is executed before `ath10k_pci_alloc_pipes`.

### Issue 4: Wi-Fi Scans OK but Cannot Associate (Timeout at Handshake)
- **Symptom**: Network list appears, but clicking network prompts for password repeatedly and times out.
- **Root Cause**: EAPOL frames dropped or `useAppleRSNSupplicant()` returning false.
- **Remedy**: Ensure `outputRaw80211Packet` transmits outbound EAPOL frames correctly, and incoming EAPOL frames received via `WMI_TLV_TAG_STRUCT_MGMT_RX_HDR` or HTT Rx are routed into `fNetIf->inputPacket(m)`.
