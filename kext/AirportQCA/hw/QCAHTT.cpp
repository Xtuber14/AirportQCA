//
// QCAHTT.cpp - QCA9377 HTT 3.x 32-bit transmit/receive plumbing.
//
#include "QCAHTT.hpp"
#include "QCAPCIDevice.hpp"
#include "QCACopyEngine.hpp"
#include "QCAHTC.hpp"
#include <IOKit/network/IOOutputQueue.h>

static const u8 HTT_H2T_TX_FRAME = 1;
static const u8 HTT_T2H_TX_COMPLETE = 7;
static const u32 HTT_TX_TYPE_ETHERNET = 2;
static const u32 HTT_TX_EXT_TID_NON_QOS = 16;
static const u32 HTT_TX_PEER_INVALID = 0xFFFF;
static const u32 HTT_TX_PREFETCH_BYTES = 128;
static const u32 HTT_DESC_MAC_HEADER_PRESENT = 1U << 0;
static const u32 HTT_DESC_POSTPONED = 1U << 11;
static const u32 HTT_RX_RING_SIZE = 2048;
static const u32 HTT_RX_RING_FILL_LEVEL = (HTT_RX_RING_SIZE / 2) - 1;
static const u32 HTT_RX_BUFFER_SIZE = 2048;
static const u16 HTT_RX_RING_FLAGS = 0xFFFF;

static IOBufferMemoryDescriptor *allocatePreparedDMA(size_t length)
{
    IOBufferMemoryDescriptor *memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryHostPhysicallyContiguous,
        length, 0x00000000FFFFFFFFULL);
    if (!memory) return nullptr;
    if (memory->prepare() != kIOReturnSuccess) {
        memory->release();
        return nullptr;
    }
    return memory;
}

struct __attribute__((packed)) htt_rx_desc_offsets32 {
    __le16 mac80211Header;
    __le16 msduPayload;
    __le16 ppduStart;
    __le16 ppduEnd;
    __le16 mpduStart;
    __le16 mpduEnd;
    __le16 msduStart;
    __le16 msduEnd;
    __le16 attention;
    __le16 fragInfo;
};
struct __attribute__((packed)) htt_rx_ring32 {
    __le32 fwIndexAddress;
    __le32 ringAddress;
    __le16 ringLength;
    __le16 bufferSize;
    __le16 flags;
    __le16 initialIndex;
    htt_rx_desc_offsets32 offsets;
};
struct __attribute__((packed)) htt_rx_ring_setup32 {
    u8 ringCount;
    __le16 reserved;
    htt_rx_ring32 ring;
};
struct __attribute__((packed)) htt_rx_ring_command32 {
    u8 messageType;
    htt_rx_ring_setup32 setup;
};
static_assert(sizeof(htt_tx_buffer) == 40, "ath10k QCA9377 HTT TX buffer ABI mismatch");
static_assert(sizeof(htt_rx_ring32) == 36, "ath10k QCA9377 HTT RX ring ABI mismatch");
static_assert(sizeof(htt_rx_ring_command32) == 40, "ath10k QCA9377 HTT RX setup ABI mismatch");

QCAHTT::QCAHTT(QCAPCIDevice *pci)
    : fPci(pci), fNextMsduId(0), fLock(IOLockAlloc()), fRxRingMemory(nullptr),
      fRxBufferMemory(nullptr), fRxIndexMemory(nullptr), fRxFillIndex(0)
{
    memset(fPending, 0, sizeof(fPending));
}

QCAHTT::~QCAHTT()
{
    if (fLock) IOLockLock(fLock);
    for (u32 i = 0; i < 256; ++i) {
        if (fPending[i]) {
            fPending[i]->complete();
            fPending[i]->release();
            fPending[i] = nullptr;
        }
    }
    if (fLock) {
        IOLockUnlock(fLock);
        IOLockFree(fLock);
        fLock = nullptr;
    }
    if (fRxRingMemory) { fRxRingMemory->complete(); fRxRingMemory->release(); fRxRingMemory = nullptr; }
    if (fRxBufferMemory) { fRxBufferMemory->complete(); fRxBufferMemory->release(); fRxBufferMemory = nullptr; }
    if (fRxIndexMemory) { fRxIndexMemory->complete(); fRxIndexMemory->release(); fRxIndexMemory = nullptr; }
}

