//
//  QCACopyEngine.cpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//

#include "QCACopyEngine.hpp"
#include "QCAPCIDevice.hpp"

/* QCA9377 Pipe Offsets */
static const u32 ce_base_addresses[CE_COUNT] = {
    CE0_BASE_ADDRESS,
    CE1_BASE_ADDRESS,
    CE2_BASE_ADDRESS,
    CE3_BASE_ADDRESS,
    CE4_BASE_ADDRESS,
    CE5_BASE_ADDRESS,
    CE6_BASE_ADDRESS,
    CE7_BASE_ADDRESS
};

/* Host pipe layout used by ath10k for QCA6174/QCA9377 PCI. */
static const CEPipeConfig qca9377_ce_configs[CE_COUNT] = {
    { 0, 16, 0, 256 },       /* BMI/HTC host to target */
    { 1, 0, 512, 2048 },     /* HTC/HTT target to host */
    { 2, 0, 128, 2048 },     /* WMI events */
    { 3, 32, 0, 2048 },     /* WMI commands */
    { 4, 8192, 0, 256 },    /* HTT messages */
    { 5, 0, 512, 512 },     /* HTT receive */
    { 6, 0, 0, 0 },
    { 7, 2, 2, 2048 }       /* diagnostic window */
};

QCACopyEngine::QCACopyEngine(QCAPCIDevice *pci)
    : fPci(pci)
{
    memset(fPipes, 0, sizeof(fPipes));
}

QCACopyEngine::~QCACopyEngine()
{
    stopPipes();
}

bool QCACopyEngine::initPipes()
{
    qca_info("Initializing Copy Engine pipes with QCA9377 overrides");

    for (int i = 0; i < CE_COUNT; i++) {
        CEPipe &pipe = fPipes[i];
        const CEPipeConfig &cfg = qca9377_ce_configs[i];

        pipe.pipeId = cfg.pipeNum;
        pipe.baseOffset = ce_base_addresses[i];
        pipe.srcEntries = cfg.srcEntries;
        pipe.destEntries = cfg.destEntries;
        pipe.maxBufSize = cfg.maxBufSize;

        /* Allocate Source Ring if entries > 0 */
        if (pipe.srcEntries > 0) {
            size_t ringSize = pipe.srcEntries * sizeof(struct ce_desc);
            pipe.srcRingMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                kernel_task,
                kIODirectionInOut | kIOMemoryHostPhysicallyContiguous,
                ringSize,
                0x00000000FFFFFFFFULL /* 32-bit DMA Mask */
            );
            if (!pipe.srcRingMem) {
                qca_err("Failed to allocate CE%d SRC ring", i);
                return false;
            }
            pipe.srcRingMem->prepare();
            pipe.srcRing = (struct ce_desc*)pipe.srcRingMem->getBytesNoCopy();
            pipe.srcRingPhys = pipe.srcRingMem->getPhysicalAddress();
            if (pipe.srcRingPhys > 0xffffffffULL) return false;
            memset(pipe.srcRing, 0, ringSize);
        }

        /* Allocate Destination Ring if entries > 0 */
        if (pipe.destEntries > 0) {
            size_t ringSize = pipe.destEntries * sizeof(struct ce_desc);
            pipe.dstRingMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                kernel_task,
                kIODirectionInOut | kIOMemoryHostPhysicallyContiguous,
                ringSize,
                0x00000000FFFFFFFFULL
            );
            if (!pipe.dstRingMem) {
                qca_err("Failed to allocate CE%d DST ring", i);
                return false;
            }
            pipe.dstRingMem->prepare();
            pipe.dstRing = (struct ce_desc*)pipe.dstRingMem->getBytesNoCopy();
            pipe.dstRingPhys = pipe.dstRingMem->getPhysicalAddress();
            if (pipe.dstRingPhys > 0xffffffffULL) return false;
            memset(pipe.dstRing, 0, ringSize);

            size_t buffersSize = (size_t)pipe.destEntries * pipe.maxBufSize;
            pipe.dstBufferMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                kernel_task, kIODirectionInOut | kIOMemoryHostPhysicallyContiguous,
                buffersSize, 0x00000000FFFFFFFFULL);
            if (!pipe.dstBufferMem) return false;
            if (pipe.dstBufferMem->prepare() != kIOReturnSuccess) return false;
            pipe.dstBuffer = (UInt8 *)pipe.dstBufferMem->getBytesNoCopy();
            pipe.dstBufferPhys = pipe.dstBufferMem->getPhysicalAddress();
            if (!pipe.dstBuffer || pipe.dstBufferPhys + buffersSize > 0x100000000ULL) return false;
            memset(pipe.dstBuffer, 0, buffersSize);
        }


    }

    qca_info("Copy Engine pipes successfully initialized");
    return true;
}

