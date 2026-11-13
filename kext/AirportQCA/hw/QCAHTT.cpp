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
static_assert(sizeof(htt_tx_buffer) == 40, "ath10k QCA9377 HTT TX buffer ABI mismatch");

QCAHTT::QCAHTT(QCAPCIDevice *pci) : fPci(pci), fNextMsduId(0), fLock(IOLockAlloc())
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
}

bool QCAHTT::init()
{
    if (!fLock) return false;
    qca_info("Initializing QCA9377 HTT 32-bit data plane");
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
