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
    { 5, 0, 0, 0 },          /* QCA6174: CE5 is used by the target itself */
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
            /* The CE descriptors must carry the address the device sees
               on the bus. This is an Intel host without a PCIe IOMMU, so
               the CPU physical address is the bus address; the value is
               recorded so a mismatch is visible in the next boot. */
            fPci->setDiagProp("QCACeSrcPhys", (u32)pipe.srcRingPhys);
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
            fPci->setDiagProp("QCACeDstPhys", (u32)pipe.dstRingPhys);

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
            fPci->setDiagProp("QCACeBufPhys", (u32)pipe.dstBufferPhys);
        }


    }

    qca_info("Copy Engine pipes successfully initialized");
    return true;
}

void QCACopyEngine::startPipes()
{
    /* Mirrors ath10k_ce_init_src_ring()/ath10k_ce_init_dest_ring():
       base address, ring size, dmax, byte swap cleared, lowmark 0 and
       highmark = number of entries. The byte-swap bits live in CTRL1
       (bit 16 source, bit 17 destination) and the ring watermarks live
       in dedicated registers at CE offset 0x4c / 0x50. */
    for (unsigned i = 0; i < CE_COUNT; ++i) {
        CEPipe &pipe = fPipes[i];
        if (!pipe.srcEntries && !pipe.destEntries) continue;
        const u32 base = pipe.baseOffset;

        /* The CTRL1 read-back tells whether the copy engine really
           presented its reset value (0x80) to the host or whether the
           target cleared it while it initialized the engine. */
        const u32 ctrl1Pre = fPci->read32(base + CE_CTRL1_OFFSET);

        if (pipe.srcEntries) {
            fPci->write32(base + CE_SRC_RING_BASE_ADDR_OFFSET, (u32)pipe.srcRingPhys);
            fPci->write32(base + CE_SRC_RING_SZ_OFFSET, pipe.srcEntries);

            /* CTRL1 is read-modify-written so that the reset value
               (0x80) and any hardware-set bits survive; byte swap must
               be cleared or every descriptor gets mangled. */
            u32 ctrl = fPci->read32(base + CE_CTRL1_OFFSET);
            ctrl &= ~(CE_CTRL1_DMAX_MASK | CE_CTRL1_SRC_BYTE_SWAP | CE_CTRL1_DST_BYTE_SWAP);
            ctrl |= (pipe.maxBufSize & CE_CTRL1_DMAX_MASK);
            fPci->write32(base + CE_CTRL1_OFFSET, ctrl);

            /* Source ring watermarks: lowmark 0, highmark nentries. */
            u32 wm = fPci->read32(base + CE_SRC_WM_OFFSET);
            wm &= ~CE_WM_LOW_MASK;
            wm = (wm & ~CE_WM_HIGH_MASK) | (pipe.srcEntries & CE_WM_HIGH_MASK);
            fPci->write32(base + CE_SRC_WM_OFFSET, wm);
        }

        if (pipe.destEntries) {
            fPci->write32(base + CE_DST_RING_BASE_ADDR_OFFSET, (u32)pipe.dstRingPhys);
            fPci->write32(base + CE_DST_RING_SZ_OFFSET, pipe.destEntries);

            u32 ctrl = fPci->read32(base + CE_CTRL1_OFFSET);
            ctrl &= ~CE_CTRL1_DST_BYTE_SWAP;
            fPci->write32(base + CE_CTRL1_OFFSET, ctrl);

            /* Destination ring watermarks: lowmark 0, highmark nentries. */
            u32 wm = fPci->read32(base + CE_DST_WM_OFFSET);
            wm &= ~CE_WM_LOW_MASK;
            wm = (wm & ~CE_WM_HIGH_MASK) | (pipe.destEntries & CE_WM_HIGH_MASK);
            fPci->write32(base + CE_DST_WM_OFFSET, wm);
        }

        pipe.srcWrIdx = 0;
        pipe.srcRdIdx = 0;
        pipe.dstRdIdx = 0;
        pipe.dstWrIdx = 0;
        if (pipe.destEntries) {
            if (i == CE_DIAG_PIPE) {
                /* The diagnostic window owns its destination ring:
                   ath10k posts the bounce buffer for every transfer, so
                   no buffer is queued up front. */
                memset(pipe.dstRing, 0, pipe.destEntries * sizeof(struct ce_desc));
                OSSynchronizeIO();
                pipe.dstWrIdx = 0;
                fPci->write32(base + CE_DST_WR_INDEX_OFFSET, 0);
            } else {
                /* Leave one slot empty to distinguish a full ring from an empty one. */
                const u32 postCount = pipe.destEntries - 1;
                for (u32 n = 0; n < postCount; ++n) {
                    pipe.dstRing[n].addr = cpu_to_le32((u32)(pipe.dstBufferPhys + (size_t)n * pipe.maxBufSize));
                    pipe.dstRing[n].nbytes = 0;
                    pipe.dstRing[n].flags = 0;
                }
                OSSynchronizeIO();
                pipe.dstWrIdx = postCount;
                fPci->write32(base + CE_DST_WR_INDEX_OFFSET, pipe.dstWrIdx);
            }
        }
        fPci->write32(base + CE_SRC_WR_INDEX_OFFSET, 0);

        /* Record the programmed state so the next boot can be compared
           against the ath10k reference values. */
        fPci->setDiagProp("QCACECtrl1", fPci->read32(base + CE_CTRL1_OFFSET));
        fPci->setDiagProp("QCACECtrl1Sz", pipe.srcEntries | (pipe.destEntries << 16));
        fPci->setDiagProp("QCACESrcWm", fPci->read32(base + CE_SRC_WM_OFFSET));
        fPci->setDiagProp("QCACEDstWm", fPci->read32(base + CE_DST_WM_OFFSET));
        {
            char key[32];
            snprintf(key, sizeof(key), "QCACeCtrl1Pre%u", i);
            fPci->setDiagProp(key, ctrl1Pre);
            snprintf(key, sizeof(key), "QCACeCtrl1Post%u", i);
            fPci->setDiagProp(key, fPci->read32(base + CE_CTRL1_OFFSET));
        }
    }
}

