# 03. Copy Engine (CE) DMA, HTC and HTT Transport Architecture

## 1. Overview of the Transport Hierarchy

Communications between the host macOS kernel and the Qualcomm Atheros QCA9377 target processor are structured in three distinct architectural layers:

```
+-------------------------------------------------------------------------+
|                              macOS Kernel                               |
|   IO80211Controller / IO80211Interface / Skywalk Networking Stack      |
+------------------------------------+------------------------------------+
                                     |
                                     v
+------------------------------------+------------------------------------+
|  Control Plane: WMI-TLV            |  Data Plane: HTT (Host-Target)     |
|  - Scanning, Association, Keys     |  - Tx packet frames, Rx indications|
|  - Channel & Regulatory Config     |  - 802.11 / 802.3 decap handling   |
+------------------------------------+------------------------------------+
                                     |
                                     v
+-------------------------------------------------------------------------+
|                  HTC (Host-Target Communication) Layer                  |
|  - Multiplexes logical endpoints (WMI_CONTROL, HTT_DATA, etc.)          |
|  - Manages credit-based flow control and buffer replenishment           |
+------------------------------------+------------------------------------+
                                     |
                                     v
+-------------------------------------------------------------------------+
|                     Copy Engine (CE) DMA Ring Engine                    |
|  - 8 hardware DMA channels (CE0 to CE7)                                 |
|  - Source (Host->Target) and Destination (Target->Host) Ring Descriptors|
|  - Hardware interrupt coalescing and MSI delivery                       |
+------------------------------------+------------------------------------+
                                     |
                                     v
+-------------------------------------------------------------------------+
|                       PCIe 1.1/2.0 Physical Link                        |
|  - MMIO BAR0 Registers, MSI Interrupt Vector, Bus Master DMA           |
+-------------------------------------------------------------------------+
```

---

## 2. Copy Engine (CE) Hardware Architecture

The **Copy Engine** is Qualcomm's proprietary hardware DMA controller implemented on the PCI Express interface of the QCA9377. Instead of standard unified PCI DMA descriptors, the Copy Engine operates through **unidirectional circular descriptor rings**:
- **Source Ring (SRC Ring)**: Used for data transfers initiated by the Host to Target (Transmit).
- **Destination Ring (DST Ring)**: Used for data transfers initiated by the Target to Host (Receive).

### 2.1 Hardware Descriptor Structure (`ce_desc`)
Each descriptor in a Copy Engine ring is a compact 8-byte structure aligned to an 8-byte boundary:

```c
struct ce_desc {
    __le32 addr;   /* Physical/DMA address of host memory buffer (low 32 bits) */
    __le16 nbytes; /* Size of buffer in bytes */
    __le16 flags;  /* Descriptor flags (e.g. CE_DESC_FLAGS_BYTE_SWAP, GATHER) */
};
```

When transmitting:
1. Host places packet/message buffer in contiguous DMA-mapped host memory.
2. Host writes the physical buffer address and length into the next available descriptor in the Source Ring.
3. Host increments the **Source Ring Write Index** register via MMIO.
4. Hardware DMA fetches descriptors, pulls data across PCIe into target SRAM/DRAM, and increments the **Source Ring Read Index**.

When receiving:
1. Host pre-allocates empty receive buffers, fills Destination Ring descriptors with their physical DMA addresses, and updates the **Destination Ring Write Index**.
2. When the target Wi-Fi firmware receives a packet or event, the hardware DMA writes the payload into the host buffer.
3. Hardware updates the **Destination Ring Read Index** and fires an MSI interrupt to the host.

### 2.2 Complete QCA9377 Copy Engine Pipe Configuration

The QCA9377 hardware utilizes 8 Copy Engine pipes (CE0 through CE7). The table below specifies the exact ring sizes, directions, and payload limits derived from `source/ath/drivers/net/wireless/ath/ath10k/pci.c`:

