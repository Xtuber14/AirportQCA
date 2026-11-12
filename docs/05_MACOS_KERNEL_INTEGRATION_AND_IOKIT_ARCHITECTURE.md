# 05. macOS Kernel Integration and IOKit Driver Architecture

## 1. Architectural Strategy: The Native AirPort Paradigm

In macOS, wireless networking is governed by Apple's proprietary **`IO80211Family.kext`**. Unlike Linux where `mac80211` exposes a unified `cfg80211` / `nl80211` userspace interface, macOS relies on a private, object-oriented IOKit C++ framework.

Historically, hackintosh developers had to choose between two paradigms:
1. **Ethernet Emulation (`itlwm` + HeliPort app)**: Driver presents itself as a standard wired `IOEthernetController`. The driver connects to APs via a custom user-space daemon (`HeliPort`). Disadvantages: No native menu bar integration, no native AirDrop, no native Location Services, no automatic OS roaming.
2. **Native AirPort Emulation (`AirportItlwm`)**: Driver directly subclasses `IO80211Controller` (or `IO80211ControllerV2` on modern macOS) and attaches an `IO80211Interface`. Disadvantages: More complex, depends on private Apple kernel headers. Advantages: **100% native macOS user experience** (native menu bar Wi-Fi icon, native System Settings Wi-Fi pane, Instant Hotspot, native Apple RSN WPA2/WPA3 authentication, AirDrop discovery, and CoreLocation).

**Design Decision for QCA9377**:
Following the user requirement ("work like native macOS wifi card like broadcom or intel with itlwm"), the proposed driver (**`AirportQCA`**) directly adopts the **Native AirPort Emulation (`AirportItlwm`)** architecture.

---

## 2. Driver Class Hierarchy & Core Components

```
+--------------------------------------------------------------------------+
|                               IOKit Kernel                               |
+------------------------------------+-------------------------------------+
                                     |
                                     v
+------------------------------------+-------------------------------------+
|                      IO80211Controller / ControllerV2                    |
|                        (Apple IO80211Family Base)                        |
+------------------------------------+-------------------------------------+
                                     |
                                     v
+------------------------------------+-------------------------------------+
|                             AirportQCA                                   |
|  - Subclasses IO80211Controller / IO80211ControllerV2                    |
|  - Owns IOWorkLoop, IOCommandGate, IOTimerEventSource                    |
|  - Implements apple80211Request IOCTL handler                            |
|  - Owns QCA9377 Hardware HAL (PCIe, CE Rings, HTC, WMI, HTT)             |
+------------------------------------+-------------------------------------+
                                     |
                                     +--------------------+
                                     v                    v
+------------------------------------+---+  +-------------+----------------+
|          AirportQCAInterface           |  |      AirportQCASkywalk        |
|      (Legacy macOS 10.13 - 12)         |  |   (macOS 13 Ventura - 15+)   |
|  - Subclasses IO80211Interface         |  | - Subclasses                 |
|  - Bridges mbuf packets to HTT Tx/Rx   |  |   IO80211InfraProtocol       |
|  - Dispatches scan & link events       |  | - Skywalk packet ring DMA    |
+----------------------------------------+  +------------------------------+
```

---

## 3. The `apple80211Request` IOCTL Dispatch Engine

In macOS, all user-space Wi-Fi actions (scanning, joining networks, retrieving RSSI, setting country codes) are initiated by the system daemon `corewland` and dispatched to the driver via `AirportQCA::apple80211Request(request_type, request_number, interface, data)`:

