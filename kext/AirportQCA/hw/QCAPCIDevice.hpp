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

    /* MMIO Register Access.
       The QCA9377 SoC sleeps on its own (ath10k pci_ps = true): while
       asleep its register file is unpowered and reads return zero. Every
       access must therefore wake the SoC first and wait for the RTC
       state to report ON. */
    inline u32 read32Raw(u32 offset) {
        if (!fMmioBase) return 0xFFFFFFFF;
        return OSReadLittleInt32(fMmioBase, offset);
    }

    inline void write32Raw(u32 offset, u32 val) {
        if (!fMmioBase) return;
        OSWriteLittleInt32(fMmioBase, offset, val);
    }

    inline u32 read32(u32 offset) {
        wakeTarget();
        return read32Raw(offset);
    }

    inline void write32(u32 offset, u32 val) {
        wakeTarget();
        write32Raw(offset, val);
    }

    bool wakeTarget();
    void markTargetAsleep();
    void dumpRegisterFingerprint(const char *tag);
    void snapshotRegisters(const char *tag);
    /* Store the recorded firmware indicator sequence in the registry. */
    void recordIndicatorSequence(const u32 *values, const u32 *times, unsigned count);

    /* PCIe link/config diagnostics. The target ROM is sensitive to the
       PCIe state, so the link policy and the configuration space are
       recorded at every stage of the bring-up and the link is kept out
       of the low-power substates while the ROM boots. */
    void applyLinkPowerPolicy();
    void dumpPcieState(const char *tag);
    void dumpPcieConfig(const char *tag);
    void captureCrashState(int elapsedMs);

    /* PCIe Advanced Error Reporting capability: the error status and the
       header log identify the transaction that upset the target ROM. */
    UInt32 findAerCapability();
    void dumpAerState(const char *tag);

    /* Registry breadcrumbs. The driver's IOLog output never reaches the
       unified log, so every bring-up value is mirrored into ioreg. */
    void setDiagProp(const char *key, u32 value);
    void setDiagStr(const char *key, const char *value);

    /* Hardware Resets */
    int warmReset();
    int chipReset();
    int waitForTargetInit(int timeoutMs = 3000);
    bool targetInitialized();

    /* Target bring-up helpers */
    int enablePllClock();
    int configureTarget();

    /* Convert a target CPU address into the address space the Copy
       Engine understands (ath10k_pci_qca6174_targ_cpu_to_ce_addr). */
    u32 targCpuToCeAddr(u32 addr);

    /* Map an HTC service id to the Copy Engine pipes used for the
       uplink (host -> target) and the downlink (target -> host)
       direction. Both the host and the target use the same table, so
       they always agree (ath10k_pci_hif_map_service_to_pipe). */
    bool mapServiceToPipe(u16 serviceId, u8 *ulPipe, u8 *dlPipe) const;

    /* Download the target-side Copy Engine configuration and the
       service-to-CE map, then release the target from its early
       configuration wait (ath10k_pci_init_config). */
    int initConfig();

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
    void reportScanBeacon(const u8 *frame, u32 frameLength, u32 channel, u32 snr);

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
    bool             fTargetAwake;
    UInt8            fPcieCap;
    UInt32           fL1PMSubstatesCap;
};

#endif /* _QCA_PCI_DEVICE_HPP_ */
