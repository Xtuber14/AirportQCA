//
//  QCAWMI.cpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//

#include "QCAWMI.hpp"
#include "QCAPCIDevice.hpp"
#include "QCAHTC.hpp"

namespace {
static const u16 TAG_ARRAY_STRUCT = 18;
static const u16 TAG_ARRAY_UINT32 = 16;
static const u16 TAG_ARRAY_BYTE = 17;
static const u16 TAG_ARRAY_FIXED_STRUCT = 19;
static const u16 TAG_STRUCT_SERVICE_READY = 32;
static const u16 TAG_STRUCT_READY = 35;
static const u16 TAG_STRUCT_SCAN_EVENT = 36;
static const u16 TAG_STRUCT_INIT = 74;
static const u16 TAG_STRUCT_RESOURCE_CONFIG = 75;
static const u16 TAG_STRUCT_HOST_MEM_REQ = 34;
static const u16 TAG_STRUCT_HOST_MEM_CHUNK = 76;
static const u16 TAG_STRUCT_START_SCAN = 77;
static const u16 TAG_STRUCT_CHANNEL = 80;
static const u16 TAG_STRUCT_VDEV_CREATE = 86;
static const u16 TAG_STRUCT_VDEV_START = 88;
static const u16 TAG_STRUCT_VDEV_UP = 92;
static const u16 TAG_STRUCT_VDEV_STOP = 93;
static const u16 TAG_STRUCT_PEER_CREATE = 97;
static const u16 TAG_STRUCT_PEER_ASSOC = 101;
static const u16 TAG_STRUCT_VHT_RATE_SET = 102;
static const u16 TAG_STRUCT_INSTALL_KEY = 96;
/* ath10k enum ath10k_hw_txrx_mode: RAW=0, NATIVE_WIFI=1, ETHERNET=2. */
static const u32 WMI_RX_DECAP_NATIVE_WIFI = 1;
static const u16 WMI_EVENT_SERVICE_READY = 1;
static const u16 WMI_EVENT_READY = 2;
static const u32 WMI_EVENT_SCAN = 0x3001;
static const u32 WMI_EVENT_VDEV_START_RESPONSE = 0x5001;
static const u16 TAG_STRUCT_VDEV_START_RESPONSE = 40;
static const u32 WMI_SCAN_EVENT_COMPLETED = 1U << 1;
static const u32 WMI_SCAN_EVENT_START_FAILED = 1U << 6;
static const u32 WMI_TLV_ABI_VER0 = 0x01000000;
static const u32 WMI_TLV_ABI_VER1 = 53;
static const u32 WMI_TLV_ABI_NS0 = 0x5F414351;
static const u32 WMI_TLV_ABI_NS1 = 0x00004C4D;
static const u32 WMI_TLV_ABI_NS2 = 0;
static const u32 WMI_TLV_ABI_NS3 = 0;

struct __attribute__((packed)) WmiTlvHeader { __le16 tag; __le16 len; };
struct __attribute__((packed)) WmiAbiVersion {
    __le32 ver0, ver1, ns0, ns1, ns2, ns3;
};
struct __attribute__((packed)) WmiServiceReady {
    __le32 build;
    WmiAbiVersion abi;
    __le32 phyCaps, maxFrag, rfChains, htCaps, vhtCaps, vhtMcs;
    __le32 minTxPower, maxTxPower, sysCaps, minPktSize, maxBeaconIe;
    __le32 numMemReqs;
};
struct __attribute__((packed)) WmiHostMemReq {
    __le32 reqId, unitSize, unitInfo, numUnits;
};
struct __attribute__((packed)) WmiHostMemChunk {
    __le32 reqId, addressLow, size, addressHigh;
};
struct __attribute__((packed)) WmiMacAddress { u8 address[6]; u8 padding[2]; };
struct __attribute__((packed)) WmiVdevCreate { __le32 id, type, subtype; WmiMacAddress mac; };
struct __attribute__((packed)) WmiVdevStop { __le32 id; };
struct __attribute__((packed)) WmiVdevUp { __le32 id, associationId; WmiMacAddress bssid; };
struct __attribute__((packed)) WmiPeerCreate { __le32 vdevId; WmiMacAddress mac; __le32 type; };
struct __attribute__((packed)) WmiPeerAssoc {
    WmiMacAddress mac;
    __le32 vdevId, newAssociation, associationId, flags, caps, listenInterval;
    __le32 htCaps, maxMpdu, mpduDensity, rateCaps, nss, vhtCaps, phyMode;
    __le32 htInfo[2], numLegacyRates, numHtRates;
};
struct __attribute__((packed)) WmiVhtRates { __le32 rxMaxRate, rxMcsSet, txMaxRate, txMcsSet; };
struct __attribute__((packed)) WmiKeySequence { __le32 low, high; };
struct __attribute__((packed)) WmiInstallKey {
    __le32 vdevId;
    WmiMacAddress peer;
    __le32 index, flags, cipher;
    WmiKeySequence rsc, globalRsc, tsc;
    u8 wpiRsc[16], wpiTsc[16];
    __le32 keyLength, txMicLength, rxMicLength;
};
struct __attribute__((packed)) WmiSSID { __le32 length; u8 bytes[32]; };
struct __attribute__((packed)) WmiChannel {
    __le32 frequency, center1, center2, flags, regInfo0, regInfo1;
};
struct __attribute__((packed)) WmiVdevStart {
    __le32 vdevId, requestorId, beaconInterval, dtimPeriod, flags;
    WmiSSID ssid;
    __le32 beaconRate, beaconPower, noaCount, disableHwAck;
};
struct __attribute__((packed)) WmiScanCommon {
    __le32 scanId, requestorId, vdevId, priority, notifyEvents;
    __le32 activeDwell, passiveDwell, minRest, maxRest, repeatProbe;
    __le32 probeSpacing, idleTime, maxScanTime, probeDelay, scanFlags;
};
struct __attribute__((packed)) WmiStartScan {
    WmiScanCommon common;
    __le32 burstDuration, numChannels, numBssids, numSsids, ieLength, numProbes;
    WmiMacAddress mac, mask;
};
struct __attribute__((packed)) WmiInitCommand {
    WmiAbiVersion abi;
    __le32 numChunks;
};
struct __attribute__((packed)) WmiReadyEvent {
    WmiAbiVersion abi;
    u8 mac[6];
    u8 pad[2];
    __le32 status;
};
struct __attribute__((packed)) WmiResourceConfig {
    __le32 numVdevs, numPeers, numOffloadPeers, numOffloadReorder, numPeerKeys, numTids;
    __le32 astSkid, txChainMask, rxChainMask, rxTimeout[4], rxDecap;
    __le32 scanPending, bmissVdevs, roamVdevs, roamProfiles;
    __le32 multicastGroups, multicastElems, multicastMode, txLogSize, wdsEntries;
    __le32 dmaBurst, aggregateDelimiter, skipDefragCheck, vowConfig, gtkVdevs;
    __le32 msduDesc, maxFragEntries, tdlsVdevs, tdlsEntries, beaconVdevs;
    __le32 multicastFilters, wowFilters, keepAlivePatterns, keepAliveSize;
    __le32 tdlsSleepSta, tdlsBufferSta, wmiSeparate, ocbVdevs, ocbChannels, ocbSchedules, hostCaps;
};
static_assert(sizeof(WmiResourceConfig) == 176, "ath10k WMI-TLV resource ABI mismatch");
static_assert(offsetof(WmiServiceReady, numMemReqs) == 72, "ath10k service-ready ABI mismatch");

static u32 readLE32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void appendTlv(u8 *dst, u32 *offset, u16 tag, const void *data, u16 len) {
    WmiTlvHeader h = { cpu_to_le16(tag), cpu_to_le16(len) };
    memcpy(dst + *offset, &h, sizeof(h));
    *offset += sizeof(h);
    if (len) memcpy(dst + *offset, data, len);
    *offset += len;
}
}