bool QCAHTT::init()
{
    if (!fLock) return false;
    QCACopyEngine *ce = fPci->getCopyEngine();
    QCAHTC *htc = fPci->getHTC();
    if (!ce || !htc) return false;

    /* Negotiate the HTT ABI before submitting setup commands. */
    const u8 versionRequest[4] = { 0, 0, 0, 0 };
    if (htc->send(ATH10K_HTC_EP_2, (void *)versionRequest, sizeof(versionRequest)) != 0) {
        qca_err("Unable to send HTT version request");
        return false;
    }
    bool versionReceived = false;
    u8 response[64];
    for (u32 retry = 0; retry < 3000 && !versionReceived; ++retry) {
        u32 length = sizeof(response);
        if (ce->recv(5, response, &length) == 0) {
            if (length < sizeof(ath10k_htc_hdr) + 4) continue;
            const auto *transport = (const ath10k_htc_hdr *)response;
            const u8 *payload = response + sizeof(*transport);
            if (transport->eid != ATH10K_HTC_EP_2 || payload[0] != 0) continue;
            const u8 major = payload[2];
            if (major != 2 && major != 3) {
                qca_err("Unsupported HTT target version %u.%u", major, payload[1]);
                return false;
            }
            versionReceived = true;
            qca_info("HTT target protocol version %u.%u", major, payload[1]);
        } else {
            qca_mdelay(1);
        }
    }
    if (!versionReceived) {
        qca_err("Timed out waiting for HTT version response");
        return false;
    }

    const u32 ringBytes = HTT_RX_RING_SIZE * sizeof(__le32);
    const u32 bufferBytes = HTT_RX_RING_SIZE * HTT_RX_BUFFER_SIZE;
    fRxRingMemory = allocatePreparedDMA(ringBytes);
    fRxBufferMemory = allocatePreparedDMA(bufferBytes);
    fRxIndexMemory = allocatePreparedDMA(sizeof(__le32));
    if (!fRxRingMemory || !fRxBufferMemory || !fRxIndexMemory) {
        qca_err("Unable to allocate/prepare 32-bit HTT RX DMA memory");
        return false;
    }
    const u64 ringPaddr = fRxRingMemory->getPhysicalAddress();
    const u64 buffersPaddr = fRxBufferMemory->getPhysicalAddress();
    const u64 indexPaddr = fRxIndexMemory->getPhysicalAddress();
    if (!fRxRingMemory->getBytesNoCopy() || !fRxBufferMemory->getBytesNoCopy() ||
        !fRxIndexMemory->getBytesNoCopy() || ringPaddr + ringBytes > 0x100000000ULL ||
        buffersPaddr + bufferBytes > 0x100000000ULL || indexPaddr + sizeof(__le32) > 0x100000000ULL) {
        qca_err("HTT RX DMA allocation is outside the QCA9377 32-bit address window");
        return false;
    }
    auto *ring = (volatile __le32 *)fRxRingMemory->getBytesNoCopy();
    memset((void *)ring, 0, ringBytes);
    for (u32 i = 0; i < HTT_RX_RING_FILL_LEVEL; ++i) {
        ring[i] = cpu_to_le32((u32)(buffersPaddr + (u64)i * HTT_RX_BUFFER_SIZE));
    }
    auto *index = (__le32 *)fRxIndexMemory->getBytesNoCopy();
    *index = cpu_to_le32(HTT_RX_RING_FILL_LEVEL);
    fRxFillIndex = HTT_RX_RING_FILL_LEVEL;

    htt_rx_ring_command32 command = {};
    command.messageType = 2; /* HTT_H2T_MSG_TYPE_RX_RING_CFG */
    command.setup.ringCount = 1;
    command.setup.ring.fwIndexAddress = cpu_to_le32((u32)indexPaddr);
    command.setup.ring.ringAddress = cpu_to_le32((u32)ringPaddr);
    command.setup.ring.ringLength = cpu_to_le16(HTT_RX_RING_SIZE);
    command.setup.ring.bufferSize = cpu_to_le16(HTT_RX_BUFFER_SIZE);
    command.setup.ring.flags = cpu_to_le16(HTT_RX_RING_FLAGS);
    command.setup.ring.initialIndex = cpu_to_le16((u16)fRxFillIndex);
    /* QCA988x/QCA9377 htt_rx_desc_v1 offsets from ath10k rx_desc.h. */
    command.setup.ring.offsets.mac80211Header = cpu_to_le16(59);
    command.setup.ring.offsets.msduPayload = cpu_to_le16(75);
    command.setup.ring.offsets.ppduStart = cpu_to_le16(21);
    command.setup.ring.offsets.ppduEnd = cpu_to_le16(31);
    command.setup.ring.offsets.mpduStart = cpu_to_le16(3);
    command.setup.ring.offsets.mpduEnd = cpu_to_le16(20);
    command.setup.ring.offsets.msduStart = cpu_to_le16(6);
    command.setup.ring.offsets.msduEnd = cpu_to_le16(10);
    command.setup.ring.offsets.attention = cpu_to_le16(1);
    command.setup.ring.offsets.fragInfo = cpu_to_le16(2);
    OSSynchronizeIO();
    if (htc->send(ATH10K_HTC_EP_2, &command, sizeof(command)) != 0) {
        qca_err("Unable to configure QCA9377 HTT RX ring");
        return false;
    }
    qca_info("Configured QCA9377 HTT RX ring: %u entries, %u initially posted",
             HTT_RX_RING_SIZE, HTT_RX_RING_FILL_LEVEL);
    return true;
}