| Pipe | Direction | Source Entries | Dest Entries | Max Buffer Size | Functional Assignment |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **CE0** | Host -> Target | 16 | 0 | 256 bytes | HTC Control & BMI Transmit |
| **CE1** | Target -> Host | 0 | 512 | 2048 bytes | HTT Data Rx & HTC Control Rx (BMI Rx) |
| **CE2** | Target -> Host | 0 | 128 | 2048 bytes | WMI Event Rx (Control Plane Events) |
| **CE3** | Host -> Target | 32 | 0 | 2048 bytes | HTT Data Tx (Frames to Air) |
| **CE4** | Host -> Target | 16 | 0 | 2048 bytes | WMI Command Tx (Control Plane Requests) |
| **CE5** | Target -> Host | 0 | 0 | 0 bytes | **Override for QCA9377/QCA6174** (Reserved / unused) |
| **CE6** | Target -> Host | 0 | 32 | 2048 bytes | Diagnostic / Debug / Firmware Logging |
| **CE7** | Target <-> Host| - | - | - | Diagnostic Window / Direct Register Access |

#### Critical Engineering Note: QCA9377 CE5 Override (`ath10k_pci_override_ce_config`)
In standard QCA9880 chips, CE5 is used for HTT completion messages. In **QCA9377 / QCA6174**, CE5 is overridden:
- `attr[5].src_sz_max = 0`
- `attr[5].dest_nentries = 0`
- `pipe_config[5].pipedir = PIPEDIR_OUT`
- HTC Service 15 (HTT Rx) is redirected to **CE1** (`serv_to_pipe[15].pipenum = 1`).
Failure to apply this override in a macOS driver results in an immediate target freeze upon HTC initialization.

---

## 3. HTC (Host-Target Communication) Layer

The **HTC** layer provides logical communication channels multiplexed over the raw Copy Engine pipes, implementing connection management, packet framing, and credit-based flow control.

### 3.1 HTC Packet Header
Every message passed across HTC contains a 4-byte header:

```c
struct ath10k_htc_hdr {
    u8 eid;          /* Endpoint ID (0: Control, 1: WMI, 2: HTT, etc.) */
    u8 flags;        /* Flags: ATH10K_HTC_FLAG_NEED_CREDIT_UPDATE (0x01), etc. */
    __le16 len;      /* Length of payload following header (excluding 4-byte header) */
} __packed;
```

### 3.2 Credit-Based Flow Control Protocol
To prevent the host from overwhelming target on-chip RAM:
1. Target firmware advertises an initial credit pool during `HTC_MSG_READY` exchange.
2. Each WMI command or HTT Tx descriptor sent by the host consumes **1 or more credits**.
3. If host credits reach 0 for a given endpoint, host transmission on that endpoint is suspended.
4. As the target finishes processing commands or transmits frames over the air, it returns replenished credits inside the `flags` and trailing trailer blocks of incoming Rx packets.
5. The host driver parses incoming trailers and wakes the transmission queue.

### 3.3 Endpoint Service Mapping for QCA9377

```
HTC Endpoint 0 : Reserved for HTC Control Messages (Setup, Connect, Credit Sync)
HTC Endpoint 1 : WMI Control Service (Commands & Events -> mapped to CE4 / CE2)
HTC Endpoint 2 : HTT Data Service (Frame Descriptors -> mapped to CE3 / CE1)
HTC Endpoint 3 : Diagnostic / Logging Service (Debug -> mapped to CE6)
```

---

## 4. HTT (Host-Target Transport) Data Plane

**HTT** is the performance-critical data plane transport governing packet transmission and reception between the macOS network stack and the 802.11 MAC baseband.

### 4.1 HTT Transmit Architecture
When macOS submits an Ethernet or 802.11 frame via `AirportItlwm::outputPacket(mbuf_t m)`:
1. Driver allocates an `htt_tx_desc` from a pre-allocated pool in DMA memory.
2. Driver maps the `mbuf_t` packet payload into physical DMA addresses:
   - For packets spanning multiple fragments, an scatter-gather fragment table is populated.