QCAWMI::QCAWMI(QCAPCIDevice *pci)
    : fPci(pci), fMemChunkCount(0), fEventLock(IOLockAlloc())
{
    memset(fMemChunks, 0, sizeof(fMemChunks));
    memset(fMemReqIds, 0, sizeof(fMemReqIds));
}

QCAWMI::~QCAWMI() {
    for (u32 i = 0; i < fMemChunkCount; ++i) {
        fMemChunks[i]->complete();
        fMemChunks[i]->release();
    }
    if (fEventLock) {
        IOLockFree(fEventLock);
        fEventLock = nullptr;
    }
}

bool QCAWMI::init()
{
    qca_info("Initializing WMI-TLV control plane");
    return fEventLock != nullptr;
}

int QCAWMI::sendWmiCmd(u32 cmdId, const void *payload, u32 len)
{
    u8 buffer[2048];
    if (len + 4 > sizeof(buffer)) return -1;

    __le32 *hdr = (__le32*)buffer;
    hdr[0] = cpu_to_le32(cmdId);
    if (payload && len > 0) {
        memcpy(buffer + 4, payload, len);
    }

    QCAHTC *htc = fPci->getHTC();
    if (!htc) return -1;
    return htc->send(ATH10K_HTC_EP_1, buffer, len + sizeof(*hdr));
}