| Request Code | Macro Name | Action & Hardware Mapping |
| :--- | :--- | :--- |
| **1** | `APPLE80211_IOC_SSID` | Get/Set current SSID (`ic_des_essid` -> `WMI_VDEV_START_REQUEST_CMDID`). |
| **2** | `APPLE80211_IOC_AUTH_TYPE` | Set Authentication Mode (Open, WPA-PSK, WPA2-PSK, 802.1X). |
| **4** | `APPLE80211_IOC_CHANNEL` | Get/Set radio frequency and channel bandwidth (20/40/80 MHz). |
| **7** | `APPLE80211_IOC_TXPOWER` | Query current Transmit Power level in dBm. |
| **8** | `APPLE80211_IOC_RATE` | Query current PHY link rate (e.g. 433.3 Mbps). |
| **9** | `APPLE80211_IOC_BSSID` | Get/Set target BSSID MAC address. |
| **10** | `APPLE80211_IOC_SCAN_REQ` | Initiate active/passive scan -> `WMI_START_SCAN_CMDID`. |
| **11** | `APPLE80211_IOC_SCAN_RESULT`| Returns discovered BSS list to macOS UI. |
| **12** | `APPLE80211_IOC_CARD_CAPABILITIES`| Informs macOS of card features: 802.11a/b/g/n/ac, 1x1, WPA2, VHT80. |
| **13** | `APPLE80211_IOC_STATE` | Reports connection state (`STATE_INIT`, `STATE_SCAN`, `STATE_ASSOC`, `STATE_RUN`). |
| **14** | `APPLE80211_IOC_PHY_MODE` | Reports active PHY mode: `APPLE80211_MODE_11AC`. |
| **16** | `APPLE80211_IOC_RSSI` | Reports current Received Signal Strength Indicator (in dBm). |
| **17** | `APPLE80211_IOC_NOISE` | Reports background noise floor (typically -95 dBm). |
| **20** | `APPLE80211_IOC_ASSOCIATE` | Triggers join sequence to selected AP. |
| **21** | `APPLE80211_IOC_ASSOCIATE_RESULT`| Returns status code of association attempt (0 = Success). |
| **22** | `APPLE80211_IOC_DISASSOCIATE` | Halts traffic, tears down peer -> `WMI_VDEV_STOP_CMDID`. |
| **27** | `APPLE80211_IOC_SUPPORTED_CHANNELS`| Returns list of valid channels based on regulatory database. |
| **46** | `APPLE80211_IOC_RSN_IE` | Provides WPA2/WPA3 Information Element to include in Assoc Request. |
| **51** | `APPLE80211_IOC_COUNTRY_CODE` | Configures ISO country code ("US", "DE", "CN", etc.) -> `WMI_PDEV_SET_REGDOMAIN`. |
| **159**| `APPLE80211_IOC_CIPHER_KEY` | Installs WPA2 PTK/GTK keys into QCA9377 hardware AES engine. |

### 3.1 Delegating Security to Apple RSN Supplicant
A major architectural advantage of `AirportItlwm` is:
```cpp
bool AirportQCA::useAppleRSNSupplicant(IO80211Interface *interface)
{
    return true; /* Leverage macOS's built-in 802.1X / WPA2-PSK 4-way handshake */
}
```
By returning `true`, the driver does **not** need to embed a complete software WPA supplicant or WPA state machine. The macOS system daemon `corewland` executes the 4-way handshake using EAPOL frames exchanged through `AirportQCA::outputRaw80211Packet`. Once complete, macOS hands the derived PTK and GTK directly to the driver via `APPLE80211_IOC_CIPHER_KEY`, which installs them into QCA9377 hardware via `WMI_VDEV_INSTALL_KEY_CMDID`.

---

## 4. WorkLoop, Command Gate & Interrupt Model

To maintain synchronization and prevent deadlocks across multi-core processors, IOKit drivers must strictly adhere to the **`IOWorkLoop`** execution model.

```
       Host PCIe Hardware Interrupt (MSI from QCA9377)
                              |
                              v
                 IOFilterInterruptEventSource
               (Validates interrupt status bit)
                              |
                              v
                         IOWorkLoop
             (Single-threaded serialized queue)
                              |
         +--------------------+--------------------+
         |                                         |
         v                                         v
   IOCommandGate                              IOTimerEventSource
   (Synchronizes IOCTLs,                      (Watchdog 1000ms,
   Tx/Rx DMA completions,                     Scan timeouts,
   and WMI state machine)                     Credit replenishment)
```