void QCACopyEngine::startPipes()
{
    for (unsigned i = 0; i < CE_COUNT; ++i) {
        CEPipe &pipe = fPipes[i];
        if (!pipe.srcEntries && !pipe.destEntries) continue;
        const u32 base = pipe.baseOffset;
        fPci->write32(base + CE_SRC_RING_BASE_ADDR_OFFSET, (u32)pipe.srcRingPhys);
        fPci->write32(base + CE_SRC_RING_SZ_OFFSET, pipe.srcEntries);
        fPci->write32(base + CE_DST_RING_BASE_ADDR_OFFSET, (u32)pipe.dstRingPhys);
        fPci->write32(base + CE_DST_RING_SZ_OFFSET, pipe.destEntries);
        u32 ctrl = pipe.maxBufSize & CE_CTRL1_DMAX_MASK;
        if (pipe.srcEntries) ctrl |= CE_CTRL1_SRC_RING_ENABLE;
        if (pipe.destEntries) ctrl |= CE_CTRL1_DST_RING_ENABLE;
        fPci->write32(base + CE_CTRL1_OFFSET, ctrl);
        pipe.srcWrIdx = pipe.srcRdIdx = 0;
        pipe.dstRdIdx = 0;
        pipe.dstWrIdx = 0;
        if (pipe.destEntries) {
            /* Leave one slot empty to distinguish a full ring from an empty one. */
            const u32 postCount = pipe.destEntries - 1;
            for (u32 n = 0; n < postCount; ++n) {
                pipe.dstRing[n].addr = cpu_to_le32((u32)(pipe.dstBufferPhys + (size_t)n * pipe.maxBufSize));
                pipe.dstRing[n].nbytes = 0;
                pipe.dstRing[n].flags = 0;
            }
            pipe.dstWrIdx = postCount;
            fPci->write32(base + CE_DST_WR_INDEX_OFFSET, pipe.dstWrIdx);
        }
        fPci->write32(base + CE_SRC_WR_INDEX_OFFSET, 0);
    }
}

void QCACopyEngine::stopPipes()
{
    for (int i = 0; i < CE_COUNT; i++) {
        CEPipe &pipe = fPipes[i];
        if (pipe.srcRingMem) {
            pipe.srcRingMem->complete();
            pipe.srcRingMem->release();
            pipe.srcRingMem = nullptr;
        }
        if (pipe.dstRingMem) {
            pipe.dstRingMem->complete();
            pipe.dstRingMem->release();
            pipe.dstRingMem = nullptr;
        }
        if (pipe.dstBufferMem) {
            pipe.dstBufferMem->complete();
            pipe.dstBufferMem->release();
            pipe.dstBufferMem = nullptr;
            pipe.dstBuffer = nullptr;
        }
    }
}

int QCACopyEngine::send(u32 pipeId, void *buf, u32 len)
{
    if (pipeId >= CE_COUNT || !buf || !len) return -1;
    CEPipe &pipe = fPipes[pipeId];
    if (!pipe.srcEntries || !pipe.srcRing || len > pipe.maxBufSize) return -1;

    auto *dma = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryHostPhysicallyContiguous,
        len, 0x00000000FFFFFFFFULL);
    if (!dma || dma->prepare() != kIOReturnSuccess) {
        if (dma) dma->release();
        return -1;
    }
    memcpy(dma->getBytesNoCopy(), buf, len);
    dma_addr_t paddr = dma->getPhysicalAddress();
    if (paddr > 0xffffffffULL) { dma->complete(); dma->release(); return -1; }

    const u32 base = pipe.baseOffset;
    const u32 next = (pipe.srcWrIdx + 1) % pipe.srcEntries;
    int timeout = 1000;
    while (next == fPci->read32(base + CE_SRC_RD_INDEX_OFFSET) && timeout-- > 0)
        qca_mdelay(1);
    if (timeout <= 0) { dma->complete(); dma->release(); return -1; }

    struct ce_desc &desc = pipe.srcRing[pipe.srcWrIdx];
    desc.addr = cpu_to_le32((u32)paddr);
    desc.nbytes = cpu_to_le16((u16)len);
    desc.flags = 0;
    OSSynchronizeIO();
    pipe.srcWrIdx = next;
    fPci->write32(base + CE_SRC_WR_INDEX_OFFSET, next);

    timeout = 1000;
    while (fPci->read32(base + CE_SRC_RD_INDEX_OFFSET) != next && timeout-- > 0)
        qca_mdelay(1);
    dma->complete();
    dma->release();
    return timeout > 0 ? 0 : -1;
}

int QCACopyEngine::recv(u32 pipeId, void *buf, u32 *len)
{
    if (pipeId >= CE_COUNT || !buf || !len || !*len) return -1;
    CEPipe &pipe = fPipes[pipeId];
    if (!pipe.destEntries || !pipe.dstRing || !pipe.dstBuffer) return -1;

    const u32 base = pipe.baseOffset;
    struct ce_desc &desc = pipe.dstRing[pipe.dstRdIdx];
    /* RX is polled by the driver work loop; keep this operation bounded. */
    int timeout = 1;
    while (le16_to_cpu(desc.nbytes) == 0 && timeout-- > 0)
        qca_mdelay(1);
    if (timeout <= 0) return -1;

    const u32 received = le16_to_cpu(desc.nbytes);
    if (received > pipe.maxBufSize) return -1;
    const u32 copyLen = received < *len ? received : *len;
    UInt8 *slot = pipe.dstBuffer + (size_t)pipe.dstRdIdx * pipe.maxBufSize;
    OSSynchronizeIO();
    memcpy(buf, slot, copyLen);
    *len = received;
    desc.nbytes = 0;
    desc.flags = 0;
    pipe.dstRdIdx = (pipe.dstRdIdx + 1) % pipe.destEntries;

    /* Repost the consumed slot at the tail of the destination ring. */
    u32 write = pipe.dstWrIdx;
    struct ce_desc &tail = pipe.dstRing[write];
    tail.addr = cpu_to_le32((u32)(pipe.dstBufferPhys + (size_t)write * pipe.maxBufSize));
    tail.nbytes = 0;
    tail.flags = 0;
    OSSynchronizeIO();
    pipe.dstWrIdx = (write + 1) % pipe.destEntries;
    fPci->write32(base + CE_DST_WR_INDEX_OFFSET, pipe.dstWrIdx);
    return received <= copyLen ? 0 : -1;
}