3. Driver initializes the HTT Tx descriptor:
   - Sets packet ID (`msdu_id`) for completion tracking.
   - Sets flags: packet type (802.3 Ethernet vs 802.11 Native WiFi), encryption key index, peer ID.
4. Descriptors are enqueued to CE3.
5. Target pulls payload via PCIe DMA, frames the packet for radio transmission, and returns a Tx completion indication (`HTT_T2H_MSG_TYPE_TX_COMPL_IND`) indicating success/failure and retry count.

### 4.2 HTT Receive Architecture & Decapsulation
When the radio receives an 802.11 frame:
1. Target firmware validates FCS, performs hardware AES-CCMP / TKIP decryption (if key installed), and processes MAC A-MPDU reordering.
2. Firmware formats an HTT Rx Indication (`HTT_T2H_MSG_TYPE_RX_IN_ORD_IND` or `HTT_T2H_MSG_TYPE_RX_IND`).
3. Payload is transferred via CE1 into host DMA buffer.
4. **Decapsulation Handling**:
   - QCA9377 supports both **Native 802.11 WiFi decap** (passes raw 802.11 frames with radio headers) and **Ethernet decap** (firmware strips 802.11 header and translates to 802.3 Ethernet header).
   - In QCA9377, `decap_align_bytes = 4` must be honored to ensure 32-bit aligned IP headers.
   - For native AirPort emulation under macOS, raw 802.11 management frames (beacons, probe responses, action frames) are forwarded to macOS AirPort core, while data frames are forwarded as Ethernet frames to `IOEthernetInterface` / `IONetworkInterface`.

---

## 5. AppleVTD, IOMMU and IOKit Memory Management

In macOS hackintoshes and modern UEFI systems, **AppleVTD (Apple Virtualization Technology for Directed I/O / DMAR / AMD-Vi)** is frequently enabled to ensure full compatibility with Apple Ethernet, Wi-Fi, and Thunderbolt devices.

### 5.1 The AppleVTD DMA Fault Problem
If a driver allocates physical DMA memory using standard physical page addresses without mapping them through the system DMAR table, AppleVTD detects an unauthorized memory access and issues an IOMMU Page Fault, triggering a kernel panic or silent packet drop:
```
AppleVTD::PageFault: Domain 0x00, Device 0x01:0x00.0, Address 0x0000000108A4B000 (Read/Write fault)
```

### 5.2 The Engineering Solution (`ItlDmaArena` Pattern)
Following the architectural pattern developed in `source/itlwm/itl80211/ItlDma.cpp`:
1. The driver must inform the PCI subsystem to register with the device mapper:
   ```cpp
   pciNub->setProperty("IOPCIUseDeviceMapper", kOSBooleanTrue);
   ```
2. Driver acquires the device mapper:
   ```cpp
   IOMapper *mapper = IOMapper::copyMapperForDevice(pciNub);
   ```
3. All Copy Engine descriptor rings and HTT packet buffers must be allocated using physically contiguous, DMA-coherent buffer descriptors:
   ```cpp
   IOBufferMemoryDescriptor *mem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
       kernel_task,
       kIODirectionInOut | kIOMemoryHostPhysicallyContiguous | kIOMemoryMapperNone,
       ringSize,
       0x00000000FFFFFFFFULL /* 32-bit DMA Mask for QCA9377 */
   );
   mem->prepare();
   ```
4. Physical addresses programmed into `ce_desc.addr` must be derived from `mem->getPhysicalSegment(0, &segLen)`.
5. When AppleVTD is active, `IOMapper::iovmAlloc` or `IODMACommand` is used to create valid DMAR scatter-gather mappings.

This ensures zero IOMMU faults, deterministic low-latency DMA transfers, and 100% stability across all macOS versions.
