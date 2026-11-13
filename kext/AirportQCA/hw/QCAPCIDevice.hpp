//
//  QCAPCIDevice.hpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//
//  Hardware abstraction layer for QCA9377 PCI Express endpoint.
//

#ifndef _QCA_PCI_DEVICE_HPP_
#define _QCA_PCI_DEVICE_HPP_

#include "../include/qca_compat.h"
#include "../include/qca_hw.h"
#include "../include/qca_regs.h"
#include "../include/Apple80211/Apple80211.h"

#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>

class AirportQCA;
class QCACopyEngine;
class QCABMI;
class QCAHTC;
class QCAWMI;
class QCAHTT;

class QCAPCIDevice {
public:
    QCAPCIDevice();
    ~QCAPCIDevice();

    bool init(IOPCIDevice *pciNub, AirportQCA *driver);
    void start();
    void stop();

    /* MMIO Register Access */
    inline u32 read32(u32 offset) {
        if (!fMmioBase) return 0xFFFFFFFF;
        return OSReadLittleInt32(fMmioBase, offset);
    }

    inline void write32(u32 offset, u32 val) {
        if (!fMmioBase) return;
        OSWriteLittleInt32(fMmioBase, offset, val);
    }

    /* Hardware Resets */
    int warmReset();
    int chipReset();
    int waitForTargetInit();

    /* MAC Address */
    void getMacAddress(uint8_t *mac);

    /* Wi-Fi Control & Data Operations */
    void setChannel(uint32_t channel);
    int startScan();
    int associate(apple80211_assoc_data *assoc, u32 channel);
    void disassociate();
    void installKey(apple80211_key *key);
    UInt32 transmitPacket(mbuf_t m);
    int transmitRaw80211(mbuf_t m);
    void watchdogCheck();
    void freePacket(mbuf_t packet);
    void notifyScanComplete();

    /* Subsystem access */
    QCACopyEngine* getCopyEngine() const { return fCopyEngine; }
    QCAHTC*        getHTC() const         { return fHtc; }
    IOPCIDevice*   getPciNub() const     { return fPciNub; }

private:
    bool bootFirmware();

    IOPCIDevice     *fPciNub;
    AirportQCA      *fDriver;
    IOMemoryMap     *fMmioMap;
    volatile void   *fMmioBase;

    QCACopyEngine   *fCopyEngine;
    QCABMI          *fBmi;
    QCAHTC          *fHtc;
    QCAWMI          *fWmi;
    QCAHTT          *fHtt;

    uint8_t          fMacAddr[6];
    bool             fFwRunning;
};

#endif /* _QCA_PCI_DEVICE_HPP_ */