int QCAWMI::receiveEvent(u32 expectedEvent, void *payload, u32 *length)
{
    if (!fEventLock) return -1;
    IOLockLock(fEventLock);
    const int result = receiveEventLocked(expectedEvent, payload, length);
    IOLockUnlock(fEventLock);
    return result;
}

int QCAWMI::receiveEventLocked(u32 expectedEvent, void *payload, u32 *length)
{
    QCAHTC *htc = fPci->getHTC();
    if (!htc || !payload || !length || *length < sizeof(__le32)) return -1;
    u8 event[4096];
    int timeout = 5000;
    while (timeout-- > 0) {
        u32 eventLength = sizeof(event);
        if (htc->recv(ATH10K_HTC_EP_1, event, &eventLength) == 0) {
            if (eventLength < sizeof(__le32)) return -1;
            u32 eventId = readLE32(event) & 0x00ffffff;
            if (eventId != expectedEvent) {
                qca_warn("Ignoring WMI event 0x%x while waiting for 0x%x", eventId, expectedEvent);
            } else {
                u32 dataLength = eventLength - sizeof(__le32);
                if (dataLength > *length) return -1;
                memcpy(payload, event + sizeof(__le32), dataLength);
                *length = dataLength;
                return 0;
            }
        } else {
            qca_mdelay(1);
        }
    }
    return -1;
}

void QCAWMI::pollEvents()
{
    QCAHTC *htc = fPci->getHTC();
    if (!htc || !fEventLock) return;

    u32 completedScans = 0;
    IOLockLock(fEventLock);
    for (u32 count = 0; count < 16; ++count) {
        u8 event[2048];
        u32 eventLength = sizeof(event);
        if (htc->recv(ATH10K_HTC_EP_1, event, &eventLength) != 0) break;
        if (eventLength < sizeof(__le32)) continue;

        const u32 eventId = readLE32(event) & 0x00ffffff;
        if (eventId != WMI_EVENT_SCAN) {
            continue;
        }

        u32 offset = sizeof(__le32);
        bool scanFinished = false;
        while (offset + sizeof(WmiTlvHeader) <= eventLength) {
            WmiTlvHeader header;
            memcpy(&header, event + offset, sizeof(header));
            offset += sizeof(header);
            const u16 tag = le16_to_cpu(header.tag);
            const u16 length = le16_to_cpu(header.len);
            if (length > eventLength - offset) break;
            if (tag == TAG_STRUCT_SCAN_EVENT && length >= 24) {
                const u32 eventType = readLE32(event + offset);
                if (eventType & (WMI_SCAN_EVENT_COMPLETED | WMI_SCAN_EVENT_START_FAILED)) {
                    qca_info("Firmware scan finished (event=0x%x)", eventType);
                    scanFinished = true;
                }
            }
            offset += length;
        }
        if (scanFinished) ++completedScans;
    }
    IOLockUnlock(fEventLock);
    for (u32 i = 0; i < completedScans; ++i) fPci->notifyScanComplete();
}

