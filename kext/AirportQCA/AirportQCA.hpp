//
//  AirportQCA.hpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//
//  Main IO80211Controller driver class for Qualcomm Atheros QCA9377.
//

#ifndef _AIRPORT_QCA_HPP_
#define _AIRPORT_QCA_HPP_

#define __IO80211_TARGET 12

#include "Apple80211/Apple80211.h"
#include "include/qca_compat.h"
#include "include/qca_hw.h"
#include "include/qca_regs.h"

#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <libkern/c++/OSString.h>

class AirportQCAInterface;
class QCAPCIDevice;

enum {
    kQCAPowerStateOff = 0,
    kQCAPowerStateOn  = 1,
    kQCAPowerStateCount
};

class AirportQCA : public IO80211Controller {
    OSDeclareDefaultStructors(AirportQCA)

public:
    virtual bool init(OSDictionary *properties = nullptr) APPLE_KEXT_OVERRIDE;
    virtual void free() APPLE_KEXT_OVERRIDE;

    virtual IOService* probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

    virtual IOReturn enable(IONetworkInterface *netif) APPLE_KEXT_OVERRIDE;
    virtual IOReturn disable(IONetworkInterface *netif) APPLE_KEXT_OVERRIDE;

    virtual IOReturn getHardwareAddress(IOEthernetAddress *addrP) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setHardwareAddress(const IOEthernetAddress *addrP) APPLE_KEXT_OVERRIDE;

    virtual UInt32 outputPacket(mbuf_t m, void *param) APPLE_KEXT_OVERRIDE;
    virtual int outputRaw80211Packet(IO80211Interface *interface, mbuf_t m) APPLE_KEXT_OVERRIDE;
    virtual bool useAppleRSNSupplicant(IO80211Interface *interface) APPLE_KEXT_OVERRIDE;

    virtual SInt32 apple80211Request(
        unsigned int request_type,
        int request_number,
        IO80211Interface *interface,
        void *data) APPLE_KEXT_OVERRIDE;

    virtual IONetworkInterface* createInterface() APPLE_KEXT_OVERRIDE;
    virtual bool configureInterface(IONetworkInterface *netif) APPLE_KEXT_OVERRIDE;

    virtual const OSString* newVendorString() const APPLE_KEXT_OVERRIDE;
    virtual const OSString* newModelString() const APPLE_KEXT_OVERRIDE;

    virtual IOWorkLoop* getWorkLoop() const APPLE_KEXT_OVERRIDE;
    virtual bool createWorkLoop() APPLE_KEXT_OVERRIDE;

    virtual bool setLinkStatus(
        UInt32 status,
        const IONetworkMedium *activeMedium = 0,
        UInt64 speed = 0,
        OSData *data = 0) APPLE_KEXT_OVERRIDE;

    virtual IOReturn setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice) APPLE_KEXT_OVERRIDE;

    /* Hardware & interrupt handling */
    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *src);
    static void interruptAction(OSObject *owner, IOInterruptEventSource *src, int count);
    void handleHardwareInterrupt();

    /* Watchdog & Periodic Timers */
    void watchdogAction(IOTimerEventSource *timer);

private:
    bool createMediumTables(const IONetworkMedium **primary);
    bool initPCIPowerManagement(IOPCIDevice *pci);

    IOPCIDevice               *fPciNub;
    AirportQCAInterface       *fNetIf;
    IOWorkLoop                *fWorkLoop;
    IOCommandGate             *fCommandGate;
    IOFilterInterruptEventSource *fInterruptSource;
    IOTimerEventSource        *fWatchdogTimer;

    IOEthernetAddress          fMacAddress;
    uint32_t                   fPowerState;
    bool                       fLinkActive;
    uint32_t                   fChannel;
    int32_t                    fRssi;
    int32_t                    fNoise;

    /* QCA Hardware state */
    void                      *fHalDevice;
};

#endif /* _AIRPORT_QCA_HPP_ */