int QCAHTT::transmit(mbuf_t m, bool raw80211)
{
    if (!m || !fLock) return kIOReturnBadArgument;
    QCACopyEngine *ce = fPci->getCopyEngine();
    if (!ce) return kIOReturnNotReady;
    const u32 packetLength = (u32)mbuf_pkthdr_len(m);
    if (!packetLength || packetLength > 65535) return kIOReturnMessageTooLarge;

    /* The packet and fragment descriptor share a 32-bit contiguous DMA region. */
    const u32 prefetch = (packetLength < HTT_TX_PREFETCH_BYTES ? packetLength : HTT_TX_PREFETCH_BYTES) & ~3U;
    const u32 dmaLength = (u32)sizeof(htt_tx_buffer) + packetLength;
    IOBufferMemoryDescriptor *dma = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryHostPhysicallyContiguous,
        dmaLength, 0x00000000FFFFFFFFULL);
    if (!dma || dma->prepare() != kIOReturnSuccess) {
        if (dma) dma->release();
        return kIOReturnNoMemory;
    }
    auto *tx = (htt_tx_buffer *)dma->getBytesNoCopy();
    const u64 baseAddress = dma->getPhysicalAddress();
    if (!tx || baseAddress + dmaLength > 0x100000000ULL) {
        dma->complete(); dma->release();
        return kIOReturnNoResources;
    }
    memset(tx, 0, sizeof(*tx));
    mbuf_copydata(m, 0, packetLength, tx->packet);

    IOLockLock(fLock);
    u32 attempts = 0;
    u16 msduId = fNextMsduId;
    while (fPending[msduId] && attempts++ < 256) msduId = (u16)((msduId + 1) & 0xff);
    if (attempts >= 256) {
        IOLockUnlock(fLock);
        dma->complete(); dma->release();
        return kIOReturnNoResources;
    }
    fNextMsduId = (u16)((msduId + 1) & 0xff);
    fPending[msduId] = dma;
    IOLockUnlock(fLock);

    tx->fragments[0].address = cpu_to_le32((u32)(baseAddress + sizeof(*tx)));
    tx->fragments[0].length = cpu_to_le32(packetLength);
    tx->htc.eid = ATH10K_HTC_EP_2;
    tx->htc.flags = 0;
    tx->htc.len = cpu_to_le16((u16)(sizeof(tx->commandType) + sizeof(tx->descriptor) + prefetch));
    tx->commandType = HTT_H2T_TX_FRAME;
    tx->descriptor.flags0 = (u8)((raw80211 ? 0 : HTT_TX_TYPE_ETHERNET) << 5);
    if (raw80211) tx->descriptor.flags0 |= HTT_DESC_MAC_HEADER_PRESENT;
    tx->descriptor.flags1 = cpu_to_le16((u16)((0U & 0x3fU) |
        ((HTT_TX_EXT_TID_NON_QOS & 0x1fU) << 6) | HTT_DESC_POSTPONED));
    tx->descriptor.length = cpu_to_le16((u16)packetLength);
    tx->descriptor.msduId = cpu_to_le16(msduId);
    tx->descriptor.fragmentsAddress = cpu_to_le32((u32)baseAddress);
    tx->descriptor.peerId = cpu_to_le32(HTT_TX_PEER_INVALID);

    /* CE4 carries HTC + HTT metadata + a prefetched portion; the target reads
     * the complete MSDU through the 32-bit fragment descriptor. */
    const u32 ceLength = (u32)(sizeof(tx->htc) + sizeof(tx->commandType) + sizeof(tx->descriptor) + prefetch);
    int result = ce->send(4, &tx->htc, ceLength);
    if (result != 0) {
        IOLockLock(fLock);
        if (fPending[msduId] == dma) fPending[msduId] = nullptr;
        IOLockUnlock(fLock);
        dma->complete(); dma->release();
        return kIOReturnOutputDropped;
    }

    fPci->freePacket(m);
    return kIOReturnOutputSuccess;
}