void QCACopyEngine::stopPipes()
{
    for (int i = 0; i < CE_COUNT; i++) {
        CEPipe &pipe = fPipes[i];
        if (pipe.pendingDma) {
            pipe.pendingDma->complete();
            pipe.pendingDma->release();
            pipe.pendingDma = nullptr;
        }
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

    /* The previous transfer has completed by now (the BMI response for
       it has already been received), so its buffer can go away. */
    releasePending(pipe);

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
    const u32 mask = pipe.srcEntries - 1;
    const u32 next = (pipe.srcWrIdx + 1) & mask;
    int timeout = 1000;
    while (next == (fPci->read32(base + CE_SRC_RD_INDEX_OFFSET) & mask) && timeout-- > 0)
        qca_mdelay(1);
    if (timeout <= 0) {
        fPci->setDiagProp("QCACeSendFull", pipe.srcWrIdx);
        dma->complete(); dma->release(); return -1;
    }

    struct ce_desc &desc = pipe.srcRing[pipe.srcWrIdx];
    desc.addr = cpu_to_le32((u32)paddr);
    desc.nbytes = cpu_to_le16((u16)len);
    /* ath10k fills the descriptor metadata field with the transfer id;
       BMI uses transfer id -1, i.e. all metadata bits set. */
    desc.flags = cpu_to_le16((u16)CE_DESC_META_DATA_BMI);
    OSSynchronizeIO();
    pipe.srcWrIdx = next;
    fPci->write32(base + CE_SRC_WR_INDEX_OFFSET, next);

    /* Wait for the Copy Engine to fetch the descriptor. */
    timeout = 1000;
    u32 rdIdx = fPci->read32(base + CE_SRC_RD_INDEX_OFFSET);
    while ((rdIdx & mask) != next && timeout-- > 0) {
        qca_mdelay(1);
        rdIdx = fPci->read32(base + CE_SRC_RD_INDEX_OFFSET);
    }
    if (timeout <= 0) {
        fPci->setDiagProp("QCACeSendStall", rdIdx);
        fPci->setDiagProp("QCACeSendWr", next);
        dma->complete(); dma->release(); return -1;
    }

    /* Keep the buffer alive until the next transfer: the Copy Engine
       may still be reading it when this call returns. */
    pipe.pendingDma = dma;
    fPci->setDiagProp("QCACeSendOk", next);
    return 0;
}

void QCACopyEngine::releasePending(CEPipe &pipe)
{
    if (pipe.pendingDma) {
        pipe.pendingDma->complete();
        pipe.pendingDma->release();
        pipe.pendingDma = nullptr;
    }
}

/* Post a descriptor on the diagnostic window's source ring without
   copying any data. The descriptor address is used verbatim, which is
   what lets the window address the target's memory space
   (ath10k_ce_send with a NULL buffer). The descriptor's metadata field
   carries the transfer id, which is 0 for the diagnostic window. */
int QCACopyEngine::diagSendRaw(u32 addr, u32 len, u32 flags)
{
    CEPipe &pipe = fPipes[CE_DIAG_PIPE];
    if (!pipe.srcEntries || !pipe.srcRing || !len || len > pipe.maxBufSize) return -1;

    const u32 base = pipe.baseOffset;
    const u32 mask = pipe.srcEntries - 1;
    const u32 next = (pipe.srcWrIdx + 1) & mask;
    int timeout = 100;
    while (next == (fPci->read32(base + CE_SRC_RD_INDEX_OFFSET) & mask) && timeout-- > 0)
        qca_mdelay(1);
    if (timeout <= 0) return -1;

    struct ce_desc &desc = pipe.srcRing[pipe.srcWrIdx];
    desc.addr = cpu_to_le32(addr);
    desc.nbytes = cpu_to_le16((u16)len);
    desc.flags = cpu_to_le16((u16)flags);
    OSSynchronizeIO();
    pipe.srcWrIdx = next;
    fPci->write32(base + CE_SRC_WR_INDEX_OFFSET, next);

    /* Wait for the Copy Engine to fetch the descriptor. ath10k detects
       the completion of a transfer the same way: the hardware advances
       the source ring's read index. */
    timeout = 100;
    u32 rdIdx = fPci->read32(base + CE_SRC_RD_INDEX_OFFSET);
    while ((rdIdx & mask) != next && timeout-- > 0) {
        qca_mdelay(1);
        rdIdx = fPci->read32(base + CE_SRC_RD_INDEX_OFFSET);
    }
    if (timeout <= 0) {
        fPci->setDiagProp("QCADiagSendStall", rdIdx);
        return -1;
    }
    return 0;
}

/* Post a destination descriptor on the diagnostic window's destination
   ring (ath10k_ce_rx_post_buf). */
int QCACopyEngine::diagPostDest(u32 addr)
{
    CEPipe &pipe = fPipes[CE_DIAG_PIPE];
    if (!pipe.destEntries || !pipe.dstRing) return -1;

    const u32 base = pipe.baseOffset;
    const u32 mask = pipe.destEntries - 1;
    const u32 next = (pipe.dstWrIdx + 1) & mask;
    int timeout = 100;
    while (next == (fPci->read32(base + CE_DST_RD_INDEX_OFFSET) & mask) && timeout-- > 0)
        qca_mdelay(1);
    if (timeout <= 0) return -1;

    struct ce_desc &desc = pipe.dstRing[pipe.dstWrIdx];
    desc.addr = cpu_to_le32(addr);
    desc.nbytes = 0;
    desc.flags = 0;
    OSSynchronizeIO();
    pipe.dstWrIdx = next;
    fPci->write32(base + CE_DST_WR_INDEX_OFFSET, pipe.dstWrIdx);
    return 0;
}

/* Wait for the diagnostic window to deliver the requested bytes. The
   completion is signalled by the destination descriptor's length field
   (ath10k_ce_completed_recv_next). */
int QCACopyEngine::diagWaitRecv(u32 len)
{
    CEPipe &pipe = fPipes[CE_DIAG_PIPE];
    if (!pipe.dstRing) return -1;

    int timeout = 100;
    struct ce_desc &desc = pipe.dstRing[pipe.dstRdIdx];
    while (le16_to_cpu(desc.nbytes) == 0 && timeout-- > 0)
        qca_mdelay(1);
    if (timeout <= 0) {
        fPci->setDiagProp("QCADiagRecvStall", pipe.dstRdIdx);
        return -1;
    }

    u16 received = le16_to_cpu(desc.nbytes);
    desc.nbytes = 0;
    desc.flags = 0;
    pipe.dstRdIdx = (pipe.dstRdIdx + 1) % pipe.destEntries;
    return received == len ? 0 : -1;
}

/* Read target memory through the diagnostic window
   (ath10k_pci_diag_read_mem).

   The window is a copy engine with a special protocol: the source
   descriptor names the *target* address to read from and the destination
   descriptor names the *host* buffer that receives the data. */
int QCACopyEngine::diagReadMem(u32 targetAddr, void *buf, u32 len)
{
    if (!buf || !len || len > DIAG_TRANSFER_LIMIT) return -1;

    /* Bounce buffer: 4-byte aligned and in DMA-able space. */
    IOBufferMemoryDescriptor *dma = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryHostPhysicallyContiguous,
        len, 0x00000000FFFFFFFFULL);
    if (!dma) return -1;
    if (dma->prepare() != kIOReturnSuccess) { dma->release(); return -1; }

    UInt8 *data = (UInt8 *)dma->getBytesNoCopy();
    dma_addr_t phys = dma->getPhysicalAddress();
    if (!data || phys > 0xffffffffULL) {
        dma->complete(); dma->release(); return -1;
    }
    memset(data, 0, len);

    u32 ceAddr = fPci->targCpuToCeAddr(targetAddr);

    int ret = diagPostDest((u32)phys);
    if (ret == 0) ret = diagSendRaw(ceAddr, len, 0);
    if (ret == 0) ret = diagWaitRecv(len);
    if (ret == 0) memcpy(buf, data, len);

    dma->complete();
    dma->release();
    return ret;
}