int QCAWMI::waitServiceReady()
{
    u8 event[8192];
    u32 eventLength = sizeof(event);
    if (receiveEvent(WMI_EVENT_SERVICE_READY, event, &eventLength) != 0) {
        qca_err("Timed out waiting for WMI_SERVICE_READY");
        return -1;
    }
    bool foundService = false, foundRequests = false;
    u32 offset = 0, expectedRequests = 0;
    u32 allocatedRequests = 0;
    while (offset + sizeof(WmiTlvHeader) <= eventLength) {
        WmiTlvHeader h;
        memcpy(&h, event + offset, sizeof(h));
        offset += sizeof(h);
        u16 tag = le16_to_cpu(h.tag), len = le16_to_cpu(h.len);
        if (len > eventLength - offset) return -1;
        if (tag == TAG_STRUCT_SERVICE_READY) {
            if (len < sizeof(WmiServiceReady)) return -1;
            WmiServiceReady ready;
            memcpy(&ready, event + offset, sizeof(ready));
            if (le32_to_cpu(ready.abi.ver0) != WMI_TLV_ABI_VER0 ||
                le32_to_cpu(ready.abi.ver1) != WMI_TLV_ABI_VER1 ||
                le32_to_cpu(ready.abi.ns0) != WMI_TLV_ABI_NS0 ||
                le32_to_cpu(ready.abi.ns1) != WMI_TLV_ABI_NS1 ||
                le32_to_cpu(ready.abi.ns2) != WMI_TLV_ABI_NS2 ||
                le32_to_cpu(ready.abi.ns3) != WMI_TLV_ABI_NS3) return -1;
            expectedRequests = le32_to_cpu(ready.numMemReqs);
            if (expectedRequests > 16) return -1;
            foundService = true;
        } else if (tag == TAG_ARRAY_STRUCT) {
            u32 sub = 0;
            while (sub + sizeof(WmiTlvHeader) <= len) {
                WmiTlvHeader child;
                memcpy(&child, event + offset + sub, sizeof(child));
                sub += sizeof(child);
                u16 childTag = le16_to_cpu(child.tag), childLen = le16_to_cpu(child.len);
                if (childLen > len - sub) return -1;
                if (childTag == TAG_STRUCT_HOST_MEM_REQ && childLen == sizeof(WmiHostMemReq)) {
                    if (foundRequests || allocatedRequests >= 16) return -1;
                    WmiHostMemReq req;
                    memcpy(&req, event + offset + sub, sizeof(req));
                    u32 units = le32_to_cpu(req.numUnits);
                    u32 flags = le32_to_cpu(req.unitInfo);
                    if (flags & (1U << 2)) units = 55; /* QCA9377 cache peers: 50 + 4 vdevs + self */
                    else if (flags & (1U << 1)) units = 34;
                    else if (flags & (1U << 0)) units = 5;
                    u32 unitSize = le32_to_cpu(req.unitSize);
                    if (!units || !unitSize || units > 2048 || unitSize > 1024 * 1024 ||
                        units > UINT32_MAX / ((unitSize + 3) & ~3U)) return -1;
                    u32 poolSize = units * ((unitSize + 3) & ~3U);
                    IOBufferMemoryDescriptor *mem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                        kernel_task, kIODirectionInOut | kIOMemoryHostPhysicallyContiguous,
                        poolSize, 0x00000000FFFFFFFFULL);
                    if (!mem || mem->prepare() != kIOReturnSuccess ||
                        mem->getPhysicalAddress() + poolSize > 0x100000000ULL) {
                        if (mem) { mem->complete(); mem->release(); }
                        return -1;
                    }
                    memset(mem->getBytesNoCopy(), 0, poolSize);
                    fMemChunks[fMemChunkCount] = mem;
                    fMemReqIds[allocatedRequests++] = le32_to_cpu(req.reqId);
                    ++fMemChunkCount;
                }
                sub += childLen;
            }
            foundRequests = true;
        }
        offset += len;
    }
    if (!foundService || (expectedRequests && (!foundRequests || allocatedRequests != expectedRequests))) {
        qca_err("WMI_SERVICE_READY memory request set is incomplete");
        return -1;
    }
    qca_info("WMI_SERVICE_READY accepted; allocated %u DMA chunks", fMemChunkCount);
    return 0;
}

int QCAWMI::waitReady(u8 mac[6])
{
    u8 event[1024];
    u32 eventLength = sizeof(event);
    if (!mac || receiveEvent(WMI_EVENT_READY, event, &eventLength) != 0) return -1;
    u32 offset = 0;
    while (offset + sizeof(WmiTlvHeader) <= eventLength) {
        WmiTlvHeader h;
        memcpy(&h, event + offset, sizeof(h));
        offset += sizeof(h);
        u16 tag = le16_to_cpu(h.tag), len = le16_to_cpu(h.len);
        if (len > eventLength - offset) return -1;
        if (tag == TAG_STRUCT_READY) {
            if (len < sizeof(WmiReadyEvent)) return -1;
            WmiReadyEvent ready;
            memcpy(&ready, event + offset, sizeof(ready));
            if (le32_to_cpu(ready.status) != 0) return -1;
            memcpy(mac, ready.mac, 6);
            return 0;
        }
        offset += len;
    }
    return -1;
}

