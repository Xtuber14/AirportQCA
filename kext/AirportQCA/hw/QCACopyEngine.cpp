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

/* QCA9377 Host CE Pipe Configuration Table (with CE5 Override) */
static const CEPipeConfig qca9377_ce_configs[CE_COUNT] = {
    /* CE0: Host -> Target HTC Control & BMI */
    { 0, 16, 0, 256 },
    /* CE1: Target -> Host HTT Data & HTC Control */
    { 1, 0, 512, 2048 },
    /* CE2: Target -> Host WMI Events */
    { 2, 0, 128, 2048 },
    /* CE3: Host -> Target HTT Data Tx */
    { 3, 32, 0, 2048 },
    /* CE4: Host -> Target WMI Commands */
    { 4, 16, 0, 2048 },
    /* CE5: Override for QCA9377 / QCA6174 (Disabled) */
    { 5, 0, 0, 0 },
    /* CE6: Target -> Host Diagnostics */
    { 6, 0, 32, 2048 },
    /* CE7: Diagnostic Window */
    { 7, 0, 0, 0 }
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
            memset(pipe.dstRing, 0, ringSize);
        }

        /* Program hardware registers if pipe is active */
        if (pipe.srcEntries > 0 || pipe.destEntries > 0) {
            if (pipe.srcEntries > 0) {
                fPci->write32(pipe.baseOffset + CE_SRC_RING_BASE_ADDR_OFFSET, (u32)pipe.srcRingPhys);
                fPci->write32(pipe.baseOffset + CE_SRC_RING_SZ_OFFSET, pipe.srcEntries);
                fPci->write32(pipe.baseOffset + CE_SRC_RING_DMAX_OFFSET, pipe.maxBufSize);
            }
            if (pipe.destEntries > 0) {
                fPci->write32(pipe.baseOffset + CE_DST_RING_BASE_ADDR_OFFSET, (u32)pipe.dstRingPhys);
                fPci->write32(pipe.baseOffset + CE_DST_RING_SZ_OFFSET, pipe.destEntries);
            }
            /* Enable interrupts */
            fPci->write32(pipe.baseOffset + CE_CTRL1_OFFSET,
                          CE_CTRL1_COMPL_INTR_EN | CE_CTRL1_WATERMARK_INTR_EN | CE_CTRL1_ERR_INTR_EN);
        }
    }

    qca_info("Copy Engine pipes successfully initialized");
    return true;
}

void QCACopyEngine::startPipes()
{
    qca_info("Copy Engine pipes started");
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
    }
}

int QCACopyEngine::send(u32 pipeId, void *buf, u32 len)
{
    if (pipeId >= CE_COUNT) return -1;
    CEPipe &pipe = fPipes[pipeId];
    if (pipe.srcEntries == 0 || !pipe.srcRing) return -1;

    u32 wrIdx = pipe.srcWrIdx;
    struct ce_desc *desc = &pipe.srcRing[wrIdx];

    /* In a full implementation, the buffer's physical address is passed */
    desc->nbytes = cpu_to_le16(len);
    desc->flags = cpu_to_le16(0);

    pipe.srcWrIdx = (wrIdx + 1) % pipe.srcEntries;
    fPci->write32(pipe.baseOffset + CE_SRC_WR_INDEX_OFFSET, pipe.srcWrIdx);
    return 0;
}

int QCACopyEngine::recv(u32 pipeId, void *buf, u32 *len)
{
    if (pipeId >= CE_COUNT) return -1;
    CEPipe &pipe = fPipes[pipeId];
    if (pipe.destEntries == 0 || !pipe.dstRing) return -1;

    u32 rdIdx = fPci->read32(pipe.baseOffset + CE_DST_RD_INDEX_OFFSET);
    if (rdIdx == pipe.dstRdIdx) {
        return -1; /* No pending data */
    }

    struct ce_desc *desc = &pipe.dstRing[pipe.dstRdIdx];
    *len = min(*len, (u32)le16_to_cpu(desc->nbytes));

    pipe.dstRdIdx = (pipe.dstRdIdx + 1) % pipe.destEntries;
    fPci->write32(pipe.baseOffset + CE_DST_RD_INDEX_OFFSET, pipe.dstRdIdx);
    return 0;
}