/* Write target memory through the diagnostic window
   (ath10k_pci_diag_write_mem).

   The roles are reversed compared to the read: the source descriptor
   names the *host* buffer that holds the data and the destination
   descriptor names the *target* address to write to. */
int QCACopyEngine::diagWriteMem(u32 targetAddr, const void *buf, u32 len)
{
    if (!buf || !len || len > DIAG_TRANSFER_LIMIT) return -1;

    IOBufferMemoryDescriptor *dma = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryHostPhysicallyContiguous,
        len, 0x00000000FFFFFFFFULL);
    if (!dma) return -1;
    if (dma->prepare() != kIOReturnSuccess) { dma->release(); return -1; }

    UInt8 *data = (UInt8 *)dma->getBytesNoCopy();
    dma_addr_t phys = dma->getPhysicalAddress();
    if (!data || phys > 0xffffffffULL) {
        dma->complete(); dma->release(); return -1;
    }
    memcpy(data, buf, len);
    OSSynchronizeIO();

    u32 ceAddr = fPci->targCpuToCeAddr(targetAddr);

    int ret = diagPostDest(ceAddr);
    if (ret == 0) ret = diagSendRaw((u32)phys, len, 0);
    if (ret == 0) ret = diagWaitRecv(len);

    dma->complete();
    dma->release();
    return ret;
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