int QCAWMI::sendInitCmd()
{
    u8 payload[1536] = {};
    u32 offset = 0;
    WmiInitCommand init = {};
    init.abi.ver0 = cpu_to_le32(WMI_TLV_ABI_VER0);
    init.abi.ver1 = cpu_to_le32(WMI_TLV_ABI_VER1);
    init.abi.ns0 = cpu_to_le32(WMI_TLV_ABI_NS0);
    init.abi.ns1 = cpu_to_le32(WMI_TLV_ABI_NS1);
    init.abi.ns2 = cpu_to_le32(WMI_TLV_ABI_NS2);
    init.abi.ns3 = cpu_to_le32(WMI_TLV_ABI_NS3);
    init.numChunks = cpu_to_le32(fMemChunkCount);
    appendTlv(payload, &offset, TAG_STRUCT_INIT, &init, sizeof(init));

    WmiResourceConfig cfg = {};
    cfg.numVdevs = cpu_to_le32(4);
    cfg.numPeers = cpu_to_le32(33);
    cfg.numPeerKeys = cpu_to_le32(2);
    cfg.numTids = cpu_to_le32(66);
    cfg.astSkid = cpu_to_le32(0x10);
    cfg.txChainMask = cpu_to_le32(1);
    cfg.rxChainMask = cpu_to_le32(1);
    cfg.rxDecap = cpu_to_le32(WMI_RX_DECAP_NATIVE_WIFI);
    for (unsigned i = 0; i < 3; ++i) cfg.rxTimeout[i] = cpu_to_le32(0x64);
    cfg.rxTimeout[3] = cpu_to_le32(0x28);
    cfg.scanPending = cpu_to_le32(4);
    cfg.bmissVdevs = cpu_to_le32(4);
    cfg.roamVdevs = cpu_to_le32(4);
    cfg.roamProfiles = cpu_to_le32(8);
    cfg.txLogSize = cpu_to_le32(0x400);
    cfg.wdsEntries = cpu_to_le32(0x20);
    cfg.gtkVdevs = cpu_to_le32(2);
    cfg.msduDesc = cpu_to_le32(1024);
    cfg.maxFragEntries = cpu_to_le32(2);
    cfg.tdlsVdevs = cpu_to_le32(4);
    cfg.tdlsEntries = cpu_to_le32(0x20);
    cfg.beaconVdevs = cpu_to_le32(2);
    cfg.multicastFilters = cpu_to_le32(5);
    cfg.keepAlivePatterns = cpu_to_le32(6);
    appendTlv(payload, &offset, TAG_STRUCT_RESOURCE_CONFIG, &cfg, sizeof(cfg));

    u32 chunkBytes = fMemChunkCount * (sizeof(WmiTlvHeader) + sizeof(WmiHostMemChunk));
    WmiTlvHeader arrayHeader = { cpu_to_le16(TAG_ARRAY_STRUCT), cpu_to_le16(chunkBytes) };
    memcpy(payload + offset, &arrayHeader, sizeof(arrayHeader));
    offset += sizeof(arrayHeader);
    for (u32 i = 0; i < fMemChunkCount; ++i) {
        WmiHostMemChunk chunk = {};
        u64 paddr = fMemChunks[i]->getPhysicalAddress();
        chunk.reqId = cpu_to_le32(fMemReqIds[i]);
        chunk.addressLow = cpu_to_le32((u32)paddr);
        chunk.size = cpu_to_le32((u32)fMemChunks[i]->getLength());
        chunk.addressHigh = cpu_to_le32((u32)(paddr >> 32));
        appendTlv(payload, &offset, TAG_STRUCT_HOST_MEM_CHUNK, &chunk, sizeof(chunk));
    }
    qca_info("Sending WMI-TLV INIT with %u firmware memory chunks", fMemChunkCount);
    return sendWmiCmd(WMI_INIT_CMDID, payload, offset);
}