UInt32 QCAHTT::transmitPacket(mbuf_t m)
{
    return static_cast<UInt32>(transmit(m, false));
}

int QCAHTT::transmitRaw80211(mbuf_t m)
{
    return transmit(m, true);
}

void QCAHTT::completeMsdu(u16 msduId)
{
    if (msduId >= 256 || !fLock) return;
    IOLockLock(fLock);
    IOBufferMemoryDescriptor *dma = fPending[msduId];
    fPending[msduId] = nullptr;
    IOLockUnlock(fLock);
    if (dma) {
        dma->complete();
        dma->release();
    }
}

void QCAHTT::receivePackets()
{
    QCACopyEngine *ce = fPci->getCopyEngine();
    if (!ce) return;
    for (u32 count = 0; count < 64; ++count) {
        u8 frame[2048];
        u32 frameLength = sizeof(frame);
        if (ce->recv(5, frame, &frameLength) != 0) break;
        if (frameLength < sizeof(ath10k_htc_hdr) + 4) continue;
        const auto *htc = (const ath10k_htc_hdr *)frame;
        if (htc->eid != ATH10K_HTC_EP_2) continue;
        const u8 *htt = frame + sizeof(*htc);
        u32 httLength = frameLength - sizeof(*htc);
        if (htt[0] != HTT_T2H_TX_COMPLETE || httLength < 4) continue;
        const u32 numMsdu = htt[2];
        if (numMsdu > 255 || httLength < 4 + numMsdu * sizeof(__le16)) continue;
        for (u32 i = 0; i < numMsdu; ++i) {
            __le16 rawId;
            memcpy(&rawId, htt + 4 + i * sizeof(rawId), sizeof(rawId));
            completeMsdu(le16_to_cpu(rawId));
        }
    }
}