1. **`IOWorkLoop`**: All packet submissions, completion callbacks, and IOCTL executions are serialized through a single workloop.
2. **`IOCommandGate`**: Functions like `tsleepHandler` and `setLinkStateGated` use the command gate to safely put threads to sleep while waiting for hardware responses (e.g. during BMI or WMI command timeouts) without holding spinlocks.
3. **MSI Interrupts**:
   - The driver configures single-vector MSI via `IOPCIDevice::enableMSI()`.
   - Interrupt service routine parses `PCIE_INTR_CAUSE_ADDRESS`, masks hardware interrupts, wakes the workloop to service CE rings, and unmasks interrupts.

---

## 5. Skywalk Architecture for Modern macOS (macOS 13 Ventura, 14 Sonoma, 15 Sequoia)

Beginning with macOS 13 Ventura, Apple deprecated standard `IOEthernetInterface` attachments for native Wi-Fi cards and transitioned fully to **Skywalk (`IOSkywalkNetworkInterface` / `IO80211InfraProtocol`)**.

### 5.1 Differences in the V2 Skywalk Driver
As demonstrated in `source/itlwm/AirportItlwm/AirportItlwmSkywalkInterface.cpp`:
- The driver class derives from **`IO80211ControllerV2`**.
- Instead of using `mbuf_t` linked lists for transmit and receive, Skywalk operates through **ring-based packet rings (`IOSkywalkPacket`)**:
  - `skywalk_rx_ring`: Pre-mapped circular packet buffers where incoming HTT Rx frames are deposited.
  - `skywalk_tx_ring`: Zero-copy transmit ring where macOS places outbound TCP/IP frames.
- **Header Alignment**: Skywalk strictly enforces 4-byte L3 header alignment, matching the QCA9377 `decap_align_bytes = 4` hardware specification perfectly.

---

## 6. Bluetooth Driver Architecture: `QCABluetoothFirmware`

The Bluetooth subsystem requires a dedicated companion driver modeled directly on `source/IntelBluetoothFirmware`:

### 6.1 Matching Personality (`Info.plist`)
The kext matches against `IOUSBHostDevice` with a high `IOProbeScore` (e.g. 5000):

```xml
<key>IOKitPersonalities</key>
<dict>
    <key>QCABluetoothFirmware</key>
    <dict>
        <key>CFBundleIdentifier</key>
        <string>com.alexandred.QCABluetoothFirmware</string>
        <key>IOClass</key>
        <string>QCABluetoothFirmware</string>
        <key>IOProviderClass</key>
        <string>IOUSBHostDevice</string>
        <key>idVendor</key>
        <integer>3315</integer> <!-- 0x0CF3 (Qualcomm Atheros) -->
        <key>idProduct</key>
        <integer>58112</integer> <!-- 0xE300 (QCA9377 Bluetooth) -->
        <key>IOProbeScore</key>
        <integer>5000</integer>
    </dict>
</dict>
```
*(Additional personalities match Vendor IDs `0x0489`, `0x04CA`, and `0x13D3`).*

### 6.2 Probe, Upload & Handoff Lifecycle
1. **Match**: The kext attaches to the raw USB controller when macOS enumerates USB devices.
2. **Open**: Kext calls `provider->open(this)` to gain exclusive USB endpoint control.
3. **Firmware Injection**:
   - Queries `QCA_GET_TARGET_VERSION` via EP0 control transfer.
   - Downloads `rampatch_usb_00000302.bin` via EP0 (header) and EP2 OUT (bulk body).
   - Downloads `nvm_usb_00000302.bin` via EP0 and EP2 OUT.
   - Checks `QCA_CHECK_STATUS` for `QCA_PATCH_UPDATED | QCA_SYSCFG_UPDATED`.
4. **Soft Reset**: Issues USB controller reset.
5. **Close & Detach**:
   - Kext sets `provider->setProperty("FirmwareLoaded", kOSBooleanTrue)`.
   - On macOS Monterey+, sets device name to `"Bluetooth USB Host Controller"`.
   - Calls `provider->close(this)`.
6. **Native Attachment**:
   - macOS's native `BroadcomBluetoothHostControllerUSBTransport` or generic USB Bluetooth host controller driver matches the now-initialized hardware.
   - System recognizes a genuine Bluetooth 4.2 / 5.0 Host Controller with standard HCI command support!