int QCAWMI::createVdev(u32 vdevId, const u8 *mac)
{
    if (!mac) return -1;
    qca_info("WMI: creating VDEV %u with MAC %02x:%02x:%02x:%02x:%02x:%02x",
             vdevId, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    WmiVdevCreate cmd = {};
    cmd.id = cpu_to_le32(vdevId);
    cmd.type = cpu_to_le32(2); /* WMI_VDEV_TYPE_STA */
    memcpy(cmd.mac.address, mac, 6);
    u8 tlv[sizeof(WmiTlvHeader) + sizeof(cmd)];
    u32 offset = 0;
    appendTlv(tlv, &offset, TAG_STRUCT_VDEV_CREATE, &cmd, sizeof(cmd));
    return sendWmiCmd(WMI_VDEV_CREATE_CMDID, tlv, offset);
}

int QCAWMI::startVdev(u32 vdevId, u32 channel, u32 chWidth,
                      const u8 *ssid, u32 ssidLength)
{
    qca_info("WMI: starting VDEV %u on channel %u (width %u MHz)", vdevId, channel, chWidth);
    if (!channel || channel > 196 || ssidLength > 32 ||
        (ssidLength && !ssid)) return -1;
    WmiVdevStart cmd = {};
    cmd.vdevId = cpu_to_le32(vdevId);
    cmd.beaconInterval = cpu_to_le32(100);
    cmd.dtimPeriod = cpu_to_le32(1);
    cmd.ssid.length = cpu_to_le32(ssidLength);
    if (ssidLength) memcpy(cmd.ssid.bytes, ssid, ssidLength);
    WmiChannel chan = {};
    u32 frequency = channel <= 13 ? 2407 + channel * 5 : (channel == 14 ? 2484 : 5000 + channel * 5);
    u32 mode;
    if (channel <= 14) mode = chWidth >= 80 ? 13 : chWidth >= 40 ? 12 : 11;
    else mode = chWidth >= 80 ? 10 : chWidth >= 40 ? 6 : 4;
    chan.frequency = cpu_to_le32(frequency);
    chan.center1 = cpu_to_le32(frequency);
    chan.flags = cpu_to_le32(mode);
    chan.regInfo0 = cpu_to_le32(40U | (40U << 8) | (0U << 16));
    chan.regInfo1 = cpu_to_le32(20U << 8);
    u8 tlv[sizeof(WmiTlvHeader) * 3 + sizeof(cmd) + sizeof(chan)];
    u32 offset = 0;
    appendTlv(tlv, &offset, TAG_STRUCT_VDEV_START, &cmd, sizeof(cmd));
    appendTlv(tlv, &offset, TAG_STRUCT_CHANNEL, &chan, sizeof(chan));
    appendTlv(tlv, &offset, TAG_ARRAY_STRUCT, nullptr, 0);
    if (sendWmiCmd(WMI_VDEV_START_REQUEST_CMDID, tlv, offset) != 0) return -1;

    /* VDEV start is asynchronous in WMI-TLV. Do not create/associate the peer
     * until firmware confirms that it accepted this channel configuration. */
    u8 response[512];
    u32 responseLength = sizeof(response);
    if (receiveEvent(WMI_EVENT_VDEV_START_RESPONSE, response, &responseLength) != 0) {
        qca_err("Timed out waiting for WMI VDEV start response");
        return -1;
    }
    bool foundResponse = false;
    u32 responseStatus = 0xFFFFFFFFU;
    for (u32 cursor = 0; cursor + sizeof(WmiTlvHeader) <= responseLength;) {
        WmiTlvHeader header;
        memcpy(&header, response + cursor, sizeof(header));
        cursor += sizeof(header);
        const u16 tag = le16_to_cpu(header.tag);
        const u16 length = le16_to_cpu(header.len);
        if (length > responseLength - cursor) break;
        if (tag == TAG_STRUCT_VDEV_START_RESPONSE && length >= 16) {
            const u32 responseVdev = readLE32(response + cursor);
            responseStatus = readLE32(response + cursor + 12);
            foundResponse = responseVdev == vdevId;
            break;
        }
        cursor += length;
    }
    if (!foundResponse || responseStatus != 0) {
        qca_err("Firmware rejected VDEV %u start (status %u)", vdevId, responseStatus);
        return -1;
    }
    return 0;
}

int QCAWMI::stopVdev(u32 vdevId)
{
    qca_info("WMI: stopping VDEV %u", vdevId);
    WmiVdevStop cmd = { cpu_to_le32(vdevId) };
    u8 tlv[sizeof(WmiTlvHeader) + sizeof(cmd)];
    u32 offset = 0;
    appendTlv(tlv, &offset, TAG_STRUCT_VDEV_STOP, &cmd, sizeof(cmd));
    return sendWmiCmd(WMI_VDEV_STOP_CMDID, tlv, offset);
}

int QCAWMI::startScan(u32 scanType)
{
    (void)scanType;
    static const u8 channels[] = {
        1,2,3,4,5,6,7,8,9,10,11,12,13,14,
        36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,
        132,136,140,144,149,153,157,161,165
    };
    __le32 channelFrequencies[sizeof(channels)];
    for (u32 i = 0; i < sizeof(channels); ++i) {
        u32 c = channels[i];
        channelFrequencies[i] = cpu_to_le32(c <= 13 ? 2407 + c * 5 : c == 14 ? 2484 : 5000 + c * 5);
    }
    WmiStartScan cmd = {};
    cmd.common.scanId = cpu_to_le32(0xA001);
    cmd.common.requestorId = cpu_to_le32(0xA001);
    cmd.common.vdevId = 0;
    cmd.common.priority = cpu_to_le32(1);
    cmd.common.notifyEvents = cpu_to_le32(0x11F);
    cmd.common.activeDwell = cpu_to_le32(50);
    cmd.common.passiveDwell = cpu_to_le32(150);
    cmd.common.minRest = cpu_to_le32(50);
    cmd.common.maxRest = cpu_to_le32(500);
    cmd.common.maxScanTime = cpu_to_le32(20000);
    cmd.common.probeDelay = cpu_to_le32(5);
    cmd.common.scanFlags = cpu_to_le32(0x30);
    cmd.numChannels = cpu_to_le32(sizeof(channels));
    cmd.numBssids = cpu_to_le32(1);
    cmd.numProbes = cpu_to_le32(3);
    memset(cmd.mac.address, 0xff, sizeof(cmd.mac.address));
    memset(cmd.mask.address, 0xff, sizeof(cmd.mask.address));

    u8 payload[sizeof(WmiTlvHeader) * 5 + sizeof(cmd) + sizeof(channelFrequencies) + sizeof(WmiMacAddress)] = {};
    u32 offset = 0;
    appendTlv(payload, &offset, TAG_STRUCT_START_SCAN, &cmd, sizeof(cmd));
    appendTlv(payload, &offset, TAG_ARRAY_UINT32, channelFrequencies, sizeof(channelFrequencies));
    appendTlv(payload, &offset, TAG_ARRAY_FIXED_STRUCT, nullptr, 0); /* SSID list */
    appendTlv(payload, &offset, TAG_ARRAY_FIXED_STRUCT, &cmd.mac, sizeof(cmd.mac));
    appendTlv(payload, &offset, TAG_ARRAY_BYTE, nullptr, 0); /* probe request IEs */
    qca_info("WMI-TLV: starting active scan over %u channels", (u32)sizeof(channels));
    return sendWmiCmd(WMI_START_SCAN_CMDID, payload, offset);
}

int QCAWMI::createPeer(u32 vdevId, const u8 *peerMac)
{
    if (!peerMac) return -1;
    qca_info("WMI: creating peer %02x:%02x:%02x:%02x:%02x:%02x for VDEV %u",
             peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5], vdevId);
    WmiPeerCreate cmd = {};
    cmd.vdevId = cpu_to_le32(vdevId);
    memcpy(cmd.mac.address, peerMac, 6);
    cmd.type = cpu_to_le32(1); /* BSS peer */
    u8 tlv[sizeof(WmiTlvHeader) + sizeof(cmd)];
    u32 offset = 0;
    appendTlv(tlv, &offset, TAG_STRUCT_PEER_CREATE, &cmd, sizeof(cmd));
    return sendWmiCmd(WMI_PEER_CREATE_CMDID, tlv, offset);
}

