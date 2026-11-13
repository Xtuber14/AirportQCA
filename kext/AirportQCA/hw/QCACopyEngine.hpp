//
//  QCACopyEngine.hpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//
//  Copy Engine (CE) DMA ring management for QCA9377.
//

#ifndef _QCA_COPY_ENGINE_HPP_
#define _QCA_COPY_ENGINE_HPP_

#include "../include/qca_compat.h"
#include "../include/qca_hw.h"
#include "../include/qca_regs.h"

#include <IOKit/IOBufferMemoryDescriptor.h>

class QCAPCIDevice;

struct CEPipeConfig {
    u32 pipeNum;
    u32 srcEntries;
    u32 destEntries;
    u32 maxBufSize;
};

class QCACopyEngine {
public:
    QCACopyEngine(QCAPCIDevice *pci);
    ~QCACopyEngine();

    bool initPipes();
    void startPipes();
    void stopPipes();

    /* Buffer Send / Receive over CE */
    int send(u32 pipeId, void *buf, u32 len);
    int recv(u32 pipeId, void *buf, u32 *len);

private:
    struct CEPipe {
        u32 pipeId;
        u32 baseOffset;
        u32 srcEntries;
        u32 destEntries;
        u32 maxBufSize;

        IOBufferMemoryDescriptor *srcRingMem;
        IOBufferMemoryDescriptor *dstRingMem;
        IOBufferMemoryDescriptor *dstBufferMem;

        struct ce_desc *srcRing;
        struct ce_desc *dstRing;
        UInt8 *dstBuffer;

        dma_addr_t srcRingPhys;
        dma_addr_t dstRingPhys;
        dma_addr_t dstBufferPhys;

        u32 srcWrIdx;
        u32 srcRdIdx;
        u32 dstWrIdx;
        u32 dstRdIdx;
    };

    QCAPCIDevice *fPci;
    CEPipe fPipes[CE_COUNT];
};

#endif /* _QCA_COPY_ENGINE_HPP_ */