int QCAWMI::associatePeer(u32 vdevId, const u8 *peerMac, apple80211_assoc_data *assoc)
{
    if (!peerMac || !assoc) return -1;
    qca_info("WMI: associating peer for VDEV %u", vdevId);
    WmiPeerAssoc cmd = {};
    memcpy(cmd.mac.address, peerMac, 6);
    cmd.vdevId = cpu_to_le32(vdevId);
    cmd.newAssociation = cpu_to_le32(1);
    cmd.associationId = cpu_to_le32(1);
    cmd.listenInterval = cpu_to_le32(10);
    cmd.nss = cpu_to_le32(1);
    WmiVhtRates vht = {};
    u8 payload[sizeof(WmiTlvHeader) * 4 + sizeof(cmd) + sizeof(vht)] = {};
    u32 offset = 0;
    appendTlv(payload, &offset, TAG_STRUCT_PEER_ASSOC, &cmd, sizeof(cmd));
    appendTlv(payload, &offset, TAG_ARRAY_BYTE, nullptr, 0); /* no legacy rates advertised */
    appendTlv(payload, &offset, TAG_ARRAY_BYTE, nullptr, 0); /* no HT rates advertised */
    appendTlv(payload, &offset, TAG_STRUCT_VHT_RATE_SET, &vht, sizeof(vht));
    if (sendWmiCmd(WMI_PEER_ASSOC_CMDID, payload, offset) != 0) return -1;
    WmiVdevUp up = {};
    up.id = cpu_to_le32(vdevId);
    up.associationId = cpu_to_le32(1);
    memcpy(up.bssid.address, peerMac, 6);
    u8 upTlv[sizeof(WmiTlvHeader) + sizeof(up)];
    u32 upOffset = 0;
    appendTlv(upTlv, &upOffset, TAG_STRUCT_VDEV_UP, &up, sizeof(up));
    return sendWmiCmd(WMI_VDEV_UP_CMDID, upTlv, upOffset);
}

int QCAWMI::installKey(u32 vdevId, apple80211_key *key)
{
    if (!key || !key->key_len || key->key_len > 32) return -1;
    qca_info("WMI: installing key index %u, len %u into hardware AES engine", key->key_index, key->key_len);
    u32 cipher;
    switch (key->key_cipher_type) {
        case APPLE80211_CIPHER_WEP_40:
        case APPLE80211_CIPHER_WEP_104: cipher = 1; break;
        case APPLE80211_CIPHER_TKIP: cipher = 2; break;
        case APPLE80211_CIPHER_AES_CCM: cipher = 4; break;
        case APPLE80211_CIPHER_AES_GCM: cipher = 9; break;
        default: qca_err("Unsupported Apple80211 cipher %u", key->key_cipher_type); return -1;
    }
    WmiInstallKey cmd = {};
    cmd.vdevId = cpu_to_le32(vdevId);
    memcpy(cmd.peer.address, key->key_ea.octet, 6);
    cmd.index = cpu_to_le32(key->key_index);
    cmd.flags = cpu_to_le32((key->key_ea.octet[0] & 1) ? 1 : 0);
    cmd.cipher = cpu_to_le32(cipher);
    cmd.keyLength = cpu_to_le32(key->key_len);
    if (key->key_rsc_len) {
        u64 rsc = 0;
        u32 rscLen = key->key_rsc_len < 8 ? key->key_rsc_len : 8;
        for (u32 i = 0; i < rscLen; ++i) rsc |= (u64)key->key_rsc[i] << (i * 8);
        cmd.rsc.low = cpu_to_le32((u32)rsc);
        cmd.rsc.high = cpu_to_le32((u32)(rsc >> 32));
    }
    u32 paddedKeyLength = (key->key_len + 3) & ~3U;
    u8 payload[sizeof(WmiTlvHeader) * 2 + sizeof(cmd) + 32] = {};
    u32 offset = 0;
    appendTlv(payload, &offset, TAG_STRUCT_INSTALL_KEY, &cmd, sizeof(cmd));
    WmiTlvHeader keyTlv = { cpu_to_le16(TAG_ARRAY_BYTE), cpu_to_le16(paddedKeyLength) };
    memcpy(payload + offset, &keyTlv, sizeof(keyTlv));
    offset += sizeof(keyTlv);
    memcpy(payload + offset, key->key, key->key_len);
    offset += paddedKeyLength;
    return sendWmiCmd(WMI_VDEV_INSTALL_KEY_CMDID, payload, offset);
}
